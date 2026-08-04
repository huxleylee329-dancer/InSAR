#include "stdafx.h"

#include "..\include\RobustCoregistration.h"
#include "..\include\Package.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
constexpr unsigned int kRobustCoregistrationVersion = 1;
constexpr double kNeighborDifferenceThreshold = 2.0;
constexpr int kMaximumIrlsIterations = 10;
constexpr int kMaximumFinalRefitPasses = 3;
constexpr size_t kConfig1ASize = offsetof(RobustCoregistrationConfig, enableRansac);

struct FitPoint
{
	int matchIndex;
	double x;
	double y;
	double dy;
	double dx;
};

struct EffectiveConfig
{
	double minimumBlockMatchSnr;
	int minimumInlierCount;
	double minimumInlierRatio;
	double maximumDesignConditionNumber;
	int minimumOccupiedGridCells;
	double residualFloorPixels;
	double huberCutoffSigma;
	double inlierSigma;
	int enableRansac;
	double ransacResidualThresholdPixels;
	double ransacConfidence;
	int ransacMaxIterations;
	unsigned int ransacRandomSeed;
	double ransacMinimumTriangleArea;
};

EffectiveConfig defaultConfig()
{
	EffectiveConfig config = {};
	config.minimumBlockMatchSnr = 3.0;
	config.minimumInlierCount = 10;
	config.minimumInlierRatio = 0.5;
	config.maximumDesignConditionNumber = 10000.0;
	config.minimumOccupiedGridCells = 5;
	config.residualFloorPixels = 1e-3;
	config.huberCutoffSigma = 2.4477;
	config.inlierSigma = 3.0;
	config.enableRansac = 1;
	config.ransacResidualThresholdPixels = 0.5;
	config.ransacConfidence = 0.99;
	config.ransacMaxIterations = 1000;
	config.ransacRandomSeed = 0x52414E53U;
	config.ransacMinimumTriangleArea = 1e-6;
	return config;
}

void writeDiagnostic(char* buffer, int capacity, const char* format, ...)
{
	if (!buffer || capacity <= 0)
	{
		return;
	}

	va_list arguments;
	va_start(arguments, format);
	vsnprintf_s(buffer, static_cast<size_t>(capacity), _TRUNCATE, format, arguments);
	va_end(arguments);
}

bool cancelled(RobustCoregistrationCancelCallback callback, void* userData)
{
	return callback && callback(userData) != 0;
}

bool isFinite(double value)
{
	return std::isfinite(value) != 0;
}

bool hasConfigField(unsigned int structureSize, size_t fieldOffset, size_t fieldSize)
{
	return structureSize >= fieldOffset + fieldSize;
}

bool makeEffectiveConfig(const RobustCoregistrationConfig* input, EffectiveConfig& output)
{
	if (!input || input->version != kRobustCoregistrationVersion || input->structSize < kConfig1ASize)
	{
		return false;
	}

	output = defaultConfig();
	output.minimumBlockMatchSnr = input->minimumBlockMatchSnr;
	output.minimumInlierCount = input->minimumInlierCount;
	output.minimumInlierRatio = input->minimumInlierRatio;
	output.maximumDesignConditionNumber = input->maximumDesignConditionNumber;
	output.minimumOccupiedGridCells = input->minimumOccupiedGridCells;
	output.residualFloorPixels = input->residualFloorPixels;
	output.huberCutoffSigma = input->huberCutoffSigma;
	output.inlierSigma = input->inlierSigma;
	if (hasConfigField(input->structSize, offsetof(RobustCoregistrationConfig, enableRansac), sizeof(input->enableRansac))) output.enableRansac = input->enableRansac;
	if (hasConfigField(input->structSize, offsetof(RobustCoregistrationConfig, ransacResidualThresholdPixels), sizeof(input->ransacResidualThresholdPixels))) output.ransacResidualThresholdPixels = input->ransacResidualThresholdPixels;
	if (hasConfigField(input->structSize, offsetof(RobustCoregistrationConfig, ransacConfidence), sizeof(input->ransacConfidence))) output.ransacConfidence = input->ransacConfidence;
	if (hasConfigField(input->structSize, offsetof(RobustCoregistrationConfig, ransacMaxIterations), sizeof(input->ransacMaxIterations))) output.ransacMaxIterations = input->ransacMaxIterations;
	if (hasConfigField(input->structSize, offsetof(RobustCoregistrationConfig, ransacRandomSeed), sizeof(input->ransacRandomSeed))) output.ransacRandomSeed = input->ransacRandomSeed;
	if (hasConfigField(input->structSize, offsetof(RobustCoregistrationConfig, ransacMinimumTriangleArea), sizeof(input->ransacMinimumTriangleArea))) output.ransacMinimumTriangleArea = input->ransacMinimumTriangleArea;

	return isFinite(output.minimumBlockMatchSnr) && output.minimumBlockMatchSnr > 0.0 && output.minimumInlierCount >= 3 &&
		isFinite(output.minimumInlierRatio) && output.minimumInlierRatio >= 0.0 && output.minimumInlierRatio <= 1.0 &&
		isFinite(output.maximumDesignConditionNumber) && output.maximumDesignConditionNumber > 0.0 &&
		output.minimumOccupiedGridCells >= 1 && output.minimumOccupiedGridCells <= 9 &&
		isFinite(output.residualFloorPixels) && output.residualFloorPixels > 0.0 &&
		isFinite(output.huberCutoffSigma) && output.huberCutoffSigma > 0.0 &&
		isFinite(output.inlierSigma) && output.inlierSigma > 0.0 &&
		(output.enableRansac == 0 || output.enableRansac == 1) &&
		isFinite(output.ransacResidualThresholdPixels) && output.ransacResidualThresholdPixels > 0.0 &&
		isFinite(output.ransacConfidence) && output.ransacConfidence > 0.0 && output.ransacConfidence < 1.0 &&
		output.ransacMaxIterations > 0 && isFinite(output.ransacMinimumTriangleArea) && output.ransacMinimumTriangleArea > 0.0;
}

double medianOfValues(std::vector<double> values)
{
	if (values.empty())
	{
		return 0.0;
	}

	std::sort(values.begin(), values.end());
	const size_t middle = values.size() / 2;
	return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) * 0.5 : values[middle];
}

double percentileOfValues(std::vector<double> values, double percentile)
{
	if (values.empty())
	{
		return 0.0;
	}

	std::sort(values.begin(), values.end());
	const double boundedPercentile = std::max(0.0, std::min(1.0, percentile));
	const size_t roundedIndex = static_cast<size_t>(std::ceil(boundedPercentile * values.size()));
	const size_t index = roundedIndex == 0 ? 0 : roundedIndex - 1;
	return values[std::min(index, values.size() - 1)];
}

bool solveWeightedAffine(const std::vector<FitPoint>& points,
	const std::vector<double>& values,
	const std::vector<double>& weights,
	cv::Mat& coefficients)
{
	if (points.empty() || points.size() != values.size() || points.size() != weights.size())
	{
		return false;
	}

	cv::Mat weightedDesign(static_cast<int>(points.size()), 3, CV_64F);
	cv::Mat weightedValues(static_cast<int>(points.size()), 1, CV_64F);
	for (size_t index = 0; index < points.size(); ++index)
	{
		const double rootWeight = std::sqrt(std::max(0.0, weights[index]));
		weightedDesign.at<double>(static_cast<int>(index), 0) = rootWeight;
		weightedDesign.at<double>(static_cast<int>(index), 1) = rootWeight * points[index].x;
		weightedDesign.at<double>(static_cast<int>(index), 2) = rootWeight * points[index].y;
		weightedValues.at<double>(static_cast<int>(index), 0) = rootWeight * values[index];
	}
	return cv::solve(weightedDesign, weightedValues, coefficients, cv::DECOMP_SVD);
}

std::vector<double> calculateResiduals(const std::vector<FitPoint>& points,
	const cv::Mat& rowCoefficients,
	const cv::Mat& columnCoefficients)
{
	std::vector<double> residuals(points.size());
	for (size_t index = 0; index < points.size(); ++index)
	{
		const FitPoint& point = points[index];
		const double predictedRow = rowCoefficients.at<double>(0, 0) + rowCoefficients.at<double>(1, 0) * point.x + rowCoefficients.at<double>(2, 0) * point.y;
		const double predictedColumn = columnCoefficients.at<double>(0, 0) + columnCoefficients.at<double>(1, 0) * point.x + columnCoefficients.at<double>(2, 0) * point.y;
		residuals[index] = std::hypot(point.dy - predictedRow, point.dx - predictedColumn);
	}
	return residuals;
}

bool solveInlierFit(const std::vector<FitPoint>& points,
	const std::vector<int>& inlierRows,
	cv::Mat& rowCoefficients,
	cv::Mat& columnCoefficients)
{
	if (inlierRows.size() < 3)
	{
		return false;
	}

	cv::Mat design(static_cast<int>(inlierRows.size()), 3, CV_64F);
	cv::Mat rowValues(static_cast<int>(inlierRows.size()), 1, CV_64F);
	cv::Mat columnValues(static_cast<int>(inlierRows.size()), 1, CV_64F);
	for (size_t index = 0; index < inlierRows.size(); ++index)
	{
		const FitPoint& point = points[inlierRows[index]];
		design.at<double>(static_cast<int>(index), 0) = 1.0;
		design.at<double>(static_cast<int>(index), 1) = point.x;
		design.at<double>(static_cast<int>(index), 2) = point.y;
		rowValues.at<double>(static_cast<int>(index), 0) = point.dy;
		columnValues.at<double>(static_cast<int>(index), 0) = point.dx;
	}

	return cv::solve(design, rowValues, rowCoefficients, cv::DECOMP_SVD) &&
		cv::solve(design, columnValues, columnCoefficients, cv::DECOMP_SVD);
}

std::vector<int> selectInliers(const std::vector<FitPoint>& points,
	const cv::Mat& rowCoefficients,
	const cv::Mat& columnCoefficients,
	const EffectiveConfig& config,
	std::vector<double>& residuals,
	double& sigma)
{
	residuals = calculateResiduals(points, rowCoefficients, columnCoefficients);
	sigma = std::max(config.residualFloorPixels,
		medianOfValues(residuals) / std::sqrt(2.0 * std::log(2.0)));
	const double threshold = config.inlierSigma * sigma;

	std::vector<int> rows;
	rows.reserve(points.size());
	for (size_t index = 0; index < residuals.size(); ++index)
	{
		if (residuals[index] <= threshold)
		{
			rows.push_back(static_cast<int>(index));
		}
	}
	return rows;
}

bool solveThreePointAffine(const FitPoint& first,
	const FitPoint& second,
	const FitPoint& third,
	const EffectiveConfig& config,
	cv::Mat& rowCoefficients,
	cv::Mat& columnCoefficients)
{
	const double doubledArea = std::abs((second.x - first.x) * (third.y - first.y) - (second.y - first.y) * (third.x - first.x));
	if (doubledArea * 0.5 <= config.ransacMinimumTriangleArea)
	{
		return false;
	}

	cv::Mat design(3, 3, CV_64F);
	const FitPoint* sample[3] = { &first, &second, &third };
	for (int index = 0; index < 3; ++index)
	{
		design.at<double>(index, 0) = 1.0;
		design.at<double>(index, 1) = sample[index]->x;
		design.at<double>(index, 2) = sample[index]->y;
	}

	cv::SVD svd(design, cv::SVD::NO_UV);
	const double maximumSingularValue = svd.w.at<double>(0, 0);
	const double minimumSingularValue = svd.w.at<double>(svd.w.rows - 1, 0);
	const double rankTolerance = std::numeric_limits<double>::epsilon() * 3.0 * maximumSingularValue;
	if (minimumSingularValue <= rankTolerance || !isFinite(maximumSingularValue) || !isFinite(minimumSingularValue) ||
		maximumSingularValue / minimumSingularValue > config.maximumDesignConditionNumber)
	{
		return false;
	}

	cv::Mat rowValues(3, 1, CV_64F);
	cv::Mat columnValues(3, 1, CV_64F);
	for (int index = 0; index < 3; ++index)
	{
		rowValues.at<double>(index, 0) = sample[index]->dy;
		columnValues.at<double>(index, 0) = sample[index]->dx;
	}
	return cv::solve(design, rowValues, rowCoefficients, cv::DECOMP_SVD) &&
		cv::solve(design, columnValues, columnCoefficients, cv::DECOMP_SVD);
}

int requiredRansacIterations(double inlierRatio, const EffectiveConfig& config)
{
	if (inlierRatio >= 1.0)
	{
		return 1;
	}
	if (inlierRatio <= 0.0)
	{
		return config.ransacMaxIterations;
	}
	const double allInlierSampleProbability = inlierRatio * inlierRatio * inlierRatio;
	if (allInlierSampleProbability >= 1.0)
	{
		return 1;
	}
	const double denominator = std::log(1.0 - allInlierSampleProbability);
	if (!isFinite(denominator) || denominator >= 0.0)
	{
		return config.ransacMaxIterations;
	}
	const double calculated = std::ceil(std::log(1.0 - config.ransacConfidence) / denominator);
	if (!isFinite(calculated) || calculated <= 0.0)
	{
		return config.ransacMaxIterations;
	}
	return std::max(1, std::min(config.ransacMaxIterations, static_cast<int>(calculated)));
}

int selectRansacConsensus(const std::vector<FitPoint>& candidates,
	const EffectiveConfig& config,
	RobustCoregistrationCancelCallback cancelCallback,
	void* cancelUserData,
	RobustCoregistrationResult* result,
	std::vector<FitPoint>& consensus)
{
	result->ransacCandidateCount = static_cast<int>(candidates.size());
	result->ransacResidualThresholdPixels = config.ransacResidualThresholdPixels;
	if (candidates.size() < 3)
	{
		return ROBUST_COREGISTRATION_RANSAC_NO_CONSENSUS;
	}

	std::mt19937 random(config.ransacRandomSeed);
	std::uniform_int_distribution<int> draw(0, static_cast<int>(candidates.size()) - 1);
	int iterationLimit = config.ransacMaxIterations;
	int bestConsensusCount = 0;
	double bestResidualSum = std::numeric_limits<double>::infinity();
	std::vector<int> bestInlierRows;

	for (int iteration = 0; iteration < iterationLimit; ++iteration)
	{
		if (cancelled(cancelCallback, cancelUserData))
		{
			return ROBUST_COREGISTRATION_CANCELLED;
		}
		result->ransacIterations = iteration + 1;
		const int firstIndex = draw(random);
		int secondIndex = draw(random);
		while (secondIndex == firstIndex) secondIndex = draw(random);
		int thirdIndex = draw(random);
		while (thirdIndex == firstIndex || thirdIndex == secondIndex) thirdIndex = draw(random);

		cv::Mat rowCoefficients;
		cv::Mat columnCoefficients;
		if (!solveThreePointAffine(candidates[firstIndex], candidates[secondIndex], candidates[thirdIndex], config, rowCoefficients, columnCoefficients))
		{
			++result->ransacDegenerateHypothesisCount;
			continue;
		}
		++result->ransacValidHypothesisCount;

		const std::vector<double> residuals = calculateResiduals(candidates, rowCoefficients, columnCoefficients);
		std::vector<int> inlierRows;
		inlierRows.reserve(candidates.size());
		double residualSum = 0.0;
		for (size_t index = 0; index < residuals.size(); ++index)
		{
			if (residuals[index] <= config.ransacResidualThresholdPixels)
			{
				inlierRows.push_back(static_cast<int>(index));
				residualSum += residuals[index] * residuals[index];
			}
		}

		const int inlierCount = static_cast<int>(inlierRows.size());
		if (inlierCount > bestConsensusCount || (inlierCount == bestConsensusCount && residualSum < bestResidualSum))
		{
			bestConsensusCount = inlierCount;
			bestResidualSum = residualSum;
			bestInlierRows.swap(inlierRows);
			iterationLimit = std::min(iterationLimit, requiredRansacIterations(
				static_cast<double>(bestConsensusCount) / static_cast<double>(candidates.size()), config));
		}
	}

	result->ransacBestConsensusCount = bestConsensusCount;
	result->ransacBestConsensusRatio = static_cast<double>(bestConsensusCount) / static_cast<double>(candidates.size());
	if (bestConsensusCount < config.minimumInlierCount || result->ransacBestConsensusRatio < config.minimumInlierRatio)
	{
		return ROBUST_COREGISTRATION_RANSAC_NO_CONSENSUS;
	}
	consensus.reserve(bestInlierRows.size());
	for (int row : bestInlierRows)
	{
		consensus.push_back(candidates[row]);
	}
	return ROBUST_COREGISTRATION_SUCCESS;
}

unsigned long long gridKey(int row, int column)
{
	return (static_cast<unsigned long long>(static_cast<unsigned int>(row)) << 32) |
		static_cast<unsigned int>(column);
}

bool validStructure(const void* value, unsigned int structureSize, unsigned int version, size_t expectedSize)
{
	return value && structureSize >= expectedSize && version == kRobustCoregistrationVersion;
}

void initializeResult(RobustCoregistrationResult* result)
{
	std::memset(result, 0, sizeof(*result));
	result->structSize = sizeof(*result);
	result->version = kRobustCoregistrationVersion;
	result->statusCode = ROBUST_COREGISTRATION_INVALID_ARGUMENT;
	result->conditionNumber = std::numeric_limits<double>::infinity();
}

int fail(RobustCoregistrationResult* result, int status, char* diagnosticBuffer, int diagnosticBufferCapacity, const char* message)
{
	result->statusCode = status;
	writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity, "%s", message);
	return status;
}
}

extern "C" ROBUST_COREGISTRATION_API int GetDefaultRobustCoregistrationConfig(RobustCoregistrationConfig* outConfig)
{
	if (!outConfig || outConfig->structSize < kConfig1ASize)
	{
		return ROBUST_COREGISTRATION_INVALID_ARGUMENT;
	}

	const unsigned int callerStructureSize = outConfig->structSize;
	std::memset(outConfig, 0, std::min(static_cast<size_t>(callerStructureSize), sizeof(*outConfig)));
	outConfig->structSize = callerStructureSize;
	outConfig->version = kRobustCoregistrationVersion;
	const EffectiveConfig defaults = defaultConfig();
	outConfig->minimumBlockMatchSnr = defaults.minimumBlockMatchSnr;
	outConfig->minimumInlierCount = defaults.minimumInlierCount;
	outConfig->minimumInlierRatio = defaults.minimumInlierRatio;
	outConfig->maximumDesignConditionNumber = defaults.maximumDesignConditionNumber;
	outConfig->minimumOccupiedGridCells = defaults.minimumOccupiedGridCells;
	outConfig->residualFloorPixels = defaults.residualFloorPixels;
	outConfig->huberCutoffSigma = defaults.huberCutoffSigma;
	outConfig->inlierSigma = defaults.inlierSigma;
	if (hasConfigField(callerStructureSize, offsetof(RobustCoregistrationConfig, enableRansac), sizeof(outConfig->enableRansac))) outConfig->enableRansac = defaults.enableRansac;
	if (hasConfigField(callerStructureSize, offsetof(RobustCoregistrationConfig, ransacResidualThresholdPixels), sizeof(outConfig->ransacResidualThresholdPixels))) outConfig->ransacResidualThresholdPixels = defaults.ransacResidualThresholdPixels;
	if (hasConfigField(callerStructureSize, offsetof(RobustCoregistrationConfig, ransacConfidence), sizeof(outConfig->ransacConfidence))) outConfig->ransacConfidence = defaults.ransacConfidence;
	if (hasConfigField(callerStructureSize, offsetof(RobustCoregistrationConfig, ransacMaxIterations), sizeof(outConfig->ransacMaxIterations))) outConfig->ransacMaxIterations = defaults.ransacMaxIterations;
	if (hasConfigField(callerStructureSize, offsetof(RobustCoregistrationConfig, ransacRandomSeed), sizeof(outConfig->ransacRandomSeed))) outConfig->ransacRandomSeed = defaults.ransacRandomSeed;
	if (hasConfigField(callerStructureSize, offsetof(RobustCoregistrationConfig, ransacMinimumTriangleArea), sizeof(outConfig->ransacMinimumTriangleArea))) outConfig->ransacMinimumTriangleArea = defaults.ransacMinimumTriangleArea;
	return ROBUST_COREGISTRATION_SUCCESS;
}

extern "C" ROBUST_COREGISTRATION_API int FitAffineRobustCoregistration(
	const RobustCoregistrationBlockMatch* matches,
	int matchCount,
	const RobustCoregistrationImageSize* imageSize,
	const RobustCoregistrationConfig* config,
	RobustCoregistrationCancelCallback cancelCallback,
	void* cancelUserData,
	unsigned char* outInlierFlags,
	int outInlierFlagsCapacity,
	RobustCoregistrationResult* outResult,
	char* diagnosticBuffer,
	int diagnosticBufferCapacity)
{
	if (!outResult || outResult->structSize < sizeof(*outResult) || outResult->version != kRobustCoregistrationVersion)
	{
		writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity, "Robust result structure is invalid.");
		return ROBUST_COREGISTRATION_INVALID_ARGUMENT;
	}
	initializeResult(outResult);
	EffectiveConfig effectiveConfig;

	if (!matches || matchCount <= 0 || !outInlierFlags || outInlierFlagsCapacity < matchCount ||
		!validStructure(imageSize, imageSize ? imageSize->structSize : 0, imageSize ? imageSize->version : 0, sizeof(*imageSize)) ||
		!makeEffectiveConfig(config, effectiveConfig) || imageSize->rows <= 0 || imageSize->columns <= 0)
	{
		return fail(outResult, ROBUST_COREGISTRATION_INVALID_ARGUMENT, diagnosticBuffer, diagnosticBufferCapacity, "Robust coregistration input or configuration is invalid.");
	}

	std::memset(outInlierFlags, 0, static_cast<size_t>(matchCount));
	outResult->totalMatchCount = matchCount;

	std::unordered_map<unsigned long long, int> gridToMatch;
	gridToMatch.reserve(static_cast<size_t>(matchCount));
	std::vector<int> eligibleMatches;
	eligibleMatches.reserve(static_cast<size_t>(matchCount));
	std::vector<unsigned char> eligibleFlags(static_cast<size_t>(matchCount), 0);
	for (int index = 0; index < matchCount; ++index)
	{
		if (cancelled(cancelCallback, cancelUserData))
		{
			return fail(outResult, ROBUST_COREGISTRATION_CANCELLED, diagnosticBuffer, diagnosticBufferCapacity, "Robust coregistration cancelled.");
		}

		const RobustCoregistrationBlockMatch& match = matches[index];
		if (!validStructure(&match, match.structSize, match.version, sizeof(match)) || match.blockRow < 0 || match.blockColumn < 0)
		{
			return fail(outResult, ROBUST_COREGISTRATION_INVALID_ARGUMENT, diagnosticBuffer, diagnosticBufferCapacity, "BlockMatch structure or grid coordinate is invalid.");
		}
		const unsigned long long key = gridKey(match.blockRow, match.blockColumn);
		if (!gridToMatch.emplace(key, index).second)
		{
			return fail(outResult, ROBUST_COREGISTRATION_INVALID_ARGUMENT, diagnosticBuffer, diagnosticBufferCapacity, "BlockMatch grid coordinates must be unique.");
		}

		if (match.valid != 0 && isFinite(match.centerRow) && isFinite(match.centerColumn) &&
			isFinite(match.dy) && isFinite(match.dx) && isFinite(match.snr) && match.snr >= effectiveConfig.minimumBlockMatchSnr)
		{
			eligibleMatches.push_back(index);
			eligibleFlags[static_cast<size_t>(index)] = 1;
		}
	}
	outResult->qualityRejectedCount = matchCount - static_cast<int>(eligibleMatches.size());

	std::vector<unsigned char> rejectedByNeighbor(static_cast<size_t>(matchCount), 0);
	for (int index : eligibleMatches)
	{
		if (cancelled(cancelCallback, cancelUserData))
		{
			return fail(outResult, ROBUST_COREGISTRATION_CANCELLED, diagnosticBuffer, diagnosticBufferCapacity, "Robust coregistration cancelled.");
		}

		const RobustCoregistrationBlockMatch& match = matches[index];
		const int neighborRows[4] = { match.blockRow - 1, match.blockRow + 1, match.blockRow, match.blockRow };
		const int neighborColumns[4] = { match.blockColumn, match.blockColumn, match.blockColumn - 1, match.blockColumn + 1 };
		int validNeighbors = 0;
		int anomalyCount = 0;
		for (int neighbor = 0; neighbor < 4; ++neighbor)
		{
			const auto position = gridToMatch.find(gridKey(neighborRows[neighbor], neighborColumns[neighbor]));
			if (position == gridToMatch.end())
			{
				continue;
			}
			const int neighborIndex = position->second;
			if (eligibleFlags[static_cast<size_t>(neighborIndex)] == 0)
			{
				continue;
			}
			++validNeighbors;
			const RobustCoregistrationBlockMatch& neighborMatch = matches[neighborIndex];
			const double delta = std::abs(match.dx - neighborMatch.dx) + std::abs(match.dy - neighborMatch.dy);
			if (delta >= kNeighborDifferenceThreshold)
			{
				++anomalyCount;
			}
		}
		if (validNeighbors >= 2 && anomalyCount >= 2)
		{
			rejectedByNeighbor[static_cast<size_t>(index)] = 1;
			++outResult->neighborRejectedCount;
		}
	}

	std::vector<FitPoint> points;
	points.reserve(eligibleMatches.size());
	const double offsetX = static_cast<double>(imageSize->columns) / 2.0;
	const double offsetY = static_cast<double>(imageSize->rows) / 2.0;
	const double scaleX = static_cast<double>(imageSize->columns);
	const double scaleY = static_cast<double>(imageSize->rows);
	for (int index : eligibleMatches)
	{
		if (rejectedByNeighbor[static_cast<size_t>(index)] != 0)
		{
			continue;
		}
		const RobustCoregistrationBlockMatch& match = matches[index];
		FitPoint point = {};
		point.matchIndex = index;
		point.x = (match.centerColumn - offsetX) / scaleX;
		point.y = (match.centerRow - offsetY) / scaleY;
		point.dy = match.dy;
		point.dx = match.dx;
		points.push_back(point);
	}
	outResult->prefilteredCount = static_cast<int>(points.size());
	if (outResult->prefilteredCount < effectiveConfig.minimumInlierCount)
	{
		writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity, "Only %d points remain after prefiltering; minimum is %d.", outResult->prefilteredCount, effectiveConfig.minimumInlierCount);
		outResult->statusCode = ROBUST_COREGISTRATION_INSUFFICIENT_MATCHES;
		return outResult->statusCode;
	}

	std::vector<FitPoint> fitPoints = points;
	if (effectiveConfig.enableRansac != 0)
	{
		std::vector<FitPoint> consensus;
		const int ransacStatus = selectRansacConsensus(points, effectiveConfig, cancelCallback, cancelUserData, outResult, consensus);
		if (ransacStatus == ROBUST_COREGISTRATION_CANCELLED)
		{
			return fail(outResult, ransacStatus, diagnosticBuffer, diagnosticBufferCapacity, "Robust coregistration cancelled during RANSAC.");
		}
		if (ransacStatus != ROBUST_COREGISTRATION_SUCCESS)
		{
			writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity,
				"RANSAC found no acceptable consensus: candidates=%d, validHypotheses=%d, degenerateHypotheses=%d, iterations=%d, best=%d/%d (%.6f), requiredCount=%d, requiredRatio=%.6f.",
				outResult->ransacCandidateCount, outResult->ransacValidHypothesisCount, outResult->ransacDegenerateHypothesisCount,
				outResult->ransacIterations, outResult->ransacBestConsensusCount, outResult->ransacCandidateCount,
				outResult->ransacBestConsensusRatio, effectiveConfig.minimumInlierCount, effectiveConfig.minimumInlierRatio);
			outResult->statusCode = ROBUST_COREGISTRATION_RANSAC_NO_CONSENSUS;
			return outResult->statusCode;
		}
		fitPoints.swap(consensus);
	}

	std::vector<double> rowValues(fitPoints.size());
	std::vector<double> columnValues(fitPoints.size());
	for (size_t index = 0; index < fitPoints.size(); ++index)
	{
		rowValues[index] = fitPoints[index].dy;
		columnValues[index] = fitPoints[index].dx;
	}
	std::vector<double> weights(fitPoints.size(), 1.0);
	cv::Mat rowCoefficients;
	cv::Mat columnCoefficients;
	if (!solveWeightedAffine(fitPoints, rowValues, weights, rowCoefficients) ||
		!solveWeightedAffine(fitPoints, columnValues, weights, columnCoefficients))
	{
		return fail(outResult, ROBUST_COREGISTRATION_FIT_FAILURE, diagnosticBuffer, diagnosticBufferCapacity, "Initial affine displacement fit failed.");
	}

	for (int iteration = 0; iteration < kMaximumIrlsIterations; ++iteration)
	{
		if (cancelled(cancelCallback, cancelUserData))
		{
			return fail(outResult, ROBUST_COREGISTRATION_CANCELLED, diagnosticBuffer, diagnosticBufferCapacity, "Robust coregistration cancelled.");
		}
		const std::vector<double> residuals = calculateResiduals(fitPoints, rowCoefficients, columnCoefficients);
		const double sigma = std::max(effectiveConfig.residualFloorPixels,
			medianOfValues(residuals) / std::sqrt(2.0 * std::log(2.0)));
		std::vector<double> nextWeights(fitPoints.size(), 1.0);
		double maxWeightChange = 0.0;
		for (size_t index = 0; index < fitPoints.size(); ++index)
		{
			const double normalizedResidual = residuals[index] / sigma;
			nextWeights[index] = normalizedResidual > effectiveConfig.huberCutoffSigma ? effectiveConfig.huberCutoffSigma / normalizedResidual : 1.0;
			maxWeightChange = std::max(maxWeightChange, std::abs(nextWeights[index] - weights[index]));
		}

		cv::Mat nextRowCoefficients;
		cv::Mat nextColumnCoefficients;
		if (!solveWeightedAffine(fitPoints, rowValues, nextWeights, nextRowCoefficients) ||
			!solveWeightedAffine(fitPoints, columnValues, nextWeights, nextColumnCoefficients))
		{
			return fail(outResult, ROBUST_COREGISTRATION_FIT_FAILURE, diagnosticBuffer, diagnosticBufferCapacity, "Robust affine displacement fit failed.");
		}

		const double coefficientChange = std::max(cv::norm(nextRowCoefficients - rowCoefficients), cv::norm(nextColumnCoefficients - columnCoefficients));
		const double coefficientScale = std::max(1.0, std::max(cv::norm(rowCoefficients), cv::norm(columnCoefficients)));
		rowCoefficients = nextRowCoefficients;
		columnCoefficients = nextColumnCoefficients;
		weights.swap(nextWeights);
		outResult->irlsIterations = iteration + 1;
		if (coefficientChange <= coefficientScale * 1e-6 || maxWeightChange <= 1e-3)
		{
			break;
		}
	}

	std::vector<double> finalResiduals;
	double robustSigma = 0.0;
	std::vector<int> inlierRows = selectInliers(fitPoints, rowCoefficients, columnCoefficients, effectiveConfig, finalResiduals, robustSigma);
	bool inlierSetSettled = false;
	for (int pass = 0; pass < kMaximumFinalRefitPasses; ++pass)
	{
		if (cancelled(cancelCallback, cancelUserData))
		{
			return fail(outResult, ROBUST_COREGISTRATION_CANCELLED, diagnosticBuffer, diagnosticBufferCapacity, "Robust coregistration cancelled.");
		}
		cv::Mat refitRowCoefficients;
		cv::Mat refitColumnCoefficients;
		if (!solveInlierFit(fitPoints, inlierRows, refitRowCoefficients, refitColumnCoefficients))
		{
			return fail(outResult, ROBUST_COREGISTRATION_FIT_FAILURE, diagnosticBuffer, diagnosticBufferCapacity, "Final inlier displacement fit has fewer than three usable points or cannot be solved.");
		}
		outResult->finalRefitPasses = pass + 1;
		std::vector<double> refitResiduals;
		double refitSigma = 0.0;
		const std::vector<int> refitInlierRows = selectInliers(fitPoints, refitRowCoefficients, refitColumnCoefficients, effectiveConfig, refitResiduals, refitSigma);
		if (refitInlierRows == inlierRows)
		{
			rowCoefficients = refitRowCoefficients;
			columnCoefficients = refitColumnCoefficients;
			finalResiduals.swap(refitResiduals);
			robustSigma = refitSigma;
			inlierSetSettled = true;
			break;
		}
		inlierRows = refitInlierRows;
	}
	if (!inlierSetSettled)
	{
		return fail(outResult, ROBUST_COREGISTRATION_INLIER_SET_UNSTABLE, diagnosticBuffer, diagnosticBufferCapacity, "Final inlier set did not settle after three passes.");
	}

	outResult->finalInlierCount = static_cast<int>(inlierRows.size());
	outResult->finalInlierRatio = static_cast<double>(outResult->finalInlierCount) / static_cast<double>(points.size());
	if (outResult->finalInlierCount < effectiveConfig.minimumInlierCount || outResult->finalInlierRatio < effectiveConfig.minimumInlierRatio)
	{
		writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity, "Robust fit retained %d/%d final inliers; required count=%d, ratio=%.6f.",
			outResult->finalInlierCount, static_cast<int>(points.size()), effectiveConfig.minimumInlierCount, effectiveConfig.minimumInlierRatio);
		outResult->statusCode = ROBUST_COREGISTRATION_INSUFFICIENT_INLIERS;
		return outResult->statusCode;
	}

	cv::Mat inlierDesign(outResult->finalInlierCount, 3, CV_64F);
	bool occupiedCells[3][3] = {};
	bool occupiedRowBands[3] = {};
	bool occupiedColumnBands[3] = {};
	for (int index = 0; index < outResult->finalInlierCount; ++index)
	{
		const FitPoint& point = fitPoints[inlierRows[index]];
		inlierDesign.at<double>(index, 0) = 1.0;
		inlierDesign.at<double>(index, 1) = point.x;
		inlierDesign.at<double>(index, 2) = point.y;
		const int gridColumn = std::max(0, std::min(2, static_cast<int>((point.x + 0.5) * 3.0)));
		const int gridRow = std::max(0, std::min(2, static_cast<int>((point.y + 0.5) * 3.0)));
		occupiedCells[gridRow][gridColumn] = true;
		occupiedRowBands[gridRow] = true;
		occupiedColumnBands[gridColumn] = true;
	}

	cv::SVD svd(inlierDesign, cv::SVD::NO_UV);
	const double maximumSingularValue = svd.w.at<double>(0, 0);
	const double minimumSingularValue = svd.w.at<double>(svd.w.rows - 1, 0);
	const double rankTolerance = std::numeric_limits<double>::epsilon() * std::max(inlierDesign.rows, inlierDesign.cols) * maximumSingularValue;
	for (int index = 0; index < svd.w.rows; ++index)
	{
		if (svd.w.at<double>(index, 0) > rankTolerance)
		{
			++outResult->designRank;
		}
	}
	outResult->conditionNumber = minimumSingularValue > rankTolerance ? maximumSingularValue / minimumSingularValue : std::numeric_limits<double>::infinity();
	for (int row = 0; row < 3; ++row)
	{
		outResult->occupiedRowBandCount += occupiedRowBands[row] ? 1 : 0;
		outResult->occupiedColumnBandCount += occupiedColumnBands[row] ? 1 : 0;
		for (int column = 0; column < 3; ++column)
		{
			outResult->occupiedGridCellCount += occupiedCells[row][column] ? 1 : 0;
		}
	}
	if (outResult->designRank != 3 || !isFinite(outResult->conditionNumber) ||
		outResult->conditionNumber > effectiveConfig.maximumDesignConditionNumber ||
		outResult->occupiedGridCellCount < effectiveConfig.minimumOccupiedGridCells ||
		outResult->occupiedRowBandCount < 2 || outResult->occupiedColumnBandCount < 2)
	{
		writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity,
			"Robust fit geometry is unstable after RANSAC/IRLS: rank=%d, condition=%.9g, coverage=%d/9, bands=%dx%d, RANSAC best=%d/%d.",
			outResult->designRank, outResult->conditionNumber, outResult->occupiedGridCellCount,
			outResult->occupiedRowBandCount, outResult->occupiedColumnBandCount,
			outResult->ransacBestConsensusCount, outResult->ransacCandidateCount);
		outResult->statusCode = ROBUST_COREGISTRATION_UNSTABLE_GEOMETRY;
		return outResult->statusCode;
	}

	double squaredRowResiduals = 0.0;
	double squaredColumnResiduals = 0.0;
	std::vector<double> finalInlierResiduals;
	finalInlierResiduals.reserve(inlierRows.size());
	for (int inlierRow : inlierRows)
	{
		const FitPoint& point = fitPoints[inlierRow];
		const double rowResidual = point.dy - (rowCoefficients.at<double>(0, 0) + rowCoefficients.at<double>(1, 0) * point.x + rowCoefficients.at<double>(2, 0) * point.y);
		const double columnResidual = point.dx - (columnCoefficients.at<double>(0, 0) + columnCoefficients.at<double>(1, 0) * point.x + columnCoefficients.at<double>(2, 0) * point.y);
		squaredRowResiduals += rowResidual * rowResidual;
		squaredColumnResiduals += columnResidual * columnResidual;
		finalInlierResiduals.push_back(finalResiduals[inlierRow]);
		outInlierFlags[fitPoints[inlierRow].matchIndex] = 1;
	}

	outResult->rowResidualRms = std::sqrt(squaredRowResiduals / static_cast<double>(inlierRows.size()));
	outResult->columnResidualRms = std::sqrt(squaredColumnResiduals / static_cast<double>(inlierRows.size()));
	outResult->residualRms = std::sqrt((squaredRowResiduals + squaredColumnResiduals) / static_cast<double>(inlierRows.size()));
	outResult->residualP95 = percentileOfValues(finalInlierResiduals, 0.95);
	outResult->robustSigma = robustSigma;
	for (int index = 0; index < 3; ++index)
	{
		outResult->rowCoefficients[index] = rowCoefficients.at<double>(index, 0);
		outResult->columnCoefficients[index] = columnCoefficients.at<double>(index, 0);
	}
	outResult->centerDy = outResult->rowCoefficients[0];
	outResult->centerDx = outResult->columnCoefficients[0];
	const double topLeftX = -offsetX / scaleX;
	const double topLeftY = -offsetY / scaleY;
	outResult->topLeftDy = outResult->rowCoefficients[0] + outResult->rowCoefficients[1] * topLeftX + outResult->rowCoefficients[2] * topLeftY;
	outResult->topLeftDx = outResult->columnCoefficients[0] + outResult->columnCoefficients[1] * topLeftX + outResult->columnCoefficients[2] * topLeftY;
	outResult->statusCode = ROBUST_COREGISTRATION_SUCCESS;
	writeDiagnostic(diagnosticBuffer, diagnosticBufferCapacity,
		"Robust fit succeeded: prefiltered=%d, inliers=%d/%d, rms=%.9g, p95=%.9g, rank=%d, condition=%.9g, coverage=%d/9; RANSAC candidates=%d, validHypotheses=%d, degenerateHypotheses=%d, iterations=%d, best=%d (%.6f), threshold=%.9g.",
		outResult->prefilteredCount, outResult->finalInlierCount, outResult->prefilteredCount,
		outResult->residualRms, outResult->residualP95, outResult->designRank,
		outResult->conditionNumber, outResult->occupiedGridCellCount,
		outResult->ransacCandidateCount, outResult->ransacValidHypothesisCount, outResult->ransacDegenerateHypothesisCount,
		outResult->ransacIterations, outResult->ransacBestConsensusCount, outResult->ransacBestConsensusRatio,
		outResult->ransacResidualThresholdPixels);
	return ROBUST_COREGISTRATION_SUCCESS;
}
