#pragma once
#include "Package.h"
#include "Cancellation.h"

// 定义 SARProcessing 专用的进度回调函数指针类型

// 基础特征结构体（GLCM + FFT）
struct BasicFeatures {
    double fphr;         // 频率峰值比
    double correlation;  // GLCM 相关性
    double contrast;     // GLCM 对比度
    double asm_val;      // GLCM 能量 (ASM)
};

class InSAR_API SARProcessor {
public:
    enum class SpeckleFilterMethod {
        Lee,
        Frost,
        GammaMAP,
        Kuan
    };

    // ============ BM3D 降噪 ============

    // 对单通道灰度图执行 BM3D 降噪（含前后处理：log变换、噪声估计）
    // imgGray: 输入灰度图 (CV_8UC1)
    // sigma8: 噪声标准差（建议值，内部会自动估计）
    // 返回: 降噪后的图像 (CV_8UC1)，失败返回空 Mat
    static int DenoiseGray(const cv::Mat& imgGray, double sigma8, cv::Mat& output,
                           IsCancelledCallback is_cancelled = nullptr,
                           void* cancel_context = nullptr,
                           InSARProgressCallback progress = nullptr,
                           void* progress_context = nullptr);

    // 按 OTB 同类滤波器公式执行空域斑点噪声抑制。
    // radius 为方形邻域半径；numberOfLooks 用于 Lee、GammaMAP、Kuan；
    // frostDeramp 仅用于 Frost 滤波器。
    static cv::Mat DespeckleGray(const cv::Mat& imgGray,
                                 SpeckleFilterMethod method,
                                 int radius,
                                 double numberOfLooks = 1.0,
                                 double frostDeramp = 0.1);

    // ============ 特征提取 ============

    // 提取 GLCM + FFT 基础特征（FPHR、相关性、对比度、能量）
    static BasicFeatures ExtractBasicFeatures(const cv::Mat& imgGray);

    // 提取差分盒维数（DBC）分形特征
    static int ExtractDiffBoxFeature(const cv::Mat& imgGray, double& feature,
                                     IsCancelledCallback is_cancelled = nullptr,
                                     void* cancel_context = nullptr,
                                     InSARProgressCallback progress = nullptr,
                                     void* progress_context = nullptr);

    // ============ 目标检测（ONNX 推理）============

    // 单张图像船舶检测
    // imagePath: 图像文件路径
    // modelPath: ONNX 模型文件路径
    // threshold: 置信度阈值
    // shipProb: [out] 船舶概率
    // resultText: [out] 结果描述缓冲区（调用方分配）
    // resultTextSize: 缓冲区大小（字节）
    // 返回: true=成功, false=失败
    static int DetectShip(const char* imagePath,
                          const char* modelPath,
                          float threshold,
                          float& shipProb,
                          char* resultText,
                          int resultTextSize,
                          IsCancelledCallback is_cancelled = nullptr,
                          void* cancel_context = nullptr,
                          InSARProgressCallback progress = nullptr,
                          void* progress_context = nullptr);

    // 批量船舶检测（同步，复用同一 ONNX Session 以提升性能）
    // imagePaths: 图像路径数组
    // imageCount: 图像数量
    // modelPath: ONNX 模型文件路径
    // threshold: 置信度阈值
    // shipProbs: [out] 各图船舶概率数组（调用方分配，大小 >= imageCount）
    // results: [out] 各图结果描述的连续缓冲区（调用方分配，
    //           总大小 >= imageCount * resultTextSize 字节，
    //           第 i 条结果写入 results + i * resultTextSize 处，
    //           单条长度上限 resultTextSize 字节（含 \0 终止符））
    // resultTextSize: 单条结果文本的缓冲区大小（字节）
    // successFlags: [out] 各图成功标志（调用方分配，大小 >= imageCount）
    // 返回: 成功检测的数量
    static int DetectShipBatch(const char** imagePaths,
                               int imageCount,
                               const char* modelPath,
                               float threshold,
                               float* shipProbs,
                               char* results,
                               int resultTextSize,
                               bool* successFlags,
                               int& successCount,
                               IsCancelledCallback is_cancelled = nullptr,
                               void* cancel_context = nullptr,
                               InSARProgressCallback progress = nullptr,
                               void* progress_context = nullptr);

    // ============ 评价指标 ============

    // 计算等效视数 (ENL = mean^2 / variance)
    static double CalculateENL(const cv::Mat& roiGray);

    // 计算边缘保持指数 (EPI)
    static double CalculateEPI(const cv::Mat& origGray, const cv::Mat& filteredGray);

    // 计算信杂比 (SCR = 20*log10(|target_mean - clutter_mean| / clutter_std))
    static double CalculateSCR(const cv::Mat& targetGray, const cv::Mat& clutterGray);
};
