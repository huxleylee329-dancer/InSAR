#include "DemSourceValidityRegression.h"

#include "..\include\DEMSourceManager.h"

#include <cmath>
#include <cstdio>
#include <string>

#define NOMINMAX
#include <windows.h>

#include "gdal_priv.h"
#include "ogr_spatialref.h"
#include "opencv2\core\core.hpp"

namespace
{
struct TemporaryRaster
{
    std::string path;

    ~TemporaryRaster()
    {
        if (!path.empty()) DeleteFileA(path.c_str());
    }
};

bool makeTemporaryTif(TemporaryRaster& raster)
{
    char directory[MAX_PATH] = {};
    char placeholder[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, directory) == 0 ||
        GetTempFileNameA(directory, "dmv", 0, placeholder) == 0)
    {
        return false;
    }
    DeleteFileA(placeholder);
    raster.path = std::string(placeholder) + ".tif";
    return true;
}

bool writeRaster(const std::string& path, const cv::Mat& values, double nodata)
{
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == nullptr) return false;

    GDALDataset* dataset = driver->Create(path.c_str(), values.cols, values.rows, 1, GDT_Float32, nullptr);
    if (dataset == nullptr) return false;

    double transform[6] = { 0.0, 1.0, 0.0, static_cast<double>(values.rows), 0.0, -1.0 };
    OGRSpatialReference spatialReference;
    spatialReference.importFromEPSG(4326);
    spatialReference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* projection = nullptr;
    spatialReference.exportToWkt(&projection);
    dataset->SetGeoTransform(transform);
    dataset->SetProjection(projection);
    CPLFree(projection);

    GDALRasterBand* band = dataset->GetRasterBand(1);
    band->SetNoDataValue(nodata);
    const CPLErr error = band->RasterIO(GF_Write, 0, 0, values.cols, values.rows,
        const_cast<unsigned char*>(values.data), values.cols, values.rows,
        GDT_Float32, 0, 0, nullptr);
    GDALClose(dataset);
    return error == CE_None;
}

bool runCase(double sourceNodata, float validZero, bool expectZeroValid)
{
    const int size = 12;
    cv::Mat values(size, size, CV_32F, cv::Scalar(100.0f));
    values.at<float>(3, 3) = validZero;
    values.at<float>(8, 8) = static_cast<float>(sourceNodata);

    TemporaryRaster raster;
    if (!makeTemporaryTif(raster) || !writeRaster(raster.path, values, sourceNodata)) return false;

    DEMSourceManager manager;
    cv::Mat dem;
    cv::Mat validMask;
    double outputTransform[6] = {};
    char projection[2048] = {};
    const int ret = manager.read_crop_and_resample_dem_ex(
        raster.path.c_str(), 0.0, size, 0.0, size, -1.0,
        dem, validMask, outputTransform, projection, sizeof(projection));
    if (ret != 0 || dem.size() != values.size() || dem.type() != CV_32F ||
        validMask.size() != values.size() || validMask.type() != CV_8U)
    {
        std::fprintf(stderr, "DEM validity regression read failed: ret=%d error=%s\n", ret, manager.error_msg);
        return false;
    }

    const bool zeroValid = validMask.at<unsigned char>(3, 3) == 1 &&
        std::abs(dem.at<float>(3, 3)) < 1e-6f;
    const bool noDataInvalid = validMask.at<unsigned char>(8, 8) == 0 &&
        std::abs(dem.at<float>(8, 8) + 32767.0f) < 1e-3f;
    std::fprintf(stdout,
        "  source_nodata=%g zero(value=%g mask=%u) nodata(value=%g mask=%u)\n",
        sourceNodata,
        dem.at<float>(3, 3), static_cast<unsigned int>(validMask.at<unsigned char>(3, 3)),
        dem.at<float>(8, 8), static_cast<unsigned int>(validMask.at<unsigned char>(8, 8)));
    return zeroValid == expectZeroValid && noDataInvalid;
}
}

int RunDemSourceValidityRegression()
{
    // A numeric zero is a valid sea-level elevation when the source's NoData
    // value is different.
    const bool seaLevelZeroValid = runCase(-9999.0, 0.0f, true);

    // Conversely, if source metadata explicitly defines zero as NoData, the
    // same numeric value must remain invalid.
    const bool zeroNoDataInvalid = runCase(0.0, 0.0f, false);

    std::fprintf(stdout, "DEM source validity: sea_level_zero=%s zero_as_nodata=%s\n",
        seaLevelZeroValid ? "PASS" : "FAIL",
        zeroNoDataInvalid ? "PASS" : "FAIL");
    return seaLevelZeroValid && zeroNoDataInvalid ? 0 : 1;
}
