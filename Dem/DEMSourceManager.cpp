#include "stdafx.h"
#include "..\include\DEMSourceManager.h"
#include "gdal_priv.h"
#include "gdalwarper.h"
#include "ogr_spatialref.h"
#include <cmath>
#include <algorithm>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 局部无损数据填充模板函数（Nearest-neighbor linear interpolation helper）
template<typename T, typename Predicate>
static void fillInvalidGaps(cv::Mat& mat, Predicate is_invalid)
{
    int rows = mat.rows;
    int cols = mat.cols;
    for (int i = 0; i < rows; i++)
    {
        for (int j = 0; j < cols; j++)
        {
            if (!is_invalid(mat.at<T>(i, j))) continue;
            int up = i, down = i, left = j, right = j;
            while (--up >= 0 && is_invalid(mat.at<T>(up, j)));
            while (++down < rows && is_invalid(mat.at<T>(down, j)));
            while (--left >= 0 && is_invalid(mat.at<T>(i, left)));
            while (++right < cols && is_invalid(mat.at<T>(i, right)));

            if (left >= 0 && right < cols && up >= 0 && down < rows)
            {
                double ratio1 = double(j - left) / double(right - left);
                double value1 = double(mat.at<T>(i, left)) + double(mat.at<T>(i, right) - mat.at<T>(i, left)) * ratio1;
                double ratio2 = double(i - up) / double(down - up);
                double value2 = double(mat.at<T>(up, j)) + double(mat.at<T>(down, j) - mat.at<T>(up, j)) * ratio2;
                mat.at<T>(i, j) = static_cast<T>((value1 + value2) / 2.0);
            }
            else if (up >= 0 && down < rows)
            {
                double ratio2 = double(i - up) / double(down - up);
                double value2 = double(mat.at<T>(up, j)) + double(mat.at<T>(down, j) - mat.at<T>(up, j)) * ratio2;
                mat.at<T>(i, j) = static_cast<T>(value2);
            }
            else if (left >= 0 && right < cols)
            {
                double ratio1 = double(j - left) / double(right - left);
                double value1 = double(mat.at<T>(i, left)) + double(mat.at<T>(i, right) - mat.at<T>(i, left)) * ratio1;
                mat.at<T>(i, j) = static_cast<T>(value1);
            }
        }
    }
}

// 辅助函数：基于 OpenCV Mat 创建 GDAL MEM 数据集
static GDALDataset* create_mem_dataset(const cv::Mat& mat, const double geotransform[6], const char* projection) {
    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!poMemDriver) return nullptr;
    
    GDALDataType dt = GDT_Float32;
    if (mat.type() == CV_16S) dt = GDT_Int16;
    else if (mat.type() == CV_32F) dt = GDT_Float32;
    else if (mat.type() == CV_64F) dt = GDT_Float64;

    GDALDataset* ds = poMemDriver->Create("", mat.cols, mat.rows, 1, dt, NULL);
    if (!ds) return nullptr;
    
    ds->SetGeoTransform(const_cast<double*>(geotransform));
    if (projection && strlen(projection) > 0) {
        ds->SetProjection(projection);
    }
    
    GDALRasterBand* band = ds->GetRasterBand(1);
    band->RasterIO(GF_Write, 0, 0, mat.cols, mat.rows, mat.data, mat.cols, mat.rows, dt, 0, 0, NULL);
    return ds;
}

DEMSourceManager::DEMSourceManager() {
    error_msg[0] = '\0';
}

DEMSourceManager::~DEMSourceManager() {
}

int DEMSourceManager::parse_geotiff_metadata(
    const char* file_path,
    int& width,
    int& height,
    double geo_transform[6],
    char* projection,
    int max_proj_len
) {
    GDALAllRegister();
    GDALDataset* poDataset = (GDALDataset*)GDALOpen(file_path, GA_ReadOnly);
    if (!poDataset) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to open GeoTIFF file: %s", file_path);
        return -1;
    }

    width = poDataset->GetRasterXSize();
    height = poDataset->GetRasterYSize();
    poDataset->GetGeoTransform(geo_transform);
    
    const char* proj_wkt = poDataset->GetProjectionRef();
    if (projection && max_proj_len > 0) {
        strncpy_s(projection, max_proj_len, proj_wkt ? proj_wkt : "", _TRUNCATE);
    }

    GDALClose(poDataset);
    return 0;
}

int DEMSourceManager::read_crop_and_resample_dem(
    const char* file_path,
    double min_lon, double max_lon,
    double min_lat, double max_lat,
    double target_resolution,
    cv::Mat& cropped_dem,
    double new_geo_transform[6],
    char* projection,
    int max_proj_len
) {
    cv::Mat source_valid_mask;
    const int ret = read_crop_and_resample_dem_ex(
        file_path, min_lon, max_lon, min_lat, max_lat, target_resolution,
        cropped_dem, source_valid_mask, new_geo_transform, projection, max_proj_len);
    if (ret != 0) return ret;

    // Preserve the legacy API's gap-filling behavior. New mask-aware callers
    // use the _ex overload and retain NoData evidence instead.
    const float output_nodata = -32767.0f;
    fillInvalidGaps<float>(cropped_dem, [output_nodata](float val) {
        return !std::isfinite(val) || std::abs(val - output_nodata) < 1e-3f;
    });
    return 0;
}

int DEMSourceManager::read_crop_and_resample_dem_ex(
    const char* file_path,
    double min_lon, double max_lon,
    double min_lat, double max_lat,
    double target_resolution,
    cv::Mat& cropped_dem,
    cv::Mat& source_valid_mask,
    double new_geo_transform[6],
    char* projection,
    int max_proj_len
) {
    GDALAllRegister();
    OGRRegisterAll();

    cropped_dem.release();
    source_valid_mask.release();

    GDALDataset* poDataset = (GDALDataset*)GDALOpen(file_path, GA_ReadOnly);
    if (!poDataset) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to open input DEM file: %s", file_path);
        return -1;
    }

    double src_transform[6];
    poDataset->GetGeoTransform(src_transform);
    const char* src_proj_wkt = poDataset->GetProjectionRef();

    // 确定源投影参考
    OGRSpatialReference src_srs;
    if (src_proj_wkt && strlen(src_proj_wkt) > 0) {
        src_srs.importFromWkt(src_proj_wkt);
        src_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    } else {
        src_srs.importFromEPSG(4326); // 默认 WGS84
        src_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    // 确定目标投影参考（WGS84，EPSG:4326）
    OGRSpatialReference dst_srs;
    dst_srs.importFromEPSG(4326);
    dst_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* dst_proj_wkt = nullptr;
    dst_srs.exportToWkt(&dst_proj_wkt);

    if (projection && max_proj_len > 0) {
        strncpy_s(projection, max_proj_len, dst_proj_wkt ? dst_proj_wkt : "", _TRUNCATE);
    }

    // 确定目标分辨率 (度)
    double dst_res_lon = 0;
    double dst_res_lat = 0;

    if (target_resolution > 0) {
        double center_lat = (min_lat + max_lat) / 2.0;
        dst_res_lon = target_resolution / (111120.0 * cos(center_lat * M_PI / 180.0));
        dst_res_lat = -target_resolution / 111120.0;
    } else {
        // target_resolution <= 0, 保持原始分辨率大小
        if (src_srs.IsGeographic()) {
            dst_res_lon = src_transform[1];
            dst_res_lat = src_transform[5];
        } else {
            // 源为投影坐标系（米），将源分辨率（米）换算为度数
            double src_res_m = src_transform[1];
            double center_lat = (min_lat + max_lat) / 2.0;
            dst_res_lon = src_res_m / (111120.0 * cos(center_lat * M_PI / 180.0));
            dst_res_lat = -src_res_m / 111120.0;
        }
    }

    // 严密计算目标影像行列数
    int dst_width = (int)ceil((max_lon - min_lon) / dst_res_lon);
    int dst_height = (int)ceil((max_lat - min_lat) / fabs(dst_res_lat));

    if (dst_width <= 0 || dst_height <= 0) {
        sprintf_s(error_msg, sizeof(error_msg), "Invalid target dimensions: %dx%d", dst_width, dst_height);
        GDALClose(poDataset);
        CPLFree(dst_proj_wkt);
        return -1;
    }

    // 新的 GeoTransform
    new_geo_transform[0] = min_lon;
    new_geo_transform[1] = dst_res_lon;
    new_geo_transform[2] = 0;
    new_geo_transform[3] = max_lat;
    new_geo_transform[4] = 0;
    new_geo_transform[5] = dst_res_lat;

    // 创建内存目标数据集
    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!poMemDriver) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to get GDAL MEM driver.");
        GDALClose(poDataset);
        CPLFree(dst_proj_wkt);
        return -1;
    }

    GDALDataset* poDstDS = poMemDriver->Create("", dst_width, dst_height, 1, GDT_Float32, NULL);
    if (!poDstDS) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create destination MEM dataset.");
        GDALClose(poDataset);
        CPLFree(dst_proj_wkt);
        return -1;
    }

    poDstDS->SetGeoTransform(new_geo_transform);
    poDstDS->SetProjection(dst_proj_wkt);

    // NoData 值处理，防止重采样边缘产生黑边
    GDALRasterBand* poSrcBand = poDataset->GetRasterBand(1);
    GDALRasterBand* poDstBand = poDstDS->GetRasterBand(1);
    
    const double output_nodata = -32767.0;
    poDstBand->SetNoDataValue(output_nodata);

    // Materialize the source validity evidence before resampling. GDAL's
    // convenience reprojection API does not guarantee that a source NoData or
    // mask band becomes a readable destination mask, especially when source
    // and destination NoData values differ.
    const int src_width = poDataset->GetRasterXSize();
    const int src_height = poDataset->GetRasterYSize();
    cv::Mat source_mask(src_height, src_width, CV_8UC1);
    GDALRasterBand* poSrcMaskBand = poSrcBand ? poSrcBand->GetMaskBand() : nullptr;
    CPLErr err = poSrcMaskBand ? poSrcMaskBand->RasterIO(
        GF_Read, 0, 0, src_width, src_height,
        source_mask.data, src_width, src_height,
        GDT_Byte, 0, 0, NULL) : CE_Failure;
    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to read source DEM validity mask.");
        GDALClose(poDstDS);
        GDALClose(poDataset);
        CPLFree(dst_proj_wkt);
        return -1;
    }

    int has_source_nodata = 0;
    const double source_nodata = poSrcBand->GetNoDataValue(&has_source_nodata);
    std::vector<float> source_row(static_cast<size_t>(src_width));
    for (int row = 0; row < src_height; ++row) {
        err = poSrcBand->RasterIO(GF_Read, 0, row, src_width, 1,
            source_row.data(), src_width, 1, GDT_Float32, 0, 0, NULL);
        if (err != CE_None) {
            sprintf_s(error_msg, sizeof(error_msg), "Failed to inspect source DEM row %d.", row);
            GDALClose(poDstDS);
            GDALClose(poDataset);
            CPLFree(dst_proj_wkt);
            return -1;
        }
        unsigned char* mask_values = source_mask.ptr<unsigned char>(row);
        for (int column = 0; column < src_width; ++column) {
            const float elevation = source_row[static_cast<size_t>(column)];
            const bool matches_nodata = has_source_nodata &&
                ((std::isnan(source_nodata) && std::isnan(elevation)) ||
                 (!std::isnan(source_nodata) &&
                  std::abs(static_cast<double>(elevation) - source_nodata) < 1e-3));
            mask_values[column] = mask_values[column] != 0 &&
                std::isfinite(elevation) && !matches_nodata ? 255 : 0;
        }
    }

    GDALDataset* poSrcMaskDS = poMemDriver->Create("", src_width, src_height, 1, GDT_Byte, NULL);
    GDALDataset* poDstSupportDS = poMemDriver->Create("", dst_width, dst_height, 1, GDT_Float32, NULL);
    if (!poSrcMaskDS || !poDstSupportDS) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create DEM validity warp datasets.");
        if (poSrcMaskDS) GDALClose(poSrcMaskDS);
        if (poDstSupportDS) GDALClose(poDstSupportDS);
        GDALClose(poDstDS);
        GDALClose(poDataset);
        CPLFree(dst_proj_wkt);
        return -1;
    }
    const char* effective_src_wkt = src_proj_wkt && strlen(src_proj_wkt) > 0
        ? src_proj_wkt : dst_proj_wkt;
    poSrcMaskDS->SetGeoTransform(src_transform);
    poSrcMaskDS->SetProjection(effective_src_wkt);
    poDstSupportDS->SetGeoTransform(new_geo_transform);
    poDstSupportDS->SetProjection(dst_proj_wkt);
    poDstSupportDS->GetRasterBand(1)->Fill(0.0);
    err = poSrcMaskDS->GetRasterBand(1)->RasterIO(
        GF_Write, 0, 0, src_width, src_height,
        source_mask.data, src_width, src_height,
        GDT_Byte, 0, 0, NULL);
    if (err == CE_None) {
        err = GDALReprojectImage(
            poSrcMaskDS, effective_src_wkt,
            poDstSupportDS, dst_proj_wkt,
            GRA_Average, 0.0, 0.0, NULL, NULL, NULL);
    }
    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to reproject DEM validity support.");
        GDALClose(poSrcMaskDS);
        GDALClose(poDstSupportDS);
        GDALClose(poDstDS);
        GDALClose(poDataset);
        CPLFree(dst_proj_wkt);
        return -1;
    }

    // 调用 GDAL 严密影像重投影 warp 算法（已内置像素几何中心采样 0.5 偏置）
    err = GDALReprojectImage(
        poDataset, src_proj_wkt,
        poDstDS, dst_proj_wkt,
        GRA_Bilinear, 0.0, 0.0, NULL, NULL, NULL
    );

    CPLFree(dst_proj_wkt);

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "GDALReprojectImage failed with error code %d", err);
        GDALClose(poSrcMaskDS);
        GDALClose(poDstSupportDS);
        GDALClose(poDstDS);
        GDALClose(poDataset);
        return -1;
    }

    // 读入 OpenCV Mat
    cropped_dem.create(dst_height, dst_width, CV_32FC1);
    err = poDstBand->RasterIO(
        GF_Read, 0, 0, dst_width, dst_height,
        cropped_dem.data, dst_width, dst_height,
        GDT_Float32, 0, 0, NULL
    );

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to read target band data into OpenCV Mat.");
        GDALClose(poSrcMaskDS);
        GDALClose(poDstSupportDS);
        GDALClose(poDstDS);
        GDALClose(poDataset);
        return -1;
    }

    cv::Mat resampled_support(dst_height, dst_width, CV_32FC1);
    err = poDstSupportDS->GetRasterBand(1)->RasterIO(
        GF_Read, 0, 0, dst_width, dst_height,
        resampled_support.data, dst_width, dst_height,
        GDT_Float32, 0, 0, NULL);
    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to read reprojected DEM validity mask.");
        GDALClose(poSrcMaskDS);
        GDALClose(poDstSupportDS);
        GDALClose(poDstDS);
        GDALClose(poDataset);
        cropped_dem.release();
        source_valid_mask.release();
        return -1;
    }

    source_valid_mask.create(dst_height, dst_width, CV_8UC1);
    // Average-resampled support is 255 only when the complete contributing
    // source footprint is valid. Any partial support fails closed.
    for (int row = 0; row < dst_height; ++row) {
        float* elevations = cropped_dem.ptr<float>(row);
        const float* support = resampled_support.ptr<float>(row);
        unsigned char* valid = source_valid_mask.ptr<unsigned char>(row);
        for (int column = 0; column < dst_width; ++column) {
            const bool is_valid = support[column] >= 254.5f &&
                std::isfinite(elevations[column]) &&
                std::abs(static_cast<double>(elevations[column]) - output_nodata) >= 1e-3;
            valid[column] = is_valid ? 1 : 0;
            if (!is_valid) elevations[column] = static_cast<float>(output_nodata);
        }
    }

    // Bilinear DEM interpolation can otherwise normalize around a NoData
    // neighbour. Eroding one target pixel conservatively requires complete
    // local source support at validity boundaries.
    cv::erode(source_valid_mask, source_valid_mask,
        cv::Mat::ones(3, 3, CV_8U), cv::Point(-1, -1), 1,
        cv::BORDER_CONSTANT, cv::Scalar(0));
    cropped_dem.setTo(static_cast<float>(output_nodata), source_valid_mask == 0);

    GDALClose(poSrcMaskDS);
    GDALClose(poDstSupportDS);
    GDALClose(poDstDS);
    GDALClose(poDataset);

    return 0;
}

int DEMSourceManager::parse_geotiff(
    const char* file_path,
    cv::Mat& dem_data,
    double geo_transform[6],
    char* projection,
    int max_proj_len
) {
    GDALAllRegister();
    GDALDataset* poDataset = (GDALDataset*)GDALOpen(file_path, GA_ReadOnly);
    if (!poDataset) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to open GeoTIFF file: %s", file_path);
        return -1;
    }

    poDataset->GetGeoTransform(geo_transform);
    const char* proj_wkt = poDataset->GetProjectionRef();
    if (projection && max_proj_len > 0) {
        strncpy_s(projection, max_proj_len, proj_wkt ? proj_wkt : "", _TRUNCATE);
    }

    int width = poDataset->GetRasterXSize();
    int height = poDataset->GetRasterYSize();

    dem_data.create(height, width, CV_32FC1);
    CPLErr err = poDataset->GetRasterBand(1)->RasterIO(
        GF_Read, 0, 0, width, height,
        dem_data.data, width, height,
        GDT_Float32, 0, 0, NULL
    );

    GDALClose(poDataset);

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to read raster data in parse_geotiff.");
        return -1;
    }

    return 0;
}

int DEMSourceManager::crop_to_aoi(
    const cv::Mat& dem_data,
    const double geo_transform[6],
    double min_lon, double max_lon,
    double min_lat, double max_lat,
    cv::Mat& cropped_dem,
    double new_geo_transform[6]
) {
    if (dem_data.empty()) {
        sprintf_s(error_msg, sizeof(error_msg), "Input DEM data is empty.");
        return -1;
    }
    
    GDALAllRegister();
    OGRRegisterAll();

    OGRSpatialReference wgs84;
    wgs84.importFromEPSG(4326);
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* wgs84_wkt = nullptr;
    wgs84.exportToWkt(&wgs84_wkt);

    GDALDataset* src_ds = create_mem_dataset(dem_data, geo_transform, wgs84_wkt);
    CPLFree(wgs84_wkt);
    if (!src_ds) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create MEM dataset from OpenCV Mat.");
        return -1;
    }

    double dst_res_lon = geo_transform[1];
    double dst_res_lat = geo_transform[5];
    
    int dst_width = (int)ceil((max_lon - min_lon) / dst_res_lon);
    int dst_height = (int)ceil((max_lat - min_lat) / fabs(dst_res_lat));
    
    if (dst_width <= 0 || dst_height <= 0) {
        sprintf_s(error_msg, sizeof(error_msg), "Invalid cropped dimensions: %dx%d", dst_width, dst_height);
        GDALClose(src_ds);
        return -1;
    }

    new_geo_transform[0] = min_lon;
    new_geo_transform[1] = dst_res_lon;
    new_geo_transform[2] = 0;
    new_geo_transform[3] = max_lat;
    new_geo_transform[4] = 0;
    new_geo_transform[5] = dst_res_lat;

    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* dst_ds = poMemDriver->Create("", dst_width, dst_height, 1, GDT_Float32, NULL);
    if (!dst_ds) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create destination MEM dataset.");
        GDALClose(src_ds);
        return -1;
    }

    char* proj_wkt = nullptr;
    wgs84.exportToWkt(&proj_wkt);
    dst_ds->SetGeoTransform(new_geo_transform);
    dst_ds->SetProjection(proj_wkt);

    CPLErr err = GDALReprojectImage(
        src_ds, proj_wkt,
        dst_ds, proj_wkt,
        GRA_Bilinear, 0.0, 0.0, NULL, NULL, NULL
    );
    CPLFree(proj_wkt);

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "GDALReprojectImage failed in crop_to_aoi.");
        GDALClose(dst_ds);
        GDALClose(src_ds);
        return -1;
    }

    cropped_dem.create(dst_height, dst_width, CV_32FC1);
    err = dst_ds->GetRasterBand(1)->RasterIO(
        GF_Read, 0, 0, dst_width, dst_height,
        cropped_dem.data, dst_width, dst_height,
        GDT_Float32, 0, 0, NULL
    );

    GDALClose(dst_ds);
    GDALClose(src_ds);

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to read data into output OpenCV Mat in crop_to_aoi.");
        return -1;
    }

    return 0;
}

int DEMSourceManager::resample_dem(
    const cv::Mat& dem_data,
    const double geo_transform[6],
    double target_resolution,
    cv::Mat& resampled_dem,
    double new_geo_transform[6]
) {
    if (dem_data.empty()) {
        sprintf_s(error_msg, sizeof(error_msg), "Input DEM data is empty.");
        return -1;
    }

    GDALAllRegister();
    OGRRegisterAll();

    OGRSpatialReference wgs84;
    wgs84.importFromEPSG(4326);
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* wgs84_wkt = nullptr;
    wgs84.exportToWkt(&wgs84_wkt);

    GDALDataset* src_ds = create_mem_dataset(dem_data, geo_transform, wgs84_wkt);
    CPLFree(wgs84_wkt);
    if (!src_ds) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create MEM dataset from OpenCV Mat.");
        return -1;
    }

    double min_lon = geo_transform[0];
    double max_lon = geo_transform[0] + dem_data.cols * geo_transform[1];
    double min_lat = geo_transform[3] + dem_data.rows * geo_transform[5];
    double max_lat = geo_transform[3];
    double center_lat = (min_lat + max_lat) / 2.0;

    double dst_res_lon = target_resolution / (111120.0 * cos(center_lat * M_PI / 180.0));
    double dst_res_lat = -target_resolution / 111120.0;

    int dst_width = (int)ceil((max_lon - min_lon) / dst_res_lon);
    int dst_height = (int)ceil((max_lat - min_lat) / fabs(dst_res_lat));

    if (dst_width <= 0 || dst_height <= 0) {
        sprintf_s(error_msg, sizeof(error_msg), "Invalid resampled dimensions: %dx%d", dst_width, dst_height);
        GDALClose(src_ds);
        return -1;
    }

    new_geo_transform[0] = min_lon;
    new_geo_transform[1] = dst_res_lon;
    new_geo_transform[2] = 0;
    new_geo_transform[3] = max_lat;
    new_geo_transform[4] = 0;
    new_geo_transform[5] = dst_res_lat;

    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* dst_ds = poMemDriver->Create("", dst_width, dst_height, 1, GDT_Float32, NULL);
    if (!dst_ds) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create destination MEM dataset.");
        GDALClose(src_ds);
        return -1;
    }

    char* proj_wkt = nullptr;
    wgs84.exportToWkt(&proj_wkt);
    dst_ds->SetGeoTransform(new_geo_transform);
    dst_ds->SetProjection(proj_wkt);

    CPLErr err = GDALReprojectImage(
        src_ds, proj_wkt,
        dst_ds, proj_wkt,
        GRA_Bilinear, 0.0, 0.0, NULL, NULL, NULL
    );
    CPLFree(proj_wkt);

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "GDALReprojectImage failed in resample_dem.");
        GDALClose(dst_ds);
        GDALClose(src_ds);
        return -1;
    }

    resampled_dem.create(dst_height, dst_width, CV_32FC1);
    err = dst_ds->GetRasterBand(1)->RasterIO(
        GF_Read, 0, 0, dst_width, dst_height,
        resampled_dem.data, dst_width, dst_height,
        GDT_Float32, 0, 0, NULL
    );

    GDALClose(dst_ds);
    GDALClose(src_ds);

    if (err != CE_None) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to read data into output OpenCV Mat in resample_dem.");
        return -1;
    }

    return 0;
}

int DEMSourceManager::convert_dem_to_ecef(
    const cv::Mat& dem_data,
    const double geo_transform[6],
    const char* projection,
    cv::Mat& dem_x,
    cv::Mat& dem_y,
    cv::Mat& dem_z
) {
    if (dem_data.empty()) {
        sprintf_s(error_msg, sizeof(error_msg), "Input DEM data is empty.");
        return -1;
    }

    GDALAllRegister();
    OGRRegisterAll();

    // 建立源参考坐标系
    OGRSpatialReference src_srs;
    if (projection && strlen(projection) > 0) {
        src_srs.importFromWkt(projection);
        src_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    } else {
        src_srs.importFromEPSG(4326); // 默认 WGS84
        src_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    // 建立目标参考坐标系（WGS84 地心直角 ECEF，EPSG:4978）
    OGRSpatialReference dst_srs;
    dst_srs.importFromEPSG(4978);
    dst_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // 创建投影转换器
    OGRCoordinateTransformation* poCT = OGRCreateCoordinateTransformation(&src_srs, &dst_srs);
    if (!poCT) {
        sprintf_s(error_msg, sizeof(error_msg), "Failed to create coordinate transformation to ECEF (EPSG:4978).");
        return -1;
    }

    int rows = dem_data.rows;
    int cols = dem_data.cols;

    // 动态类型转换以确保类型安全，规避指针 Mismatch Bug
    cv::Mat dem_32f = dem_data;
    if (dem_32f.type() != CV_32F) {
        dem_32f.convertTo(dem_32f, CV_32F);
    }

    dem_x.create(rows, cols, CV_32FC1);
    dem_y.create(rows, cols, CV_32FC1);
    dem_z.create(rows, cols, CV_32FC1);

    std::vector<double> x_arr(cols);
    std::vector<double> y_arr(cols);
    std::vector<double> z_arr(cols);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            // 应用严密的仿射变换公式（支持图像旋转，中心采样含 0.5 偏置）
            x_arr[c] = geo_transform[0] + (c + 0.5) * geo_transform[1] + (r + 0.5) * geo_transform[2];
            y_arr[c] = geo_transform[3] + (c + 0.5) * geo_transform[4] + (r + 0.5) * geo_transform[5];
            z_arr[c] = dem_32f.at<float>(r, c);
        }

        // 行级分批投影转换以大幅提高性能
        poCT->Transform(cols, x_arr.data(), y_arr.data(), z_arr.data());

        for (int c = 0; c < cols; c++) {
            dem_x.at<float>(r, c) = static_cast<float>(x_arr[c]);
            dem_y.at<float>(r, c) = static_cast<float>(y_arr[c]);
            dem_z.at<float>(r, c) = static_cast<float>(z_arr[c]);
        }
    }

    OGRCoordinateTransformation::DestroyCT(poCT);
    return 0;
}
