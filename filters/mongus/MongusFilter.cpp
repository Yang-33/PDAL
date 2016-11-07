/******************************************************************************
* Copyright (c) 2016, Bradley J Chambers (brad.chambers@gmail.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include "MongusFilter.hpp"

#include <pdal/pdal_macros.hpp>
#include <pdal/Eigen.hpp>
#include <pdal/PipelineManager.hpp>
#include <buffer/BufferReader.hpp>
#include <pdal/util/ProgramArgs.hpp>
#include <pdal/util/Utils.hpp>

#include <Eigen/Dense>

#include "gdal_priv.h" // For File I/O
#include "gdal_version.h" // For version info
#include "ogr_spatialref.h"  //For Geographic Information/Transformations

namespace pdal
{

static PluginInfo const s_info =
    PluginInfo("filters.mongus", "Mongus and Zalik (2012)",
               "http://pdal.io/stages/filters.mongus.html");

CREATE_STATIC_PLUGIN(1, 0, MongusFilter, Filter, s_info)

std::string MongusFilter::getName() const
{
    return s_info.name;
}

void MongusFilter::addArgs(ProgramArgs& args)
{
    args.add("cell", "Cell size", m_cellSize, 1.0);
    args.add("k", "Stdev multiplier for threshold", m_k, 3.0);
    args.add("l", "Max level", m_l, 8);
    args.add("classify", "Apply classification labels?", m_classify, true);
    args.add("extract", "Extract ground returns?", m_extract);
}

void MongusFilter::addDimensions(PointLayoutPtr layout)
{
    layout->registerDim(Dimension::Id::Classification);
}

int MongusFilter::getColIndex(double x, double cell_size)
{
    return static_cast<int>(floor((x - m_bounds.minx) / cell_size));
}

int MongusFilter::getRowIndex(double y, double cell_size)
{
    return static_cast<int>(floor((m_maxRow - y) / cell_size));
}

Eigen::MatrixXd MongusFilter::computeSpline(Eigen::MatrixXd x_prev,
        Eigen::MatrixXd y_prev,
        Eigen::MatrixXd z_prev,
        Eigen::MatrixXd x_samp,
        Eigen::MatrixXd y_samp)
{
    using namespace Eigen;

// maybe make sure that all prevs are the same size, same with samps

    int num_rows = x_samp.rows();
    int num_cols = x_samp.cols();

    MatrixXd S = MatrixXd::Zero(num_rows, num_cols);

    for (auto outer_col = 0; outer_col < num_cols; ++outer_col)
    {
        for (auto outer_row = 0; outer_row < num_rows; ++outer_row)
        {
            // Further optimizations are achieved by estimating only the
            // interpolated surface within a local neighbourhood (e.g. a 7 x 7
            // neighbourhood is used in our case) of the cell being filtered.
            int radius = 3;

            int inner_col = std::floor(outer_col/2);
            int inner_row = std::floor(outer_row/2);

            int cs = Utils::clamp(inner_col-radius, 0, static_cast<int>(z_prev.cols()-1));
            int ce = Utils::clamp(inner_col+radius, 0, static_cast<int>(z_prev.cols()-1));
            int col_size = ce - cs + 1;
            int rs = Utils::clamp(inner_row-radius, 0, static_cast<int>(z_prev.rows()-1));
            int re = Utils::clamp(inner_row+radius, 0, static_cast<int>(z_prev.rows()-1));
            int row_size = re - rs + 1;

            MatrixXd Xn = x_prev.block(rs, cs, row_size, col_size);
            MatrixXd Yn = y_prev.block(rs, cs, row_size, col_size);
            MatrixXd Hn = z_prev.block(rs, cs, row_size, col_size);

            int nsize = Hn.size();
            VectorXd T = VectorXd::Zero(nsize);
            MatrixXd P = MatrixXd::Zero(nsize, 3);
            MatrixXd K = MatrixXd::Zero(nsize, nsize);

            for (auto id = 0; id < Hn.size(); ++id)
            {
                double xj = Xn(id);
                double yj = Yn(id);
                double zj = Hn(id);
                if (std::isnan(xj) || std::isnan(yj) || std::isnan(zj))
                    continue;
                T(id) = zj;
                P.row(id) << 1, xj, yj;
                for (auto id2 = 0; id2 < Hn.size(); ++id2)
                {
                    if (id == id2)
                        continue;
                    double xk = Xn(id2);
                    double yk = Yn(id2);
                    double zk = Hn(id2);
                    if (std::isnan(xk) || std::isnan(yk) || std::isnan(zk))
                        continue;
                    double rsqr = (xj - xk) * (xj - xk) + (yj - yk) * (yj - yk);
                    if (rsqr == 0.0)
                        continue;
                    K(id, id2) = rsqr * std::log10(std::sqrt(rsqr));
                }
            }

            MatrixXd A = MatrixXd::Zero(nsize+3, nsize+3);
            A.block(0,0,nsize,nsize) = K;
            A.block(0,nsize,nsize,3) = P;
            A.block(nsize,0,3,nsize) = P.transpose();

            VectorXd b = VectorXd::Zero(nsize+3);
            b.head(nsize) = T;

            VectorXd x = A.fullPivHouseholderQr().solve(b);

            Vector3d a = x.tail(3);
            VectorXd w = x.head(nsize);

            double sum = 0.0;
            double xi2 = x_samp(outer_row, outer_col);
            double yi2 = y_samp(outer_row, outer_col);
            for (auto j = 0; j < nsize; ++j)
            {
                double xj = Xn(j);
                double yj = Yn(j);
                double zj = Hn(j);
                if (std::isnan(xj) || std::isnan(yj) || std::isnan(zj))
                    continue;
                double rsqr = (xj - xi2) * (xj - xi2) + (yj - yi2) * (yj - yi2);
                if (rsqr == 0.0)
                    continue;
                sum += w(j) * rsqr * std::log10(std::sqrt(rsqr));
            }

            S(outer_row, outer_col) = a(0) + a(1)*xi2 + a(2)*yi2 + sum;
        }
    }

    return S;
}

void MongusFilter::writeMatrix(Eigen::MatrixXd data, std::string filename, double cell_size, PointViewPtr view)
{
    int cols = data.cols();
    int rows = data.rows();

    GDALAllRegister();

    GDALDataset *mpDstDS(0);

    char **papszMetadata;

    // parse the format driver, hardcoded for the time being
    std::string tFormat("GTIFF");
    const char *pszFormat = tFormat.c_str();
    GDALDriver* tpDriver = GetGDALDriverManager()->GetDriverByName(pszFormat);

    // try to create a file of the requested format
    if (tpDriver != NULL)
    {
        papszMetadata = tpDriver->GetMetadata();
        if (CSLFetchBoolean(papszMetadata, GDAL_DCAP_CREATE, FALSE))
        {
            char **papszOptions = NULL;

            mpDstDS = tpDriver->Create(filename.c_str(), cols, rows, 1,
                                       GDT_Float32, papszOptions);

            // set the geo transformation
            double adfGeoTransform[6];
            adfGeoTransform[0] = m_bounds.minx; // - 0.5*m_GRID_DIST_X;
            adfGeoTransform[1] = cell_size;
            adfGeoTransform[2] = 0.0;
            adfGeoTransform[3] = m_bounds.maxy; // + 0.5*m_GRID_DIST_Y;
            adfGeoTransform[4] = 0.0;
            adfGeoTransform[5] = -1 * cell_size;
            mpDstDS->SetGeoTransform(adfGeoTransform);

            // set the projection
            mpDstDS->SetProjection(view->spatialReference().getWKT().c_str());
        }
    }

    // if we have a valid file
    if (mpDstDS)
    {
        // loop over the raster and determine max slope at each location
        int cs = 0, ce = cols;
        int rs = 0, re = rows;
        float *poRasterData = new float[cols*rows];
        for (auto i=0; i<cols*rows; i++)
        {
            poRasterData[i] = std::numeric_limits<float>::min();
        }

        #pragma omp parallel for
        for (auto c = cs; c < ce; ++c)
        {
            for (auto r = rs; r < re; ++r)
            {
                if (data(r, c) == 0.0 || std::isnan(data(r, c)) || data(r, c) == std::numeric_limits<double>::max())
                    continue;
                poRasterData[(r * cols) + c] =
                    data(r, c);
            }
        }

        // write the data
        if (poRasterData)
        {
            GDALRasterBand *tBand = mpDstDS->GetRasterBand(1);

            tBand->SetNoDataValue(std::numeric_limits<float>::min());

            if (cols > 0 && rows > 0)
#if GDAL_VERSION_MAJOR <= 1
                tBand->RasterIO(GF_Write, 0, 0, cols, rows,
                                poRasterData, cols, rows,
                                GDT_Float32, 0, 0);
#else

                int ret = tBand->RasterIO(GF_Write, 0, 0, cols, rows,
                                          poRasterData, cols, rows,
                                          GDT_Float32, 0, 0, 0);
#endif
        }

        GDALClose((GDALDatasetH) mpDstDS);

        delete [] poRasterData;
    }
}

void MongusFilter::writeControl(Eigen::MatrixXd cx, Eigen::MatrixXd cy, Eigen::MatrixXd cz, std::string filename)
{
    using namespace Dimension;

    PipelineManager m;

    PointTable table;
    PointViewPtr view(new PointView(table));

    table.layout()->registerDim(Id::X);
    table.layout()->registerDim(Id::Y);
    table.layout()->registerDim(Id::Z);

    PointId i = 0;
    for (auto j = 0; j < cz.size(); ++j)
    {
        if (std::isnan(cx(j)) || std::isnan(cy(j)) || std::isnan(cz(j)))
            continue;
        view->setField(Id::X, i, cx(j));
        view->setField(Id::Y, i, cy(j));
        view->setField(Id::Z, i, cz(j));
        i++;
    }

    BufferReader r;
    r.addView(view);

    Stage& w = m.makeWriter(filename, "writers.las", r);
    w.prepare(table);
    w.execute(table);
}

std::vector<PointId> MongusFilter::processGround(PointViewPtr view)
{
    using namespace Eigen;

    point_count_t np(view->size());

    std::vector<PointId> groundIdx;

    // initialization

    view->calculateBounds(m_bounds);

    m_numCols =
        static_cast<int>(ceil((m_bounds.maxx - m_bounds.minx)/m_cellSize)) + 1;
    m_numRows =
        static_cast<int>(ceil((m_bounds.maxy - m_bounds.miny)/m_cellSize)) + 1;
    m_maxRow = m_bounds.miny + m_numRows * m_cellSize;

    // create control points matrix at default cell size
    MatrixXd cx(m_numRows, m_numCols);
    cx.setConstant(std::numeric_limits<double>::quiet_NaN());

    MatrixXd cy(m_numRows, m_numCols);
    cy.setConstant(std::numeric_limits<double>::quiet_NaN());

    MatrixXd cz(m_numRows, m_numCols);
    cz.setConstant(std::numeric_limits<double>::max());

    // find initial set of Z minimums at native resolution
    for (point_count_t i = 0; i < np; ++i)
    {
        using namespace Dimension;
        double x = view->getFieldAs<double>(Id::X, i);
        double y = view->getFieldAs<double>(Id::Y, i);
        double z = view->getFieldAs<double>(Id::Z, i);

        int c = Utils::clamp(getColIndex(x, m_cellSize), 0, m_numCols-1);
        int r = Utils::clamp(getRowIndex(y, m_cellSize), 0, m_numRows-1);

        if (z < cz(r, c))
        {
            cx(r, c) = x;
            cy(r, c) = y;
            cz(r, c) = z;
        }
    }

    writeControl(cx, cy, cz, "grid_mins.laz");

    // In our case, 2D structural elements of circular shape are employed and
    // sufficient accuracy is achieved by using a larger window size for opening
    // (W11) than for closing (W9).
    MatrixXd mo = matrixOpen(cz, 11);
    writeControl(cx, cy, mo, "grid_open.laz");
    MatrixXd mc = matrixClose(mo, 9);
    writeControl(cx, cy, mc, "grid_close.laz");

    // ...in order to minimize the distortions caused by such filtering, the
    // output points ... are compared to C and only ci with significantly lower
    // elevation [are] replaced... In our case, d = 1.0 m was used.
    for (auto i = 0; i < cz.size(); ++i)
    {
        if ((mc(i) - cz(i)) >= 1.0)
            cz(i) = mc(i);
    }
    // cz is still at native resolution, with low points replaced by morphological operators
    writeControl(cx, cy, cz, "grid_mins_adjusted.laz");

    // downsample control at max_level
    int level = m_l;
    double cur_cell_size = m_cellSize * std::pow(2, level);
    // for max level = 8 and cell size 1, this is 256

    MatrixXd x_prev, y_prev, z_prev;

    // Top-level control samples are assumed to be ground points, no filtering
    // is applied.
    downsampleMin(&cx, &cy, &cz, &x_prev, &y_prev, &z_prev, cur_cell_size);
    // x|y|z_prev are control points downsampled to coarsest resolution for the hierarchy, e.g., for 512x512, this would be 2x2
    writeControl(x_prev, y_prev, z_prev, "control_init.laz");

    // Point-filtering is performed iteratively at each level of the
    // control-points hierarchy in a top-down fashion
    for (auto l = level-1; l > 0; --l)
    {
        std::cerr << "Level " << l << std::endl;
        cur_cell_size /= 2;
        // 128, 64, 32, 16, 8, 4, 1

        // compute TPS with update control at level

        // The interpolated surface is estimated based on the filtered set of
        // TPS control-points at the previous level of hierarchy
        // MatrixXd surface = TPS(x_prev, y_prev, z_prev, cur_cell_size);
        // 4x4, 8x8, 16x16, 32x32, 64x64, 128x128, 256x256

        // downsample control at level
        MatrixXd x_samp, y_samp, z_samp;
        downsampleMin(&cx, &cy, &cz, &x_samp, &y_samp, &z_samp, cur_cell_size);
        // 4x4, 8x8, 16x16, 32x32, 64x64, 128x128, 256x256

        MatrixXd surface = computeSpline(x_prev, y_prev, z_prev, x_samp, y_samp);

        // if (l == 3)
        // {
        //     log()->get(LogLevel::Debug) << cx.rows() << "\t" << cx.cols() << std::endl;
        //     log()->get(LogLevel::Debug) << x_prev.rows() << "\t" << x_prev.cols() << std::endl;
        //     log()->get(LogLevel::Debug) << x_samp.rows() << "\t" << x_samp.cols() << std::endl;
        //     log()->get(LogLevel::Debug) << surface.rows() << "\t" << surface.cols() << std::endl;
        //     log()->get(LogLevel::Debug) << "x: " << cx.row(1) << std::endl;
        //     log()->get(LogLevel::Debug) << "z: " << cz.row(1) << std::endl;
        //     log()->get(LogLevel::Debug) << "control_x: " << x_prev.row(0) << std::endl;
        //     log()->get(LogLevel::Debug) << "control_z: " << z_prev.row(0) << std::endl;
        //     log()->get(LogLevel::Debug) << "samples_x: " << x_samp.row(0) << std::endl;
        //     log()->get(LogLevel::Debug) << "samples_z: " << z_samp.row(0) << std::endl;
        //     log()->get(LogLevel::Debug) << "spline: " << surface.row(0) << std::endl;
        // }

        char bufs[256];
        sprintf(bufs, "cur_control_%d.laz", l);
        std::string names(bufs);
        writeControl(x_samp, y_samp, z_samp, names);

        MatrixXd R = z_samp - surface;

        if (l == 7)
            log()->get(LogLevel::Debug) << R << std::endl;

        double sum = 0.0;
        double maxcoeff = std::numeric_limits<double>::lowest();
        double mincoeff = std::numeric_limits<double>::max();
        for (auto i = 0; i < R.size(); ++i)
        {
            if (std::isnan(R(i)))
                continue;
            if (R(i) > maxcoeff)
                maxcoeff = R(i);
            if (R(i) < mincoeff)
                mincoeff = R(i);
            sum += R(i);
        }

        log()->get(LogLevel::Debug) << "R: max=" << maxcoeff
                                    << "; min=" << mincoeff
                                    << "; sum=" << sum
                                    << "; size=" << R.size() << std::endl;

        // median takes an unsorted vector, possibly containing NANs, and
        // returns the median value.
        auto median = [&](std::vector<double> vals)
        {
            // Begin by partitioning the vector by isnan.
            auto ptr = std::partition(vals.begin(), vals.end(), [](double p)
            {
                return std::isnan(p);
            });

            // Copy the actual values, thus eliminating NANs, and sort it.
            std::vector<double> cp(ptr, vals.end());
            std::sort(cp.begin(), cp.end());

            std::cerr << "median troubleshooting\n";
            std::cerr << vals.size() << "\t" << cp.size() << std::endl;
            std::cerr << cp.size() % 2 << std::endl;
            std::cerr << cp[cp.size()/2-1] << "\t" << cp[cp.size()/2] << std::endl;
            if (l == 7)
            {
                for (auto const& v : cp)
                    std::cerr << v << ", ";
                std::cerr << std::endl;
            }

            // Compute the median value. For even sized vectors, this is the
            // average of the midpoints, otherwise it is the midpoint.
            double median = 0.0;
            if (cp.size() % 2 == 0)
                median = (cp[cp.size()/2-1]+cp[cp.size()/2])/2;
            else
                median = cp[cp.size()/2];

            return median;
        };

        // Compute median of residuals.
        std::vector<double> allres(R.data(), R.data()+R.size());
        double m = median(allres);

        // Compute absolute difference of the residuals from the median.
        ArrayXd ad = (R.array()-m).abs();

        // Compute median of absolute differences, with scale factor (1.4862)
        // for a normal distribution.
        std::vector<double> absdiff(ad.data(), ad.data()+ad.size());
        double mad = 1.4862 * median(absdiff);

        // Divide absolute differences by MAD. Values greater than 2 are
        // considered outliers.
        MatrixXd M = (ad / mad).matrix();

        sum = 0.0;
        maxcoeff = std::numeric_limits<double>::lowest();
        mincoeff = std::numeric_limits<double>::max();
        for (auto i = 0; i < M.size(); ++i)
        {
            if (std::isnan(M(i)))
                continue;
            if (M(i) > maxcoeff)
                maxcoeff = M(i);
            if (M(i) < mincoeff)
                mincoeff = M(i);
            sum += M(i);
        }

        log()->get(LogLevel::Debug) << "M: max=" << maxcoeff
                                    << "; min=" << mincoeff
                                    << "; sum=" << sum
                                    << "; size=" << M.size() << std::endl;

        double madthresh = 2.0;
        // Just computing the percent outlier FYI.
        double perc = static_cast<double>((M.array() > madthresh).count());
        perc /= static_cast<double>(R.size());
        perc *= 100.0;
        log()->get(LogLevel::Debug) << "median=" << m
                                    << "; MAD=" << mad
                                    << "; " << (M.array() > madthresh).count()
                                    << " outliers out of " << R.size()
                                    << " control points (" << perc << "%)\n";

        // If the TPS control-point is recognized as a non-ground point, it is
        // replaced by the interpolated point. The time complexity of the
        // approach is reduced by filtering only the control-points in each
        // iteration.
        if (l < 3)
        {
            for (auto i = 0; i < M.size(); ++i)
            {
                if (M(i) > madthresh)
                    z_samp(i) = std::numeric_limits<double>::quiet_NaN();
                // z_samp(i) = surface(i);
            }
        }

        if (log()->getLevel() > LogLevel::Debug5)
        {
            char buffer[256];
            sprintf(buffer, "interp_surface_%d.laz", l);
            std::string name(buffer);
            // writeMatrix(surface, name, cur_cell_size, view);
            writeControl(x_samp, y_samp, surface, name);

            char bufm[256];
            sprintf(bufm, "master_control_%d.laz", l);
            std::string namem(bufm);
            writeControl(cx, cy, cz, namem);

            // this is identical to filtered control when written here - should move it...
            char buf3[256];
            sprintf(buf3, "prev_control_%d.laz", l);
            std::string name3(buf3);
            writeControl(x_prev, y_prev, z_prev, name3);

            char rbuf[256];
            sprintf(rbuf, "residual_%d.laz", l);
            std::string rbufn(rbuf);
            // writeMatrix(R, rbufn, cur_cell_size, view);
            writeControl(x_samp, y_samp, R, rbufn);

            char mbuf[256];
            sprintf(mbuf, "median_%d.laz", l);
            std::string mbufn(mbuf);
            // writeMatrix(M, mbufn, cur_cell_size, view);
            writeControl(x_samp, y_samp, M, mbufn);

            char buf2[256];
            sprintf(buf2, "adjusted_control_%d.laz", l);
            std::string name2(buf2);
            writeControl(x_samp, y_samp, z_samp, name2);
        }

        x_prev = x_samp;
        y_prev = y_samp;
        z_prev = z_samp;
    }

    MatrixXd surface = computeSpline(x_prev, y_prev, z_prev, cx, cy);

    if (log()->getLevel() > LogLevel::Debug5)
    {
        //     writeControl(cx, cy, mc, "closed.laz");
        //
        char buffer[256];
        sprintf(buffer, "final_surface.tif");
        std::string name(buffer);
        writeMatrix(surface, name, m_cellSize, view);
        //
        //     char rbuf[256];
        //     sprintf(rbuf, "final_residual.tif");
        //     std::string rbufn(rbuf);
        //     writeMatrix(R, rbufn, cur_cell_size, view);
        //
        //     char obuf[256];
        //     sprintf(obuf, "final_opened.tif");
        //     std::string obufn(obuf);
        //     writeMatrix(maxZ, obufn, cur_cell_size, view);
        //
        //     char Tbuf[256];
        //     sprintf(Tbuf, "final_tophat.tif");
        //     std::string Tbufn(Tbuf);
        //     writeMatrix(T, Tbufn, cur_cell_size, view);
        //
        //     char tbuf[256];
        //     sprintf(tbuf, "final_thresh.tif");
        //     std::string tbufn(tbuf);
        //     writeMatrix(t, tbufn, cur_cell_size, view);
    }

    // apply final filtering (top hat) using raw points against TPS

    // ...the LiDAR points are filtered only at the bottom level.
    for (point_count_t i = 0; i < np; ++i)
    {
        using namespace Dimension;

        double x = view->getFieldAs<double>(Id::X, i);
        double y = view->getFieldAs<double>(Id::Y, i);
        double z = view->getFieldAs<double>(Id::Z, i);

        int c = Utils::clamp(getColIndex(x, cur_cell_size), 0, m_numCols-1);
        int r = Utils::clamp(getRowIndex(y, cur_cell_size), 0, m_numRows-1);

        double res = z - surface(r, c);
        if (res < 1.0)
            groundIdx.push_back(i);
    }

    return groundIdx;
}

void MongusFilter::downsampleMin(Eigen::MatrixXd *cx, Eigen::MatrixXd *cy,
                                 Eigen::MatrixXd* cz, Eigen::MatrixXd *dcx,
                                 Eigen::MatrixXd *dcy, Eigen::MatrixXd* dcz,
                                 double cell_size)
{
    int nr = ceil(cz->rows() / cell_size);
    int nc = ceil(cz->cols() / cell_size);

    // std::cerr << nr << "\t" << nc << "\t" << cell_size << std::endl;

    dcx->resize(nr, nc);
    dcx->setConstant(std::numeric_limits<double>::quiet_NaN());

    dcy->resize(nr, nc);
    dcy->setConstant(std::numeric_limits<double>::quiet_NaN());

    dcz->resize(nr, nc);
    dcz->setConstant(std::numeric_limits<double>::max());

    for (auto c = 0; c < cz->cols(); ++c)
    {
        for (auto r = 0; r < cz->rows(); ++r)
        {
            if ((*cz)(r, c) == std::numeric_limits<double>::max())
                continue;

            int rr = std::floor(r/cell_size);
            int cc = std::floor(c/cell_size);

            if ((*cz)(r, c) < (*dcz)(rr, cc))
            {
                (*dcx)(rr, cc) = (*cx)(r, c);
                (*dcy)(rr, cc) = (*cy)(r, c);
                (*dcz)(rr, cc) = (*cz)(r, c);
            }
        }
    }
}

PointViewSet MongusFilter::run(PointViewPtr view)
{
    bool logOutput = log()->getLevel() > LogLevel::Debug1;
    if (logOutput)
        log()->floatPrecision(8);
    log()->get(LogLevel::Debug2) << "Process MongusFilter...\n";

    std::vector<PointId> idx = processGround(view);
    std::cerr << idx.size() << std::endl;

    PointViewSet viewSet;

    if (!idx.empty() && (m_classify || m_extract))
    {

        if (m_classify)
        {
            log()->get(LogLevel::Debug2) << "Labeled " << idx.size() << " ground returns!\n";

            // set the classification label of ground returns as 2
            // (corresponding to ASPRS LAS specification)
            for (const auto& i : idx)
            {
                view->setField(Dimension::Id::Classification, i, 2);
            }

            viewSet.insert(view);
        }

        if (m_extract)
        {
            log()->get(LogLevel::Debug2) << "Extracted " << idx.size() << " ground returns!\n";

            // create new PointView containing only ground returns
            PointViewPtr output = view->makeNew();
            for (const auto& i : idx)
            {
                output->appendPoint(*view, i);
            }

            viewSet.erase(view);
            viewSet.insert(output);
        }
    }
    else
    {
        if (idx.empty())
            log()->get(LogLevel::Debug2) << "Filtered cloud has no ground returns!\n";

        if (!(m_classify || m_extract))
            log()->get(LogLevel::Debug2) << "Must choose --classify or --extract\n";

        // return the view buffer unchanged
        viewSet.insert(view);
    }

    return viewSet;
}

} // namespace pdal
