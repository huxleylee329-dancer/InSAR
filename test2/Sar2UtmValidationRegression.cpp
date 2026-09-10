#include "Sar2UtmValidationRegression.h"

#include <cstdio>
#include <limits>

#include "opencv2\core\core.hpp"
#include "..\include\Utils.h"

int RunSar2UtmValidationRegression()
{
	Utils util;
	cv::Mat mappedLon(8, 8, CV_64F);
	cv::Mat mappedLat(8, 8, CV_64F);
	cv::Mat phase(8, 8, CV_64F, cv::Scalar::all(1.0));
	cv::Mat mappedPhase;

	for (int row = 0; row < mappedLon.rows; ++row)
	{
		for (int column = 0; column < mappedLon.cols; ++column)
		{
			mappedLon.at<double>(row, column) = 100.0 + column * 0.01;
			mappedLat.at<double>(row, column) = 30.0 + row * 0.01;
		}
	}

	if (util.SAR2UTM(mappedLon, mappedLat, phase, mappedPhase, 1) != 0 || mappedPhase.empty())
	{
		std::fprintf(stderr, "SAR2UTM validation regression: valid coordinates were rejected\n");
		return 1;
	}

	mappedLat.at<double>(0, 0) = std::numeric_limits<double>::quiet_NaN();
	if (util.SAR2UTM(mappedLon, mappedLat, phase, mappedPhase, 1) == 0)
	{
		std::fprintf(stderr, "SAR2UTM validation regression: NaN coordinate was accepted\n");
		return 2;
	}

	mappedLat.at<double>(0, 0) = 30.0;
	mappedLon.at<double>(0, 0) = 200.0;
	if (util.SAR2UTM(mappedLon, mappedLat, phase, mappedPhase, 1) == 0)
	{
		std::fprintf(stderr, "SAR2UTM validation regression: out-of-range coordinate was accepted\n");
		return 3;
	}

	mappedLon.at<double>(0, 0) = 100.0;
	if (util.SAR2UTM(mappedLon, mappedLat, phase, mappedPhase, 1e-20, 1) == 0)
	{
		std::fprintf(stderr, "SAR2UTM validation regression: oversized output grid was accepted\n");
		return 4;
	}

	// The old estimator sampled only a fixed middle window.  This grid remains
	// geographically two-dimensional even though latitude is locally flat there.
	const int plateauSize = 130;
	mappedLon.create(plateauSize, plateauSize, CV_64F);
	mappedLat.create(plateauSize, plateauSize, CV_64F);
	phase = cv::Mat(plateauSize, plateauSize, CV_64F, cv::Scalar::all(1.0));
	for (int row = 0; row < plateauSize; ++row)
	{
		for (int column = 0; column < plateauSize; ++column)
		{
			mappedLon.at<double>(row, column) = 100.0 + column * 0.01;
			mappedLat.at<double>(row, column) = 30.0 + (row >= 32 && row <= 64 ? 32 : row) * 0.01;
		}
	}
	if (util.SAR2UTM(mappedLon, mappedLat, phase, mappedPhase, 1) != 0 || mappedPhase.empty())
	{
		std::fprintf(stderr, "SAR2UTM validation regression: a locally flat coordinate region was rejected\n");
		return 5;
	}

	mappedLon = 100.0;
	mappedLat = 30.0;
	if (util.SAR2UTM(mappedLon, mappedLat, phase, mappedPhase, 1) == 0)
	{
		std::fprintf(stderr, "SAR2UTM validation regression: a degenerate coordinate grid was accepted\n");
		return 6;
	}

	std::fprintf(stdout, "SAR2UTM validation regression: PASS\n");
	return 0;
}
