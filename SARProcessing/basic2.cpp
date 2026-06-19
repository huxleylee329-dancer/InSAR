#include "internal/basic2.h"
#include <cmath>
#include <iostream>

BasicFeatures extract_basic_features(const cv::Mat& img_gray) {
    BasicFeatures feats = { 0.0, 0.0, 0.0, 0.0 };
    if (img_gray.empty()) return feats;

    // 1. 图像归一化
    double minVal, maxVal;
    cv::minMaxLoc(img_gray, &minVal, &maxVal);
    cv::Mat img_norm;
    if (maxVal != minVal) {
        img_gray.convertTo(img_norm, CV_32F, 1.0 / (maxVal - minVal), -minVal / (maxVal - minVal));
    }
    else {
        img_gray.convertTo(img_norm, CV_32F, 0.0);
        return feats;
    }

    // 2. 频域特征 (peak_high_feature)
    cv::Mat planes[] = { img_norm.clone(), cv::Mat::zeros(img_norm.size(), CV_32F) };
    cv::Mat complexI;
    cv::merge(planes, 2, complexI);
    cv::dft(complexI, complexI, cv::DFT_COMPLEX_OUTPUT);
    cv::split(complexI, planes);
    cv::magnitude(planes[0], planes[1], planes[0]);
    cv::Mat magI = planes[0];

    // 2.2 FFT Shift
    magI = magI(cv::Rect(0, 0, magI.cols & -2, magI.rows & -2));
    int cx = magI.cols / 2;
    int cy = magI.rows / 2;
    cv::Mat q0(magI, cv::Rect(0, 0, cx, cy));
    cv::Mat q1(magI, cv::Rect(cx, 0, cx, cy));
    cv::Mat q2(magI, cv::Rect(0, cy, cx, cy));
    cv::Mat q3(magI, cv::Rect(cx, cy, cx, cy));
    cv::Mat tmp;
    q0.copyTo(tmp); q3.copyTo(q0); tmp.copyTo(q3);
    q1.copyTo(tmp); q2.copyTo(q1); tmp.copyTo(q2);

    // 2.3 对数尺度变换
    magI += cv::Scalar::all(1);
    cv::log(magI, magI);

    // 2.4 计算直方图
    double min_mag, max_mag;
    cv::minMaxLoc(magI, &min_mag, &max_mag);
    int histSize = 50;
    float range[] = { (float)min_mag, (float)max_mag + 1e-5f };
    const float* histRange = { range };
    cv::Mat hist;
    cv::calcHist(&magI, 1, 0, cv::Mat(), hist, 1, &histSize, &histRange, true, false);

    // 密度归一化
    double sum_hist = cv::sum(hist)[0];
    double bin_width = (max_mag - min_mag) / histSize;
    if (sum_hist > 0 && bin_width > 0) {
        hist /= (sum_hist * bin_width);
    }

    // 找出峰值
    double minH, maxH;
    cv::Point minP, maxP;
    cv::minMaxLoc(hist, &minH, &maxH, &minP, &maxP);
    int max_idx = maxP.y;
    double max_bin_val = min_mag + max_idx * bin_width;

    feats.fphr = (max_bin_val != 0.0) ? (maxH / max_bin_val) : 0.0;

    // 3. 纹理特征 (GLCM)
    cv::Mat img_255;
    // 此时的 img_norm 是原图数据
    img_norm.convertTo(img_255, CV_8U, 255.0);

    // 初始化 256x256 的共生矩阵
    cv::Mat glcm = cv::Mat::zeros(256, 256, CV_64F);
    int dx = 5, dy = 0;

    // 填充 GLCM
    for (int y = 0; y < img_255.rows; ++y) {
        for (int x = 0; x < img_255.cols - dx; ++x) {
            int i = img_255.at<uchar>(y, x);
            int j = img_255.at<uchar>(y, x + dx);
            glcm.at<double>(i, j) += 1.0;
            glcm.at<double>(j, i) += 1.0;
        }
    }

    // GLCM 归一化
    double glcm_sum = cv::sum(glcm)[0];
    if (glcm_sum > 0) glcm /= glcm_sum;

    // 4. 从 GLCM 提取 Contrast, Correlation, ASM
    double contrast = 0.0, asm_val = 0.0;
    double mean_i = 0.0, mean_j = 0.0;

    for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 256; ++j) {
            double p = glcm.at<double>(i, j);
            if (p > 0) {
                contrast += p * (i - j) * (i - j);
                asm_val += p * p;
                mean_i += i * p;
                mean_j += j * p;
            }
        }
    }

    double std_i = 0.0, std_j = 0.0;
    for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 256; ++j) {
            double p = glcm.at<double>(i, j);
            if (p > 0) {
                std_i += p * (i - mean_i) * (i - mean_i);
                std_j += p * (j - mean_j) * (j - mean_j);
            }
        }
    }
    std_i = std::sqrt(std_i);
    std_j = std::sqrt(std_j);

    double correlation = 0.0;
    for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 256; ++j) {
            double p = glcm.at<double>(i, j);
            if (p > 0 && std_i > 0 && std_j > 0) {
                correlation += p * (i - mean_i) * (j - mean_j) / (std_i * std_j);
            }
        }
    }

    feats.contrast = contrast;
    feats.asm_val = asm_val;
    feats.correlation = correlation;

    return feats;
}