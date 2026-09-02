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
#include <atomic>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
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

	bool validateVersionedFlatEarthContract(const char* file, const Mat& phase, Mat& reference, std::string& detail)
	{
		FormatConversion conversion;
		int modelVersion = 0, sourceRowCount = 0, validationRequired = 0;
		if (conversion.read_int_from_h5(file, "flat_earth_model_version", &modelVersion) != 0 ||
			(modelVersion != 5 && modelVersion != 6) ||
			conversion.read_int_from_h5(file, "flat_earth_model_source_row_count", &sourceRowCount) != 0 || sourceRowCount < 1 ||
			conversion.read_int_from_h5(file, "flat_earth_reference_validation_required", &validationRequired) != 0 || validationRequired != 1) {
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
		std::string sourceRows, timing, processing, mappingSemantics, rerampSemantics, status, target, semantics, geolocation, timeScale, strategy, masterSource, slaveSource, masterSelectionReason, slaveSelectionReason, masterLookSideSource;
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
			conversion.read_str_from_h5(file, "flat_earth_reference_validation_target", target) != 0 ||
			conversion.read_str_from_h5(file, "flat_earth_reference_phase_semantics", semantics) != 0 ||
			sourceRows != "source_row_map_selects_master_native_burst_line_only_v1" ||
			timing != "strict_gps_h5_time_v2__registration_time_seed_not_geometry_truth_v1" ||
			processing != expectedProcessing ||
			mappingSemantics != "pull_source_row_and_column_offsets_a0_a1_column_a2_master_burst_line_v1" ||
			(!geometryOnly && rerampSemantics != "resampled_slave_deramp_demod_phase_before_conjugated_reramp_v1") ||
			status != expectedStatus ||
			geolocation != "master_native_line_sample_to_h0_rde__slave_zero_doppler_range_v1" ||
			timeScale != "GPS" || strategy != "orbit_state_vectors_apply_orbit_1s_lagrange_v1" ||
			!((masterSource == "fine_state_vec" && masterSelectionReason == "fine_state_vec_valid_preferred_v1") ||
			  (masterSource == "state_vec" && (masterSelectionReason == "fine_state_vec_invalid__state_vec_valid_fallback_v1" || masterSelectionReason == "fine_state_vec_absent__state_vec_valid_fallback_v1"))) ||
			!((slaveSource == "fine_state_vec" && slaveSelectionReason == "fine_state_vec_valid_preferred_v1") ||
			  (slaveSource == "state_vec" && (slaveSelectionReason == "fine_state_vec_invalid__state_vec_valid_fallback_v1" || slaveSelectionReason == "fine_state_vec_absent__state_vec_valid_fallback_v1"))) ||
			(masterLookSideSource != "h5_lookside_v1" && masterLookSideSource != "sentinel1_fixed_right_looking_v1") ||
			target != "external_unfitted_comparison_only_v1" ||
			semantics != expectedReferenceSemantics) {
			detail = "versioned flat-earth string descriptors are missing or unsupported";
			return false;
		}
		int transmitReceiveMode = 0, rdeMaxIterations = 0, zeroDopplerMaxIterations = 0;
		double masterOrbitStart = 0.0, masterOrbitStop = 0.0, masterGeometryStart = 0.0, masterGeometryStop = 0.0,
			slaveOrbitStart = 0.0, slaveOrbitStop = 0.0, slaveGeometryStart = 0.0, slaveGeometryStop = 0.0, interpolationMargin = 0.0,
			epsilonPhase = 0.0, wavelength = 0.0,
			rdeResidual = 0.0, zeroDopplerResidual = 0.0, jacobianCondition = 0.0, initialWindow = 0.0, maximumWindow = 0.0, expansionFactor = 0.0;
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
			!std::isfinite(expansionFactor) || epsilonPhase <= 0.0 || rdeResidual <= 0.0 || zeroDopplerResidual <= 0.0 ||
			wavelength <= 0.0 || (transmitReceiveMode != 1 && transmitReceiveMode != 2) || rdeMaxIterations < 1 || zeroDopplerMaxIterations < 1 || jacobianCondition <= 0.0 || initialWindow <= 0.0 || maximumWindow < initialWindow || expansionFactor <= 1.0) {
			detail = "v5 orbit or RDE numerical contract is invalid";
			return false;
		}
		int maxSlaveSearchExpansions = 0;
		if (conversion.read_int_from_h5(file, "flat_earth_slave_search_max_expansions", &maxSlaveSearchExpansions) != 0 || maxSlaveSearchExpansions < 0) {
			detail = "v5 slave zero-Doppler expansion policy is invalid";
			return false;
		}
		Mat rdeStatistics, validMask, validSampleCount, rerampPhase, mappingCoefficients, mappingBurstIndices;
		if (conversion.read_array_from_h5(file, "flat_earth_reference_phase", reference) != 0 ||
			reference.type() != CV_64F || reference.size() != phase.size() || !cv::checkRange(reference, true, nullptr) ||
			conversion.read_array_from_h5(file, "phase_valid_mask", validMask) != 0 ||
			validMask.type() != CV_8U || validMask.size() != phase.size() ||
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
		for (int row = 0; row < phase.rows; ++row) {
			const uchar* valid = validMask.ptr<uchar>(row);
			const int* samples = validSampleCount.ptr<int>(row);
			for (int column = 0; column < phase.cols; ++column) {
				if (valid[column] != 1 || samples[column] <= 0) {
					detail = "v2 phase-validity contract contains an unsupported masked sample";
					return false;
				}
			}
		}
		return true;
	}
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
			unwrapped_phase_file, unwrapped_phase, flat_earth_reference_phase, flatEarthContractDetail);
		if (!hasFlatEarthReference) {
		diagnosticContext.emit(DEM_LOG_ERROR, DEM_ERROR_INVALID_SHAPE, "input.flat_earth_reference_phase",
			"Versioned flat-earth contract is incomplete or unsupported; legacy coefficients are not a fallback.",
			flatEarthContractDetail, unwrapped_phase_file, "flat_earth_reference_phase");
		return DEM_ERROR_INVALID_SHAPE;
		}
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

