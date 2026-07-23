#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\Hdf5IO.h"

#include <cmath>
#include <cstdio>
#include <memory>

using namespace cv;
int Utils::get_AOI_from_h5SLC(const char* h5_file, double lon_topleft, double lat_topleft, double lon_bottomright, double lat_bottomright, ComplexMat& slc, int* offset_row, int* offset_col)
{
	if (h5_file == NULL ||
		fabs(lon_topleft) > 180.0 ||
		fabs(lon_bottomright) > 180.0 ||
		fabs(lat_topleft) > 90.0 ||
		fabs(lat_bottomright) > 90.0)
	{
		fprintf(stderr, "get_AOI_from_h5SLC(): input check failed!\n");
		return -1;
	}
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(h5_file), Hdf5IO::closeReadSession);
	if (!session) return -1;
	int ret, row_start, row_end, col_start, col_end;double lon_start, lat_start, lon_end, lat_end;
	Mat row_coef, col_coef, lon_coef, lat_coef;

	//获取经纬度范围
	int rows, cols;
	Mat tmp;
	ret = Hdf5IO::readArray(session.get(), "azimuth_len", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	rows = tmp.at<int>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "range_len", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	cols = tmp.at<int>(0, 0);

	ret = Hdf5IO::readArray(session.get(), "lon_coefficient", lon_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(session.get(), "lat_coefficient", lat_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	Mat tmp_row = Mat::zeros(1, 1, CV_64F); tmp_row.at<double>(0, 0) = 1;
	Mat tmp_col = Mat::zeros(1, 1, CV_64F); tmp_col.at<double>(0, 0) = 1;
	Mat tmp_out;
	ret = coord_conversion(lon_coef, tmp_row, tmp_col, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	lon_start = (tmp_out.at<double>(0, 0));
	ret = coord_conversion(lat_coef, tmp_row, tmp_col, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	lat_start = (tmp_out.at<double>(0, 0));
	tmp_row.at<double>(0, 0) = rows; tmp_col.at<double>(0, 0) = cols;
	ret = coord_conversion(lon_coef, tmp_row, tmp_col, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	lon_end = (tmp_out.at<double>(0, 0));
	ret = coord_conversion(lat_coef, tmp_row, tmp_col, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	lat_end = (tmp_out.at<double>(0, 0));

	double x;
	if (lon_start > lon_end)
	{
		x = lon_start;
		lon_start = lon_end;
		lon_end = x;
	}
	if (lat_start > lat_end)
	{
		x = lat_start;
		lat_start = lat_end;
		lat_end = x;
	}
	if (lon_topleft > lon_bottomright)
	{
		x = lon_topleft;
		lon_topleft = lon_bottomright;
		lon_bottomright = x;
	}
	if (lat_topleft < lat_bottomright)
	{
		x = lat_topleft;
		lat_topleft = lat_bottomright;
		lat_bottomright = x;
	}

	lon_topleft = lon_topleft < lon_start ? lon_start : lon_topleft;
	lon_topleft = lon_topleft < lon_end ? lon_topleft : lon_start;

	lat_topleft = lat_topleft < lat_end ? lat_topleft : lat_end;
	lat_topleft = lat_topleft < lat_start ? lat_end : lat_topleft;

	lon_bottomright = lon_bottomright < lon_end ? lon_bottomright : lon_end;
	lon_bottomright = lon_bottomright < lon_start ? lon_end : lon_bottomright;

	lat_bottomright = lat_bottomright < lat_start ? lat_start : lat_bottomright;
	lat_bottomright = lat_bottomright < lat_end ? lat_bottomright : lat_start;

	ret = Hdf5IO::readArray(session.get(), "row_coefficient", row_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(session.get(), "col_coefficient", col_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	Mat tmp_lon = Mat::zeros(1, 1, CV_64F); tmp_lon.at<double>(0, 0) = lon_topleft;
	Mat tmp_lat = Mat::zeros(1, 1, CV_64F); tmp_lat.at<double>(0, 0) = lat_topleft;
	
	ret = coord_conversion(row_coef, tmp_lon, tmp_lat, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	row_start = (int)floor(tmp_out.at<double>(0, 0));
	ret = coord_conversion(col_coef, tmp_lon, tmp_lat, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	col_start = (int)floor(tmp_out.at<double>(0, 0));

	tmp_lon.at<double>(0, 0) = lon_bottomright; tmp_lat.at<double>(0, 0) = lat_bottomright;
	ret = coord_conversion(row_coef, tmp_lon, tmp_lat, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	row_end = (int)floor(tmp_out.at<double>(0, 0));
	ret = coord_conversion(col_coef, tmp_lon, tmp_lat, tmp_out);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	col_end = (int)floor(tmp_out.at<double>(0, 0));

	//读取slc数据
	
	row_start = row_start < 0 ? 0 : row_start;
	row_start = row_start > (rows - 1) ? (rows - 1) : row_start;
	row_end = row_end < 0 ? 0 : row_end;
	row_end = row_end > (rows - 1) ? (rows - 1) : row_end;
	col_start = col_start < 0 ? 0 : col_start;
	col_start = col_start > (cols - 1) ? (cols - 1) : col_start;
	col_end = col_end < 0 ? 0 : col_end;
	col_end = col_end > (cols - 1) ? (cols - 1) : col_end;

	int start_r, end_r, start_c, end_c;
	start_r = row_start < row_end ? row_start : row_end;
	end_r = row_start < row_end ? row_end : row_start;
	start_c = col_start < col_end ? col_start : col_end;
	end_c = col_start < col_end ? col_end : col_start;
	if(offset_row) *offset_row = start_r;
	if (offset_col)*offset_col = start_c;
	ret = Hdf5IO::readSubarray(session.get(), "s_re", start_r, start_c, (end_r - start_r), (end_c - start_c), slc.re);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readSubarray(session.get(), "s_im", start_r, start_c, (end_r - start_r), (end_c - start_c), slc.im);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	return 0;
}

int Utils::get_AOI_from_h5slc(const char* h5_file, double lon_center, double lat_center, double width, double height, ComplexMat& slc, int* offset_row, int* offset_col)
{
	if (h5_file == NULL ||
		fabs(lon_center) > 180.0 ||
		fabs(lat_center) > 90.0 ||
		width < 0.0 ||
		height < 0.0)
	{
		fprintf(stderr, "get_AOI_from_h5slc(): input check failed!\n");
		return -1;
	}
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(h5_file), Hdf5IO::closeReadSession);
	if (!session) return -1;
	int ret, row_center, col_center, total_rows, total_cols, AOI_rows, AOI_cols, offset_r, offset_c;
	double inc_center, range_spacing, azimuth_spacing;
	Mat row_coef, col_coef, lon, lat, tmp;
	lon = Mat::zeros(1, 1, CV_64F); lon.at<double>(0, 0) = lon_center;
	lat = Mat::zeros(1, 1, CV_64F); lat.at<double>(0, 0) = lat_center;
	//读取图像行列总数
	ret = Hdf5IO::readArray(session.get(), "azimuth_len", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	total_rows = tmp.at<int>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "range_len", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	total_cols = tmp.at<int>(0, 0);
	//读取采样间隔和下视角
	ret = Hdf5IO::readArray(session.get(), "azimuth_spacing", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	azimuth_spacing = tmp.at<double>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "range_spacing", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	range_spacing = tmp.at<double>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "inc_center", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	inc_center = tmp.at<double>(0, 0);
	//确定AOI中心图像坐标
	ret = Hdf5IO::readArray(session.get(), "row_coefficient", row_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(session.get(), "col_coefficient", col_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = coord_conversion(row_coef, lon, lat, tmp);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	row_center = (int)floor(tmp.at<double>(0, 0));
	ret = coord_conversion(col_coef, lon, lat, tmp);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	col_center = (int)floor(tmp.at<double>(0, 0));
	if (row_center < 1 || row_center > total_rows - 1 || col_center < 1 || col_center > total_cols - 1)
	{
		fprintf(stderr, "get_AOI_from_h5slc(): AOI not found in %s!\n", h5_file);
		return -1;
	}

	AOI_rows = (int)floor(height / azimuth_spacing);
	AOI_cols = (int)floor(width / range_spacing * sin(inc_center / 180.0 * PI));

	AOI_rows = AOI_rows > total_rows ? total_rows : AOI_rows;
	AOI_cols = AOI_cols > total_cols ? total_cols : AOI_cols;

	offset_r = row_center - (int)AOI_rows / 2;
	offset_r = offset_r < 0 ? 0 : offset_r;
	offset_c = col_center - (int)AOI_cols / 2;
	offset_c = offset_c < 0 ? 0 : offset_c;
	AOI_rows = (total_rows - offset_r) > AOI_rows ? AOI_rows : (total_rows - offset_r);
	AOI_cols = (total_cols - offset_c) > AOI_cols ? AOI_cols : (total_cols - offset_c);
	if (offset_row) *offset_row = offset_r;
	if (offset_col)*offset_col = offset_c;
	ret = Hdf5IO::readSubarray(session.get(), "s_re", offset_r, offset_c, AOI_rows, AOI_cols, slc.re);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readSubarray(session.get(), "s_im", offset_r, offset_c, AOI_rows, AOI_cols, slc.im);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	return 0;
}

int Utils::get_AOI_size(const char* h5_file, double lon_center, double lat_center, double width, double height, int* rows, int* cols, int* offset_row, int* offset_col)
{
	if (h5_file == NULL || rows == NULL || cols == NULL||
		fabs(lon_center) > 180.0 ||
		fabs(lat_center) > 180.0 ||
		width < 0.0 ||
		height < 0.0)
	{
		fprintf(stderr, "get_AOI_from_h5slc(): input check failed!\n");
		return -1;
	}
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(h5_file), Hdf5IO::closeReadSession);
	if (!session) return -1;
	int ret, row_center, col_center, total_rows, total_cols, AOI_rows, AOI_cols, offset_r, offset_c;
	double inc_center, range_spacing, azimuth_spacing;
	Mat row_coef, col_coef, lon, lat, tmp;
	lon = Mat::zeros(1, 1, CV_64F); lon.at<double>(0, 0) = lon_center;
	lat = Mat::zeros(1, 1, CV_64F); lat.at<double>(0, 0) = lat_center;
	//读取图像行列总数
	ret = Hdf5IO::readArray(session.get(), "azimuth_len", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	total_rows = tmp.at<int>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "range_len", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	total_cols = tmp.at<int>(0, 0);
	//读取采样间隔和下视角
	ret = Hdf5IO::readArray(session.get(), "azimuth_spacing", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	azimuth_spacing = tmp.at<double>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "range_spacing", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	range_spacing = tmp.at<double>(0, 0);
	ret = Hdf5IO::readArray(session.get(), "inc_center", tmp);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	inc_center = tmp.at<double>(0, 0);
	//确定AOI中心图像坐标
	ret = Hdf5IO::readArray(session.get(), "row_coefficient", row_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(session.get(), "col_coefficient", col_coef);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = coord_conversion(row_coef, lon, lat, tmp);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	row_center = (int)floor(tmp.at<double>(0, 0));
	ret = coord_conversion(col_coef, lon, lat, tmp);
	if (return_check(ret, "coord_conversion()", error_head)) return -1;
	col_center = (int)floor(tmp.at<double>(0, 0));
	if (row_center < 1 || row_center > total_rows - 1 || col_center < 1 || col_center > total_cols - 1)
	{
		fprintf(stderr, "get_AOI_from_h5slc(): AOI not found in %s!\n", h5_file);
		return -1;
	}

	AOI_rows = (int)floor(height / azimuth_spacing);
	AOI_cols = (int)floor(width / range_spacing * sin(inc_center / 180.0 * PI));

	AOI_rows = AOI_rows > total_rows ? total_rows : AOI_rows;
	AOI_cols = AOI_cols > total_cols ? total_cols : AOI_cols;

	offset_r = row_center - (int)AOI_rows / 2;
	offset_r = offset_r < 0 ? 0 : offset_r;
	offset_c = col_center - (int)AOI_cols / 2;
	offset_c = offset_c < 0 ? 0 : offset_c;
	AOI_rows = (total_rows - offset_r) > AOI_rows ? AOI_rows : (total_rows - offset_r);
	AOI_cols = (total_cols - offset_c) > AOI_cols ? AOI_cols : (total_cols - offset_c);
	*rows = AOI_rows;
	*cols = AOI_cols;
	if (offset_row) *offset_row = offset_r;
	if (offset_col)*offset_col = offset_c;
	return 0;
}

