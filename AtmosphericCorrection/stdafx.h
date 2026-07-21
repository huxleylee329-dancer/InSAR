#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Standard C++ headers
#include <vector>
#include <string>
#include <cmath>
#include <iostream>
#include <fstream>
#include <regex>
#include <complex>
#include <algorithm>

// OpenCV headers
#include <opencv2/opencv.hpp>

// GDAL headers
#include <gdal_priv.h>
#include <cpl_conv.h>

// Eigen headers
#include <Eigen/Dense>
