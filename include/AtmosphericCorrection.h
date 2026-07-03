#pragma once

#ifdef ATMOSPHERIC_CORRECTION_EXPORTS
#define ATMOS_API __declspec(dllexport)
#else
#define ATMOS_API __declspec(dllimport)
#endif

#include <opencv2/opencv.hpp>

// 1. 电离层校正参数
struct IonosphericParams {
    double subbandRatio = 0.25;   // 子频带比例
    double filterStrength = 1.0;  // 滤波强度 (Gaussian Kernel 缩放系数)
};

// 2. 相位高程多项式回归参数
struct RegressionParams {
    int polyOrder = 1;            // 多项式阶数 (1-2)
    int windowSize = 31;          // 过滤滑动窗口大小 (0 为全局回归)
    double coherenceThresh = 0.3; // 相干系数阈值
};

// 进度回调函数指针类型
typedef bool (__stdcall *AtmosProgressCallback)(int progress, const char* message);

extern "C" {

/**
 * @brief 对流层 ERA5 延迟校正
 * @param phase 输入的缠绕相位矩阵 (CV_32FC1)
 * @param latMat 纬度矩阵 (CV_32FC1)
 * @param lonMat 经度矩阵 (CV_32FC1)
 * @param demMat DEM 矩阵 (CV_32FC1)
 * @param hasDem 是否使用 DEM 参与校正
 * @param masterNcPath 主图像日期 ERA5 NetCDF 文件的绝对路径 (C 风格字符串)
 * @param slaveNcPath 从图像日期 ERA5 NetCDF 文件的绝对路径 (C 风格字符串)
 * @param wavelength 雷达波长 (米)
 * @param correctedPhase 输出校正后的相位矩阵 (CV_32FC1) (要求调用者预先分配好大小和类型)
 * @param errBuf 错误信息缓冲区
 * @param bufSize 缓冲区大小
 * @param cb 进度回调函数指针
 * @return 是否计算成功
 */
ATMOS_API bool computeTroposphericCorrection(
    const cv::Mat& phase,
    const cv::Mat& latMat,
    const cv::Mat& lonMat,
    const cv::Mat& demMat,
    bool hasDem,
    const char* masterNcPath,
    const char* slaveNcPath,
    double wavelength,
    cv::Mat& correctedPhase,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb = nullptr
);

/**
 * @brief 电离层双频分裂校正
 * @param masterComplex 主 SLC 图像矩阵 (CV_32FC2)
 * @param slaveComplex 从 SLC 图像矩阵 (CV_32FC2)
 * @param params 电离层校正参数
 * @param correctedSlaveComplex 输出校正后的从图像矩阵 (CV_32FC2) (要求调用者预先分配好大小和类型)
 * @param errBuf 错误信息缓冲区
 * @param bufSize 缓冲区大小
 * @param cb 进度回调函数指针
 * @return 是否计算成功
 */
ATMOS_API bool computeIonosphericCorrection(
    const cv::Mat& masterComplex,
    const cv::Mat& slaveComplex,
    const IonosphericParams& params,
    cv::Mat& correctedSlaveComplex,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb = nullptr
);

/**
 * @brief 相位-高程多项式回归拟合及轨道误差消除
 * @param phase 缠绕相位矩阵 (CV_32FC1)
 * @param coherence 相干系数矩阵 (CV_32FC1)
 * @param hasCoherence 是否使用相干系数进行掩膜过滤
 * @param dem DEM 高程矩阵 (CV_32FC1)
 * @param hasDem 是否使用 DEM 参与回归
 * @param latMat 纬度矩阵 (CV_32FC1)
 * @param lonMat 经度矩阵 (CV_32FC1)
 * @param hasLatLon 是否使用经纬度坐标进行轨道趋势面拟合
 * @param params 回归参数
 * @param correctedPhase 输出去趋势后的相位矩阵 (CV_32FC1) (要求调用者预先分配好大小和类型)
 * @param errBuf 错误信息缓冲区
 * @param bufSize 缓冲区大小
 * @param cb 进度回调函数指针
 * @return 是否计算成功
 */
ATMOS_API bool computePhaseElevationRegression(
    const cv::Mat& phase,
    const cv::Mat& coherence,
    bool hasCoherence,
    const cv::Mat& dem,
    bool hasDem,
    const cv::Mat& latMat,
    const cv::Mat& lonMat,
    bool hasLatLon,
    const RegressionParams& params,
    cv::Mat& correctedPhase,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb = nullptr
);

/**
 * @brief 应用 GACOS 延迟文件完成插值和相位校正
 * @param phase 缠绕相位矩阵 (CV_32FC1)
 * @param latMat 纬度矩阵 (CV_32FC1)
 * @param lonMat 经度矩阵 (CV_32FC1)
 * @param localGacosFilePath 本地 GACOS 改正结果文件的绝对路径 (C 风格字符串)
 * @param dataFormat 数据格式 (0: GeoTIFF, 1: Binary)
 * @param wavelength 雷达波长 (米)
 * @param correctedPhase 输出校正后的相位矩阵 (CV_32FC1) (要求调用者预先分配好大小和类型)
 * @param errBuf 错误信息缓冲区
 * @param bufSize 缓冲区大小
 * @param cb 进度回调函数指针
 * @return 是否计算成功
 */
ATMOS_API bool applyGacosCorrection(
    const cv::Mat& phase,
    const cv::Mat& latMat,
    const cv::Mat& lonMat,
    const char* localGacosFilePath,
    int dataFormat,
    double wavelength,
    cv::Mat& correctedPhase,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb = nullptr
);

}
