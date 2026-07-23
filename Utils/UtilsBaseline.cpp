#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\Hdf5IO.h"

#include <cmath>
#include <string>
#include <vector>

using namespace cv;
using namespace std;

extern int utc_to_gps(const char* utc_time, double* gps_time);
int Utils::baseline_estimation(
	const Mat& stateVec1,
	const Mat& stateVec2, 
	const Mat& lon_coef,
	const Mat& lat_coef, 
	int offset_row,
	int offset_col,
	int scene_height, 
	int scene_width,
	double time_interval,
	double time_interval2,
	double* B_effect,
	double* B_parallel,
	double* sigma_B_effect, 
	double* sigma_B_parallel
)
{
	if (stateVec1.cols != 7 ||
		stateVec1.rows < 7 ||
		stateVec2.cols != 7 ||
		stateVec2.rows < 7 ||
		stateVec1.type() != CV_64F ||
		stateVec2.type() != CV_64F ||
		lon_coef.cols != 32 ||
		lon_coef.rows != 1 ||
		lon_coef.type() != CV_64F ||
		lat_coef.cols != 32 ||
		lat_coef.rows != 1 ||
		lat_coef.type() != CV_64F ||
		B_effect == NULL ||
		B_parallel == NULL ||
		time_interval < 0.0 ||
		time_interval2 < 0.0 ||
		scene_height < 7 ||
		scene_width < 0
		)
	{
		fprintf(stderr, "baseline_estimation(): input check failed!\n");
		return -1;
	}
	int ret;
	int rows = scene_height; int cols = scene_width;
	/*
	* 轨道插值
	*/
	Mat state_vec1, state_vec2;
	stateVec1.copyTo(state_vec1);
	stateVec2.copyTo(state_vec2);
	ret = stateVec_interp(state_vec1, time_interval, state_vec1);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
	ret = stateVec_interp(state_vec2, time_interval2, state_vec2);
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
	int j_col = (int)cols / 2;
	row.create(rows, 1, CV_64F); col.create(rows, 1, CV_64F);
	for (int i = 0; i < rows; i++)
	{
		row.at<double>(i, 0) = i + offset_row;
		col.at<double>(i, 0) = j_col + offset_col;
	}
	Mat lon, lat, lon_coefficient, lat_coefficient;
	lon_coef.copyTo(lon_coefficient);
	lat_coef.copyTo(lat_coefficient);
	ret = coord_conversion(lon_coefficient, row, col, lon);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	ret = coord_conversion(lat_coefficient, row, col, lat);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;


	/*
	* 图像1成像点位置计算
	*/

	Mat sate1 = Mat::zeros(rows, 3, CV_64F);
	Mat sate2 = Mat::zeros(rows, 3, CV_64F);
	Mat satev1 = Mat::zeros(rows, 3, CV_64F);
	Mat satev2 = Mat::zeros(rows, 3, CV_64F);
	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat.at<double>(i, 0);
		tmp.at<double>(0, 1) = lon.at<double>(i, 0);
		tmp.at<double>(0, 2) = 0;
		ell2xyz(tmp, xyz);

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
			sate1_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev1(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	* 图像2成像点位置计算
	*/


	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat.at<double>(i, 0);
		tmp.at<double>(0, 1) = lon.at<double>(i, 0);
		tmp.at<double>(0, 2) = 0;
		ell2xyz(tmp, xyz);

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
			sate2_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev2(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	*估计基线 
	*/
	Mat B_effe(rows, 1, CV_64F); Mat B_para(rows, 1, CV_64F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		Mat R, B, tmp, xyz, effect_dir; double r;
		tmp = Mat::zeros(1, 3, CV_64F);
		tmp.at<double>(0, 0) = lat.at<double>(i, 0);
		tmp.at<double>(0, 1) = lon.at<double>(i, 0);
		tmp.at<double>(0, 2) = 0;
		ell2xyz(tmp, xyz);

		R = xyz - sate1(Range(i, i + 1), Range(0, 3));
		r = sqrt(sum(R.mul(R))[0]);
		R = R / r;
		B = sate2(Range(i, i + 1), Range(0, 3)) - sate1(Range(i, i + 1), Range(0, 3));
		B_para.at<double>(i, 0) = sum(R.mul(B))[0];//平行基线

		tmp = satev1(Range(i, i + 1), Range(0, 3));
		cross(tmp, R, effect_dir);
		r = sqrt(sum(effect_dir.mul(effect_dir))[0]);
		effect_dir = effect_dir / r;
		r = sqrt(sum(xyz.mul(xyz))[0]);
		xyz = xyz / r;
		r = sum(xyz.mul(effect_dir))[0];
		effect_dir = r < 0.0 ? -effect_dir : effect_dir;
		B_effe.at<double>(i, 0) = sum(effect_dir.mul(B))[0];//平行基线
	}
	*B_effect = sum(B_effe)[0] / (double)rows;
	*B_parallel = sum(B_para)[0] / (double)rows;
	if (sigma_B_effect)
	{
		this->std(B_effe, sigma_B_effect);
	}
	if (sigma_B_parallel)
	{
		this->std(B_para, sigma_B_parallel);
	}
	return 0;
}

int Utils::baseline_estimation(
	const Mat& stateVec1, 
	const Mat& stateVec2, 
	double lon_center,
	double lat_center, 
	int offset_row, 
	int offset_col, 
	int scene_height,
	int scene_width, 
	double time_interval,
	double time_interval2,
	double* B_effect,
	double* B_parallel
)
{
	if (stateVec1.cols != 7 ||
		stateVec1.rows < 7 ||
		stateVec2.cols != 7 ||
		stateVec2.rows < 7 ||
		stateVec1.type() != CV_64F ||
		stateVec2.type() != CV_64F ||
		B_effect == NULL ||
		B_parallel == NULL ||
		time_interval < 0.0 ||
		time_interval2 < 0.0 ||
		scene_height < 7 ||
		scene_width < 0
		)
	{
		fprintf(stderr, "baseline_estimation(): input check failed!\n");
		return -1;
	}
	int ret;
	int rows = scene_height; int cols = scene_width;
	/*
	* 轨道插值
	*/
	Mat state_vec1, state_vec2;
	stateVec1.copyTo(state_vec1);
	stateVec2.copyTo(state_vec2);
	ret = stateVec_interp(state_vec1, time_interval, state_vec1);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
	ret = stateVec_interp(state_vec2, time_interval2, state_vec2);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;

	Mat sate1_xyz, sate2_xyz, sate1_v, sate2_v;
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(1, 4)).copyTo(sate1_xyz);
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(4, 7)).copyTo(sate1_v);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(1, 4)).copyTo(sate2_xyz);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(4, 7)).copyTo(sate2_v);

	/*
	* 图像1成像点位置计算
	*/

	Mat sate1 = Mat::zeros(rows, 3, CV_64F);
	Mat sate2 = Mat::zeros(rows, 3, CV_64F);
	Mat satev1 = Mat::zeros(rows, 3, CV_64F);
	Mat satev2 = Mat::zeros(rows, 3, CV_64F);
	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat_center;
		tmp.at<double>(0, 1) = lon_center;
		tmp.at<double>(0, 2) = 0;
		ell2xyz(tmp, xyz);

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
			sate1_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev1(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	* 图像2成像点位置计算
	*/


	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat_center;
		tmp.at<double>(0, 1) = lon_center;
		tmp.at<double>(0, 2) = 0;
		ell2xyz(tmp, xyz);

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
			sate2_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev2(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	*估计基线
	*/
	Mat R, B, tmp, xyz, effect_dir; double r;
	tmp = Mat::zeros(1, 3, CV_64F);
	tmp.at<double>(0, 0) = lat_center;
	tmp.at<double>(0, 1) = lon_center;
	tmp.at<double>(0, 2) = 0;
	ell2xyz(tmp, xyz);

	R = xyz - sate1(Range(0, 1), Range(0, 3));
	r = sqrt(sum(R.mul(R))[0]);
	R = R / r;
	B = sate2(Range(0, 1), Range(0, 3)) - sate1(Range(0, 1), Range(0, 3));
	*B_parallel = sum(R.mul(B))[0];//平行基线

	tmp = satev1(Range(0, 1), Range(0, 3));
	cross(tmp, R, effect_dir);
	r = sqrt(sum(effect_dir.mul(effect_dir))[0]);
	effect_dir = effect_dir / r;
	r = sqrt(sum(xyz.mul(xyz))[0]);
	xyz = xyz / r;
	r = sum(xyz.mul(effect_dir))[0];
	effect_dir = r < 0.0 ? -effect_dir : effect_dir;
	*B_effect = sum(effect_dir.mul(B))[0];//平行基线


	return 0;
}

int Utils::baseline_estimation(
	const Mat& stateVec1,
	const Mat& stateVec2,
	double lon_center,
	double lat_center,
	double dem_center,
	int offset_row,
	int offset_col,
	int scene_height, 
	int scene_width, 
	double time_interval,
	double time_interval2,
	double* B_effect, 
	double* B_parallel
)
{
	if (stateVec1.cols != 7 ||
		stateVec1.rows < 7 ||
		stateVec2.cols != 7 ||
		stateVec2.rows < 7 ||
		stateVec1.type() != CV_64F ||
		stateVec2.type() != CV_64F ||
		B_effect == NULL ||
		B_parallel == NULL ||
		time_interval < 0.0 ||
		time_interval2 < 0.0 ||
		scene_height < 7 ||
		scene_width < 0
		)
	{
		fprintf(stderr, "baseline_estimation(): input check failed!\n");
		return -1;
	}
	int ret;
	int rows = scene_height; int cols = scene_width;
	/*
	* 轨道插值
	*/
	Mat state_vec1, state_vec2;
	stateVec1.copyTo(state_vec1);
	stateVec2.copyTo(state_vec2);
	ret = stateVec_interp(state_vec1, time_interval, state_vec1);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;
	ret = stateVec_interp(state_vec2, time_interval2, state_vec2);
	if (return_check(ret, "stateVec_interp()", error_head)) return -1;

	Mat sate1_xyz, sate2_xyz, sate1_v, sate2_v;
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(1, 4)).copyTo(sate1_xyz);
	state_vec1(cv::Range(0, state_vec1.rows), cv::Range(4, 7)).copyTo(sate1_v);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(1, 4)).copyTo(sate2_xyz);
	state_vec2(cv::Range(0, state_vec2.rows), cv::Range(4, 7)).copyTo(sate2_v);

	/*
	* 图像1成像点位置计算
	*/

	Mat sate1 = Mat::zeros(rows, 3, CV_64F);
	Mat sate2 = Mat::zeros(rows, 3, CV_64F);
	Mat satev1 = Mat::zeros(rows, 3, CV_64F);
	Mat satev2 = Mat::zeros(rows, 3, CV_64F);
	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat_center;
		tmp.at<double>(0, 1) = lon_center;
		tmp.at<double>(0, 2) = dem_center;
		ell2xyz(tmp, xyz);

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
			sate1_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev1(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	* 图像2成像点位置计算
	*/


	for (int i = 0; i < 1; i++)
	{
		Mat tmp(1, 3, CV_64F); Mat xyz;
		tmp.at<double>(0, 0) = lat_center;
		tmp.at<double>(0, 1) = lon_center;
		tmp.at<double>(0, 2) = 0;
		ell2xyz(tmp, xyz);

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
			sate2_v(Range(orbit_idx, orbit_idx + 1), Range(0, 3)).copyTo(satev2(Range(j, j + 1), Range(0, 3)));
		}
	}

	/*
	*估计基线
	*/
	Mat R, B, tmp, xyz, effect_dir; double r;
	tmp = Mat::zeros(1, 3, CV_64F);
	tmp.at<double>(0, 0) = lat_center;
	tmp.at<double>(0, 1) = lon_center;
	tmp.at<double>(0, 2) = 0;
	ell2xyz(tmp, xyz);

	R = xyz - sate1(Range(0, 1), Range(0, 3));
	r = sqrt(sum(R.mul(R))[0]);
	R = R / r;
	B = sate2(Range(0, 1), Range(0, 3)) - sate1(Range(0, 1), Range(0, 3));
	*B_parallel = sum(R.mul(B))[0];//平行基线

	tmp = satev1(Range(0, 1), Range(0, 3));
	cross(tmp, R, effect_dir);
	r = sqrt(sum(effect_dir.mul(effect_dir))[0]);
	effect_dir = effect_dir / r;
	r = sqrt(sum(xyz.mul(xyz))[0]);
	xyz = xyz / r;
	r = sum(xyz.mul(effect_dir))[0];
	effect_dir = r < 0.0 ? -effect_dir : effect_dir;
	*B_effect = sum(effect_dir.mul(B))[0];//平行基线
	return 0;
}

int Utils::spatialTemporalBaselineEstimation(
	vector<string>& SLCH5Files,
	int reference,
	Mat& temporal,
	Mat& spatial
)
{
	if (SLCH5Files.size() < 2 ||
		reference < 1 || reference > SLCH5Files.size()
		)
	{
		fprintf(stderr, "spatialTemporalBaselineEstimation(): input check failed!\n");
		return -1;
	}
	Utils util;
	int ret, offset_row, offset_col, num_images, sceneHeight, sceneWidth;
	double prf1, prf2, acquisitionTime1, acquisitionTime2, B_temporal, B_spatial_para, B_spatial_effect;
	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat;
	Mat lon_coef, lat_coef, statevec1, statevec2;
	string start;
	num_images = static_cast<int>(SLCH5Files.size());
	temporal.create(1, num_images, CV_64F); spatial.create(1, num_images, CV_64F);
	temporal.at<double>(0, reference - 1) = 0; spatial.at<double>(0, reference - 1) = 0;
	ret = Hdf5IO::readArray(SLCH5Files[reference - 1].c_str(), "lon_coefficient", lon_coef);
	ret = Hdf5IO::readArray(SLCH5Files[reference - 1].c_str(), "lat_coefficient", lat_coef);
	ret = Hdf5IO::readArray(SLCH5Files[reference - 1].c_str(), "state_vec", statevec1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "prf", &prf1);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(SLCH5Files[reference - 1].c_str(), "acquisition_start_time", start);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(start.c_str(), &acquisitionTime1);
	ret = Hdf5IO::readInt(SLCH5Files[reference - 1].c_str(), "offset_row", &offset_row);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(SLCH5Files[reference - 1].c_str(), "offset_col", &offset_col);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(SLCH5Files[reference - 1].c_str(), "range_len", &sceneWidth);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(SLCH5Files[reference - 1].c_str(), "azimuth_len", &sceneHeight);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	int ret2 = 0;
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "topLeftLon", &topleft_lon);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "topLeftLat", &topleft_lat);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "topRightLon", &topright_lon);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "topRightLat", &topright_lat);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "bottomLeftLon", &bottomleft_lon);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "bottomLeftLat", &bottomleft_lat);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "bottomRightLon", &bottomright_lon);
	ret2 += Hdf5IO::readDouble(SLCH5Files[reference - 1].c_str(), "bottomRightLat", &bottomright_lat);
	double lon_center, lat_center;
	if (ret2 == 0)
	{
		lon_center = (topleft_lon + topright_lon + bottomleft_lon + bottomright_lon) / 4.0;
		lat_center = (topleft_lat + topright_lat + bottomleft_lat + bottomright_lat) / 4.0;
	}

	for (int i = 0; i < num_images; i++)
	{
		if (i == reference - 1) continue;
		ret = Hdf5IO::readArray(SLCH5Files[i].c_str(), "state_vec", statevec2);
		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
		ret = Hdf5IO::readString(SLCH5Files[i].c_str(), "acquisition_start_time", start);
		if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
		ret = utc_to_gps(start.c_str(), &acquisitionTime2);
		ret = Hdf5IO::readDouble(SLCH5Files[i].c_str(), "prf", &prf2);
		if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
		if (ret2 == 0)
		{
			util.baseline_estimation(statevec1, statevec2, lon_center, lat_center,
				offset_row, offset_col, sceneHeight, sceneWidth, 1.0 / prf1, 1.0 / prf2, &B_spatial_effect, &B_spatial_para);
			if (return_check(ret, "baseline_estimation()", error_head)) return -1;
		}
		else
		{
			ret = util.baseline_estimation(statevec1, statevec2, lon_coef, lat_coef, offset_row, offset_col,
				sceneHeight, sceneWidth, 1.0 / prf1, 1.0 / prf2, &B_spatial_effect, &B_spatial_para);
			if (return_check(ret, "baseline_estimation()", error_head)) return -1;
		}
		B_temporal = (acquisitionTime2 - acquisitionTime1) / 60 / 60 / 24;
		temporal.at<double>(0, i) = B_temporal;
		spatial.at<double>(0, i) = B_spatial_effect;
	}
	return 0;
}

