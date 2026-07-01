#include "internal/diff_boxcount.h"
#include <vector>
#include <cmath>

// 自定义一阶多项式拟合 (对应 np.polyfit(..., 1))
double polyfit_slope(const std::vector<double>& x, const std::vector<double>& y) {
    int n = static_cast<int>(x.size());
    if (n == 0) return 0.0;

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    for (int i = 0; i < n; ++i) {
        sum_x += x[i];
        sum_y += y[i];
        sum_xy += x[i] * y[i];
        sum_xx += x[i] * x[i];
    }

    double denominator = n * sum_xx - sum_x * sum_x;
    if (denominator == 0.0) return 0.0;

    // 返回斜率 slope (即 coeff[0])
    return (n * sum_xy - sum_x * sum_y) / denominator;
}

double extract_diffbox_feature(const cv::Mat& img_gray, SARProgressCallback cb) {
    if (img_gray.empty()) return 0.0;

    // 1. 转为浮点型并计算极差
    cv::Mat img_float;
    img_gray.convertTo(img_float, CV_32F);

    double min_val_f, max_val_f;
    cv::minMaxLoc(img_float, &min_val_f, &max_val_f);
    double delta = max_val_f - min_val_f;
    if (delta == 0.0) return 0.0;

    // 2. 灰度拉伸到 0-255 并转回 uint8
    cv::Mat src = img_float * 255.0 / delta;
    cv::Mat img_;
    src.convertTo(img_, CV_8U);

    // 3. 盒维数核心逻辑
    int M = std::min(img_.rows, img_.cols);

    double min_val, max_gray;
    cv::minMaxLoc(img_, &min_val, &max_gray);

    std::vector<double> NRlist;
    std::vector<double> rlist;

    // np.arange(2, (M // 2) - 1, 2)
    int upper_limit = (M / 2) - 1;
    int iterations = 0;
    int total_iterations = std::max(1, (upper_limit > 2) ? ((upper_limit - 3) / 2 + 1) : 1);
    for (int data = 2; data < upper_limit; data += 2) {
        if (cb) {
            int progress = iterations * 100 / total_iterations;
            if (!cb(progress, "Extracting fractal features...")) {
                return -2.0; // Early return on cancellation (std::vectors will automatically release memory via RAII)
            }
        }
        iterations++;

        double r = (double)data / M;
        double s_dot = std::ceil(max_gray * r);
        int length = M / data;

        long long NR = 0;

        // 遍历图像网格
        for (int w = 0; w < length; ++w) {
            for (int h = 0; h < length; ++h) {
                // 在 OpenCV 中，Rect(x, y, width, height)，x对应列(h)，y对应行(w)
                cv::Rect roi(h * data, w * data, data, data);
                cv::Mat grid = img_(roi);

                double grid_min, grid_max;
                cv::minMaxLoc(grid, &grid_min, &grid_max);

                // (np.max(grid) // s_dot) - (np.min(grid) // s_dot) + 1
                long long nr = static_cast<long long>(std::floor(grid_max / s_dot)) -
                    static_cast<long long>(std::floor(grid_min / s_dot)) + 1;
                NR += nr;
            }
        }
        NRlist.push_back(static_cast<double>(NR));
        rlist.push_back(1.0 / r);
    }

    // 4. 对数域线性拟合 np.polyfit(np.log(rlist), np.log(NRlist), 1)
    std::vector<double> log_rlist(rlist.size()), log_NRlist(rlist.size());
    for (size_t i = 0; i < rlist.size(); ++i) {
        log_rlist[i] = std::log(rlist[i]);
        log_NRlist[i] = std::log(NRlist[i]);
    }

    return polyfit_slope(log_rlist, log_NRlist);
}