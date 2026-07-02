// include/GCPManager.h
#pragma once

#ifdef GCP_MANAGER_EXPORTS
#define GCP_MANAGER_API __declspec(dllexport)
#else
#define GCP_MANAGER_API __declspec(dllimport)
#endif

#include "GCPPoint.h"
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

class GCP_MANAGER_API GCPManager {
public:
    GCPManager();
    ~GCPManager();

    // 1. 坐标转换：WGS84 经纬高转换为 ECEF 地心坐标 (X, Y, Z)
    int convert_gcp_to_ecef(
        const std::vector<GCPPoint>& gcps,
        std::vector<double>& gcp_x,
        std::vector<double>& gcp_y,
        std::vector<double>& gcp_z
    );

    // 2. 几何映射计算：基于地理编码多项式系数计算影像行列号 (输出为 double 亚像素值)
    int compute_image_coordinates(
        const std::vector<GCPPoint>& gcps,
        const cv::Mat& row_coefficient,
        const cv::Mat& col_coefficient,
        int scene_height, int scene_width,
        int offset_row, int offset_col,
        std::vector<double>& computed_rows,
        std::vector<double>& computed_cols
    );

    // 3. 配准精度评估：计算影像坐标与理论几何投影的残差，并写回 GCPPoint 的残差字段
    int evaluate_coregistration_accuracy(
        std::vector<GCPPoint>& gcps,
        const cv::Mat& row_coefficient,
        const cv::Mat& col_coefficient,
        int scene_height, int scene_width,
        int offset_row, int offset_col,
        double range_spacing, double azimuth_spacing,
        GCPEvaluationResult& result
    );

    // 4. 离群点检测：基于 2-Sigma 准则过滤并标记粗差 (修改 quality 等级为 0 代表无效/离群)
    int detect_outliers(
        std::vector<GCPPoint>& gcps,
        double threshold_sigma = 2.0
    );

    // 5. 生成精度评估报告文本
    int generate_evaluation_report(
        const GCPEvaluationResult& result,
        const std::vector<GCPPoint>& gcps,
        std::string& report_text
    );
};
