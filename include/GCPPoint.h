// include/GCPPoint.h
#pragma once
#include <string>
#include <vector>
#include <limits>
#include <cmath>

struct GCPPoint {
    int id;                     // 唯一标识符
    double lon;                 // 经度 (度, WGS84)
    double lat;                 // 纬度 (度, WGS84)
    double height;              // 高程 (米)
    double row;                 // 影像行号 (方位向，支持亚像素，未标注为 NaN)
    double col;                 // 影像列号 (距离向，支持亚像素，未标注为 NaN)
    double residual_range;      // 距离向残差 (米，未计算为 NaN)
    double residual_azimuth;    // 方位向残差 (米，未计算为 NaN)
    double residual_height;     // 高程残差 (米，一般设为 0.0 或 NaN)
    double coherence;           // 相干系数 (未计算为 NaN)
    std::string description;    // 描述信息
    std::string timestamp;      // 采集/标注时间
    std::string source;         // 数据来源: "manual", "import" 等
    int quality;                // 质量等级 (0=无效/低, 1=中, 2=高)

    // 构造函数初始化默认值
    GCPPoint() : 
        id(-1), 
        lon(0.0), 
        lat(0.0), 
        height(0.0), 
        row(std::numeric_limits<double>::quiet_NaN()), 
        col(std::numeric_limits<double>::quiet_NaN()),
        residual_range(std::numeric_limits<double>::quiet_NaN()),
        residual_azimuth(std::numeric_limits<double>::quiet_NaN()),
        residual_height(std::numeric_limits<double>::quiet_NaN()),
        coherence(std::numeric_limits<double>::quiet_NaN()),
        quality(1) {}

    // 判断该点是否已被标注影像坐标
    bool isAnnotated() const {
        return (!std::isnan(row) && !std::isnan(col) && row >= 0.0 && col >= 0.0);
    }
};

struct GCPEvaluationResult {
    double mean_residual_range;     // 平均距离向残差 (米)
    double mean_residual_azimuth;   // 平均方位向残差 (米)
    double mean_residual_height;    // 平均高程残差 (米)
    double rms_residual_2d;         // 2D RMS 残差 (米)
    double rms_residual_3d;         // 3D RMS 残差 (米)
    int num_gcp_used;               // 参与评估的 GCP 数量
    int num_gcp_rejected;           // 被拒绝的离群点数量
    std::vector<int> rejected_ids;  // 被拒绝的 GCP ID 列表

    GCPEvaluationResult() :
        mean_residual_range(0.0),
        mean_residual_azimuth(0.0),
        mean_residual_height(0.0),
        rms_residual_2d(0.0),
        rms_residual_3d(0.0),
        num_gcp_used(0),
        num_gcp_rejected(0) {}
};
