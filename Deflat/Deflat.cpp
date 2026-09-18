// Deflat.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include"..\include\Deflat.h"
#include"..\include\FormatConversion.h"
#include "TopsNativeGeometry.h"
#include<direct.h>
#include <mutex>
#include <vector>
#include <cfloat>
#include <atomic>
#include<Windows.h>
#include<SensAPI.h>
#include<urlmon.h>
#include<algorithm>
#include<cmath>
#include<cstdint>
#include<cstdio>
#pragma comment(lib,"URlmon")
#pragma comment(lib, "Sensapi.lib")
#ifdef _DEBUG
#pragma comment(lib, "FormatConversion_d.lib")
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#else
#pragma comment(lib, "FormatConversion.lib")
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#endif // _DEBUG
using namespace cv;

namespace {
	const double NEWTON_CONVERGENCE_THRESHOLD = 0.0000454;

	bool interpolateOrbitPositionAtTime(const Mat& stateVectors, double time, Vec3d& position)
	{
		if (stateVectors.type() != CV_64F || stateVectors.cols != 7 || stateVectors.rows < 2 ||
			!std::isfinite(time)) return false;
		for (int row = 0; row < stateVectors.rows; ++row) {
			if (!std::isfinite(stateVectors.at<double>(row, 0)) ||
				(row > 0 && stateVectors.at<double>(row, 0) <= stateVectors.at<double>(row - 1, 0))) return false;
		}
		if (time < stateVectors.at<double>(0, 0) || time > stateVectors.at<double>(stateVectors.rows - 1, 0)) return false;
		int upper = 1;
		while (upper < stateVectors.rows && stateVectors.at<double>(upper, 0) < time) ++upper;
		if (upper == stateVectors.rows) upper = stateVectors.rows - 1;
		const int lower = std::max(0, upper - 1);
		const double t0 = stateVectors.at<double>(lower, 0);
		const double t1 = stateVectors.at<double>(upper, 0);
		if (t1 <= t0) return false;
		const double h = t1 - t0;
		const double u = (time - t0) / h;
		const double h00 = 2.0 * u * u * u - 3.0 * u * u + 1.0;
		const double h10 = u * u * u - 2.0 * u * u + u;
		const double h01 = -2.0 * u * u * u + 3.0 * u * u;
		const double h11 = u * u * u - u * u;
		for (int coordinate = 0; coordinate < 3; ++coordinate) {
			const double p0 = stateVectors.at<double>(lower, coordinate + 1);
			const double p1 = stateVectors.at<double>(upper, coordinate + 1);
			const double v0 = stateVectors.at<double>(lower, coordinate + 4);
			const double v1 = stateVectors.at<double>(upper, coordinate + 4);
			if (!std::isfinite(p0) || !std::isfinite(p1) || !std::isfinite(v0) || !std::isfinite(v1)) return false;
			position[coordinate] = h00 * p0 + h10 * h * v0 + h01 * p1 + h11 * h * v1;
		}
		return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]);
	}

	bool buildTopsBurstTrack(const Mat& stateVectors, const Mat& burstAzimuthTime,
		const Mat& sourceRowMap, int linesPerBurst, int burstOffset, double lineInterval, Mat& track)
	{
		if (burstAzimuthTime.type() != CV_64F || burstAzimuthTime.cols != 1 || burstAzimuthTime.rows < 1 ||
			linesPerBurst < 1 || !std::isfinite(lineInterval) || lineInterval <= 0.0) return false;
		track.create(sourceRowMap.rows, 3, CV_64F);
		for (int row = 0; row < sourceRowMap.rows; ++row) {
			const int sourceRow = sourceRowMap.at<int>(row, 0);
			const int masterBurst = sourceRow / linesPerBurst;
			const int burst = masterBurst + burstOffset;
			const int lineInBurst = sourceRow % linesPerBurst;
			if (sourceRow < 0 || burst < 0 || burst >= burstAzimuthTime.rows || lineInBurst < 0 ||
				!std::isfinite(burstAzimuthTime.at<double>(burst, 0))) return false;
			Vec3d position;
			const double acquisitionTime = burstAzimuthTime.at<double>(burst, 0) + lineInBurst * lineInterval;
			if (!interpolateOrbitPositionAtTime(stateVectors, acquisitionTime, position)) return false;
			track.at<double>(row, 0) = position[0];
			track.at<double>(row, 1) = position[1];
			track.at<double>(row, 2) = position[2];
		}
		return true;
	}

	bool isValidTopsPhaseMetadata(const TopsBurstPhaseMetadata& metadata)
	{
		const Mat* const matrices[] = {
			&metadata.burstAzimuthTime, &metadata.azimuthFmRateList, &metadata.dcEstimateList,
			&metadata.firstValidLine, &metadata.lastValidLine,
			&metadata.firstValidSample, &metadata.lastValidSample };
		for (const Mat* matrix : matrices) {
			if (!matrix || matrix->empty()) return false;
		}
		const int bursts = metadata.burstAzimuthTime.rows;
		if (metadata.burstAzimuthTime.type() != CV_64F || metadata.burstAzimuthTime.cols != 1 || bursts < 1 ||
			metadata.azimuthFmRateList.type() != CV_64F || metadata.azimuthFmRateList.rows < 1 || metadata.azimuthFmRateList.cols < 5 ||
			metadata.dcEstimateList.type() != CV_64F || metadata.dcEstimateList.rows < 1 || metadata.dcEstimateList.cols < 5 ||
			metadata.firstValidLine.type() != CV_32S || metadata.lastValidLine.type() != CV_32S ||
			metadata.firstValidSample.type() != CV_32S || metadata.lastValidSample.type() != CV_32S ||
			metadata.firstValidLine.rows != bursts || metadata.lastValidLine.rows != bursts ||
			metadata.firstValidSample.rows != bursts || metadata.lastValidSample.rows != bursts ||
			metadata.firstValidLine.cols != 1 || metadata.lastValidLine.cols != 1 ||
			metadata.firstValidSample.cols != 1 || metadata.lastValidSample.cols != 1 ||
			metadata.linesPerBurst < 1 || !std::isfinite(metadata.azimuthSteeringRate) ||
			fabs(metadata.azimuthSteeringRate) <= DBL_EPSILON ||
		!std::isfinite(metadata.rangeSpacing) || metadata.rangeSpacing <= 0.0 ||
		!std::isfinite(metadata.slantRangeFirstPixel) || metadata.slantRangeFirstPixel <= 0.0 ||
		!std::isfinite(metadata.azimuthIntervalSeconds) || metadata.azimuthIntervalSeconds <= 0.0 ||
			!checkRange(metadata.burstAzimuthTime, true, nullptr) ||
			!checkRange(metadata.azimuthFmRateList, true, nullptr) || !checkRange(metadata.dcEstimateList, true, nullptr)) return false;
		for (int burst = 0; burst < bursts; ++burst) {
			const int firstLine = metadata.firstValidLine.at<int>(burst, 0);
			const int lastLine = metadata.lastValidLine.at<int>(burst, 0);
			const int firstSample = metadata.firstValidSample.at<int>(burst, 0);
			const int lastSample = metadata.lastValidSample.at<int>(burst, 0);
			if (firstLine < 1 || lastLine < firstLine || lastLine > metadata.linesPerBurst ||
				firstSample < 0 || lastSample < firstSample) return false;
		}
		return true;
	}

	bool isValidTopsRegistrationReference(const TopsBurstPhaseMetadata& metadata, int outputRows, int outputColumns)
	{
		if (metadata.registrationRerampPhase.type() != CV_64F ||
			metadata.registrationRerampPhase.rows != outputRows ||
			metadata.registrationRerampPhase.cols != outputColumns ||
			!checkRange(metadata.registrationRerampPhase, true, nullptr) ||
			metadata.registrationMappingCoefficients.type() != CV_64F ||
			metadata.registrationMappingCoefficients.rows < 1 ||
			metadata.registrationMappingCoefficients.cols != 6 ||
			!checkRange(metadata.registrationMappingCoefficients, true, nullptr) ||
			metadata.registrationMappingMasterBurstIndices.type() != CV_32S ||
			metadata.registrationMappingMasterBurstIndices.rows != 1 ||
			metadata.registrationMappingMasterBurstIndices.cols != metadata.registrationMappingCoefficients.rows)
			return false;
		for (int row = 0; row < metadata.registrationMappingMasterBurstIndices.cols; ++row) {
			const int burst = metadata.registrationMappingMasterBurstIndices.at<int>(0, row);
			if (burst < 1) return false;
			for (int previous = 0; previous < row; ++previous) {
				if (metadata.registrationMappingMasterBurstIndices.at<int>(0, previous) == burst) return false;
			}
		}
		return true;
	}

	bool isValidTopsRegistrationMapping(const TopsBurstPhaseMetadata& metadata)
	{
		if (metadata.registrationMappingCoefficients.type() != CV_64F ||
			metadata.registrationMappingCoefficients.rows < 1 ||
			metadata.registrationMappingCoefficients.cols != 6 ||
			!checkRange(metadata.registrationMappingCoefficients, true, nullptr) ||
			metadata.registrationMappingMasterBurstIndices.type() != CV_32S ||
			metadata.registrationMappingMasterBurstIndices.rows != 1 ||
			metadata.registrationMappingMasterBurstIndices.cols != metadata.registrationMappingCoefficients.rows)
			return false;
		for (int row = 0; row < metadata.registrationMappingMasterBurstIndices.cols; ++row) {
			const int burst = metadata.registrationMappingMasterBurstIndices.at<int>(0, row);
			if (burst < 1) return false;
			for (int previous = 0; previous < row; ++previous) {
				if (metadata.registrationMappingMasterBurstIndices.at<int>(0, previous) == burst) return false;
			}
		}
		return true;
	}

	bool registrationMappingForMasterBurst(const TopsBurstPhaseMetadata& metadata, int masterBurst,
		double& a0Rg, double& a1Rg, double& a2Rg, double& a0Az, double& a1Az, double& a2Az)
	{
		for (int row = 0; row < metadata.registrationMappingMasterBurstIndices.cols; ++row) {
			if (metadata.registrationMappingMasterBurstIndices.at<int>(0, row) == masterBurst + 1) {
				a0Rg = metadata.registrationMappingCoefficients.at<double>(row, 0);
				a1Rg = metadata.registrationMappingCoefficients.at<double>(row, 1);
				a2Rg = metadata.registrationMappingCoefficients.at<double>(row, 2);
				a0Az = metadata.registrationMappingCoefficients.at<double>(row, 3);
				a1Az = metadata.registrationMappingCoefficients.at<double>(row, 4);
				a2Az = metadata.registrationMappingCoefficients.at<double>(row, 5);
				return true;
			}
		}
		return false;
	}

	int selectTopsPolynomialRow(const Mat& estimates, double burstTime)
	{
		if (burstTime <= estimates.at<double>(0, 0)) return 0;
		for (int row = 1; row < estimates.rows; ++row) {
			if (burstTime <= estimates.at<double>(row, 0)) return row;
		}
		return estimates.rows - 1;
	}

	bool computeTopsDerampDemodPhase(const TopsBurstPhaseMetadata& metadata, const Mat& stateVectors,
		int burst, double lineInBurst, double sample, double timeInterval, double wavelength, double& phase)
	{
		if (burst < 0 || burst >= metadata.burstAzimuthTime.rows || lineInBurst < 0 ||
			lineInBurst >= metadata.linesPerBurst || sample < metadata.firstValidSample.at<int>(burst, 0) ||
			sample > metadata.lastValidSample.at<int>(burst, 0) ||
			lineInBurst + 1.0 < metadata.firstValidLine.at<int>(burst, 0) ||
			lineInBurst + 1.0 > metadata.lastValidLine.at<int>(burst, 0)) return false;
		const double burstTime = metadata.burstAzimuthTime.at<double>(burst, 0);
		const int fmRow = selectTopsPolynomialRow(metadata.azimuthFmRateList, burstTime);
		const int dcRow = selectTopsPolynomialRow(metadata.dcEstimateList, burstTime);
		const auto evaluate = [&](const Mat& estimates, int row, double slantRangeTime) {
			const double delta = slantRangeTime - estimates.at<double>(row, 1);
			return estimates.at<double>(row, 2) + estimates.at<double>(row, 3) * delta +
				estimates.at<double>(row, 4) * delta * delta;
		};
		const double slantRangeTime = 2.0 * (metadata.slantRangeFirstPixel + sample * metadata.rangeSpacing) / VEL_C;
		const double rangeRate = evaluate(metadata.azimuthFmRateList, fmRow, slantRangeTime);
		const double doppler = evaluate(metadata.dcEstimateList, dcRow, slantRangeTime);
		const int referenceSample = metadata.firstValidSample.at<int>(burst, 0);
		const double referenceRangeTime = 2.0 * (metadata.slantRangeFirstPixel + referenceSample * metadata.rangeSpacing) / VEL_C;
		const double referenceRangeRate = evaluate(metadata.azimuthFmRateList, fmRow, referenceRangeTime);
		const double referenceDoppler = evaluate(metadata.dcEstimateList, dcRow, referenceRangeTime);
		const Vec3d velocity(stateVectors.at<double>(0, 4), stateVectors.at<double>(0, 5), stateVectors.at<double>(0, 6));
		const double speed = sqrt(velocity.dot(velocity));
		const double krot = 2.0 * speed * metadata.azimuthSteeringRate * PI / 180.0 / wavelength;
		const double totalRate = rangeRate * krot / (rangeRate - krot);
		const double referenceTime = metadata.linesPerBurst * timeInterval * 0.5 +
			referenceDoppler / referenceRangeRate - doppler / rangeRate;
		const double azimuthTime = lineInBurst * timeInterval;
		phase = -PI * totalRate * pow(azimuthTime - referenceTime, 2.0) - 2.0 * PI * azimuthTime * doppler;
		return std::isfinite(phase) && std::isfinite(rangeRate) && std::isfinite(referenceRangeRate) &&
			std::isfinite(speed) && speed > 0.0 && fabs(rangeRate) > DBL_EPSILON &&
			fabs(referenceRangeRate) > DBL_EPSILON && fabs(rangeRate - krot) > DBL_EPSILON;
	}

	bool isTopsNativeCoordinateInsideValidWindow(const TopsBurstPhaseMetadata& metadata,
		int burst, double lineInBurst, double sample)
	{
		return burst >= 0 && burst < metadata.burstAzimuthTime.rows &&
			std::isfinite(lineInBurst) && std::isfinite(sample) &&
			lineInBurst >= 0.0 && lineInBurst < metadata.linesPerBurst &&
			sample >= metadata.firstValidSample.at<int>(burst, 0) &&
			sample <= metadata.lastValidSample.at<int>(burst, 0) &&
			lineInBurst + 1.0 >= metadata.firstValidLine.at<int>(burst, 0) &&
			lineInBurst + 1.0 <= metadata.lastValidLine.at<int>(burst, 0);
	}
}


template<typename T, typename Predicate>
bool fillInvalidGaps(cv::Mat& mat, Predicate is_invalid,
	DeflatProgressCallback cb = nullptr, int progressStart = 98, int progressEnd = 99);

void logMappedCoordinateDiagnostics(const Mat& mappedLat, const Mat& mappedLon, const char* stage)
{
	uint64_t valid = 0;
	uint64_t missing = 0;
	uint64_t nonFinite = 0;
	uint64_t outOfRange = 0;
	double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;
	for (int row = 0; row < mappedLat.rows; ++row)
	{
		for (int column = 0; column < mappedLat.cols; ++column)
		{
			const double lat = mappedLat.at<double>(row, column);
			const double lon = mappedLon.at<double>(row, column);
			if (!std::isfinite(lat) || !std::isfinite(lon)) { ++nonFinite; continue; }
			if (lat <= -998.0 || lon <= -998.0 || (lat > 350.0 && lon > 350.0)) { ++missing; continue; }
			if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) { ++outOfRange; continue; }
			if (valid == 0)
			{
				minLat = maxLat = lat;
				minLon = maxLon = lon;
			}
			else
			{
				minLat = (std::min)(minLat, lat); maxLat = (std::max)(maxLat, lat);
				minLon = (std::min)(minLon, lon); maxLon = (std::max)(maxLon, lon);
			}
			++valid;
		}
	}
	fprintf(stderr,
		"demMapping(): %s mappedLat=%dx%d mappedLon=%dx%d valid=%llu missing=%llu nonFinite=%llu outOfRange=%llu lat=[%.17g,%.17g] lon=[%.17g,%.17g]\n",
		stage, mappedLat.rows, mappedLat.cols, mappedLon.rows, mappedLon.cols,
		static_cast<unsigned long long>(valid), static_cast<unsigned long long>(missing),
		static_cast<unsigned long long>(nonFinite), static_cast<unsigned long long>(outOfRange),
		minLat, maxLat, minLon, maxLon);
}

Deflat::Deflat()
{
	memset(this->error_head, 0, 256);
	memset(this->parallel_error_head, 0, 256);
	strcpy(this->error_head, "DEFLAT_DLL_ERROR: error happens when using ");
	strcpy(this->parallel_error_head, "DEFLAT_DLL_ERROR: error happens when using parallel computing in function: ");
	latSpacing = 5.0 / 6000.0;
	lonSpacing = 5.0 / 6000.0;
}

Deflat::~Deflat()
{
}

int Deflat::computeSentinel1FlatEarthPhaseCandidate(
	const Mat& stateVec1, const Mat& stateVec2, const Mat& lonCoef, const Mat& latCoef,
	const Mat& sourceRowMap, Mat& pairValidMask,
	const TopsBurstPhaseMetadata& masterTops, const TopsBurstPhaseMetadata& slaveTops,
	int sourceRowCount, int outputColumns, int offsetCol,
	double height, double timeInterval1, double timeInterval2, int slaveSourceBurstOffset, int mode, double wavelength,
	int polynomialDegree, int numberPoints, Mat& flatEarthPhase, FlatEarthModel& model,
	DeflatProgressCallback cb)
{
	if (stateVec1.cols != 7 || stateVec2.cols != 7 || stateVec1.rows < 7 || stateVec2.rows < 7 ||
		stateVec1.type() != CV_64F || stateVec2.type() != CV_64F || lonCoef.type() != CV_64F || latCoef.type() != CV_64F ||
		lonCoef.rows != 1 || latCoef.rows != 1 || lonCoef.cols != 32 || latCoef.cols != 32 ||
		sourceRowMap.type() != CV_32S || sourceRowMap.cols != 1 || sourceRowMap.rows < 1 ||
		pairValidMask.type() != CV_8U || pairValidMask.rows != sourceRowMap.rows || pairValidMask.cols != outputColumns ||
		sourceRowCount < 1 || outputColumns < 1 || height < 0.0 || timeInterval1 <= 0.0 || timeInterval2 <= 0.0 ||
		!isValidTopsPhaseMetadata(masterTops) || !isValidTopsPhaseMetadata(slaveTops) ||
		!isValidTopsRegistrationReference(slaveTops, sourceRowMap.rows, outputColumns) ||
		(mode != TR_MODE_SINGLE_TX_SINGLE_RX && mode != TR_MODE_SINGLE_TX_DOUBLE_RX) || wavelength <= 0.0 ||
		polynomialDegree < 0 || polynomialDegree > 8 || numberPoints < 1)
	{
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): input check failed!\n");
		return -1;
	}

	const int rows = sourceRowMap.rows;
	for (int y = 0; y < rows; ++y) {
		const uchar* valid = pairValidMask.ptr<uchar>(y);
		for (int x = 0; x < outputColumns; ++x) {
			if (valid[x] != 0 && valid[x] != 1) {
				fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): pair-valid mask is not binary!\n");
				return -1;
			}
		}
	}
	const int basisCount = (polynomialDegree + 1) * (polynomialDegree + 1);
	if (numberPoints < basisCount || static_cast<long long>(rows) * outputColumns < basisCount) {
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): insufficient polynomial samples!\n");
		return -1;
	}
	int minSourceRow = sourceRowMap.at<int>(0, 0);
	int maxSourceRow = minSourceRow;
	for (int y = 0; y < rows; ++y) {
		const int sourceRow = sourceRowMap.at<int>(y, 0);
		if (sourceRow < 0 || sourceRow >= sourceRowCount) {
			fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): source-row map is out of range!\n");
			return -1;
		}
		const int masterBurst = sourceRow / masterTops.linesPerBurst;
		const int slaveBurst = masterBurst + slaveSourceBurstOffset;
		if (masterBurst < 0 || masterBurst >= masterTops.burstAzimuthTime.rows ||
			slaveBurst < 0 || slaveBurst >= slaveTops.burstAzimuthTime.rows) {
			fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): common burst mapping is outside TOPS metadata coverage!\n");
			return -1;
		}
		minSourceRow = std::min(minSourceRow, sourceRow);
		maxSourceRow = std::max(maxSourceRow, sourceRow);
	}

	if (sourceRowCount > masterTops.burstAzimuthTime.rows * masterTops.linesPerBurst) {
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): source-row grid exceeds master burst timing!\n");
		return -1;
	}
	Utils util;
	Mat row(rows, outputColumns, CV_64F), col(rows, outputColumns, CV_64F);
	for (int y = 0; y < rows; ++y) {
		for (int x = 0; x < outputColumns; ++x) {
			// The geolocation polynomial belongs to the published, continuous
			// deburst raster.  sourceRowMap is only the native TOPS timing key;
			// using it here would reintroduce each removed burst-overlap gap.
			row.at<double>(y, x) = static_cast<double>(y);
			col.at<double>(y, x) = static_cast<double>(offsetCol + x);
		}
	}
	Mat lon, lat, lonCoefficient = lonCoef.clone(), latCoefficient = latCoef.clone();
	if (util.coord_conversion(lonCoefficient, row, col, lon) != 0 ||
		util.coord_conversion(latCoefficient, row, col, lat) != 0) return -1;

	Mat sate1;
	if (!buildTopsBurstTrack(stateVec1, masterTops.burstAzimuthTime, sourceRowMap, masterTops.linesPerBurst, 0,
		timeInterval1, sate1)) {
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): TOPS burst timing or orbit coverage is invalid!\n");
		return -1;
	}

	Mat geometricPhase(rows, outputColumns, CV_64F);
	std::atomic<bool> cancelled(false);
	std::atomic<bool> invalidProcessingReference(false);
	std::atomic<long long> nativeSupportMasked(0);
	std::atomic<int> completed(0);
	const int progressStep = std::max(1, rows / 100);
#pragma omp parallel for schedule(guided)
	for (int y = 0; y < rows; ++y) {
		if (cancelled) continue;
		Mat point(1, 3, CV_64F), pointXyz;
		for (int x = 0; x < outputColumns; ++x) {
			const int sourceRow = sourceRowMap.at<int>(y, 0);
			const int masterBurst = sourceRow / masterTops.linesPerBurst;
			const int slaveBurst = masterBurst + slaveSourceBurstOffset;
			const int lineInBurst = sourceRow % masterTops.linesPerBurst;
			if (masterBurst < 0 || masterBurst >= masterTops.burstAzimuthTime.rows ||
				slaveBurst < 0 || slaveBurst >= slaveTops.burstAzimuthTime.rows) {
				invalidProcessingReference = true;
				continue;
			}
			double a0Rg = 0.0, a1Rg = 0.0, a2Rg = 0.0, a0Az = 0.0, a1Az = 0.0, a2Az = 0.0;
			if (!registrationMappingForMasterBurst(slaveTops, masterBurst,
				a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az)) {
				invalidProcessingReference = true;
				continue;
			}
			if (!std::isfinite(a0Rg) || !std::isfinite(a1Rg) || !std::isfinite(a2Rg) ||
				!std::isfinite(a0Az) || !std::isfinite(a1Az) || !std::isfinite(a2Az)) {
				invalidProcessingReference = true;
				continue;
			}
			const double outputSample = static_cast<double>(x);
			const double slaveLine = lineInBurst + a0Az + a1Az * outputSample + a2Az * lineInBurst;
			const double slaveSample = outputSample + a0Rg + a1Rg * outputSample + a2Rg * lineInBurst;
			if (!std::isfinite(slaveLine) || !std::isfinite(slaveSample)) {
				invalidProcessingReference = true;
				continue;
			}
			const bool masterNativeCoordinateValid =
				isTopsNativeCoordinateInsideValidWindow(masterTops, masterBurst,
					lineInBurst, offsetCol + x);
			const bool slaveNativeCoordinateValid =
				isTopsNativeCoordinateInsideValidWindow(slaveTops, slaveBurst, slaveLine, slaveSample);
			if (!masterNativeCoordinateValid || !slaveNativeCoordinateValid) {
				// Sinc interpolation can retain non-zero tails beyond Sentinel-1's
				// declared native support, and retained master rows can include the
				// analogous invalid boundary. Neither has a physical TOPS reference.
				if (pairValidMask.at<uchar>(y, x) != 0) {
					pairValidMask.at<uchar>(y, x) = 0;
					nativeSupportMasked.fetch_add(1, std::memory_order_relaxed);
				}
				geometricPhase.at<double>(y, x) = 0.0;
				continue;
			}
			double masterNativeDerampDemod = 0.0;
			double slaveNativeDerampDemod = 0.0;
			if (!computeTopsDerampDemodPhase(masterTops, stateVec1, masterBurst, lineInBurst, offsetCol + x,
				timeInterval1, wavelength, masterNativeDerampDemod) ||
				!computeTopsDerampDemodPhase(slaveTops, stateVec2, slaveBurst, slaveLine, slaveSample,
				timeInterval2, wavelength, slaveNativeDerampDemod)) {
				// The slave native-window condition was checked separately above. Any
				// remaining failure is a metadata, Doppler/FM, or timing contract error.
				invalidProcessingReference = true;
				continue;
			}
			Vec3d slavePosition;
			const double slaveAcquisitionTime = slaveTops.burstAzimuthTime.at<double>(slaveBurst, 0) +
				slaveLine * timeInterval2;
			if (!interpolateOrbitPositionAtTime(stateVec2, slaveAcquisitionTime, slavePosition)) {
				invalidProcessingReference = true;
				continue;
			}
			point.at<double>(0, 0) = lat.at<double>(y, x);
			point.at<double>(0, 1) = lon.at<double>(y, x);
			point.at<double>(0, 2) = height;
			util.ell2xyz(point, pointXyz);
			Mat difference = pointXyz - sate1(Range(y, y + 1), Range(0, 3));
			const double range1 = sqrt(sum(difference.mul(difference))[0]);
			const double dx = pointXyz.at<double>(0, 0) - slavePosition[0];
			const double dy = pointXyz.at<double>(0, 1) - slavePosition[1];
			const double dz = pointXyz.at<double>(0, 2) - slavePosition[2];
			const double range2 = sqrt(dx * dx + dy * dy + dz * dz);
			// The master output is native.  The registered slave applies +d before
			// resampling and -d_r after resampling.  In M * conj(S), that becomes
			// -d(source) + d_r(output).  d_r is the exact field persisted by the
			// registration path rather than a same-coordinate approximation.
			const double slaveDerampApplied = -slaveNativeDerampDemod;
			const double slaveRerampApplied = slaveTops.registrationRerampPhase.at<double>(y, x);
			const double masterOutputProcessingReference = 0.0 * masterNativeDerampDemod;
			const double finalProcessingReference = masterOutputProcessingReference +
				slaveDerampApplied + slaveRerampApplied;
			geometricPhase.at<double>(y, x) = (range2 - range1) * 4.0 * PI / (wavelength * mode) +
				finalProcessingReference;
		}
		const int current = ++completed;
		if (cb && current % progressStep == 0) {
			bool keepGoing = true;
#pragma omp critical(deflat_progress_lock)
			{
				keepGoing = cb(current * 100 / rows, "Computing flat phase reference...");
			}
			if (!keepGoing) cancelled = true;
		}
	}
	if (cancelled) return -2;
	if (invalidProcessingReference) {
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): TOPS deramp/demod reference is invalid!\n");
		return -1;
	}
	if (nativeSupportMasked != 0) {
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): excluded %lld pair samples outside TOPS native support.\n",
			static_cast<long long>(nativeSupportMasked));
	}

	model = FlatEarthModel();
	model.polynomialDegree = polynomialDegree;
	model.numberPoints = numberPoints;
	model.sourceRowCount = sourceRowCount;
	model.rowOrigin = 0.5 * (minSourceRow + maxSourceRow);
	model.rowScale = std::max(1.0, 0.5 * static_cast<double>(maxSourceRow - minSourceRow));
	model.columnOrigin = 0.5 * (outputColumns - 1);
	model.columnScale = std::max(1.0, 0.5 * static_cast<double>(outputColumns - 1));
	const int sampleCount = std::min(numberPoints, rows * outputColumns);
	Mat design(sampleCount, basisCount, CV_64F), values(sampleCount, 1, CV_64F);
	for (int sample = 0; sample < sampleCount; ++sample) {
		const long long flatIndex = sampleCount == 1 ? 0 :
			(static_cast<long long>(sample) * (static_cast<long long>(rows) * outputColumns - 1)) / (sampleCount - 1);
		const int y = static_cast<int>(flatIndex / outputColumns);
		const int x = static_cast<int>(flatIndex % outputColumns);
		const double normalizedRow = (sourceRowMap.at<int>(y, 0) - model.rowOrigin) / model.rowScale;
		const double normalizedColumn = (x - model.columnOrigin) / model.columnScale;
		int term = 0;
		for (int rowPower = 0; rowPower <= polynomialDegree; ++rowPower) {
			const double rowTerm = pow(normalizedRow, rowPower);
			for (int columnPower = 0; columnPower <= polynomialDegree; ++columnPower) {
				design.at<double>(sample, term++) = rowTerm * pow(normalizedColumn, columnPower);
			}
		}
		values.at<double>(sample, 0) = geometricPhase.at<double>(y, x);
	}
	Mat coefficients;
	if (!solve(design, values, coefficients, DECOMP_SVD)) {
		fprintf(stderr, "computeSentinel1FlatEarthPhaseCandidate(): polynomial solve failed!\n");
		return -1;
	}
	model.coefficients = coefficients.t();
	// The polynomial records a compact scene-level diagnostic only.  It must not
	// replace the physical TOPS field: a single global polynomial cannot retain
	// a burst-specific acquisition-time reference across source-row gaps.
	flatEarthPhase = geometricPhase;
	if (cb && !cb(100, "Flat phase reference ready")) return -2;
	return 0;
}

int Deflat::computeSentinel1FlatEarthPhaseV5(
	const TopsFepV5Orbit& masterOrbit,
	const TopsFepV5Orbit& slaveOrbit,
	const TopsBurstPhaseMetadata& masterTops,
	const TopsBurstPhaseMetadata& slaveTops,
	const Mat& sourceRowMap,
	Mat& pairValidMask,
	int sourceRowCount,
	int outputColumns,
	int offsetCol,
	int slaveSourceBurstOffset,
	int mode,
	double wavelength,
	const TopsFepV5Options& options,
	Mat& flatEarthPhase,
	TopsFepV5Provenance& provenance,
	DeflatProgressCallback cb)
{
	if (sourceRowMap.type() != CV_32S || sourceRowMap.cols != 1 || sourceRowMap.rows < 1 ||
		pairValidMask.type() != CV_8U || pairValidMask.rows != sourceRowMap.rows || pairValidMask.cols != outputColumns ||
		sourceRowCount < 1 || outputColumns < 1 || offsetCol < 0 ||
		!isValidTopsPhaseMetadata(masterTops) || !isValidTopsPhaseMetadata(slaveTops) ||
		!isValidTopsRegistrationMapping(slaveTops) ||
		(mode != TR_MODE_SINGLE_TX_SINGLE_RX && mode != TR_MODE_SINGLE_TX_DOUBLE_RX) ||
		!std::isfinite(wavelength) || wavelength <= 0.0 || !std::isfinite(options.epsilonPhase) ||
		options.epsilonPhase <= 0.0 || options.masterLookSide == 0)
	{
		fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): input contract is invalid!\n");
		return -1;
	}
	if (sourceRowCount > masterTops.burstAzimuthTime.rows * masterTops.linesPerBurst)
	{
		fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): source-row grid exceeds master TOPS timing!\n");
		return -1;
	}
	for (int y = 0; y < sourceRowMap.rows; ++y)
	{
		const int sourceRow = sourceRowMap.at<int>(y, 0);
		const int masterBurst = sourceRow / masterTops.linesPerBurst;
		const int slaveBurst = masterBurst + slaveSourceBurstOffset;
		if (sourceRow < 0 || sourceRow >= sourceRowCount || masterBurst < 0 ||
			masterBurst >= masterTops.burstAzimuthTime.rows || slaveBurst < 0 ||
			slaveBurst >= slaveTops.burstAzimuthTime.rows)
		{
			fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): source-row or common burst mapping is invalid!\n");
			return -1;
		}
		const uchar* valid = pairValidMask.ptr<uchar>(y);
		for (int x = 0; x < outputColumns; ++x)
		{
			if (valid[x] != 0 && valid[x] != 1)
			{
				fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): pair-valid mask is not binary!\n");
				return -1;
			}
		}
	}

	const double phaseScale = 4.0 * PI / (wavelength * mode);
	const double deltaRangeBudget = wavelength * mode * options.epsilonPhase / (4.0 * PI);
	TopsFepV5Options geometryOptions = options;
	// Reserve half of the differential-range error budget for the slave
	// zero-Doppler solution. The propagated master RDE bound is checked after
	// both physical ranges are available.
	geometryOptions.maxSlaveRangeError = 0.5 * deltaRangeBudget;
	TopsNativeGeometry geometry(masterOrbit, slaveOrbit, geometryOptions);
	if (!geometry.prepare())
	{
		fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): strict GPS orbit preparation failed!\n");
		return -1;
	}
	const int rows = sourceRowMap.rows;
	std::vector<Position> masterPositions(rows);
	std::vector<Velocity> masterVelocities(rows);
	std::vector<double> masterTimes(rows);
	// `burstAzimuthTime` is a GPS timestamp for native line zero.  The line
	// interval is supplied by the v5 caller in the metadata's physical source
	// contract through `azimuthIntervalSeconds`; use it below after validation.
	// This guard makes an unset interval a whole-task failure rather than a
	// silent zero-time geometry field.
	const double masterAzimuthInterval = masterTops.azimuthIntervalSeconds;
	const double slaveAzimuthInterval = slaveTops.azimuthIntervalSeconds;
	if (!std::isfinite(masterAzimuthInterval) || !std::isfinite(slaveAzimuthInterval) ||
		masterAzimuthInterval <= 0.0 || slaveAzimuthInterval <= 0.0)
	{
		fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): native azimuth intervals are invalid!\n");
		return -1;
	}
	for (int y = 0; y < rows; ++y)
	{
		const int sourceRow = sourceRowMap.at<int>(y, 0);
		const int burst = sourceRow / masterTops.linesPerBurst;
		const int line = sourceRow % masterTops.linesPerBurst;
		const double time = masterTops.burstAzimuthTime.at<double>(burst, 0) + line * masterAzimuthInterval;
		masterTimes[y] = time;
		if (!geometry.masterState(time, masterPositions[y], masterVelocities[y]))
		{
			fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): master orbit does not cover a native burst line!\n");
			return -1;
		}
	}

	flatEarthPhase = Mat::zeros(rows, outputColumns, CV_64F);
	provenance = TopsFepV5Provenance();
	provenance.masterOrbit = masterOrbit;
	provenance.slaveOrbit = slaveOrbit;
	provenance.options = options;
	provenance.burstStatistics.assign(static_cast<size_t>(masterTops.burstAzimuthTime.rows), TopsFepV5BurstStatistics());
	provenance.fixedPointDiagnostics.assign(static_cast<size_t>(masterTops.burstAzimuthTime.rows), TopsFepV5FixedPointDiagnostic());
	std::vector<int> fixedPointRows(masterTops.burstAzimuthTime.rows, -1);
	std::vector<int> fixedPointColumns(masterTops.burstAzimuthTime.rows, -1);
	for (int y = 0; y < rows; ++y)
	{
		const int sourceRow = sourceRowMap.at<int>(y, 0);
		const int burst = sourceRow / masterTops.linesPerBurst;
		const int nativeLine = sourceRow % masterTops.linesPerBurst;
		const int preferredLine = masterTops.linesPerBurst / 2;
		if (burst < 0 || burst >= static_cast<int>(fixedPointRows.size()) ||
			(fixedPointRows[burst] >= 0 &&
			 std::abs(nativeLine - preferredLine) >=
			 std::abs((sourceRowMap.at<int>(fixedPointRows[burst], 0) % masterTops.linesPerBurst) - preferredLine))) continue;
		fixedPointRows[burst] = y;
	}
	for (int burst = 0; burst < static_cast<int>(fixedPointRows.size()); ++burst)
	{
		const int y = fixedPointRows[burst];
		if (y < 0) continue;
		const int center = outputColumns / 2;
		for (int delta = 0; delta < outputColumns; ++delta)
		{
			const int left = center - delta;
			const int right = center + delta;
			if (left >= 0 && pairValidMask.at<uchar>(y, left) == 1) { fixedPointColumns[burst] = left; break; }
			if (right < outputColumns && pairValidMask.at<uchar>(y, right) == 1) { fixedPointColumns[burst] = right; break; }
		}
		if (fixedPointColumns[burst] < 0) continue;
		TopsFepV5FixedPointDiagnostic& diagnostic = provenance.fixedPointDiagnostics[burst];
		diagnostic.outputRow = y;
		diagnostic.outputColumn = fixedPointColumns[burst];
		diagnostic.sourceRow = sourceRowMap.at<int>(y, 0);
		diagnostic.masterBurst = burst + 1;
		diagnostic.slaveBurst = burst + slaveSourceBurstOffset + 1;
		diagnostic.masterTimeGps = masterTimes[y];
	}
	std::atomic<bool> cancelled(false);
	std::atomic<bool> failed(false);
	std::atomic<bool> failureCaptured(false);
	std::atomic<int> completed(0);
	struct NativeGeometryFailureDiagnostic
	{
		int reason = 0;
		int outputRow = -1;
		int outputColumn = -1;
		int sourceRow = -1;
		int masterBurst = -1;
		int slaveBurst = -1;
		double rhoMaster = 0.0;
		double slaveLine = 0.0;
		double slaveSample = 0.0;
		double differentialRangeErrorBound = 0.0;
		double finalGeometryPhaseChange = 0.0;
	};
	NativeGeometryFailureDiagnostic failureDiagnostic;
	const auto captureFailure = [&](int reason, int outputRow, int outputColumn, int sourceRow,
		int masterBurst, int slaveBurst, double rhoMaster, double slaveLine, double slaveSample,
		double differentialRangeErrorBound, double finalGeometryPhaseChange)
	{
		bool expected = false;
		if (failureCaptured.compare_exchange_strong(expected, true))
		{
			failureDiagnostic.reason = reason;
			failureDiagnostic.outputRow = outputRow;
			failureDiagnostic.outputColumn = outputColumn;
			failureDiagnostic.sourceRow = sourceRow;
			failureDiagnostic.masterBurst = masterBurst;
			failureDiagnostic.slaveBurst = slaveBurst;
			failureDiagnostic.rhoMaster = rhoMaster;
			failureDiagnostic.slaveLine = slaveLine;
			failureDiagnostic.slaveSample = slaveSample;
			failureDiagnostic.differentialRangeErrorBound = differentialRangeErrorBound;
			failureDiagnostic.finalGeometryPhaseChange = finalGeometryPhaseChange;
		}
	};
	const int progressStep = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int y = 0; y < rows; ++y)
	{
		if (cancelled || failed) continue;
		const int sourceRow = sourceRowMap.at<int>(y, 0);
		const int masterBurst = sourceRow / masterTops.linesPerBurst;
		const int slaveBurst = masterBurst + slaveSourceBurstOffset;
		const int masterLine = sourceRow % masterTops.linesPerBurst;
		TopsFepV5BurstStatistics local;
		const int fixedPointColumn = fixedPointColumns[masterBurst];
		double a0Rg = 0.0, a1Rg = 0.0, a2Rg = 0.0, a0Az = 0.0, a1Az = 0.0, a2Az = 0.0;
		if (!registrationMappingForMasterBurst(slaveTops, masterBurst, a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az))
		{
			captureFailure(1, y, 0, sourceRow, masterBurst, slaveBurst, 0.0, 0.0, 0.0, 0.0, 0.0);
			failed = true;
			continue;
		}
		Position previousMasterPoint;
		bool hasPreviousMasterPoint = false;
		for (int x = 0; x < outputColumns; ++x)
		{
			TopsFepV5FixedPointDiagnostic* fixedPoint =
				(y == fixedPointRows[masterBurst] && x == fixedPointColumn)
				? &provenance.fixedPointDiagnostics[masterBurst] : nullptr;
			bool fixedPointSlaveStateCaptured = true;
			const double slaveLine = masterLine + a0Az + a1Az * x + a2Az * masterLine;
			const double slaveSample = x + a0Rg + a1Rg * x + a2Rg * masterLine;
			if (fixedPoint) {
				fixedPoint->masterSatelliteX = masterPositions[y].x;
				fixedPoint->masterSatelliteY = masterPositions[y].y;
				fixedPoint->masterSatelliteZ = masterPositions[y].z;
				fixedPoint->masterVelocityX = masterVelocities[y].vx;
				fixedPoint->masterVelocityY = masterVelocities[y].vy;
				fixedPoint->masterVelocityZ = masterVelocities[y].vz;
				fixedPoint->slaveNativeLine = slaveLine;
				fixedPoint->slaveNativeSample = slaveSample;
			}
			if (pairValidMask.at<uchar>(y, x) == 0)
			{
				flatEarthPhase.at<double>(y, x) = 0.0;
				continue;
			}
			if (!isTopsNativeCoordinateInsideValidWindow(masterTops, masterBurst, masterLine, offsetCol + x) ||
				!isTopsNativeCoordinateInsideValidWindow(slaveTops, slaveBurst, slaveLine, slaveSample))
			{
				if (fixedPoint) fixedPoint->status = 2;
				if (pairValidMask.at<uchar>(y, x) != 0) pairValidMask.at<uchar>(y, x) = 0;
				flatEarthPhase.at<double>(y, x) = 0.0;
				++local.nativeSupportMaskedSamples;
				continue;
			}
			Position point;
			TopsNativeGeometry::SampleClosure closure;
			const double rhoMaster = masterTops.slantRangeFirstPixel + (offsetCol + x) * masterTops.rangeSpacing;
			if (fixedPoint) fixedPoint->rhoMaster = rhoMaster;
			if (!geometry.solveMasterH0Point(masterPositions[y], masterVelocities[y], rhoMaster,
				hasPreviousMasterPoint ? &previousMasterPoint : nullptr, point, closure, local))
			{
				if (fixedPoint) fixedPoint->status = 3;
				++local.masterRdeFailureCount;
				captureFailure(2, y, x, sourceRow, masterBurst, slaveBurst, rhoMaster, slaveLine, slaveSample, 0.0, 0.0);
				failed = true;
				break;
			}
			previousMasterPoint = point;
			hasPreviousMasterPoint = true;
			if (fixedPoint) {
				fixedPoint->pointX = point.x;
				fixedPoint->pointY = point.y;
				fixedPoint->pointZ = point.z;
			}
			const double slaveSeed = slaveTops.burstAzimuthTime.at<double>(slaveBurst, 0) + slaveLine * slaveAzimuthInterval;
			if (fixedPoint) fixedPoint->slaveTimeSeedGps = slaveSeed;
			double slaveTime = 0.0;
			double rhoSlave = 0.0;
			if (!geometry.solveSlaveZeroDoppler(point, slaveSeed, slaveTime, rhoSlave, closure, local))
			{
				if (fixedPoint) fixedPoint->status = 4;
				++local.slaveZeroDopplerFailureCount;
				captureFailure(3, y, x, sourceRow, masterBurst, slaveBurst, rhoMaster, slaveLine, slaveSample, 0.0, 0.0);
				failed = true;
				break;
			}
			// Deramp/reramp is an internal interpolation operation.  It restores the
			// registered slave SLC to its native phase convention and therefore must
			// not be counted again as a flat-earth reference term.  Its per-burst
			// phase origin would otherwise be exported as an artificial burst offset.
			flatEarthPhase.at<double>(y, x) = phaseScale * (rhoSlave - rhoMaster);
			const double differentialRangeErrorBound = closure.masterRangeErrorBound + closure.slaveRangeErrorBound;
			const double finalGeometryPhaseChange = phaseScale * (closure.masterPointLastStep + closure.slaveRangeLastStep);
			if (fixedPoint) {
				Position slaveSatellite;
				Velocity slaveVelocity;
				fixedPointSlaveStateCaptured = geometry.slaveState(slaveTime, slaveSatellite, slaveVelocity);
				fixedPoint->slaveTimeGps = slaveTime;
				if (fixedPointSlaveStateCaptured) {
					fixedPoint->slaveSatelliteX = slaveSatellite.x;
					fixedPoint->slaveSatelliteY = slaveSatellite.y;
					fixedPoint->slaveSatelliteZ = slaveSatellite.z;
					fixedPoint->slaveVelocityX = slaveVelocity.vx;
					fixedPoint->slaveVelocityY = slaveVelocity.vy;
					fixedPoint->slaveVelocityZ = slaveVelocity.vz;
				}
				fixedPoint->rhoSlave = rhoSlave;
				fixedPoint->geometryPhase = flatEarthPhase.at<double>(y, x);
				fixedPoint->differentialRangeErrorBound = differentialRangeErrorBound;
				fixedPoint->finalGeometryPhaseChange = finalGeometryPhaseChange;
			}
			if (!std::isfinite(flatEarthPhase.at<double>(y, x)) ||
				!std::isfinite(differentialRangeErrorBound) || differentialRangeErrorBound > deltaRangeBudget ||
				!std::isfinite(finalGeometryPhaseChange) || finalGeometryPhaseChange > options.epsilonPhase ||
				deltaRangeBudget <= 0.0)
			{
				if (fixedPoint) fixedPoint->status = 5;
				++local.closureFailureCount;
				captureFailure(4, y, x, sourceRow, masterBurst, slaveBurst, rhoMaster, slaveLine, slaveSample,
					differentialRangeErrorBound, finalGeometryPhaseChange);
				failed = true;
				break;
			}
			if (fixedPoint) {
				fixedPoint->status = fixedPointSlaveStateCaptured ? 1 : 6;
			}
			local.maxDifferentialRangeErrorBound = std::max(local.maxDifferentialRangeErrorBound, differentialRangeErrorBound);
			local.maxLastGeometryPhaseChange = std::max(local.maxLastGeometryPhaseChange, finalGeometryPhaseChange);
			++local.solvedSamples;
		}
#pragma omp critical(deflat_v5_statistics)
		{
			TopsFepV5BurstStatistics& total = provenance.burstStatistics[masterBurst];
			total.solvedSamples += local.solvedSamples;
			total.nativeSupportMaskedSamples += local.nativeSupportMaskedSamples;
			total.masterRdeFailureCount += local.masterRdeFailureCount;
			total.slaveZeroDopplerFailureCount += local.slaveZeroDopplerFailureCount;
			total.closureFailureCount += local.closureFailureCount;
			total.totalRdeIterations += local.totalRdeIterations;
			total.totalZeroDopplerIterations += local.totalZeroDopplerIterations;
			total.totalSlaveDopplerEvaluations += local.totalSlaveDopplerEvaluations;
			total.maxRdeIterations = std::max(total.maxRdeIterations, local.maxRdeIterations);
			total.maxZeroDopplerIterations = std::max(total.maxZeroDopplerIterations, local.maxZeroDopplerIterations);
			total.maxSlaveDopplerEvaluations = std::max(total.maxSlaveDopplerEvaluations, local.maxSlaveDopplerEvaluations);
			total.maxMasterRangeResidual = std::max(total.maxMasterRangeResidual, local.maxMasterRangeResidual);
			total.maxMasterZeroDopplerResidual = std::max(total.maxMasterZeroDopplerResidual, local.maxMasterZeroDopplerResidual);
			total.maxSlaveZeroDopplerResidual = std::max(total.maxSlaveZeroDopplerResidual, local.maxSlaveZeroDopplerResidual);
			total.maxEllipsoidResidual = std::max(total.maxEllipsoidResidual, local.maxEllipsoidResidual);
			total.maxJacobianCondition = std::max(total.maxJacobianCondition, local.maxJacobianCondition);
			total.maxLastGeometryPhaseChange = std::max(total.maxLastGeometryPhaseChange, local.maxLastGeometryPhaseChange);
			total.maxDifferentialRangeErrorBound = std::max(total.maxDifferentialRangeErrorBound, local.maxDifferentialRangeErrorBound);
			total.maxSlaveSearchHalfWindowSeconds = std::max(total.maxSlaveSearchHalfWindowSeconds, local.maxSlaveSearchHalfWindowSeconds);
			total.maxSlaveSearchExpansions = std::max(total.maxSlaveSearchExpansions, local.maxSlaveSearchExpansions);
			total.masterRdeElapsedMilliseconds += local.masterRdeElapsedMilliseconds;
			total.slaveZeroDopplerElapsedMilliseconds += local.slaveZeroDopplerElapsedMilliseconds;
		}
		const int current = ++completed;
		if (cb && current % progressStep == 0)
		{
			bool keepGoing = true;
#pragma omp critical(deflat_v5_progress)
			{
				keepGoing = cb(current * 100 / rows, "Computing native TOPS flat phase reference...");
			}
			if (!keepGoing) cancelled = true;
		}
	}
	if (cancelled) return -2;
	if (failed)
	{
		fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): physical closure diagnostic: reason=%d, output=(%d,%d), sourceRow=%d, masterBurst=%d, slaveBurst=%d, rhoMaster=%.12g, slaveNative=(%.12g,%.12g), differentialRangeErrorBound=%.12g, finalGeometryPhaseChange=%.12g.\n",
			failureDiagnostic.reason, failureDiagnostic.outputRow, failureDiagnostic.outputColumn,
			failureDiagnostic.sourceRow, failureDiagnostic.masterBurst + 1, failureDiagnostic.slaveBurst + 1,
			failureDiagnostic.rhoMaster, failureDiagnostic.slaveLine, failureDiagnostic.slaveSample,
			failureDiagnostic.differentialRangeErrorBound, failureDiagnostic.finalGeometryPhaseChange);
		fprintf(stderr, "computeSentinel1FlatEarthPhaseV5(): a valid native sample did not satisfy the physical closure!\n");
		return -1;
	}
	if (cb && !cb(100, "Native TOPS flat phase reference ready")) return -2;
	return 0;
}

int Deflat::get_xyz(double aztime, Mat& coef, Mat& pos_xyz)
{
	if (aztime < 0 ||
		coef.rows != 6 ||
		coef.cols != 6 ||
		coef.type() != CV_64F ||
		coef.channels() != 1)
	{
		fprintf(stderr, "get_xyz(): input check failed!\n\n");
		return -1;
	}
	Mat tmp = Mat::ones(1, 6, CV_64F);
	tmp.at<double>(0, 1) = aztime;
	tmp.at<double>(0, 2) = aztime * aztime;
	tmp.at<double>(0, 3) = aztime * aztime * aztime;
	tmp.at<double>(0, 4) = aztime * aztime * aztime * aztime;
	tmp.at<double>(0, 5) = aztime * aztime * aztime * aztime * aztime;
	cv::transpose(tmp, tmp);
	pos_xyz = Mat::zeros(1, 3, CV_64F);
	pos_xyz.at<double>(0, 0) = tmp.dot(coef(Range(0, coef.rows), Range(0, 1)));
	pos_xyz.at<double>(0, 1) = tmp.dot(coef(Range(0, coef.rows), Range(1, 2)));
	pos_xyz.at<double>(0, 2) = tmp.dot(coef(Range(0, coef.rows), Range(2, 3)));
	return 0;
}

int Deflat::get_vel(double aztime, Mat& coef, Mat& vel_xyz)
{
	if (aztime < 0 ||
		coef.rows != 6 ||
		coef.cols != 6 ||
		coef.type() != CV_64F ||
		coef.channels() != 1)
	{
		fprintf(stderr, "get_vel(): input check failed!\n\n");
		return -1;
	}
	Mat tmp = Mat::ones(1, 6, CV_64F);
	tmp.at<double>(0, 1) = aztime;
	tmp.at<double>(0, 2) = aztime * aztime;
	tmp.at<double>(0, 3) = aztime * aztime * aztime;
	tmp.at<double>(0, 4) = aztime * aztime * aztime * aztime;
	tmp.at<double>(0, 5) = aztime * aztime * aztime * aztime * aztime;
	cv::transpose(tmp, tmp);
	vel_xyz = Mat::zeros(1, 3, CV_64F);
	vel_xyz.at<double>(0, 0) = tmp.dot(coef(Range(0, coef.rows), Range(3, 4)));
	vel_xyz.at<double>(0, 1) = tmp.dot(coef(Range(0, coef.rows), Range(4, 5)));
	vel_xyz.at<double>(0, 2) = tmp.dot(coef(Range(0, coef.rows), Range(5, 6)));
	return 0;
}

int Deflat::get_acc(double aztime, Mat& coef, Mat& acc_xyz)
{
	if (aztime < 0 ||
		coef.rows != 6 ||
		coef.cols != 6 ||
		coef.type() != CV_64F ||
		coef.channels() != 1)
	{
		fprintf(stderr, "get_acc(): input check failed!\n\n");
		return -1;
	}
	Mat tmp = Mat::ones(1, 5, CV_64F);
	acc_xyz = Mat::zeros(1, 3, CV_64F);
	tmp.at<double>(0, 0) = 1.0;
	tmp.at<double>(0, 1) = 2.0 * aztime;
	tmp.at<double>(0, 2) = 3.0 * aztime * aztime;
	tmp.at<double>(0, 3) = 4.0 * aztime * aztime * aztime;
	tmp.at<double>(0, 4) = 5.0 * aztime * aztime * aztime * aztime;
	cv::transpose(tmp, tmp);
	acc_xyz.at<double>(0, 0) = tmp.dot(coef(Range(1, 6), Range(3, 4)));
	acc_xyz.at<double>(0, 1) = tmp.dot(coef(Range(1, 6), Range(4, 5)));
	acc_xyz.at<double>(0, 2) = tmp.dot(coef(Range(1, 6), Range(5, 6)));
	return 0;
}

int Deflat::get_satellite_aztime_NEWTON(double center_time, Mat& coef, Mat pos_xyz, double start_time, double* aztime)
{
	if (center_time < 0.0 ||
		coef.rows != 6 ||
		coef.cols != 6 ||
		coef.type() != CV_64F ||
		coef.channels() != 1 ||
		pos_xyz.type() != CV_64F ||
		pos_xyz.rows != 1 ||
		pos_xyz.cols != 3 ||
		start_time > center_time||
		aztime == NULL)
	{
		fprintf(stderr, "get_satellite_aztime_NEWTON(): input check failed!\n\n");
		return -1;
	}
	double init_time = (center_time - start_time) * 100000;
	double sol = 0;
	*aztime = init_time;
	Mat S_xyz, V_xyz, A_xyz, D_xyz;
	int ret;
	bool converged = false;
	for (int iter = 0; iter < 15; iter++)
	{
		ret = get_xyz(*aztime, coef, S_xyz);
		if (return_check(ret, "get_xyz(*, *, *)", error_head)) return -1;
		ret = get_vel(*aztime, coef, V_xyz);
		if (return_check(ret, "get_vel(*, *, *)", error_head)) return -1;
		ret = get_acc(*aztime, coef, A_xyz);
		if (return_check(ret, "get_acc(*, *, *)", error_head)) return -1;
		D_xyz = pos_xyz - S_xyz;
		sol = -(D_xyz.dot(V_xyz)) / (A_xyz.dot(D_xyz) - V_xyz.dot(V_xyz) + 0.000000001);
		*aztime = *aztime + sol;
		if (fabs(sol) < NEWTON_CONVERGENCE_THRESHOLD)
		{
			converged = true;
			break;
		}
		
	}
	if (!converged)
	{
		fprintf(stderr, "get_satellite_aztime_NEWTON(): Newton iteration failed to converge!\n\n");
		return -1;
	}
	return 0;
}

int Deflat::Orbit_Polyfit(const Mat& Orbit, Mat& coef)
{
	if (Orbit.rows < 7 ||
		Orbit.cols != 7 ||
		Orbit.type() != CV_64F ||
		Orbit.channels() != 1)
	{
		fprintf(stderr, "Orbit_Polyfit(): input check failed!\n\n");
		return -1;
	}
	int nr = Orbit.rows;
	coef = Mat::zeros(6, 6, CV_64F);
	double tmp;
	Mat t = (Orbit(Range(0, Orbit.rows), Range(0, 1)) - Orbit.at<double>(0, 0))*100000;
	Mat A = Mat::zeros(nr, 6, CV_64F);
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < 6; j++)
		{
			tmp = t.at<double>(i, 0);
			A.at<double>(i, j) = pow(tmp, j);
		}
	}
	Mat A_t, _coef;
	cv::transpose(A, A_t);
	double ret = invert(A_t * A, A);
	if (fabs(ret) < 1e-12)
	{
		fprintf(stderr, "matrix is singular!\n\n");
		return -1;
	}
	for (int j = 1; j < 7; j++)
	{
		_coef = A * A_t * Orbit(Range(0, nr), Range(j, j + 1));
		for (int i = 0; i < 6; i++)
		{
			coef.at<double>(i, j - 1) = _coef.at<double>(i, 0);
		}
	}
	
	return 0;
}

int Deflat::deflat(
	Mat& phase,
	Mat& phase_deflat,
	Mat& flat_phase,
	const Mat& auxi,
	const Mat& gcps,
	const Mat& orbit_main,
	const Mat& orbit_slave,
	int mode,
	int multilook_times,
	DeflatProgressCallback cb
)
{
	if (phase.cols < 2 ||
		phase.rows < 2 ||
		phase.type() != CV_64F ||
		phase.channels() != 1 ||
		auxi.rows != 1 ||
		auxi.cols != 5 ||
		auxi.type() != CV_64F ||
		auxi.channels() != 1 ||
		(mode == TR_MODE_SINGLE_TX_SINGLE_RX || mode == TR_MODE_SINGLE_TX_DOUBLE_RX) == false ||
		gcps.rows < 2 ||
		gcps.cols != 5 ||
		orbit_main.rows < 1 ||
		orbit_main.cols != 7 ||
		orbit_slave.cols != 7 ||
		orbit_slave.rows < 1 ||
		orbit_main.type() != CV_64F ||
		orbit_main.channels() != 1 ||
		orbit_slave.type() != CV_64F ||
		orbit_slave.channels() != 1||
		multilook_times < 1)
	{
		fprintf(stderr, "deflat(): input check failed!\n\n");
		return -1;
	}
	int ret;
	double C = 2.0 * PI;
	double lambda = VEL_C / (auxi.at<double>(0, 4));
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX) C = 4.0 * PI;
	else
	{
		C = 2.0 * PI;
	}
	Mat gcps_local = gcps;
	if (multilook_times > 1)
	{
		gcps_local = gcps.clone();
		for (int i = 0; i < gcps_local.rows; i++)
		{
			gcps_local.at<double>(i, 0) = gcps_local.at<double>(i, 0) / multilook_times + 1;
			gcps_local.at<double>(i, 1) = gcps_local.at<double>(i, 1) / multilook_times + 1;
		}
	}
	///////////////////////////////////机载无轨道数据情况//////////////////////////////////////////
	if (orbit_main.rows == 1 && orbit_slave.rows == 1 && gcps_local.rows == 2)
	{
		double delta_r1 = cv::norm(orbit_main(Range(0, 1), Range(1, 4)) - gcps_local(Range(0, 1), Range(2, 5))) - 
			cv::norm(orbit_slave(Range(0, 1), Range(1, 4)) - gcps_local(Range(0, 1), Range(2, 5)));
		double delta_r2 = cv::norm(orbit_main(Range(0, 1), Range(1, 4)) - gcps_local(Range(1, 2), Range(2, 5))) -
			cv::norm(orbit_slave(Range(0, 1), Range(1, 4)) - gcps_local(Range(1, 2), Range(2, 5)));
		double a = C * (delta_r2 - delta_r1) / lambda / (gcps_local.at<double>(1, 1) - gcps_local.at<double>(0, 1));
		double b = C * delta_r1 / lambda - a * gcps_local.at<double>(0, 1);
		flat_phase = Mat::zeros(phase.rows, phase.cols, CV_64F);
		int nr = phase.rows;
		int nc = phase.cols;
		std::atomic<bool> cancel_flag(false);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			if (cancel_flag) continue;
			for (int j = 0; j < nc; j++)
			{
				flat_phase.at<double>(i, j) = a * double(i) + b;
			}
		}
		if (cb && !cb(100, "deflat finished")) cancel_flag = true;
		if (cancel_flag) return -2;
		Utils util;
		phase_deflat = phase - flat_phase;
		ret = util.wrap(phase_deflat, phase_deflat);
		if (return_check(ret, "util.wrap(*, *)", error_head)) return -1;
		//ret = util.wrap(flat_phase, flat_phase);
		if (return_check(ret, "util.wrap(*, *)", error_head)) return -1;

	}
	///////////////////////////////////机载无轨道数据情况//////////////////////////////////////////

	///////////////////////////////////星载有轨道数据情况//////////////////////////////////////////
	else
	{
		Mat coef_m, coef_s;
		ret = Orbit_Polyfit(orbit_main, coef_m);
		if (return_check(ret, "Orbit_Polyfit(*, *)", error_head)) return -1;
		ret = Orbit_Polyfit(orbit_slave, coef_s);
		if (return_check(ret, "Orbit_Polyfit(*, *)", error_head)) return -1;
		int N_gcps = 2;
		Mat Satemain_xyz = Mat::zeros(N_gcps, 3, CV_64F);
		Mat Sateslave_xyz = Mat::zeros(N_gcps, 3, CV_64F);
		Mat tmp_xyz = Mat::zeros(1, 3, CV_64F);
		double aztime = 0.0;
		double temp1,temp2;
		Mat tmp;
		for (int i = 0; i < N_gcps; i++)
		{
			temp1 = orbit_main.at<double>(int(orbit_main.rows / 2), 0);
			temp2 = orbit_main.at<double>(0, 0);
			tmp = gcps_local(Range(i, i + 1), Range(2, 5));
			ret = get_satellite_aztime_NEWTON(temp1,coef_m, tmp, temp2, &aztime);
			if (return_check(ret, "get_satellite_aztime_NEWTON(*, *, *, *, *)", error_head)) return -1;
			ret = get_xyz(aztime, coef_m, tmp_xyz);
			if (return_check(ret, "get_xyz(*, *, *)", error_head)) return -1;
			tmp_xyz.copyTo(Satemain_xyz(Range(i, i + 1), Range(0, 3)));

			ret = get_satellite_aztime_NEWTON(orbit_slave.at<double>(int(orbit_slave.rows / 2), 0),coef_s, gcps_local(Range(i, i + 1), Range(2, 5)), orbit_slave.at<double>(0, 0), &aztime);
			if (return_check(ret, "get_satellite_aztime_NEWTON(*, *, *, *, *)", error_head)) return -1;
			ret = get_xyz(aztime, coef_s, tmp_xyz);
			if (return_check(ret, "get_xyz(*, *, *)", error_head)) return -1;
			tmp_xyz.copyTo(Sateslave_xyz(Range(i, i + 1), Range(0, 3)));
		}
		double delta_r1 = cv::norm(Satemain_xyz(Range(0, 1), Range(0, 3)) - gcps_local(Range(0, 1), Range(2, 5))) -
			cv::norm(Sateslave_xyz(Range(0, 1), Range(0, 3)) - gcps_local(Range(0, 1), Range(2, 5)));
		double delta_r2 = cv::norm(Satemain_xyz(Range(1, 2), Range(0, 3)) - gcps_local(Range(1, 2), Range(2, 5))) -
			cv::norm(Sateslave_xyz(Range(1, 2), Range(0, 3)) - gcps_local(Range(1, 2), Range(2, 5)));
		double a = C * (delta_r2 - delta_r1) / lambda / (gcps_local.at<double>(1, 1) - gcps_local.at<double>(0, 1));
		double b = C * delta_r1 / lambda - a * gcps_local.at<double>(0, 1);
		flat_phase = Mat::zeros(phase.rows, phase.cols, CV_64F);
		int nr = phase.rows;
		int nc = phase.cols;
		std::atomic<bool> cancel_flag(false);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			if (cancel_flag) continue;
			for (int j = 0; j < nc; j++)
			{
				flat_phase.at<double>(i, j) = a * double(j) + b;
			}
		}
		if (cb && !cb(100, "deflat finished")) cancel_flag = true;
		if (cancel_flag) return -2;
		Utils util;
		phase_deflat = phase - flat_phase;
		ret = util.wrap(phase_deflat, phase_deflat);
		if (return_check(ret, "util.wrap(*, *)", error_head)) return -1;
		//ret = util.wrap(flat_phase, flat_phase);
		if (return_check(ret, "util.wrap(*, *)", error_head)) return -1;

	}
	///////////////////////////////////星载有轨道数据情况//////////////////////////////////////////
	return 0;
}

int Deflat::deflat(
	const Mat& stateVec1,
	const Mat& stateVec2,
	const Mat& lon_coef,
	const Mat& lat_coef,
	const Mat& phase, 
	int offset_row,
	int offset_col, 
	double height,
	double time_interval, 
	double time_interval2,
	int mode,
	double wave_length,
	Mat& phase_deflated,
	Mat& flat_phase_coef,
	DeflatProgressCallback cb
)
{
	if (stateVec1.cols != 7 ||
		stateVec2.cols != 7 ||
		stateVec1.rows < 7 ||
		stateVec2.rows < 7 ||
		stateVec1.type() != CV_64F ||
		stateVec2.type() != CV_64F ||
		lon_coef.cols != 32 ||
		lat_coef.cols != 32 ||
		lon_coef.rows != 1 ||
		lat_coef.rows != 1 ||
		lon_coef.type() != CV_64F ||
		lat_coef.type() != CV_64F ||
		phase.empty() ||
		phase.type() != CV_64F ||
		phase.cols * phase.rows < 500||
		height < 0.0||
		time_interval < 0.0||
		time_interval2 < 0.0 ||
		mode > TR_MODE_SINGLE_TX_DOUBLE_RX ||
		mode < TR_MODE_SINGLE_TX_SINGLE_RX ||
		wave_length <= 0.0
		)
	{
		fprintf(stderr, "deflat(): input check failed!\n");
		return -1;
	}
	FormatConversion conversion;
	Utils util;
	int ret;
	int rows = phase.rows; int cols = phase.cols;
	/*
	* 轨道插值
	*/
	Mat state_vec1, state_vec2;
	stateVec1.copyTo(state_vec1);
	stateVec2.copyTo(state_vec2);
	ret = util.stateVec_interp(state_vec1, time_interval, state_vec1);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
	ret = util.stateVec_interp(state_vec2, time_interval2, state_vec2);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;

	Mat sate1_xyz, sate2_xyz, sate1_v, sate2_v;
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(1, 4)).copyTo(sate1_xyz);
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(4, 7)).copyTo(sate1_v);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(1, 4)).copyTo(sate2_xyz);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(4, 7)).copyTo(sate2_v);
	/*
	* 图像坐标转经纬坐标
	*/
	Mat row, col;
	row.create(rows, cols, CV_64F); col.create(rows, cols, CV_64F);
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			row.at<double>(i, j) = i + offset_row;
			col.at<double>(i, j) = j + offset_col;
		}
	}
	Mat lon, lat, lon_coefficient, lat_coefficient;
	lon_coef.copyTo(lon_coefficient);
	lat_coef.copyTo(lat_coefficient);
	ret = util.coord_conversion(lon_coefficient, row, col, lon);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	ret = util.coord_conversion(lat_coefficient, row, col, lat);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;


	/*
	* 图像1成像点位置计算
	*/

	Mat sate1 = Mat::zeros(rows, 3, CV_64F);
	Mat sate2 = Mat::zeros(rows, 3, CV_64F);

	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 1) = lon.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 2) = height;
		util.ell2xyz(tmp, xyz);
		
		//找到零多普勒位置
		Mat dop = Mat::zeros(sate1_xyz.rows, 1, CV_64F);
		Mat r;
		for (int j = 0; j < sate1_xyz.rows; j++)
		{
			r = xyz - sate1_xyz(Range(j, j + 1), Range(0, 3));
			dop.at<double>(j, 0) = fabs(cv::sum(r.mul(sate1_v(Range(j, j + 1), Range(0, 3))))[0]);
		}
		Point peak_loc;
		cv::minMaxLoc(dop, NULL, NULL, &peak_loc, NULL);
		//sate1_xyz(Range(peak_loc.y, peak_loc.y + 1), Range(0, 3)).copyTo(sate1(Range(i, i + 1), Range(0, 3)));
		int orbit_idx;
		for (int j = 0; j < rows; j++)
		{
			orbit_idx = (peak_loc.y + j) > (sate1_xyz.rows - 1) ? (sate1_xyz.rows - 1) : (peak_loc.y + j);
			sate1_xyz(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(sate1(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	* 图像2成像点位置计算
	*/

	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 1) = lon.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 2) = height;
		util.ell2xyz(tmp, xyz);

		//找到零多普勒位置
		Mat dop = Mat::zeros(sate2_xyz.rows, 1, CV_64F);
		Mat r;
		for (int j = 0; j < sate2_xyz.rows; j++)
		{
			r = xyz - sate2_xyz(Range(j, j + 1), Range(0, 3));
			dop.at<double>(j, 0) = fabs(cv::sum(r.mul(sate2_v(Range(j, j + 1), Range(0, 3))))[0]);
		}
		Point peak_loc;
		cv::minMaxLoc(dop, NULL, NULL, &peak_loc, NULL);
		//sate2_xyz(Range(peak_loc.y, peak_loc.y + 1), Range(0, 3)).copyTo(sate2(Range(i, i + 1), Range(0, 3)));
		int orbit_idx;
		for (int j = 0; j < rows; j++)
		{
			orbit_idx = (peak_loc.y + j) > (sate2_xyz.rows - 1) ? (sate2_xyz.rows - 1) : (peak_loc.y + j);
			sate2_xyz(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(sate2(Range(j, j + 1), Range(0, 3)));
		}
	}


	/*
	* 计算斜距和平地相位
	*/

	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	std::atomic<int> max_reported_pct(0);
	int step = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag) continue;
		Mat tmp(1, 3, CV_64F); Mat r, xyz; double r1, r2;
		for (int j = 0; j < cols; j++)
		{
			tmp.at<double>(0, 0) = lat.at<double>(i, j);
			tmp.at<double>(0, 1) = lon.at<double>(i, j);
			tmp.at<double>(0, 2) = height;
			util.ell2xyz(tmp, xyz);
			r = xyz - sate1(Range(i, i + 1), Range(0, 3));
			r1 = sqrt(sum(r.mul(r))[0]);//主星斜距
			r = xyz - sate2(Range(i, i + 1), Range(0, 3));
			r2 = sqrt(sum(r.mul(r))[0]);//辅星斜距
			lat.at<double>(i, j) = (r2 - r1) / wave_length * (1 / (double)mode) * 4 * PI;
		}
		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			int current_pct = current * 100 / rows;
			int prev = max_reported_pct.load();
			while (current_pct > prev && !max_reported_pct.compare_exchange_weak(prev, current_pct))
			{
			}
			if (current_pct > prev)
			{
				#pragma omp critical(deflat_progress_lock)
				{
					if (!cb(current_pct, "Computing flat phase..."))
					{
						cancel_flag = true;
					}
				}
			}
		}
	}
	if (cancel_flag) return -2;

	/*
	* 拟合平地相位（二阶拟合），取1/20进行拟合
	*/
	int rows1 = (int)floor(rows * cols / 20);
	Mat A = Mat::ones(rows1, 6, CV_64F); Mat ro(rows1, 1, CV_64F); Mat co(rows1, 1, CV_64F); Mat delta_phi(rows1, 1, CV_64F);
	lat = lat.reshape(0, cols* rows);
	row = row - offset_row; col = col - offset_col;
	row = row.reshape(0, cols * rows); col = col.reshape(0, cols * rows);
	for (int i = 0; i < rows1; i++)
	{
		ro.at<double>(i, 0) = row.at<double>(i * 20, 0);
		co.at<double>(i, 0) = col.at<double>(i * 20, 0);
		delta_phi.at<double>(i, 0) = lat.at<double>(i * 20, 0);
	}
	row.release(); col.release();
	ro.copyTo(A(Range(0, rows1), Range(1, 2)));
	co.copyTo(A(Range(0, rows1), Range(2, 3)));
	Mat tmp = ro.mul(co);
	tmp.copyTo(A(Range(0, rows1), Range(3, 4)));
	tmp = ro.mul(ro);
	tmp.copyTo(A(Range(0, rows1), Range(4, 5)));
	tmp = co.mul(co);
	tmp.copyTo(A(Range(0, rows1), Range(5, 6)));

	Mat coef, A_t, b;
	cv::transpose(A, A_t);
	b = A_t * delta_phi;
	A = A_t * A;
	if (!cv::solve(A, b, coef, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "deflat(): matrix deficiency!");
		return -1;
	}

	//求平地相位
	cv::transpose(coef, coef);
	coef.copyTo(flat_phase_coef);
	lat = lat.reshape(0, rows);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		Mat temp(1, 6, CV_64F);
		for (int j = 0; j < cols; j++)
		{
			temp.at<double>(0, 0) = 1.0;
			temp.at<double>(0, 1) = i;
			temp.at<double>(0, 2) = j;
			temp.at<double>(0, 3) = i * j;
			temp.at<double>(0, 4) = i * i;
			temp.at<double>(0, 5) = j * j;
			lat.at<double>(i, j) = sum(temp.mul(coef))[0];
		}
	}
	lat = phase - lat;
	util.wrap(lat, lat);
	lat.copyTo(phase_deflated);
	return 0;
}


int Deflat::topo_removal(
	const Mat& phase,
	const Mat& dem, 
	const Mat& dem_range_lon,
	const Mat& dem_range_lat, 
	const Mat& stateVec1, 
	const Mat& stateVec2, 
	const Mat& lon_coef,
	const Mat& lat_coef,
	const Mat& inc_coef,
	int offset_row, 
	int offset_col,
	double interp_interval1,
	double interp_interval2,
	int mode,
	double wavelength,
	double B_effect,
	Mat& phase_detopo
)
{
	if (phase.empty() ||
		phase.type() != CV_64F ||
		dem.empty() ||
		dem_range_lon.empty() ||
		dem_range_lat.empty() ||
		dem_range_lat.rows != 1 || dem_range_lat.cols != 2 || dem_range_lon.rows != 1 || dem_range_lon.cols != 2 ||
		dem_range_lat.type() != CV_64F || dem_range_lon.type() != CV_64F ||
		stateVec1.cols != 7 || stateVec1.rows < 7 || stateVec1.type() != CV_64F || stateVec2.cols != 7 || stateVec2.rows < 7 || stateVec2.type() != CV_64F ||
		lon_coef.rows != 1 || lon_coef.cols != 32 || lon_coef.type() != CV_64F || lat_coef.rows != 1 || lat_coef.cols != 32 || lat_coef.type() != CV_64F ||
		interp_interval1 < 0.0 || interp_interval2 < 0.0 ||
		mode > TR_MODE_SINGLE_TX_DOUBLE_RX || mode < TR_MODE_SINGLE_TX_SINGLE_RX || wavelength < 0.0 || inc_coef.type() != CV_64F||inc_coef.rows != 1|| inc_coef.cols != 11
		)
	{
		fprintf(stderr, "topo_removal(): input check failed!\n");
		return -1;
	}

	/*
	* 轨道插值
	*/

	FormatConversion conversion;
	Utils util;
	int ret;
	int rows = phase.rows; int cols = phase.cols;
	/*
	* 轨道插值
	*/
	Mat state_vec1, state_vec2;
	stateVec1.copyTo(state_vec1);
	stateVec2.copyTo(state_vec2);
	ret = util.stateVec_interp(state_vec1, interp_interval1, state_vec1);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
	ret = util.stateVec_interp(state_vec2, interp_interval2, state_vec2);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;

	Mat sate1_xyz, sate2_xyz, sate1_v, sate2_v;
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(1, 4)).copyTo(sate1_xyz);
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(4, 7)).copyTo(sate1_v);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(1, 4)).copyTo(sate2_xyz);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(4, 7)).copyTo(sate2_v);
	/*
	* 图像坐标转经纬坐标
	*/
	Mat row, col;
	row.create(rows, cols, CV_64F); col.create(rows, cols, CV_64F);
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			row.at<double>(i, j) = i + offset_row;
			col.at<double>(i, j) = j + offset_col;
		}
	}
	Mat lon, lat, lon_coefficient, lat_coefficient;
	lon_coef.copyTo(lon_coefficient);
	lat_coef.copyTo(lat_coefficient);
	ret = util.coord_conversion(lon_coefficient, row, col, lon);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	ret = util.coord_conversion(lat_coefficient, row, col, lat);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;

	/*
	* 图像1成像点位置计算
	*/

	Mat sate1 = Mat::zeros(rows, 3, CV_64F);
	Mat sate2 = Mat::zeros(rows, 3, CV_64F);
	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 1) = lon.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 2) = 0;
		util.ell2xyz(tmp, xyz);

		//找到零多普勒位置
		Mat dop = Mat::zeros(sate1_xyz.rows, 1, CV_64F);
		Mat r;
		for (int j = 0; j < sate1_xyz.rows; j++)
		{
			r = xyz - sate1_xyz(Range(j, j + 1), Range(0, 3));
			dop.at<double>(j, 0) = fabs(cv::sum(r.mul(sate1_v(Range(j, j + 1), Range(0, 3))))[0]);
		}
		Point peak_loc;
		cv::minMaxLoc(dop, NULL, NULL, &peak_loc, NULL);
		int orbit_idx;
		for (int j = 0; j < rows; j++)
		{
			orbit_idx = (peak_loc.y + j) > (sate1_xyz.rows - 1) ? (sate1_xyz.rows - 1) : (peak_loc.y + j);
			sate1_xyz(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(sate1(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	* 图像2成像点位置计算
	*/

	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 1) = lon.at<double>(i, (int)cols / 2);
		tmp.at<double>(0, 2) = 0;
		util.ell2xyz(tmp, xyz);

		//找到零多普勒位置
		Mat dop = Mat::zeros(sate2_xyz.rows, 1, CV_64F);
		Mat r;
		for (int j = 0; j < sate2_xyz.rows; j++)
		{
			r = xyz - sate2_xyz(Range(j, j + 1), Range(0, 3));
			dop.at<double>(j, 0) = fabs(cv::sum(r.mul(sate2_v(Range(j, j + 1), Range(0, 3))))[0]);
		}
		Point peak_loc;
		cv::minMaxLoc(dop, NULL, NULL, &peak_loc, NULL);
		int orbit_idx;
		for (int j = 0; j < rows; j++)
		{
			orbit_idx = (peak_loc.y + j) > (sate2_xyz.rows - 1) ? (sate2_xyz.rows - 1) : (peak_loc.y + j);
			sate2_xyz(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(sate2(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	* 根据外部DEM数据计算相应的地形相位
	*/
	double lon_left, lon_right, lat_top, lat_bottom, delta_lon_dem, delta_lat_dem,
		offset_inc, scale_inc, offset_y, scale_y, a0, a1, a2, a3, a4, a5;
	offset_inc = inc_coef.at<double>(0, 0);
	scale_inc = inc_coef.at<double>(0, 1);
	offset_y = inc_coef.at<double>(0, 2);
	scale_y = inc_coef.at<double>(0, 3);
	a0 = inc_coef.at<double>(0, 4);
	a1 = inc_coef.at<double>(0, 5);
	a2 = inc_coef.at<double>(0, 6);
	a3 = inc_coef.at<double>(0, 7);
	a4 = inc_coef.at<double>(0, 8);
	a5 = inc_coef.at<double>(0, 9);
	int dem_rows = dem.rows; int dem_cols = dem.cols;
	lon_left = dem_range_lon.at<double>(0, 0); lon_right = dem_range_lon.at<double>(0, 1);
	lat_top = dem_range_lat.at<double>(0, 1); lat_bottom = dem_range_lat.at<double>(0, 0);
	delta_lon_dem = fabs(lon_right - lon_left) > 180.0 ? (lon_right - lon_left + 360.0) / (double)dem_cols : (lon_right - lon_left) / (double)dem_cols;
	delta_lat_dem = (lat_top - lat_bottom) / (double)dem_rows;
	Mat DEM; DEM.create(lat.size(), CV_64F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat r, xyz; 
		// removed unused: r2 (copy-paste remnant from deflat overload)
		double r1, latitude, longtitude, inc, jj, delta_x, delta_y, upper, lower, dem_interp;
		int m, n, m_p1, n_p1;
		for (int j = 0; j < cols; j++)
		{
			jj = (double)j + (double)offset_col;
			jj = (jj - offset_y) / scale_y;
			inc = a0 + a1 * jj + a2 * jj * jj + a3 * jj * jj * jj + a4 * jj * jj * jj * jj + a5 * jj * jj * jj * jj * jj;
			inc = inc * scale_inc + offset_inc;
			latitude = lat.at<double>(i, j);
			longtitude = lon.at<double>(i, j);
			n = fabs(longtitude - lon_left) > 180.0 ? (int)floor((longtitude - lon_left + 360.0) / delta_lon_dem) : (int)floor((longtitude - lon_left) / delta_lon_dem);
			m = (int)floor((lat_top - latitude) / delta_lat_dem);
			m = m > (dem_rows - 1) ? (dem_rows - 1) : m;
			n = n > (dem_cols - 1) ? (dem_cols - 1) : n;
			m_p1 = m + 1; n_p1 = n + 1;
			m_p1 = m_p1 > (dem_rows - 1) ? (dem_rows - 1) : m_p1;
			n_p1 = n_p1 > (dem_cols - 1) ? (dem_cols - 1) : n_p1;
			delta_x = fabs(delta_lon_dem * (double)n + lon_left) > 180.0 ? (delta_lon_dem * (double)n + lon_left + 360.0) : (delta_lon_dem * (double)n + lon_left);
			delta_x = longtitude - delta_x;
			delta_y = lat_top - (delta_lat_dem * (double)m + latitude);
			upper = (double)dem.at<short>(m, n) + delta_x / delta_lon_dem * ((double)dem.at<short>(m, n_p1) - (double)dem.at<short>(m, n));
			lower = (double)dem.at<short>(m_p1, n) + delta_x / delta_lon_dem * ((double)dem.at<short>(m_p1, n_p1) - (double)dem.at<short>(m_p1, n));
			dem_interp = upper + delta_y / delta_lat_dem * (lower - upper);

			tmp.at<double>(0, 0) = lat.at<double>(i, j);
			tmp.at<double>(0, 1) = lon.at<double>(i, j);
			tmp.at<double>(0, 2) = dem_interp;
			DEM.at<double>(i, j) = dem_interp;
			util.ell2xyz(tmp, xyz);
			r = xyz - sate1(Range(i, i + 1), Range(0, 3));
			r1 = sqrt(sum(r.mul(r))[0]);//主星斜距
			lat.at<double>(i, j) =  4 * PI * dem_interp * B_effect / wavelength / r1 / sin(inc / 180.0 * PI) / ((double)mode);
		}
	}
	lat = phase + lat;
	util.wrap(lat, lat);
	lat.copyTo(phase_detopo);
	return 0;
}

namespace {
	// 线程局部变量存储原始的 cb 回调指针，解决 __stdcall 仿函数 Lambda 转换失败的问题
	std::atomic<DeflatProgressCallback> g_original_cb(nullptr);
	std::mutex g_topography_callback_mutex;
}

// 无捕获的静态回调函数，带有 __stdcall 约定
static bool __stdcall dem_mapping_wrapper_cb(int progress, const char* msg)
{
	DeflatProgressCallback cb = g_original_cb.load(std::memory_order_acquire);
	if (cb)
	{
		return cb(progress * 70 / 100, msg);
	}
	return true;
}

static bool __stdcall topo_sim_wrapper_cb(int progress, const char* msg)
{
	DeflatProgressCallback cb = g_original_cb.load(std::memory_order_acquire);
	if (cb)
	{
		return cb(70 + progress * 30 / 100, msg);
	}
	return true;
}

int Deflat::topography_simulation(
	Mat& topography_phase, 
	Mat& statevector1,
	Mat& statevector2, 
	Mat& lon_cofficient,
	Mat& lat_cofficient,
	Mat& inc_cofficient, 
	double prf1, double prf2, 
	int sceneHeight, int sceneWidth,
	int offset_row, int offset_col,
	double nearRangeTime, double rangeSpacing, double wavelength,
	double acquisition_start_time, double acquisition_stop_time, 
	const char* DEMpath,
	int interp_times,
	DeflatProgressCallback cb
)
{
	if (wavelength < 0.0 ||
		inc_cofficient.type() != CV_64F ||
		inc_cofficient.rows != 1 ||
		inc_cofficient.cols != 11 ||
		nearRangeTime <= 0.0 ||
		prf1 <= 0.0 ||
		prf2 <= 0.0 ||
		wavelength < 0.0 ||
		rangeSpacing < 0.0 ||
		statevector1.type() != CV_64F ||
		statevector1.rows < 5 ||
		statevector1.cols != 7 ||
		statevector2.type() != CV_64F ||
		statevector2.rows < 5 ||
		statevector2.cols != 7 ||
		acquisition_start_time <= 0 ||
		acquisition_stop_time <= 0 ||
		sceneHeight < 1 ||
		sceneWidth < 1 ||
		!DEMpath
		)
	{
		fprintf(stderr, "topography_simulation(): input check failed!\n");
		return -1;
	}
	int ret;
	Mat dem, dem_out;
	Utils util;
	double lon_upperleft, lat_upperleft, lonMax, lonMin, latMax, latMin, B_effect, B_para;
	ret = computeImageGeoBoundry(lat_cofficient, lon_cofficient, sceneHeight, sceneWidth, offset_row, offset_col,
		&lonMax, &latMax, &lonMin, &latMin);
	if (return_check(ret, "computeImageGeoBoundry()", error_head)) return -1;
	ret = getSRTMDEM(DEMpath, dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
	if (return_check(ret, "getSRTMDEM()", error_head)) return -1;

	// 保存原始回调指针到 TLS 变量
	std::lock_guard<std::mutex> callbackLock(g_topography_callback_mutex);
	g_original_cb.store(cb, std::memory_order_release);

	ret = demMapping(dem, dem_out, lon_upperleft, lat_upperleft, offset_row, offset_col,
		sceneHeight, sceneWidth, prf1, rangeSpacing, wavelength,
		nearRangeTime, acquisition_start_time, acquisition_stop_time, statevector1, interp_times,
		5.0 / 6000.0, 5.0 / 6000.0, dem_mapping_wrapper_cb);
	if (ret == -2)
	{
		g_original_cb.store(nullptr, std::memory_order_release);
		return -2;
	}
	if (return_check(ret, "demMapping()", error_head))
	{
		g_original_cb.store(nullptr, std::memory_order_release);
		return -1;
	}
	//Mat out; dem_out.convertTo(out, CV_64F);
	//util.cvmat2bin("E:\\zgb1\\functions\\out.bin", out);
	ret = util.baseline_estimation(statevector1, statevector2, lon_cofficient, lat_cofficient, offset_row,
		offset_col, dem_out.rows, dem_out.cols,
		1 / prf1, 1 / prf2, &B_effect, &B_para);
	if (return_check(ret, "baseline_estimation()", error_head))
	{
		g_original_cb.store(nullptr, std::memory_order_release);
		return -1;
	}

	ret = topography_phase_simulation(dem_out, topography_phase, inc_cofficient, B_effect, nearRangeTime,
		offset_row, offset_col, wavelength, rangeSpacing, topo_sim_wrapper_cb);

	g_original_cb.store(nullptr, std::memory_order_release);
	if (ret == -2) return -2;
	if (return_check(ret, "topography_phase_simulation()", error_head)) return -1;
	return 0;
}

int Deflat::demMapping(
	Mat& DEM84,
	Mat& mappedDEM,
	double lon_upperleft,
	double lat_upperleft,
	int offset_row,
	int offset_col,
	int sceneHeight,
	int sceneWidth,
	double prf, 
	double rangeSpacing,
	double wavelength,
	double nearRangeTime,
	double acquisitionStartTime,
	double acquisitionStopTime,
	Mat& stateVector,
	int interp_times,
	double lon_spacing,
	double lat_spacing,
	DeflatProgressCallback cb
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_16S ||
		sceneHeight < 10 ||
		sceneWidth < 10 ||
		prf <= 0 ||
		wavelength <= 0 ||
		rangeSpacing <= 0 ||
		nearRangeTime <= 0 ||
		acquisitionStartTime <= 0 ||
		acquisitionStopTime <= 0 ||
		lon_spacing <= 0.0 ||
		lat_spacing <= 0.0 ||
		fabs(lon_upperleft) > 180.0 ||
		fabs(lat_upperleft) > 90.0 ||
		stateVector.type() != CV_64F ||
		stateVector.rows < 5 ||
		stateVector.cols != 7
		)
	{
		fprintf(stderr, "demMapping(): input check failed!\n");
		return -1;
	}

	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times = interp_times < 1 ? 1 : interp_times;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times, DEM84.rows * interp_times));
	Mat DEM_out = Mat::zeros(sceneHeight, sceneWidth, CV_16S);
	short invalid = -999;
	DEM_out = DEM_out + invalid;
	lon_spacing = lon_spacing / (double)interp_times;
	lat_spacing = lat_spacing / (double)interp_times;
	//初始化轨道类
	double delta_t = stateVector.at<double>(1, 0) - stateVector.at<double>(0, 0);
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime, delta_t);
	stateVectors.applyOrbit();
	// removed unused: ret (never assigned, return values not checked)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	
	std::atomic<int> completed_items(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, DEM_rows / 100);

	//采用迭代计算每个DEM点在SAR图像中的坐标，以减小计算量
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		for (int j = 0; j < DEM_cols; j++)
		{
			Position groundPosition;
			double lat, lon, height;
			lat = lat_upperleft - (double)i * lat_spacing;
			lon = lon_upperleft + (double)j * lon_spacing;
			lon = lon > 180.0 ? (lon - 360.0) : lon;
			height = DEM.at<short>(i, j);
			Utils::ell2xyz(lon, lat, height, groundPosition);
			double zeroDopplerTime, distance;
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance)) {
				continue;
			}
			int azimuthIndex = cvRound((zeroDopplerTime - acquisitionStartTime) / time_interval);
			int rangeIndex = cvRound((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 1 || rangeIndex < 0 || rangeIndex > sceneWidth - 1)
			{
				
			}
			else
			{
				DEM_out.at<short>(azimuthIndex, rangeIndex) = DEM.at<short>(i, j);
			}
		}

		int current_completed = ++completed_items;
		if (cb && current_completed % step == 0)
		{
			int progress = current_completed * 97 / DEM_rows;
			if (!cb(progress, "Mapping DEM..."))
			{
				cancel_flag = true;
			}
		}
	}
	
	if (cancel_flag)
	{
		return -2; // 提前返回 -2 表示用户中止
	}

	//投影DEM插值
	if (!fillInvalidGaps<short>(DEM_out, [invalid](short val) { return val == invalid; }, cb, 98, 99)) return -2;
	cv::GaussianBlur(DEM_out, mappedDEM, cv::Size(5, 5), 1, 1);
	if (cb && !cb(100, "Mapping DEM completed.")) return -2;
	return 0;
}

int Deflat::demMapping(
	Mat& DEM84,
	Mat& mappedDEM,
	Mat& mappedLat,
	Mat& mappedLon,
	double lon_upperleft, 
	double lat_upperleft, 
	int offset_row,
	int offset_col,
	int sceneHeight,
	int sceneWidth,
	double prf,
	double rangeSpacing, 
	double wavelength,
	double nearRangeTime,
	double acquisitionStartTime,
	double acquisitionStopTime, 
	Mat& stateVector, 
	int interp_times, 
	double lon_spacing,
	double lat_spacing,
	int geocoding_cali_factor_rg,
	int geocoding_cali_factor_az,
	DeflatProgressCallback cb
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_16S ||
		sceneHeight < 10 ||
		sceneWidth < 10 ||
		prf <= 0 ||
		wavelength <= 0 ||
		rangeSpacing <= 0 ||
		nearRangeTime <= 0 ||
		acquisitionStartTime <= 0 ||
		acquisitionStopTime <= 0 ||
		lon_spacing <= 0.0 ||
		lat_spacing <= 0.0 ||
		fabs(lon_upperleft) > 180.0 ||
		fabs(lat_upperleft) > 90.0 ||
		stateVector.type() != CV_64F ||
		stateVector.rows < 5 ||
		stateVector.cols != 7
		)
	{
		fprintf(stderr, "demMapping(): input check failed!\n");
		return -1;
	}

	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times = interp_times < 1 ? 1 : interp_times;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times, DEM84.rows * interp_times));
	Mat DEM_out = Mat::zeros(sceneHeight, sceneWidth, CV_16S);
	//Mat temp_lonlat = Mat::zeros(sceneHeight, sceneWidth, CV_32F);
	mappedLat.create(sceneHeight, sceneWidth, CV_64F); mappedLon.create(sceneHeight, sceneWidth, CV_64F);
	mappedLat = -999.0; mappedLon = -999.0;
	//temp_lonlat = temp_lonlat - 999.0;
	//temp_lonlat.copyTo(mappedLat); temp_lonlat.copyTo(mappedLon);
	short invalid = -999;
	DEM_out = DEM_out + invalid;
	//考虑DEM像素中心与边缘差值
	lat_upperleft = lat_upperleft + lat_spacing / 2.0 - lat_spacing / (double)interp_times * 0.5;
	lon_upperleft = lon_upperleft - lon_spacing / 2.0 + lon_spacing / (double)interp_times * 0.5;
	lon_spacing = lon_spacing / (double)interp_times;
	lat_spacing = lat_spacing / (double)interp_times;
	//初始化轨道类
	double delta_t = stateVector.at<double>(1, 0) - stateVector.at<double>(0, 0);
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime, delta_t);
	stateVectors.applyOrbit();
	// removed unused: ret (never assigned, return values not checked)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	
	std::atomic<int> completed_items(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, DEM_rows / 100);

	//采用迭代计算每个DEM点在SAR图像中的坐标，以减小计算量
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		for (int j = 0; j < DEM_cols; j++)
		{
			Position groundPosition;
			double lat, lon, height;
			lat = lat_upperleft - (double)i * lat_spacing;
			lon = lon_upperleft + (double)j * lon_spacing;
			lon = lon > 180.0 ? (lon - 360.0) : lon;
			height = DEM.at<short>(i, j);
			Utils::ell2xyz(lon, lat, height, groundPosition);
			double zeroDopplerTime, distance;
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance)) {
				continue;
			}

			double azimuthIndexD = (zeroDopplerTime - acquisitionStartTime) / time_interval
				+ geocoding_cali_factor_az - offset_row;

			double rangeIndexD = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing
				+ geocoding_cali_factor_rg - offset_col;

			int azimuthIndex = cvRound(azimuthIndexD);
			int rangeIndex = cvRound(rangeIndexD);

			/*int azimuthIndex = (zeroDopplerTime - acquisitionStartTime) / time_interval + geocoding_cali_factor_az;
			int rangeIndex = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing + geocoding_cali_factor_rg;

			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;*/
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 1 || rangeIndex < 0 || rangeIndex > sceneWidth - 1)
			{

			}
			else
			{
				DEM_out.at<short>(azimuthIndex, rangeIndex) = DEM.at<short>(i, j);
				mappedLon.at<double>(azimuthIndex, rangeIndex) = lon;
				mappedLat.at<double>(azimuthIndex, rangeIndex) = lat;
			}
		}

		int current_completed = ++completed_items;
		if (cb && current_completed % step == 0)
		{
			int progress = current_completed * 97 / DEM_rows;
			if (!cb(progress, "Mapping DEM..."))
			{
				cancel_flag = true;
			}
		}
	}

	if (cancel_flag)
	{
		return -2; // 提前返回 -2 表示用户中止
	}
	//投影DEM插值
	const ULONGLONG gapFillStarted = GetTickCount64();
	fprintf(stderr, "demMapping(): filling DEM gaps rows=%d cols=%d\n", sceneHeight, sceneWidth);
	logMappedCoordinateDiagnostics(mappedLat, mappedLon, "before gap filling");
	if (cb && !cb(98, "Filling DEM gaps...")) return -2;
	if (!fillInvalidGaps<short>(DEM_out, [invalid](short val) { return val == invalid; }, cb, 98, 99)) return -2;
	fprintf(stderr, "demMapping(): DEM gaps finished in %llu ms\n",
		static_cast<unsigned long long>(GetTickCount64() - gapFillStarted));

	//投影经纬度插值
	const ULONGLONG longitudeFillStarted = GetTickCount64();
	fprintf(stderr, "demMapping(): filling longitude gaps rows=%d cols=%d\n", sceneHeight, sceneWidth);
	if (!fillInvalidGaps<double>(mappedLon, [](double val) { return val <= -998.0; }, cb, 98, 99)) return -2;
	fprintf(stderr, "demMapping(): longitude gaps finished in %llu ms\n",
		static_cast<unsigned long long>(GetTickCount64() - longitudeFillStarted));
	const ULONGLONG latitudeFillStarted = GetTickCount64();
	fprintf(stderr, "demMapping(): filling latitude gaps rows=%d cols=%d\n", sceneHeight, sceneWidth);
	if (!fillInvalidGaps<double>(mappedLat, [](double val) { return val <= -998.0; }, cb, 98, 99)) return -2;
	fprintf(stderr, "demMapping(): latitude gaps finished in %llu ms\n",
		static_cast<unsigned long long>(GetTickCount64() - latitudeFillStarted));
	logMappedCoordinateDiagnostics(mappedLat, mappedLon, "after gap filling");
	//投影纬度插值
	const ULONGLONG blurStarted = GetTickCount64();
	fprintf(stderr, "demMapping(): applying Gaussian blur rows=%d cols=%d\n", sceneHeight, sceneWidth);
	if (cb && !cb(99, "Applying Gaussian blur...")) return -2;
	cv::GaussianBlur(mappedLat, mappedLat, cv::Size(5, 5), 1, 1);
	cv::GaussianBlur(mappedLon, mappedLon, cv::Size(5, 5), 1, 1);
	cv::GaussianBlur(DEM_out, mappedDEM, cv::Size(5, 5), 1, 1);
	fprintf(stderr, "demMapping(): Gaussian blur finished in %llu ms\n",
		static_cast<unsigned long long>(GetTickCount64() - blurStarted));
	if (cb && !cb(100, "Mapping DEM completed.")) return -2;
	return 0;
}

int Deflat::demMapping_float(
	Mat& DEM84,
	Mat& mappedDEM,
	double lon_upperleft,
	double lat_upperleft,
	int offset_row,
	int offset_col,
	int sceneHeight,
	int sceneWidth,
	double prf,
	double rangeSpacing,
	double wavelength,
	double nearRangeTime,
	double acquisitionStartTime,
	double acquisitionStopTime,
	Mat& stateVector,
	int interp_times,
	double lon_spacing,
	double lat_spacing,
	DeflatProgressCallback cb
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_32F ||
		sceneHeight < 10 ||
		sceneWidth < 10 ||
		prf <= 0 ||
		wavelength <= 0 ||
		rangeSpacing <= 0 ||
		nearRangeTime <= 0 ||
		acquisitionStartTime <= 0 ||
		acquisitionStopTime <= 0 ||
		lon_spacing <= 0.0 ||
		lat_spacing <= 0.0 ||
		fabs(lon_upperleft) > 180.0 ||
		fabs(lat_upperleft) > 90.0 ||
		stateVector.type() != CV_64F ||
		stateVector.rows < 5 ||
		stateVector.cols != 7
		)
	{
		fprintf(stderr, "demMapping_float(): input check failed!\n");
		return -1;
	}

	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times = interp_times < 1 ? 1 : interp_times;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times, DEM84.rows * interp_times));
	Mat DEM_out = Mat::zeros(sceneHeight, sceneWidth, CV_32F);

	float invalid = -999.0;
	DEM_out = DEM_out + invalid;
	//考虑DEM像素中心与边缘差值
	lat_upperleft = lat_upperleft + lat_spacing / 2.0 - lat_spacing / (double)interp_times * 0.5;
	lon_upperleft = lon_upperleft - lon_spacing / 2.0 + lon_spacing / (double)interp_times * 0.5;
	lon_spacing = lon_spacing / (double)interp_times;
	lat_spacing = lat_spacing / (double)interp_times;
	//初始化轨道类
	double delta_t = stateVector.at<double>(1, 0) - stateVector.at<double>(0, 0);
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime, delta_t);
	stateVectors.applyOrbit();
	/*int ret;*/
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	
	std::atomic<int> completed_items(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, DEM_rows / 100);

	//采用迭代计算每个DEM点在SAR图像中的坐标，以减小计算量
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		for (int j = 0; j < DEM_cols; j++)
		{
			Position groundPosition;
			double lat, lon, height;
			lat = lat_upperleft - (double)i * lat_spacing;
			lon = lon_upperleft + (double)j * lon_spacing;
			lon = lon > 180.0 ? (lon - 360.0) : lon;
			height = DEM.at<float>(i, j);
			Utils::ell2xyz(lon, lat, height, groundPosition);
			double zeroDopplerTime, distance;
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance)) {
				continue;
			}
			int azimuthIndex = cvRound((zeroDopplerTime - acquisitionStartTime) / time_interval);
			int rangeIndex = cvRound((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 1 || rangeIndex < 0 || rangeIndex > sceneWidth - 1)
			{

				
			}
			else
			{
				DEM_out.at<float>(azimuthIndex, rangeIndex) = DEM.at<float>(i, j);
			}
		}

		int current_completed = ++completed_items;
		if (cb && current_completed % step == 0)
		{
			int progress = current_completed * 97 / DEM_rows;
			if (!cb(progress, "Mapping DEM..."))
			{
				cancel_flag = true;
			}
		}
	}

	if (cancel_flag)
	{
		return -2; // 提前返回 -2 表示用户中止
	}
	//DEM_out.copyTo(mappedDEM);
	//return 0;
	//投影DEM插值
	if (!fillInvalidGaps<float>(DEM_out, [](float val) { return val <= -998.0f; }, cb, 98, 99)) return -2;

	cv::GaussianBlur(DEM_out, mappedDEM, cv::Size(5, 5), 1, 1);
	if (cb && !cb(100, "Mapping DEM completed.")) return -2;
	return 0;
}

int Deflat::paraMapping_float(
	Mat& input, 
	Mat& DEM84, 
	Mat& output, 
	double lon_upperleft, 
	double lat_upperleft, 
	int offset_row, int offset_col, int sceneHeight, int sceneWidth, double prf,
	double rangeSpacing, double wavelength, double nearRangeTime,
	double acquisitionStartTime, double acquisitionStopTime, Mat& stateVector,
	int interp_times, double lon_spacing, double lat_spacing, DeflatProgressCallback cb)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_32F ||
		input.empty() ||
		input.type() != CV_32F ||
		input.rows != DEM84.rows ||
		input.cols != DEM84.cols ||
		sceneHeight < 10 ||
		sceneWidth < 10 ||
		prf <= 0 ||
		wavelength <= 0 ||
		rangeSpacing <= 0 ||
		nearRangeTime <= 0 ||
		acquisitionStartTime <= 0 ||
		acquisitionStopTime <= 0 ||
		lon_spacing <= 0.0 ||
		lat_spacing <= 0.0 ||
		fabs(lon_upperleft) > 180.0 ||
		fabs(lat_upperleft) > 90.0 ||
		stateVector.type() != CV_64F ||
		stateVector.rows < 5 ||
		stateVector.cols != 7
		)
	{
		fprintf(stderr, "demMapping_float(): input check failed!\n");
		return -1;
	}

	//84坐标系DEM插值
	Mat DEM, parameter, stateVector_interp;
	interp_times = interp_times < 1 ? 1 : interp_times;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times, DEM84.rows * interp_times));
	cv::resize(input, parameter, cv::Size(DEM84.cols * interp_times, DEM84.rows * interp_times));
	Mat DEM_out = Mat::zeros(sceneHeight, sceneWidth, CV_32F);

	float invalid = -999.0;
	DEM_out = DEM_out + invalid;
	//考虑DEM像素中心与边缘差值
	lat_upperleft = lat_upperleft + lat_spacing / 2.0 - lat_spacing / (double)interp_times * 0.5;
	lon_upperleft = lon_upperleft - lon_spacing / 2.0 + lon_spacing / (double)interp_times * 0.5;
	lon_spacing = lon_spacing / (double)interp_times;
	lat_spacing = lat_spacing / (double)interp_times;
	//初始化轨道类
	double delta_t = stateVector.at<double>(1, 0) - stateVector.at<double>(0, 0);
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime, delta_t);
	stateVectors.applyOrbit();
	/*int ret;*/
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	//采用迭代计算每个DEM点在SAR图像中的坐标，以减小计算量
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	int step = std::max(1, DEM_rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
		if (cancel_flag) continue;
		for (int j = 0; j < DEM_cols; j++)
		{
			if (cancel_flag) break;
			Position groundPosition;
			double lat, lon, height;
			lat = lat_upperleft - (double)i * lat_spacing;
			lon = lon_upperleft + (double)j * lon_spacing;
			lon = lon > 180.0 ? (lon - 360.0) : lon;
			height = DEM.at<float>(i, j);
			Utils::ell2xyz(lon, lat, height, groundPosition);
			double zeroDopplerTime, distance;
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance)) {
				continue;
			}
			int azimuthIndex = cvRound((zeroDopplerTime - acquisitionStartTime) / time_interval);
			int rangeIndex = cvRound((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 1 || rangeIndex < 0 || rangeIndex > sceneWidth - 1)
			{

			}
			else
			{
				DEM_out.at<float>(azimuthIndex, rangeIndex) = parameter.at<float>(i, j);
			}
		}
		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 97 / DEM_rows, "Mapping parameters..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	//DEM_out.copyTo(mappedDEM);
	//return 0;
	//投影DEM插值
	if (!fillInvalidGaps<float>(DEM_out, [](float val) { return val <= -998.0f; }, cb, 98, 99)) return -2;

	cv::GaussianBlur(DEM_out, output, cv::Size(5, 5), 1, 1);
	if (cb && !cb(100, "Mapping parameters completed.")) return -2;
	return 0;
}

int Deflat::SLC_deramp(ComplexMat& slc, Mat& mappedDEM, Mat& mappedLat, Mat& mappedLon, const char* slcH5File, int mode, DeflatProgressCallback cb)
{
	if (mappedDEM.rows != mappedLat.rows ||
		mappedDEM.rows != mappedLon.rows ||
		mappedDEM.cols != mappedLat.cols ||
		mappedDEM.cols != mappedLon.cols ||
		mappedDEM.type() != CV_16S ||
		mappedLat.type() != CV_64F ||
		mappedLon.type() != CV_64F ||
		mappedDEM.empty() ||
		!slcH5File
		)
	{
		fprintf(stderr, "SLC_deramp(): input check failed!\n");
		return -1;
	}
	FormatConversion conversion; Deflat flat; Utils util;
	int ret;
	// removed unused: lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing, nearRangeTime (commented-out H5 reads)
	double wavelength, prf, start, end;
	// removed unused: offset_row, offset_col (commented-out H5 reads)
	int sceneHeight, sceneWidth;
	Mat lon_coef, lat_coef, statevec;
	string start_time, end_time;
	ret = conversion.read_int_from_h5(slcH5File, "range_len", &sceneWidth);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(slcH5File, "azimuth_len", &sceneHeight);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	//ret = conversion.read_int_from_h5(slcH5File, "offset_row", &offset_row);
	//if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	//ret = conversion.read_int_from_h5(slcH5File, "offset_col", &offset_col);
	//if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	//ret = conversion.read_array_from_h5(slcH5File, "lon_coefficient", lon_coef);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	//ret = conversion.read_array_from_h5(slcH5File, "lat_coefficient", lat_coef);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(slcH5File, "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(slcH5File, "carrier_frequency", &wavelength);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	wavelength = VEL_C / wavelength;
	//ret = conversion.read_double_from_h5(slcH5File, "range_spacing", &rangeSpacing);
	//if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	//ret = conversion.read_double_from_h5(slcH5File, "slant_range_first_pixel", &nearRangeTime);
	//if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	//nearRangeTime = 2.0 * nearRangeTime / VEL_C;
	ret = conversion.read_str_from_h5(slcH5File, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(slcH5File, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(slcH5File, "state_vec", statevec);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_slc_from_h5(slcH5File, slc);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (slc.type() != CV_32F) slc.convertTo(slc, CV_32F);
	Mat sate1 = Mat::zeros(sceneHeight, 3, CV_64F);
	double delta_t = statevec.at<double>(1, 0) - statevec.at<double>(0, 0);
	orbitStateVectors stateVectors(statevec, start, end, delta_t);
	stateVectors.applyOrbit();

	double dopplerFrequency = 0.0;

	Position groundPosition;
	double lat, lon, height;
	lat = mappedLat.at<double>(0, 0);
	lon = mappedLon.at<double>(0, 0);
	lon = lon > 180.0 ? (lon - 360.0) : lon;
	height = mappedDEM.at<short>(0, 0);
	Utils::ell2xyz(lon, lat, height, groundPosition);
	double zeroDopplerTime, distance;
	if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, 1.0 / prf, dopplerFrequency, zeroDopplerTime, distance)) {
		fprintf(stderr, "SLC_deramp(): orbit mismatch!\n");
		return -1;
	}

	Position pos;
	for (int i = 0; i < sceneHeight; i++)
	{
		double time = zeroDopplerTime + (double)i * (1.0 / prf);
		stateVectors.getPosition(time, pos);
		sate1.at<double>(i, 0) = pos.x;
		sate1.at<double>(i, 1) = pos.y;
		sate1.at<double>(i, 2) = pos.z;
	}
	double constant = (mode == TR_MODE_SINGLE_TX_SINGLE_RX ? 4.0 * PI : 2.0 * PI);

	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	int step = std::max(1, sceneHeight / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < sceneHeight; i++)
	{
		if (cancel_flag) continue;
		for (int j = 0; j < sceneWidth; j++)
		{
			double r, real, imagine, real2, imagine2; // used for complex phase deramping
			Mat XYZ, LLH(1, 3, CV_64F), tt;
			LLH.at<double>(0, 0) = mappedLat.at<double>(i, j);
			LLH.at<double>(0, 1) = mappedLon.at<double>(i, j);
			LLH.at<double>(0, 2) = mappedDEM.at<short>(i, j);
			util.ell2xyz(LLH, XYZ);
			tt = XYZ - sate1(cv::Range(i, i + 1), cv::Range(0, 3));
			r = cv::norm(tt, cv::NORM_L2);
			r = -r / wavelength * constant;
			real = cos(r);
			imagine = sin(r);
			real2 = slc.re.at<float>(i, j);
			imagine2 = slc.im.at<float>(i, j);
			slc.re.at<float>(i, j) = static_cast<float>(real * real2 + imagine * imagine2);
			slc.im.at<float>(i, j) = static_cast<float>(real * imagine2 - real2 * imagine);
		}
		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / sceneHeight, "SLC phase deramping..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Deflat::slantrange_compute_test(Mat& slant_range, Mat& mappedDEM, Mat& mappedLat, Mat& mappedLon, const char* slcH5File, int mode, DeflatProgressCallback cb)
{
	if (mappedDEM.rows != mappedLat.rows ||
		mappedDEM.rows != mappedLon.rows ||
		mappedDEM.cols != mappedLat.cols ||
		mappedDEM.cols != mappedLon.cols ||
		mappedDEM.type() != CV_16S ||
		mappedLat.type() != CV_64F ||
		mappedLon.type() != CV_64F ||
		mappedDEM.empty() ||
		!slcH5File
		)
	{
		fprintf(stderr, "slantrange_compute_test(): input check failed!\n");
		return -1;
	}
	FormatConversion conversion; Deflat flat; Utils util;
	int ret;
	double /*lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing,
		nearRangeTime, */wavelength, prf, start, end;
	int sceneHeight, sceneWidth/*, offset_row, offset_col*/;
	Mat lon_coef, lat_coef, statevec;
	string start_time, end_time;
	ret = conversion.read_int_from_h5(slcH5File, "range_len", &sceneWidth);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(slcH5File, "azimuth_len", &sceneHeight);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(slcH5File, "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(slcH5File, "carrier_frequency", &wavelength);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	wavelength = VEL_C / wavelength;
	ret = conversion.read_str_from_h5(slcH5File, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(slcH5File, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(slcH5File, "state_vec", statevec);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;

	Mat sate1 = Mat::zeros(sceneHeight, 3, CV_64F);
	slant_range.create(sceneHeight, sceneWidth, CV_64F);
	double delta_t = statevec.at<double>(1, 0) - statevec.at<double>(0, 0);
	orbitStateVectors stateVectors(statevec, start, end, delta_t);
	stateVectors.applyOrbit();
	Position pos;
	for (int i = 0; i < sceneHeight; i++)
	{
		double dopplerFrequency = 0.0;

		Position groundPosition;
		double lat, lon, height;
		lat = mappedLat.at<double>(i, (int)sceneWidth / 2);
		lon = mappedLon.at<double>(i, (int)sceneWidth / 2);
		lon = lon > 180.0 ? (lon - 360.0) : lon;
		height = mappedDEM.at<short>(i, (int)sceneWidth / 2);
		Utils::ell2xyz(lon, lat, height, groundPosition);
		double zeroDopplerTime, distance;
		if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, 1.0 / prf, dopplerFrequency, zeroDopplerTime, distance)) {
			fprintf(stderr, "slantrange_compute_test(): orbit mismatch!\n");
			return -1;
		}
		double time = zeroDopplerTime;
		stateVectors.getPosition(time, pos);
		sate1.at<double>(i, 0) = pos.x;
		sate1.at<double>(i, 1) = pos.y;
		sate1.at<double>(i, 2) = pos.z;
	}

	double constant = (mode == TR_MODE_SINGLE_TX_SINGLE_RX ? 4.0 * PI : 2.0 * PI);

	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	int step = std::max(1, sceneHeight / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < sceneHeight; i++)
	{
		if (cancel_flag) continue;
		for (int j = 0; j < sceneWidth; j++)
		{
			double r;
			Mat XYZ, LLH(1, 3, CV_64F), tt;
			LLH.at<double>(0, 0) = mappedLat.at<double>(i, j);
			LLH.at<double>(0, 1) = mappedLon.at<double>(i, j);
			LLH.at<double>(0, 2) = mappedDEM.at<short>(i, j);
			util.ell2xyz(LLH, XYZ);
			tt = XYZ - sate1(cv::Range(i, i + 1), cv::Range(0, 3));
			r = cv::norm(tt, cv::NORM_L2);
			slant_range.at<double>(i, j) = r;
		}
		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / sceneHeight, "Computing slant ranges..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Deflat::slantrange_compute(Mat& slant_range, Mat& sate_pos, Mat& sate_vel, Mat& mappedDEM, Mat& mappedLat, Mat& mappedLon, const char* slcH5File, DeflatProgressCallback cb)
{
	if (mappedDEM.rows != mappedLat.rows ||
		mappedDEM.rows != mappedLon.rows ||
		mappedDEM.cols != mappedLat.cols ||
		mappedDEM.cols != mappedLon.cols ||
		mappedDEM.type() != CV_16S ||
		mappedLat.type() != CV_64F ||
		mappedLon.type() != CV_64F ||
		mappedDEM.empty() ||
		!slcH5File
		)
	{
		fprintf(stderr, "slantrange_compute(): input check failed!\n");
		return -1;
	}
	FormatConversion conversion; Deflat flat; Utils util;
	int ret;
	// removed unused: lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing, nearRangeTime (commented-out H5 reads)
	double wavelength, prf, start, end;
	// removed unused: offset_row, offset_col (commented-out H5 reads)
	int sceneHeight, sceneWidth;
	Mat lon_coef, lat_coef, statevec;
	string start_time, end_time;
	ret = conversion.read_int_from_h5(slcH5File, "range_len", &sceneWidth);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(slcH5File, "azimuth_len", &sceneHeight);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(slcH5File, "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(slcH5File, "carrier_frequency", &wavelength);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	wavelength = VEL_C / wavelength;
	ret = conversion.read_str_from_h5(slcH5File, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(slcH5File, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(slcH5File, "state_vec", statevec);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;

	slant_range.create(sceneHeight, sceneWidth, CV_64F);
	sate_pos.create(sceneHeight, 3, CV_64F);
	sate_vel.create(sceneHeight, 3, CV_64F);
	double delta_t = statevec.at<double>(1, 0) - statevec.at<double>(0, 0);
	orbitStateVectors stateVectors(statevec, start, end, delta_t);
	stateVectors.applyOrbit();

	double dopplerFrequency = 0.0;

	Position groundPosition;
	double lat, lon, height;
	lat = mappedLat.at<double>(0, 0);
	lon = mappedLon.at<double>(0, 0);
	lon = lon > 180.0 ? (lon - 360.0) : lon;
	height = mappedDEM.at<short>(0, 0);
	Utils::ell2xyz(lon, lat, height, groundPosition);
	double zeroDopplerTime, distance;
	if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, 1.0 / prf, dopplerFrequency, zeroDopplerTime, distance)) {
		fprintf(stderr, "slantrange_compute(): orbit mismatch!\n");
		return -1;
	}

	Position pos; Velocity vel;
	for (int i = 0; i < sceneHeight; i++)
	{
		double time = zeroDopplerTime + (double)i * (1.0 / prf);
		stateVectors.getPosition(time, pos);
		stateVectors.getVelocity(time, vel);
		sate_pos.at<double>(i, 0) = pos.x;
		sate_pos.at<double>(i, 1) = pos.y;
		sate_pos.at<double>(i, 2) = pos.z;
		sate_vel.at<double>(i, 0) = vel.vx;
		sate_vel.at<double>(i, 1) = vel.vy;
		sate_vel.at<double>(i, 2) = vel.vz;
	}
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	int step = std::max(1, sceneHeight / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < sceneHeight; i++)
	{
		if (cancel_flag) continue;
		for (int j = 0; j < sceneWidth; j++)
		{
			// removed unused: real, imagine, real2, imagine2 (copy-paste from SLC_deramp, no complex math here)
			double r;
			Mat XYZ, LLH(1, 3, CV_64F), tt;
			LLH.at<double>(0, 0) = mappedLat.at<double>(i, j);
			LLH.at<double>(0, 1) = mappedLon.at<double>(i, j);
			LLH.at<double>(0, 2) = mappedDEM.at<short>(i, j);
			util.ell2xyz(LLH, XYZ);
			tt = XYZ - sate_pos(cv::Range(i, i + 1), cv::Range(0, 3));
			r = cv::norm(tt, cv::NORM_L2);
			slant_range.at<double>(i, j) = r;
		}
		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / sceneHeight, "Computing slant ranges..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Deflat::SLCs_deramp(
	vector<string>& SLCH5Files,
	int reference,
	const char* demPath,
	vector<string>& outSLCH5Files,
	DeflatProgressCallback cb
)
{
	if (SLCH5Files.size() < 1 ||
		reference < 1 || reference > SLCH5Files.size() ||
		!demPath ||
		outSLCH5Files.size() != SLCH5Files.size()
		)
	{
		fprintf(stderr, "SLCs_deramp(): input check failed!\n");
		return -1;
	}
	/*
	* 准备输出h5文件
	*/
	int ret;
	FormatConversion conversion;
	int images_num = static_cast<int>(outSLCH5Files.size());
	for (int i = 0; i < images_num; i++)
	{
		ret = conversion.validate_distinct_h5_output(SLCH5Files[i].c_str(), outSLCH5Files[i].c_str());
		if (return_check(ret, "validate_distinct_h5_output()", error_head)) return -1;
		ret = conversion.creat_new_h5(outSLCH5Files[i].c_str());
		if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	}
	double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing,
		nearRangeTime, wavelength, prf, start, end;
	int sceneHeight, sceneWidth, offset_row, offset_col;
	Mat lon_coef, lat_coef, dem, mappedDem, statevec, rangePos, azimuthPos;
	ComplexMat slc;
	string start_time, end_time, master_file;
	master_file = SLCH5Files[reference - 1];
	ret = conversion.read_int_from_h5(master_file.c_str(), "range_len", &sceneWidth);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(master_file.c_str(), "azimuth_len", &sceneHeight);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(master_file.c_str(), "offset_row", &offset_row);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(master_file.c_str(), "offset_col", &offset_col);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(master_file.c_str(), "lon_coefficient", lon_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(master_file.c_str(), "lat_coefficient", lat_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(master_file.c_str(), "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(master_file.c_str(), "carrier_frequency", &wavelength);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	wavelength = VEL_C / wavelength;
	ret = conversion.read_double_from_h5(master_file.c_str(), "range_spacing", &rangeSpacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(master_file.c_str(), "slant_range_first_pixel", &nearRangeTime);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(master_file.c_str(), "state_vec", statevec);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Utils::computeImageGeoBoundry(lat_coef, lon_coef, sceneHeight, sceneWidth, offset_row, offset_col,
		&lonMax, &latMax, &lonMin, &latMin);
	if (return_check(ret, "computeImageGeoBoundry()", error_head)) return -1;
	ret = Utils::getSRTMDEM(demPath, dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
	if (return_check(ret, "getSRTMDEM()", error_head)) return -1;
	Mat mappedLon, mappedLat;
	ret = demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
		prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 20);
	if (ret == -2) return -2;
	if (return_check(ret, "demMapping()", error_head)) return -1;
	ret = conversion.write_array_to_h5(outSLCH5Files[reference - 1].c_str(), "mapped_lat", mappedLat);
	ret = conversion.write_array_to_h5(outSLCH5Files[reference - 1].c_str(), "mapped_lon", mappedLon);
	for (int i = 0; i < images_num; i++)
	{
		if (cb && !cb(i * 100 / images_num, "Batch phase deramping..."))
		{
			return -2;
		}
		ret = SLC_deramp(slc, mappedDem, mappedLat, mappedLon, SLCH5Files[i].c_str(), TR_MODE_SINGLE_TX_SINGLE_RX, cb);
		if (ret == -2) return -2;
		if (return_check(ret, "SLC_deramp()", error_head)) return -1;
		ret = conversion.write_slc_to_h5(outSLCH5Files[i].c_str(), slc);
		if (return_check(ret, "write_slc_to_h5()", error_head)) return -1;
		ret = conversion.Copy_para_from_h5_2_h5(SLCH5Files[i].c_str(), outSLCH5Files[i].c_str());
		if (return_check(ret, "Copy_para_from_h5_2_h5()", error_head)) return -1;
		ret = conversion.read_int_from_h5(SLCH5Files[i].c_str(), "offset_row", &offset_row);
		if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(outSLCH5Files[i].c_str(), "offset_row", offset_row);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.read_int_from_h5(SLCH5Files[i].c_str(), "offset_col", &offset_col);
		if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(outSLCH5Files[i].c_str(), "offset_col", offset_col);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(outSLCH5Files[i].c_str(), "range_len", sceneWidth);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(outSLCH5Files[i].c_str(), "azimuth_len", sceneHeight);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
	}
	
	return 0;
}

int Deflat::topography_phase_simulation(
	Mat& mappedDEM, 
	Mat& topography_phase,
	Mat& inc_coef, 
	double B_effect, 
	double nearRangeTime,
	int offset_row,
	int offset_col,
	double wavelength,
	double rangeSpacing,
	DeflatProgressCallback cb
)
{
	if (mappedDEM.empty() ||
		mappedDEM.type() != CV_16S ||
		wavelength < 0.0 ||
		inc_coef.type() != CV_64F ||
		inc_coef.rows != 1 ||
		inc_coef.cols != 11 ||
		nearRangeTime <= 0.0 ||
		wavelength < 0.0 ||
		rangeSpacing < 0.0
		)
	{
		fprintf(stderr, "topography_phase_simulation(): input check failed!\n");
		return -1;
	}
	int rows = mappedDEM.rows;
	int cols = mappedDEM.cols;
	topography_phase.create(rows, cols, CV_64F);
    double offset_inc, scale_inc, offset_y, scale_y, a0, a1, a2, a3, a4, a5;
	offset_inc = inc_coef.at<double>(0, 0);
	scale_inc = inc_coef.at<double>(0, 1);
	offset_y = inc_coef.at<double>(0, 2);
	scale_y = inc_coef.at<double>(0, 3);
	a0 = inc_coef.at<double>(0, 4);
	a1 = inc_coef.at<double>(0, 5);
	a2 = inc_coef.at<double>(0, 6);
	a3 = inc_coef.at<double>(0, 7);
	a4 = inc_coef.at<double>(0, 8);
	a5 = inc_coef.at<double>(0, 9);

	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	int step = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag) continue;
		double r1, inc, jj;
		for (int j = 0; j < cols; j++)
		{
			jj = (double)j + (double)offset_col;
			jj = (jj - offset_y) / scale_y;
			inc = a0 + a1 * jj + a2 * jj * jj + a3 * jj * jj * jj + a4 * jj * jj * jj * jj + a5 * jj * jj * jj * jj * jj;
			inc = inc * scale_inc + offset_inc;

			r1 = nearRangeTime * VEL_C / 2.0 + rangeSpacing * (double)j;//主星斜距
			topography_phase.at<double>(i, j) = - 4 * PI * mappedDEM.at<short>(i, j) * B_effect / wavelength / r1 / sin(inc / 180.0 * PI);
		}
		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / rows, "Simulating topography phase..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Deflat::computeImageGeoBoundry(
	Mat& lat_coefficient,
	Mat& lon_coefficient,
	int sceneHeight,
	int sceneWidth,
	int offset_row,
	int offset_col,
	double* lonMax,
	double* latMax,
	double* lonMin, 
	double* latMin
)
{
	if ( lon_coefficient.rows != 1 || 
		lon_coefficient.cols != 32 || 
		lon_coefficient.type() != CV_64F ||
		lat_coefficient.rows != 1 ||
		lat_coefficient.cols != 32 || 
		lat_coefficient.type() != CV_64F ||
		sceneHeight < 1 ||
		sceneWidth < 1 ||
		!lonMax || !lonMin || !latMax || !latMin
		)
	{
		fprintf(stderr, "computeImageGeoBoundry(): input check failed! \n");
		return -1;
	}
	int ret;
	Utils util;
	/*
	* 图像坐标转经纬坐标
	*/
	Mat row, col;
	row.create(4, 1, CV_64F); col.create(4, 1, CV_64F);
	row.at<double>(0, 0) = offset_row;//左上角
	col.at<double>(0, 0) = offset_col;

	row.at<double>(1, 0) = offset_row;//右上角
	col.at<double>(1, 0) = offset_col + sceneWidth;

	row.at<double>(2, 0) = offset_row + sceneHeight;//左下角
	col.at<double>(2, 0) = offset_col;

	row.at<double>(3, 0) = offset_row + sceneHeight;//右下角
	col.at<double>(3, 0) = offset_col + sceneWidth;
	Mat lon, lat;
	ret = util.coord_conversion(lon_coefficient, row, col, lon);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	ret = util.coord_conversion(lat_coefficient, row, col, lat);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	cv::minMaxLoc(lon, lonMin, lonMax);
	cv::minMaxLoc(lat, latMin, latMax);
	double extra = 5.0 / 6000;
	*lonMin = *lonMin - extra * 20;
	*lonMax = *lonMax + extra * 20;
	*latMin = *latMin - extra * 20;
	*latMax = *latMax + extra * 20;

	return 0;
}

int Deflat::getSRTMFileName(double lonMin, double lonMax, double latMin, double latMax, vector<string>& name)
{
	if (fabs(lonMin) > 180.0 ||
		fabs(lonMax) > 180.0 ||
		fabs(latMin) >= 60.0 ||
		fabs(latMax) >= 60.0
		)
	{
		fprintf(stderr, "getSRTMFileName(): input check failed!\n");
		return -1;
	}
	name.clear();
	char tmp[512];
	int startRow, endRow, startCol, endCol;
	double spacing = 5.0;
	startRow = (int)((60.0 - latMax) / spacing) + 1;
	endRow = (int)((60.0 - latMin) / spacing) + 1;
	startCol = (int)((lonMin + 180.0) / spacing) + 1;
	endCol = (int)((lonMax + 180.0) / spacing) + 1;

	for (int r = startRow; r <= endRow; r++)
	{
		for (int c = startCol; c <= endCol; c++)
		{
			sprintf(tmp, "srtm_%02d_%02d.zip", c, r);
			name.push_back(string(tmp));
		}
	}

	return 0;
}

int Deflat::downloadSRTM(const char* name)
{
	bool isConnect;
	DWORD dw;
	isConnect = IsNetworkAlive(&dw);
	if (!isConnect)
	{
		fprintf(stderr, "downloadSRTM(): network is not connected!\n");
		return -1;
	}
	// removed unused: ret (replaced by HRESULT Result below)
	string url = this->SRTMURL + name;
	string savefile = this->DEMPath + string("\\") + name;
	std::replace(savefile.begin(), savefile.end(), '/', '\\');
	HRESULT Result = URLDownloadToFileA(NULL, url.c_str(), savefile.c_str(), 0, NULL);
	if (Result != S_OK)
	{
		fprintf(stderr, "downloadSRTM(): download failed!\n");
		return -1;
	}
	return 0;
}

namespace {
	std::string getTifPath(const std::string& demPath, const std::string& zipFileName)
	{
		std::string folderName = zipFileName.substr(0, zipFileName.length() - 4);
		std::string path = demPath + "\\" + folderName + "\\" + folderName + ".tif";
		std::replace(path.begin(), path.end(), '/', '\\');
		return path;
	}
}

template<typename T, typename Predicate>
bool fillInvalidGaps(cv::Mat& mat, Predicate is_invalid,
	DeflatProgressCallback cb, int progressStart, int progressEnd)
{
	const int rows = mat.rows;
	const int cols = mat.cols;
	if (rows <= 0 || cols <= 0) return true;
	const auto reportRows = [&](int completedRows, int totalRows, int start, int end) {
		if (!cb || (completedRows != totalRows && completedRows % std::max(1, totalRows / 100) != 0)) return true;
		const int progress = start + (completedRows * (end - start)) / totalRows;
		char message[128] = {};
		sprintf_s(message, sizeof(message), "Filling DEM gaps (rows %d/%d)...", completedRows, totalRows);
		return cb(progress, message);
	};

	// Fill complete horizontal invalid runs in one pass.  The old implementation
	// searched outward separately for every invalid cell, which becomes
	// quadratic for a large invalid region.
	for (int row = 0; row < rows; ++row)
	{
		int column = 0;
		while (column < cols)
		{
			if (!is_invalid(mat.at<T>(row, column))) { ++column; continue; }
			const int first = column;
			while (column < cols && is_invalid(mat.at<T>(row, column))) ++column;
			const int last = column - 1;
			const int left = first - 1;
			const int right = column;
			if (left >= 0 && right < cols)
			{
				const double leftValue = static_cast<double>(mat.at<T>(row, left));
				const double rightValue = static_cast<double>(mat.at<T>(row, right));
				for (int k = first; k <= last; ++k)
				{
					const double ratio = static_cast<double>(k - left) / static_cast<double>(right - left);
					mat.at<T>(row, k) = static_cast<T>(leftValue + (rightValue - leftValue) * ratio);
				}
			}
		}
		if (!reportRows(row + 1, rows, progressStart, progressEnd)) return false;
	}

	// Fill remaining internal vertical runs.  This also handles rows that had
	// no horizontal anchor without allocating a mask proportional to the image.
	for (int column = 0; column < cols; ++column)
	{
		int row = 0;
		while (row < rows)
		{
			if (!is_invalid(mat.at<T>(row, column))) { ++row; continue; }
			const int first = row;
			while (row < rows && is_invalid(mat.at<T>(row, column))) ++row;
			const int last = row - 1;
			const int up = first - 1;
			const int down = row;
			if (up >= 0 && down < rows)
			{
				const double upValue = static_cast<double>(mat.at<T>(up, column));
				const double downValue = static_cast<double>(mat.at<T>(down, column));
				for (int k = first; k <= last; ++k)
				{
					const double ratio = static_cast<double>(k - up) / static_cast<double>(down - up);
					mat.at<T>(k, column) = static_cast<T>(upValue + (downValue - upValue) * ratio);
				}
			}
		}
		if (cb && (column == cols - 1 || (column + 1) % std::max(1, cols / 100) == 0))
		{
			const int progress = progressStart + ((column + 1) * (progressEnd - progressStart)) / cols;
			char message[128] = {};
			sprintf_s(message, sizeof(message), "Filling DEM gaps (columns %d/%d)...", column + 1, cols);
			if (!cb(progress, message)) return false;
		}
	}

	// Propagate values into edge-only gaps, then zero a completely invalid
	// matrix.  These passes are linear and preserve the historical fallback.
	for (int row = 0; row < rows; ++row)
	{
		int firstValid = -1;
		for (int column = 0; column < cols; ++column)
		{
			if (!is_invalid(mat.at<T>(row, column))) { firstValid = column; break; }
		}
		if (firstValid >= 0)
			for (int column = 0; column < firstValid; ++column) mat.at<T>(row, column) = mat.at<T>(row, firstValid);
		int lastValid = -1;
		for (int column = cols - 1; column >= 0; --column)
		{
			if (!is_invalid(mat.at<T>(row, column))) { lastValid = column; break; }
		}
		if (lastValid >= 0)
			for (int column = lastValid + 1; column < cols; ++column) mat.at<T>(row, column) = mat.at<T>(row, lastValid);
		if (!reportRows(row + 1, rows, progressStart, progressEnd)) return false;
	}
	for (int column = 0; column < cols; ++column)
	{
		int firstValid = -1;
		for (int row = 0; row < rows; ++row)
		{
			if (!is_invalid(mat.at<T>(row, column))) { firstValid = row; break; }
		}
		if (firstValid >= 0)
			for (int row = 0; row < firstValid; ++row) mat.at<T>(row, column) = mat.at<T>(firstValid, column);
		int lastValid = -1;
		for (int row = rows - 1; row >= 0; --row)
		{
			if (!is_invalid(mat.at<T>(row, column))) { lastValid = row; break; }
		}
		if (lastValid >= 0)
			for (int row = lastValid + 1; row < rows; ++row) mat.at<T>(row, column) = mat.at<T>(lastValid, column);
		if (cb && (column == cols - 1 || (column + 1) % std::max(1, cols / 100) == 0))
		{
			char message[128] = {};
			sprintf_s(message, sizeof(message), "Filling DEM gaps (edge columns %d/%d)...", column + 1, cols);
			if (!cb(progressEnd, message)) return false;
		}
	}
	for (int row = 0; row < rows; ++row)
	{
		for (int column = 0; column < cols; ++column)
			if (is_invalid(mat.at<T>(row, column))) mat.at<T>(row, column) = static_cast<T>(0);
		if (!reportRows(row + 1, rows, progressStart, progressEnd)) return false;
	}
	if (cb && !cb(progressEnd, "Filling DEM gaps completed.")) return false;
	return true;
}



int Deflat::getSRTMDEM(
	const char* filepath,
	Mat& DEM_out,
	double* lonUL,
	double* latUL,
	double lonMin, 
	double lonMax, 
	double latMin,
	double latMax
)
{
	if (!filepath || !lonUL || !latUL) return -1;
	this->DEMPath = filepath;
	int ret = Utils::getSRTMDEM(filepath, DEM_out, lonUL, latUL, lonMin, lonMax, latMin, latMax);
	if (ret == 0)
	{
		this->rows = DEM_out.rows;
		this->cols = DEM_out.cols;
	}
	return ret;
}
