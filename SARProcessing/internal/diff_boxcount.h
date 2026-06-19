#ifndef DIFF_BOXCOUNT_H
#define DIFF_BOXCOUNT_H

#include <opencv2/opencv.hpp>

// 提取差分盒维数 (DBC)
double extract_diffbox_feature(const cv::Mat& img_gray);

#endif // DIFF_BOXCOUNT_H