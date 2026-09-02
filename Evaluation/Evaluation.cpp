#include<Evaluation.h>
#include<FormatConversion.h>
#include<Utils.h>
#include<ComplexMat.h>
#include<Deflat.h>
#include<Hdf5IO.h>
#include<cmath>

#ifdef _DEBUG
#pragma comment(lib, "FormatConversion_d.lib")
#pragma comment(lib, "Utils_d.lib")
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Deflat_d.lib")
#else
#pragma comment(lib, "FormatConversion.lib")
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Deflat.lib")
#endif

static bool validateVersionedFlatEarthContract(const char* file, const cv::Mat& phase, cv::Mat& reference)
{
	FormatConversion conversion;
	int modelVersion = 0, sourceRowCount = 0, validationRequired = 0;
	if (conversion.read_int_from_h5(file, "flat_earth_model_version", &modelVersion) != 0 ||
		(modelVersion != 5 && modelVersion != 6) ||
		conversion.read_int_from_h5(file, "flat_earth_model_source_row_count", &sourceRowCount) != 0 || sourceRowCount < 1 ||
		conversion.read_int_from_h5(file, "flat_earth_reference_validation_required", &validationRequired) != 0 || validationRequired != 1) return false;
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
		!cv::checkRange(masterBurstTimes, true, nullptr) || !cv::checkRange(slaveBurstTimes, true, nullptr)) return false;
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
			value.rows < 1 || value.cols < 5 || !cv::checkRange(value, true, nullptr)) return false;
	}
	for (const char* dataset : phaseIntegerDatasets) {
		Mat value;
		if (conversion.read_array_from_h5(file, dataset, value) != 0 || value.type() != CV_32S ||
			value.cols != 1 || value.rows < 1) return false;
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
		semantics != expectedReferenceSemantics) return false;
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
		wavelength <= 0.0 || (transmitReceiveMode != 1 && transmitReceiveMode != 2) || rdeMaxIterations < 1 || zeroDopplerMaxIterations < 1 || jacobianCondition <= 0.0 || initialWindow <= 0.0 || maximumWindow < initialWindow || expansionFactor <= 1.0) return false;
	int maxSlaveSearchExpansions = 0;
	if (conversion.read_int_from_h5(file, "flat_earth_slave_search_max_expansions", &maxSlaveSearchExpansions) != 0 || maxSlaveSearchExpansions < 0) return false;
	Mat rdeStatistics, validMask, validSampleCount, rerampPhase, mappingCoefficients, mappingBurstIndices;
	const bool contractArraysValid = conversion.read_array_from_h5(file, "flat_earth_reference_phase", reference) == 0 &&
		reference.type() == CV_64F && reference.size() == phase.size() && cv::checkRange(reference, true, nullptr) &&
		conversion.read_array_from_h5(file, "phase_valid_mask", validMask) == 0 &&
		validMask.type() == CV_8U && validMask.size() == phase.size() &&
		conversion.read_array_from_h5(file, "phase_valid_sample_count", validSampleCount) == 0 &&
		validSampleCount.type() == CV_32S && validSampleCount.size() == phase.size() &&
		conversion.read_array_from_h5(file, "flat_earth_rde_burst_statistics", rdeStatistics) == 0 &&
		rdeStatistics.type() == CV_64F && rdeStatistics.rows == masterBurstTimes.rows && rdeStatistics.cols == 13 &&
		cv::checkRange(rdeStatistics, true, nullptr) &&
		(geometryOnly || (conversion.read_array_from_h5(file, "flat_earth_slave_registration_reramp_phase", rerampPhase) == 0 &&
		rerampPhase.type() == CV_64F && rerampPhase.size() == phase.size() && cv::checkRange(rerampPhase, true, nullptr))) &&
		conversion.read_array_from_h5(file, "flat_earth_slave_registration_mapping_coefficients", mappingCoefficients) == 0 &&
		mappingCoefficients.type() == CV_64F && mappingCoefficients.rows >= 1 && mappingCoefficients.cols == 6 && cv::checkRange(mappingCoefficients, true, nullptr) &&
		conversion.read_array_from_h5(file, "flat_earth_slave_registration_mapping_master_burst_indices", mappingBurstIndices) == 0 &&
		mappingBurstIndices.type() == CV_32S && mappingBurstIndices.rows == 1 && mappingBurstIndices.cols == mappingCoefficients.rows &&
		cv::countNonZero(validMask) == static_cast<int>(validMask.total()) &&
		cv::countNonZero(validSampleCount <= 0) == 0;
	if (!contractArraysValid) return false;
	if (geometryOnly) {
		int rerampExists = 0;
		int rerampSemanticsExists = 0;
		if (Hdf5IO::datasetExists(file, "flat_earth_slave_registration_reramp_phase", &rerampExists) != 0 ||
			Hdf5IO::datasetExists(file, "flat_earth_slave_registration_reramp_phase_semantics", &rerampSemanticsExists) != 0 ||
			rerampExists != rerampSemanticsExists) return false;
		if (rerampExists != 0) {
			std::string optionalRerampSemantics;
			if (conversion.read_array_from_h5(file, "flat_earth_slave_registration_reramp_phase", rerampPhase) != 0 ||
				rerampPhase.type() != CV_64F || rerampPhase.size() != phase.size() || !cv::checkRange(rerampPhase, true, nullptr) ||
				conversion.read_str_from_h5(file, "flat_earth_slave_registration_reramp_phase_semantics", optionalRerampSemantics) != 0 ||
				optionalRerampSemantics != "resampled_slave_deramp_demod_phase_registration_only_v1") return false;
		}
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
			rdeStatistics.at<double>(burst, 12) > maxSlaveSearchExpansions) return false;
	}
	return true;
}



static int readSatelliteParams(
	FormatConversion& conversion,
	const char* h5_file,
	double& prf,
	double& wavelength,
	double& start,
	double& end,
	cv::Mat& statevec,
	const char* error_head)
{
	int ret = 0;
	std::string start_time, end_time;
	ret = conversion.read_double_from_h5(h5_file, "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(h5_file, "carrier_frequency", &wavelength);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	wavelength = VEL_C / wavelength;
	ret = conversion.read_str_from_h5(h5_file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(h5_file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(h5_file, "state_vec", statevec);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	return 0;
}

Evaluation::Evaluation()
{
	memset(this->error_head, 0, 256);
	memset(this->parallel_error_head, 0, 256);
	strcpy(this->error_head, "Evaluation_DLL_ERROR: error happens when using ");
	strcpy(this->parallel_error_head, "Evaluation_DLL_ERROR: error happens when using parallel computing in function: ");
}

Evaluation::~Evaluation()
{

}

int Evaluation::PhasePreserve(const char* master_h5,
	const char* slave_h5,
	double* Output)
{
	if (master_h5 == NULL ||
		slave_h5 == NULL ||
		Output == NULL)
	{
		fprintf(stderr, "PhasePreserve(): input check failed!\n\n");
		return -1;
	}
	FormatConversion conversion; Deflat flat; Utils util;
	
	int ret = 0;
	double PhaseError = 0;
	ComplexMat master, slave;
	//读取SLC数据
	ret = conversion.read_slc_from_h5(master_h5, master);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (master.type() != CV_64F) master.convertTo(master, CV_64F);
	ret = conversion.read_slc_from_h5(slave_h5, slave);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
	int rows = master.GetRows();
	int cols = master.GetCols();
	Mat GCPS, slave_Gcps;
	//读取标志点数据
	ret = conversion.read_array_from_h5(master_h5, "GCP", GCPS);
	if (return_check(ret, "read_Gcps_from_h5()", error_head)) return -1;
	int Gcps_number = GCPS.rows;
	bool RealPhaseIsExisted = false;
	// removed unused: lonMax, lonMin, latMax, latMin (commented-out H5 reads)
	double wavelength, wavelength2, prf, prf2, start, start2, end, end2;
	int sceneHeight = rows, sceneWidth = cols;
	Mat lon_coef, lat_coef, statevec, statevec2;
	string start_time, start_time2, end_time, end_time2;
	//if (GCPS.cols == 6) RealPhaseIsExisted = true;
		//主星参数
		ret = readSatelliteParams(conversion, master_h5, prf, wavelength, start, end, statevec, error_head);
		if (ret < 0) return -1;
		//辅星参数
		ret = readSatelliteParams(conversion, slave_h5, prf2, wavelength2, start2, end2, statevec2, error_head);
		if (ret < 0) return -1;
		
	int interp_times = 32;
	int win_size = 16;
	int count = 0; //符合要求的标志点个数

	Mat Inphase_pre = Mat::zeros(Size(Gcps_number, 1), CV_64FC1);
	Mat Inphase_pro = Mat::zeros(Size(Gcps_number, 1), CV_64FC1);
	Mat Error = Mat::zeros(Size(Gcps_number, 1), CV_64FC1);
	
	

	for (int i = 0; i < Gcps_number; i++)
	{
		/*后验相位-插值*/
		ComplexMat master_win;
		ComplexMat slave_win;
		ComplexMat master_interp, slave_interp;
		int row = static_cast<int>(GCPS.at<double>(i, 0));
		int col = static_cast<int>(GCPS.at<double>(i, 1));
		if (row >= win_size && row < rows - win_size && col >= win_size && col < cols - win_size)
		{
			master_win = master(Range(row - win_size, row + win_size), Range(col - win_size, col + win_size));
			slave_win = slave(Range(row - win_size, row + win_size), Range(col - win_size, col + win_size));
		}
		else
			continue;
		resize(master_win.re, master_interp.re, Size(2 * win_size * interp_times, 2 * win_size * interp_times), 0, 0, INTER_CUBIC);
		resize(master_win.im, master_interp.im, Size(2 * win_size * interp_times, 2 * win_size * interp_times), 0, 0, INTER_CUBIC);
		resize(slave_win.re, slave_interp.re, Size(2 * win_size * interp_times, 2 * win_size * interp_times), 0, 0, INTER_CUBIC);
		resize(slave_win.im, slave_interp.im, Size(2 * win_size * interp_times, 2 * win_size * interp_times), 0, 0, INTER_CUBIC);
		Mat master_mod = master_interp.GetMod();
		Mat slave_mod = slave_interp.GetMod();
		Point master_max, slave_max;
		minMaxLoc(master_mod, NULL, NULL, NULL, &master_max);
		minMaxLoc(slave_mod, NULL, NULL, NULL, &slave_max);
		double master_phase = atan2(master_interp.im.at<double>(master_max.y, master_max.x),
			master_interp.re.at<double>(master_max.y, master_max.x));
		double slave_phase = atan2(slave_interp.im.at<double>(slave_max.y, slave_max.x),
			slave_interp.re.at<double>(slave_max.y, slave_max.x));
		double Inphase_post = atan2(sin(master_phase - slave_phase), cos(master_phase - slave_phase));
		Inphase_pre.at<double>(i) = Inphase_post;
		/*先验相位-斜距差*/
		double Inphase_prior = 0;
			////主卫星斜距
			//Mat sate1 = Mat::zeros(1, 3, CV_64F);
			//orbitStateVectors stateVectors(statevec, start, end);
			//stateVectors.applyOrbit();

			//double dopplerFrequency = 0.0;

			//Position groundPosition;
			//double lat, lon, height;
			//lat = GCPS.at<double>(i, 3);
			//lon = GCPS.at<double>(i, 2);
			//lon = lon > 180.0 ? (lon - 360.0) : lon;
			//height = GCPS.at<double>(i, 4);
			//Utils::ell2xyz(lon, lat, height, groundPosition);
			//int numOrbitVec = stateVectors.newStateVectors.rows;
			//double firstVecTime = 0.0;
			//double secondVecTime = 0.0;
			//double firstVecFreq = 0.0;
			//double secondVecFreq = 0.0;
			//double currentFreq, xdiff, ydiff, zdiff, distance = 1.0, zeroDopplerTime;
			////检测标志点位于哪两个轨道点之间
			//for (int ii = 0; ii < numOrbitVec; ii++) {
			//	Position orb_pos(stateVectors.newStateVectors.at<double>(ii, 1), stateVectors.newStateVectors.at<double>(ii, 2),
			//		stateVectors.newStateVectors.at<double>(ii, 3));
			//	Velocity orb_vel(stateVectors.newStateVectors.at<double>(ii, 4), stateVectors.newStateVectors.at<double>(ii, 5),
			//		stateVectors.newStateVectors.at<double>(ii, 6));
			//	currentFreq = 0;
			//	xdiff = groundPosition.x - orb_pos.x;
			//	ydiff = groundPosition.y - orb_pos.y;
			//	zdiff = groundPosition.z - orb_pos.z;
			//	distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
			//	currentFreq = 2.0 * (xdiff * orb_vel.vx + ydiff * orb_vel.vy + zdiff * orb_vel.vz) / (wavelength * distance);
			//	if (ii == 0 || (firstVecFreq - dopplerFrequency) * (currentFreq - dopplerFrequency) > 0) {
			//		firstVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
			//		firstVecFreq = currentFreq;
			//	}
			//	else {
			//		secondVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
			//		secondVecFreq = currentFreq;
			//		break;
			//	}
			//}

			//if ((firstVecFreq - dopplerFrequency) * (secondVecFreq - dopplerFrequency) >= 0.0) {
			//	fprintf(stderr, "SLC_deramp(): orbit mismatch!\n");
			//	return -1;
			//}

			//double lowerBoundTime = firstVecTime;
			//double upperBoundTime = secondVecTime;
			//double lowerBoundFreq = firstVecFreq;
			//double upperBoundFreq = secondVecFreq;
			//double midTime, midFreq;
			//double diffTime = fabs(upperBoundTime - lowerBoundTime);
			//double absLineTimeInterval = 1.0 / prf;

			//int totalIterations = (int)(diffTime / absLineTimeInterval) + 1;
			//int numIterations = 0; Position pos; Velocity vel;
			////对两个点之间（相差10s）进行进一步插值检测找到标志点对应的具体卫星位置
			//while (diffTime > absLineTimeInterval * 0.1 && numIterations <= totalIterations) {

			//	midTime = (upperBoundTime + lowerBoundTime) / 2.0;
			//	stateVectors.getPosition(midTime, pos);
			//	stateVectors.getVelocity(midTime, vel);
			//	xdiff = groundPosition.x - pos.x;
			//	ydiff = groundPosition.y - pos.y;
			//	zdiff = groundPosition.z - pos.z;
			//	distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
			//	midFreq = 2.0 * (xdiff * vel.vx + ydiff * vel.vy + zdiff * vel.vz) / (wavelength * distance);
			//	if ((midFreq - dopplerFrequency) * (lowerBoundFreq - dopplerFrequency) > 0.0) {
			//		lowerBoundTime = midTime;
			//		lowerBoundFreq = midFreq;
			//	}
			//	else if ((midFreq - dopplerFrequency) * (upperBoundFreq - dopplerFrequency) > 0.0) {
			//		upperBoundTime = midTime;
			//		upperBoundFreq = midFreq;
			//	}
			//	else if (fabs(midFreq - dopplerFrequency) < 0.01) {
			//		zeroDopplerTime = midTime;
			//		break;
			//	}

			//	diffTime = fabs(upperBoundTime - lowerBoundTime);
			//	numIterations++;
			//}
			//zeroDopplerTime = lowerBoundTime - lowerBoundFreq * (upperBoundTime - lowerBoundTime) / (upperBoundFreq - lowerBoundFreq);
			//stateVectors.getPosition(zeroDopplerTime, pos);
			//sate1.at<double>(0) = pos.x;
			//sate1.at<double>(1) = pos.y;
			//sate1.at<double>(2) = pos.z;
			//double r;
			//Mat XYZ, LLH(1, 3, CV_64F), tt;
			//LLH.at<double>(0, 0) = GCPS.at<double>(i, 3);
			//LLH.at<double>(0, 1) = GCPS.at<double>(i, 2);
			//LLH.at<double>(0, 2) = GCPS.at<double>(i, 4);
			//util.ell2xyz(LLH, XYZ);
			//tt = XYZ - sate1;
			//r = cv::norm(tt, cv::NORM_L2);
			//master_phase = -r / wavelength * 4 * PI;

			////辅卫星斜距
			//Mat sate2 = Mat::zeros(1, 3, CV_64F);
			//orbitStateVectors stateVectors2(statevec2, start2, end2);
			//stateVectors2.applyOrbit();

			//dopplerFrequency = 0.0;

			//numOrbitVec = stateVectors2.newStateVectors.rows;
			//firstVecTime = 0.0;
			//secondVecTime = 0.0;
			//firstVecFreq = 0.0;
			//secondVecFreq = 0.0;
			//distance = 1.0;
			//for (int ii = 0; ii < numOrbitVec; ii++) {
			//	Position orb_pos(stateVectors2.newStateVectors.at<double>(ii, 1), stateVectors2.newStateVectors.at<double>(ii, 2),
			//		stateVectors2.newStateVectors.at<double>(ii, 3));
			//	Velocity orb_vel(stateVectors2.newStateVectors.at<double>(ii, 4), stateVectors2.newStateVectors.at<double>(ii, 5),
			//		stateVectors2.newStateVectors.at<double>(ii, 6));
			//	currentFreq = 0;
			//	xdiff = groundPosition.x - orb_pos.x;
			//	ydiff = groundPosition.y - orb_pos.y;
			//	zdiff = groundPosition.z - orb_pos.z;
			//	distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
			//	currentFreq = 2.0 * (xdiff * orb_vel.vx + ydiff * orb_vel.vy + zdiff * orb_vel.vz) / (wavelength2 * distance);
			//	if (ii == 0 || (firstVecFreq - dopplerFrequency) * (currentFreq - dopplerFrequency) > 0) {
			//		firstVecTime = stateVectors2.newStateVectors.at<double>(ii, 0);
			//		firstVecFreq = currentFreq;
			//	}
			//	else {
			//		secondVecTime = stateVectors2.newStateVectors.at<double>(ii, 0);
			//		secondVecFreq = currentFreq;
			//		break;
			//	}
			//}

			//if ((firstVecFreq - dopplerFrequency) * (secondVecFreq - dopplerFrequency) >= 0.0) {
			//	fprintf(stderr, "SLC_deramp(): orbit mismatch!\n");
			//	return -1;
			//}

			//lowerBoundTime = firstVecTime;
			//upperBoundTime = secondVecTime;
			//lowerBoundFreq = firstVecFreq;
			//upperBoundFreq = secondVecFreq;

			//diffTime = fabs(upperBoundTime - lowerBoundTime);
			//absLineTimeInterval = 1.0 / prf2;

			//totalIterations = (int)(diffTime / absLineTimeInterval) + 1;
			//numIterations = 0;
			//while (diffTime > absLineTimeInterval * 0.1 && numIterations <= totalIterations) {

			//	midTime = (upperBoundTime + lowerBoundTime) / 2.0;
			//	stateVectors2.getPosition(midTime, pos);
			//	stateVectors2.getVelocity(midTime, vel);
			//	xdiff = groundPosition.x - pos.x;
			//	ydiff = groundPosition.y - pos.y;
			//	zdiff = groundPosition.z - pos.z;
			//	distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
			//	midFreq = 2.0 * (xdiff * vel.vx + ydiff * vel.vy + zdiff * vel.vz) / (wavelength2 * distance);
			//	if ((midFreq - dopplerFrequency) * (lowerBoundFreq - dopplerFrequency) > 0.0) {
			//		lowerBoundTime = midTime;
			//		lowerBoundFreq = midFreq;
			//	}
			//	else if ((midFreq - dopplerFrequency) * (upperBoundFreq - dopplerFrequency) > 0.0) {
			//		upperBoundTime = midTime;
			//		upperBoundFreq = midFreq;
			//	}
			//	else if (fabs(midFreq - dopplerFrequency) < 0.01) {
			//		zeroDopplerTime = midTime;
			//		break;
			//	}

			//	diffTime = fabs(upperBoundTime - lowerBoundTime);
			//	numIterations++;
			//}
			//zeroDopplerTime = lowerBoundTime - lowerBoundFreq * (upperBoundTime - lowerBoundTime) / (upperBoundFreq - lowerBoundFreq);

			//double r2;
			//stateVectors2.getPosition(zeroDopplerTime, pos);
			//sate2.at<double>(0) = pos.x;
			//sate2.at<double>(1) = pos.y;
			//sate2.at<double>(2) = pos.z;
			//LLH.at<double>(0, 0) = GCPS.at<double>(i, 3);
			//LLH.at<double>(0, 1) = GCPS.at<double>(i, 2);
			//LLH.at<double>(0, 2) = GCPS.at<double>(i, 4);
			//util.ell2xyz(LLH, XYZ);
			//tt = XYZ - sate2;
			//r2 = cv::norm(tt, cv::NORM_L2);
			//slave_phase = -r2 / wavelength2 * 4 * PI;

			//先验相位
		double r = GCPS.at<double>(i, 5);
		double r2 = GCPS.at<double>(i, 6);
		master_phase = -r * 4 * PI / wavelength;
		slave_phase = -r2 * 4 * PI / wavelength2;
		Inphase_prior = atan2(sin(master_phase - slave_phase), cos(master_phase - slave_phase));
		Inphase_pro.at<double>(i) = Inphase_prior;
		Error.at<double>(i) = atan2(sin(Inphase_post - Inphase_prior), cos(Inphase_post - Inphase_prior));
		PhaseError += Error.at<double>(i);
		count++;
	}
	double relevant_error = 0;
	double count2 = 0;
	for (int i = 0; i < Gcps_number; i++)
		for (int j = i + 1; j < Gcps_number; j++)
		{
				relevant_error += pow(atan2(sin(Inphase_pre.at<double>(i) - Inphase_pre.at<double>(j) - (Inphase_pro.at<double>(i) - Inphase_pro.at<double>(j))),
					cos(Inphase_pre.at<double>(i) - Inphase_pre.at<double>(j) - (Inphase_pro.at<double>(i) - Inphase_pro.at<double>(j)))),2);
				count2++;
		}
	relevant_error = sqrt(relevant_error / count2);
	// util.cvmat2bin("D:\\Test\\Error.bin", Error);
	*Output = sqrt(PhaseError / count);
	return 0;
}

int Evaluation::Regis(const char* master_h5,  const char* slave_regis_h5, Mat& coherence, Mat& regis_error)
{
	if(master_h5 == NULL ||
		slave_regis_h5 == NULL)
	{
		fprintf(stderr, "Regis(): input check failed!\n\n");
		return -1;
	}
	FormatConversion conversion; Utils util;

	int ret = 0;
	double PhaseError = 0;
	ComplexMat master, slave;
	//读取SLC数据
	ret = conversion.read_slc_from_h5(master_h5, master);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (master.type() != CV_64F) master.convertTo(master, CV_64F);
	ret = conversion.read_slc_from_h5(slave_regis_h5, slave);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
	int rows = master.GetRows();
	int cols = master.GetCols();
	Mat GCPS, slave_Gcps;
	//读取标志点数据
	ret = conversion.read_array_from_h5(master_h5, "gcps", GCPS);
	if (return_check(ret, "read_Gcps_from_h5()", error_head)) return -1;
	int offset_row, offset_col;
	ret = conversion.read_int_from_h5(master_h5, "offset_row", &offset_row);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(master_h5, "offset_col", &offset_col);
	vector<double> r_error, a_error;
	vector<int> Gcp_SN;
	int Gcps_number = GCPS.rows;
	int interp_times = 32;
	int win_size = 32;
	int interp_size = interp_times * win_size;
	int count = 0; //符合要求的标志点个数
	for (int i = 0; i < Gcps_number; i++)
	{
		ComplexMat master_win, master_fft;
		ComplexMat slave_win, slave_fft;
		ComplexMat master_interp(interp_size, interp_size),
			slave_interp(interp_size, interp_size),
			master_interp_fft(interp_size, interp_size),
			slave_interp_fft(interp_size, interp_size);
		int row = static_cast<int>(GCPS.at<double>(i, 0));
		int col = static_cast<int>(GCPS.at<double>(i, 1));
		if ((row - offset_row) >= win_size / 2 && (row - offset_row) < rows - win_size / 2 
			&& (col - offset_col) >= win_size / 2 && (col - offset_col) < cols - win_size / 2)
		{
			master_win = master(Range(row - offset_row - win_size/2, row - offset_row + win_size/2), Range(col - offset_col - win_size/2, col - offset_col + win_size/2));
			slave_win = slave(Range(row - offset_row - win_size / 2, row - offset_row + win_size / 2), Range(col - offset_col - win_size / 2, col - offset_col + win_size / 2));
		}
		else
			continue;
		//resize(master_win.re, master_interp.re, Size( win_size * interp_times,  win_size * interp_times), 0, 0, INTER_CUBIC);
		//resize(master_win.im, master_interp.im, Size( win_size * interp_times, win_size * interp_times), 0, 0, INTER_CUBIC);
		//resize(slave_win.re, slave_interp.re, Size( win_size * interp_times, win_size * interp_times), 0, 0, INTER_CUBIC);
		//resize(slave_win.im, slave_interp.im, Size( win_size * interp_times, win_size * interp_times), 0, 0, INTER_CUBIC);
		
		FFT2(master_win, master_interp, win_size, interp_times);
		FFT2(slave_win, slave_interp, win_size, interp_times);;
		Mat master_mod = master_interp.GetMod();
		Mat slave_mod = slave_interp.GetMod();
		Point master_max, slave_max;
		double max1, max2;
		minMaxLoc(master_mod, NULL, &max1, NULL, &master_max);
		minMaxLoc(slave_mod, NULL, &max2, NULL, &slave_max);
		r_error.push_back(double(master_max.x - slave_max.x) / interp_times);
		a_error.push_back(double(master_max.y - slave_max.y) / interp_times);
		Gcp_SN.push_back(i + 1);
		count++;
	}
	Mat regis_error_tmp = Mat::zeros(Size(2, count), CV_64FC1);
	for (int i = 0; i < count; i++)
	{
		regis_error_tmp.at<double>(i, 0) = a_error.at(i);
		regis_error_tmp.at<double>(i, 1) = r_error.at(i);
	}
	regis_error = regis_error_tmp;
	util.complex_coherence(master, slave, coherence);

	//util.real_coherence(master, slave, coherence);
	return 0;
}

int Evaluation::Unwrap(const char* master_h5, const char* slave_regis_h5, const char* phase_unwrapped_h5, double* Output)
{
	if (master_h5 == NULL ||
		slave_regis_h5 == NULL ||
		phase_unwrapped_h5 == NULL ||
		Output == NULL)
	{
		fprintf(stderr, "Unwrap(): input check failed!\n\n");
		return -1;
	}
	FormatConversion conversion; Deflat flat; Utils util;

	int ret = 0;
	double PhaseError = 0;
	ComplexMat master, slave;
	//读取SLC数据
	ret = conversion.read_slc_from_h5(master_h5, master);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (master.type() != CV_64F) master.convertTo(master, CV_64F);
	ret = conversion.read_slc_from_h5(slave_regis_h5, slave);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
	int rows = master.GetRows();
	int cols = master.GetCols();
	Mat GCPS, slave_Gcps;
	//读取标志点数据
	ret = conversion.read_array_from_h5(master_h5, "GCP", GCPS);
	if (return_check(ret, "read_Gcps_from_h5()", error_head)) return -1;
	int Gcps_number = GCPS.rows;
	Mat GCPS_New = Mat::zeros(Size(6, Gcps_number), CV_64FC1);
	bool RealPhaseIsExisted = false;
	// removed unused: lonMax, lonMin, latMax, latMin (commented-out H5 reads)
	double wavelength, wavelength2, prf, prf2, start, start2, end, end2;
	int sceneHeight = rows, sceneWidth = cols;
	Mat lon_coef, lat_coef, statevec, statevec2;
	string start_time, start_time2, end_time, end_time2;
	if (GCPS.cols == 6) RealPhaseIsExisted = true;
	else
	{
		GCPS.copyTo(GCPS_New(Range(0, Gcps_number), Range(0, 5)));
		//主星参数
		ret = readSatelliteParams(conversion, master_h5, prf, wavelength, start, end, statevec, error_head);
		if (ret < 0) return -1;
		//辅星参数
		ret = readSatelliteParams(conversion, slave_regis_h5, prf2, wavelength2, start2, end2, statevec2, error_head);
		if (ret < 0) return -1;
	}

	for (int i = 0; i < Gcps_number; i++)
	{
		/*解缠相位*/
		int row = static_cast<int>(GCPS.at<double>(i, 0));
		int col = static_cast<int>(GCPS.at<double>(i, 1));
		Mat phase_unwrapped;
		ret = conversion.read_subarray_from_h5(phase_unwrapped_h5, "phase", row - 1, col - 1, 1, 1, phase_unwrapped);
		double Inphase_unwrapped = phase_unwrapped.at<double>(0, 0);
		/*真实相位*/
		double Inphase_prior = 0;
		if (RealPhaseIsExisted)
			Inphase_prior = GCPS.at<double>(i, 5);
		else
		{
			//主卫星斜距
			Mat sate1 = Mat::zeros(1, 3, CV_64F);
			orbitStateVectors stateVectors(statevec, start, end);
			stateVectors.applyOrbit();

			double dopplerFrequency = 0.0;

			Position groundPosition;
			double lat, lon, height;
			lat = GCPS.at<double>(i, 3);
			lon = GCPS.at<double>(i, 2);
			lon = lon > 180.0 ? (lon - 360.0) : lon;
			height = GCPS.at<double>(i, 4);
			Utils::ell2xyz(lon, lat, height, groundPosition);
			double zeroDopplerTime, distance;
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, 1.0 / prf, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
				fprintf(stderr, "SLC_deramp(): orbit mismatch!\n");
				return -1;
			}
			Position pos;
			stateVectors.getPosition(zeroDopplerTime, pos);
			sate1.at<double>(0) = pos.x;
			sate1.at<double>(1) = pos.y;
			sate1.at<double>(2) = pos.z;
			double r;
			Mat XYZ, LLH(1, 3, CV_64F), tt;
			LLH.at<double>(0, 0) = GCPS.at<double>(i, 3);
			LLH.at<double>(0, 1) = GCPS.at<double>(i, 2);
			LLH.at<double>(0, 2) = GCPS.at<double>(i, 4);
			util.ell2xyz(LLH, XYZ);
			tt = XYZ - sate1;
			r = cv::norm(tt, cv::NORM_L2);
			double master_phase = -r / wavelength * 4 * PI;

			//辅卫星斜距
			Mat sate2 = Mat::zeros(1, 3, CV_64F);
			orbitStateVectors stateVectors2(statevec2, start2, end2);
			stateVectors2.applyOrbit();

			dopplerFrequency = 0.0;

			if (!Utils::findZeroDopplerTime(stateVectors2, groundPosition, wavelength2, 1.0 / prf2, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
				fprintf(stderr, "SLC_deramp(): orbit mismatch!\n");
				return -1;
			}

			double r2;
			stateVectors2.getPosition(zeroDopplerTime, pos);
			sate2.at<double>(0) = pos.x;
			sate2.at<double>(1) = pos.y;
			sate2.at<double>(2) = pos.z;
			LLH.at<double>(0, 0) = GCPS.at<double>(i, 3);
			LLH.at<double>(0, 1) = GCPS.at<double>(i, 2);
			LLH.at<double>(0, 2) = GCPS.at<double>(i, 4);
			util.ell2xyz(LLH, XYZ);
			tt = XYZ - sate2;
			r2 = cv::norm(tt, cv::NORM_L2);
			double slave_phase = -r2 / wavelength2 * 4 * PI;

			//先验相位
			Inphase_prior = atan2(sin(master_phase - slave_phase), cos(master_phase - slave_phase));
			GCPS_New.at<double>(i, 5) = Inphase_prior;
		}
		PhaseError += pow(atan2(sin(Inphase_unwrapped - Inphase_prior), cos(Inphase_unwrapped - Inphase_prior)), 2);
	}
	*Output = sqrt(PhaseError / Gcps_number);
	return 0;
}

int Evaluation::FFT2(ComplexMat src, ComplexMat& dst, int win_size, int interp_times)
{
	Point master_max; // Point slave_max; (unused)
	// removed unused: max2 (never read after assignment)
	double max1 = 0;
	Utils util;
	int interp_size = win_size * interp_times;
	ComplexMat tmp, tmp_fft, tmp_fft_half;
	ComplexMat tmp_interp_fft(1, interp_size), tmp_interp(1, interp_size),
		tmp_interp_fft2(interp_size, 1), tmp_interp2(interp_size,1);
	ComplexMat tmp_fft1(win_size, interp_size), tmp_fft2(interp_size, interp_size);
	for (int i = 0; i < win_size; i++)
	{
		tmp = src(Range(i, i + 1), Range(0, win_size));
		util.fft2(tmp, tmp_fft);
		tmp_fft = tmp_fft * interp_times;
		tmp_interp_fft = ComplexMat(1, interp_size);
		tmp_fft_half = tmp_fft(Range(0, 1), Range(0, win_size / 2));
		tmp_interp_fft.SetValue(Range(0, 1), Range(0, win_size / 2), tmp_fft_half);
		tmp_fft_half = tmp_fft(Range(0, 1), Range(win_size/2, win_size));
		tmp_interp_fft.SetValue(Range(0, 1), Range(interp_size-win_size/2, interp_size), tmp_fft_half);
		util.ifft2(tmp_interp_fft, tmp_interp);
		tmp_interp = tmp_interp * ((double)1 / interp_times / interp_times);
		tmp_fft1.SetValue(Range(i, i + 1), Range(0, interp_size),tmp_interp);
		minMaxLoc(tmp_interp.GetMod(), NULL, &max1, NULL, &master_max);
	}

	for (int i = 0; i < interp_size; i++)
	{
		tmp = tmp_fft1(Range(0, win_size), Range(i, i+1));
		util.fft2(tmp, tmp_fft);
		tmp_fft = tmp_fft * interp_times;
		tmp_interp_fft2 = ComplexMat(interp_size,1);
		tmp_fft_half = tmp_fft(Range(0, win_size / 2), Range(0, 1));
		tmp_interp_fft2.SetValue(Range(0, win_size / 2), Range(0, 1), tmp_fft_half);
		tmp_fft_half = tmp_fft( Range(win_size / 2, win_size), Range(0, 1));
		tmp_interp_fft2.SetValue(Range(interp_size - win_size / 2, interp_size), Range(0, 1), tmp_fft_half);
		util.ifft2(tmp_interp_fft2, tmp_interp2);
		tmp_interp2 = tmp_interp2 * ((double)1 / interp_times / interp_times);
		tmp_fft2.SetValue(Range(0, interp_size), Range(i, i + 1), tmp_interp2);
	}
	
	minMaxLoc(tmp_fft2.GetMod(), NULL, &max1, NULL, &master_max);
	dst = tmp_fft2;
	return 0;
}

int Evaluation::Pos(const char* unwrapped_phase_file, const char* project_path, const char* GCP_path, double* lat_abs, double* lat_rel, double* lon_abs, double* lon_rel, double* height_abs, double* height_rel, NewtonProgressCallback cb)
{
	FormatConversion conversion; Utils util;
	int nr, nc, ret, offset_row, offset_col;
	double time_interval1, time_interval2;
	string source_1, source_2;
	Mat unwrapped_phase, flat_phase_coefficient, flat_earth_reference_phase, gcps, temp, range_spacing,
		stateVec1, stateVec2, lat_coefficient, lon_coefficient, prf1, prf2, carrier_frequency;
	ret = conversion.read_array_from_h5(unwrapped_phase_file, "phase", unwrapped_phase);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	nr = unwrapped_phase.rows; nc = unwrapped_phase.cols;
	if (nr < 1 || nc < 1)
	{
		fprintf(stderr, "dem_newton_iter(): invalid unwrapped_phase !\n");
		return -1;
	}
	int schemaExists = 0;
	int modelExists = 0;
	if (Hdf5IO::datasetExists(unwrapped_phase_file, "phase_processing_schema_version", &schemaExists) != 0 ||
		Hdf5IO::datasetExists(unwrapped_phase_file, "flat_earth_model_version", &modelExists) != 0) {
		fprintf(stderr, "Evaluation::Pos(): cannot inspect flat-earth contract version!\n");
		return -1;
	}
	int phaseSchemaVersion = 0;
	if (schemaExists != 0 &&
		(conversion.read_int_from_h5(unwrapped_phase_file, "phase_processing_schema_version", &phaseSchemaVersion) != 0 ||
		 (phaseSchemaVersion != 1 && phaseSchemaVersion != 2))) {
		fprintf(stderr, "Evaluation::Pos(): unsupported phase-processing schema version!\n");
		return -1;
	}
	const bool requiresVersionedReference = phaseSchemaVersion == 2 || modelExists != 0;
	if (requiresVersionedReference && phaseSchemaVersion != 2) {
		fprintf(stderr, "Evaluation::Pos(): versioned flat-earth model requires phase-processing schema version 2!\n");
		return -1;
	}
	const bool hasFlatEarthReference = requiresVersionedReference &&
		validateVersionedFlatEarthContract(unwrapped_phase_file, unwrapped_phase, flat_earth_reference_phase);
	if (requiresVersionedReference && !hasFlatEarthReference) {
		fprintf(stderr, "Evaluation::Pos(): versioned flat-earth contract is incomplete or unsupported; legacy coefficients are not a fallback!\n");
		return -1;
	}
	if (hasFlatEarthReference) {
		if (flat_earth_reference_phase.type() != CV_64F || flat_earth_reference_phase.size() != unwrapped_phase.size()) {
			fprintf(stderr, "Evaluation::Pos(): flat_earth_reference_phase does not match phase grid!\n");
			return -1;
		}
	}
	else {
		ret = conversion.read_array_from_h5(unwrapped_phase_file, "flat_phase_coefficient", flat_phase_coefficient);
		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
		if (flat_phase_coefficient.type() != CV_64F || flat_phase_coefficient.rows != 1 || flat_phase_coefficient.cols != 6) {
			fprintf(stderr, "Evaluation::Pos(): legacy flat_phase_coefficient must be one 1x6 row!\n");
			return -1;
		}
	}
	PathResolver::SourcePathPair sourcePaths;
	PathResolver::Error pathError = PathResolver::Error::None;
	string pathDetail;
	if (!PathResolver::readSourcePathPair(unwrapped_phase_file, project_path, sourcePaths, &pathError, &pathDetail))
	{
		fprintf(stderr, "Evaluation::Pos(): %s (%s)\n", PathResolver::errorMessage(pathError), pathDetail.c_str());
		return -1;
	}
	source_1 = sourcePaths.source1.utf8;
	source_2 = sourcePaths.source2.utf8;
	ret = conversion.read_array_from_h5(GCP_path, "GCP", gcps);
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
	ret = conversion.read_array_from_h5(source_1.c_str(), "lat_coefficient", lat_coefficient);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "lon_coefficient", lon_coefficient);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(source_1.c_str(), "carrier_frequency", carrier_frequency);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;

	//寻找图像范围内的控制点信息

	int num_gcps = gcps.rows;
	int row, col, i = 0;
	bool b_gcp = false;
	for (i = 0; i < num_gcps; i++)
	{
		row = (int)gcps.at<double>(i, 0);
		col = (int)gcps.at<double>(i, 1);
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
		llh.at<double>(0, 0) = gcps.at<double>(i, 3);
		llh.at<double>(0, 1) = gcps.at<double>(i, 2);
		llh.at<double>(0, 2) = gcps.at<double>(i, 4);
		row = row - offset_row; col = col - offset_col;
	}
	else
	{
		row_coord.at<double>(0, 0) = offset_row + double(nr) / 2.0;
		col_coord.at<double>(0, 0) = offset_col + double(nc) / 2.0;
		ret = util.coord_conversion(lat_coefficient, row_coord, col_coord, lat);
		if (return_check(ret, "coord_conversion", error_head)) return -1;
		ret = util.coord_conversion(lon_coefficient, row_coord, col_coord, lon);
		if (return_check(ret, "coord_conversion", error_head)) return -1;
		llh.at<double>(0, 0) = lat.at<double>(0, 0);
		llh.at<double>(0, 1) = lon.at<double>(0, 0);
		llh.at<double>(0, 2) = 0.0;
		row = (int)nr / 2; col = (int)nc / 2;
	}
	ret = util.ell2xyz(llh, xyz_ground);
	if (return_check(ret, "ell2xyz", error_head)) return -1;


	/*
	* 轨道插值
	*/
	ret = util.stateVec_interp(stateVec1, time_interval1, stateVec1);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
	ret = util.stateVec_interp(stateVec2, time_interval2, stateVec2);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
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
	if (return_check(ret, "coord_conversion", error_head)) return -1;
	ret = util.coord_conversion(lon_coefficient, row_coord, col_coord, lon);
	if (return_check(ret, "coord_conversion", error_head)) return -1;
	llh_upperleft.at<double>(0, 0) = lat.at<double>(0, 0);
	llh_upperleft.at<double>(0, 1) = lon.at<double>(0, 0);
	llh_upperleft.at<double>(0, 2) = 0.0;
	ret = util.ell2xyz(llh_upperleft, xyz);
	if (return_check(ret, "ell2xyz", error_head)) return -1;
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
	if (hasFlatEarthReference) {
		unwrapped_phase += flat_earth_reference_phase;
	}
	else {
	#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			Mat temp(1, 6, CV_64F);
			for (int j = 0; j < nc; j++)
			{
				temp.at<double>(0, 0) = 1.0;
				temp.at<double>(0, 1) = i;
				temp.at<double>(0, 2) = j;
				temp.at<double>(0, 3) = i * j;
				temp.at<double>(0, 4) = i * i;
				temp.at<double>(0, 5) = j * j;
				unwrapped_phase.at<double>(i, j) = unwrapped_phase.at<double>(i, j) + sum(temp.mul(flat_phase_coefficient))[0];
			}
		}
	}
	//控制点绝对相位计算
	double r_main = sqrt(sum((sate1(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground).mul(sate1(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground))[0]);
	double r_slave = sqrt(sum((sate2(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground).mul(sate2(cv::Range(row, row + 1), cv::Range(0, 3)) - xyz_ground))[0]);
	int mode = 1;
	double C = 4*PI; //自发自收
	double lambda = 3e8 / (carrier_frequency.at<double>(0, 0) + 1e-10);
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
	if (mode == 1)
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
	bool success = Utils::newton_iter_core(15, P1, P2, P3, Satellite_M_T_Position, Satellite_S_T_Position,
	                        Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs,
	                        R_M, R_F, fd, lambda, cb);
	if (!success)
	{
		return -2;
	}
	Mat GCPs;
	ret = conversion.read_array_from_h5(GCP_path, "GCP", GCPs);
	int GCP_count = GCPs.rows;
	int count = 0, count2 = 0;
	double lat_1 = 0, lat_2 = 0, lon_1 = 0, lon_2 = 0, height_1 = 0, height_2 = 0;
	double height_offset = 0;
	for (int i = 0; i < GCP_count; i++)
	{
		int row1 = static_cast<int>(GCPs.at<double>(i, 0));
		int col1 = static_cast<int>(GCPs.at<double>(i, 1));
		if (row1 >= offset_row && row1 < offset_row + nr && col1 >= offset_col && col1 < offset_col + nc)
		{
			double lat_val = 0, lon_val = 0, h_val = 0;
			ret = Utils::xyz2ell(
				P1.at<double>(row1 - offset_row, col1 - offset_col),
				P2.at<double>(row1 - offset_row, col1 - offset_col),
				P3.at<double>(row1 - offset_row, col1 - offset_col),
				lat_val, lon_val, h_val
			);
			height_offset = h_val;
			lat_1 += GCPs.at<double>(i, 3) - lat_val;
			lon_1 += GCPs.at<double>(i, 2) - lon_val;
			height_1 += GCPs.at<double>(i, 4) - h_val;
			count++;
			for (int j = i; j < GCP_count; j++)
			{
				int row2 = static_cast<int>(GCPs.at<double>(j, 0));
				int col2 = static_cast<int>(GCPs.at<double>(j, 1));
				if (row2 >= offset_row && row2 < offset_row + nr && col2 >= offset_col && col2 < offset_col + nc)
				{
					double lat_val2 = 0, lon_val2 = 0, h_val2 = 0;
					ret = Utils::xyz2ell(
						P1.at<double>(row2 - offset_row, col2 - offset_col),
						P2.at<double>(row2 - offset_row, col2 - offset_col),
						P3.at<double>(row2 - offset_row, col2 - offset_col),
						lat_val2, lon_val2, h_val2
					);
					lat_2 += GCPs.at<double>(i, 3) - lat_val - (GCPs.at<double>(j, 3) - lat_val2);
					lon_2 += GCPs.at<double>(i, 2) - lon_val - (GCPs.at<double>(j, 2) - lon_val2);
					height_2 += GCPs.at<double>(i, 4) - h_val - (GCPs.at<double>(j, 4) - h_val2);
					count2++;
				}
				else continue;
			}

		}
		else continue;
	}
	height_1 = height_1 + count * (llh.at<double>(0, 2) - height_offset);
		lon_1 /= count; lat_1 /= count;
		lon_2 /= count2; lat_2 /= count2;
		height_1 /= count; height_2 /= count2;
	*lon_abs = lon_1; *lon_rel = lon_2; *lat_abs = lat_1; *lat_rel = lat_2; *height_abs = height_1; *height_rel = height_2;
	return 0;
}
