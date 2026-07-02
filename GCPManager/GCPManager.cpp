// GCPManager/GCPManager.cpp
#include "../include/GCPManager.h"
#include "../include/Utils.h"

#ifdef _DEBUG
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#else
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#endif // _DEBUG

#include <cmath>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <algorithm>

GCPManager::GCPManager() {}
GCPManager::~GCPManager() {}

int GCPManager::convert_gcp_to_ecef(
    const std::vector<GCPPoint>& gcps,
    std::vector<double>& gcp_x,
    std::vector<double>& gcp_y,
    std::vector<double>& gcp_z
) {
    gcp_x.clear();
    gcp_y.clear();
    gcp_z.clear();

    gcp_x.reserve(gcps.size());
    gcp_y.reserve(gcps.size());
    gcp_z.reserve(gcps.size());

    for (const auto& gcp : gcps) {
        Position ecef;
        int ret = Utils::ell2xyz(gcp.lon, gcp.lat, gcp.height, ecef);
        if (ret != 0) {
            fprintf(stderr, "GCPManager::convert_gcp_to_ecef: Utils::ell2xyz failed for GCP ID %d\n", gcp.id);
            return -1;
        }
        gcp_x.push_back(ecef.x);
        gcp_y.push_back(ecef.y);
        gcp_z.push_back(ecef.z);
    }
    return 0;
}

int GCPManager::compute_image_coordinates(
    const std::vector<GCPPoint>& gcps,
    const cv::Mat& row_coefficient,
    const cv::Mat& col_coefficient,
    int scene_height, int scene_width,
    int offset_row, int offset_col,
    std::vector<double>& computed_rows,
    std::vector<double>& computed_cols
) {
    computed_rows.clear();
    computed_cols.clear();
    computed_rows.resize(gcps.size(), std::numeric_limits<double>::quiet_NaN());
    computed_cols.resize(gcps.size(), std::numeric_limits<double>::quiet_NaN());

    if (row_coefficient.empty() || col_coefficient.empty()) {
        fprintf(stderr, "GCPManager::compute_image_coordinates: Coefficients are empty!\n");
        return -1;
    }

    Utils util;
    cv::Mat lonMat = cv::Mat::zeros(1, 1, CV_64F);
    cv::Mat latMat = cv::Mat::zeros(1, 1, CV_64F);
    cv::Mat outMat = cv::Mat::zeros(1, 1, CV_64F);

    for (size_t i = 0; i < gcps.size(); ++i) {
        lonMat.at<double>(0, 0) = gcps[i].lon;
        latMat.at<double>(0, 0) = gcps[i].lat;

        // 计算行号 (方位向)
        int ret = util.coord_conversion(const_cast<cv::Mat&>(row_coefficient), lonMat, latMat, outMat);
        if (ret != 0) {
            fprintf(stderr, "GCPManager::compute_image_coordinates: coord_conversion for row failed for GCP ID %d\n", gcps[i].id);
            return -1;
        }
        double raw_row = outMat.at<double>(0, 0);

        // 计算列号 (距离向)
        ret = util.coord_conversion(const_cast<cv::Mat&>(col_coefficient), lonMat, latMat, outMat);
        if (ret != 0) {
            fprintf(stderr, "GCPManager::compute_image_coordinates: coord_conversion for col failed for GCP ID %d\n", gcps[i].id);
            return -1;
        }
        double raw_col = outMat.at<double>(0, 0);

        computed_rows[i] = raw_row - offset_row;
        computed_cols[i] = raw_col - offset_col;
    }
    return 0;
}

int GCPManager::evaluate_coregistration_accuracy(
    std::vector<GCPPoint>& gcps,
    const cv::Mat& row_coefficient,
    const cv::Mat& col_coefficient,
    int scene_height, int scene_width,
    int offset_row, int offset_col,
    double range_spacing, double azimuth_spacing,
    GCPEvaluationResult& result
) {
    if (gcps.empty()) {
        result = GCPEvaluationResult();
        return 0;
    }

    std::vector<double> computed_rows;
    std::vector<double> computed_cols;
    int ret = compute_image_coordinates(
        gcps, row_coefficient, col_coefficient, 
        scene_height, scene_width, offset_row, offset_col, 
        computed_rows, computed_cols
    );
    if (ret != 0) {
        fprintf(stderr, "GCPManager::evaluate_coregistration_accuracy: compute_image_coordinates failed!\n");
        return -1;
    }

    double sum_range = 0.0;
    double sum_azimuth = 0.0;
    double sum_sq_2d = 0.0;
    int num_used = 0;
    int num_rejected = 0;
    std::vector<int> rejected;

    for (size_t i = 0; i < gcps.size(); ++i) {
        auto& gcp = gcps[i];
        
        // 离群点或无效点计数
        if (gcp.quality <= 0) {
            if (gcp.isAnnotated()) {
                num_rejected++;
                rejected.push_back(gcp.id);
            }
            gcp.residual_range = std::numeric_limits<double>::quiet_NaN();
            gcp.residual_azimuth = std::numeric_limits<double>::quiet_NaN();
            gcp.residual_height = std::numeric_limits<double>::quiet_NaN();
            continue;
        }

        if (!gcp.isAnnotated()) {
            gcp.residual_range = std::numeric_limits<double>::quiet_NaN();
            gcp.residual_azimuth = std::numeric_limits<double>::quiet_NaN();
            gcp.residual_height = std::numeric_limits<double>::quiet_NaN();
            continue;
        }

        // 计算行号和列号差值 (像素)
        double diff_row = gcp.row - computed_rows[i];
        double diff_col = gcp.col - computed_cols[i];

        // 转换到物理米制残差
        gcp.residual_azimuth = diff_row * azimuth_spacing; // 行号对应方位向
        gcp.residual_range = diff_col * range_spacing;     // 列号对应距离向
        gcp.residual_height = 0.0;

        sum_range += gcp.residual_range;
        sum_azimuth += gcp.residual_azimuth;
        
        double dist_sq = gcp.residual_range * gcp.residual_range + gcp.residual_azimuth * gcp.residual_azimuth;
        sum_sq_2d += dist_sq;
        num_used++;
    }

    result.num_gcp_used = num_used;
    result.num_gcp_rejected = num_rejected;
    result.rejected_ids = rejected;

    if (num_used > 0) {
        result.mean_residual_range = sum_range / num_used;
        result.mean_residual_azimuth = sum_azimuth / num_used;
        result.mean_residual_height = 0.0;
        result.rms_residual_2d = std::sqrt(sum_sq_2d / num_used);
        result.rms_residual_3d = result.rms_residual_2d;
    } else {
        result.mean_residual_range = 0.0;
        result.mean_residual_azimuth = 0.0;
        result.mean_residual_height = 0.0;
        result.rms_residual_2d = 0.0;
        result.rms_residual_3d = 0.0;
    }

    return 0;
}

int GCPManager::detect_outliers(
    std::vector<GCPPoint>& gcps,
    double threshold_sigma
) {
    std::vector<double> res_ranges;
    std::vector<double> res_azimuths;
    std::vector<size_t> active_indices;

    for (size_t i = 0; i < gcps.size(); ++i) {
        const auto& gcp = gcps[i];
        if (gcp.quality > 0 && gcp.isAnnotated() && 
            !std::isnan(gcp.residual_range) && !std::isnan(gcp.residual_azimuth)) {
            res_ranges.push_back(gcp.residual_range);
            res_azimuths.push_back(gcp.residual_azimuth);
            active_indices.push_back(i);
        }
    }

    size_t n = active_indices.size();
    if (n < 3) {
        // 点数太少，不足以做统计学粗差剔除
        return 0;
    }

    // 计算均值
    double mean_r = std::accumulate(res_ranges.begin(), res_ranges.end(), 0.0) / n;
    double mean_a = std::accumulate(res_azimuths.begin(), res_azimuths.end(), 0.0) / n;

    // 计算标准差
    double sq_sum_r = 0.0;
    double sq_sum_a = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sq_sum_r += (res_ranges[i] - mean_r) * (res_ranges[i] - mean_r);
        sq_sum_a += (res_azimuths[i] - mean_a) * (res_azimuths[i] - mean_a);
    }
    double sigma_r = std::sqrt(sq_sum_r / (n - 1));
    double sigma_a = std::sqrt(sq_sum_a / (n - 1));

    // 如果 sigma 过小，设为极小值以防除以零
    if (sigma_r < 1e-6) sigma_r = 1e-6;
    if (sigma_a < 1e-6) sigma_a = 1e-6;

    int new_rejected = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = active_indices[i];
        double dev_r = std::abs(gcps[idx].residual_range - mean_r);
        double dev_a = std::abs(gcps[idx].residual_azimuth - mean_a);

        if (dev_r > threshold_sigma * sigma_r || dev_a > threshold_sigma * sigma_a) {
            gcps[idx].quality = 0; // 标记为离群/无效点
            new_rejected++;
        }
    }

    return new_rejected;
}

int GCPManager::generate_evaluation_report(
    const GCPEvaluationResult& result,
    const std::vector<GCPPoint>& gcps,
    std::string& report_text
) {
    std::stringstream ss;
    ss << "==================================================\n";
    ss << "             GCP Coregistration Report            \n";
    ss << "==================================================\n";
    ss << "Evaluation Summary:\n";
    ss << "  Total GCPs Evaluated : " << (result.num_gcp_used + result.num_gcp_rejected) << "\n";
    ss << "  Active GCPs Used     : " << result.num_gcp_used << "\n";
    ss << "  Rejected Outliers    : " << result.num_gcp_rejected << "\n";
    ss << "Residuals Metrics (Physical Meters):\n";
    ss << std::fixed << std::setprecision(4);
    ss << "  Mean Range Residual   : " << result.mean_residual_range << " m\n";
    ss << "  Mean Azimuth Residual : " << result.mean_residual_azimuth << " m\n";
    ss << "  RMS 2D Residual       : " << result.rms_residual_2d << " m\n";
    ss << "  RMS 3D Residual       : " << result.rms_residual_3d << " m\n";
    
    if (result.num_gcp_rejected > 0) {
        ss << "Rejected GCP IDs       : ";
        for (size_t i = 0; i < result.rejected_ids.size(); ++i) {
            ss << result.rejected_ids[i];
            if (i < result.rejected_ids.size() - 1) ss << ", ";
        }
        ss << "\n";
    }
    
    ss << "\nDetailed GCP Residuals Table:\n";
    ss << "----------------------------------------------------------------------------------------------------\n";
    ss << std::left << std::setw(6) << "ID" 
       << std::setw(12) << "Source" 
       << std::setw(12) << "Lon(deg)" 
       << std::setw(12) << "Lat(deg)" 
       << std::setw(10) << "Row(pix)" 
       << std::setw(10) << "Col(pix)" 
       << std::setw(12) << "Res_Rng(m)" 
       << std::setw(12) << "Res_Azi(m)" 
       << "Quality\n";
    ss << "----------------------------------------------------------------------------------------------------\n";
    
    for (const auto& gcp : gcps) {
        ss << std::left << std::setw(6) << gcp.id 
           << std::setw(12) << gcp.source.substr(0, 10)
           << std::setw(12) << gcp.lon 
           << std::setw(12) << gcp.lat;
        
        if (gcp.isAnnotated()) {
            ss << std::setw(10) << gcp.row 
               << std::setw(10) << gcp.col;
        } else {
            ss << std::setw(10) << "N/A" 
               << std::setw(10) << "N/A";
        }

        if (!std::isnan(gcp.residual_range)) {
            ss << std::setw(12) << gcp.residual_range;
        } else {
            ss << std::setw(12) << "N/A";
        }

        if (!std::isnan(gcp.residual_azimuth)) {
            ss << std::setw(12) << gcp.residual_azimuth;
        } else {
            ss << std::setw(12) << "N/A";
        }

        ss << gcp.quality << "\n";
    }
    ss << "----------------------------------------------------------------------------------------------------\n";
    
    report_text = ss.str();
    return 0;
}
