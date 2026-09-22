#include "internal/basic2.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

// 重构傅里叶图像的象限，使原点位于图像中心 (FFT Shift)
static void fftShift(cv::Mat& magI) {
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
}

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
    fftShift(magI);

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

    double var_i = 0.0, var_j = 0.0;
    double cov = 0.0;
    for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 256; ++j) {
            double p = glcm.at<double>(i, j);
            if (p > 0) {
                double diff_i = i - mean_i;
                double diff_j = j - mean_j;
                var_i += p * diff_i * diff_i;
                var_j += p * diff_j * diff_j;
                cov += p * diff_i * diff_j;
            }
        }
    }
    double std_i = std::sqrt(var_i);
    double std_j = std::sqrt(var_j);

    double correlation = 0.0;
    if (std_i > 0.0 && std_j > 0.0) {
        correlation = cov / (std_i * std_j);
    }

    feats.contrast = contrast;
    feats.asm_val = asm_val;
    feats.correlation = correlation;

    return feats;
}

namespace {

double calculate_high_frequency_energy_ratio(const cv::Mat& img_gray) {
    if (img_gray.empty()) return 0.0;

    cv::Mat gray_float;
    img_gray.convertTo(gray_float, CV_32F);
    gray_float -= cv::mean(gray_float)[0];

    cv::Mat spectrum;
    cv::dft(gray_float, spectrum, cv::DFT_COMPLEX_OUTPUT);
    std::vector<cv::Mat> planes;
    cv::split(spectrum, planes);
    cv::Mat energy = planes[0].mul(planes[0]) + planes[1].mul(planes[1]);

    const double cutoff = std::max(1.0, 0.15 * std::min(energy.rows, energy.cols));
    const double cutoff_squared = cutoff * cutoff;
    double total_energy = 0.0;
    double high_frequency_energy = 0.0;
    for (int y = 0; y < energy.rows; ++y) {
        const int wrapped_y = std::min(y, energy.rows - y);
        const float* row = energy.ptr<float>(y);
        for (int x = 0; x < energy.cols; ++x) {
            const int wrapped_x = std::min(x, energy.cols - x);
            const double value = std::max(0.0, static_cast<double>(row[x]));
            total_energy += value;
            if (static_cast<double>(wrapped_x * wrapped_x + wrapped_y * wrapped_y) >= cutoff_squared)
                high_frequency_energy += value;
        }
    }

    if (total_energy <= 1.0e-12) return 0.0;
    return std::max(0.0, std::min(1.0, high_frequency_energy / total_energy));
}

cv::Mat normalize_to_8u(const cv::Mat& img_gray) {
    double minimum = 0.0;
    double maximum = 0.0;
    cv::minMaxLoc(img_gray, &minimum, &maximum);
    cv::Mat normalized;
    if (maximum <= minimum) {
        normalized = cv::Mat::zeros(img_gray.size(), CV_8U);
        return normalized;
    }
    img_gray.convertTo(normalized, CV_8U, 255.0 / (maximum - minimum),
                       -minimum * 255.0 / (maximum - minimum));
    return normalized;
}

void calculate_bright_target_features(const cv::Mat& img_gray,
                                      double& target_background_contrast,
                                      double& largest_area_ratio,
                                      double& largest_aspect_ratio) {
    target_background_contrast = 0.0;
    largest_area_ratio = 0.0;
    largest_aspect_ratio = 0.0;
    if (img_gray.empty() || img_gray.total() < 16) return;

    const cv::Mat gray8 = normalize_to_8u(img_gray);
    double minimum = 0.0;
    double maximum = 0.0;
    cv::minMaxLoc(gray8, &minimum, &maximum);
    if (maximum <= minimum) return;

    std::vector<unsigned char> pixels;
    pixels.reserve(gray8.total());
    for (int row = 0; row < gray8.rows; ++row) {
        const unsigned char* begin = gray8.ptr<unsigned char>(row);
        pixels.insert(pixels.end(), begin, begin + gray8.cols);
    }
    const size_t percentile_index = static_cast<size_t>(0.95 * (pixels.size() - 1));
    std::nth_element(pixels.begin(), pixels.begin() + percentile_index, pixels.end());
    const int bright_threshold = pixels[percentile_index];

    cv::Mat bright_mask;
    cv::compare(gray8, bright_threshold, bright_mask, cv::CMP_GE);
    cv::morphologyEx(bright_mask, bright_mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int component_count =
        cv::connectedComponentsWithStats(bright_mask, labels, stats, centroids, 8, CV_32S);
    int largest_label = 0;
    int largest_area = 0;
    for (int label = 1; label < component_count; ++label) {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area > largest_area) {
            largest_area = area;
            largest_label = label;
        }
    }
    if (largest_label == 0 || largest_area <= 0) return;

    const int width = stats.at<int>(largest_label, cv::CC_STAT_WIDTH);
    const int height = stats.at<int>(largest_label, cv::CC_STAT_HEIGHT);
    largest_area_ratio = static_cast<double>(largest_area) /
                         static_cast<double>(gray8.total());
    largest_aspect_ratio = static_cast<double>(std::max(width, height)) /
                           static_cast<double>(std::max(1, std::min(width, height)));
    largest_aspect_ratio = std::min(20.0, largest_aspect_ratio);

    cv::Mat target_mask;
    cv::compare(labels, largest_label, target_mask, cv::CMP_EQ);
    const int ring_radius = std::max(3, std::min(gray8.rows, gray8.cols) / 16);
    cv::Mat outer;
    cv::Mat inner;
    cv::dilate(target_mask, outer,
               cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                         cv::Size(2 * ring_radius + 1,
                                                  2 * ring_radius + 1)));
    cv::dilate(target_mask, inner,
               cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    cv::Mat background_mask;
    cv::subtract(outer, inner, background_mask);
    if (cv::countNonZero(background_mask) < 8)
        cv::bitwise_not(target_mask, background_mask);

    cv::Scalar target_mean;
    cv::Scalar target_stddev;
    cv::Scalar background_mean;
    cv::Scalar background_stddev;
    cv::meanStdDev(gray8, target_mean, target_stddev, target_mask);
    cv::meanStdDev(gray8, background_mean, background_stddev, background_mask);
    const double denominator = std::max(background_stddev[0], 1.0e-6);
    target_background_contrast =
        (target_mean[0] - background_mean[0]) / denominator;
    target_background_contrast =
        std::max(-50.0, std::min(50.0, target_background_contrast));
}

} // namespace

ShipFeaturesV2 extract_ship_features_v2(const cv::Mat& img_gray, double diff_box) {
    ShipFeaturesV2 features = {};
    if (img_gray.empty()) return features;

    const BasicFeatures texture = extract_basic_features(img_gray);
    features.high_frequency_energy_ratio =
        calculate_high_frequency_energy_ratio(img_gray);
    features.diff_box = diff_box;
    features.correlation = texture.correlation;
    features.contrast = texture.contrast;
    features.asm_val = texture.asm_val;
    calculate_bright_target_features(
        img_gray,
        features.target_background_contrast,
        features.largest_bright_area_ratio,
        features.largest_bright_aspect_ratio);
    return features;
}
