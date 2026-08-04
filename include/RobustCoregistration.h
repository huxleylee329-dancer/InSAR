#pragma once

#include <windows.h>

#if defined(REGISTRATION_EXPORTS)
#define ROBUST_COREGISTRATION_API __declspec(dllexport)
#else
#define ROBUST_COREGISTRATION_API __declspec(dllimport)
#endif

#pragma pack(push, 8)

// Versioned C ABI for robust affine displacement fitting. All coordinates are
// zero-based master-image pixel coordinates. The fitted model is
// dy/dx = c0 + c1 * ((column - columns / 2) / columns)
//             + c2 * ((row - rows / 2) / rows). For a master pixel (row,
// column), the corresponding slave sample is (row + dy, column + dx).
// topLeftDy/topLeftDx evaluate the same model at master coordinate (0, 0).
enum RobustCoregistrationStatus
{
	ROBUST_COREGISTRATION_SUCCESS = 0,
	ROBUST_COREGISTRATION_CANCELLED = -2,
	ROBUST_COREGISTRATION_INVALID_ARGUMENT = -100,
	ROBUST_COREGISTRATION_INSUFFICIENT_MATCHES = -101,
	ROBUST_COREGISTRATION_FIT_FAILURE = -102,
	ROBUST_COREGISTRATION_INLIER_SET_UNSTABLE = -103,
	ROBUST_COREGISTRATION_INSUFFICIENT_INLIERS = -104,
	ROBUST_COREGISTRATION_UNSTABLE_GEOMETRY = -105,
	ROBUST_COREGISTRATION_RANSAC_NO_CONSENSUS = -106
};

// The caller owns this array and its order is the stable identity used by the
// inlier output. blockRow/blockColumn identify four-neighbor grid adjacency.
// This version has a fixed array element stride; incompatible extensions use a
// new API instead of changing this structure in place.
struct RobustCoregistrationBlockMatch
{
	unsigned int structSize;
	unsigned int version;
	int blockRow;
	int blockColumn;
	double centerRow;
	double centerColumn;
	double dy;
	double dx;
	double snr;
	int valid;
};

struct RobustCoregistrationImageSize
{
	unsigned int structSize;
	unsigned int version;
	int rows;
	int columns;
};

// The 1A prefix ends immediately before enableRansac. Calls with that older
// structSize remain valid and receive the DLL's default, enabled RANSAC controls.
struct RobustCoregistrationConfig
{
	unsigned int structSize;
	unsigned int version;
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

struct RobustCoregistrationResult
{
	unsigned int structSize;
	unsigned int version;
	int statusCode;
	int totalMatchCount;
	int qualityRejectedCount;
	int neighborRejectedCount;
	int prefilteredCount;
	int finalInlierCount;
	double finalInlierRatio;
	int irlsIterations;
	int finalRefitPasses;
	double rowCoefficients[3];
	double columnCoefficients[3];
	double topLeftDy;
	double topLeftDx;
	double centerDy;
	double centerDx;
	double residualRms;
	double residualP95;
	double rowResidualRms;
	double columnResidualRms;
	double robustSigma;
	int designRank;
	double conditionNumber;
	int occupiedGridCellCount;
	int occupiedRowBandCount;
	int occupiedColumnBandCount;
	int ransacCandidateCount;
	int ransacValidHypothesisCount;
	int ransacDegenerateHypothesisCount;
	int ransacIterations;
	int ransacBestConsensusCount;
	double ransacBestConsensusRatio;
	double ransacResidualThresholdPixels;
};

// Return nonzero from this synchronous callback to request cancellation.
typedef int (__stdcall *RobustCoregistrationCancelCallback)(void* userData);

#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

// Set outConfig->structSize to sizeof(RobustCoregistrationConfig) before
// calling. The function fills all Version 1 fields, including version and the
// default enabled RANSAC controls when the caller provides the extended size.
ROBUST_COREGISTRATION_API int GetDefaultRobustCoregistrationConfig(
	RobustCoregistrationConfig* outConfig
);

// outInlierFlags must point to an array with at least matchCount bytes. The
// function never allocates memory for the caller. diagnosticBuffer is optional
// and receives a NUL-terminated description when supplied. Before this call,
// set structSize/version for every BlockMatch and ImageSize and for outResult.
ROBUST_COREGISTRATION_API int FitAffineRobustCoregistration(
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
	int diagnosticBufferCapacity
);

#ifdef __cplusplus
}
#endif
