#pragma once
#ifndef __DEM_SOURCE_MANAGER__H__
#define __DEM_SOURCE_MANAGER__H__

#include "..\include\Package.h"
#include <opencv2/opencv.hpp>

enum class DEMSource {
    SRTM1,      // SRTM 1" (~30m)
    SRTM3,      // SRTM 3" (~90m)
    COPERNICUS, // Copernicus DEM (~30m)
    ASTER       // ASTER GDEM v3 (~30m)
};

class InSAR_API DEMSourceManager {
public:
    DEMSourceManager();
    ~DEMSourceManager();

    // 存放错误信息，避免跨 DLL 传递 std::string 发生 ABI 冲突
    char error_msg[512];

    /**
     * @brief 解析 GeoTIFF 文件的基本元数据（尺寸、地理变换矩阵和投影参考）
     *        注意：此函数仅打开文件并读取元数据，不读入完整的栅格矩阵以防内存溢出。
     */
    int parse_geotiff_metadata(
        const char* file_path,
        int& width,
        int& height,
        double geo_transform[6],
        char* projection,
        int max_proj_len
    );

    /**
     * @brief 根据目标 AOI 范围，直接利用 GDAL 窗口读取机制裁剪并重采样 GeoTIFF 文件，直接读入 cropped_dem 中。
     *        支持多瓦片合并（可通过 VRT 文件路径作为 file_path 传入）。
     *        集成了半像素偏差纠正 (Half-pixel Shift) 和空值(NoData)填充。
     * 
     * @param file_path 输入 GeoTIFF 或 VRT 文件的绝对路径
     * @param min_lon 最小经度
     * @param max_lon 最大经度
     * @param min_lat 最小纬度
     * @param max_lat 最大纬度
     * @param target_resolution 目标分辨率（米），若 <= 0 则保持原图分辨率
     * @param cropped_dem 输出裁剪重采样后的 DEM 矩阵 (CV_32FC1)
     * @param new_geo_transform 输出裁剪后新的地理变换矩阵
     * @param projection 输出坐标系的投影参考字符串 (WKT)
     * @param max_proj_len 投影字符串缓冲区的最大长度
     */
    int read_crop_and_resample_dem(
        const char* file_path,
        double min_lon, double max_lon,
        double min_lat, double max_lat,
        double target_resolution,
        cv::Mat& cropped_dem,
        double new_geo_transform[6],
        char* projection,
        int max_proj_len
    );

    /**
     * @brief 解析 GeoTIFF 文件元数据与矩阵（不推荐用于超大图）
     */
    int parse_geotiff(
        const char* file_path,
        cv::Mat& dem_data,
        double geo_transform[6],
        char* projection,
        int max_proj_len
    );

    /**
     * @brief 裁剪到指定的经纬度 AOI 范围（内存中处理，支持半像素纠偏）
     */
    int crop_to_aoi(
        const cv::Mat& dem_data,
        const double geo_transform[6],
        double min_lon, double max_lon,
        double min_lat, double max_lat,
        cv::Mat& cropped_dem,
        double new_geo_transform[6]
    );

    /**
     * @brief 重采样至目标分辨率（米）（内存中处理）
     */
    int resample_dem(
        const cv::Mat& dem_data,
        const double geo_transform[6],
        double target_resolution,
        cv::Mat& resampled_dem,
        double new_geo_transform[6]
    );

    /**
     * @brief 经纬高 WGS84 转换为 ECEF X/Y/Z 坐标
     *        支持投影坐标系（如 UTM 等）的严密坐标系转换，如果输入是投影坐标系，会利用 GDAL/PROJ 转换为地心坐标。
     * 
     * @param dem_data 高程矩阵 (米)
     * @param geo_transform 地理变换矩阵
     * @param projection 投影参考字符串 (WKT)
     * @param dem_x 输出 ECEF X 矩阵 (CV_32FC1)
     * @param dem_y 输出 ECEF Y 矩阵 (CV_32FC1)
     * @param dem_z 输出 ECEF Z 矩阵 (CV_32FC1)
     */
    int convert_dem_to_ecef(
        const cv::Mat& dem_data,
        const double geo_transform[6],
        const char* projection,
        cv::Mat& dem_x,
        cv::Mat& dem_y,
        cv::Mat& dem_z
    );
};

#endif // __DEM_SOURCE_MANAGER__H__
