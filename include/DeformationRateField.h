#pragma once
#ifndef __DEFORMATION_RATE_FIELD_H__
#define __DEFORMATION_RATE_FIELD_H__

#include "Package.h"
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>

// ===== 速率场参数 =====
struct RateFieldParams {
    int    model_type;           // 模型类型：1=线性（仅估计不确定性）, 2=二次多项式
    double confidence_level;     // 置信水平（默认 0.95）
    bool   estimate_uncertainty; // 是否估计不确定性
    bool   compute_acceleration; // 是否计算加速度场（仅二次多项式模型）
};

// ===== 速率场结果（仅含新增计算结果，不含 SBAS 已有字段）=====
struct RateFieldResult {
    cv::Mat velocity_nonlinear; // 非线性速率场（mm/year，可选）
    cv::Mat velocity_std;       // 速率标准差（mm/year）
    cv::Mat velocity_lower;     // 置信区间下界
    cv::Mat velocity_upper;     // 置信区间上界
    cv::Mat acceleration;       // 加速度场（mm/year²，仅二次多项式模型）
    cv::Mat acceleration_std;   // 加速度标准差
    cv::Mat quality_mask;       // 质量掩膜（0=低质量, 1=中等, 2=高质量）
    cv::Mat mask;               // 有效像素掩膜
    double  mean_velocity;      // 平均速率（基于复用/计算的速率场）
    double  std_velocity;       // 全局速率标准差
    int     num_valid_pixels;   // 有效像素数
};

// ===== 速率场分析进度回调函数指针类型 =====
typedef bool (__stdcall *DeformationProgressCallback)(int progress, const char* message);

// ===== 速率场分析主类 =====
class InSAR_API DeformationRateField {
public:
    DeformationRateField();
    ~DeformationRateField();

    std::string error_head;

    // 非线性速率场估算（二次多项式模型）
    int estimate_nonlinear_velocity(
        const cv::Mat& time_series,    // 时间序列矩阵（N_valid × N_times，单位为mm）
        const cv::Mat& temporal,       // 时间基线（1 × N_times，单位为年或者天）
        const cv::Mat& spatial,        // 空间基线（1 × N_times，单位为米）
        const cv::Mat& mask,           // 有效像素掩膜
        const cv::Mat& coherence,      // 相干系数矩阵（N_valid × N_times 或 1 × N_valid）
        const RateFieldParams& params,
        RateFieldResult& result,
        DeformationProgressCallback cb = nullptr
    );

    // 速率不确定性评估（协方差传播法）
    int estimate_uncertainty(
        const cv::Mat& time_series,
        const cv::Mat& temporal,
        const cv::Mat& coherence,
        const cv::Mat& velocity,       // 输入速率场（线性或非线性）
        double         confidence_level,
        cv::Mat&       velocity_std,
        cv::Mat&       velocity_lower,
        cv::Mat&       velocity_upper,
        DeformationProgressCallback cb = nullptr
    );

    // 质量评估（阈值由调用方传入，对应 Node UI 可配置参数）
    int assess_quality(
        const cv::Mat& velocity,
        const cv::Mat& velocity_std,
        const cv::Mat& temporal_coherence,
        double coherence_threshold_high,  // 高质量相干性阈值
        double coherence_threshold_mid,   // 中等质量相干性阈值
        double uncertainty_threshold_high,// 高质量不确定性阈值（mm/year）
        double uncertainty_threshold_mid, // 中等质量不确定性阈值（mm/year）
        cv::Mat& quality_mask,
        double& mean_velocity,
        double& std_velocity,
        int&    num_valid_pixels
    );

    // 统计报告生成
    int generate_statistics_report(
        const RateFieldResult& result,
        std::string& report_text
    );

    // 一键分析接口
    int analyze_rate_field(
        const cv::Mat& time_series,
        const cv::Mat& temporal,
        const cv::Mat& spatial,
        const cv::Mat& mask,
        const cv::Mat& coherence,
        const cv::Mat& velocity_linear,  // 来自 SBAS 的线性速率，用于不确定性评估基准
        const RateFieldParams& params,
        RateFieldResult& result,
        DeformationProgressCallback cb = nullptr
    );
};

#endif // __DEFORMATION_RATE_FIELD_H__
