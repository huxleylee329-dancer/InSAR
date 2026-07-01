#ifndef DIFF_BOXCOUNT_H
#define DIFF_BOXCOUNT_H

#include <opencv2/opencv.hpp>
#include "global_define.h"

// 提取差分盒维数 (DBC)
double extract_diffbox_feature(const cv::Mat& img_gray, SARProgressCallback cb = nullptr);

#endif // DIFF_BOXCOUNT_H