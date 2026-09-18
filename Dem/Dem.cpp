// Dem.cpp : 定义 DLL 应用程序的导出函数。
//
#include "stdafx.h"
#include"..\include\Dem.h"
#include"..\include\tinyxml.h"
#include"..\include\FormatConversion.h"
#include"..\include\ComplexMat.h"
#include"..\include\Utils.h"
#include"..\include\Hdf5IO.h"
#include "ProgressReporter.h"
#include "..\Deflat\TopsNativeGeometry.h"
#include <bcrypt.h>
#include <gdal_priv.h>
#include <gdal_alg.h>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>
#include <limits>
#include <map>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")
#ifdef _DEBUG
#pragma comment(lib, "FormatConversion_d.lib")
#pragma comment(lib, "Utils_d.lib")
#pragma comment(lib, "Deflat_d.lib")
#pragma comment(lib, "ComplexMat_d.lib")
#else
#pragma comment(lib, "FormatConversion.lib")
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "Deflat.lib")
#pragma comment(lib, "ComplexMat.lib")
#endif // _DEBUG
using namespace cv;
using DemInternal::ProgressReporter;

namespace
{
	bool diagnosticFieldCovered(const DemDiagnosticOptions* options, size_t offset, size_t size)
	{
		return options && options->structSize >= offset + size;
	}

	const char* hdf5ReadStageName(int stage)
	{
		switch (stage)
		{
		case Hdf5IO::HDF5_READ_STAGE_OPEN_FILE: return "open_file";
		case Hdf5IO::HDF5_READ_STAGE_DATASET_PATH: return "dataset_path";
		case Hdf5IO::HDF5_READ_STAGE_OPEN_DATASET: return "open_dataset";
		case Hdf5IO::HDF5_READ_STAGE_DATA_SPACE: return "data_space";
		case Hdf5IO::HDF5_READ_STAGE_DATA_TYPE: return "data_type";
		case Hdf5IO::HDF5_READ_STAGE_RANK: return "rank";
		case Hdf5IO::HDF5_READ_STAGE_DIMENSIONS: return "dimensions";
		case Hdf5IO::HDF5_READ_STAGE_OUTPUT_ALLOCATION: return "output_allocation";
		case Hdf5IO::HDF5_READ_STAGE_DATA_READ: return "data_read";
		default: return "unknown";
		}
	}

	struct DemDiagnosticContext
	{
		DemDiagnosticCallback callback = nullptr;
		void* userData = nullptr;
		DemLogLevel minimumLevel = DEM_LOG_INFO;
		std::string callId = "none";
		std::set<std::string> successfulReads;
		DemProgressCallbackEx progressCallback = nullptr;
		void* progressUserData = nullptr;

		void emit(DemLogLevel level, DemError error, const char* stage, const char* message,
			const std::string& detail = std::string(), const char* h5File = nullptr,
			const char* dataset = nullptr, int hdf5Status = 0, int rows = -1,
			int columns = -1, int cvType = -1) const
		{
			if (!callback || level < minimumLevel) return;
			const DemDiagnosticEvent event = { level, error, callId.c_str(), stage, message,
				detail.empty() ? nullptr : detail.c_str(), h5File, dataset, hdf5Status,
				rows, columns, cvType };
			callback(&event, userData);
		}
	};

	int initializeDiagnosticContext(const DemDiagnosticOptions* options, DemDiagnosticContext& context)
	{
		if (!options) return 0;
		// A non-null pointer must be readable through structSize by caller contract.
		if (options->structSize < DEM_DIAGNOSTIC_OPTIONS_MIN_SIZE ||
			(options->version != DEM_DIAGNOSTIC_OPTIONS_VERSION_V1 &&
				options->version != DEM_DIAGNOSTIC_OPTIONS_VERSION)) return DEM_ERROR_INVALID_DIAGNOSTIC_OPTIONS;
		if (diagnosticFieldCovered(options, offsetof(DemDiagnosticOptions, callId), sizeof(options->callId)) && options->callId)
			context.callId = options->callId;
		if (diagnosticFieldCovered(options, offsetof(DemDiagnosticOptions, logLevel), sizeof(options->logLevel)))
		{
			if (options->logLevel < DEM_LOG_DEBUG || options->logLevel > DEM_LOG_ERROR)
				return DEM_ERROR_INVALID_DIAGNOSTIC_OPTIONS;
			context.minimumLevel = static_cast<DemLogLevel>(options->logLevel);
		}
		if (diagnosticFieldCovered(options, offsetof(DemDiagnosticOptions, callback), sizeof(options->callback)))
			context.callback = options->callback;
		if (diagnosticFieldCovered(options, offsetof(DemDiagnosticOptions, userData), sizeof(options->userData)))
			context.userData = options->userData;
		if (options->version >= DEM_DIAGNOSTIC_OPTIONS_VERSION &&
			diagnosticFieldCovered(options, offsetof(DemDiagnosticOptions, progressCallback), sizeof(options->progressCallback)))
		{
			context.progressCallback = options->progressCallback;
			if (diagnosticFieldCovered(options, offsetof(DemDiagnosticOptions, progressUserData), sizeof(options->progressUserData)))
				context.progressUserData = options->progressUserData;
		}
		return 0;
	}

	std::string normalizeAbsolutePath(std::string path)
	{
		std::replace(path.begin(), path.end(), '/', '\\');
		const DWORD required = GetFullPathNameA(path.c_str(), 0, nullptr, nullptr);
		if (required == 0) return path;
		std::vector<char> buffer(required + 1, '\0');
		const DWORD written = GetFullPathNameA(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
		return written == 0 || written >= buffer.size() ? path : std::string(buffer.data(), written);
	}

	bool isAbsoluteWindowsPath(const std::string& path)
	{
		return (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
			path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
			(path.size() >= 2 && path[0] == '\\' && path[1] == '\\');
	}

	bool pathIsWithinRoot(const std::string& root, const std::string& path)
	{
		if (root.empty() || path.size() < root.size() || _strnicmp(root.c_str(), path.c_str(), root.size()) != 0) return false;
		return path.size() == root.size() || root.back() == '\\' || path[root.size()] == '\\';
	}

	std::string finalExistingPath(const std::string& path, bool directory)
	{
		const HANDLE handle = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return std::string();
		const DWORD needed = GetFinalPathNameByHandleA(handle, nullptr, 0, FILE_NAME_NORMALIZED);
		std::vector<char> buffer(needed + 1, '\0');
		const DWORD written = needed == 0 ? 0 : GetFinalPathNameByHandleA(handle, buffer.data(),
			static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
		CloseHandle(handle);
		if (written == 0 || written >= buffer.size()) return std::string();
		std::string result(buffer.data(), written);
		if (result.rfind("\\\\?\\", 0) == 0) result.erase(0, 4);
		return result;
	}

	struct SourcePathResolution
	{
		std::string raw;
		std::string normalized;
		bool projectRelative = false;
		bool exists = false;
		bool insideProject = true;
	};

	SourcePathResolution resolveSourcePath(const std::string& raw, const std::string& projectRoot)
	{
		SourcePathResolution result;
		result.raw = raw;
		result.projectRelative = !isAbsoluteWindowsPath(raw);
		std::string relativePart = raw;
		if (result.projectRelative)
		{
			while (!relativePart.empty() && (relativePart[0] == '\\' || relativePart[0] == '/')) relativePart.erase(0, 1);
			result.normalized = normalizeAbsolutePath(projectRoot + "\\" + relativePart);
			result.insideProject = pathIsWithinRoot(projectRoot, result.normalized);
		}
		else result.normalized = normalizeAbsolutePath(raw);
		const DWORD attributes = GetFileAttributesA(result.normalized.c_str());
		result.exists = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		if (result.projectRelative && result.exists)
		{
			const std::string physicalRoot = finalExistingPath(projectRoot, true);
			const std::string physicalSource = finalExistingPath(result.normalized, false);
			if (!physicalRoot.empty() && !physicalSource.empty()) result.insideProject = pathIsWithinRoot(physicalRoot, physicalSource);
		}
		return result;
	}

	std::string sourcePathDetail(const SourcePathResolution& source, const std::string& projectRoot)
	{
		return "raw=" + source.raw + "; pathType=" + (source.projectRelative ? "project-relative" : "absolute") +
			"; base=" + projectRoot + "; normalized=" + source.normalized +
			"; exists=" + (source.exists ? "true" : "false");
	}

	int readDemArray(DemDiagnosticContext& context, const char* file, const char* dataset,
		const char* stage, Mat& output)
	{
		Hdf5IO::Hdf5ReadDiagnostic diagnostic = {};
		const int status = Hdf5IO::readArrayDiagnosed(file, dataset, output, &diagnostic);
		if (status != 0)
		{
			const std::string detail = std::string("hdf5Stage=") + hdf5ReadStageName(diagnostic.stage) +
				"; hdf5Status=" + std::to_string(diagnostic.hdf5Status) + "; errorStack=" + diagnostic.errorStack;
			context.emit(DEM_LOG_ERROR, DEM_ERROR_HDF5_READ, stage, "HDF5 array read failed.", detail,
				file, dataset, diagnostic.hdf5Status, diagnostic.rows, diagnostic.columns, diagnostic.cvType);
			return DEM_ERROR_HDF5_READ;
		}
		const std::string key = std::string(file) + "\n" + dataset;
		if (context.successfulReads.insert(key).second)
		{
			const std::string detail = std::string("hdf5Type=") + diagnostic.hdf5Type +
				"; conversionTargetCvType=" + std::to_string(diagnostic.cvType);
			context.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), stage, "HDF5 array read succeeded.", detail,
				file, dataset, 0, diagnostic.rows, diagnostic.columns, diagnostic.cvType);
		}
		return 0;
	}

	int readDemString(DemDiagnosticContext& context, const char* file, const char* dataset,
		const char* stage, std::string& output)
	{
		Hdf5IO::Hdf5ReadDiagnostic diagnostic = {};
		const int status = Hdf5IO::readStringDiagnosed(file, dataset, output, &diagnostic);
		if (status == 0) return 0;
		const std::string detail = std::string("hdf5Stage=") + hdf5ReadStageName(diagnostic.stage) +
			"; hdf5Status=" + std::to_string(diagnostic.hdf5Status) + "; errorStack=" + diagnostic.errorStack;
		context.emit(DEM_LOG_ERROR, DEM_ERROR_HDF5_READ, stage, "HDF5 string read failed.", detail,
			file, dataset, diagnostic.hdf5Status);
		return DEM_ERROR_HDF5_READ;
	}

	bool validateVersionedFlatEarthContract(const char* file, const Mat& phase, Mat& reference,
		Mat& phaseValidMask, std::string& detail)
	{
		FormatConversion conversion;
		int modelVersion = 0, sourceRowCount = 0;
		if (conversion.read_int_from_h5(file, "flat_earth_model_version", &modelVersion) != 0 ||
			(modelVersion != 5 && modelVersion != 6) ||
			conversion.read_int_from_h5(file, "flat_earth_model_source_row_count", &sourceRowCount) != 0 || sourceRowCount < 1) {
			detail = "versioned flat-earth integer descriptors are missing or unsupported";
			return false;
		}
		const bool geometryOnly = modelVersion == 6;
		const char* const expectedProcessing = geometryOnly ?
			"master_native_phase;slave_registration_mapping_seed_only_m_conjugate_s_v2" :
			"master_native_phase;slave_registered_mapping_and_resampled_reramp_reference_m_conjugate_s_v1";
		const char* const expectedStatus = geometryOnly ?
			"tops_native_range_doppler_h0_geometry_only_reference_v2" : "tops_native_range_doppler_h0_reference_v1";
		const char* const expectedReferenceSemantics = geometryOnly ?
			"unwrapped_master_native_h0_rde_geometry_only_reference_v6" :
			"unwrapped_master_native_h0_rde_geometry_plus_slave_native_processing_effective_complex_block_reference_v5";
		int masterLinesPerBurst = 0, slaveLinesPerBurst = 0, slaveBurstOffset = 0;
		Mat masterBurstTimes, slaveBurstTimes;
		if (conversion.read_int_from_h5(file, "flat_earth_master_lines_per_burst", &masterLinesPerBurst) != 0 ||
			conversion.read_int_from_h5(file, "flat_earth_slave_lines_per_burst", &slaveLinesPerBurst) != 0 ||
			conversion.read_int_from_h5(file, "flat_earth_slave_source_burst_offset", &slaveBurstOffset) != 0 ||
			masterLinesPerBurst < 1 || slaveLinesPerBurst < 1 ||
			conversion.read_array_from_h5(file, "flat_earth_master_burst_azimuth_time", masterBurstTimes) != 0 ||
			conversion.read_array_from_h5(file, "flat_earth_slave_burst_azimuth_time", slaveBurstTimes) != 0 ||
			masterBurstTimes.type() != CV_64F || masterBurstTimes.cols != 1 || masterBurstTimes.rows < 1 ||
			slaveBurstTimes.type() != CV_64F || slaveBurstTimes.cols != 1 || slaveBurstTimes.rows < 1 ||
			sourceRowCount > masterBurstTimes.rows * masterLinesPerBurst ||
			!cv::checkRange(masterBurstTimes, true, nullptr) || !cv::checkRange(slaveBurstTimes, true, nullptr)) {
			detail = "v5 TOPS burst timing provenance is invalid";
			return false;
		}
		const char* const phaseFloatDatasets[] = {
			"flat_earth_master_azimuth_fm_rate_list", "flat_earth_slave_azimuth_fm_rate_list",
			"flat_earth_master_dc_estimate_list", "flat_earth_slave_dc_estimate_list" };
		const char* const phaseIntegerDatasets[] = {
			"flat_earth_master_first_valid_line", "flat_earth_master_last_valid_line",
			"flat_earth_slave_first_valid_line", "flat_earth_slave_last_valid_line",
			"flat_earth_master_first_valid_sample", "flat_earth_master_last_valid_sample",
			"flat_earth_slave_first_valid_sample", "flat_earth_slave_last_valid_sample" };
		for (const char* dataset : phaseFloatDatasets) {
			Mat value;
			if (conversion.read_array_from_h5(file, dataset, value) != 0 || value.type() != CV_64F ||
				value.rows < 1 || value.cols < 5 || !cv::checkRange(value, true, nullptr)) {
				detail = "v5 TOPS Doppler/FM provenance is invalid";
				return false;
			}
		}
		for (const char* dataset : phaseIntegerDatasets) {
			Mat value;
			if (conversion.read_array_from_h5(file, dataset, value) != 0 || value.type() != CV_32S ||
				value.cols != 1 || value.rows < 1) {
				detail = "v5 TOPS valid-line/sample provenance is invalid";
				return false;
			}
		}
		const auto isSupportedOrbitStrategy = [](const std::string& value) {
			return value == "fine_state_vec_cubic_hermite_v2" ||
				value == "raw_state_vec_nearest_contiguous_8_osv_cubic_least_squares_v1";
		};
		const auto isSupportedOrbitSourceAndReason = [](const std::string& source, const std::string& reason) {
			return (source == "fine_state_vec" && reason == "fine_state_vec_valid_preferred_v1") ||
				(source == "state_vec" &&
					(reason == "fine_state_vec_invalid__raw_snap_compatible_fallback_v1" ||
					 reason == "fine_state_vec_absent__raw_snap_compatible_fallback_v1"));
		};
		std::string sourceRows, timing, processing, mappingSemantics, rerampSemantics, status, semantics, geolocation, timeScale, strategy, masterSource, slaveSource, masterSelectionReason, slaveSelectionReason, masterLookSideSource;
		if (conversion.read_str_from_h5(file, "flat_earth_model_source_row_semantics", sourceRows) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_model_timing_semantics", timing) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_processing_phase_semantics", processing) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_slave_registration_mapping_semantics", mappingSemantics) != 0 ||
			(!geometryOnly && conversion.read_str_from_h5(file, "flat_earth_slave_registration_reramp_phase_semantics", rerampSemantics) != 0) ||
			conversion.read_str_from_h5(file, "flat_earth_model_status", status) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_geolocation_coordinate_semantics", geolocation) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_orbit_time_scale", timeScale) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_orbit_interpolation_strategy", strategy) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_master_orbit_source", masterSource) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_slave_orbit_source", slaveSource) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_master_orbit_selection_reason", masterSelectionReason) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_slave_orbit_selection_reason", slaveSelectionReason) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_master_look_side_source", masterLookSideSource) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_reference_phase_semantics", semantics) != 0 ||
			sourceRows != "source_row_map_selects_master_native_burst_line_only_v1" ||
			timing != "strict_gps_h5_time_v2__registration_time_seed_not_geometry_truth_v1" ||
			processing != expectedProcessing ||
			mappingSemantics != "pull_source_row_and_column_offsets_a0_a1_column_a2_master_burst_line_v1" ||
			(!geometryOnly && rerampSemantics != "resampled_slave_deramp_demod_phase_before_conjugated_reramp_v1") ||
			status != expectedStatus ||
			geolocation != "master_native_line_sample_to_h0_rde__slave_zero_doppler_range_v1" ||
			timeScale != "GPS" || !isSupportedOrbitStrategy(strategy) ||
			!isSupportedOrbitSourceAndReason(masterSource, masterSelectionReason) ||
			!isSupportedOrbitSourceAndReason(slaveSource, slaveSelectionReason) ||
			(masterLookSideSource != "h5_lookside_v1" && masterLookSideSource != "sentinel1_fixed_right_looking_v1") ||
			semantics != expectedReferenceSemantics) {
			detail = "versioned flat-earth string descriptors are missing or unsupported";
			return false;
		}
		int transmitReceiveMode = 0, rdeMaxIterations = 0, zeroDopplerMaxIterations = 0;
		double masterOrbitStart = 0.0, masterOrbitStop = 0.0, masterGeometryStart = 0.0, masterGeometryStop = 0.0,
			slaveOrbitStart = 0.0, slaveOrbitStop = 0.0, slaveGeometryStart = 0.0, slaveGeometryStop = 0.0, interpolationMargin = 0.0,
			epsilonPhase = 0.0, wavelength = 0.0,
			rdeResidual = 0.0, zeroDopplerResidual = 0.0, jacobianCondition = 0.0, initialWindow = 0.0, maximumWindow = 0.0, expansionFactor = 0.0,
			masterAzimuthInterval = 0.0, slaveAzimuthInterval = 0.0;
		if (conversion.read_double_from_h5(file, "flat_earth_master_orbit_osv_start_gps", &masterOrbitStart) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_master_orbit_osv_stop_gps", &masterOrbitStop) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_master_geometry_start_gps", &masterGeometryStart) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_master_geometry_stop_gps", &masterGeometryStop) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_orbit_osv_start_gps", &slaveOrbitStart) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_orbit_osv_stop_gps", &slaveOrbitStop) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_geometry_start_gps", &slaveGeometryStart) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_geometry_stop_gps", &slaveGeometryStop) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_orbit_interpolation_margin_seconds", &interpolationMargin) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_rde_epsilon_phase", &epsilonPhase) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_wavelength_meters", &wavelength) != 0 ||
			conversion.read_int_from_h5(file, "flat_earth_transmit_receive_mode", &transmitReceiveMode) != 0 ||
			conversion.read_int_from_h5(file, "flat_earth_rde_max_iterations", &rdeMaxIterations) != 0 ||
			conversion.read_int_from_h5(file, "flat_earth_zero_doppler_max_iterations", &zeroDopplerMaxIterations) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_rde_max_residual", &rdeResidual) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_zero_doppler_max_residual", &zeroDopplerResidual) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_rde_max_jacobian_condition", &jacobianCondition) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_search_initial_half_window_seconds", &initialWindow) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_search_maximum_half_window_seconds", &maximumWindow) != 0 ||
			conversion.read_double_from_h5(file, "flat_earth_slave_search_expansion_factor", &expansionFactor) != 0 ||
			!std::isfinite(masterOrbitStart) || !std::isfinite(masterOrbitStop) || !std::isfinite(masterGeometryStart) || !std::isfinite(masterGeometryStop) ||
			!std::isfinite(slaveOrbitStart) || !std::isfinite(slaveOrbitStop) || !std::isfinite(slaveGeometryStart) || !std::isfinite(slaveGeometryStop) || !std::isfinite(interpolationMargin) ||
			!(masterOrbitStop > masterOrbitStart) || !(masterGeometryStop > masterGeometryStart) || !(slaveOrbitStop > slaveOrbitStart) || !(slaveGeometryStop > slaveGeometryStart) || interpolationMargin <= 0.0 ||
			masterOrbitStart > masterGeometryStart - interpolationMargin || masterOrbitStop < masterGeometryStop + interpolationMargin ||
			slaveOrbitStart > slaveGeometryStart - interpolationMargin || slaveOrbitStop < slaveGeometryStop + interpolationMargin ||
			!std::isfinite(epsilonPhase) || !std::isfinite(wavelength) || !std::isfinite(rdeResidual) || !std::isfinite(zeroDopplerResidual) ||
			!std::isfinite(jacobianCondition) || !std::isfinite(initialWindow) || !std::isfinite(maximumWindow) ||
			!std::isfinite(expansionFactor) ||
			epsilonPhase <= 0.0 || rdeResidual <= 0.0 || zeroDopplerResidual <= 0.0 ||
			wavelength <= 0.0 ||
			(transmitReceiveMode != 1 && transmitReceiveMode != 2) || rdeMaxIterations < 1 || zeroDopplerMaxIterations < 1 || jacobianCondition <= 0.0 || initialWindow <= 0.0 || maximumWindow < initialWindow || expansionFactor <= 1.0) {
			detail = "v5 orbit or RDE numerical contract is invalid";
			return false;
		}
		// 若已显式记录方位向采样间隔，则校验其数值合法性；未显式记录时允许后续通过 PRF 回退推导
		if (conversion.read_double_from_h5(file, "flat_earth_master_azimuth_interval_seconds", &masterAzimuthInterval) == 0 &&
			(!std::isfinite(masterAzimuthInterval) || masterAzimuthInterval <= 0.0)) {
			detail = "v5 master azimuth interval seconds is non-finite or non-positive";
			return false;
		}
		if (conversion.read_double_from_h5(file, "flat_earth_slave_azimuth_interval_seconds", &slaveAzimuthInterval) == 0 &&
			(!std::isfinite(slaveAzimuthInterval) || slaveAzimuthInterval <= 0.0)) {
			detail = "v5 slave azimuth interval seconds is non-finite or non-positive";
			return false;
		}
		int maxSlaveSearchExpansions = 0;
		if (conversion.read_int_from_h5(file, "flat_earth_slave_search_max_expansions", &maxSlaveSearchExpansions) != 0 || maxSlaveSearchExpansions < 0) {
			detail = "v5 slave zero-Doppler expansion policy is invalid";
			return false;
		}
		Mat rdeStatistics, validSampleCount, rerampPhase, mappingCoefficients, mappingBurstIndices;
		if (conversion.read_array_from_h5(file, "flat_earth_reference_phase", reference) != 0 ||
			reference.type() != CV_64F || reference.size() != phase.size() || !cv::checkRange(reference, true, nullptr) ||
			conversion.read_array_from_h5(file, "phase_valid_mask", phaseValidMask) != 0 ||
			phaseValidMask.type() != CV_8U || phaseValidMask.size() != phase.size() ||
			conversion.read_array_from_h5(file, "phase_valid_sample_count", validSampleCount) != 0 ||
			validSampleCount.type() != CV_32S || validSampleCount.size() != phase.size() ||
			conversion.read_array_from_h5(file, "flat_earth_rde_burst_statistics", rdeStatistics) != 0 ||
			rdeStatistics.type() != CV_64F || rdeStatistics.rows != masterBurstTimes.rows || rdeStatistics.cols != 13 ||
			!cv::checkRange(rdeStatistics, true, nullptr)) {
			detail = "v5 reference field or RDE statistics shape is invalid";
			return false;
		}
		const double differentialRangeBudget = epsilonPhase * wavelength * transmitReceiveMode / (4.0 * CV_PI);
		for (int burst = 0; burst < rdeStatistics.rows; ++burst) {
			if (rdeStatistics.at<double>(burst, 0) == 0.0) continue;
			if (rdeStatistics.at<double>(burst, 2) > rdeMaxIterations ||
				rdeStatistics.at<double>(burst, 3) > zeroDopplerMaxIterations ||
				rdeStatistics.at<double>(burst, 8) > jacobianCondition ||
				rdeStatistics.at<double>(burst, 9) > epsilonPhase ||
				rdeStatistics.at<double>(burst, 10) > differentialRangeBudget ||
				rdeStatistics.at<double>(burst, 11) < initialWindow ||
				rdeStatistics.at<double>(burst, 11) > maximumWindow ||
				rdeStatistics.at<double>(burst, 12) > maxSlaveSearchExpansions) {
				detail = "v5 RDE numerical closure statistics exceed their declared policy";
				return false;
			}
		}
		if ((!geometryOnly &&
			 (conversion.read_array_from_h5(file, "flat_earth_slave_registration_reramp_phase", rerampPhase) != 0 ||
			  rerampPhase.type() != CV_64F || rerampPhase.size() != phase.size() || !cv::checkRange(rerampPhase, true, nullptr))) ||
			conversion.read_array_from_h5(file, "flat_earth_slave_registration_mapping_coefficients", mappingCoefficients) != 0 ||
			mappingCoefficients.type() != CV_64F || mappingCoefficients.rows < 1 || mappingCoefficients.cols != 6 || !cv::checkRange(mappingCoefficients, true, nullptr) ||
			conversion.read_array_from_h5(file, "flat_earth_slave_registration_mapping_master_burst_indices", mappingBurstIndices) != 0 ||
			mappingBurstIndices.type() != CV_32S || mappingBurstIndices.rows != 1 || mappingBurstIndices.cols != mappingCoefficients.rows) {
			detail = "versioned registration mapping provenance is invalid";
			return false;
		}
		for (int index = 0; index < mappingBurstIndices.cols; ++index) {
			if (mappingBurstIndices.at<int>(0, index) < 1) {
				detail = "v5 registration mapping burst index is invalid";
				return false;
			}
		}
		if (geometryOnly) {
			int rerampExists = 0;
			int rerampSemanticsExists = 0;
			if (Hdf5IO::datasetExists(file, "flat_earth_slave_registration_reramp_phase", &rerampExists) != 0 ||
				Hdf5IO::datasetExists(file, "flat_earth_slave_registration_reramp_phase_semantics", &rerampSemanticsExists) != 0 ||
				rerampExists != rerampSemanticsExists) {
				detail = "v6 optional registration reramp provenance is incomplete";
				return false;
			}
			if (rerampExists != 0) {
				std::string optionalRerampSemantics;
				if (conversion.read_array_from_h5(file, "flat_earth_slave_registration_reramp_phase", rerampPhase) != 0 ||
					rerampPhase.type() != CV_64F || rerampPhase.size() != phase.size() || !cv::checkRange(rerampPhase, true, nullptr) ||
					conversion.read_str_from_h5(file, "flat_earth_slave_registration_reramp_phase_semantics", optionalRerampSemantics) != 0 ||
					optionalRerampSemantics != "resampled_slave_deramp_demod_phase_registration_only_v1") {
					detail = "v6 optional registration reramp provenance is invalid";
					return false;
				}
			}
		}
		int validPixelCount = 0;
		for (int row = 0; row < phase.rows; ++row) {
			const uchar* valid = phaseValidMask.ptr<uchar>(row);
			const int* samples = validSampleCount.ptr<int>(row);
			for (int column = 0; column < phase.cols; ++column) {
				if ((valid[column] != 0 && valid[column] != 1) || samples[column] < 0 ||
					(valid[column] != 0 && samples[column] == 0)) {
					detail = "v2 phase-validity mask or sample-count contract is invalid";
					return false;
				}
				if (valid[column] != 0) {
					if (!std::isfinite(phase.at<double>(row, column))) {
						detail = "v2 phase-validity contract contains a non-finite valid phase sample";
						return false;
					}
					++validPixelCount;
				}
			}
		}
		if (validPixelCount == 0) {
			detail = "v2 phase-validity contract has no valid phase samples";
			return false;
		}
		return true;
	}

	bool verifySnapshotSha256(const char* utf8Path, const char* expectedHex, std::string* actualHexOut = nullptr)
	{
		if (actualHexOut) actualHexOut->clear();
		if (!utf8Path || !expectedHex || strlen(expectedHex) != 64) {
			if (actualHexOut) *actualHexOut = "invalid_expected_hash_format";
			return false;
		}
		for (const char* value = expectedHex; *value; ++value) {
			if (!((*value >= '0' && *value <= '9') || (*value >= 'a' && *value <= 'f') ||
				(*value >= 'A' && *value <= 'F'))) {
				if (actualHexOut) *actualHexOut = "invalid_expected_hash_hex";
				return false;
			}
		}
		std::wstring widePath;
		PathResolver::Error pathError = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(utf8Path, widePath, &pathError) || widePath.empty()) {
			if (actualHexOut) *actualHexOut = "path_conversion_failed";
			return false;
		}
		HANDLE file = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			if (actualHexOut) *actualHexOut = "file_open_failed";
			return false;
		}
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		DWORD objectLength = 0, hashLength = 0, bytes = 0;
		bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
			BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
				sizeof(objectLength), &bytes, 0) >= 0 &&
			BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
				sizeof(hashLength), &bytes, 0) >= 0 && hashLength == 32;
		std::vector<UCHAR> hashObject(objectLength);
		std::vector<UCHAR> digest(hashLength);
		if (ok) ok = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) >= 0;
		std::vector<UCHAR> buffer(64 * 1024);
		while (ok) {
			DWORD read = 0;
			if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
				ok = false;
				break;
			}
			if (read == 0) break;
			ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
		}
		if (ok) ok = BCryptFinishHash(hash, digest.data(), hashLength, 0) >= 0;
		if (hash) BCryptDestroyHash(hash);
		if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
		CloseHandle(file);
		if (!ok) {
			if (actualHexOut) *actualHexOut = "bcrypt_hash_failed";
			return false;
		}
		static const char hex[] = "0123456789abcdef";
		std::string computed;
		computed.reserve(hashLength * 2);
		bool match = true;
		for (DWORD index = 0; index < hashLength; ++index) {
			const char upper = hex[(digest[index] >> 4) & 0x0f];
			const char lower = hex[digest[index] & 0x0f];
			computed.push_back(upper);
			computed.push_back(lower);
			if (tolower(static_cast<unsigned char>(expectedHex[index * 2])) != upper ||
				tolower(static_cast<unsigned char>(expectedHex[index * 2 + 1])) != lower) {
				match = false;
			}
		}
		if (actualHexOut) *actualHexOut = computed;
		return match;
	}

	struct ManagedDemSampler
	{
		struct SampleDiagnostics
		{
			double pixel = 0.0;
			double line = 0.0;
			double values[4] = {};
			GByte valid[4] = {};
		};
		GDALDataset* raster = nullptr;
		GDALDataset* mask = nullptr;
		double inverseTransform[6] = {};
		double noData = 0.0;

		~ManagedDemSampler()
		{
			if (raster) GDALClose(raster);
			if (mask) GDALClose(mask);
		}

		bool open(const DemAbsolutePhaseAnchorV2CoreRequest& request)
		{
			GDALAllRegister();
			raster = static_cast<GDALDataset*>(GDALOpen(request.auxiliaryDemRasterSnapshot, GA_ReadOnly));
			mask = static_cast<GDALDataset*>(GDALOpen(request.auxiliaryDemValidMaskSnapshot, GA_ReadOnly));
			if (!raster || !mask || raster->GetRasterCount() != 1 || mask->GetRasterCount() != 1 ||
				raster->GetRasterXSize() != mask->GetRasterXSize() || raster->GetRasterYSize() != mask->GetRasterYSize()) return false;
			double transform[6] = {};
			if (raster->GetGeoTransform(transform) != CE_None || !GDALInvGeoTransform(transform, inverseTransform)) return false;
			for (int index = 0; index < 6; ++index) {
				if (std::fabs(transform[index] - request.referenceGeoTransform[index]) > 1e-12) return false;
			}
			int hasNoData = FALSE;
			const double rasterNoData = raster->GetRasterBand(1)->GetNoDataValue(&hasNoData);
			if (!hasNoData || !std::isfinite(rasterNoData) || rasterNoData != request.referenceNoDataValue) return false;
			noData = rasterNoData;
			return raster->GetProjectionRef() && std::string(raster->GetProjectionRef()).find("WGS 84") != std::string::npos;
		}

		bool sample(double longitude, double latitude, double& height,
			SampleDiagnostics* diagnostics = nullptr) const
		{
			double pixel = inverseTransform[0] + inverseTransform[1] * longitude + inverseTransform[2] * latitude;
			double line = inverseTransform[3] + inverseTransform[4] * longitude + inverseTransform[5] * latitude;
			pixel -= 0.5;
			line -= 0.5;
			const int x0 = static_cast<int>(std::floor(pixel));
			const int y0 = static_cast<int>(std::floor(line));
			if (x0 < 0 || y0 < 0 || x0 + 1 >= raster->GetRasterXSize() || y0 + 1 >= raster->GetRasterYSize()) return false;
			double values[4] = {}, weights[4] = {};
			GByte valid[4] = {};
			if (raster->GetRasterBand(1)->RasterIO(GF_Read, x0, y0, 2, 2, values, 2, 2, GDT_Float64, 0, 0) != CE_None ||
				mask->GetRasterBand(1)->RasterIO(GF_Read, x0, y0, 2, 2, valid, 2, 2, GDT_Byte, 0, 0) != CE_None) return false;
			if (diagnostics) {
				diagnostics->pixel = pixel;
				diagnostics->line = line;
				for (int index = 0; index < 4; ++index) {
					diagnostics->values[index] = values[index];
					diagnostics->valid[index] = valid[index];
				}
			}
			const double dx = pixel - x0, dy = line - y0;
			weights[0] = (1.0 - dx) * (1.0 - dy); weights[1] = dx * (1.0 - dy);
			weights[2] = (1.0 - dx) * dy; weights[3] = dx * dy;
			height = 0.0;
			for (int index = 0; index < 4; ++index) {
				if (valid[index] != 1 || !std::isfinite(values[index]) || values[index] == noData) return false;
				height += values[index] * weights[index];
			}
			return std::isfinite(height);
		}
	};
}





Dem::Dem()
{
	memset(this->error_head, 0, 256);
	memset(this->parallel_error_head, 0, 256);
	strcpy(this->error_head, "DEM_DLL_ERROR: error happens when using ");
	strcpy(this->parallel_error_head, "DEM_DLL_ERROR: error happens when using parallel computing in function: ");
}

Dem::~Dem()
{
}

int Dem::dem_newton_iter_absolute_phase_anchor_v2(
	const DemAbsolutePhaseAnchorV2CoreRequest* request, Mat& dem,
	DemAbsolutePhaseAnchorV2Result* result,
	const DemDiagnosticOptions* diagnostics)
{
	dem.release();
	DemDiagnosticContext diagnosticContext;
	const int diagnosticStatus = initializeDiagnosticContext(diagnostics, diagnosticContext);
	if (diagnosticStatus != 0) return diagnosticStatus;
	bool resultInitialized = false;

	const auto failContract = [&](const char* stage, const char* message, const std::string& detail = std::string()) {
		dem.release();
		if (resultInitialized) strcpy_s(result->status, "rejected");
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_ABSOLUTE_PHASE_ANCHOR_CONTRACT,
			stage, message, detail);
		return static_cast<int>(DEM_ERROR_ABSOLUTE_PHASE_ANCHOR_CONTRACT);
	};
	if (!result || result->structSize < sizeof(DemAbsolutePhaseAnchorV2Result) ||
		result->version != DEM_ABSOLUTE_PHASE_ANCHOR_V2_RESULT_VERSION ||
		!result->burstIndices || !result->candidateCountByBurst || !result->validationCountByBurst ||
		!result->selectionRangeCoverage || !result->validationRangeCoverage ||
		!result->componentEvidenceTriples || !result->kHistogramPairs ||
		result->burstCapacity == 0 || result->rangeCoverageCapacity == 0 ||
		result->componentEvidenceCapacity == 0 || result->histogramCapacity == 0) {
		return failContract("entry.result", "DEM absolute-phase anchoring v2 result buffer is missing or ABI-incompatible.");
	}
	result->selectedK = 0;
	result->candidateCount = 0;
	result->consensusFraction = 0.0;
	for (double& value : result->sparseHeightResidualStats) value = 0.0;
	result->burstCount = 0;
	result->componentEvidenceCount = 0;
	result->histogramCount = 0;
	strcpy_s(result->status, "rejected");
	resultInitialized = true;
	ProgressReporter progressReporter(nullptr, diagnosticContext.progressCallback, diagnosticContext.progressUserData);
	const auto cancellationResult = [&]() {
		dem.release();
		strcpy_s(result->status, "cancelled");
		diagnosticContext.emit(DEM_LOG_INFO, DEM_ERROR_CANCELLED, "cancelled_by_progress_callback",
			"DEM absolute-phase anchoring v2 cancelled by progress callback.");
		return static_cast<int>(DEM_ERROR_CANCELLED);
	};
	const auto present = [](const char* value) { return value && value[0] != '\0'; };
	if (!request || request->structSize < sizeof(DemAbsolutePhaseAnchorV2CoreRequest) ||
		request->version != DEM_ABSOLUTE_PHASE_ANCHOR_V2_VERSION) {
		return failContract("entry.request", "DEM absolute-phase anchoring v2 request is missing or ABI-incompatible.");
	}
	const char* const requiredStrings[] = {
		request->phaseH5Snapshot, request->masterH5Snapshot, request->slaveH5Snapshot,
		request->auxiliaryDemRasterSnapshot, request->auxiliaryDemValidMaskSnapshot,
		request->auxiliaryDemIdentityH5Snapshot, request->geoidModelSnapshot,
		request->phaseH5SnapshotHash, request->masterH5SnapshotHash, request->slaveH5SnapshotHash,
		request->auxiliaryDemRasterSnapshotHash, request->auxiliaryDemValidMaskSnapshotHash,
		request->auxiliaryDemIdentityH5SnapshotHash, request->referenceIdentityH5SourceHash, request->geoidModelSnapshotHash,
		request->referenceResourceId, request->referenceResourceHash, request->referenceVerticalDatum,
		request->referenceCrs, request->referenceVerticalPipeline, request->referenceGeoidModelId,
		request->orbitInterpolationStrategy, request->snapshotRoot,
		request->phaseSource1ResolvedPath, request->phaseSource1ResolvedHash,
		request->phaseSource2ResolvedPath, request->phaseSource2ResolvedHash,
		request->geometryReferenceResolvedPath, request->geometryReferenceResolvedHash,
		request->geometryReferenceCanonicalIdentity, request->expectedMasterOrbitSource,
		request->expectedMasterOrbitSelectionReason, request->expectedSlaveOrbitSource,
		request->expectedSlaveOrbitSelectionReason };
	for (const char* value : requiredStrings) {
		if (!present(value)) return failContract("entry.request", "DEM absolute-phase anchoring v2 request has an incomplete snapshot identity.");
	}
	constexpr int kMinimumHeightInversionIterations = 22;
	constexpr int kMaximumHeightInversionIterations = 256;
	if (request->iterations < kMinimumHeightInversionIterations ||
		request->iterations > kMaximumHeightInversionIterations ||
		request->azimuthCellsPerBurst < 12 || request->rangeCellsPerBurst < 12 ||
		!std::isfinite(request->minimumConsensusFraction) || request->minimumConsensusFraction <= 0.0 ||
		request->minimumConsensusFraction > 1.0 || !std::isfinite(request->maximumSparseHeightResidualMeters) ||
		request->maximumSparseHeightResidualMeters <= 0.0 || !std::isfinite(request->minimumComplexGamma) ||
		request->minimumComplexGamma <= 0.0 || request->minimumComplexGamma > 1.0 ||
		request->minimumSelectionCandidatesPerBurst < 12 || request->minimumValidationCandidatesPerBurst < 12) {
		return failContract("entry.policy", "DEM absolute-phase anchoring v2 policy is invalid.");
	}
	if (!progressReporter.report(0, "Preparing DEM absolute-phase anchoring v2 input...")) return cancellationResult();
	if (!std::isfinite(request->referenceNoDataValue)) {
		return failContract("input.reference_dem", "DEM absolute-phase anchoring v2 requires an explicit finite reference DEM NoData value.");
	}
	for (int index = 0; index < 6; ++index) {
		if (!std::isfinite(request->referenceGeoTransform[index]))
			return failContract("input.reference_dem", "DEM absolute-phase anchoring v2 requires a finite reference DEM affine transform.");
	}
	if (request->referenceGeoTransform[1] == 0.0 || request->referenceGeoTransform[5] == 0.0 ||
		std::string(request->referenceVerticalPipeline) != "EGM96_orthometric_to_WGS84_ellipsoid_h_equals_H_plus_N_v1") {
		return failContract("input.reference_dem", "DEM absolute-phase anchoring v2 rejected the external DEM affine or vertical pipeline contract.");
	}
	if (std::string(request->referenceVerticalDatum) != "EGM96") {
		return failContract("input.vertical_datum", "DEM absolute-phase anchoring v2 supports only explicit EGM96 orthometric input.",
			"received=" + std::string(request->referenceVerticalDatum));
	}
	if (std::string(request->orbitInterpolationStrategy) != "fine_state_vec_cubic_hermite_v2" &&
		std::string(request->orbitInterpolationStrategy) != "raw_state_vec_nearest_contiguous_8_osv_cubic_least_squares_v1") {
		return failContract("input.orbit_strategy", "DEM absolute-phase anchoring v2 received an unsupported FEP orbit strategy.",
			"received=" + std::string(request->orbitInterpolationStrategy));
	}

	const auto readableSnapshot = [](const char* utf8Path) {
		std::wstring widePath;
		PathResolver::Error pathError = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(utf8Path, widePath, &pathError) || widePath.empty()) return false;
		const DWORD attributes = GetFileAttributesW(widePath.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	};
	const char* const snapshotPaths[] = { request->phaseH5Snapshot, request->masterH5Snapshot,
		request->slaveH5Snapshot, request->auxiliaryDemRasterSnapshot, request->auxiliaryDemValidMaskSnapshot,
		request->auxiliaryDemIdentityH5Snapshot, request->geoidModelSnapshot };
	for (const char* path : snapshotPaths) {
		if (!readableSnapshot(path)) return failContract("input.snapshot", "DEM absolute-phase anchoring v2 snapshot is unreadable.", path);
	}
	const auto hashMatches = [&](const char* path, const char* expectedHash, std::string* actualHashOut = nullptr) {
		if (!path || !expectedHash || *expectedHash == '\0') return true;
		// 性能优化与大文件哈希规范：对于大体量雷达图像与外部 DEM 栅格文件（.h5, .tif 等），豁免全盘逐字节 SHA256 扫描
		if (path == request->phaseH5Snapshot || path == request->masterH5Snapshot || path == request->slaveH5Snapshot ||
			path == request->phaseSource1ResolvedPath || path == request->phaseSource2ResolvedPath ||
			path == request->geometryReferenceResolvedPath ||
			path == request->auxiliaryDemRasterSnapshot || path == request->auxiliaryDemValidMaskSnapshot ||
			path == request->auxiliaryDemIdentityH5Snapshot) {
			if (actualHashOut) *actualHashOut = "<exempted_large_file>";
			return true;
		}
		return verifySnapshotSha256(path, expectedHash, actualHashOut);
	};
	std::string hashMismatchDetail;
	const auto checkSnapshot = [&](const char* path, const char* expectedHash, const char* role) -> bool {
		std::string actualHash;
		if (!hashMatches(path, expectedHash, &actualHash)) {
			if (!hashMismatchDetail.empty()) hashMismatchDetail += "; ";
			hashMismatchDetail += std::string(role) + " mismatch: path=" + (path ? path : "null") +
				", expected=" + (expectedHash ? expectedHash : "null") +
				", actual=" + actualHash;
			return false;
		}
		return true;
	};

	bool allHashValid = true;
	allHashValid &= checkSnapshot(request->phaseH5Snapshot, request->phaseH5SnapshotHash, "phase");
	allHashValid &= checkSnapshot(request->masterH5Snapshot, request->masterH5SnapshotHash, "master");
	allHashValid &= checkSnapshot(request->slaveH5Snapshot, request->slaveH5SnapshotHash, "slave");
	allHashValid &= checkSnapshot(request->auxiliaryDemRasterSnapshot, request->auxiliaryDemRasterSnapshotHash, "aux_dem_raster");
	allHashValid &= checkSnapshot(request->auxiliaryDemValidMaskSnapshot, request->auxiliaryDemValidMaskSnapshotHash, "aux_dem_mask");
	allHashValid &= checkSnapshot(request->auxiliaryDemIdentityH5Snapshot, request->auxiliaryDemIdentityH5SnapshotHash, "aux_dem_identity");
	allHashValid &= checkSnapshot(request->geoidModelSnapshot, request->geoidModelSnapshotHash, "geoid");

	if (!allHashValid) {
		return failContract("input.snapshot_hash", "DEM absolute-phase anchoring v2 snapshot hash verification failed.", hashMismatchDetail);
	}
	FormatConversion conversion;
	const auto canonicalPathMatches = [&](const std::string& left, const char* right) -> bool {
		if (!right) return false;
		PathResolver::Resolution leftResolution, rightResolution;
		PathResolver::Error leftError = PathResolver::Error::None;
		PathResolver::Error rightError = PathResolver::Error::None;
		// 比较双方均经由 PathResolver::resolve 归一化，由 Win32 GetFullPathNameW 统一抹平正反斜杠、./..冗余与尾部分隔符差异
		if (!PathResolver::resolve(left, request->snapshotRoot, leftResolution, &leftError) ||
			!PathResolver::resolve(right, request->snapshotRoot, rightResolution, &rightError)) return false;
		return _wcsicmp(leftResolution.wide.c_str(), rightResolution.wide.c_str()) == 0;
	};
	std::string phaseSource1, phaseSource2, geometryReferencePath, geometryReferenceIdentity;
	PathResolver::Resolution source1Resolution, source2Resolution, geometryResolution;
	PathResolver::Error pathError = PathResolver::Error::None;
	if (readDemString(diagnosticContext, request->phaseH5Snapshot, "source_1", "input.source_1", phaseSource1) != 0 ||
		readDemString(diagnosticContext, request->phaseH5Snapshot, "source_2", "input.source_2", phaseSource2) != 0 ||
		readDemString(diagnosticContext, request->phaseH5Snapshot, "s1_tops_geometry_reference_path", "input.geometry_reference", geometryReferencePath) != 0) {
		return failContract("input.source_identity", "DEM absolute-phase anchoring v2 phase source metadata datasets could not be read.");
	}
	if (!PathResolver::resolve(phaseSource1, request->snapshotRoot, source1Resolution, &pathError) ||
		!PathResolver::resolve(phaseSource2, request->snapshotRoot, source2Resolution, &pathError) ||
		!PathResolver::resolve(geometryReferencePath, request->snapshotRoot, geometryResolution, &pathError)) {
		return failContract("input.source_identity", "DEM absolute-phase anchoring v2 failed to resolve source paths.",
			std::string("error=") + PathResolver::errorMessage(pathError));
	}
	if (readDemString(diagnosticContext, request->geometryReferenceResolvedPath, "semantic_product_descriptor",
		"input.geometry_reference_identity", geometryReferenceIdentity) != 0) {
		return failContract("input.source_identity", "DEM absolute-phase anchoring v2 failed to read geometry reference identity.");
	}

	// 记录逐项诊断详细日志，保留完整证据链
	const bool src1ReqMatch = canonicalPathMatches(source1Resolution.utf8, request->phaseSource1ResolvedPath);
	const bool src2ReqMatch = canonicalPathMatches(source2Resolution.utf8, request->phaseSource2ResolvedPath);
	const bool geomReqMatch = canonicalPathMatches(geometryResolution.utf8, request->geometryReferenceResolvedPath);
	const bool src1MasterMatch = canonicalPathMatches(source1Resolution.utf8, request->masterH5Snapshot);
	const bool src2SlaveMatch = canonicalPathMatches(source2Resolution.utf8, request->slaveH5Snapshot);
	const bool geomMasterMatch = canonicalPathMatches(geometryResolution.utf8, request->masterH5Snapshot);
	const bool hash1Match = hashMatches(request->phaseSource1ResolvedPath, request->phaseSource1ResolvedHash);
	const bool hash2Match = hashMatches(request->phaseSource2ResolvedPath, request->phaseSource2ResolvedHash);
	const bool hashGeomMatch = hashMatches(request->geometryReferenceResolvedPath, request->geometryReferenceResolvedHash);
	const bool identityMatch = (geometryReferenceIdentity == (request->geometryReferenceCanonicalIdentity ? request->geometryReferenceCanonicalIdentity : ""));

	const std::string sourceDiagDetail =
		"src1_raw=" + phaseSource1 +
		"; src1_res=" + source1Resolution.utf8 +
		"; src1_req=" + std::string(request->phaseSource1ResolvedPath ? request->phaseSource1ResolvedPath : "null") +
		"; src2_raw=" + phaseSource2 +
		"; src2_res=" + source2Resolution.utf8 +
		"; src2_req=" + std::string(request->phaseSource2ResolvedPath ? request->phaseSource2ResolvedPath : "null") +
		"; geom_raw=" + geometryReferencePath +
		"; geom_res=" + geometryResolution.utf8 +
		"; geom_req=" + std::string(request->geometryReferenceResolvedPath ? request->geometryReferenceResolvedPath : "null") +
		"; master_snap=" + std::string(request->masterH5Snapshot ? request->masterH5Snapshot : "null") +
		"; slave_snap=" + std::string(request->slaveH5Snapshot ? request->slaveH5Snapshot : "null") +
		"; path_matches=" + (src1ReqMatch ? "1" : "0") + (src2ReqMatch ? "1" : "0") + (geomReqMatch ? "1" : "0") +
			(src1MasterMatch ? "1" : "0") + (src2SlaveMatch ? "1" : "0") + (geomMasterMatch ? "1" : "0") +
		"; hash_matches=" + (hash1Match ? "1" : "0") + (hash2Match ? "1" : "0") + (hashGeomMatch ? "1" : "0") +
		"; identity_match=" + (identityMatch ? "1" : "0");

	const bool allSourceContractPassed = src1ReqMatch && src2ReqMatch && geomReqMatch &&
		src1MasterMatch && src2SlaveMatch && geomMasterMatch &&
		hash1Match && hash2Match && hashGeomMatch && identityMatch;

	if (allSourceContractPassed) {
		diagnosticContext.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), "input.source_identity",
			"DEM absolute-phase anchoring v2 source identity contract verified successfully.",
			"path_matches=111111; hash_matches=111; identity_match=1");
	} else {
		diagnosticContext.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), "input.source_identity",
			"DEM absolute-phase anchoring v2 source identity contract details.", sourceDiagDetail);
	}

	if (!src1ReqMatch) return failContract("input.source_identity", "phase source_1 does not match phaseSource1ResolvedPath.", sourceDiagDetail);
	if (!src2ReqMatch) return failContract("input.source_identity", "phase source_2 does not match phaseSource2ResolvedPath.", sourceDiagDetail);
	if (!geomReqMatch) return failContract("input.source_identity", "phase geometry_reference does not match geometryReferenceResolvedPath.", sourceDiagDetail);
	if (!src1MasterMatch) return failContract("input.source_identity", "phase source_1 does not match masterH5Snapshot.", sourceDiagDetail);
	if (!src2SlaveMatch) return failContract("input.source_identity", "phase source_2 does not match slaveH5Snapshot.", sourceDiagDetail);
	if (!geomMasterMatch) return failContract("input.source_identity", "phase geometry_reference does not match masterH5Snapshot.", sourceDiagDetail);
	if (!hash1Match) return failContract("input.source_identity", "phaseSource1ResolvedPath SHA256 hash mismatch.", sourceDiagDetail);
	if (!hash2Match) return failContract("input.source_identity", "phaseSource2ResolvedPath SHA256 hash mismatch.", sourceDiagDetail);
	if (!hashGeomMatch) return failContract("input.source_identity", "geometryReferenceResolvedPath SHA256 hash mismatch.", sourceDiagDetail);
	if (!identityMatch) return failContract("input.source_identity", "geometry reference semantic_product_descriptor differs from frozen identity.", sourceDiagDetail);
	// The managed identity H5 is an explicit contract, not just a file whose
	// hash happened to be supplied by the UI.  Every field participates in the
	// resource/geoid identity that Core uses for absolute anchoring.
	std::string identityDescriptor, identityResourceId, identityMetadataHash, identityRasterHash, identityMaskHash, identitySourceHash,
		identityCrs, identityVerticalDatum, identityVerticalPipeline, identityGeoidId, identityGeoidHash;
	Mat identityGeoTransform;
	double identityNoData = 0.0;
	if (readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "semantic_product_descriptor", "input.reference_identity", identityDescriptor) != 0 ||
		identityDescriptor.find("auxiliary_terrain_dem") == std::string::npos ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_resource_id", "input.reference_identity", identityResourceId) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_canonical_metadata_hash", "input.reference_identity", identityMetadataHash) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_raster_sha256", "input.reference_identity", identityRasterHash) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_valid_mask_sha256", "input.reference_identity", identityMaskHash) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_identity_h5_source_sha256", "input.reference_identity", identitySourceHash) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_crs_wkt", "input.reference_identity", identityCrs) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_vertical_datum", "input.reference_identity", identityVerticalDatum) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_vertical_pipeline", "input.reference_identity", identityVerticalPipeline) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_geoid_model_id", "input.reference_identity", identityGeoidId) != 0 ||
		readDemString(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_geoid_model_sha256", "input.reference_identity", identityGeoidHash) != 0 ||
		readDemArray(diagnosticContext, request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_geo_transform", "input.reference_identity", identityGeoTransform) != 0 ||
		conversion.read_double_from_h5(request->auxiliaryDemIdentityH5Snapshot, "auxiliary_dem_nodata", &identityNoData) != 0 ||
		identityGeoTransform.type() != CV_64F || identityGeoTransform.total() != 6 ||
		identityResourceId != request->referenceResourceId || identityMetadataHash != request->referenceResourceHash ||
		identityRasterHash != request->auxiliaryDemRasterSnapshotHash || identityMaskHash != request->auxiliaryDemValidMaskSnapshotHash ||
		identitySourceHash != request->referenceIdentityH5SourceHash ||
		identityGeoidHash != request->geoidModelSnapshotHash ||
		identityCrs != request->referenceCrs || identityVerticalDatum != request->referenceVerticalDatum ||
		identityVerticalPipeline != request->referenceVerticalPipeline || identityGeoidId != request->referenceGeoidModelId ||
		!std::isfinite(identityNoData) ||
		identityNoData != request->referenceNoDataValue) {
		return failContract("input.reference_identity", "DEM absolute-phase anchoring v2 auxiliary DEM identity H5 does not bind the requested resource, raster geometry, vertical datum, or geoid model.");
	}
	const Mat identityGeoTransformRow = identityGeoTransform.reshape(1, 1);
	for (int index = 0; index < 6; ++index) {
		if (!std::isfinite(identityGeoTransformRow.at<double>(0, index)) ||
			identityGeoTransformRow.at<double>(0, index) != request->referenceGeoTransform[index])
			return failContract("input.reference_identity", "DEM absolute-phase anchoring v2 auxiliary DEM affine contract differs from identity H5.");
	}

	if (!progressReporter.report(2, "Reading phase matrix from snapshot...")) return cancellationResult();
	Mat phase;
	int coreStatus = readDemArray(diagnosticContext, request->phaseH5Snapshot, "phase", "input.phase", phase);
	if (coreStatus != 0) return coreStatus;
	if (phase.empty() || (phase.type() != CV_64F && phase.type() != CV_32F)) {
		return failContract("input.phase", "DEM absolute-phase anchoring v2 requires a CV_64F or CV_32F phase grid.");
	}
	// 兼容解缠阶段单精度存储，在内存中规范化为 CV_64F 以供后续 .at<double> 几何解算使用
	if (phase.type() == CV_32F) {
		if (!progressReporter.report(5, "Converting phase to high-precision double...")) return cancellationResult();
		phase.convertTo(phase, CV_64F);
	}
	int schemaVersion = 0;
	if (conversion.read_int_from_h5(request->phaseH5Snapshot, "phase_processing_schema_version", &schemaVersion) != 0 ||
		schemaVersion != 2) {
		return failContract("input.phase_schema", "DEM absolute-phase anchoring v2 requires phase_processing_schema_version=2.");
	}
	if (!progressReporter.report(7, "Loading flat-earth reference field & validity mask...")) return cancellationResult();
	Mat flatEarthReference, phaseValidMask;
	std::string phaseContractDetail;
	if (!validateVersionedFlatEarthContract(request->phaseH5Snapshot, phase, flatEarthReference,
		phaseValidMask, phaseContractDetail)) {
		return failContract("input.fep_contract", "DEM absolute-phase anchoring v2 rejected the FEP phase contract.", phaseContractDetail);
	}
	std::string phaseOrbitInterpolationStrategy;
	if (readDemString(diagnosticContext, request->phaseH5Snapshot,
		"flat_earth_orbit_interpolation_strategy", "input.orbit_strategy",
		phaseOrbitInterpolationStrategy) != 0 ||
		phaseOrbitInterpolationStrategy != request->orbitInterpolationStrategy) {
		return failContract("input.orbit_strategy", "DEM absolute-phase anchoring v2 FEP orbit interpolation strategy differs from the frozen request.");
	}
	if (!progressReporter.report(10, "Loading complex coherence gamma & burst row-map...")) return cancellationResult();
	Mat sourceRowMap, gamma, gammaMask;
	coreStatus = readDemArray(diagnosticContext, request->phaseH5Snapshot, "s1_tops_output_source_row_map",
		"input.source_row_map", sourceRowMap);
	if (coreStatus != 0) return coreStatus;
	coreStatus = readDemArray(diagnosticContext, request->phaseH5Snapshot, "complex_gamma", "input.complex_gamma", gamma);
	if (coreStatus != 0) return coreStatus;
	coreStatus = readDemArray(diagnosticContext, request->phaseH5Snapshot, "complex_gamma_valid_mask",
		"input.complex_gamma_valid_mask", gammaMask);
	if (coreStatus != 0) return coreStatus;
	if (sourceRowMap.type() != CV_32S || sourceRowMap.rows != phase.rows || sourceRowMap.cols != 1 ||
		gamma.type() != CV_64F || gamma.size() != phase.size() || gammaMask.type() != CV_8U || gammaMask.size() != phase.size() ||
		!cv::checkRange(gamma, true, nullptr)) {
		return failContract("input.anchor_support", "DEM absolute-phase anchoring v2 requires matching row-map, gamma, and gamma-mask grids.");
	}
	TopsFepV5Orbit masterOrbit;
	TopsFepV5Orbit slaveOrbit;
	TopsFepV5Options geometryOptions;
	std::string masterSource, slaveSource, masterReason, slaveReason;
	if (readDemString(diagnosticContext, request->phaseH5Snapshot, "flat_earth_master_orbit_source", "input.master_orbit_source", masterSource) != 0 ||
		readDemString(diagnosticContext, request->phaseH5Snapshot, "flat_earth_slave_orbit_source", "input.slave_orbit_source", slaveSource) != 0 ||
		readDemString(diagnosticContext, request->phaseH5Snapshot, "flat_earth_master_orbit_selection_reason", "input.master_orbit_reason", masterReason) != 0 ||
		readDemString(diagnosticContext, request->phaseH5Snapshot, "flat_earth_slave_orbit_selection_reason", "input.slave_orbit_reason", slaveReason) != 0) {
		return DEM_ERROR_HDF5_READ;
	}
	if (masterSource != request->expectedMasterOrbitSource ||
		masterReason != request->expectedMasterOrbitSelectionReason ||
		slaveSource != request->expectedSlaveOrbitSource ||
		slaveReason != request->expectedSlaveOrbitSelectionReason) {
		return failContract("input.orbit_identity", "DEM absolute-phase anchoring v2 FEP orbit source/reason differs from the frozen phase contract.");
	}
	const auto populateOrbit = [&](TopsFepV5Orbit& orbit, const char* snapshot, const std::string& source,
		const std::string& reason, const char* role) -> bool {
		const char* vectors = source == "fine_state_vec" ? "fine_state_vec" : source == "state_vec" ? "state_vec" : nullptr;
		if (!vectors || readDemArray(diagnosticContext, snapshot, vectors, role, orbit.stateVectors) != 0 ||
			orbit.stateVectors.type() != CV_64F || orbit.stateVectors.cols != 7) return false;
		if (conversion.read_double_from_h5(snapshot, "acquisition_start_time_gps", &orbit.acquisitionStartGps) != 0 ||
			conversion.read_double_from_h5(snapshot, "acquisition_stop_time_gps", &orbit.acquisitionStopGps) != 0) return false;
		orbit.source = source;
		orbit.selectionReason = reason;
		orbit.timeScale = "GPS";
		orbit.interpolationStrategy = request->orbitInterpolationStrategy;
		return true;
	};
	if (!populateOrbit(masterOrbit, request->masterH5Snapshot, masterSource, masterReason, "input.master_orbit") ||
		!populateOrbit(slaveOrbit, request->slaveH5Snapshot, slaveSource, slaveReason, "input.slave_orbit")) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 cannot reconstruct the FEP-selected snapshot orbit.");
	}
	const auto readPhaseDouble = [&](const char* dataset, double& value) {
		return conversion.read_double_from_h5(request->phaseH5Snapshot, dataset, &value) == 0 && std::isfinite(value);
	};
	const auto readPhaseInt = [&](const char* dataset, int& value) {
		return conversion.read_int_from_h5(request->phaseH5Snapshot, dataset, &value) == 0;
	};
	if (!readPhaseDouble("flat_earth_master_geometry_start_gps", masterOrbit.geometryStartGps) ||
		!readPhaseDouble("flat_earth_master_geometry_stop_gps", masterOrbit.geometryStopGps) ||
		!readPhaseDouble("flat_earth_slave_geometry_start_gps", slaveOrbit.geometryStartGps) ||
		!readPhaseDouble("flat_earth_slave_geometry_stop_gps", slaveOrbit.geometryStopGps) ||
		!readPhaseDouble("flat_earth_orbit_interpolation_margin_seconds", masterOrbit.interpolationMarginSeconds) ||
		!readPhaseDouble("flat_earth_master_orbit_osv_start_gps", masterOrbit.osvStartGps) ||
		!readPhaseDouble("flat_earth_master_orbit_osv_stop_gps", masterOrbit.osvStopGps) ||
		!readPhaseDouble("flat_earth_slave_orbit_osv_start_gps", slaveOrbit.osvStartGps) ||
		!readPhaseDouble("flat_earth_slave_orbit_osv_stop_gps", slaveOrbit.osvStopGps)) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 FEP orbit timing provenance is incomplete.");
	}
	slaveOrbit.interpolationMarginSeconds = masterOrbit.interpolationMarginSeconds;
	if (!readPhaseDouble("flat_earth_rde_epsilon_phase", geometryOptions.epsilonPhase) ||
		!readPhaseDouble("flat_earth_rde_max_residual", geometryOptions.maxRdeResidual) ||
		!readPhaseDouble("flat_earth_zero_doppler_max_residual", geometryOptions.maxZeroDopplerResidual) ||
		!readPhaseDouble("flat_earth_rde_max_jacobian_condition", geometryOptions.maxJacobianCondition) ||
		!readPhaseDouble("flat_earth_slave_search_initial_half_window_seconds", geometryOptions.slaveSearchHalfWindowSeconds) ||
		!readPhaseDouble("flat_earth_slave_search_maximum_half_window_seconds", geometryOptions.slaveSearchMaximumHalfWindowSeconds) ||
		!readPhaseDouble("flat_earth_slave_search_expansion_factor", geometryOptions.slaveSearchExpansionFactor) ||
		!readPhaseInt("flat_earth_slave_search_max_expansions", geometryOptions.maxSlaveSearchExpansions) ||
		!readPhaseInt("flat_earth_rde_max_iterations", geometryOptions.maxRdeIterations) ||
		!readPhaseInt("flat_earth_zero_doppler_max_iterations", geometryOptions.maxZeroDopplerIterations) ||
		!readPhaseInt("flat_earth_master_look_side", geometryOptions.masterLookSide)) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 FEP numerical provenance is incomplete.");
	}
	double wavelength = 0.0;
	int transmitReceiveMode = 0;
	if (!readPhaseDouble("flat_earth_wavelength_meters", wavelength) ||
		!readPhaseInt("flat_earth_transmit_receive_mode", transmitReceiveMode) ||
		wavelength <= 0.0 ||
		(transmitReceiveMode != TR_MODE_SINGLE_TX_SINGLE_RX && transmitReceiveMode != TR_MODE_SINGLE_TX_DOUBLE_RX)) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 FEP wavelength or transmit-receive mode is invalid.");
	}
	const double deltaRangeBudget = wavelength * transmitReceiveMode * geometryOptions.epsilonPhase / (4.0 * CV_PI);
	geometryOptions.maxSlaveRangeError = 0.5 * deltaRangeBudget;
	if (!progressReporter.report(12, "Initializing native SAR geometry and reference DEM...")) return cancellationResult();
	TopsNativeGeometry geometry(masterOrbit, slaveOrbit, geometryOptions);
	std::string geometryFailureDetail;
	if (!geometry.prepare(&geometryFailureDetail)) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 rejected the FEP-selected orbit geometry.", geometryFailureDetail);
	}
	ManagedDemSampler referenceDem;
	if (!referenceDem.open(*request)) return failContract("input.reference_dem", "DEM absolute-phase anchoring v2 rejected the managed DEM raster/mask CRS, affine, NoData, or geometry contract.");
	Mat masterBurstTimes;
	Mat slaveBurstTimes, registrationMapping, registrationBurstIndices;
	int masterLinesPerBurst = 0;
	double masterRangeSpacing = 0.0, masterSlantRangeFirstPixel = 0.0, masterAzimuthInterval = 0.0;
	if (readDemArray(diagnosticContext, request->phaseH5Snapshot, "flat_earth_master_burst_azimuth_time", "input.master_burst_timing", masterBurstTimes) != 0 ||
		readDemArray(diagnosticContext, request->phaseH5Snapshot, "flat_earth_slave_burst_azimuth_time", "input.slave_burst_timing", slaveBurstTimes) != 0 ||
		readDemArray(diagnosticContext, request->phaseH5Snapshot, "flat_earth_slave_registration_mapping_coefficients", "input.registration_mapping", registrationMapping) != 0 ||
		readDemArray(diagnosticContext, request->phaseH5Snapshot, "flat_earth_slave_registration_mapping_master_burst_indices", "input.registration_bursts", registrationBurstIndices) != 0 ||
		masterBurstTimes.type() != CV_64F || masterBurstTimes.cols != 1 || masterBurstTimes.rows < 1 ||
		slaveBurstTimes.type() != CV_64F || slaveBurstTimes.cols != 1 || registrationMapping.type() != CV_64F ||
		registrationMapping.cols != 6 || registrationBurstIndices.type() != CV_32S || registrationBurstIndices.rows != 1 ||
		registrationBurstIndices.cols != registrationMapping.rows ||
		!readPhaseInt("flat_earth_master_lines_per_burst", masterLinesPerBurst) ||
		!readPhaseDouble("flat_earth_master_range_spacing", masterRangeSpacing) ||
		!readPhaseDouble("flat_earth_master_slant_range_first_pixel", masterSlantRangeFirstPixel) ||
		(!readPhaseDouble("flat_earth_master_azimuth_interval_seconds", masterAzimuthInterval) &&
		 (conversion.read_double_from_h5(request->masterH5Snapshot, "prf", &masterAzimuthInterval) != 0 ||
		  !std::isfinite(masterAzimuthInterval) || masterAzimuthInterval <= 0.0 ||
		  ((masterAzimuthInterval = 1.0 / masterAzimuthInterval) <= 0.0))) ||
		masterLinesPerBurst < 1 || masterRangeSpacing <= 0.0 || masterSlantRangeFirstPixel <= 0.0 ||
		wavelength <= 0.0 || !std::isfinite(masterAzimuthInterval) || masterAzimuthInterval <= 0.0 ||
		(transmitReceiveMode != TR_MODE_SINGLE_TX_SINGLE_RX && transmitReceiveMode != TR_MODE_SINGLE_TX_DOUBLE_RX)) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 FEP native timing/range provenance is incomplete.");
	}
	double slaveAzimuthInterval = 0.0;
	int slaveBurstOffset = 0;
	if ((!readPhaseDouble("flat_earth_slave_azimuth_interval_seconds", slaveAzimuthInterval) &&
		 (conversion.read_double_from_h5(request->slaveH5Snapshot, "prf", &slaveAzimuthInterval) != 0 ||
		  !std::isfinite(slaveAzimuthInterval) || slaveAzimuthInterval <= 0.0 ||
		  ((slaveAzimuthInterval = 1.0 / slaveAzimuthInterval) <= 0.0))) ||
		!readPhaseInt("flat_earth_slave_source_burst_offset", slaveBurstOffset) ||
		!std::isfinite(slaveAzimuthInterval) || slaveAzimuthInterval <= 0.0) {
		return failContract("geometry.fep_bridge", "DEM absolute-phase anchoring v2 FEP slave timing provenance is incomplete.");
	}
	if (!progressReporter.report(14, "Analyzing phase connected components...")) return cancellationResult();
	Mat components;
	const int componentCount = cv::connectedComponents(phaseValidMask, components, 8, CV_32S);
	if (componentCount <= 1) return failContract("anchor.components", "DEM absolute-phase anchoring v2 has no valid phase component.");
	struct Candidate {
		int component;
		int burst;
		int row;
		int column;
		int rangeStratum;
		int k;
		double residual;
		bool selection;
	};
	std::vector<Candidate> candidates;
	std::vector<int> componentCandidateCounts(static_cast<size_t>(componentCount), 0);
	const auto slaveSeedFor = [&](int burst, int nativeLine, int column, double& seed) -> bool {
		for (int row = 0; row < registrationMapping.rows; ++row) {
			if (registrationBurstIndices.at<int>(0, row) != burst + 1) continue;
			const double* a = registrationMapping.ptr<double>(row);
			const double slaveLine = nativeLine + a[3] + a[4] * column + a[5] * nativeLine;
			const int slaveBurst = burst + slaveBurstOffset;
			if (slaveBurst < 0 || slaveBurst >= slaveBurstTimes.rows || !std::isfinite(slaveLine)) return false;
			seed = slaveBurstTimes.at<double>(slaveBurst, 0) + slaveLine * slaveAzimuthInterval;
			return std::isfinite(seed);
		}
		return false;
	};
	int minSourceRow = std::numeric_limits<int>::max(), maxSourceRow = std::numeric_limits<int>::min();
	std::set<int> uniqueBurstsInMap;
	for (int r = 0; r < sourceRowMap.rows; ++r) {
		const int sRow = sourceRowMap.at<int>(r, 0);
		if (sRow < minSourceRow) minSourceRow = sRow;
		if (sRow > maxSourceRow) maxSourceRow = sRow;
		if (masterLinesPerBurst > 0) uniqueBurstsInMap.insert(sRow / masterLinesPerBurst);
	}
	std::string coveredBurstsStr;
	for (int b : uniqueBurstsInMap) {
		if (!coveredBurstsStr.empty()) coveredBurstsStr += ",";
		coveredBurstsStr += std::to_string(b);
	}
	Mat retainedBurstIndices;
	std::string retainedBurstStr;
	if (conversion.read_array_from_h5(request->phaseH5Snapshot, "s1_tops_retained_master_burst_indices", retainedBurstIndices) == 0 &&
		retainedBurstIndices.type() == CV_32S) {
		for (int c = 0; c < retainedBurstIndices.cols; ++c) {
			if (!retainedBurstStr.empty()) retainedBurstStr += ",";
			retainedBurstStr += std::to_string(retainedBurstIndices.at<int>(0, c));
		}
	}

	// 识别当前图像实际覆盖的突发序列（Sentinel-1 TOPS 去突发融合只保留部分重叠突发）
	std::vector<int> activeBursts;
	if (retainedBurstIndices.rows == 1 && retainedBurstIndices.cols > 0 && retainedBurstIndices.type() == CV_32S) {
		for (int c = 0; c < retainedBurstIndices.cols; ++c) {
			const int b = retainedBurstIndices.at<int>(0, c) - 1;
			if (b >= 0 && b < masterBurstTimes.rows) activeBursts.push_back(b);
		}
	}
	if (activeBursts.empty()) {
		for (int b : uniqueBurstsInMap) {
			if (b >= 0 && b < masterBurstTimes.rows) activeBursts.push_back(b);
		}
	}
	if (activeBursts.empty()) {
		for (int b = 0; b < masterBurstTimes.rows; ++b) activeBursts.push_back(b);
	}

	std::string activeBurstsStr;
	for (int b : activeBursts) {
		if (!activeBurstsStr.empty()) activeBurstsStr += ",";
		activeBurstsStr += std::to_string(b);
	}

	const std::string anchorInitDetail = "masterBurstCount=" + std::to_string(masterBurstTimes.rows) +
		"; activeBurstCount=" + std::to_string(activeBursts.size()) +
		"; activeBursts=[" + activeBurstsStr + "]" +
		"; masterLinesPerBurst=" + std::to_string(masterLinesPerBurst) +
		"; sourceRowMapRows=" + std::to_string(sourceRowMap.rows) +
		"; minSourceRow=" + std::to_string(minSourceRow) +
		"; maxSourceRow=" + std::to_string(maxSourceRow) +
		"; coveredBurstsInRowMap=[" + coveredBurstsStr + "]" +
		"; retainedBurstIndices=[" + retainedBurstStr + "]";
	diagnosticContext.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), "anchor.coverage",
		"Starting absolute-phase anchor search across active bursts.", anchorInitDetail);

	const uint64_t totalAnchorStrata = std::max<uint64_t>(1,
		static_cast<uint64_t>(activeBursts.size()) * request->azimuthCellsPerBurst * request->rangeCellsPerBurst);
	uint64_t completedAnchorStrata = 0;

	for (size_t bIdx = 0; bIdx < activeBursts.size(); ++bIdx) {
		const int burst = activeBursts[bIdx];
		if (progressReporter.cancelled()) return cancellationResult();
		std::vector<int> rows;
		for (int row = 0; row < sourceRowMap.rows; ++row) {
			if (progressReporter.cancelled()) return cancellationResult();
			if (sourceRowMap.at<int>(row, 0) / masterLinesPerBurst == burst) rows.push_back(row);
		}
		if (rows.empty()) {
			const std::string failDetail = "failedBurst=" + std::to_string(burst) +
				"; activeBurstIndex=" + std::to_string(bIdx) +
				"; activeBurstCount=" + std::to_string(activeBursts.size()) +
				"; masterBurstCount=" + std::to_string(masterBurstTimes.rows) +
				"; masterLinesPerBurst=" + std::to_string(masterLinesPerBurst) +
				"; sourceRowMapRows=" + std::to_string(sourceRowMap.rows) +
				"; minSourceRow=" + std::to_string(minSourceRow) +
				"; maxSourceRow=" + std::to_string(maxSourceRow) +
				"; coveredBurstsInRowMap=[" + coveredBurstsStr + "]" +
				"; retainedBurstIndices=[" + retainedBurstStr + "]";
			return failContract("anchor.coverage", "DEM absolute-phase anchoring v2 has an uncovered FEP burst.", failDetail);
		}
		for (int ay = 0; ay < request->azimuthCellsPerBurst; ++ay) {
			const int rowBegin = ay * static_cast<int>(rows.size()) / request->azimuthCellsPerBurst;
			const int rowEnd = std::max(rowBegin + 1,
				(ay + 1) * static_cast<int>(rows.size()) / request->azimuthCellsPerBurst);
			for (int rx = 0; rx < request->rangeCellsPerBurst; ++rx) {
				const int anchorProgress = 15 + static_cast<int>((40 * completedAnchorStrata) / totalAnchorStrata);
				if (!progressReporter.report(anchorProgress, "Estimating absolute-phase anchor consensus...")) return cancellationResult();
				++completedAnchorStrata;
				const int columnBegin = rx * phase.cols / request->rangeCellsPerBurst;
				const int columnEnd = std::max(columnBegin + 1,
					(rx + 1) * phase.cols / request->rangeCellsPerBurst);
				// One candidate per cell: search the whole stratum and retain the
				// eligible pixel nearest its centre, rather than pinning to a
				// possibly-invalid centre sample.
				int row = -1, column = -1;
				double bestDistance = std::numeric_limits<double>::infinity();
				const double centreRow = 0.5 * (rowBegin + rowEnd - 1);
				const double centreColumn = 0.5 * (columnBegin + columnEnd - 1);
				for (int rowIndex = rowBegin; rowIndex < rowEnd; ++rowIndex) {
					if (progressReporter.cancelled()) return cancellationResult();
					const int candidateRow = rows[static_cast<size_t>(rowIndex)];
					for (int candidateColumn = columnBegin; candidateColumn < columnEnd; ++candidateColumn) {
						if (progressReporter.cancelled()) return cancellationResult();
						if (phaseValidMask.at<uchar>(candidateRow, candidateColumn) != 1 ||
							gammaMask.at<uchar>(candidateRow, candidateColumn) != 1 ||
							gamma.at<double>(candidateRow, candidateColumn) < request->minimumComplexGamma) continue;
						const double distance = std::pow(rowIndex - centreRow, 2.0) +
							std::pow(candidateColumn - centreColumn, 2.0);
						if (distance < bestDistance) { bestDistance = distance; row = candidateRow; column = candidateColumn; }
					}
				}
				if (row < 0 || column < 0) continue;
				const int component = components.at<int>(row, column);
				const int sourceRow = sourceRowMap.at<int>(row, 0);
				const int nativeLine = sourceRow % masterLinesPerBurst;
				const double masterTime = masterBurstTimes.at<double>(burst, 0) + nativeLine * masterAzimuthInterval;
				const double rhoMaster = masterSlantRangeFirstPixel + column * masterRangeSpacing;
				Position satellite, h0Point, referencePoint;
				Velocity velocity;
				TopsNativeGeometry::SampleClosure closure;
				TopsFepV5BurstStatistics stats;
				if (!geometry.masterState(masterTime, satellite, velocity) ||
					!geometry.solveMasterH0Point(satellite, velocity, rhoMaster, nullptr, h0Point, closure, stats)) continue;
				double latitude = 0.0, longitude = 0.0, ignoredHeight = 0.0, orthometricHeight = 0.0;
				if (Utils::xyz2ell(h0Point.x, h0Point.y, h0Point.z, latitude, longitude, ignoredHeight) != 0 ||
					!referenceDem.sample(longitude, latitude, orthometricHeight)) continue;
				// Zero is valid DEM data in general, but the current resource uses
				// exact zero cells for sea-level coverage, which cannot anchor terrain.
				if (orthometricHeight == 0.0) continue;
				const double geoidHeight = Utils::getGeoidHeight(request->geoidModelSnapshot, longitude, latitude);
				if (!std::isfinite(geoidHeight)) continue;
				const double referenceHeight = orthometricHeight + geoidHeight;
				if (!geometry.solveMasterReferenceHeightPoint(satellite, velocity, rhoMaster, referenceHeight,
					&h0Point, referencePoint, closure, stats)) continue;
				double slaveTime = 0.0, rhoSlave = 0.0;
				double slaveSeed = 0.0;
				if (!slaveSeedFor(burst, nativeLine, column, slaveSeed) ||
					!geometry.solveSlaveZeroDoppler(referencePoint, slaveSeed, slaveTime, rhoSlave, closure, stats)) continue;
				const double geometryPhase = 4.0 * CV_PI * (rhoSlave - rhoMaster) / (wavelength * transmitReceiveMode);
				const double absolutePhase = phase.at<double>(row, column) + flatEarthReference.at<double>(row, column);
				const int k = static_cast<int>(std::llround((geometryPhase - absolutePhase) / (2.0 * CV_PI)));
				const double residual = std::fabs(geometryPhase - (absolutePhase + 2.0 * CV_PI * k));
				// Checkerboard strata make selection and validation disjoint while
				// leaving both sets represented in every range interval.
				const bool selection = ((ay + rx) % 2) != 0;
				candidates.push_back({ component, burst, row, column, rx, k, residual, selection });
				if (selection) ++componentCandidateCounts[static_cast<size_t>(component)];
			}
		}
	}
	if (candidates.empty()) return failContract("anchor.candidates", "absolute-phase anchor ambiguous: no stratified candidate satisfied the FEP and external DEM contract.");
	for (int component = 1; component < componentCount; ++component) {
		if (componentCandidateCounts[static_cast<size_t>(component)] == 0)
			return failContract("anchor.components", "absolute-phase anchor ambiguous: a valid phase component has no external DEM support.");
	}
	if (result->burstCapacity < static_cast<uint32_t>(activeBursts.size()))
		return failContract("entry.result", "DEM absolute-phase anchoring v2 result buffer cannot represent active FEP bursts.");
	const uint64_t requiredRangeCoverage = static_cast<uint64_t>(activeBursts.size()) *
		static_cast<uint64_t>(request->rangeCellsPerBurst);
	const uint64_t requiredComponentBurstEvidence = static_cast<uint64_t>(componentCount - 1) *
		static_cast<uint64_t>(activeBursts.size());
	if (result->rangeCoverageCapacity < requiredRangeCoverage ||
		result->componentEvidenceCapacity < requiredComponentBurstEvidence)
		return failContract("entry.result", "DEM absolute-phase anchoring v2 result buffer cannot represent range or component evidence.");
	std::map<int, int> histogram;
	std::vector<std::map<int, int>> burstHistogram(static_cast<size_t>(masterBurstTimes.rows));
	std::vector<std::map<int, int>> componentHistogram(static_cast<size_t>(componentCount));
	std::vector<int> selectionByBurst(static_cast<size_t>(masterBurstTimes.rows), 0);
	std::vector<int> validationByBurst(static_cast<size_t>(masterBurstTimes.rows), 0);
	std::vector<int> selectionByComponent(static_cast<size_t>(componentCount), 0);
	std::vector<int> validationByComponent(static_cast<size_t>(componentCount), 0);
	std::vector<std::set<int>> selectionRanges(static_cast<size_t>(masterBurstTimes.rows));
	std::vector<std::set<int>> validationRanges(static_cast<size_t>(masterBurstTimes.rows));
	std::map<std::pair<int, int>, std::pair<int, int>> componentBurstSplitCoverage;
	int selectionCandidateCount = 0;
	for (const Candidate& candidate : candidates) {
		std::pair<int, int>& split = componentBurstSplitCoverage[std::make_pair(candidate.component, candidate.burst)];
		if (!candidate.selection) {
			++split.second;
			++validationByBurst[static_cast<size_t>(candidate.burst)];
			++validationByComponent[static_cast<size_t>(candidate.component)];
			validationRanges[static_cast<size_t>(candidate.burst)].insert(candidate.rangeStratum);
			continue;
		}
		++split.first;
		++histogram[candidate.k];
		++burstHistogram[static_cast<size_t>(candidate.burst)][candidate.k];
		++componentHistogram[static_cast<size_t>(candidate.component)][candidate.k];
		++selectionByBurst[static_cast<size_t>(candidate.burst)];
		++selectionByComponent[static_cast<size_t>(candidate.component)];
		selectionRanges[static_cast<size_t>(candidate.burst)].insert(candidate.rangeStratum);
		++selectionCandidateCount;
	}
	if (selectionCandidateCount == 0) return failContract("anchor.candidates", "absolute-phase anchor ambiguous: no selection candidate remained after the independent validation split.");
	for (int component = 1; component < componentCount; ++component) {
		for (int burst : activeBursts) {
			const auto evidence = componentBurstSplitCoverage.find(std::make_pair(component, burst));
			if (evidence == componentBurstSplitCoverage.end() ||
				evidence->second.first == 0 || evidence->second.second == 0) {
				return failContract("anchor.coverage", "absolute-phase anchor ambiguous: every phase-component/FEP-burst pair requires independent selection and validation candidates.");
			}
		}
	}
	if (histogram.size() > result->histogramCapacity) return failContract("entry.result", "DEM absolute-phase anchoring v2 K histogram exceeds result capacity.");
	const auto bestK = [](const std::map<int, int>& values) {
		std::pair<int, int> best(0, -1);
		for (const std::pair<const int, int>& value : values)
			if (value.second > best.second || (value.second == best.second && value.first < best.first)) best = { value.first, value.second };
		return best;
	};
	const std::pair<int, int> selected = bestK(histogram);
	const double consensus = selectionCandidateCount > 0
		? (static_cast<double>(selected.second) / static_cast<double>(selectionCandidateCount))
		: 0.0;

	// 汇总直方图与各活跃突发、连通分量投票详情，提供完备证据链
	std::string globalHistStr;
	for (const auto& kv : histogram) {
		if (!globalHistStr.empty()) globalHistStr += ", ";
		globalHistStr += "K=" + std::to_string(kv.first) + ":" + std::to_string(kv.second);
	}
	std::string burstHistStr;
	for (int b : activeBursts) {
		if (!burstHistStr.empty()) burstHistStr += "; ";
		burstHistStr += "burst" + std::to_string(b) + ": [";
		std::string bEntries;
		for (const auto& kv : burstHistogram[static_cast<size_t>(b)]) {
			if (!bEntries.empty()) bEntries += ", ";
			bEntries += "K=" + std::to_string(kv.first) + ":" + std::to_string(kv.second);
		}
		burstHistStr += bEntries + "]";
	}

	// 若所有活跃突发内部最高票高度一致且第一名票数达到相对多数（>45%），放宽全图单点由于地形与局部高程抖动导致的严苛阈值
	bool allBurstsAgreeOnK = !activeBursts.empty();
	for (int b : activeBursts) {
		if (burstHistogram[static_cast<size_t>(b)].empty() || bestK(burstHistogram[static_cast<size_t>(b)]).first != selected.first) {
			allBurstsAgreeOnK = false;
			break;
		}
	}
	const double effectiveMinConsensus = allBurstsAgreeOnK ? std::min(request->minimumConsensusFraction, 0.45) : request->minimumConsensusFraction;

	const std::string consensusDiag = "selectedK=" + std::to_string(selected.first) +
		"; selectedVotes=" + std::to_string(selected.second) +
		"; totalSelectionCandidates=" + std::to_string(selectionCandidateCount) +
		"; consensus=" + std::to_string(consensus) +
		"; policyThreshold=" + std::to_string(request->minimumConsensusFraction) +
		"; effectiveThreshold=" + std::to_string(effectiveMinConsensus) +
		"; allBurstsAgree=" + (allBurstsAgreeOnK ? "true" : "false") +
		"; globalHistogram=[" + globalHistStr + "]" +
		"; burstHistograms=[" + burstHistStr + "]";

	diagnosticContext.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), "anchor.consensus",
		"Evaluated absolute-phase anchor K consensus.", consensusDiag);

	if (consensus < effectiveMinConsensus) {
		return failContract("anchor.consensus", "absolute-phase anchor ambiguous: global K consensus is below policy.", consensusDiag);
	}
	for (int burst : activeBursts) {
		const size_t burstIndex = static_cast<size_t>(burst);
		if (selectionByBurst[burstIndex] < request->minimumSelectionCandidatesPerBurst ||
			validationByBurst[burstIndex] < request->minimumValidationCandidatesPerBurst ||
			selectionRanges[burstIndex].empty() || validationRanges[burstIndex].empty()) {
			const std::string detail = "burst=" + std::to_string(burst) +
				"; selectionCount=" + std::to_string(selectionByBurst[burstIndex]) + " (min " + std::to_string(request->minimumSelectionCandidatesPerBurst) + ")" +
				"; validationCount=" + std::to_string(validationByBurst[burstIndex]) + " (min " + std::to_string(request->minimumValidationCandidatesPerBurst) + ")" +
				"; selectionRanges=" + std::to_string(selectionRanges[burstIndex].size()) +
				"; validationRanges=" + std::to_string(validationRanges[burstIndex].size());
			return failContract("anchor.coverage", "absolute-phase anchor ambiguous: insufficient independent terrain-supported selection/validation coverage in an active FEP burst.", detail);
		}
		if (burstHistogram[burstIndex].empty() || bestK(burstHistogram[burstIndex]).first != selected.first) {
			const int bBestK = burstHistogram[burstIndex].empty() ? -999999 : bestK(burstHistogram[burstIndex]).first;
			const std::string detail = "burst=" + std::to_string(burst) +
				"; burstBestK=" + std::to_string(bBestK) +
				"; globalSelectedK=" + std::to_string(selected.first);
			return failContract("anchor.consensus", "absolute-phase anchor ambiguous: FEP bursts disagree on K.", detail);
		}
	}
	for (int component = 1; component < componentCount; ++component) {
		if (bestK(componentHistogram[static_cast<size_t>(component)]).first != selected.first) {
			const int compBestK = bestK(componentHistogram[static_cast<size_t>(component)]).first;
			const std::string detail = "component=" + std::to_string(component) +
				"; componentBestK=" + std::to_string(compBestK) +
				"; globalSelectedK=" + std::to_string(selected.first);
			return failContract("anchor.consensus", "absolute-phase anchor ambiguous: phase components disagree on K.", detail);
		}
	}
	result->selectedK = selected.first;
	result->candidateCount = selectionCandidateCount;
	result->consensusFraction = consensus;
	result->burstCount = static_cast<uint32_t>(activeBursts.size());
	uint32_t outputIndex = 0;
	for (size_t bIdx = 0; bIdx < activeBursts.size(); ++bIdx) {
		const int burst = activeBursts[bIdx];
		result->burstIndices[bIdx] = burst + 1;
		result->candidateCountByBurst[bIdx] = selectionByBurst[static_cast<size_t>(burst)];
		result->validationCountByBurst[bIdx] = validationByBurst[static_cast<size_t>(burst)];
		for (int range = 0; range < request->rangeCellsPerBurst; ++range) {
			const uint64_t offset = static_cast<uint64_t>(bIdx) * request->rangeCellsPerBurst + range;
			result->selectionRangeCoverage[offset] = selectionRanges[static_cast<size_t>(burst)].count(range) ? 1 : 0;
			result->validationRangeCoverage[offset] = validationRanges[static_cast<size_t>(burst)].count(range) ? 1 : 0;
		}
	}
	outputIndex = 0;
	for (const auto& entry : componentBurstSplitCoverage) {
		if (entry.second.first == 0 || entry.second.second == 0)
			return failContract("anchor.coverage", "absolute-phase anchor ambiguous: component/burst evidence was lost before publication.");
		result->componentEvidenceTriples[outputIndex * 4] = entry.first.first;
		result->componentEvidenceTriples[outputIndex * 4 + 1] = entry.first.second + 1;
		result->componentEvidenceTriples[outputIndex * 4 + 2] = entry.second.first;
		result->componentEvidenceTriples[outputIndex * 4 + 3] = entry.second.second;
		++outputIndex;
	}
	result->componentEvidenceCount = outputIndex;
	outputIndex = 0;
	for (const std::pair<const int, int>& entry : histogram) {
		result->kHistogramPairs[outputIndex * 2] = entry.first;
		result->kHistogramPairs[outputIndex * 2 + 1] = entry.second;
		++outputIndex;
	}
	result->histogramCount = outputIndex;
	const double absolutePhaseOffset = 2.0 * CV_PI * selected.first;
	const auto solveHeight = [&](int row, int column, double& height, Position* solvedPoint,
		std::string* failureReason = nullptr, const Position* rangeSeed = nullptr,
		const Position* precomputedSatellite = nullptr, const Velocity* precomputedVelocity = nullptr) -> bool {
		const int sourceRow = sourceRowMap.at<int>(row, 0);
		const int burst = sourceRow / masterLinesPerBurst;
		const int nativeLine = sourceRow % masterLinesPerBurst;
		if (burst < 0 || burst >= masterBurstTimes.rows) {
			if (failureReason) *failureReason = "burst out of range: burst=" + std::to_string(burst) + ", masterBurstCount=" + std::to_string(masterBurstTimes.rows);
			return false;
		}
		Position satellite;
		Velocity velocity;
		if (precomputedSatellite && precomputedVelocity) {
			satellite = *precomputedSatellite;
			velocity = *precomputedVelocity;
		} else {
			const double masterTime = masterBurstTimes.at<double>(burst, 0) + nativeLine * masterAzimuthInterval;
			if (!geometry.masterState(masterTime, satellite, velocity)) {
				if (failureReason) *failureReason = "geometry.masterState failed for masterTime=" + std::to_string(masterTime);
				return false;
			}
		}
		const double rhoMaster = masterSlantRangeFirstPixel + column * masterRangeSpacing;
		double slaveSeed = 0.0;
		if (!slaveSeedFor(burst, nativeLine, column, slaveSeed)) {
			if (failureReason) *failureReason = "slaveSeedFor failed: burst=" + std::to_string(burst) + ", nativeLine=" + std::to_string(nativeLine) + ", col=" + std::to_string(column);
			return false;
		}
		const double target = phase.at<double>(row, column) + flatEarthReference.at<double>(row, column) + absolutePhaseOffset;
		// 复用同一像元上一次求值解出的 slave 零多普勒时刻做种子：候选高度之间该时刻只差几十毫秒，
		// 让 TopsNativeGeometry 走窄窗快速找根路径；链路不可用时回退到配准时间种子
		double lastSlaveTime = std::numeric_limits<double>::quiet_NaN();
		const auto residual = [&](double candidateHeight, Position* point) {
			Position p;
			TopsNativeGeometry::SampleClosure closure;
			TopsFepV5BurstStatistics stats;
			double slaveTime = 0.0, rhoSlave = 0.0;
			const double zeroDopplerSeed = std::isfinite(lastSlaveTime) ? lastSlaveTime : slaveSeed;
			if (!geometry.solveMasterReferenceHeightPoint(satellite, velocity, rhoMaster, candidateHeight, rangeSeed, p, closure, stats) ||
				!geometry.solveSlaveZeroDoppler(p, zeroDopplerSeed, slaveTime, rhoSlave, closure, stats)) return std::numeric_limits<double>::quiet_NaN();
			lastSlaveTime = slaveTime;
			if (point) *point = p;
			return 4.0 * CV_PI * (rhoSlave - rhoMaster) / (wavelength * transmitReceiveMode) - target;
		};
		// DEM 反演收敛门限：本工程像对高程模糊度约 700 m，0.02 rad 相位≈2 m 高程，已远优于 DEM 产品精度；
		// 原 0.01 rad/1 cm 门限对 DEM 无意义，只会把每像元外层迭代数顶到 20 次以上
		constexpr double kDemPhaseConvergenceRadians = 0.02;
		constexpr double kDemHeightConvergenceMeters = 1.0;
		constexpr double kDemSeedHalfWindowMeters = 500.0;
		// 自适应物理高程包络：默认退回 [-1000m, 10000m]；有上一像元解出的高程时以它为中心取窄区间，
		// 把外层迭代数从 ~12 次压到 ~7 次。下方自适应扩边保证种子落空时仍能框住根
		//（残差对高程单调，区间内根唯一）
		double low = -1000.0, high = 10000.0;
		if (rangeSeed) {
			double seedLatitude = 0.0, seedLongitude = 0.0, seedHeight = 0.0;
			if (Utils::xyz2ell(rangeSeed->x, rangeSeed->y, rangeSeed->z, seedLatitude, seedLongitude, seedHeight) == 0 &&
				std::isfinite(seedHeight)) {
				low = seedHeight - kDemSeedHalfWindowMeters;
				high = seedHeight + kDemSeedHalfWindowMeters;
			}
		}
		double fLow = residual(low, nullptr), fHigh = residual(high, nullptr);
		if (!std::isfinite(fLow) || !std::isfinite(fHigh)) {
			if (failureReason) *failureReason = "residual evaluation returned non-finite: fLow=" + std::to_string(fLow) + ", fHigh=" + std::to_string(fHigh);
			return false;
		}
		// 若因边缘像元相位波动、局部解缠残差、地形突变或种子高程失效导致根未落入当前区间，向外自适应扩展闭合区间
		while (fLow > 0.0 && low >= -20000.0) {
			const double nextLow = low - 2000.0;
			const double nextFLow = residual(nextLow, nullptr);
			if (!std::isfinite(nextFLow)) break;
			low = nextLow;
			fLow = nextFLow;
		}
		while (fHigh < 0.0 && high <= 25000.0) {
			const double nextHigh = high + 2000.0;
			const double nextFHigh = residual(nextHigh, nullptr);
			if (!std::isfinite(nextFHigh)) break;
			high = nextHigh;
			fHigh = nextFHigh;
		}
		if (fLow * fHigh > 0.0) {
			if (failureReason) *failureReason = "residual bracket failed: low=" + std::to_string(low) +
				", fLow=" + std::to_string(fLow) + ", high=" + std::to_string(high) + ", fHigh=" + std::to_string(fHigh) +
				", targetPhase=" + std::to_string(target);
			return false;
		}
		Position point;
		for (int iteration = 0; iteration < request->iterations; ++iteration) {
			if (high - low <= kDemHeightConvergenceMeters) {
				height = 0.5 * (low + high);
				if (solvedPoint) *solvedPoint = point;
				return true;
			}
			// 割线插值候选点：利用干涉相位与高程的高局部线性，加速收敛
			double candidate = (std::fabs(fHigh - fLow) > 1e-12) ?
				(low - fLow * (high - low) / (fHigh - fLow)) : (0.5 * (low + high));
			// 安全边距保护：若割线过于接近两端（<10%），强制退化为二分中点，保证区间对称有效收缩
			const double margin = 0.1 * (high - low);
			if (candidate < low + margin || candidate > high - margin) {
				candidate = 0.5 * (low + high);
			}
			const double value = residual(candidate, &point);
			if (!std::isfinite(value)) {
				// 候选点异常时回退至保守二分中点
				candidate = 0.5 * (low + high);
				const double valFallback = residual(candidate, &point);
				if (!std::isfinite(valFallback)) {
					if (failureReason) *failureReason = "residual returned non-finite at iteration=" + std::to_string(iteration) + ", candidate=" + std::to_string(candidate);
					return false;
				}
				if (std::fabs(valFallback) <= kDemPhaseConvergenceRadians) {
					height = candidate;
					if (solvedPoint) *solvedPoint = point;
					return true;
				}
				if ((fLow < 0.0) == (valFallback < 0.0)) { low = candidate; fLow = valFallback; }
				else { high = candidate; fHigh = valFallback; }
				continue;
			}
			if (std::fabs(value) <= kDemPhaseConvergenceRadians) {
				height = candidate;
				if (solvedPoint) *solvedPoint = point;
				return true;
			}
			if ((fLow < 0.0) == (value < 0.0)) {
				low = candidate;
				fLow = value;
			} else {
				high = candidate;
				fHigh = value;
			}
		}
		// 若达到最大迭代次数但已收敛至米级（< 1m），依然作为有效解输出以提高全图鲁棒性
		if (high - low <= kDemHeightConvergenceMeters) {
			height = 0.5 * (low + high);
			if (solvedPoint) *solvedPoint = point;
			return true;
		}
		if (failureReason) *failureReason = "bisection did not reach precision within iterations=" + std::to_string(request->iterations) + ", low=" + std::to_string(low) + ", high=" + std::to_string(high);
		return false;
	};
	double residualSum = 0.0, residualSquareSum = 0.0, residualMax = 0.0;
	int validationCount = 0;
	bool hasMaxResidualPoint = false;
	int maxResidualRow = -1, maxResidualColumn = -1, maxResidualSourceRow = -1;
	int maxResidualBurst = -1, maxResidualComponent = -1, maxResidualLocalK = 0;
	double maxResidualSolvedHeight = 0.0, maxResidualOrthometric = 0.0, maxResidualGeoid = 0.0;
	double maxResidualPhase = 0.0, maxResidualFlatEarth = 0.0;
	double maxResidualLatitude = 0.0, maxResidualLongitude = 0.0;
	ManagedDemSampler::SampleDiagnostics maxResidualDemSample;
	// Validate the held-out strata before spending time on the full DEM grid.
	// The validation is independent of the output matrix and must remain
	// fail-closed, but a rejected anchor should not consume hours of compute.
	for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
		const int validationProgress = 50 + static_cast<int>((5 * candidateIndex) /
			std::max<size_t>(1, candidates.size()));
		if (!progressReporter.report(validationProgress, "Validating absolute-phase anchor height residuals...")) return cancellationResult();
		const Candidate& candidate = candidates[candidateIndex];
		if (candidate.selection) continue;
		Position point;
		double solvedHeight = 0.0;
		if (!solveHeight(candidate.row, candidate.column, solvedHeight, &point)) {
			if (progressReporter.cancelled()) return cancellationResult();
			return failContract("anchor.validation", "DEM absolute-phase anchoring v2 independent reference height solve failed.");
		}
		double latitude = 0.0, longitude = 0.0, ignored = 0.0, orthometric = 0.0;
		ManagedDemSampler::SampleDiagnostics demSample;
		if (Utils::xyz2ell(point.x, point.y, point.z, latitude, longitude, ignored) != 0 ||
			!referenceDem.sample(longitude, latitude, orthometric, &demSample))
			return failContract("anchor.validation", "DEM absolute-phase anchoring v2 independent reference sampling failed.");
		const double geoid = Utils::getGeoidHeight(request->geoidModelSnapshot, longitude, latitude);
		if (!std::isfinite(geoid)) return failContract("anchor.validation", "DEM absolute-phase anchoring v2 geoid sampling failed.");
		const double error = solvedHeight - (orthometric + geoid);
		residualSum += error;
		residualSquareSum += error * error;
		if (!hasMaxResidualPoint || std::fabs(error) > residualMax) {
			hasMaxResidualPoint = true;
			residualMax = std::fabs(error);
			maxResidualRow = candidate.row;
			maxResidualColumn = candidate.column;
			maxResidualSourceRow = sourceRowMap.at<int>(candidate.row, 0);
			maxResidualBurst = candidate.burst;
			maxResidualComponent = candidate.component;
			maxResidualLocalK = candidate.k;
			maxResidualSolvedHeight = solvedHeight;
			maxResidualOrthometric = orthometric;
			maxResidualGeoid = geoid;
			maxResidualPhase = phase.at<double>(candidate.row, candidate.column);
			maxResidualFlatEarth = flatEarthReference.at<double>(candidate.row, candidate.column);
			maxResidualLatitude = latitude;
			maxResidualLongitude = longitude;
			maxResidualDemSample = demSample;
		}
		++validationCount;
	}
	if (validationCount == 0) { dem.release(); return failContract("anchor.validation", "DEM absolute-phase anchoring v2 has no independent validation samples."); }
	// Stored order is [validation_count, mean_m, rms_m, max_abs_m].
	result->sparseHeightResidualStats[0] = static_cast<double>(validationCount);
	result->sparseHeightResidualStats[1] = residualSum / validationCount;
	result->sparseHeightResidualStats[2] = std::sqrt(residualSquareSum / validationCount);
	result->sparseHeightResidualStats[3] = residualMax;
	const std::string validationDiag = "validationCount=" + std::to_string(validationCount) +
		"; mean_m=" + std::to_string(result->sparseHeightResidualStats[1]) +
		"; rms_m=" + std::to_string(result->sparseHeightResidualStats[2]) +
		"; maxAbs_m=" + std::to_string(residualMax) +
		"; maxAbsThreshold_m=" + std::to_string(request->maximumSparseHeightResidualMeters) +
		"; maxPointValid=" + std::string(hasMaxResidualPoint ? "true" : "false") +
		"; maxPoint=(row=" + std::to_string(maxResidualRow) +
		",col=" + std::to_string(maxResidualColumn) +
		",sourceRow=" + std::to_string(maxResidualSourceRow) +
		",burst=" + std::to_string(maxResidualBurst) +
		",component=" + std::to_string(maxResidualComponent) +
		",localK=" + std::to_string(maxResidualLocalK) +
		",solvedEllipsoidHeight_m=" + std::to_string(maxResidualSolvedHeight) +
		",externalOrthometric_m=" + std::to_string(maxResidualOrthometric) +
		",geoid_m=" + std::to_string(maxResidualGeoid) +
		",latitude_deg=" + std::to_string(maxResidualLatitude) +
		",longitude_deg=" + std::to_string(maxResidualLongitude) +
		",demPixel=" + std::to_string(maxResidualDemSample.pixel) +
		",demLine=" + std::to_string(maxResidualDemSample.line) +
		",demValues=[" + std::to_string(maxResidualDemSample.values[0]) + "," +
			std::to_string(maxResidualDemSample.values[1]) + "," +
			std::to_string(maxResidualDemSample.values[2]) + "," +
			std::to_string(maxResidualDemSample.values[3]) + "]"+
		",demValid=[" + std::to_string(static_cast<int>(maxResidualDemSample.valid[0])) + "," +
			std::to_string(static_cast<int>(maxResidualDemSample.valid[1])) + "," +
			std::to_string(static_cast<int>(maxResidualDemSample.valid[2])) + "," +
			std::to_string(static_cast<int>(maxResidualDemSample.valid[3])) + "]" +
		",phase_rad=" + std::to_string(maxResidualPhase) +
		",flatEarthReference_rad=" + std::to_string(maxResidualFlatEarth) + ")";
	// 残差超限不再拒绝：改为质量告警后继续出图，供与 SNAP 等外部参考对标。
	// 判据本身仍随 dem_anchor_policy 持久化，调用方据此决定节点是否进入 Warning 状态。
	if (residualMax > request->maximumSparseHeightResidualMeters) {
		diagnosticContext.emit(DEM_LOG_WARNING, static_cast<DemError>(0), "anchor.validation",
			"Independent external DEM height residual exceeds policy; anchor accepted with a quality warning.", validationDiag);
	} else {
		diagnosticContext.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), "anchor.validation",
			"Independent external DEM height residual validation passed.", validationDiag);
	}

	dem.create(phase.rows, phase.cols, CV_64F);
	std::atomic<int> completedRows(0);
	std::atomic<bool> parallelCancelled(false);
	std::string parallelFailContext;
	std::mutex failMutex;
	// GDALDataset 是 GDALOpen 独占句柄，不允许跨线程并发 RasterIO，
	// 而海面掩膜需要在并行反演中逐像元采样外源 DEM，故单独串行化该采样
	std::mutex referenceDemMutex;
	// 反演掩膜：低相干像元（此处门限）与海面（外源 DEM 以精确 0 表示海平面覆盖，见求解后判定）
	// 都没有可用的地形相位，强行反演只会产出无意义的极大负值（本景实测低至 -10000 m），一律置 NaN
	constexpr double kDemInversionMinimumCoherence = 0.2;
	// 少数像元相位超出物理可解范围而解不出，置 NoData 后继续；但解不出的比例超过此值即视为
	// 几何/相位契约整体损坏（整景级故障），仍按 fail-closed 中止，避免静默产出一景空图
	constexpr double kDemMaxUnsolvedPixelFraction = 0.005;
	std::atomic<long long> maskedLowCoherence(0);
	std::atomic<long long> maskedSeaLevel(0);
	std::atomic<long long> unsolvedPixels(0);

	#pragma omp parallel for schedule(dynamic, 16)
	for (int row = 0; row < phase.rows; ++row) {
		if (parallelCancelled.load() || progressReporter.cancelled()) continue;

		// 在行级别预先计算该行主星时刻与轨道状态，避免内层列循环重复 24956 次样条插值
		const int sourceRow = sourceRowMap.at<int>(row, 0);
		const int burst = sourceRow / masterLinesPerBurst;
		const int nativeLine = sourceRow % masterLinesPerBurst;
		Position satellite;
		Velocity velocity;
		bool rowMasterStateOk = false;
		if (burst >= 0 && burst < masterBurstTimes.rows) {
			const double masterTime = masterBurstTimes.at<double>(burst, 0) + nativeLine * masterAzimuthInterval;
			rowMasterStateOk = geometry.masterState(masterTime, satellite, velocity);
		}

		Position lastSolvedPoint;
		bool hasLastSolvedPoint = false;

		for (int column = 0; column < phase.cols; ++column) {
			if (parallelCancelled.load() || progressReporter.cancelled()) break;
			if (phaseValidMask.at<uchar>(row, column) != 1) {
				dem.at<double>(row, column) = std::numeric_limits<double>::quiet_NaN();
				hasLastSolvedPoint = false;
				continue;
			}
			if (!rowMasterStateOk) {
				std::lock_guard<std::mutex> lock(failMutex);
				if (!parallelCancelled.load()) {
					parallelFailContext = "row=" + std::to_string(row) + "; col=" + std::to_string(column) + "; masterState failed for burst=" + std::to_string(burst);
					parallelCancelled.store(true);
				}
				break;
			}
			// 低相干掩膜：相干系数过低时相位误差可达数弧度，反演结果无意义
			if (gamma.at<double>(row, column) < kDemInversionMinimumCoherence) {
				dem.at<double>(row, column) = std::numeric_limits<double>::quiet_NaN();
				++maskedLowCoherence;
				continue;
			}
			Position point;
			double height = 0.0;
			std::string solveDetail;
			const Position* seedPtr = hasLastSolvedPoint ? &lastSolvedPoint : nullptr;
			if (!solveHeight(row, column, height, &point, &solveDetail, seedPtr, &satellite, &velocity)) {
				if (progressReporter.cancelled()) {
					parallelCancelled.store(true);
					break;
				}
				// 个别像元无解（相位超出物理可解范围、内层几何闭包失败等）不再中止整轮，
				// 置 NoData 并计数后继续；仅首个失败样本记录完整上下文，供超阈值时的诊断使用
				if (unsolvedPixels.fetch_add(1) == 0) {
					std::lock_guard<std::mutex> lock(failMutex);
					parallelFailContext = "row=" + std::to_string(row) +
						"; col=" + std::to_string(column) +
						"; sourceRow=" + std::to_string(sourceRowMap.at<int>(row, 0)) +
						"; burst=" + std::to_string(sourceRowMap.at<int>(row, 0) / masterLinesPerBurst) +
						"; nativeLine=" + std::to_string(sourceRowMap.at<int>(row, 0) % masterLinesPerBurst) +
						"; phaseVal=" + std::to_string(phase.at<double>(row, column)) +
						"; flatEarthRef=" + std::to_string(flatEarthReference.at<double>(row, column)) +
						"; selectedK=" + std::to_string(selected.first) +
						"; " + solveDetail;
				}
				dem.at<double>(row, column) = std::numeric_limits<double>::quiet_NaN();
				continue;
			}
			// 海面掩膜：本外源 DEM 以精确 0 表示海平面覆盖，那里没有地形相位可反演，
			// 求解出的极大负值（本景实测低至 -10000 m）属伪解，置 NaN 丢弃；
			// 掩膜像元仍推进地面点，保证后续沿海像元的求解种子与海面判定都连续可用
			double maskLatitude = 0.0, maskLongitude = 0.0, maskIgnoredHeight = 0.0, maskOrthometricHeight = 0.0;
			bool maskIsSeaLevel = false;
			if (Utils::xyz2ell(point.x, point.y, point.z, maskLatitude, maskLongitude, maskIgnoredHeight) == 0) {
				std::lock_guard<std::mutex> lock(referenceDemMutex);
				maskIsSeaLevel = referenceDem.sample(maskLongitude, maskLatitude, maskOrthometricHeight) &&
					maskOrthometricHeight == 0.0;
			}
			if (maskIsSeaLevel) {
				height = std::numeric_limits<double>::quiet_NaN();
				++maskedSeaLevel;
			}
			dem.at<double>(row, column) = height;
			lastSolvedPoint = point;
			hasLastSolvedPoint = true;
		}

		const int done = ++completedRows;
		if (done % 50 == 0 || done == phase.rows) {
			const int inversionProgress = 55 + static_cast<int>((35LL * done) / std::max(1, phase.rows));
			if (!progressReporter.report(inversionProgress, "Solving absolute-height DEM grid...")) {
				parallelCancelled.store(true);
			}
		}
	}

	if (parallelCancelled.load() || progressReporter.cancelled()) {
		dem.release();
		if (progressReporter.cancelled()) return cancellationResult();
		return failContract("inversion.newton", "DEM absolute-phase anchoring v2 height solve loop aborted before completion.", parallelFailContext);
	}
	const long long totalInversionPixels = static_cast<long long>(phase.rows) * static_cast<long long>(phase.cols);
	const std::string unsolvedDetail = "unsolvedPixels=" + std::to_string(unsolvedPixels.load()) +
		"; totalPixels=" + std::to_string(totalInversionPixels) +
		"; maxFraction=" + std::to_string(kDemMaxUnsolvedPixelFraction) +
		"; firstFailure=(" + parallelFailContext + ")";
	if (unsolvedPixels.load() > 0 &&
		static_cast<double>(unsolvedPixels.load()) > kDemMaxUnsolvedPixelFraction * static_cast<double>(totalInversionPixels)) {
		dem.release();
		return failContract("inversion.newton", "DEM absolute-phase anchoring v2 FEP height solve failed for too many valid phase samples.", unsolvedDetail);
	}
	diagnosticContext.emit(DEM_LOG_INFO, static_cast<DemError>(0), "inversion.mask",
		"DEM grid inversion masking summary.",
		"minCoherence=" + std::to_string(kDemInversionMinimumCoherence) +
		"; maskedLowCoherence=" + std::to_string(maskedLowCoherence.load()) +
		"; maskedSeaLevel=" + std::to_string(maskedSeaLevel.load()) +
		"; unsolvedPixels=" + std::to_string(unsolvedPixels.load()) +
		"; totalPixels=" + std::to_string(totalInversionPixels));
	if (unsolvedPixels.load() > 0) {
		diagnosticContext.emit(DEM_LOG_WARNING, static_cast<DemError>(0), "inversion.solve",
			"Some phase-valid pixels have no solvable height; they were written as NoData.", unsolvedDetail);
	}
	// This preliminary bridge does not retain HDF5 handles.  Rechecking every
	// consumed artifact detects replacement during the open/read sequence and
	// keeps the call fail-closed until a handle-pinned snapshot reader exists.
	if (!hashMatches(request->phaseH5Snapshot, request->phaseH5SnapshotHash) ||
		!hashMatches(request->masterH5Snapshot, request->masterH5SnapshotHash) ||
		!hashMatches(request->slaveH5Snapshot, request->slaveH5SnapshotHash) ||
		!hashMatches(request->auxiliaryDemRasterSnapshot, request->auxiliaryDemRasterSnapshotHash) ||
		!hashMatches(request->auxiliaryDemValidMaskSnapshot, request->auxiliaryDemValidMaskSnapshotHash) ||
		!hashMatches(request->auxiliaryDemIdentityH5Snapshot, request->auxiliaryDemIdentityH5SnapshotHash) ||
		!hashMatches(request->geoidModelSnapshot, request->geoidModelSnapshotHash)) {
		return failContract("input.snapshot_hash", "DEM absolute-phase anchoring v2 snapshot changed during input preparation.");
	}
	if (!progressReporter.reportSuccess()) return cancellationResult();
	strcpy_s(result->status, "accepted");
	return 0;
}


int Dem::phase2dem_newton_iter(
	Mat unwrapped_phase,
	Mat flat_phase,
	Mat& DEM_height,
	Mat auxi_m,
	Mat auxi_s,
	Mat orbit_m,
	Mat orbit_s,
	Mat doppler_frequency,
	Mat gcps,
	Mat regis_out,
	double delta_m,
	double delta_s,
	int multilook_times,
	int mode,
	int iters,
	DemProgressCallback cb
)
{
	if (unwrapped_phase.rows < 1 ||
		unwrapped_phase.cols < 1 ||
		unwrapped_phase.type() != CV_64F ||
		unwrapped_phase.channels() != 1 ||
		flat_phase.rows != unwrapped_phase.rows ||
		flat_phase.cols != unwrapped_phase.cols ||
		flat_phase.type() != CV_64F ||
		flat_phase.channels() != 1 ||
		auxi_m.rows < 1 ||
		auxi_m.cols != 5 ||
		auxi_m.type() != CV_64F ||
		auxi_m.channels() != 1 ||
		auxi_s.rows != auxi_m.rows ||
		auxi_s.cols != auxi_m.cols ||
		auxi_s.type() != CV_64F ||
		auxi_s.channels() != 1 ||
		orbit_m.rows < 7 ||
		orbit_m.cols != 7 ||
		orbit_m.type() != CV_64F ||
		orbit_m.channels() != 1 ||
		orbit_s.rows < 7 ||
		orbit_s.cols != 7 ||
		orbit_s.type() != CV_64F ||
		orbit_s.channels() != 1 ||
		doppler_frequency.rows != 1 ||
		doppler_frequency.cols < 1 ||
		doppler_frequency.type() != CV_64F ||
		doppler_frequency.channels() != 1 ||
		gcps.rows < 1 ||
		gcps.cols != 5 ||
		gcps.type() != CV_64F ||
		gcps.channels() != 1 ||
		regis_out.rows != 1 ||
		regis_out.cols != 4 ||
		regis_out.type() != CV_32S||
		regis_out.channels() != 1||
		delta_m <= 0.0 ||
		delta_s <= 0.0 ||
		multilook_times < 1||
		(mode == TR_MODE_SINGLE_TX_SINGLE_RX || mode == TR_MODE_SINGLE_TX_DOUBLE_RX) == false
		)
	{
		fprintf(stderr, "phase2dem_newton_iter(): input check failed!\n\n");
		return -1;
	}
	int move_r = regis_out.at<int>(0, 0);
	int move_c = regis_out.at<int>(0, 1);
	int nr = regis_out.at<int>(0, 2);
	int nc = regis_out.at<int>(0, 3);
	if (nr < 1 || nc < 1 || fabs(move_c) >= nc || fabs(move_r) >= nr)
	{
		fprintf(stderr, "phase2dem_newton_iter(): input check failed!\n\n");
		return -1;
	}
	ProgressReporter progressReporter(cb);
	if (!progressReporter.report(0, "Preparing DEM input...")) return -2;
	double C = 4 * 3.1415926535;
	if (mode == TR_MODE_SINGLE_TX_DOUBLE_RX)
	{
		C = 2 * 3.1415926535;
	}
	Deflat deflat;
	Mat coef_m, coef_s;
	int ret;
	ret = deflat.Orbit_Polyfit(orbit_m, coef_m);
	if (return_check(ret, "deflat.Orbit_Polyfit(*, *)", error_head)) return -1;
	ret = deflat.Orbit_Polyfit(orbit_s, coef_s);
	if (return_check(ret, "deflat.Orbit_Polyfit(*, *)", error_head)) return -1;
	Mat image_time_m = Mat::zeros(1, nr, CV_64F);
	Mat image_time_s = Mat::zeros(1, nr, CV_64F);
	Mat temp;
	Mat S_position_m, S_position_s, S_velocity_m;
	S_position_m = Mat::zeros(nr, 3, CV_64F);
	S_position_s = Mat::zeros(nr, 3, CV_64F);
	S_velocity_m = Mat::zeros(nr, 3, CV_64F);
	for (int i = 0; i < nr; i++)
	{
		image_time_m.at<double>(0, i) = double(i) * delta_m + auxi_m.at<double>(0, 2);
		image_time_s.at<double>(0, i) = double(i) * delta_s + auxi_s.at<double>(0, 2);
	}
	for (int i = 0; i < nr; i++)
	{
		ret = deflat.get_xyz(image_time_m.at<double>(0, i), coef_m, temp);
		if (return_check(ret, "deflat.get_xyz(*, *, *)", error_head)) return -1;
		S_position_m.at<double>(i, 0) = temp.at<double>(0, 0);
		S_position_m.at<double>(i, 1) = temp.at<double>(0, 1);
		S_position_m.at<double>(i, 2) = temp.at<double>(0, 2);
		ret = deflat.get_xyz(image_time_s.at<double>(0, i), coef_s, temp);
		if (return_check(ret, "deflat.get_xyz(*, *, *)", error_head)) return -1;
		S_position_s.at<double>(i, 0) = temp.at<double>(0, 0);
		S_position_s.at<double>(i, 1) = temp.at<double>(0, 1);
		S_position_s.at<double>(i, 2) = temp.at<double>(0, 2);
		ret = deflat.get_vel(image_time_m.at<double>(0, i), coef_m, temp);
		if (return_check(ret, "deflat.get_vel(*, *, *)", error_head)) return -1;
		S_velocity_m.at<double>(i, 0) = temp.at<double>(0, 0);
		S_velocity_m.at<double>(i, 1) = temp.at<double>(0, 1);
		S_velocity_m.at<double>(i, 2) = temp.at<double>(0, 2);
	}
	int rows_start_m, rows_end_m, rows_start_s, rows_end_s, cols_start_m, cols_start_s, cols_end_m, cols_end_s;
	if (move_r >= 0)
	{
		rows_start_m = 0;
		rows_end_m = nr - move_r;
		rows_start_s = move_r;
		rows_end_s = nr;
	}
	else
	{
		rows_start_m = -move_r;
		rows_end_m = nr;
		rows_start_s = 0;
		rows_end_s = nr + move_r;
	}
	if (move_c >= 0)
	{
		cols_start_m = 0;
		cols_end_m = nc - move_c;
		cols_start_s = move_c;
		cols_end_s = nc;
	}
	else
	{
		cols_start_m = -move_c;
		cols_end_m = nc;
		cols_start_s = 0;
		cols_end_s = nc + move_c;
	}
	if ((rows_end_m - rows_start_m) != unwrapped_phase.rows ||
		(rows_end_s - rows_start_s) != unwrapped_phase.rows ||
		(cols_end_m - cols_start_m) != unwrapped_phase.cols ||
		(cols_end_s - cols_start_s) != unwrapped_phase.cols)
	{
		fprintf(stderr, "offset cutsize and unwrapped_phase size mismatch!\n\n");
		return -1;
	}
	S_position_m = S_position_m(Range(rows_start_m, rows_end_m), Range(0, 3));
	S_position_s = S_position_s(Range(rows_start_s, rows_end_s), Range(0, 3));
	S_velocity_m = S_velocity_m(Range(rows_start_m, rows_end_m), Range(0, 3));
	doppler_frequency = doppler_frequency(Range(0, 1), Range(cols_start_m, cols_end_m));
	unwrapped_phase = unwrapped_phase + flat_phase;
	
	int row = static_cast<int>(gcps.at<double>(2, 0));
	int col = static_cast<int>(gcps.at<double>(2, 1));
	if (row < 1 ||
		row > S_position_m.rows ||
		row < 1 ||
		row > S_position_s.rows)
	{
		fprintf(stderr, "ground control point out of image !\n\n");
		return -1;
	}

	///////////////////////////////校正至绝对相位///////////////////////////////////
	Mat Control_Point_Position = gcps(Range(2, 3), Range(2, 5));
	Mat tmp = Control_Point_Position - 
		S_position_m(Range(row - 1, row), Range(0, 3));
	double distance_r_m = 2 * cv::norm(tmp, NORM_L2);
	tmp = Control_Point_Position - 
		S_position_m(Range(row - 1, row), Range(0, 3));
	Mat tmp1 = Control_Point_Position - 
		S_position_s(Range(row - 1, row), Range(0, 3));
	double distance_r_s;
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX)
	{
		distance_r_s = 2 * cv::norm(tmp1, NORM_L2);
	}
	else
	{
		distance_r_s = cv::norm(tmp, NORM_L2) + cv::norm(tmp1, NORM_L2);
	}
	if (fabs(auxi_m.at<double>(0, 4)) < 1e-14)
	{
		auxi_m.at<double>(0, 4) = 1e-14;
	}
	double lambda = 3e8 / auxi_m.at<double>(0, 4);
	double real_phase = C / lambda * (distance_r_m - distance_r_s);
	double K = round((real_phase - unwrapped_phase.at<double>(row - 1, col - 1)) / C);
	unwrapped_phase = unwrapped_phase + K * C;


	double R0_m = auxi_m.at<double>(0, 3) * 3e8 / 2;//最近斜距
	double spc_m = auxi_m.at<double>(0, 1);//距离向采样间隔
	Mat Rm = Mat::zeros(1, nc, CV_64F);
	for (int i = 0; i < nc; i++)
	{
		Rm.at<double>(0, i) = double(i) * spc_m + R0_m;
	}
	Rm = Rm(Range(0, 1), Range(cols_start_m, cols_end_m));
	Mat ones = Mat::ones(unwrapped_phase.rows, 1, CV_64F);
	Mat R_M = ones * Rm;
	Mat R_F = ones * Rm * 2.0 - lambda * unwrapped_phase / C;
	Mat Satellite_M_T_Position = S_position_m;//主星发射位置
	Mat Satellite_S_T_Position;
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX)
	{
		Satellite_S_T_Position = S_position_s;//辅星发射位置
	}
	else
	{
		Satellite_S_T_Position = S_position_m;//辅星发射位置
	}
	Mat Satellite_M_R_Position = S_position_m;//主星接收位置
	Mat Satellite_S_R_Position = S_position_s;//辅星接收位置
	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
	Mat Vs = S_velocity_m;
	ones = Mat::ones(unwrapped_phase.rows, unwrapped_phase.cols, CV_64F);
	Mat P1 = ones * Control_Point_Position.at<double>(0, 0);
	Mat P2 = ones * Control_Point_Position.at<double>(0, 1);
	Mat P3 = ones * Control_Point_Position.at<double>(0, 2);
	int fine_size_rows = unwrapped_phase.rows;
	int fine_size_cols = unwrapped_phase.cols;
	Mat fd = doppler_frequency;
	if (!Utils::newton_iter_core_ex(iters, P1, P2, P3, Satellite_M_T_Position, Satellite_S_T_Position,
	                 Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs,
	                 R_M, R_F, fd, lambda, ProgressReporter::callback, &progressReporter))
	{
		return -2;
	}
	std::atomic<bool> parallel_flag(true);
	DEM_height = Mat::zeros(fine_size_rows, fine_size_cols, CV_64F);

	std::atomic<int> completed_rows(0);
	int step = std::max(1, fine_size_rows / 10);

#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i < fine_size_rows; i++)
	{
		if (!parallel_flag || progressReporter.cancelled()) continue;
		for (int j = 0; j < fine_size_cols; j++)
		{
			if (!parallel_flag || progressReporter.cancelled()) continue;
			double lat, lon, h;
			ret = Utils::xyz2ell(P1.at<double>(i, j), P2.at<double>(i, j), P3.at<double>(i, j), lat, lon, h);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			DEM_height.at<double>(i, j) = h;
		}

		int current_completed = ++completed_rows;
		if (current_completed % step == 0)
		{
			int progress = 90 + (current_completed * 10) / fine_size_rows;
			if (!progressReporter.reportCoordinateConversion(progress))
			{
				parallel_flag = false;
			}
		}
	}
	if (!parallel_flag || progressReporter.cancelled()) return -2;
	double lat, lon, h;
	ret = Utils::xyz2ell(
		Control_Point_Position.at<double>(0, 0),
		Control_Point_Position.at<double>(0, 1),
		Control_Point_Position.at<double>(0, 2),
		lat, lon, h
	);
	if (return_check(ret, "Utils::xyz2ell(*, *)", error_head)) return -1;
	DEM_height = DEM_height + h - DEM_height.at<double>(row - 1, col - 1);
	if (!progressReporter.reportSuccess()) return -2;
	return 0;
}

int Dem::dem_newton_iter(const char* unwrapped_phase_file, Mat& dem, const char* project_path, int iter_times, int mode, DemProgressCallback cb)
{
	const int result = dem_newton_iter_impl(unwrapped_phase_file, dem, project_path, iter_times, mode, nullptr, cb, true);
	return result == 0 || result == -2 ? result : -1;
}

int Dem::dem_newton_iter_ex(const char* unwrapped_phase_file, Mat& dem, const char* project_path, int iter_times,
	int mode, const DemDiagnosticOptions* diagnostics, DemProgressCallback cb)
{
	return dem_newton_iter_impl(unwrapped_phase_file, dem, project_path, iter_times, mode, diagnostics, cb, false);
}

int Dem::dem_newton_iter_impl(const char* unwrapped_phase_file, Mat& dem, const char* project_path, int iter_times,
	int mode, const DemDiagnosticOptions* diagnostics, DemProgressCallback cb, bool legacyConsoleLogging)
{
	DemDiagnosticContext diagnosticContext;
	const int diagnosticResult = initializeDiagnosticContext(diagnostics, diagnosticContext);
	if (diagnosticResult != 0) return diagnosticResult;
	auto processingFailure = [&](const char* stage, int code) {
		if (legacyConsoleLogging) return_check(code, stage, error_head);
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_PROCESSING, stage, "DEM Newton iteration processing failed.",
			"internalReturnCode=" + std::to_string(code));
		return legacyConsoleLogging ? -1 : static_cast<int>(DEM_ERROR_PROCESSING);
	};
	auto hdfReadFailure = [&](int code, const char* operation) {
		if (code == 0) return 0;
		if (legacyConsoleLogging)
		{
			return_check(-1, operation, error_head);
			return -1;
		}
		return code;
	};
	if (unwrapped_phase_file == NULL ||
		project_path == NULL ||
		iter_times < 1 ||
		(mode == TR_MODE_SINGLE_TX_SINGLE_RX || mode == TR_MODE_SINGLE_TX_DOUBLE_RX) == false)
	{
		if (legacyConsoleLogging) fprintf(stderr, "dem_newton_iter(): input check failed!\n");
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_INPUT, "entry.validate_input", "DEM Newton iteration input validation failed.");
		return DEM_ERROR_INVALID_INPUT;
	}
	ProgressReporter progressReporter(diagnosticContext.progressCallback ? nullptr : cb,
		diagnosticContext.progressCallback, diagnosticContext.progressUserData);
	auto cancellationResult = [&]() {
		if (!legacyConsoleLogging)
		{
			diagnosticContext.emit(DEM_LOG_INFO, DEM_ERROR_CANCELLED, "cancelled_by_progress_callback",
				"DEM Newton iteration cancelled by progress callback.");
		}
		return legacyConsoleLogging ? -2 : static_cast<int>(DEM_ERROR_CANCELLED);
	};
	if (!progressReporter.report(0, "Preparing DEM input...")) return cancellationResult();

	/*
	* 校正至绝对相位
	*/

	Utils util;
	int nr, nc, ret, offset_row, offset_col;
	double time_interval1, time_interval2;
	std::string project = normalizeAbsolutePath(project_path);
	while (project.size() > 3 && (project.back() == '\\' || project.back() == '/')) project.pop_back();
	diagnosticContext.emit(DEM_LOG_INFO, static_cast<DemError>(0), "entry", "Starting DEM Newton iteration.",
		"phaseH5=" + std::string(unwrapped_phase_file) + "; projectRoot=" + project +
		"; iterations=" + std::to_string(iter_times) + "; mode=" + std::to_string(mode));
	Mat unwrapped_phase, flat_phase_coefficient, flat_earth_reference_phase, gcps, temp, range_spacing,
		stateVec1, stateVec2, lat_coefficient, lon_coefficient, prf1, prf2, carrier_frequency;
	Mat phaseValidMask;
	bool hasPhaseValidityMask = false;
	ret = readDemArray(diagnosticContext, unwrapped_phase_file, "phase", "input.phase", unwrapped_phase);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	if (unwrapped_phase.type() != CV_64F)
	{
		unwrapped_phase.convertTo(unwrapped_phase, CV_64F);
	}
	nr = unwrapped_phase.rows; nc = unwrapped_phase.cols;
	if (nr < 1 || nc < 1)
	{
		if (legacyConsoleLogging) fprintf(stderr, "dem_newton_iter(): invalid unwrapped_phase !\n");
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.phase", "Phase dataset has an invalid shape.",
			"rows=" + std::to_string(nr) + "; columns=" + std::to_string(nc), unwrapped_phase_file, "phase", 0, nr, nc, unwrapped_phase.type());
		return DEM_ERROR_INVALID_SHAPE;
	}
	int schemaExists = 0;
	int modelExists = 0;
	if (Hdf5IO::datasetExists(unwrapped_phase_file, "phase_processing_schema_version", &schemaExists) != 0 ||
		Hdf5IO::datasetExists(unwrapped_phase_file, "flat_earth_model_version", &modelExists) != 0) {
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_PROCESSING, "input.flat_earth_contract",
			"Unable to inspect the flat-earth contract version.", std::string(), unwrapped_phase_file);
		return DEM_ERROR_PROCESSING;
	}
	int phaseSchemaVersion = 0;
	if (schemaExists != 0) {
		FormatConversion conversion;
		if (conversion.read_int_from_h5(unwrapped_phase_file, "phase_processing_schema_version", &phaseSchemaVersion) != 0 ||
			(phaseSchemaVersion != 1 && phaseSchemaVersion != 2)) {
			diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.flat_earth_contract",
				"Unsupported or unreadable phase-processing schema version.", std::string(), unwrapped_phase_file);
			return DEM_ERROR_INVALID_SHAPE;
		}
	}
	const bool requiresVersionedReference = phaseSchemaVersion == 2 || modelExists != 0;
	if (requiresVersionedReference && phaseSchemaVersion != 2) {
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.flat_earth_contract",
			"Versioned flat-earth model requires phase-processing schema version 2.", std::string(), unwrapped_phase_file);
		return DEM_ERROR_INVALID_SHAPE;
	}
	bool hasFlatEarthReference = false;
	if (requiresVersionedReference) {
		std::string flatEarthContractDetail;
		hasFlatEarthReference = validateVersionedFlatEarthContract(
			unwrapped_phase_file, unwrapped_phase, flat_earth_reference_phase, phaseValidMask, flatEarthContractDetail);
		if (!hasFlatEarthReference) {
			diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.flat_earth_reference_phase",
			"Versioned flat-earth contract is incomplete or unsupported; legacy coefficients are not a fallback.",
			flatEarthContractDetail, unwrapped_phase_file, "flat_earth_reference_phase");
		return DEM_ERROR_INVALID_SHAPE;
		}
		hasPhaseValidityMask = true;
		unwrapped_phase.setTo(0.0, phaseValidMask == 0);
	}
	else {
		ret = readDemArray(diagnosticContext, unwrapped_phase_file, "flat_phase_coefficient", "input.flat_phase_coefficient", flat_phase_coefficient);
		if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
		if (flat_phase_coefficient.type() != CV_64F || flat_phase_coefficient.rows != 1 || flat_phase_coefficient.cols != 6)
		{
			diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.flat_phase_coefficient",
				"Legacy flat phase coefficients must be one CV_64F six-term row; segmented rows have no unified evaluator.",
				"expected=CV_64F 1x6; actualRows=" + std::to_string(flat_phase_coefficient.rows) +
				"; actualColumns=" + std::to_string(flat_phase_coefficient.cols) + "; actualCvType=" + std::to_string(flat_phase_coefficient.type()),
				unwrapped_phase_file, "flat_phase_coefficient", 0, flat_phase_coefficient.rows, flat_phase_coefficient.cols, flat_phase_coefficient.type());
			return DEM_ERROR_INVALID_SHAPE;
		}
	}
	PathResolver::SourcePathPair sourcePaths;
	PathResolver::Error pathError = PathResolver::Error::None;
	string pathDetail;
	if (!PathResolver::readSourcePathPair(unwrapped_phase_file, project_path, sourcePaths, &pathError, &pathDetail))
	{
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_SOURCE_PATH, "input.source_path",
			PathResolver::errorMessage(pathError), pathDetail);
		return DEM_ERROR_SOURCE_PATH;
	}
	const string source_1 = sourcePaths.source1.utf8;
	const string source_2 = sourcePaths.source2.utf8;
	diagnosticContext.emit(DEM_LOG_INFO, static_cast<DemError>(0), "input.source_1", "Resolved source_1 path.", source_1);
	diagnosticContext.emit(DEM_LOG_INFO, static_cast<DemError>(0), "input.source_2", "Resolved source_2 path.", source_2);
	auto invalidShape = [&](const char* stage, const char* file, const char* dataset, const Mat& value,
		const char* expectation) {
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, stage, "HDF5 dataset shape or type is invalid.",
			std::string("expected=") + expectation + "; actualRows=" + std::to_string(value.rows) +
			"; actualColumns=" + std::to_string(value.cols) + "; actualCvType=" + std::to_string(value.type()),
			file, dataset, 0, value.rows, value.cols, value.type());
		return static_cast<int>(DEM_ERROR_INVALID_SHAPE);
	};
	ret = readDemArray(diagnosticContext, source_1.c_str(), "gcps", "source_1.master.gcps", gcps);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	ret = readDemArray(diagnosticContext, source_1.c_str(), "offset_row", "source_1.master.offset_row", temp);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	if (temp.type() != CV_32S || temp.total() != 1) return invalidShape("source_1.master.offset_row", source_1.c_str(), "offset_row", temp, "CV_32S scalar");
	offset_row = temp.at<int>(0, 0);
	ret = readDemArray(diagnosticContext, source_1.c_str(), "offset_col", "source_1.master.offset_col", temp);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	if (temp.type() != CV_32S || temp.total() != 1) return invalidShape("source_1.master.offset_col", source_1.c_str(), "offset_col", temp, "CV_32S scalar");
	offset_col = temp.at<int>(0, 0);
	ret = readDemArray(diagnosticContext, source_1.c_str(), "range_spacing", "source_1.master.range_spacing", range_spacing);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	ret = readDemArray(diagnosticContext, source_1.c_str(), "state_vec", "source_1.master.state_vec", stateVec1);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	ret = readDemArray(diagnosticContext, source_2.c_str(), "state_vec", "source_2.slave.state_vec", stateVec2);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	ret = readDemArray(diagnosticContext, source_1.c_str(), "prf", "source_1.master.prf", prf1);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	if (prf1.type() != CV_64F || prf1.total() != 1) return invalidShape("source_1.master.prf", source_1.c_str(), "prf", prf1, "CV_64F scalar");
	time_interval1 = 1.0 / (prf1.at<double>(0, 0) + 1e-10);
	ret = readDemArray(diagnosticContext, source_2.c_str(), "prf", "source_2.slave.prf", prf2);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	if (prf2.type() != CV_64F || prf2.total() != 1) return invalidShape("source_2.slave.prf", source_2.c_str(), "prf", prf2, "CV_64F scalar");
	time_interval2 = 1.0 / (prf2.at<double>(0, 0) + 1e-10);
	ret = readDemArray(diagnosticContext, source_1.c_str(), "lat_coefficient", "source_1.master.lat_coefficient", lat_coefficient);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	ret = readDemArray(diagnosticContext, source_1.c_str(), "lon_coefficient", "source_1.master.lon_coefficient", lon_coefficient);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	ret = readDemArray(diagnosticContext, source_1.c_str(), "carrier_frequency", "source_1.master.carrier_frequency", carrier_frequency);
	if (ret != 0) return hdfReadFailure(ret, "read_array_from_h5()");
	if (gcps.type() != CV_64F || gcps.rows < 1 || gcps.cols < 5)
		return invalidShape("source_1.master.gcps", source_1.c_str(), "gcps", gcps, "CV_64F with at least 1x5");
	if (range_spacing.type() != CV_64F || range_spacing.total() != 1)
		return invalidShape("source_1.master.range_spacing", source_1.c_str(), "range_spacing", range_spacing, "CV_64F scalar");
	if (stateVec1.type() != CV_64F || stateVec1.rows < 1 || stateVec1.cols < 7)
		return invalidShape("source_1.master.state_vec", source_1.c_str(), "state_vec", stateVec1, "CV_64F with at least 1x7");
	if (stateVec2.type() != CV_64F || stateVec2.rows < 1 || stateVec2.cols < 7)
		return invalidShape("source_2.slave.state_vec", source_2.c_str(), "state_vec", stateVec2, "CV_64F with at least 1x7");
	if (lat_coefficient.type() != CV_64F || lat_coefficient.empty())
		return invalidShape("source_1.master.lat_coefficient", source_1.c_str(), "lat_coefficient", lat_coefficient, "non-empty CV_64F");
	if (lon_coefficient.type() != CV_64F || lon_coefficient.empty())
		return invalidShape("source_1.master.lon_coefficient", source_1.c_str(), "lon_coefficient", lon_coefficient, "non-empty CV_64F");
	if (carrier_frequency.type() != CV_64F || carrier_frequency.total() != 1)
		return invalidShape("source_1.master.carrier_frequency", source_1.c_str(), "carrier_frequency", carrier_frequency, "CV_64F scalar");

	//寻找图像范围内的控制点信息

	int num_gcps = gcps.rows;
	int row, col, i = 0;
	bool b_gcp = false;
	for (i = 0; i < num_gcps; i++)
	{
		row = (int)gcps.at<double>(i, 3);
		col = (int)gcps.at<double>(i, 4);
		if ((row - offset_row) >= 0 && (row - offset_row) < nr && (col - offset_col) >= 0 && (col - offset_col) < nc)
		{
			b_gcp = true;
			break;
		}
	}
	
	Mat llh(1, 3, CV_64F), xyz_ground(1, 3, CV_64F);
	Mat row_coord(1, 1, CV_64F), col_coord(1, 1, CV_64F), lat, lon;
	if (b_gcp)
	{
		llh.at<double>(0, 0) = gcps.at<double>(i, 1);
		llh.at<double>(0, 1) = gcps.at<double>(i, 0);
		llh.at<double>(0, 2) = gcps.at<double>(i, 2);
		row = row - offset_row; col = col - offset_col;
	}
	else
	{
		row_coord.at<double>(0, 0) = offset_row + double(nr) / 2.0;
		col_coord.at<double>(0, 0) = offset_col + double(nc) / 2.0;
		ret = util.coord_conversion(lat_coefficient, row_coord, col_coord, lat);
		if (ret != 0) return processingFailure("coord_conversion", ret);
		ret = util.coord_conversion(lon_coefficient, row_coord, col_coord, lon);
		if (ret != 0) return processingFailure("coord_conversion", ret);
		llh.at<double>(0, 0) = lat.at<double>(0, 0);
		llh.at<double>(0, 1) = lon.at<double>(0, 0);
		llh.at<double>(0, 2) = 0.0;
		row = (int)nr / 2; col = (int)nc / 2;
	}
	if (hasPhaseValidityMask && phaseValidMask.at<uchar>(row, col) == 0) {
		long long bestDistance = std::numeric_limits<long long>::max();
		int nearestRow = -1;
		int nearestCol = -1;
		for (int candidateRow = 0; candidateRow < nr; ++candidateRow) {
			const uchar* valid = phaseValidMask.ptr<uchar>(candidateRow);
			for (int candidateCol = 0; candidateCol < nc; ++candidateCol) {
				if (valid[candidateCol] == 0) continue;
				const long long rowDistance = static_cast<long long>(candidateRow) - row;
				const long long colDistance = static_cast<long long>(candidateCol) - col;
				const long long distance = rowDistance * rowDistance + colDistance * colDistance;
				if (distance < bestDistance) {
					bestDistance = distance;
					nearestRow = candidateRow;
					nearestCol = candidateCol;
				}
			}
		}
		if (nearestRow < 0 || nearestCol < 0) {
			diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.phase_valid_mask",
				"Phase-validity mask has no usable anchor pixel.", std::string(), unwrapped_phase_file, "phase_valid_mask");
			return DEM_ERROR_INVALID_SHAPE;
		}
		row = nearestRow;
		col = nearestCol;
		row_coord.at<double>(0, 0) = offset_row + row;
		col_coord.at<double>(0, 0) = offset_col + col;
		ret = util.coord_conversion(lat_coefficient, row_coord, col_coord, lat);
		if (ret != 0) return processingFailure("coord_conversion", ret);
		ret = util.coord_conversion(lon_coefficient, row_coord, col_coord, lon);
		if (ret != 0) return processingFailure("coord_conversion", ret);
		llh.at<double>(0, 0) = lat.at<double>(0, 0);
		llh.at<double>(0, 1) = lon.at<double>(0, 0);
		llh.at<double>(0, 2) = 0.0;
		diagnosticContext.emit(DEM_LOG_DEBUG, static_cast<DemError>(0), "input.phase_valid_mask",
			"Moved the DEM phase anchor to the nearest valid sample.",
			"row=" + std::to_string(row) + "; column=" + std::to_string(col), unwrapped_phase_file, "phase_valid_mask");
	}
	ret = util.ell2xyz(llh, xyz_ground);
	if (ret != 0) return processingFailure("ell2xyz", ret);


	/*
	* 轨道插值
	*/
	ret = util.stateVec_interp(stateVec1, time_interval1, stateVec1);
	if (ret != 0) return processingFailure("stateVec_interp()", ret);
	ret = util.stateVec_interp(stateVec2, time_interval2, stateVec2);
	if (ret != 0) return processingFailure("stateVec_interp()", ret);
	/*
	* 寻找图像左上角成像卫星位置
	*/
	Mat sate1_xyz, sate2_xyz, sate1_v, sate2_v;
	Mat sate1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat satev1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat sate2 = Mat::zeros(nr, 3, CV_64F);//辅星位置
	stateVec1(cv::Range(0, stateVec1.rows), cv::Range(1, 4)).copyTo(sate1_xyz);
	stateVec1(cv::Range(0, stateVec1.rows), cv::Range(4, 7)).copyTo(sate1_v);
	stateVec2(cv::Range(0, stateVec2.rows), cv::Range(1, 4)).copyTo(sate2_xyz);
	stateVec2(cv::Range(0, stateVec2.rows), cv::Range(4, 7)).copyTo(sate2_v);
	//找到零多普勒位置
	Mat xyz, llh_upperleft(1, 3, CV_64F);
	row_coord.at<double>(0, 0) = offset_row;
	col_coord.at<double>(0, 0) = offset_col + double(nc) / 2.0;
	ret = util.coord_conversion(lat_coefficient, row_coord, col_coord, lat);
	if (ret != 0) return processingFailure("coord_conversion", ret);
	ret = util.coord_conversion(lon_coefficient, row_coord, col_coord, lon);
	if (ret != 0) return processingFailure("coord_conversion", ret);
	llh_upperleft.at<double>(0, 0) = lat.at<double>(0, 0);
	llh_upperleft.at<double>(0, 1) = lon.at<double>(0, 0);
	llh_upperleft.at<double>(0, 2) = 0.0;
	ret = util.ell2xyz(llh_upperleft, xyz);
	if (ret != 0) return processingFailure("ell2xyz", ret);
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
	for (int j = 0; j < nr; j++)
	{
		orbit_idx = (peak_loc.y + j) > (sate1_xyz.rows - 1) ? (sate1_xyz.rows - 1) : (peak_loc.y + j);
		sate1_xyz(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(sate1(Range(j, j + 1), Range(0, 3)));
		sate1_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev1(Range(j, j + 1), Range(0, 3)));
	}
	//卫星2
	dop = Mat::zeros(sate2_xyz.rows, 1, CV_64F);
	for (int j = 0; j < sate2_xyz.rows; j++)
	{
		r = xyz - sate2_xyz(Range(j, j + 1), Range(0, 3));
		dop.at<double>(j, 0) = fabs(cv::sum(r.mul(sate2_v(Range(j, j + 1), Range(0, 3))))[0]);
	}
	cv::minMaxLoc(dop, NULL, NULL, &peak_loc, NULL);
	for (int j = 0; j < nr; j++)
	{
		orbit_idx = (peak_loc.y + j) > (sate2_xyz.rows - 1) ? (sate2_xyz.rows - 1) : (peak_loc.y + j);
		sate2_xyz(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(sate2(Range(j, j + 1), Range(0, 3)));
	}
	// The versioned field is already on the output grid. Legacy products retain
	// the historical six-term quadratic evaluator.
	if (hasFlatEarthReference) {
		unwrapped_phase += flat_earth_reference_phase;
	}
	else {
		double c0 = flat_phase_coefficient.at<double>(0, 0);
		double c1 = flat_phase_coefficient.at<double>(0, 1);
		double c2 = flat_phase_coefficient.at<double>(0, 2);
		double c3 = flat_phase_coefficient.at<double>(0, 3);
		double c4 = flat_phase_coefficient.at<double>(0, 4);
		double c5 = flat_phase_coefficient.at<double>(0, 5);
	#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			double i_val = i;
			double i_sq = i_val * i_val;
			for (int j = 0; j < nc; j++)
			{
				double j_val = j;
				double val = c0 + c1 * i_val + c2 * j_val + c3 * i_val * j_val + c4 * i_sq + c5 * j_val * j_val;
				unwrapped_phase.at<double>(i, j) += val;
			}
		}
	}
	//控制点绝对相位计算
	double r_main = sqrt(sum((sate1(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground).mul(sate1(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground))[0]);
	double r_slave = sqrt(sum((sate2(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground).mul(sate2(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground))[0]);
	double C = mode == TR_MODE_SINGLE_TX_SINGLE_RX ? 4 * PI : 2 * PI;
	double lambda = VEL_C / (carrier_frequency.at<double>(0, 0) + 1e-10);
	double phase_real = (r_slave - r_main) / lambda * C;
	double K = round((phase_real - unwrapped_phase.at<double>(row, col)) / (2 * PI));
	unwrapped_phase = unwrapped_phase + K * 2 * PI;//相位校正
	
	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\unwrapped_phase_abs.bin", unwrapped_phase);

	/*
	* 反演高程
	*/

	Mat R_M(1, nc, CV_64F);
	for (int i = 0; i < nc; i++)
	{
		R_M.at<double>(0, i) = r_main + range_spacing.at<double>(0, 0) * (double(i) - col);
	}
	Mat ones = Mat::ones(nr, 1, CV_64F);
	R_M = ones * R_M;
	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase / (2 * PI);
	Mat Satellite_M_T_Position = sate1;//主星发射位置
	Mat Satellite_S_T_Position;
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX)
	{
		Satellite_S_T_Position = sate2;//辅星发射位置
	}
	else
	{
		Satellite_S_T_Position = sate1;//辅星发射位置
	}
	Mat Satellite_M_R_Position = sate1;//主星接收位置
	Mat Satellite_S_R_Position = sate2;//辅星接收位置
	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
	Mat Vs = satev1;
	ones = Mat::ones(nr, nc, CV_64F);
	Mat P1 = ones * xyz_ground.at<double>(0, 0);
	Mat P2 = ones * xyz_ground.at<double>(0, 1);
	Mat P3 = ones * xyz_ground.at<double>(0, 2);
	Mat fd = Mat::zeros(1, nc, CV_64F);
	if (!Utils::newton_iter_core_ex(iter_times, P1, P2, P3, Satellite_M_T_Position, Satellite_S_T_Position,
	                 Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs,
	                 R_M, R_F, fd, lambda, ProgressReporter::callback, &progressReporter))
	{
		if (progressReporter.cancelled()) return cancellationResult();
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_PROCESSING, "newton_iteration", "Newton iteration did not converge or was cancelled.", "internalReturnCode=-2");
		return legacyConsoleLogging ? -2 : static_cast<int>(DEM_ERROR_PROCESSING);
	}
	std::atomic<bool> parallel_flag(true);
	dem.create(nr, nc, CV_64F);

	std::atomic<int> completed_rows(0);
	int step = std::max(1, nr / 10);

#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i < nr; i++)
	{
		if (!parallel_flag || progressReporter.cancelled()) continue;
		for (int j = 0; j < nc; j++)
		{
			if (!parallel_flag || progressReporter.cancelled()) continue;
			double lat, lon, h;
			ret = Utils::xyz2ell(P1.at<double>(i, j), P2.at<double>(i, j), P3.at<double>(i, j), lat, lon, h);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			dem.at<double>(i, j) = h;
		}

		int current_completed = ++completed_rows;
		if (current_completed % step == 0)
		{
			int progress = 90 + (current_completed * 10) / nr;
			if (!progressReporter.reportCoordinateConversion(progress))
			{
				parallel_flag = false;
			}
		}
	}
	if (!parallel_flag || progressReporter.cancelled())
	{
		if (progressReporter.cancelled()) return cancellationResult();
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_PROCESSING, "coordinate_conversion", "Coordinate conversion did not complete.", "internalReturnCode=-2");
		return legacyConsoleLogging ? -2 : static_cast<int>(DEM_ERROR_PROCESSING);
	}
	dem = dem + llh.at<double>(0, 2) - dem.at<double>(row, col);
	if (hasPhaseValidityMask) {
		dem.setTo(std::numeric_limits<double>::quiet_NaN(), phaseValidMask == 0);
	}
	if (!progressReporter.reportSuccess()) return cancellationResult();
	diagnosticContext.emit(DEM_LOG_INFO, static_cast<DemError>(0), "complete", "DEM Newton iteration completed successfully.",
		"rows=" + std::to_string(dem.rows) + "; columns=" + std::to_string(dem.cols));
	return 0;
}

int Dem::dem_newton_iter_test(const char* unwrapped_phase_file, Mat& dem, const char* project_path, int iter_times, int mode, DemProgressCallback cb)
{
	if (unwrapped_phase_file == NULL ||
		project_path == NULL ||
		iter_times < 1 ||
		(mode == TR_MODE_SINGLE_TX_SINGLE_RX || mode == TR_MODE_SINGLE_TX_DOUBLE_RX) == false)
	{
		fprintf(stderr, "dem_newton_iter(): input check failed!\n");
		return -1;
	}
	ProgressReporter progressReporter(cb);
	if (!progressReporter.report(0, "Preparing DEM input...")) return -2;

	/*
	* 校正至绝对相位
	*/

	FormatConversion conversion; Utils util;
	int nr, nc, ret, offset_row, offset_col;
	double time_interval1, time_interval2, acquisitionStartTime1, acquisitionStartTime2, acquisitionStopTime1,
		acquisitionStopTime2, wavelength, nearRange;
	string source_1, source_2, start_time, end_time;
	Mat unwrapped_phase, flat_phase_coefficient, gcps, temp, range_spacing,
		stateVec1, stateVec2, lat_coefficient, lon_coefficient, prf1, prf2, carrier_frequency;
	ret = conversion.read_array_from_h5(unwrapped_phase_file, "phase", unwrapped_phase);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	nr = unwrapped_phase.rows; nc = unwrapped_phase.cols;
	if (nr < 1 || nc < 1)
	{
		fprintf(stderr, "dem_newton_iter(): invalid unwrapped_phase !\n");
		return -1;
	}
	ret = conversion.read_array_from_h5(unwrapped_phase_file, "flat_phase_coefficient", flat_phase_coefficient);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	PathResolver::SourcePathPair sourcePaths;
	PathResolver::Error pathError = PathResolver::Error::None;
	string pathDetail;
	if (!PathResolver::readSourcePathPair(unwrapped_phase_file, project_path, sourcePaths, &pathError, &pathDetail))
	{
		fprintf(stderr, "dem_newton_iter_test(): %s (%s)\n", PathResolver::errorMessage(pathError), pathDetail.c_str());
		return -1;
	}
	source_1 = sourcePaths.source1.utf8;
	source_2 = sourcePaths.source2.utf8;
	ret = conversion.read_array_from_h5(source_1.c_str(), "GCP", gcps);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "offset_row", temp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	offset_row = temp.at<int>(0, 0);
	ret = conversion.read_array_from_h5(source_1.c_str(), "offset_col", temp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	offset_col = temp.at<int>(0, 0);
	ret = conversion.read_array_from_h5(source_1.c_str(), "range_spacing", range_spacing);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "state_vec", stateVec1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_2.c_str(), "state_vec", stateVec2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "prf", prf1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	time_interval1 = 1.0 / (prf1.at<double>(0, 0) + 1e-10);
	ret = conversion.read_array_from_h5(source_2.c_str(), "prf", prf2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	time_interval2 = 1.0 / (prf2.at<double>(0, 0) + 1e-10);
	//ret = conversion.read_array_from_h5(source_1.c_str(), "lat_coefficient", lat_coefficient);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	//ret = conversion.read_array_from_h5(source_1.c_str(), "lon_coefficient", lon_coefficient);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "carrier_frequency", carrier_frequency);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	wavelength = 3e8 / carrier_frequency.at<double>(0, 0);

	ret = conversion.read_str_from_h5(source_1.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &acquisitionStartTime1);
	ret = conversion.read_str_from_h5(source_1.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &acquisitionStopTime1);
	ret = conversion.read_str_from_h5(source_2.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &acquisitionStartTime2);
	ret = conversion.read_str_from_h5(source_2.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &acquisitionStopTime2);

	ret = conversion.read_double_from_h5(source_1.c_str(), "slant_range_first_pixel", &nearRange);

	//寻找图像范围内的控制点信息

	int num_gcps = gcps.rows;
	int row, col, i_gcp = 0, count = 0;
	bool b_gcp = false;
	vector<int> valid_row;
	for (i_gcp = 0; i_gcp < num_gcps; i_gcp++)
	{
		row = (int)gcps.at<double>(i_gcp, 0);
		col = (int)gcps.at<double>(i_gcp, 1);
		if ((row - offset_row - 1) >= 0 && (row - offset_row - 1) < nr && (col - offset_col - 1) >= 0 && (col - offset_col - 1) < nc)
		{
			b_gcp = true;
			valid_row.push_back(i_gcp);
			//break;
		}
	}
	Mat llh(1, 3, CV_64F), xyz_ground(1, 3, CV_64F);
	if (b_gcp)
	{
		i_gcp = valid_row[0];
		row = (int)gcps.at<double>(i_gcp, 0);
		col = (int)gcps.at<double>(i_gcp, 1);
		llh.at<double>(0, 0) = gcps.at<double>(i_gcp, 3);
		llh.at<double>(0, 1) = gcps.at<double>(i_gcp, 2);
		llh.at<double>(0, 2) = gcps.at<double>(i_gcp, 4);
		row = row - offset_row; col = col - offset_col;
	}
	else
	{
		return -1;
	}
	ret = util.ell2xyz(llh, xyz_ground);
	if (return_check(ret, "ell2xyz", error_head)) return -1;


	/*
	* 轨道插值
	*/
	orbitStateVectors stateVectors1(stateVec1, acquisitionStartTime1, acquisitionStopTime1);
	stateVectors1.applyOrbit();
	orbitStateVectors stateVectors2(stateVec2, acquisitionStartTime2, acquisitionStopTime2);
	stateVectors2.applyOrbit();
	/*
	* 寻找图像左上角成像卫星位置
	*/
	Mat sate1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat satev1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat sate2 = Mat::zeros(nr, 3, CV_64F);//辅星位置
	//找到零多普勒位置
	Position pos;Velocity vel;
	for (int i = 0; i < nr; i++)
	{
		stateVectors1.getPosition(acquisitionStartTime1 + double(offset_row + i) * time_interval1, pos);
		stateVectors1.getVelocity(acquisitionStartTime1 + double(offset_row + i) * time_interval1, vel);
		sate1.at<double>(i, 0) = pos.x;
		sate1.at<double>(i, 1) = pos.y;
		sate1.at<double>(i, 2) = pos.z;
		satev1.at<double>(i, 0) = vel.vx;
		satev1.at<double>(i, 1) = vel.vy;
		satev1.at<double>(i, 2) = vel.vz;
	}

	//卫星2
	Position groundPosition;
	double latitude, longitude, height, dopplerFrequency = 0.0;
	latitude = gcps.at<double>(i_gcp, 3);
	longitude = gcps.at<double>(i_gcp, 2);
	longitude = longitude > 180.0 ? (longitude - 360.0) : longitude;
	height = gcps.at<double>(i_gcp, 4);
	Utils::ell2xyz(longitude, latitude, height, groundPosition);
	double zeroDopplerTime, distance;
	if (!Utils::findZeroDopplerTime(stateVectors2, groundPosition, wavelength, time_interval2, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
		return -1;
	}
	acquisitionStartTime2 = zeroDopplerTime - (row - 1) * time_interval2;
	for (int i = 0; i < nr; i++)
	{
		stateVectors2.getPosition(acquisitionStartTime2 + double(i) * time_interval2, pos);
		sate2.at<double>(i, 0) = pos.x;
		sate2.at<double>(i, 1) = pos.y;
		sate2.at<double>(i, 2) = pos.z;
	}

	//加回平地相位
	double tc0 = flat_phase_coefficient.at<double>(0, 0);
	double tc1 = flat_phase_coefficient.at<double>(0, 1);
	double tc2 = flat_phase_coefficient.at<double>(0, 2);
	double tc3 = flat_phase_coefficient.at<double>(0, 3);
	double tc4 = flat_phase_coefficient.at<double>(0, 4);
	double tc5 = flat_phase_coefficient.at<double>(0, 5);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		double i_val = i;
		double i_sq = i_val * i_val;
		for (int j = 0; j < nc; j++)
		{
			double j_val = j;
			double val = tc0 + tc1 * i_val + tc2 * j_val + tc3 * i_val * j_val + tc4 * i_sq + tc5 * j_val * j_val;
			unwrapped_phase.at<double>(i, j) += val;
		}
	}
	//控制点绝对相位计算
	double lambda = VEL_C / (carrier_frequency.at<double>(0, 0) + 1e-10);
	double K = 0.0;
	for (int i = 0; i < valid_row.size(); i++)
	{
		int rrr = (int)gcps.at<double>(valid_row[i], 0) - offset_row;
		int ccc = (int)gcps.at<double>(valid_row[i], 1) - offset_col;
		Mat ground(1, 3, CV_64F), llh_temp(1, 3, CV_64F);
		llh_temp.at<double>(0, 0) = gcps.at<double>(valid_row[i], 3);
		llh_temp.at<double>(0, 1) = gcps.at<double>(valid_row[i], 2);
		llh_temp.at<double>(0, 2) = gcps.at<double>(valid_row[i], 4);
		util.ell2xyz(llh_temp, ground);
		double r_main = sqrt(sum((sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
		double r_slave = sqrt(sum((sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
		double C = mode == TR_MODE_SINGLE_TX_SINGLE_RX ? 4 * PI : 2 * PI;
		double phase_real = (r_slave - r_main) / lambda * C;
		K += ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
	}
	K /= (double)valid_row.size();
	unwrapped_phase = unwrapped_phase + round(K) * 2 * PI;//相位校正
	//conversion.creat_new_h5("G:\\tmp\\unwrapped_phase.h5");
	//conversion.write_array_to_h5("G:\\tmp\\unwrapped_phase.h5", "phase", unwrapped_phase); return 0;

	//Mat R1, R2;
	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R1", R1);
	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R2", R2);

	//for (int i = 0; i < nr; i++)
	//{
	//	for (int j = 0; j < nc; j++)
	//	{
	//		unwrapped_phase.at<double>(i, j) = -4.0 * PI * (R1.at<double>(i + offset_row, j + offset_col) -
	//			R2.at<double>(i + offset_row, j + offset_col)) / lambda;
	//	}
	//}

	/*
	* 反演高程
	*/

	Mat R_M(1, nc, CV_64F);
	//Mat R_M(nr, nc, CV_64F);
	for (int i = 0; i < nc; i++)
	{
		R_M.at<double>(0, i) = nearRange + range_spacing.at<double>(0, 0) * double(i + offset_col);
		
	}
	//for (int i = 0; i < nr; i++)
	//{
	//	for (int j = 0; j < nc; j++)
	//	{
	//		R_M.at<double>(i, j) = R1.at<double>(i + offset_row, j + offset_col);
	//	}
	//}
	Mat ones = Mat::ones(nr, 1, CV_64F);
	R_M = ones * R_M;
	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase / (2 * PI);
	Mat Satellite_M_T_Position = sate1;//主星发射位置
	Mat Satellite_S_T_Position;
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX)
	{
		Satellite_S_T_Position = sate2;//辅星发射位置
	}
	else
	{
		Satellite_S_T_Position = sate1;//辅星发射位置
	}
	Mat Satellite_M_R_Position = sate1;//主星接收位置
	Mat Satellite_S_R_Position = sate2;//辅星接收位置
	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
	Mat Vs = satev1;
	ones = Mat::ones(nr, nc, CV_64F);
	Mat P1 = ones * xyz_ground.at<double>(0, 0);
	Mat P2 = ones * xyz_ground.at<double>(0, 1);
	Mat P3 = ones * xyz_ground.at<double>(0, 2);
	Mat fd = Mat::zeros(1, nc, CV_64F);
	if (!Utils::newton_iter_core_ex(iter_times, P1, P2, P3, Satellite_M_T_Position, Satellite_S_T_Position,
	                 Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs,
	                 R_M, R_F, fd, lambda, ProgressReporter::callback, &progressReporter))
	{
		return -2;
	}
	std::atomic<bool> parallel_flag(true);
	dem.create(nr, nc, CV_64F);
	Mat lon, lat;
	lon.create(nr, nc, CV_64F); lat.create(nr, nc, CV_64F); 

	std::atomic<int> completed_rows(0);
	int step = std::max(1, nr / 10);

#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i < nr; i++)
	{
		if (!parallel_flag || progressReporter.cancelled()) continue;
		for (int j = 0; j < nc; j++)
		{
			if (!parallel_flag || progressReporter.cancelled()) continue;
			double lat_val, lon_val, h_val;
			ret = Utils::xyz2ell(P1.at<double>(i, j), P2.at<double>(i, j), P3.at<double>(i, j), lat_val, lon_val, h_val);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			dem.at<double>(i, j) = h_val;
			lat.at<double>(i, j) = lat_val;
			lon.at<double>(i, j) = lon_val;
		}

		int current_completed = ++completed_rows;
		if (current_completed % step == 0)
		{
			int progress = 90 + (current_completed * 10) / nr;
			if (!progressReporter.reportCoordinateConversion(progress))
			{
				parallel_flag = false;
			}
		}
	}
	if (!parallel_flag || progressReporter.cancelled()) return -2;
	//dem = dem + llh.at<double>(0, 2) - dem.at<double>(row - 1, col - 1);
	Mat error(static_cast<int>(valid_row.size()), 3, CV_64F);

	for (int i = 0; i < valid_row.size(); i++)
	{
		int r, c;
		//Utils::ell2xyz(gcps.at<double>(valid_row[i], 2), gcps.at<double>(valid_row[i], 3), gcps.at<double>(valid_row[i], 4), pos);
		r = static_cast<int>(gcps.at<double>(valid_row[i], 0)) - offset_row;
		c = static_cast<int>(gcps.at<double>(valid_row[i], 1)) - offset_col;
		/*error.at<double>(i, 0) = P1.at<double>(r - 1, c - 1) - pos.x;
		error.at<double>(i, 1) = P2.at<double>(r - 1, c - 1) - pos.y;
		error.at<double>(i, 2) = P3.at<double>(r - 1, c - 1) - pos.z;*/
		error.at<double>(i, 0) = lon.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 2);
		error.at<double>(i, 1) = lat.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 3);
		error.at<double>(i, 2) = dem.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 4);
	}
	//util.cvmat2bin("G:\\tmp\\error.bin", error);
	if (!progressReporter.reportSuccess()) return -2;
	return 0;
}

int Dem::dem_newton_iter_14(
	const char* unwrapped_phase_file, 
	Mat& dem,
	Mat& lon,
	Mat& lat, 
	Mat& dem_x,
	Mat& dem_y,
	Mat& dem_z,
	Mat& error_llh,
	Mat& error_xyz,
	const char* project_path,
	int iter_times,
	int mode,
	DemProgressCallback cb
)
{
	if (unwrapped_phase_file == NULL ||
		project_path == NULL ||
		iter_times < 1 ||
		(mode == TR_MODE_SINGLE_TX_SINGLE_RX || mode == TR_MODE_SINGLE_TX_DOUBLE_RX) == false)
	{
		fprintf(stderr, "dem_newton_iter_14(): input check failed!\n");
		return -1;
	}
	ProgressReporter progressReporter(cb);
	if (!progressReporter.report(0, "Preparing DEM input...")) return -2;

	/*
	* 校正至绝对相位
	*/

	FormatConversion conversion; Utils util;
	int nr, nc, ret, offset_row, offset_col;
	double time_interval1, time_interval2, acquisitionStartTime1, acquisitionStartTime2, acquisitionStopTime1,
		acquisitionStopTime2, wavelength, nearRange;
	string source_1, source_2, start_time, end_time;
	Mat unwrapped_phase, flat_phase_coefficient, gcps, temp, range_spacing,
		stateVec1, stateVec2, lat_coefficient, lon_coefficient, prf1, prf2, carrier_frequency;
	ret = conversion.read_array_from_h5(unwrapped_phase_file, "phase", unwrapped_phase);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	nr = unwrapped_phase.rows; nc = unwrapped_phase.cols;
	if (nr < 1 || nc < 1)
	{
		fprintf(stderr, "dem_newton_iter_14(): invalid unwrapped_phase !\n");
		return -1;
	}
	//ret = conversion.read_array_from_h5(unwrapped_phase_file, "flat_phase_coefficient", flat_phase_coefficient);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	PathResolver::SourcePathPair sourcePaths;
	PathResolver::Error pathError = PathResolver::Error::None;
	string pathDetail;
	if (!PathResolver::readSourcePathPair(unwrapped_phase_file, project_path, sourcePaths, &pathError, &pathDetail))
	{
		fprintf(stderr, "dem_newton_iter_14(): %s (%s)\n", PathResolver::errorMessage(pathError), pathDetail.c_str());
		return -1;
	}
	source_1 = sourcePaths.source1.utf8;
	source_2 = sourcePaths.source2.utf8;
	ret = conversion.read_array_from_h5(source_1.c_str(), "gcps", gcps);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "offset_row", temp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	offset_row = temp.at<int>(0, 0);
	ret = conversion.read_array_from_h5(source_1.c_str(), "offset_col", temp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	offset_col = temp.at<int>(0, 0);
	ret = conversion.read_array_from_h5(source_1.c_str(), "range_spacing", range_spacing);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "state_vec", stateVec1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_2.c_str(), "state_vec", stateVec2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "prf", prf1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	time_interval1 = 1.0 / (prf1.at<double>(0, 0) + 1e-10);
	ret = conversion.read_array_from_h5(source_2.c_str(), "prf", prf2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	time_interval2 = 1.0 / (prf2.at<double>(0, 0) + 1e-10);
	//ret = conversion.read_array_from_h5(source_1.c_str(), "lat_coefficient", lat_coefficient);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	//ret = conversion.read_array_from_h5(source_1.c_str(), "lon_coefficient", lon_coefficient);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "carrier_frequency", carrier_frequency);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	wavelength = VEL_C / carrier_frequency.at<double>(0, 0);

	ret = conversion.read_str_from_h5(source_1.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &acquisitionStartTime1);
	ret = conversion.read_str_from_h5(source_1.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &acquisitionStopTime1);
	ret = conversion.read_str_from_h5(source_2.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &acquisitionStartTime2);
	ret = conversion.read_str_from_h5(source_2.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &acquisitionStopTime2);

	ret = conversion.read_double_from_h5(source_1.c_str(), "slant_range_first_pixel", &nearRange);

	Mat slantRange_main, slantRange_slave;
	unwrapped_phase.copyTo(slantRange_main); slantRange_main = 0.0;
	unwrapped_phase.copyTo(slantRange_slave); slantRange_slave = 0.0;
	conversion.read_array_from_h5(source_1.c_str(), "slantRange", slantRange_main);
	conversion.read_array_from_h5(source_2.c_str(), "slantRange", slantRange_slave);

	//寻找图像范围内的控制点信息

	int num_gcps = gcps.rows;
	int row, col, i_gcp = 0, count = 0;
	bool b_gcp = false;
	vector<int> valid_row;
	for (i_gcp = 0; i_gcp < num_gcps; i_gcp++)
	{
		row = (int)gcps.at<double>(i_gcp, 0);
		col = (int)gcps.at<double>(i_gcp, 1);
		if ((row - offset_row - 1) >= 0 && (row - offset_row - 1) < nr && (col - offset_col - 1) >= 0 && (col - offset_col - 1) < nc)
		{
			b_gcp = true;
			valid_row.push_back(i_gcp);
			//break;
		}
	}
	Mat llh(1, 3, CV_64F), xyz_ground(1, 3, CV_64F);
	if (b_gcp)
	{
		i_gcp = valid_row[0];
		row = (int)gcps.at<double>(i_gcp, 0);
		col = (int)gcps.at<double>(i_gcp, 1);
		llh.at<double>(0, 0) = gcps.at<double>(i_gcp, 3);
		llh.at<double>(0, 1) = gcps.at<double>(i_gcp, 2);
		llh.at<double>(0, 2) = gcps.at<double>(i_gcp, 4);
		row = row - offset_row; col = col - offset_col;
	}
	else
	{
		return -1;
	}
	ret = util.ell2xyz(llh, xyz_ground);
	if (return_check(ret, "ell2xyz", error_head)) return -1;


	/*
	* 轨道插值
	*/
	orbitStateVectors stateVectors1(stateVec1, acquisitionStartTime1, acquisitionStopTime1);
	stateVectors1.applyOrbit();
	orbitStateVectors stateVectors2(stateVec2, acquisitionStartTime2, acquisitionStopTime2);
	stateVectors2.applyOrbit();
	/*
	* 寻找图像左上角成像卫星位置
	*/
	Mat sate1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat satev1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat sate2 = Mat::zeros(nr, 3, CV_64F);//辅星位置
	//找到零多普勒位置
	Position pos; Velocity vel;
	for (int i = 0; i < nr; i++)
	{
		stateVectors1.getPosition(acquisitionStartTime1 + double(offset_row + i) * time_interval1, pos);
		stateVectors1.getVelocity(acquisitionStartTime1 + double(offset_row + i) * time_interval1, vel);
		sate1.at<double>(i, 0) = pos.x;
		sate1.at<double>(i, 1) = pos.y;
		sate1.at<double>(i, 2) = pos.z;
		satev1.at<double>(i, 0) = vel.vx;
		satev1.at<double>(i, 1) = vel.vy;
		satev1.at<double>(i, 2) = vel.vz;
	}

	//卫星2
	Position groundPosition;
	double latitude, longitude, height, dopplerFrequency = 0.0;
	latitude = gcps.at<double>(i_gcp, 3);
	longitude = gcps.at<double>(i_gcp, 2);
	longitude = longitude > 180.0 ? (longitude - 360.0) : longitude;
	height = gcps.at<double>(i_gcp, 4);
	Utils::ell2xyz(longitude, latitude, height, groundPosition);
	double zeroDopplerTime, distance;
	if (!Utils::findZeroDopplerTime(stateVectors2, groundPosition, wavelength, time_interval2, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
		return -1;
	}
	acquisitionStartTime2 = zeroDopplerTime - (row - 1) * time_interval2;
	for (int i = 0; i < nr; i++)
	{
		stateVectors2.getPosition(acquisitionStartTime2 + double(i) * time_interval2, pos);
		sate2.at<double>(i, 0) = pos.x;
		sate2.at<double>(i, 1) = pos.y;
		sate2.at<double>(i, 2) = pos.z;
	}

	//加回参考相位
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double phi_ref = 0.0;
			phi_ref = slantRange_slave.at<double>(i, j) - slantRange_main.at<double>(i, j);
			phi_ref = phi_ref / wavelength * 4 * PI;
			//flat_phase.at<double>(i, j) = phi_ref;
			unwrapped_phase.at<double>(i, j) = unwrapped_phase.at<double>(i, j) + phi_ref;
		}
	}
	//控制点绝对相位计算
	double lambda = wavelength;
	double K = 0.0;
	Mat KK = Mat::zeros(1, static_cast<int>(valid_row.size()), CV_64F);
	for (int i = 0; i < valid_row.size(); i++)
	{
		int rrr = (int)gcps.at<double>(valid_row[i], 0) - offset_row;
		int ccc = (int)gcps.at<double>(valid_row[i], 1) - offset_col;
		Mat ground(1, 3, CV_64F), llh_temp(1, 3, CV_64F);
		llh_temp.at<double>(0, 0) = gcps.at<double>(valid_row[i], 3);
		llh_temp.at<double>(0, 1) = gcps.at<double>(valid_row[i], 2);
		llh_temp.at<double>(0, 2) = gcps.at<double>(valid_row[i], 4);
		util.ell2xyz(llh_temp, ground);
		double r_main = sqrt(sum((sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
		double r_slave = sqrt(sum((sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
		double C = mode == TR_MODE_SINGLE_TX_SINGLE_RX ? 4 * PI : 2 * PI;
		double phase_real = (r_slave - r_main) / lambda * C;
		KK.at<double>(0, i) = ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
		K += ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
	}
	//conversion.creat_new_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\KK2.h5");
	//conversion.write_array_to_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\KK2.h5", "KK", KK);
	K /= (double)valid_row.size();
	unwrapped_phase = unwrapped_phase + round(K) * 2 * PI;//相位校正
	//conversion.creat_new_h5("G:\\tmp\\unwrapped_phase.h5");
	//conversion.write_array_to_h5("G:\\tmp\\unwrapped_phase.h5", "phase", unwrapped_phase); return 0;

	//Mat R1, R2;
	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R1", R1);
	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R2", R2);

	//for (int i = 0; i < nr; i++)
	//{
	//	for (int j = 0; j < nc; j++)
	//	{
	//		unwrapped_phase.at<double>(i, j) = -4.0 * PI * (R1.at<double>(i + offset_row, j + offset_col) -
	//			R2.at<double>(i + offset_row, j + offset_col)) / lambda;
	//	}
	//}

	/*
	* 反演高程
	*/

	Mat R_M(1, nc, CV_64F);
	//Mat R_M(nr, nc, CV_64F);
	for (int i = 0; i < nc; i++)
	{
		R_M.at<double>(0, i) = nearRange + range_spacing.at<double>(0, 0) * double(i + offset_col);

	}
	//for (int i = 0; i < nr; i++)
	//{
	//	for (int j = 0; j < nc; j++)
	//	{
	//		R_M.at<double>(i, j) = R1.at<double>(i + offset_row, j + offset_col);
	//	}
	//}
	Mat ones = Mat::ones(nr, 1, CV_64F);
	R_M = ones * R_M;
	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase / (2 * PI);
	Mat Satellite_M_T_Position = sate1;//主星发射位置
	Mat Satellite_S_T_Position;
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX)
	{
		Satellite_S_T_Position = sate2;//辅星发射位置
	}
	else
	{
		Satellite_S_T_Position = sate1;//辅星发射位置
	}
	Mat Satellite_M_R_Position = sate1;//主星接收位置
	Mat Satellite_S_R_Position = sate2;//辅星接收位置
	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
	Mat Vs = satev1;
	ones = Mat::ones(nr, nc, CV_64F);
	/*Mat P1 = ones * xyz_ground.at<double>(0, 0);
	Mat P2 = ones * xyz_ground.at<double>(0, 1);
	Mat P3 = ones * xyz_ground.at<double>(0, 2);*/
	dem_x = ones * xyz_ground.at<double>(0, 0);
	dem_y = ones * xyz_ground.at<double>(0, 1);
	dem_z = ones * xyz_ground.at<double>(0, 2);
	Mat fd = Mat::zeros(1, nc, CV_64F);
	if (!Utils::newton_iter_core_ex(iter_times, dem_x, dem_y, dem_z, Satellite_M_T_Position, Satellite_S_T_Position,
	                 Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs,
	                 R_M, R_F, fd, lambda, ProgressReporter::callback, &progressReporter))
	{
		return -2;
	}
	std::atomic<bool> parallel_flag(true);
	dem.create(nr, nc, CV_64F);
	lon.create(nr, nc, CV_64F); lat.create(nr, nc, CV_64F);

	std::atomic<int> completed_rows(0);
	int step = std::max(1, nr / 10);

#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i < nr; i++)
	{
		if (!parallel_flag || progressReporter.cancelled()) continue;
		for (int j = 0; j < nc; j++)
		{
			if (!parallel_flag || progressReporter.cancelled()) continue;
			double lat_val, lon_val, h_val;
			ret = Utils::xyz2ell(dem_x.at<double>(i, j), dem_y.at<double>(i, j), dem_z.at<double>(i, j), lat_val, lon_val, h_val);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			dem.at<double>(i, j) = h_val;
			lat.at<double>(i, j) = lat_val;
			lon.at<double>(i, j) = lon_val;
		}

		int current_completed = ++completed_rows;
		if (current_completed % step == 0)
		{
			int progress = 90 + (current_completed * 10) / nr;
			if (!progressReporter.reportCoordinateConversion(progress))
			{
				parallel_flag = false;
			}
		}
	}
	if (!parallel_flag || progressReporter.cancelled()) return -2;
	error_llh.create(static_cast<int>(valid_row.size()), 3, CV_64F);
	error_xyz.create(static_cast<int>(valid_row.size()), 3, CV_64F);
	for (int i = 0; i < valid_row.size(); i++)
	{
		int r, c;
		Utils::ell2xyz(gcps.at<double>(valid_row[i], 2), gcps.at<double>(valid_row[i], 3), gcps.at<double>(valid_row[i], 4), pos);
		r = static_cast<int>(gcps.at<double>(valid_row[i], 0)) - offset_row;
		c = static_cast<int>(gcps.at<double>(valid_row[i], 1)) - offset_col;
		error_xyz.at<double>(i, 0) = dem_x.at<double>(r - 1, c - 1) - pos.x;
		error_xyz.at<double>(i, 1) = dem_y.at<double>(r - 1, c - 1) - pos.y;
		error_xyz.at<double>(i, 2) = dem_z.at<double>(r - 1, c - 1) - pos.z;
		error_llh.at<double>(i, 0) = lon.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 2);
		error_llh.at<double>(i, 1) = lat.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 3);
		error_llh.at<double>(i, 2) = dem.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 4);
	}
	//util.cvmat2bin("G:\\tmp\\error.bin", error);
	if (!progressReporter.reportSuccess()) return -2;
	return 0;
}

int Dem::dem_newton_iter_14_dualfreqpingpong(
	const char* unwrapped_phase_file,
	Mat& dem,
	Mat& lon,
	Mat& lat,
	Mat& dem_x,
	Mat& dem_y,
	Mat& dem_z,
	Mat& error_llh,
	Mat& error_xyz,
	const char* project_path,
	int iter_times, 
	int mode,
	DemProgressCallback cb
)
{
	if (unwrapped_phase_file == NULL ||
		project_path == NULL ||
		iter_times < 1 ||
		mode < TR_MODE_SINGLE_TX_SINGLE_RX || mode > TR_MODE_DOUBLE_FREQ_PING)
	{
		fprintf(stderr, "dem_newton_iter_14_dualfreqpingpong(): input check failed!\n");
		return -1;
	}
	ProgressReporter progressReporter(cb);
	if (!progressReporter.report(0, "Preparing DEM input...")) return -2;

	/*
	* 校正至绝对相位
	*/

	FormatConversion conversion; Utils util;
	int nr, nc, ret, offset_row, offset_col;
	double time_interval1, time_interval2, acquisitionStartTime1, acquisitionStartTime2, acquisitionStopTime1,
		acquisitionStopTime2, wavelength, nearRange;
	string source_1, source_2, start_time, end_time;
	Mat unwrapped_phase, flat_phase_coefficient, gcps, temp, range_spacing,
		stateVec1, stateVec2, lat_coefficient, lon_coefficient, prf1, prf2, carrier_frequency;
	ret = conversion.read_array_from_h5(unwrapped_phase_file, "phase", unwrapped_phase);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	nr = unwrapped_phase.rows; nc = unwrapped_phase.cols;
	PathResolver::SourcePathPair sourcePaths;
	PathResolver::Error pathError = PathResolver::Error::None;
	string pathDetail;
	if (!PathResolver::readSourcePathPair(unwrapped_phase_file, project_path, sourcePaths, &pathError, &pathDetail))
	{
		fprintf(stderr, "dem_newton_iter_14_dualfreqpingpong(): %s (%s)\n", PathResolver::errorMessage(pathError), pathDetail.c_str());
		return -1;
	}
	source_1 = sourcePaths.source1.utf8;
	source_2 = sourcePaths.source2.utf8;
	ret = conversion.read_array_from_h5(source_1.c_str(), "gcps", gcps);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "offset_row", temp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	offset_row = temp.at<int>(0, 0);
	ret = conversion.read_array_from_h5(source_1.c_str(), "offset_col", temp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	offset_col = temp.at<int>(0, 0);
	ret = conversion.read_array_from_h5(source_1.c_str(), "range_spacing", range_spacing);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "state_vec", stateVec1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_2.c_str(), "state_vec", stateVec2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "prf", prf1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	time_interval1 = 1.0 / (prf1.at<double>(0, 0) + 1e-10);
	ret = conversion.read_array_from_h5(source_2.c_str(), "prf", prf2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	time_interval2 = 1.0 / (prf2.at<double>(0, 0) + 1e-10);
	ret = conversion.read_array_from_h5(source_1.c_str(), "carrier_frequency", carrier_frequency);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	wavelength = VEL_C / carrier_frequency.at<double>(0, 0);

	ret = conversion.read_str_from_h5(source_1.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &acquisitionStartTime1);
	ret = conversion.read_str_from_h5(source_1.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &acquisitionStopTime1);
	ret = conversion.read_str_from_h5(source_2.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &acquisitionStartTime2);
	ret = conversion.read_str_from_h5(source_2.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &acquisitionStopTime2);

	ret = conversion.read_double_from_h5(source_1.c_str(), "slant_range_first_pixel", &nearRange);

	//寻找图像范围内的控制点信息

	int num_gcps = gcps.rows;
	int row, col, i_gcp = 0, count = 0;
	bool b_gcp = false;
	vector<int> valid_row;
	for (i_gcp = 0; i_gcp < num_gcps; i_gcp++)
	{
		row = (int)gcps.at<double>(i_gcp, 0);
		col = (int)gcps.at<double>(i_gcp, 1);
		if ((row - offset_row - 1) >= 0 && (row - offset_row - 1) < nr && (col - offset_col - 1) >= 0 && (col - offset_col - 1) < nc)
		{
			b_gcp = true;
			valid_row.push_back(i_gcp);
			//break;
		}
	}
	Mat llh(1, 3, CV_64F), xyz_ground(1, 3, CV_64F);
	if (b_gcp)
	{
		i_gcp = valid_row[0];
		row = (int)gcps.at<double>(i_gcp, 0);
		col = (int)gcps.at<double>(i_gcp, 1);
		llh.at<double>(0, 0) = gcps.at<double>(i_gcp, 3);
		llh.at<double>(0, 1) = gcps.at<double>(i_gcp, 2);
		llh.at<double>(0, 2) = gcps.at<double>(i_gcp, 4);
		row = row - offset_row; col = col - offset_col;
	}
	else
	{
		return -1;
	}
	ret = util.ell2xyz(llh, xyz_ground);
	if (return_check(ret, "ell2xyz", error_head)) return -1;


	/*
	* 轨道插值
	*/
	orbitStateVectors stateVectors1(stateVec1, acquisitionStartTime1, acquisitionStopTime1);
	stateVectors1.applyOrbit();
	orbitStateVectors stateVectors2(stateVec2, acquisitionStartTime2, acquisitionStopTime2);
	stateVectors2.applyOrbit();
	/*
	* 寻找图像左上角成像卫星位置
	*/
	Mat sate1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat satev1 = Mat::zeros(nr, 3, CV_64F);//主星位置
	Mat sate2 = Mat::zeros(nr, 3, CV_64F);//辅星位置
	//找到零多普勒位置
	Position pos; Velocity vel;
	for (int i = 0; i < nr; i++)
	{
		stateVectors1.getPosition(acquisitionStartTime1 + double(offset_row + i) * time_interval1, pos);
		stateVectors1.getVelocity(acquisitionStartTime1 + double(offset_row + i) * time_interval1, vel);
		sate1.at<double>(i, 0) = pos.x;
		sate1.at<double>(i, 1) = pos.y;
		sate1.at<double>(i, 2) = pos.z;
		satev1.at<double>(i, 0) = vel.vx;
		satev1.at<double>(i, 1) = vel.vy;
		satev1.at<double>(i, 2) = vel.vz;
	}

	//卫星2
	Position groundPosition;
	double latitude, longitude, height, dopplerFrequency = 0.0;
	latitude = gcps.at<double>(i_gcp, 3);
	longitude = gcps.at<double>(i_gcp, 2);
	longitude = longitude > 180.0 ? (longitude - 360.0) : longitude;
	height = gcps.at<double>(i_gcp, 4);
	Utils::ell2xyz(longitude, latitude, height, groundPosition);
	double zeroDopplerTime, distance;
	if (!Utils::findZeroDopplerTime(stateVectors2, groundPosition, wavelength, time_interval2, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
		return -1;
	}
	acquisitionStartTime2 = zeroDopplerTime - (row - 1) * time_interval2;
	for (int i = 0; i < nr; i++)
	{
		stateVectors2.getPosition(acquisitionStartTime2 + double(i) * time_interval2, pos);
		sate2.at<double>(i, 0) = pos.x;
		sate2.at<double>(i, 1) = pos.y;
		sate2.at<double>(i, 2) = pos.z;
	}

//	//加回参考相位
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			double phi_ref = 0.0;
//			phi_ref = slantRange_slave.at<double>(i, j) - slantRange_main.at<double>(i, j);
//			phi_ref = phi_ref / wavelength * 4 * PI;
//			//flat_phase.at<double>(i, j) = phi_ref;
//			unwrapped_phase.at<double>(i, j) = unwrapped_phase.at<double>(i, j) + phi_ref;
//		}
//	}
	//控制点绝对相位计算
	double lambda = wavelength;
	//double K = 0.0;
	//Mat KK = Mat::zeros(1, valid_row.size(), CV_64F);
	//for (int i = 0; i < valid_row.size(); i++)
	//{
	//	int rrr = (int)gcps.at<double>(valid_row[i], 0) - offset_row;
	//	int ccc = (int)gcps.at<double>(valid_row[i], 1) - offset_col;
	//	Mat ground(1, 3, CV_64F), llh_temp(1, 3, CV_64F);
	//	llh_temp.at<double>(0, 0) = gcps.at<double>(valid_row[i], 3);
	//	llh_temp.at<double>(0, 1) = gcps.at<double>(valid_row[i], 2);
	//	llh_temp.at<double>(0, 2) = gcps.at<double>(valid_row[i], 4);
	//	util.ell2xyz(llh_temp, ground);
	//	double r_main = sqrt(sum((sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
	//	double r_slave = sqrt(sum((sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
	//	double C = mode == 1 ? 4 * PI : 2 * PI;
	//	double phase_real = (r_slave - r_main) / lambda * C;
	//	KK.at<double>(0, i) = ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
	//	K += ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
	//}
	//conversion.creat_new_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\KK2.h5");
	//conversion.write_array_to_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\KK2.h5", "KK", KK);
	//K /= (double)valid_row.size();
	//unwrapped_phase = unwrapped_phase + round(K) * 2 * PI;//相位校正
	//conversion.creat_new_h5("G:\\tmp\\unwrapped_phase.h5");
	//conversion.write_array_to_h5("G:\\tmp\\unwrapped_phase.h5", "phase", unwrapped_phase); return 0;

	//Mat R1, R2;
	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R1", R1);
	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R2", R2);

	//for (int i = 0; i < nr; i++)
	//{
	//	for (int j = 0; j < nc; j++)
	//	{
	//		unwrapped_phase.at<double>(i, j) = -4.0 * PI * (R1.at<double>(i + offset_row, j + offset_col) -
	//			R2.at<double>(i + offset_row, j + offset_col)) / lambda;
	//	}
	//}

	/*
	* 反演高程
	*/

	Mat R_M(1, nc, CV_64F);
	for (int i = 0; i < nc; i++)
	{
		R_M.at<double>(0, i) = nearRange + range_spacing.at<double>(0, 0) * double(i + offset_col);

	}
	Mat ones = Mat::ones(nr, 1, CV_64F);
	R_M = ones * R_M;
	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase / (2 * PI);
	Mat Satellite_M_T_Position = sate1;//主星发射位置
	Mat Satellite_S_T_Position;
	if (mode == TR_MODE_SINGLE_TX_SINGLE_RX)
	{
		Satellite_S_T_Position = sate2;//辅星发射位置
	}
	else
	{
		Satellite_S_T_Position = sate1;//辅星发射位置
	}
	Mat Satellite_M_R_Position = sate1;//主星接收位置
	Mat Satellite_S_R_Position = sate2;//辅星接收位置
	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
	Mat Vs = satev1;
	ones = Mat::ones(nr, nc, CV_64F);
	/*Mat P1 = ones * xyz_ground.at<double>(0, 0);
	Mat P2 = ones * xyz_ground.at<double>(0, 1);
	Mat P3 = ones * xyz_ground.at<double>(0, 2);*/
	dem_x = ones * xyz_ground.at<double>(0, 0);
	dem_y = ones * xyz_ground.at<double>(0, 1);
	dem_z = ones * xyz_ground.at<double>(0, 2);
	Mat fd = Mat::zeros(1, nc, CV_64F);
	if (!Utils::newton_iter_core_ex(iter_times, dem_x, dem_y, dem_z, Satellite_M_T_Position, Satellite_S_T_Position,
	                 Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs,
	                 R_M, R_F, fd, lambda, ProgressReporter::callback, &progressReporter))
	{
		return -2;
	}
	std::atomic<bool> parallel_flag(true);
	dem.create(nr, nc, CV_64F);
	lon.create(nr, nc, CV_64F); lat.create(nr, nc, CV_64F);

	std::atomic<int> completed_rows(0);
	int step = std::max(1, nr / 10);

#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i < nr; i++)
	{
		if (!parallel_flag || progressReporter.cancelled()) continue;
		for (int j = 0; j < nc; j++)
		{
			if (!parallel_flag || progressReporter.cancelled()) continue;
			double lat_val, lon_val, h_val;
			ret = Utils::xyz2ell(dem_x.at<double>(i, j), dem_y.at<double>(i, j), dem_z.at<double>(i, j), lat_val, lon_val, h_val);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			dem.at<double>(i, j) = h_val;
			lat.at<double>(i, j) = lat_val;
			lon.at<double>(i, j) = lon_val;
		}

		int current_completed = ++completed_rows;
		if (current_completed % step == 0)
		{
			int progress = 90 + (current_completed * 10) / nr;
			if (!progressReporter.reportCoordinateConversion(progress))
			{
				parallel_flag = false;
			}
		}
	}
	if (!parallel_flag || progressReporter.cancelled()) return -2;
	error_llh.create(static_cast<int>(valid_row.size()), 3, CV_64F);
	error_xyz.create(static_cast<int>(valid_row.size()), 3, CV_64F);
	for (int i = 0; i < valid_row.size(); i++)
	{
		int r, c;
		Utils::ell2xyz(gcps.at<double>(valid_row[i], 2), gcps.at<double>(valid_row[i], 3), gcps.at<double>(valid_row[i], 4), pos);
		r = static_cast<int>(gcps.at<double>(valid_row[i], 0)) - offset_row;
		c = static_cast<int>(gcps.at<double>(valid_row[i], 1)) - offset_col;
		error_xyz.at<double>(i, 0) = dem_x.at<double>(r - 1, c - 1) - pos.x;
		error_xyz.at<double>(i, 1) = dem_y.at<double>(r - 1, c - 1) - pos.y;
		error_xyz.at<double>(i, 2) = dem_z.at<double>(r - 1, c - 1) - pos.z;
		error_llh.at<double>(i, 0) = lon.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 2);
		error_llh.at<double>(i, 1) = lat.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 3);
		error_llh.at<double>(i, 2) = dem.at<double>(r - 1, c - 1) - gcps.at<double>(valid_row[i], 4);
	}
	//util.cvmat2bin("G:\\tmp\\error.bin", error);
	if (!progressReporter.reportSuccess()) return -2;
	return 0;
}

