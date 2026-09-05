#include "SARProcessor.h"
#include "internal/bm3d.h"
#include "internal/bm3d_wiener.h"
#include "internal/basic2.h"
#include "internal/diff_boxcount.h"

#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <limits>

// MultiByteToWideChar 需要 windows.h（x64 配置未启用预编译头）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 链接 ONNX Runtime 库
#pragma comment(lib, "onnxruntime.lib")

// ============ 内部辅助函数 ============

#ifdef _WIN32
// 将 UTF-8 编码的 std::string 转换为宽字符 std::wstring
static std::wstring toWideString(const std::string& str)
{
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) return L"";
    std::wstring wstr(wideLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], wideLen);
    if (!wstr.empty() && wstr.back() == L'\0') {
        wstr.pop_back();
    }
    return wstr;
}
#endif

// 计算中值（用于噪声估计）
static double calcMedian(const cv::Mat& img)
{
    cv::Mat imgCopy = img.clone();
    imgCopy = imgCopy.reshape(0, 1);
    std::sort(imgCopy.begin<double>(), imgCopy.end<double>());
    int n = static_cast<int>(imgCopy.total());
    if (n % 2 == 0 && n > 0) {
        return (imgCopy.at<double>(n / 2 - 1) + imgCopy.at<double>(n / 2)) / 2.0;
    } else if (n > 0) {
        return imgCopy.at<double>(n / 2);
    }
    return 0.0;
}

static int bm3dCoreDenoise(const cv::Mat& img8U, double sigma8, cv::Mat& clean,
                           IsCancelledCallback is_cancelled, void* cancel_context,
                           InSARProgressCallback progress_callback, void* progress_context)
{
    if (img8U.empty() || img8U.type() != CV_8UC1) {
        return -1;
    }

    int width = img8U.cols;
    int height = img8U.rows;

    cv::Mat noisy = img8U.clone();
    cv::Mat basic(height, width, CV_8UC1);
    clean.create(height, width, CV_8UC1);

    int sigma = cvRound(sigma8);
    if (sigma < 1) sigma = 1;

    // 第一阶段：硬阈值
    BM3D bm3d(width, height, 16, 8, 3, 16, 1, 16, 1);
    bm3d.load(noisy.ptr<ImageType>(), sigma, 2500);
    int ret = bm3d.run(basic.ptr<ImageType>(), is_cancelled, cancel_context,
                       progress_callback, progress_context);
    if (ret != 0) return ret;

    // 第二阶段：Wiener 滤波
    BM3D_WIE bm3d_wie(width, height, 32, 8, 3, 16, 1, 16, 1);
    bm3d_wie.load(noisy.ptr<ImageType>(), basic.ptr<ImageType>(), sigma, 400);
    ret = bm3d_wie.run(clean.ptr<ImageType>(), is_cancelled, cancel_context,
                       progress_callback, progress_context);
    if (ret != 0) return ret;

    return 0;
}

// ONNX 单次推理内部实现（复用已有的 Env 和 Session）
static int detectShipInternal(Ort::Session& session,
                               const cv::Mat& img,
                               float threshold,
                               float& shipProb,
                               char* resultText,
                               int resultTextSize,
                               IsCancelledCallback is_cancelled,
                               void* cancel_context,
                               InSARProgressCallback progress_callback,
                               void* progress_context)
{
    if (is_cancelled && is_cancelled(cancel_context)) return -2;
    BasicFeatures feats = extract_basic_features(img);
    if (is_cancelled && is_cancelled(cancel_context)) return -2;
    double difbox = 0.0;
    int ret = extract_diffbox_feature(img, difbox, is_cancelled, cancel_context,
                                      progress_callback, progress_context);
    if (ret != 0) return ret;

    std::vector<float> inputTensorValues = {
        static_cast<float>(feats.fphr),
        static_cast<float>(difbox),
        static_cast<float>(feats.correlation),
        static_cast<float>(feats.contrast),
        static_cast<float>(feats.asm_val)
    };

    const char* inputNames[] = { "float_input" };
    const char* outputNames[] = { "label", "probabilities" };

    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> inputShape = { 1, 5 };

    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memoryInfo,
        inputTensorValues.data(),
        inputTensorValues.size(),
        inputShape.data(),
        inputShape.size()
    );

    auto outputTensors = session.Run(
        Ort::RunOptions{ nullptr },
        inputNames,
        &inputTensor,
        1,
        outputNames,
        2
    );

    if (is_cancelled && is_cancelled(cancel_context)) return -2;

    auto type_info = outputTensors[1].GetTensorTypeAndShapeInfo();
    size_t elem_count = type_info.GetElementCount();
    float* probArr = outputTensors[1].GetTensorMutableData<float>();

    if (elem_count >= 2) {
        shipProb = probArr[1];
    } else if (elem_count == 1) {
        shipProb = probArr[0];
    } else {
        shipProb = 0.0f;
    }

    const char* label = shipProb >= threshold ? "Ship" : "Sea";
    std::strncpy(resultText, label, resultTextSize - 1);
    resultText[resultTextSize - 1] = '\0';
    return 0;
}

// ============ SARProcessor 公开 API 实现 ============

// BM3D 降噪（含前后处理：log 变换、噪声估计、逆 log、亮度保持）
int SARProcessor::DenoiseGray(const cv::Mat& imgGray, double sigma8, cv::Mat& output,
                               IsCancelledCallback is_cancelled, void* cancel_context,
                               InSARProgressCallback progress_callback, void* progress_context)
{
    output.release();
    if (imgGray.empty()) {
        return -1;
    }
    if (is_cancelled && is_cancelled(cancel_context)) return -2;

    cv::Mat inputGray;
    if (imgGray.type() != CV_8UC1) {
        // 如果不是 8bit 灰度，先尝试转换
        if (imgGray.channels() == 3) {
            cv::cvtColor(imgGray, inputGray, cv::COLOR_BGR2GRAY);
        } else {
            imgGray.convertTo(inputGray, CV_8U);
        }
    } else {
        inputGray = imgGray;
    }

    const double noiseGain = 1.1;

    // 转换到 log 域
    cv::Mat imgDouble;
    inputGray.convertTo(imgDouble, CV_64F);
    cv::Mat imgLog;
    cv::log(imgDouble + 1.0, imgLog);

    // 归一化到 [0, 1]
    double minV = 0.0, maxV = 0.0;
    cv::minMaxLoc(imgLog, &minV, &maxV);
    double rangeV = maxV - minV;
    if (rangeV <= 0.0) rangeV = 1.0;
    cv::Mat imgNorm = (imgLog - minV) / rangeV;

    // 噪声估计（MAD 方法）
    double medianValue = calcMedian(imgLog);
    cv::Mat absDiff;
    cv::absdiff(imgLog, medianValue, absDiff);
    double sigmaEst = calcMedian(absDiff) / 0.6745;
    double sigmaFinal = (sigmaEst * noiseGain) / rangeV;

    // 转换为 8bit 并执行 BM3D
    cv::Mat img8U;
    imgNorm.convertTo(img8U, CV_8U, 255.0);
    double sigma8Final = sigmaFinal * 255.0;
    cv::Mat den8U;
    int ret = bm3dCoreDenoise(img8U, sigma8Final, den8U, is_cancelled,
                              cancel_context, progress_callback, progress_context);

    if (ret != 0) return ret;
    if (is_cancelled && is_cancelled(cancel_context)) return -2;

    // 后处理：逆 log 变换 + 亮度保持
    cv::Mat denNorm;
    den8U.convertTo(denNorm, CV_64F, 1.0 / 255.0);
    cv::Mat imgDen = denNorm * rangeV + minV;
    cv::Mat imgOut;
    cv::exp(imgDen, imgOut);
    imgOut = imgOut - 1.0;

    double meanInput = cv::mean(imgDouble)[0];
    double meanOutput = cv::mean(imgOut)[0];
    if (meanOutput != 0.0) {
        imgOut = imgOut * (meanInput / meanOutput);
    }

    cv::min(imgOut, 255.0, imgOut);
    cv::max(imgOut, 0.0, imgOut);

    cv::Mat output8U;
    imgOut.convertTo(output8U, CV_8U);
    if (is_cancelled && is_cancelled(cancel_context)) return -2;
    output = output8U;
    return 0;
}

// Lee、Frost、GammaMAP、Kuan 空域斑点噪声滤波。
cv::Mat SARProcessor::DespeckleGray(const cv::Mat& imgGray,
                                    SpeckleFilterMethod method,
                                    int radius,
                                    double numberOfLooks,
                                    double frostDeramp)
{
    if (imgGray.empty()) {
        return cv::Mat();
    }

    cv::Mat inputGray;
    if (imgGray.channels() == 3) {
        cv::cvtColor(imgGray, inputGray, cv::COLOR_BGR2GRAY);
    } else if (imgGray.channels() == 4) {
        cv::cvtColor(imgGray, inputGray, cv::COLOR_BGRA2GRAY);
    } else if (imgGray.channels() == 1) {
        imgGray.convertTo(inputGray, CV_8U);
    } else {
        return cv::Mat();
    }

    radius = std::max(1, radius);
    numberOfLooks = std::max(numberOfLooks, 1.0e-6);
    frostDeramp = std::max(frostDeramp, 0.0);

    cv::Mat source;
    inputGray.convertTo(source, CV_64F);

    const int windowSize = radius * 2 + 1;
    const int sampleCount = windowSize * windowSize;
    const cv::Size kernelSize(windowSize, windowSize);

    cv::Mat localMean;
    cv::Mat localSquareMean;
    cv::boxFilter(source, localMean, CV_64F, kernelSize,
                  cv::Point(-1, -1), true, cv::BORDER_REPLICATE);
    cv::boxFilter(source.mul(source), localSquareMean, CV_64F, kernelSize,
                  cv::Point(-1, -1), true, cv::BORDER_REPLICATE);

    cv::Mat localVariance = localSquareMean - localMean.mul(localMean);
    cv::max(localVariance, 0.0, localVariance);
    if (sampleCount > 1) {
        localVariance *= static_cast<double>(sampleCount) /
                         static_cast<double>(sampleCount - 1);
    }

    cv::Mat output(source.size(), CV_64F, cv::Scalar(0));
    const double cu2 = 1.0 / numberOfLooks;
    const double cu = std::sqrt(cu2);
    const double cmax = std::sqrt(2.0) * cu;
    const double epsilon = 1.0e-12;

    if (method == SpeckleFilterMethod::Frost) {
        cv::Mat padded;
        cv::copyMakeBorder(source, padded, radius, radius, radius, radius,
                           cv::BORDER_REPLICATE);

#pragma omp parallel for
        for (int row = 0; row < source.rows; ++row) {
            double* outputRow = output.ptr<double>(row);
            const double* meanRow = localMean.ptr<double>(row);
            const double* varianceRow = localVariance.ptr<double>(row);

            for (int col = 0; col < source.cols; ++col) {
                const double mean = meanRow[col];
                if (std::abs(mean) <= epsilon) {
                    outputRow[col] = 0.0;
                    continue;
                }

                const double cs2 = varianceRow[col] / (mean * mean);
                const double alpha = frostDeramp * cs2;
                if (alpha <= epsilon) {
                    outputRow[col] = mean;
                    continue;
                }

                double weightedSum = 0.0;
                double weightSum = 0.0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    const double* paddedRow = padded.ptr<double>(row + radius + dy);
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const double distance = std::sqrt(static_cast<double>(dx * dx + dy * dy));
                        const double weight = std::exp(-alpha * distance);
                        weightedSum += weight * paddedRow[col + radius + dx];
                        weightSum += weight;
                    }
                }
                outputRow[col] = weightSum > epsilon ? weightedSum / weightSum : mean;
            }
        }
    } else {
#pragma omp parallel for
        for (int row = 0; row < source.rows; ++row) {
            const double* sourceRow = source.ptr<double>(row);
            const double* meanRow = localMean.ptr<double>(row);
            const double* varianceRow = localVariance.ptr<double>(row);
            double* outputRow = output.ptr<double>(row);

            for (int col = 0; col < source.cols; ++col) {
                const double value = sourceRow[col];
                const double mean = meanRow[col];
                const double variance = varianceRow[col];

                if (std::abs(mean) <= epsilon) {
                    outputRow[col] = 0.0;
                    continue;
                }
                if (variance <= epsilon) {
                    outputRow[col] = mean;
                    continue;
                }

                const double ci2 = variance / (mean * mean);
                if (ci2 <= cu2) {
                    outputRow[col] = mean;
                    continue;
                }

                if (method == SpeckleFilterMethod::GammaMAP) {
                    const double ci = std::sqrt(ci2);
                    if (ci >= cmax) {
                        outputRow[col] = value;
                        continue;
                    }

                    const double alpha = (1.0 + cu2) / (ci2 - cu2);
                    const double b = alpha - numberOfLooks - 1.0;
                    const double discriminant = std::max(
                        0.0,
                        mean * mean * b * b +
                        4.0 * alpha * numberOfLooks * mean * value);
                    outputRow[col] = (b * mean + std::sqrt(discriminant)) /
                                     (2.0 * alpha);
                    continue;
                }

                double weight = 1.0 - cu2 / ci2;
                if (method == SpeckleFilterMethod::Kuan) {
                    weight /= 1.0 + cu2;
                }
                weight = std::clamp(weight, 0.0, 1.0);
                outputRow[col] = mean + weight * (value - mean);
            }
        }
    }

    cv::min(output, 255.0, output);
    cv::max(output, 0.0, output);
    cv::Mat output8U;
    output.convertTo(output8U, CV_8U);
    return output8U;
}

// 特征提取：GLCM + FFT
BasicFeatures SARProcessor::ExtractBasicFeatures(const cv::Mat& imgGray)
{
    return extract_basic_features(imgGray);
}

// 特征提取：差分盒维数
int SARProcessor::ExtractDiffBoxFeature(const cv::Mat& imgGray, double& feature,
                                         IsCancelledCallback is_cancelled, void* cancel_context,
                                         InSARProgressCallback progress_callback, void* progress_context)
{
    return extract_diffbox_feature(imgGray, feature, is_cancelled, cancel_context,
                                   progress_callback, progress_context);
}

// 单张图像船舶检测
int SARProcessor::DetectShip(const char* imagePath,
                               const char* modelPath,
                               float threshold,
                               float& shipProb,
                               char* resultText,
                               int resultTextSize,
                               IsCancelledCallback is_cancelled,
                               void* cancel_context,
                               InSARProgressCallback progress_callback,
                               void* progress_context)
{
    if (is_cancelled && is_cancelled(cancel_context)) return -2;
    if (!imagePath || !modelPath || !resultText || resultTextSize <= 0) {
        return -1;
    }

    cv::Mat img = cv::imread(imagePath, cv::IMREAD_GRAYSCALE | cv::IMREAD_ANYDEPTH);
    if (img.empty()) {
        std::strncpy(resultText, "Failed to read image.", resultTextSize - 1);
        resultText[resultTextSize - 1] = '\0';
        return -1;
    }

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ShipDetection");
        Ort::SessionOptions sessionOptions;

        // 将路径转换为 native 分隔符
        std::string modelPathStr(modelPath);
#ifdef _WIN32
        std::wstring modelPathW = toWideString(modelPathStr);
        Ort::Session session(env, modelPathW.c_str(), sessionOptions);
#else
        Ort::Session session(env, modelPathStr.c_str(), sessionOptions);
#endif

        if (is_cancelled && is_cancelled(cancel_context)) return -2;
        if (progress_callback) progress_callback(progress_context, 0, "Detecting ship...");
        int ret = detectShipInternal(session, img, threshold, shipProb, resultText,
                                     resultTextSize, is_cancelled, cancel_context,
                                     progress_callback, progress_context);
        if (is_cancelled && is_cancelled(cancel_context)) return -2;
        if (progress_callback) progress_callback(progress_context, 100, "Detecting ship...");
        return ret;
    }
    catch (const Ort::Exception& e) {
        std::snprintf(resultText, resultTextSize, "ONNX Runtime Error: %s", e.what());
        return -1;
    }
    catch (const std::exception& e) {
        std::snprintf(resultText, resultTextSize, "Detection Error: %s", e.what());
        return -1;
    }
}

// 批量船舶检测（复用同一 ONNX Session）
int SARProcessor::DetectShipBatch(const char** imagePaths,
                                   int imageCount,
                                   const char* modelPath,
                                   float threshold,
                                   float* shipProbs,
                                   char* results,
                                   int resultTextSize,
                                   bool* successFlags,
                                   int& successCount,
                                   IsCancelledCallback is_cancelled,
                                   void* cancel_context,
                                   InSARProgressCallback progress_callback,
                                   void* progress_context)
{
    successCount = 0;
    if (!imagePaths || imageCount <= 0 || !modelPath || !shipProbs || !results || !successFlags) {
        return -1;
    }

    // 创建一次 Env 和 Session，循环内复用
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "BatchShipDetection");
    Ort::SessionOptions sessionOptions;

    std::string modelPathStr(modelPath);
#ifdef _WIN32
    std::wstring modelPathW = toWideString(modelPathStr);
    Ort::Session session(env, modelPathW.c_str(), sessionOptions);
#else
    Ort::Session session(env, modelPathStr.c_str(), sessionOptions);
#endif

    for (int i = 0; i < imageCount; ++i) {
        if (is_cancelled && is_cancelled(cancel_context)) return -2;
        if (progress_callback) progress_callback(progress_context,
            i * 100 / imageCount, "Detecting ships...");
        char* resultPtr = results + i * resultTextSize;
        shipProbs[i] = 0.0f;
        successFlags[i] = false;
        resultPtr[0] = '\0';

        cv::Mat img = cv::imread(imagePaths[i], cv::IMREAD_GRAYSCALE | cv::IMREAD_ANYDEPTH);
        if (img.empty()) {
            std::strncpy(resultPtr, "Failed to read image.", resultTextSize - 1);
            resultPtr[resultTextSize - 1] = '\0';
            continue;
        }

        try {
            int ret = detectShipInternal(session, img, threshold, shipProbs[i], resultPtr,
                                         resultTextSize, is_cancelled, cancel_context,
                                         progress_callback, progress_context);
            if (ret == -2) return -2;
            if (ret == 0) {
                successFlags[i] = true;
                successCount++;
            }
        }
        catch (const std::exception& e) {
            std::snprintf(resultPtr, resultTextSize, "Error: %s", e.what());
        }
    }

    if (is_cancelled && is_cancelled(cancel_context)) return -2;
    if (progress_callback) progress_callback(progress_context, 100, "Detecting ships...");
    return 0;
}

// 计算等效视数 (ENL)
double SARProcessor::CalculateENL(const cv::Mat& roiGray)
{
    if (roiGray.empty()) return 0.0;

    cv::Mat meanMat, stddevMat;
    cv::meanStdDev(roiGray, meanMat, stddevMat);

    double mean = meanMat.at<double>(0, 0);
    double stddev = stddevMat.at<double>(0, 0);

    if (stddev <= 1.0e-12) return std::numeric_limits<double>::infinity();

    return (mean * mean) / (stddev * stddev);
}

// 计算边缘保持指数 (EPI)
double SARProcessor::CalculateEPI(const cv::Mat& origGray, const cv::Mat& filteredGray)
{
    if (origGray.empty() || filteredGray.empty()) return 0.0;

    cv::Mat lapOrig, lapFilt;
    cv::Laplacian(origGray, lapOrig, CV_64F);
    cv::Laplacian(filteredGray, lapFilt, CV_64F);

    double sumOrig = cv::sum(cv::abs(lapOrig))[0];
    double sumFilt = cv::sum(cv::abs(lapFilt))[0];

    if (sumOrig == 0) return 0.0;
    return sumFilt / sumOrig;
}

// 计算信杂比 (SCR)
double SARProcessor::CalculateSCR(const cv::Mat& targetGray, const cv::Mat& clutterGray)
{
    if (targetGray.empty() || clutterGray.empty()) return 0.0;

    cv::Mat targetDouble, clutterDouble;
    targetGray.convertTo(targetDouble, CV_64F);
    clutterGray.convertTo(clutterDouble, CV_64F);

    cv::Scalar targetMeanValue, targetStdValue;
    cv::Scalar clutterMeanValue, clutterStdValue;

    cv::meanStdDev(targetDouble, targetMeanValue, targetStdValue);
    cv::meanStdDev(clutterDouble, clutterMeanValue, clutterStdValue);

    double targetMean = targetMeanValue[0];
    double clutterMean = clutterMeanValue[0];
    double clutterStd = clutterStdValue[0];

    if (clutterStd <= 1e-12) return 0.0;

    double numerator = std::abs(targetMean - clutterMean);
    if (numerator <= 1e-12) return 0.0;

    return 20.0 * std::log10(numerator / clutterStd);
}
