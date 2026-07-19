#ifndef DIFF_BOXCOUNT_H
#define DIFF_BOXCOUNT_H

#include <opencv2/opencv.hpp>
#include "global_define.h"

// 提取差分盒维数 (DBC)
int extract_diffbox_feature(const cv::Mat& img_gray, double& feature,
                            IsCancelledCallback is_cancelled = nullptr,
                            void* cancel_context = nullptr,
                            InSARProgressCallback progress = nullptr,
                            void* progress_context = nullptr);

#endif // DIFF_BOXCOUNT_H
