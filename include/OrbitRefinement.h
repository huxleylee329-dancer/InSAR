// include/OrbitRefinement.h
#pragma once

#ifdef GCP_MANAGER_EXPORTS
#define GCP_MANAGER_API __declspec(dllexport)
#else
#define GCP_MANAGER_API __declspec(dllimport)
#endif

#include "GCPPoint.h"
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>

// ===== 轨道修正参数 =====
struct OrbitCorrection {
    std::vector<double> coeff_along;  // 沿轨方向多项式系数 [a0, a1, a2]
    std::vector<double> coeff_cross;  // 沿距方向多项式系数 [b0, b1, b2]
    std::vector<double> coeff_radial; // 径向方向多项式系数 [c0, c1, c2]
    double rms_residual_range;        // 距离向残差 RMS（米）
    double rms_residual_azimuth;      // 方位向残差 RMS（米）
    int num_gcp_used;                 // 实际使用的 GCP 数量
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

// ===== 轨道精炼主类 =====
class GCP_MANAGER_API OrbitRefinement {
public:
    OrbitRefinement();
    ~OrbitRefinement();

    std::string error_head;

    // ==================== 步骤1: 计算 GCP 处的轨道位置 ====================
    // 根据 GCP 的影像行列号和 SLC 图像参数，反算对应方位时刻的卫星位置与速度矢量
    int compute_satellite_state_at_gcp(
        const std::vector<GCPPoint>& gcps,
        const cv::Mat& state_vec,           // 轨道状态矢量
        double start_gps_time,              // 影像首行 GPS 开始时间
        double prf,                         // 脉冲重复频率
        int offset_row,                     // 行偏移
        int offset_col,                     // 列偏移
        double slant_range_first_pixel,     // 近距斜距
        double range_spacing,               // 距离采样间隔
        std::vector<cv::Point3d>& sat_pos,  // 输出: 卫星 ECEF 位置
        std::vector<cv::Point3d>& sat_vel   // 输出: 卫星 ECEF 速度
    );

    // ==================== 步骤2: 计算轨道残差并投影至卫星本体系 ====================
    // 计算 GCP 地理坐标（WGS84）对应的真实地心坐标，并与卫星观测位置求差，投影到本体系
    int compute_orbit_residuals_local(
        std::vector<GCPPoint>& gcps,                   // 注意：去掉了 const，允许回写计算的残差
        const std::vector<cv::Point3d>& sat_pos,
        const std::vector<cv::Point3d>& sat_vel,
        std::vector<double>& residual_along,           // 输出: 沿轨方向残差（米）
        std::vector<double>& residual_cross,           // 输出: 沿距方向残差（米）
        std::vector<double>& residual_radial           // 输出: 径向方向残差（米）
    );

    // ==================== 步骤3: 多项式拟合轨道修正量 ====================
    // 对各向残差在时间轴上进行多项式最小二乘拟合。如果有效 GCP 不足，自动退化阶数
    int fit_orbit_correction(
        const std::vector<double>& residual_along,
        const std::vector<double>& residual_cross,
        const std::vector<double>& residual_radial,
        const std::vector<double>& normalized_time,    // 归一化时间 [-1, 1]
        int poly_degree,                              // 多项式阶数（1=线性, 2=二次）
        OrbitCorrection& correction                   // 输出: 拟合得到的修正参数
    );

    // ==================== 步骤4: 应用轨道修正 ====================
    // 计算各状态点时刻的本体系修正量，变换回 ECEF 后加到原 state_vec 上
    int apply_orbit_correction(
        cv::Mat& state_vec,                           // 输入/输出: 轨道状态矢量矩阵
        const OrbitCorrection& correction,
        const std::vector<double>& state_normalized_time
    );

    // ==================== 步骤5: 重新计算快速坐标映射系数 ====================
    // 由于轨道状态改变，原 (row, col) 到 (lon, lat) 的近似多项式系数失效，需要重新拟合
    // 方法：在全影像建立 N x N 均匀网格，用修正后轨道进行精细地理定位，再重新用最小二乘拟合多项式系数
    // 内部采用 OpenMP 提高网格迭代解算效率
    int refit_coordinate_coefficients(
        const cv::Mat& state_vec,
        int scene_height,
        int scene_width,
        int offset_row,
        int offset_col,
        double prf,
        double slant_range_first_pixel,
        double range_spacing,
        double carrier_frequency,
        cv::Mat& lon_coeff_new,                       // 输出: 重新拟合的经度系数
        cv::Mat& lat_coeff_new                        // 输出: 重新拟合的纬度系数
    );

    // ==================== 一键轨道精炼（集成核心步骤）====================
    int refine_orbit(
        cv::Mat& state_vec,                           // 输入/输出
        cv::Mat& lon_coefficient,                     // 输入/输出
        cv::Mat& lat_coefficient,                     // 输入/输出
        std::vector<GCPPoint>& gcps,                  // 输入/输出（回写残差）
        double start_gps_time,                        // 影像首行 GPS 开始时间
        int poly_degree,
        double prf,
        int offset_row,
        int offset_col,
        int scene_height,
        int scene_width,
        double slant_range_first_pixel,
        double range_spacing,
        double carrier_frequency,
        OrbitCorrection& correction                   // 输出: 精炼质量报告参数
    );
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
