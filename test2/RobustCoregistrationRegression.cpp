#include "RobustCoregistrationRegression.h"

#include "..\include\RobustCoregistration.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
constexpr int kRows = 1000;
constexpr int kColumns = 1000;
constexpr double kTolerance = 1e-9;

bool nearlyEqual(double actual, double expected)
{
	return std::abs(actual - expected) <= kTolerance + kTolerance * std::abs(expected);
}

RobustCoregistrationBlockMatch makeMatch(int blockRow, int blockColumn, double centerRow, double centerColumn, double dy, double dx, double snr = 10.0, int valid = 1)
{
	RobustCoregistrationBlockMatch match = {};
	match.structSize = sizeof(match);
	match.version = 1;
	match.blockRow = blockRow;
	match.blockColumn = blockColumn;
	match.centerRow = centerRow;
	match.centerColumn = centerColumn;
	match.dy = dy;
	match.dx = dx;
	match.snr = snr;
	match.valid = valid;
	return match;
}

std::vector<RobustCoregistrationBlockMatch> makeFiveByFiveMatches()
{
	std::vector<RobustCoregistrationBlockMatch> matches;
	matches.reserve(25);
	for (int row = 0; row < 5; ++row)
	{
		for (int column = 0; column < 5; ++column)
		{
			const double centerRow = 100.0 + row * 200.0;
			const double centerColumn = 100.0 + column * 200.0;
			const double x = (centerColumn - kColumns / 2.0) / kColumns;
			const double y = (centerRow - kRows / 2.0) / kRows;
			const double dy = 1.25 + 0.70 * x - 0.40 * y;
			const double dx = -2.00 + 0.30 * x + 0.50 * y;
			matches.push_back(makeMatch(row, column, centerRow, centerColumn, dy, dx));
		}
	}
	return matches;
}

bool initializeInputs(RobustCoregistrationImageSize& imageSize, RobustCoregistrationConfig& config)
{
	imageSize = {};
	imageSize.structSize = sizeof(imageSize);
	imageSize.version = 1;
	imageSize.rows = kRows;
	imageSize.columns = kColumns;
	config = {};
	config.structSize = sizeof(config);
	return GetDefaultRobustCoregistrationConfig(&config) == ROBUST_COREGISTRATION_SUCCESS && config.version == 1;
}

int runFit(const std::vector<RobustCoregistrationBlockMatch>& matches,
	RobustCoregistrationImageSize& imageSize,
	RobustCoregistrationConfig& config,
	RobustCoregistrationCancelCallback cancelCallback,
	void* cancelUserData,
	unsigned char* flags,
	RobustCoregistrationResult& result,
	char* diagnostic,
	int flagsCapacity = -1)
{
	result = {};
	result.structSize = sizeof(result);
	result.version = 1;
	return FitAffineRobustCoregistration(matches.data(), static_cast<int>(matches.size()), &imageSize, &config,
		cancelCallback, cancelUserData, flags, flagsCapacity < 0 ? static_cast<int>(matches.size()) : flagsCapacity,
		&result, diagnostic, 512);
}

int __stdcall cancelImmediately(void*)
{
	return 1;
}

struct CancellationAfterCalls
{
	int calls;
	int cancelAfter;
};

int __stdcall cancelAfterCalls(void* userData)
{
	CancellationAfterCalls* cancellation = static_cast<CancellationAfterCalls*>(userData);
	return ++cancellation->calls >= cancellation->cancelAfter;
}

std::vector<RobustCoregistrationBlockMatch> makeHighOutlierMatches()
{
	std::vector<RobustCoregistrationBlockMatch> matches;
	matches.reserve(100);
	for (int row = 0; row < 10; ++row)
	{
		for (int column = 0; column < 10; ++column)
		{
			const int index = row * 10 + column;
			const double centerRow = 50.0 + row * 100.0;
			const double centerColumn = 50.0 + column * 100.0;
			const double x = (centerColumn - kColumns / 2.0) / kColumns;
			const double y = (centerRow - kRows / 2.0) / kRows;
			const bool isInlier = ((row * 7 + column * 3) % 5) < 2;
			const double dy = isInlier ? 1.25 + 0.70 * x - 0.40 * y : 40.0 + static_cast<double>((index * 37) % 101);
			const double dx = isInlier ? -2.00 + 0.30 * x + 0.50 * y : -50.0 + static_cast<double>((index * 61) % 89);
			matches.push_back(makeMatch(row * 2, column * 2, centerRow, centerColumn, dy, dx));
		}
	}
	return matches;
}

std::vector<RobustCoregistrationBlockMatch> makeCoverageLimitedConsensusMatches()
{
	std::vector<RobustCoregistrationBlockMatch> matches;
	matches.reserve(25);
	for (int row = 0; row < 5; ++row)
	{
		for (int column = 0; column < 5; ++column)
		{
			const int index = row * 5 + column;
			const double centerRow = 50.0 + row * 200.0;
			const double centerColumn = 50.0 + column * 200.0;
			const double x = (centerColumn - kColumns / 2.0) / kColumns;
			const double y = (centerRow - kRows / 2.0) / kRows;
			const bool isInlier = row < 3 && column < 3;
			const double dy = isInlier ? 1.25 + 0.70 * x - 0.40 * y : 70.0 + static_cast<double>((index * 19) % 53);
			const double dx = isInlier ? -2.00 + 0.30 * x + 0.50 * y : -80.0 + static_cast<double>((index * 29) % 47);
			matches.push_back(makeMatch(row * 2, column * 2, centerRow, centerColumn, dy, dx));
		}
	}
	return matches;
}

std::vector<RobustCoregistrationBlockMatch> makeNoConsensusMatches()
{
	std::vector<RobustCoregistrationBlockMatch> matches;
	matches.reserve(25);
	for (int row = 0; row < 5; ++row)
	{
		for (int column = 0; column < 5; ++column)
		{
			const int index = row * 5 + column;
			matches.push_back(makeMatch(row * 2, column * 2, 50.0 + row * 200.0, 50.0 + column * 200.0,
				100.0 + static_cast<double>((index * 17) % 71), -120.0 + static_cast<double>((index * 31) % 67)));
		}
	}
	return matches;
}

std::vector<RobustCoregistrationBlockMatch> makeCollinearMatches()
{
	std::vector<RobustCoregistrationBlockMatch> matches;
	matches.reserve(16);
	for (int row = 0; row < 4; ++row)
	{
		for (int column = 0; column < 4; ++column)
		{
			const double centerRow = 125.0 + row * 250.0;
			const double centerColumn = 500.0;
			const double y = (centerRow - kRows / 2.0) / kRows;
			matches.push_back(makeMatch(row, column, centerRow, centerColumn, 1.0 - 0.4 * y, -2.0 + 0.5 * y));
		}
	}
	return matches;
}

int testNominalFit()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	if (config.enableRansac != 1) return 2;
	const std::vector<RobustCoregistrationBlockMatch> matches = makeFiveByFiveMatches();
	std::vector<unsigned char> flags(matches.size(), 0);
	RobustCoregistrationResult result;
	RobustCoregistrationResult oneAResult;
	char diagnostic[512] = {};
	char oneADiagnostic[512] = {};
	std::vector<unsigned char> oneAFlags(matches.size(), 0);
	config.enableRansac = 0;
	const int oneAStatus = runFit(matches, imageSize, config, nullptr, nullptr, oneAFlags.data(), oneAResult, oneADiagnostic);
	config.enableRansac = 1;
	const int status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (oneAStatus != ROBUST_COREGISTRATION_SUCCESS || status != ROBUST_COREGISTRATION_SUCCESS || result.statusCode != status || result.finalInlierCount != 25 ||
		result.prefilteredCount != 25 || result.qualityRejectedCount != 0 || result.neighborRejectedCount != 0) return 3;
	for (size_t index = 0; index < flags.size(); ++index)
	{
		if (flags[index] != 1 || oneAFlags[index] != 1) return 4;
	}
	if (!nearlyEqual(result.rowCoefficients[0], 1.25) || !nearlyEqual(result.rowCoefficients[1], 0.70) || !nearlyEqual(result.rowCoefficients[2], -0.40) ||
		!nearlyEqual(result.columnCoefficients[0], -2.00) || !nearlyEqual(result.columnCoefficients[1], 0.30) || !nearlyEqual(result.columnCoefficients[2], 0.50) ||
		!nearlyEqual(result.topLeftDy, 1.10) || !nearlyEqual(result.topLeftDx, -2.40) ||
		!nearlyEqual(result.rowCoefficients[0], oneAResult.rowCoefficients[0]) || !nearlyEqual(result.columnCoefficients[0], oneAResult.columnCoefficients[0]) ||
		!nearlyEqual(result.rowCoefficients[1], oneAResult.rowCoefficients[1]) || !nearlyEqual(result.rowCoefficients[2], oneAResult.rowCoefficients[2]) ||
		!nearlyEqual(result.columnCoefficients[1], oneAResult.columnCoefficients[1]) || !nearlyEqual(result.columnCoefficients[2], oneAResult.columnCoefficients[2]) ||
		result.ransacBestConsensusCount != 25 || result.designRank != 3 || result.occupiedGridCellCount != 9 || diagnostic[0] == '\0') return 5;
	return 0;
}

int testQualityAndNeighborRejection()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	std::vector<RobustCoregistrationBlockMatch> matches = makeFiveByFiveMatches();
	matches[0].snr = 1.0;
	matches[12].dy += 20.0;
	matches[12].dx -= 20.0;
	std::vector<unsigned char> flags(matches.size(), 0);
	RobustCoregistrationResult result;
	char diagnostic[512] = {};
	const int status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_SUCCESS || result.qualityRejectedCount != 1 || result.neighborRejectedCount != 1 ||
		result.prefilteredCount != 23 || result.finalInlierCount != 23 || flags[0] != 0 || flags[12] != 0) return 2;
	if (!nearlyEqual(result.rowCoefficients[0], 1.25) || !nearlyEqual(result.columnCoefficients[0], -2.00)) return 3;
	return 0;
}

int testRansacHighOutliersAndDeterminism()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	config.ransacResidualThresholdPixels = 0.1;
	config.ransacConfidence = 0.99;
	config.ransacMaxIterations = 1000;
	config.ransacRandomSeed = 0x12345678U;
	config.minimumInlierRatio = 0.30;
	const std::vector<RobustCoregistrationBlockMatch> matches = makeHighOutlierMatches();
	std::vector<unsigned char> firstFlags(matches.size(), 0);
	std::vector<unsigned char> secondFlags(matches.size(), 0);
	RobustCoregistrationResult firstResult;
	RobustCoregistrationResult secondResult;
	char firstDiagnostic[512] = {};
	char secondDiagnostic[512] = {};
	const int firstStatus = runFit(matches, imageSize, config, nullptr, nullptr, firstFlags.data(), firstResult, firstDiagnostic);
	const int secondStatus = runFit(matches, imageSize, config, nullptr, nullptr, secondFlags.data(), secondResult, secondDiagnostic);
	int expectedInliers = 0;
	for (int row = 0; row < 10; ++row)
	{
		for (int column = 0; column < 10; ++column)
		{
			const int index = row * 10 + column;
			const unsigned char expected = ((row * 7 + column * 3) % 5) < 2 ? 1 : 0;
			expectedInliers += expected;
			if (firstFlags[index] != expected || secondFlags[index] != expected) return 2;
		}
	}
	if (firstStatus != ROBUST_COREGISTRATION_SUCCESS || secondStatus != ROBUST_COREGISTRATION_SUCCESS ||
		firstResult.finalInlierCount != expectedInliers || firstResult.ransacBestConsensusCount != expectedInliers ||
		firstResult.ransacCandidateCount != static_cast<int>(matches.size()) || firstResult.ransacValidHypothesisCount == 0 ||
		!nearlyEqual(firstResult.rowCoefficients[0], 1.25) || !nearlyEqual(firstResult.columnCoefficients[0], -2.0) ||
		std::memcmp(firstFlags.data(), secondFlags.data(), firstFlags.size()) != 0 ||
		!nearlyEqual(firstResult.rowCoefficients[0], secondResult.rowCoefficients[0]) ||
		firstResult.ransacIterations != secondResult.ransacIterations || std::strcmp(firstDiagnostic, secondDiagnostic) != 0) return 3;

	config.structSize = static_cast<unsigned int>(offsetof(RobustCoregistrationConfig, enableRansac));
	config.enableRansac = 0;
	std::vector<unsigned char> legacyFlags(matches.size(), 0);
	RobustCoregistrationResult legacyResult;
	char legacyDiagnostic[512] = {};
	const int legacyStatus = runFit(matches, imageSize, config, nullptr, nullptr, legacyFlags.data(), legacyResult, legacyDiagnostic);
	if (legacyStatus != ROBUST_COREGISTRATION_SUCCESS || legacyResult.ransacBestConsensusCount != expectedInliers ||
		std::memcmp(firstFlags.data(), legacyFlags.data(), firstFlags.size()) != 0) return 4;
	return 0;
}

int testRansacCoverageAndNoConsensus()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	config.ransacResidualThresholdPixels = 0.1;
	config.ransacMaxIterations = 1000;
	config.minimumInlierCount = 5;
	config.minimumInlierRatio = 0.20;
	std::vector<RobustCoregistrationBlockMatch> matches = makeCoverageLimitedConsensusMatches();
	std::vector<unsigned char> flags(matches.size(), 0);
	RobustCoregistrationResult result;
	char diagnostic[512] = {};
	int status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_UNSTABLE_GEOMETRY || result.ransacBestConsensusCount != 9 ||
		result.occupiedGridCellCount >= config.minimumOccupiedGridCells) return 2;

	matches = makeNoConsensusMatches();
	flags.assign(matches.size(), 0);
	config.minimumInlierCount = 10;
	config.minimumInlierRatio = 0.5;
	config.ransacResidualThresholdPixels = 0.01;
	status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_RANSAC_NO_CONSENSUS || result.ransacBestConsensusCount >= config.minimumInlierCount || diagnostic[0] == '\0') return 3;
	return 0;
}

int testRansacDegenerateSamples()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	config.ransacMaxIterations = 20;
	const std::vector<RobustCoregistrationBlockMatch> matches = makeCollinearMatches();
	std::vector<unsigned char> flags(matches.size(), 0);
	RobustCoregistrationResult result;
	char diagnostic[512] = {};
	const int status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_RANSAC_NO_CONSENSUS || result.ransacValidHypothesisCount != 0 ||
		result.ransacDegenerateHypothesisCount != config.ransacMaxIterations || result.ransacIterations != config.ransacMaxIterations) return 2;
	return 0;
}

int testUnstableGeometry()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	config.enableRansac = 0;
	const std::vector<RobustCoregistrationBlockMatch> matches = makeCollinearMatches();
	std::vector<unsigned char> flags(matches.size(), 0);
	RobustCoregistrationResult result;
	char diagnostic[512] = {};
	const int status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_UNSTABLE_GEOMETRY || result.designRank >= 3 || result.occupiedColumnBandCount != 1) return 2;
	for (unsigned char flag : flags)
	{
		if (flag != 0) return 3;
	}
	return 0;
}

int testInsufficientMatches()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	std::vector<RobustCoregistrationBlockMatch> matches = makeFiveByFiveMatches();
	matches.resize(5);
	std::vector<unsigned char> flags(matches.size(), 0);
	RobustCoregistrationResult result;
	char diagnostic[512] = {};
	const int status = runFit(matches, imageSize, config, nullptr, nullptr, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_INSUFFICIENT_MATCHES || result.prefilteredCount != 5 || diagnostic[0] == '\0') return 2;
	for (unsigned char flag : flags)
	{
		if (flag != 0) return 3;
	}
	return 0;
}

int testCancellationAndAbiValidation()
{
	RobustCoregistrationImageSize imageSize;
	RobustCoregistrationConfig config;
	if (!initializeInputs(imageSize, config)) return 1;
	const std::vector<RobustCoregistrationBlockMatch> matches = makeHighOutlierMatches();
	std::vector<unsigned char> flags(matches.size(), 7);
	RobustCoregistrationResult result;
	char diagnostic[512] = {};
	CancellationAfterCalls cancellation = {};
	cancellation.cancelAfter = static_cast<int>(matches.size()) * 2 + 2;
	int status = runFit(matches, imageSize, config, cancelAfterCalls, &cancellation, flags.data(), result, diagnostic);
	if (status != ROBUST_COREGISTRATION_CANCELLED || result.statusCode != status || result.ransacCandidateCount != static_cast<int>(matches.size()) || result.ransacIterations < 1) return 2;
	for (unsigned char flag : flags)
	{
		if (flag != 0) return 3;
	}
	status = runFit(matches, imageSize, config, cancelImmediately, nullptr, flags.data(), result, diagnostic, static_cast<int>(matches.size()) - 1);
	if (status != ROBUST_COREGISTRATION_INVALID_ARGUMENT || result.statusCode != status) return 4;
	return 0;
}
}

int RunRobustCoregistrationRegression()
{
	const int nominal = testNominalFit();
	const int filtering = testQualityAndNeighborRejection();
	const int ransacOutliers = testRansacHighOutliersAndDeterminism();
	const int ransacFailures = testRansacCoverageAndNoConsensus();
	const int ransacDegenerate = testRansacDegenerateSamples();
	const int geometry = testUnstableGeometry();
	const int insufficient = testInsufficientMatches();
	const int cancellation = testCancellationAndAbiValidation();
	const bool passed = nominal == 0 && filtering == 0 && ransacOutliers == 0 && ransacFailures == 0 && ransacDegenerate == 0 && geometry == 0 && insufficient == 0 && cancellation == 0;
	std::printf("Robust coregistration regression: %s (nominal=%d, filtering=%d, ransacOutliers=%d, ransacFailures=%d, ransacDegenerate=%d, geometry=%d, insufficient=%d, cancellation=%d)\n",
		passed ? "PASS" : "FAIL", nominal, filtering, ransacOutliers, ransacFailures, ransacDegenerate, geometry, insufficient, cancellation);
	return passed ? 0 : 1;
}
