#include "RegistrationSubpixelRegression.h"

#include "..\include\ComplexMat.h"
#include "..\include\Hdf5IO.h"
#include "..\include\Registration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#define NOMINMAX
#include <windows.h>

#include "opencv2\core\core.hpp"
#include "opencv2\imgproc\imgproc.hpp"

namespace
{
const int kRows = 256;
const int kCols = 256;
const double kTolerancePixels = 0.05;
const int kCropWindowSize = 5;
const int kCropBorder = kCropWindowSize / 2;

struct ExpectedCropStatistics
{
	double mean = 0.0;
	double median = 0.0;
	double maximum = 0.0;
	double highValueRatio = 0.0;
	long long sampleCount = 0;
};

struct TemporaryCropFiles
{
	std::string masterH5;
	std::string slaveH5;
	std::string coherenceJpg;
	std::string phaseJpg;

	~TemporaryCropFiles()
	{
		if (!masterH5.empty()) DeleteFileA(masterH5.c_str());
		if (!slaveH5.empty()) DeleteFileA(slaveH5.c_str());
		if (!coherenceJpg.empty()) DeleteFileA(coherenceJpg.c_str());
		if (!phaseJpg.empty()) DeleteFileA(phaseJpg.c_str());
	}
};

bool makeTemporaryPath(const char* extension, std::string& path)
{
	char directory[MAX_PATH] = {};
	char placeholder[MAX_PATH] = {};
	if (GetTempPathA(MAX_PATH, directory) == 0 ||
		GetTempFileNameA(directory, "icr", 0, placeholder) == 0)
	{
		return false;
	}
	DeleteFileA(placeholder);
	path = std::string(placeholder) + extension;
	return true;
}

bool computeExpectedStatistics(const cv::Mat& coherence, const cv::Mat& validSampleCount,
	int requiredSampleCount, int border, ExpectedCropStatistics& statistics)
{
	statistics = {};
	if (coherence.empty() || coherence.type() != CV_32F ||
		coherence.rows <= 2 * border || coherence.cols <= 2 * border ||
		(!validSampleCount.empty() && (validSampleCount.type() != CV_16U ||
			validSampleCount.size() != coherence.size() || requiredSampleCount <= 0)))
	{
		return false;
	}

	int histogram[10000] = { 0 };
	double sum = 0.0;
	double maximum = 0.0;
	long long highCount = 0;
	for (int row = border; row < coherence.rows - border; ++row)
	{
		const float* values = coherence.ptr<float>(row);
		const unsigned short* support = validSampleCount.empty()
			? nullptr : validSampleCount.ptr<unsigned short>(row);
		for (int column = border; column < coherence.cols - border; ++column)
		{
			const float value = values[column];
			if (support != nullptr && support[column] != requiredSampleCount) continue;
			if (!std::isfinite(value) || value < 0.0f || value > 1.0f) continue;
			const int bin = std::min(9999, static_cast<int>(value * 9999.0f));
			++histogram[bin];
			sum += value;
			maximum = std::max(maximum, static_cast<double>(value));
			++statistics.sampleCount;
			if (value > 0.5f) ++highCount;
		}
	}
	if (statistics.sampleCount <= 0) return false;

	const long long target = (statistics.sampleCount + 1) / 2;
	long long accumulated = 0;
	int medianBin = 0;
	for (int i = 0; i < 10000; ++i)
	{
		accumulated += histogram[i];
		if (accumulated >= target)
		{
			medianBin = i;
			break;
		}
	}
	statistics.mean = sum / static_cast<double>(statistics.sampleCount);
	statistics.median = medianBin / 9999.0;
	statistics.maximum = maximum;
	statistics.highValueRatio = static_cast<double>(highCount) / static_cast<double>(statistics.sampleCount);
	return true;
}

bool buildExpectedCoherence(const cv::Mat& masterRe, const cv::Mat& masterIm,
	const cv::Mat& slaveRe, const cv::Mat& slaveIm, cv::Mat& coherence,
	cv::Mat& validSampleCount)
{
	cv::Mat numeratorRe = masterRe.mul(slaveRe) + masterIm.mul(slaveIm);
	cv::Mat numeratorIm = masterIm.mul(slaveRe) - masterRe.mul(slaveIm);
	cv::Mat masterPower = masterRe.mul(masterRe) + masterIm.mul(masterIm);
	cv::Mat slavePower = slaveRe.mul(slaveRe) + slaveIm.mul(slaveIm);
	cv::Mat masterValid = masterPower > 0.0f;
	cv::Mat slaveValid = slavePower > 0.0f;
	cv::Mat inputValid;
	cv::bitwise_and(masterValid, slaveValid, inputValid);
	inputValid.setTo(1, inputValid);
	const cv::Size window(kCropWindowSize, kCropWindowSize);
	cv::boxFilter(numeratorRe, numeratorRe, CV_32F, window, cv::Point(-1, -1), false);
	cv::boxFilter(numeratorIm, numeratorIm, CV_32F, window, cv::Point(-1, -1), false);
	cv::boxFilter(masterPower, masterPower, CV_32F, window, cv::Point(-1, -1), false);
	cv::boxFilter(slavePower, slavePower, CV_32F, window, cv::Point(-1, -1), false);
	cv::boxFilter(inputValid, validSampleCount, CV_16U, window, cv::Point(-1, -1), false);
	cv::Mat numeratorMagnitude;
	cv::magnitude(numeratorRe, numeratorIm, numeratorMagnitude);
	cv::Mat denominator;
	cv::sqrt(masterPower.mul(slavePower), denominator);
	coherence = numeratorMagnitude / (denominator + 1e-12f);
	cv::threshold(coherence, coherence, 1.0, 1.0, cv::THRESH_TRUNC);
	cv::threshold(coherence, coherence, 0.0, 0.0, cv::THRESH_TOZERO);
	return true;
}

bool RunCropStatisticsCase()
{
	const int rows = 96;
	const int columns = 96;
	cv::Mat masterRe(rows, columns, CV_32F);
	cv::Mat masterIm(rows, columns, CV_32F);
	cv::Mat slaveRe(rows, columns, CV_32F);
	cv::Mat slaveIm(rows, columns, CV_32F);
	cv::RNG random(0x491ad2U);
	for (int row = 0; row < rows; ++row)
	{
		for (int column = 0; column < columns; ++column)
		{
			const double phase = random.uniform(-CV_PI, CV_PI);
			const double amplitude = random.uniform(0.5, 2.0);
			masterRe.at<float>(row, column) = static_cast<float>(amplitude * std::cos(phase));
			masterIm.at<float>(row, column) = static_cast<float>(amplitude * std::sin(phase));
			const bool lowQualityEdge = row < 8 || row >= rows - 8 || column < 8 || column >= columns - 8;
			if (lowQualityEdge)
			{
				slaveRe.at<float>(row, column) = 0.0f;
				slaveIm.at<float>(row, column) = 0.0f;
			}
			else
			{
				const double slavePhase = phase + random.gaussian(1.2);
				slaveRe.at<float>(row, column) = static_cast<float>(amplitude * std::cos(slavePhase));
				slaveIm.at<float>(row, column) = static_cast<float>(amplitude * std::sin(slavePhase));
			}
		}
	}

	cv::Mat expectedCoherence;
	cv::Mat validSampleCount;
	buildExpectedCoherence(masterRe, masterIm, slaveRe, slaveIm, expectedCoherence, validSampleCount);
	ExpectedCropStatistics expectedInner;
	ExpectedCropStatistics expectedFull;
	const int requiredSampleCount = kCropWindowSize * kCropWindowSize;
	if (!computeExpectedStatistics(expectedCoherence, validSampleCount,
			requiredSampleCount, kCropBorder, expectedInner) ||
		!computeExpectedStatistics(expectedCoherence, cv::Mat(), 0, 0, expectedFull))
	{
		return false;
	}
	const bool discriminatesOldSampling =
		std::abs(expectedInner.mean - expectedFull.mean) > 1e-4 ||
		std::abs(expectedInner.median - expectedFull.median) > 1e-4 ||
		std::abs(expectedInner.maximum - expectedFull.maximum) > 1e-4 ||
		std::abs(expectedInner.highValueRatio - expectedFull.highValueRatio) > 1e-4;
	if (!discriminatesOldSampling) return false;

	TemporaryCropFiles files;
	if (!makeTemporaryPath(".h5", files.masterH5) ||
		!makeTemporaryPath(".h5", files.slaveH5) ||
		!makeTemporaryPath(".jpg", files.coherenceJpg) ||
		!makeTemporaryPath(".jpg", files.phaseJpg))
	{
		return false;
	}
	if (Hdf5IO::createFile(files.masterH5.c_str()) != 0 ||
		Hdf5IO::writeArray(files.masterH5.c_str(), "s_re", masterRe) != 0 ||
		Hdf5IO::writeArray(files.masterH5.c_str(), "s_im", masterIm) != 0 ||
		Hdf5IO::createFile(files.slaveH5.c_str()) != 0 ||
		Hdf5IO::writeArray(files.slaveH5.c_str(), "s_re", slaveRe) != 0 ||
		Hdf5IO::writeArray(files.slaveH5.c_str(), "s_im", slaveIm) != 0)
	{
		return false;
	}

	CropEvalResult result = {};
	result.structSize = sizeof(result);
	const int status = AnalyzeCropRegistration(
		files.masterH5.c_str(), files.slaveH5.c_str(),
		files.coherenceJpg.c_str(), files.phaseJpg.c_str(),
		-1.0, -1.0, -1.0, -1.0, &result);
	const double medianTolerance = 1.1 / 9999.0;
	const bool passed = status == 0 &&
		std::abs(result.meanCoherence - expectedInner.mean) < 1e-6 &&
		std::abs(result.medianCoherence - expectedInner.median) < medianTolerance &&
		std::abs(result.maxCoherence - expectedInner.maximum) < 1e-6 &&
		std::abs(result.highCoherencePct - expectedInner.highValueRatio) < 1e-9;
	std::printf(
		"crop coherence inner-region regression: %s; mean=%.6f/%.6f median=%.6f/%.6f max=%.6f/%.6f high=%.6f/%.6f\n",
		passed ? "PASS" : "FAIL",
		result.meanCoherence, expectedInner.mean,
		result.medianCoherence, expectedInner.median,
		result.maxCoherence, expectedInner.maximum,
		result.highCoherencePct, expectedInner.highValueRatio);
	return passed;
}

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
	passed = RunCropStatisticsCase() && passed;

	std::printf("Registration subpixel regression: %s\n", passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
