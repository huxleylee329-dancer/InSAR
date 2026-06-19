#ifndef BASIC2_H
#define BASIC2_H

#include <opencv2/opencv.hpp>
#include <vector>

#include "SARProcessor.h"

BasicFeatures extract_basic_features(const cv::Mat& img_gray);

#endif // BASIC2_H