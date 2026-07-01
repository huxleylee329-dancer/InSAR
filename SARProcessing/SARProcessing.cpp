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

static cv::Mat bm3dCoreDenoise(const cv::Mat& img8U, double sigma8, SARProgressCallback cb)
{
    if (img8U.empty() || img8U.type() != CV_8UC1) {
        return cv::Mat();
    }

    int width = img8U.cols;
    int height = img8U.rows;

    cv::Mat noisy = img8U.clone();
    cv::Mat basic(height, width, CV_8UC1);
    cv::Mat clean(height, width, CV_8UC1);

    int sigma = cvRound(sigma8);
    if (sigma < 1) sigma = 1;

    // 第一阶段：硬阈值
    BM3D bm3d(width, height, 16, 8, 3, 16, 1, 16, 1);
    bm3d.load(noisy.ptr<ImageType>(), sigma, 2500);
    if (!bm3d.run(basic.ptr<ImageType>(), cb)) {
        return cv::Mat(); // User canceled
    }

    // 第二阶段：Wiener 滤波
    BM3D_WIE bm3d_wie(width, height, 32, 8, 3, 16, 1, 16, 1);
    bm3d_wie.load(noisy.ptr<ImageType>(), basic.ptr<ImageType>(), sigma, 400);
    if (!bm3d_wie.run(clean.ptr<ImageType>(), cb)) {
        return cv::Mat(); // User canceled
    }

    return clean;
}

// ONNX 单次推理内部实现（复用已有的 Env 和 Session）
static bool detectShipInternal(Ort::Session& session,
                               const cv::Mat& img,
                               float threshold,
                               float& shipProb,
                               char* resultText,
                               int resultTextSize)
{
    BasicFeatures feats = extract_basic_features(img);
    double difbox = extract_diffbox_feature(img);

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
    return true;
}

// ============ SARProcessor 公开 API 实现 ============

// BM3D 降噪（含前后处理：log 变换、噪声估计、逆 log、亮度保持）
cv::Mat SARProcessor::DenoiseGray(const cv::Mat& imgGray, double sigma8, SARProgressCallback cb)
{
    if (imgGray.empty()) {
        return cv::Mat();
    }

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
    cv::Mat den8U = bm3dCoreDenoise(img8U, sigma8Final, cb);

    if (den8U.empty()) {
        return cv::Mat();
    }

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
    return output8U;
}

// 特征提取：GLCM + FFT
BasicFeatures SARProcessor::ExtractBasicFeatures(const cv::Mat& imgGray)
{
    return extract_basic_features(imgGray);
}

// 特征提取：差分盒维数
double SARProcessor::ExtractDiffBoxFeature(const cv::Mat& imgGray, SARProgressCallback cb)
{
    return extract_diffbox_feature(imgGray, cb);
}

// 单张图像船舶检测
bool SARProcessor::DetectShip(const char* imagePath,
                               const char* modelPath,
                               float threshold,
                               float& shipProb,
                               char* resultText,
                               int resultTextSize)
{
    if (!imagePath || !modelPath || !resultText || resultTextSize <= 0) {
        return false;
    }

    cv::Mat img = cv::imread(imagePath, cv::IMREAD_GRAYSCALE | cv::IMREAD_ANYDEPTH);
    if (img.empty()) {
        std::strncpy(resultText, "Failed to read image.", resultTextSize - 1);
        resultText[resultTextSize - 1] = '\0';
        return false;
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

        return detectShipInternal(session, img, threshold, shipProb, resultText, resultTextSize);
    }
    catch (const Ort::Exception& e) {
        std::snprintf(resultText, resultTextSize, "ONNX Runtime Error: %s", e.what());
        return false;
    }
    catch (const std::exception& e) {
        std::snprintf(resultText, resultTextSize, "Detection Error: %s", e.what());
        return false;
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
                                   SARProgressCallback cb)
{
    if (!imagePaths || imageCount <= 0 || !modelPath || !shipProbs || !results || !successFlags) {
        return 0;
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

    int successCount = 0;
    for (int i = 0; i < imageCount; ++i) {
        if (cb && !cb(i * 100 / imageCount, "Detecting ships...")) {
            return successCount;
        }
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
            if (detectShipInternal(session, img, threshold, shipProbs[i], resultPtr, resultTextSize)) {
                successFlags[i] = true;
                successCount++;
            }
        }
        catch (const std::exception& e) {
            std::snprintf(resultPtr, resultTextSize, "Error: %s", e.what());
        }
    }

    return successCount;
}

// 计算等效视数 (ENL)
double SARProcessor::CalculateENL(const cv::Mat& roiGray)
{
    if (roiGray.empty()) return 0.0;

    cv::Mat meanMat, stddevMat;
    cv::meanStdDev(roiGray, meanMat, stddevMat);

    double mean = meanMat.at<double>(0, 0);
    double stddev = stddevMat.at<double>(0, 0);

    if (stddev == 0) return 0.0;

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
