#include "RegistrationSubpixelRegression.h"

#include "..\include\ComplexMat.h"
#include "..\include\Registration.h"

#include <cmath>
#include <cstdio>

#include "opencv2\core\core.hpp"
#include "opencv2\imgproc\imgproc.hpp"

namespace
{
const int kRows = 256;
const int kCols = 256;
const double kTolerancePixels = 0.05;

ComplexMat CreateMaster()
{
	ComplexMat master(kRows, kCols);
	cv::RNG random(0x7a2c1e5dU);
	random.fill(master.re, cv::RNG::UNIFORM, 0.0, 1.0);

	// A smoothed, non-periodic amplitude texture makes the subpixel peak stable.
	cv::GaussianBlur(master.re, master.re, cv::Size(), 1.5, 1.5, cv::BORDER_REFLECT101);
	cv::normalize(master.re, master.re, 20.0, 120.0, cv::NORM_MINMAX);
	master.im = cv::Mat::zeros(kRows, kCols, CV_64F);
	return master;
}

ComplexMat Translate(const ComplexMat& master, double dy, double dx)
{
	ComplexMat slave(kRows, kCols);
	const cv::Mat affine = (cv::Mat_<double>(2, 3) << 1.0, 0.0, dx, 0.0, 1.0, dy);

	// slave(r, c) = master(r - dy, c - dx)
	cv::warpAffine(master.re, slave.re, affine, master.re.size(), cv::INTER_LINEAR, cv::BORDER_WRAP);
	cv::warpAffine(master.im, slave.im, affine, master.im.size(), cv::INTER_LINEAR, cv::BORDER_WRAP);
	return slave;
}

ComplexMat ResampleInverse(const ComplexMat& slave, double dy, double dx)
{
	ComplexMat restored(kRows, kCols);
	cv::Mat map_x(kRows, kCols, CV_32F);
	cv::Mat map_y(kRows, kCols, CV_32F);
	for (int r = 0; r < kRows; ++r)
	{
		float* const map_x_row = map_x.ptr<float>(r);
		float* const map_y_row = map_y.ptr<float>(r);
		for (int c = 0; c < kCols; ++c)
		{
			map_x_row[c] = static_cast<float>(c + dx);
			map_y_row[c] = static_cast<float>(r + dy);
		}
	}

	// out(r, c) = slave(r + dy, c + dx)
	cv::remap(slave.re, restored.re, map_x, map_y, cv::INTER_LINEAR, cv::BORDER_WRAP);
	cv::remap(slave.im, restored.im, map_x, map_y, cv::INTER_LINEAR, cv::BORDER_WRAP);
	return restored;
}

bool RunCase(Registration& registration, const ComplexMat& master, double injected_dy, double injected_dx)
{
	const ComplexMat slave = Translate(master, injected_dy, injected_dx);
	double measured_dy = 0.0;
	double measured_dx = 0.0;
	double snr = 0.0;
	const int measure_status = registration.real_coherent(master, slave, &measured_dy, &measured_dx, &snr);

	const ComplexMat restored = ResampleInverse(slave, measured_dy, measured_dx);
	double residual_dy = 0.0;
	double residual_dx = 0.0;
	double residual_snr = 0.0;
	const int residual_status = registration.real_coherent(
		master, restored, &residual_dy, &residual_dx, &residual_snr);

	std::printf(
		"injected dy=%+.3f dx=%+.3f; measured dy=%+.6f dx=%+.6f; SNR=%.3f; "
		"restored residual dy=%+.6f dx=%+.6f\n",
		injected_dy, injected_dx, measured_dy, measured_dx, snr, residual_dy, residual_dx);

	return measure_status == 0 &&
		residual_status == 0 &&
		std::isfinite(measured_dy) &&
		std::isfinite(measured_dx) &&
		std::isfinite(residual_dy) &&
		std::isfinite(residual_dx) &&
		std::abs(measured_dy - injected_dy) <= kTolerancePixels &&
		std::abs(measured_dx - injected_dx) <= kTolerancePixels &&
		std::abs(residual_dy) <= kTolerancePixels &&
		std::abs(residual_dx) <= kTolerancePixels;
}
}

int RunRegistrationSubpixelRegression()
{
	Registration registration;
	const ComplexMat master = CreateMaster();
	const double cases[][2] = {
		{ 0.00, -0.40 },
		{ 0.00, +0.40 },
		{ +0.25, -0.40 },
		{ -0.25, +0.40 },
	};

	bool passed = true;
	for (const auto& test_case : cases)
	{
		passed = RunCase(registration, master, test_case[0], test_case[1]) && passed;
	}

	std::printf("Registration subpixel regression: %s\n", passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
