#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\Hdf5IO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cv;
using namespace std;

extern int utc_to_gps(const char* utc_time, double* gps_time);

int Utils::read_slc_from_h5io(const char* filename, ComplexMat& slc)
{
	if (!filename) return -1;
	if (Hdf5IO::readArray(filename, "s_re", slc.re) != 0) return -1;
	if (Hdf5IO::readArray(filename, "s_im", slc.im) != 0) return -1;
	return slc.re.size() == slc.im.size() && slc.re.type() == slc.im.type() ? 0 : -1;
}

int Utils::write_slc_to_h5io(const char* filename, const ComplexMat& slc)
{
	if (!filename || slc.isEmpty()) return -1;
	if (Hdf5IO::writeArray(filename, "s_re", slc.re) != 0) return -1;
	return Hdf5IO::writeArray(filename, "s_im", slc.im);
}

int Utils::S1_subswath_merge(
	const char* IW1_h5file,
	const char* IW2_h5file,
	const char* IW3_h5file, 
	const char* merged_phase_h5file,
	NewtonProgressCallback cb
)
{
	if (cb && !cb(0, "Preparing Sentinel-1 subswath merge...")) return -2;
	if (!IW1_h5file || !IW2_h5file || !IW3_h5file || !merged_phase_h5file)
	{
		fprintf(stderr, "S1_subswath_merge(): input check failed\n");
		return -1;
	}
	double start1, start2, start3, end1, end2, end3, first_pixel1, first_pixel2, first_pixel3,
		range_spacing, prf;
	int mul_az, mul_rg, mul_az1, mul_rg1, rows1, rows2, rows3, cols1, cols2, cols3;
	string start_time, end_time;
	int ret;
	ret = Hdf5IO::readDouble(IW1_h5file, "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(IW1_h5file, "slant_range_first_pixel", &first_pixel1);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(IW2_h5file, "slant_range_first_pixel", &first_pixel2);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(IW3_h5file, "slant_range_first_pixel", &first_pixel3);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	if (first_pixel1 >= first_pixel2 || first_pixel2 >= first_pixel3)
	{
		fprintf(stderr, "S1_subswath_merge(): please rearrange input swath order!\n");
		return -1;
	}
	ret = Hdf5IO::readDouble(IW3_h5file, "range_spacing", &range_spacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;

	ret = Hdf5IO::readInt(IW1_h5file, "multilook_az", &mul_az);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW1_h5file, "multilook_rg", &mul_rg);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	ret = Hdf5IO::readInt(IW2_h5file, "multilook_az", &mul_az1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW2_h5file, "multilook_rg", &mul_rg1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	if (mul_az != mul_az1 || mul_rg != mul_rg1)
	{
		fprintf(stderr, "S1_subswath_merge(): multilook times disagree!\n");
		return -1;
	}
	ret = Hdf5IO::readInt(IW3_h5file, "multilook_az", &mul_az1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW3_h5file, "multilook_rg", &mul_rg1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	if (mul_az != mul_az1 || mul_rg != mul_rg1)
	{
		fprintf(stderr, "S1_subswath_merge(): multilook times disagree!\n");
		return -1;
	}
	
	ret = Hdf5IO::readInt(IW1_h5file, "range_len", &cols1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW1_h5file, "azimuth_len", &rows1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	ret = Hdf5IO::readInt(IW2_h5file, "range_len", &cols2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW2_h5file, "azimuth_len", &rows2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	ret = Hdf5IO::readInt(IW3_h5file, "range_len", &cols3);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW3_h5file, "azimuth_len", &rows3);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;



	ret = Hdf5IO::readString(IW1_h5file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW1_h5file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	utc_to_gps(start_time.c_str(), &start1);
	utc_to_gps(end_time.c_str(), &end1);

	ret = Hdf5IO::readString(IW2_h5file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW2_h5file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	utc_to_gps(start_time.c_str(), &start2);
	utc_to_gps(end_time.c_str(), &end2);

	ret = Hdf5IO::readString(IW3_h5file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW3_h5file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	utc_to_gps(start_time.c_str(), &start3);
	utc_to_gps(end_time.c_str(), &end3);

	//IW1和IW2拼接

	int x = static_cast<int>((double(cols1 * mul_rg) - round((first_pixel2 - first_pixel1) / range_spacing)) / 2.0);
	int tmp = x + static_cast<int>(round((first_pixel2 - first_pixel1) / range_spacing));
	int col_end_last = tmp / mul_rg;
	int col_start_next = static_cast<int>((col_end_last * mul_rg - round((first_pixel2 - first_pixel1) / range_spacing)) / mul_rg);
	int total_cols = col_end_last + (cols2 - col_start_next);
	int row_offset = static_cast<int>((start1 - start2) * prf / (double)mul_az);
	int total_rows = static_cast<int>((((end1 > end2 ? end1 : end2) - (start1 < start2 ? start1 : start2))*prf + 1) / (double)mul_az + 1);
	Mat phase_tmp(total_rows, total_cols, CV_64F); phase_tmp = 0.0;
	Mat lon_tmp(total_rows, total_cols, CV_32F); lon_tmp = 360.0;
	Mat lat_tmp(total_rows, total_cols, CV_32F); lat_tmp = 360.0;
	Mat phase1, phase2, mapped_lon1, mapped_lon2, mapped_lat1, mapped_lat2;
	ret = Hdf5IO::readArray(IW1_h5file, "phase", phase1);
	if (cb && !cb(30, "Reading Sentinel-1 subswaths...")) return -2;
	ret = Hdf5IO::readArray(IW2_h5file, "phase", phase2);
	ret = Hdf5IO::readArray(IW1_h5file, "mapped_lon", mapped_lon1);
	ret += Hdf5IO::readArray(IW1_h5file, "mapped_lat", mapped_lat1);
	ret += Hdf5IO::readArray(IW2_h5file, "mapped_lon", mapped_lon2);
	ret += Hdf5IO::readArray(IW2_h5file, "mapped_lat", mapped_lat2);
	int temp_ret = ret;
	if (row_offset < 0)
	{
		if (temp_ret == 0)
		{
			mapped_lon1(cv::Range(0, mapped_lon1.rows), cv::Range(0, col_end_last)).copyTo
			(lon_tmp(cv::Range(0, mapped_lon1.rows), cv::Range(0, col_end_last)));
			mapped_lon2(cv::Range(0, mapped_lon2.rows), cv::Range(col_start_next, cols2)).copyTo
			(lon_tmp(cv::Range(-row_offset, mapped_lon2.rows - row_offset), cv::Range(col_end_last, total_cols)));

			mapped_lat1(cv::Range(0, mapped_lat1.rows), cv::Range(0, col_end_last)).copyTo
			(lat_tmp(cv::Range(0, mapped_lat1.rows), cv::Range(0, col_end_last)));
			mapped_lat2(cv::Range(0, mapped_lat2.rows), cv::Range(col_start_next, cols2)).copyTo
			(lat_tmp(cv::Range(-row_offset, mapped_lat2.rows - row_offset), cv::Range(col_end_last, total_cols)));
		}
		phase1(cv::Range(0, phase1.rows), cv::Range(0, col_end_last)).copyTo
		(phase_tmp(cv::Range(0, phase1.rows), cv::Range(0, col_end_last)));
		phase2(cv::Range(0, phase2.rows), cv::Range(col_start_next, cols2)).copyTo
		(phase_tmp(cv::Range(-row_offset, phase2.rows - row_offset), cv::Range(col_end_last, total_cols)));
	}
	else
	{
		if (temp_ret == 0)
		{
			mapped_lon1(cv::Range(0, mapped_lon1.rows), cv::Range(0, col_end_last)).copyTo
			(lon_tmp(cv::Range(row_offset, mapped_lon1.rows + row_offset), cv::Range(0, col_end_last)));
			mapped_lon2(cv::Range(0, mapped_lon2.rows), cv::Range(col_start_next, cols2)).copyTo
			(lon_tmp(cv::Range(0, mapped_lon2.rows), cv::Range(col_end_last, total_cols)));

			mapped_lat1(cv::Range(0, mapped_lat1.rows), cv::Range(0, col_end_last)).copyTo
			(lat_tmp(cv::Range(row_offset, mapped_lat1.rows + row_offset), cv::Range(0, col_end_last)));
			mapped_lat2(cv::Range(0, mapped_lat2.rows), cv::Range(col_start_next, cols2)).copyTo
			(lat_tmp(cv::Range(0, mapped_lat2.rows), cv::Range(col_end_last, total_cols)));
		}
		phase1(cv::Range(0, phase1.rows), cv::Range(0, col_end_last)).copyTo
		(phase_tmp(cv::Range(row_offset, phase1.rows + row_offset), cv::Range(0, col_end_last)));
		phase2(cv::Range(0, phase2.rows), cv::Range(col_start_next, cols2)).copyTo
		(phase_tmp(cv::Range(0, phase2.rows), cv::Range(col_end_last, total_cols)));
	}

	//拼接IW3

	x = static_cast<int>((double(total_cols * mul_rg) - round((first_pixel3 - first_pixel1) / range_spacing)) / 2.0);
	tmp = x + static_cast<int>(round((first_pixel3 - first_pixel1) / range_spacing));
	col_end_last = tmp / mul_rg;
	col_start_next = static_cast<int>((col_end_last * mul_rg - round((first_pixel3 - first_pixel1) / range_spacing)) / mul_rg);
	total_cols = col_end_last + (cols3 - col_start_next);
	start1 = start1 < start2 ? start1 : start2;
	end1 = end1 > end2 ? end1 : end2;
	row_offset = static_cast<int>((start1 - start3) * prf / (double)mul_az);
	total_rows = static_cast<int>((((end1 > end3 ? end1 : end3) - (start1 < start3 ? start1 : start3)) * prf + 1) / (double)mul_az + 1);

	phase1.create(total_rows, total_cols, CV_64F); phase1 = 0.0;
	mapped_lat2.create(total_rows, total_cols, CV_32F); mapped_lat2 = 360.0;
	mapped_lon2.create(total_rows, total_cols, CV_32F); mapped_lon2 = 360.0;
	ret = Hdf5IO::readArray(IW3_h5file, "phase", phase2);
	if (cb && !cb(65, "Merging Sentinel-1 subswaths...")) return -2;
	ret = Hdf5IO::readArray(IW3_h5file, "mapped_lon", mapped_lon1);
	ret += Hdf5IO::readArray(IW3_h5file, "mapped_lat", mapped_lat1);
	temp_ret += ret;
	if (row_offset < 0)
	{
		if (temp_ret == 0)
		{
			lon_tmp(cv::Range(0, lon_tmp.rows), cv::Range(0, col_end_last)).copyTo
			(mapped_lon2(cv::Range(0, lon_tmp.rows), cv::Range(0, col_end_last)));
			mapped_lon1(cv::Range(0, mapped_lon1.rows), cv::Range(col_start_next, cols3)).copyTo
			(mapped_lon2(cv::Range(-row_offset, mapped_lon1.rows - row_offset), cv::Range(col_end_last, total_cols)));

			lat_tmp(cv::Range(0, lat_tmp.rows), cv::Range(0, col_end_last)).copyTo
			(mapped_lat2(cv::Range(0, lat_tmp.rows), cv::Range(0, col_end_last)));
			mapped_lat1(cv::Range(0, mapped_lat1.rows), cv::Range(col_start_next, cols3)).copyTo
			(mapped_lat2(cv::Range(-row_offset, mapped_lat1.rows - row_offset), cv::Range(col_end_last, total_cols)));
		}
		phase_tmp(cv::Range(0, phase_tmp.rows), cv::Range(0, col_end_last)).copyTo
		(phase1(cv::Range(0, phase_tmp.rows), cv::Range(0, col_end_last)));
		phase2(cv::Range(0, phase2.rows), cv::Range(col_start_next, cols3)).copyTo
		(phase1(cv::Range(-row_offset, phase2.rows - row_offset), cv::Range(col_end_last, total_cols)));
	}
	else
	{
		if (temp_ret == 0)
		{
			lon_tmp(cv::Range(0, lon_tmp.rows), cv::Range(0, col_end_last)).copyTo
			(mapped_lon2(cv::Range(row_offset, lon_tmp.rows + row_offset), cv::Range(0, col_end_last)));
			mapped_lon1(cv::Range(0, mapped_lon1.rows), cv::Range(col_start_next, cols3)).copyTo
			(mapped_lon2(cv::Range(0, mapped_lon1.rows), cv::Range(col_end_last, total_cols)));

			lat_tmp(cv::Range(0, lat_tmp.rows), cv::Range(0, col_end_last)).copyTo
			(mapped_lat2(cv::Range(row_offset, lat_tmp.rows + row_offset), cv::Range(0, col_end_last)));
			mapped_lat1(cv::Range(0, mapped_lat1.rows), cv::Range(col_start_next, cols3)).copyTo
			(mapped_lat2(cv::Range(0, mapped_lat1.rows), cv::Range(col_end_last, total_cols)));
		}
		phase_tmp(cv::Range(0, phase_tmp.rows), cv::Range(0, col_end_last)).copyTo
		(phase1(cv::Range(row_offset, phase_tmp.rows + row_offset), cv::Range(0, col_end_last)));
		phase2(cv::Range(0, phase2.rows), cv::Range(col_start_next, cols3)).copyTo
		(phase1(cv::Range(0, phase2.rows), cv::Range(col_end_last, total_cols)));
	}
	ret = Hdf5IO::createFile(merged_phase_h5file);
	if (cb && !cb(85, "Writing merged Sentinel-1 subswath...")) return -2;
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	ret = Hdf5IO::writeArray(merged_phase_h5file, "phase", phase1);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	if (temp_ret == 0)
	{
		ret = Hdf5IO::writeArray(merged_phase_h5file, "mapped_lon", mapped_lon2);
		ret = Hdf5IO::writeArray(merged_phase_h5file, "mapped_lat", mapped_lat2);
	}

	Hdf5IO::writeInt(merged_phase_h5file, "multilook_az", mul_az);
	Hdf5IO::writeInt(merged_phase_h5file, "multilook_rg", mul_rg);
	Hdf5IO::writeInt(merged_phase_h5file, "azimuth_len", phase1.rows);
	Hdf5IO::writeInt(merged_phase_h5file, "range_len", phase1.cols);

	//写入三个子带的source_1和source_2

	string source_1, source_2;
	ret = Hdf5IO::readString(IW1_h5file, "source_1", source_1);
	ret = Hdf5IO::writeString(merged_phase_h5file, "source_1_IW1", source_1.c_str());
	ret = Hdf5IO::readString(IW1_h5file, "source_2", source_2);
	ret = Hdf5IO::writeString(merged_phase_h5file, "source_2_IW1", source_2.c_str());
	ret = Hdf5IO::readString(IW2_h5file, "source_1", source_1);
	ret = Hdf5IO::writeString(merged_phase_h5file, "source_1_IW2", source_1.c_str());
	ret = Hdf5IO::readString(IW2_h5file, "source_2", source_2);
	ret = Hdf5IO::writeString(merged_phase_h5file, "source_2_IW2", source_2.c_str());
	ret = Hdf5IO::readString(IW3_h5file, "source_1", source_1);
	ret = Hdf5IO::writeString(merged_phase_h5file, "source_1_IW3", source_1.c_str());
	ret = Hdf5IO::readString(IW3_h5file, "source_1", source_1);
	ret = Hdf5IO::writeString(merged_phase_h5file, "source_2_IW3", source_2.c_str());

	if (cb && !cb(100, "Sentinel-1 subswath merge complete.")) return -2;
	return 0;
}

int Utils::S1_subswath_merge_slc(const char* IW1_h5file, const char* IW2_h5file, const char* IW3_h5file, const char* merged_phase_h5file, NewtonProgressCallback cb)
{
	if (cb && !cb(0, "Preparing Sentinel-1 SLC subswath merge...")) return -2;
	if (!IW1_h5file || !IW2_h5file || !IW3_h5file || !merged_phase_h5file)
	{
		fprintf(stderr, "S1_subswath_merge_slc(): input check failed\n");
		return -1;
	}
	double start1, start2, start3, end1, end2, end3, first_pixel1, first_pixel2, first_pixel3,
		range_spacing, prf;
	int rows1, rows2, rows3, cols1, cols2, cols3;
	// removed unused: mul_az, mul_rg, mul_az1, mul_rg1 (read from H5 but SLC merge uses raw coordinates)
	string sensor1, sensor2, sensor3;
	string start_time, end_time;
	int ret;
	ret = Hdf5IO::readString(IW1_h5file, "sensor", sensor1);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW2_h5file, "sensor", sensor2);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW3_h5file, "sensor", sensor3);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	if (sensor1 != "sentinel" || sensor2 != "sentinel" || sensor3 != "sentinel")
	{
		fprintf(stderr, "S1_subswath_merge_slc(): not sentinel1 data!\n");
		return -1;
	}

	ret = Hdf5IO::readDouble(IW1_h5file, "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(IW1_h5file, "slant_range_first_pixel", &first_pixel1);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(IW2_h5file, "slant_range_first_pixel", &first_pixel2);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(IW3_h5file, "slant_range_first_pixel", &first_pixel3);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	if (first_pixel1 >= first_pixel2 || first_pixel2 >= first_pixel3)
	{
		fprintf(stderr, "S1_subswath_merge_slc(): please rearrange input swath order!\n");
		return -1;
	}
	ret = Hdf5IO::readDouble(IW3_h5file, "range_spacing", &range_spacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;


	ret = Hdf5IO::readInt(IW1_h5file, "range_len", &cols1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW1_h5file, "azimuth_len", &rows1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	ret = Hdf5IO::readInt(IW2_h5file, "range_len", &cols2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW2_h5file, "azimuth_len", &rows2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	ret = Hdf5IO::readInt(IW3_h5file, "range_len", &cols3);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(IW3_h5file, "azimuth_len", &rows3);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;



	ret = Hdf5IO::readString(IW1_h5file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW1_h5file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	utc_to_gps(start_time.c_str(), &start1);
	utc_to_gps(end_time.c_str(), &end1);

	ret = Hdf5IO::readString(IW2_h5file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW2_h5file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	utc_to_gps(start_time.c_str(), &start2);
	utc_to_gps(end_time.c_str(), &end2);

	ret = Hdf5IO::readString(IW3_h5file, "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(IW3_h5file, "acquisition_stop_time", end_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	utc_to_gps(start_time.c_str(), &start3);
	utc_to_gps(end_time.c_str(), &end3);

	//IW1和IW2拼接

	int x = static_cast<int>((double(cols1) - round((first_pixel2 - first_pixel1) / range_spacing)) / 2.0);
	int col_end_last = x + static_cast<int>(round((first_pixel2 - first_pixel1) / range_spacing));
	int col_start_next = x;
	int total_cols = col_end_last + (cols2 - col_start_next);
	int row_offset = static_cast<int>((start1 - start2) * prf);
	int total_rows = static_cast<int>((((end1 > end2 ? end1 : end2) - (start1 < start2 ? start1 : start2)) * prf + 1) + 1);
	ComplexMat slc1, slc2, slc3, slc_tmp, slc_tmp2;
	ret = read_slc_from_h5io(IW1_h5file, slc1);
	ret = read_slc_from_h5io(IW2_h5file, slc2);
	if (cb && !cb(30, "Reading Sentinel-1 SLC subswaths...")) return -2;
	if (slc1.type() != slc2.type())
	{
		fprintf(stderr, "S1_subswath_merge_slc(): data type mismatch!\n");
		return -1;
	}
	slc_tmp.re.create(total_rows, total_cols, slc1.type()); slc_tmp.im.create(total_rows, total_cols, slc1.type());
	slc_tmp.re = 0; slc_tmp.im = 0;
	if (row_offset < 0)
	{
		slc1.re(cv::Range(0, slc1.re.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp.re(cv::Range(0, slc1.re.rows), cv::Range(0, col_end_last)));
		slc1.im(cv::Range(0, slc1.im.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp.im(cv::Range(0, slc1.im.rows), cv::Range(0, col_end_last)));

		slc2.re(cv::Range(0, slc2.re.rows), cv::Range(col_start_next, cols2)).copyTo
		(slc_tmp.re(cv::Range(-row_offset, slc2.re.rows - row_offset), cv::Range(col_end_last, total_cols)));
		slc2.im(cv::Range(0, slc2.im.rows), cv::Range(col_start_next, cols2)).copyTo
		(slc_tmp.im(cv::Range(-row_offset, slc2.im.rows - row_offset), cv::Range(col_end_last, total_cols)));
	}
	else
	{
		slc1.re(cv::Range(0, slc1.re.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp.re(cv::Range(row_offset, slc1.re.rows + row_offset), cv::Range(0, col_end_last)));
		slc1.im(cv::Range(0, slc1.im.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp.im(cv::Range(row_offset, slc1.im.rows + row_offset), cv::Range(0, col_end_last)));

		slc2.re(cv::Range(0, slc2.re.rows), cv::Range(col_start_next, cols2)).copyTo
		(slc_tmp.re(cv::Range(0, slc2.re.rows), cv::Range(col_end_last, total_cols)));
		slc2.im(cv::Range(0, slc2.im.rows), cv::Range(col_start_next, cols2)).copyTo
		(slc_tmp.im(cv::Range(0, slc2.im.rows), cv::Range(col_end_last, total_cols)));
	}

	//拼接IW3

	x = static_cast<int>((double(total_cols) - round((first_pixel3 - first_pixel1) / range_spacing)) / 2.0);
	col_end_last = x + static_cast<int>(round((first_pixel3 - first_pixel1) / range_spacing));
	col_start_next = x;
	total_cols = col_end_last + (cols3 - col_start_next);
	double start11 = start1 < start2 ? start1 : start2;
	double end11 = end1 > end2 ? end1 : end2;
	row_offset = static_cast<int>((start11 - start3) * prf);
	total_rows = static_cast<int>((((end11 > end3 ? end11 : end3) - (start11 < start3 ? start11 : start3)) * prf + 1) + 1);
	slc_tmp2.re.create(total_rows, total_cols, slc1.type()); slc_tmp2.im.create(total_rows, total_cols, slc1.type());
	slc_tmp2.re = 0; slc_tmp2.im = 0;
	ret = read_slc_from_h5io(IW3_h5file, slc2);
	if (cb && !cb(60, "Merging Sentinel-1 SLC subswaths...")) return -2;
	if (slc1.type() != slc2.type())
	{
		fprintf(stderr, "S1_subswath_merge_slc(): data type mismatch!\n");
		return -1;
	}
	if (row_offset < 0)
	{
		slc_tmp.re(cv::Range(0, slc_tmp.re.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp2.re(cv::Range(0, slc_tmp.re.rows), cv::Range(0, col_end_last)));
		slc_tmp.im(cv::Range(0, slc_tmp.im.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp2.im(cv::Range(0, slc_tmp.im.rows), cv::Range(0, col_end_last)));

		slc2.re(cv::Range(0, slc2.re.rows), cv::Range(col_start_next, cols3)).copyTo
		(slc_tmp2.re(cv::Range(-row_offset, slc2.re.rows - row_offset), cv::Range(col_end_last, total_cols)));
		slc2.im(cv::Range(0, slc2.im.rows), cv::Range(col_start_next, cols3)).copyTo
		(slc_tmp2.im(cv::Range(-row_offset, slc2.im.rows - row_offset), cv::Range(col_end_last, total_cols)));
	}
	else
	{
		slc_tmp.re(cv::Range(0, slc_tmp.re.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp2.re(cv::Range(row_offset, slc_tmp.re.rows + row_offset), cv::Range(0, col_end_last)));
		slc_tmp.im(cv::Range(0, slc_tmp.im.rows), cv::Range(0, col_end_last)).copyTo
		(slc_tmp2.im(cv::Range(row_offset, slc_tmp.im.rows + row_offset), cv::Range(0, col_end_last)));

		slc2.re(cv::Range(0, slc2.re.rows), cv::Range(col_start_next, cols3)).copyTo
		(slc_tmp2.re(cv::Range(0, slc2.re.rows), cv::Range(col_end_last, total_cols)));
		slc2.im(cv::Range(0, slc2.im.rows), cv::Range(col_start_next, cols3)).copyTo
		(slc_tmp2.im(cv::Range(0, slc2.im.rows), cv::Range(col_end_last, total_cols)));
	}
	ret = Hdf5IO::createFile(merged_phase_h5file);
	if (cb && !cb(85, "Writing merged Sentinel-1 SLC...")) return -2;
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	ret = write_slc_to_h5io(merged_phase_h5file, slc_tmp2);
	if (return_check(ret, "write_slc_to_h5()", error_head)) return -1;


	Hdf5IO::writeInt(merged_phase_h5file, "azimuth_len", slc_tmp2.re.rows);
	Hdf5IO::writeInt(merged_phase_h5file, "range_len", slc_tmp2.re.cols);

	/////写入其它参数
	//拍摄起始时间
	if (start1 < start2 && start1 < start3)
	{
		ret = Hdf5IO::readString(IW1_h5file, "acquisition_start_time", start_time);
		Hdf5IO::writeString(merged_phase_h5file, "acquisition_start_time", start_time.c_str());
	}
	else if (start2 < start1 && start2 < start3)
	{
		ret = Hdf5IO::readString(IW2_h5file, "acquisition_start_time", start_time);
		Hdf5IO::writeString(merged_phase_h5file, "acquisition_start_time", start_time.c_str());
	}
	else
	{
		ret = Hdf5IO::readString(IW3_h5file, "acquisition_start_time", start_time);
		Hdf5IO::writeString(merged_phase_h5file, "acquisition_start_time", start_time.c_str());
	}
	//拍摄结束时间
	if (end1 > end2 && end1 > end3)
	{
		ret = Hdf5IO::readString(IW1_h5file, "acquisition_stop_time", end_time);
		Hdf5IO::writeString(merged_phase_h5file, "acquisition_stop_time", end_time.c_str());
	}
	else if (end2 > end1 && end2 > end3)
	{
		ret = Hdf5IO::readString(IW2_h5file, "acquisition_stop_time", end_time);
		Hdf5IO::writeString(merged_phase_h5file, "acquisition_stop_time", end_time.c_str());
	}
	else
	{
		ret = Hdf5IO::readString(IW3_h5file, "acquisition_stop_time", end_time);
		Hdf5IO::writeString(merged_phase_h5file, "acquisition_stop_time", end_time.c_str());
	}
	//最近斜距
	Hdf5IO::writeDouble(merged_phase_h5file, "slant_range_first_pixel", first_pixel1);

	//采样间隔
	Hdf5IO::writeDouble(merged_phase_h5file, "range_spacing", range_spacing);
	Hdf5IO::readDouble(IW3_h5file, "azimuth_spacing", &range_spacing);
	Hdf5IO::writeDouble(merged_phase_h5file, "azimuth_spacing", range_spacing);
	//中心下视角
	Hdf5IO::readDouble(IW2_h5file, "inc_center", &range_spacing);
	Hdf5IO::writeDouble(merged_phase_h5file, "inc_center", range_spacing);
	//prf
	Hdf5IO::writeDouble(merged_phase_h5file, "prf", prf);
	//载频
	Hdf5IO::readDouble(IW3_h5file, "carrier_frequency", &range_spacing);
	Hdf5IO::writeDouble(merged_phase_h5file, "carrier_frequency", range_spacing);
	//极化、swath、传感器名
	ret = Hdf5IO::readString(IW3_h5file, "polarization", end_time);
	Hdf5IO::writeString(merged_phase_h5file, "polarization", end_time.c_str());
	Hdf5IO::writeString(merged_phase_h5file, "swath", "IW123");
	//conversion.write_str_to_h5(merged_phase_h5file, "sensor", "sentinel");
	//轨道
	Mat state_vec;
	ret = Hdf5IO::readArray(IW3_h5file, "state_vec", state_vec);
	Hdf5IO::writeArray(merged_phase_h5file, "state_vec", state_vec);
	//控制点
	Mat gcps1, gcps2, gcps3, lon, lat;
	ret = Hdf5IO::readArray(IW1_h5file, "gcps", gcps1);
	ret = Hdf5IO::readArray(IW2_h5file, "gcps", gcps2);
	ret = Hdf5IO::readArray(IW3_h5file, "gcps", gcps3);
	cv::vconcat(gcps1, gcps2, gcps2);
	cv::vconcat(gcps2, gcps3, gcps3);
	gcps3(cv::Range(0, gcps3.rows), cv::Range(0, 1)).copyTo(lon);
	gcps3(cv::Range(0, gcps3.rows), cv::Range(1, 2)).copyTo(lat);

	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat, lonMin, lonMax, latMin, latMax;

	cv::minMaxLoc(lon, &lonMin, &lonMax);
	cv::minMaxLoc(lat, &latMin, &latMax);
	double extra = 5.0 / 6000;
	lonMin = lonMin - extra * 100;
	lonMax = lonMax + extra * 100;
	latMin = latMin - extra * 100;
	latMax = latMax + extra * 100;
	topleft_lon = lonMin;
	topright_lon = lonMax;
	bottomleft_lon = lonMin;
	bottomright_lon = lonMax;
	topleft_lat = latMax;
	topright_lat = latMax;
	bottomleft_lat = latMin;
	bottomright_lat = latMin;

	Hdf5IO::writeDouble(merged_phase_h5file, "topLeftLat", topleft_lat);
	Hdf5IO::writeDouble(merged_phase_h5file, "topLeftLon", topleft_lon);
	Hdf5IO::writeDouble(merged_phase_h5file, "topRightLat", topright_lat);
	Hdf5IO::writeDouble(merged_phase_h5file, "topRightLon", topright_lon);
	Hdf5IO::writeDouble(merged_phase_h5file, "bottomLeftLat", bottomleft_lat);
	Hdf5IO::writeDouble(merged_phase_h5file, "bottomLeftLon", bottomleft_lon);
	Hdf5IO::writeDouble(merged_phase_h5file, "bottomRightLat", bottomright_lat);
	Hdf5IO::writeDouble(merged_phase_h5file, "bottomRightLon", bottomright_lon);


	if (cb && !cb(100, "Sentinel-1 SLC subswath merge complete.")) return -2;
	return 0;
}

int Utils::S1_frame_merge(vector<string>& h5files, const char* merged_phase_h5, NewtonProgressCallback cb)
{
	if (cb && !cb(0, "Preparing Sentinel-1 frame merge...")) return -2;
	if (h5files.size() < 2 || !merged_phase_h5)
	{
		fprintf(stderr, "S1_frame_merge(): input check failed!\n");
		return -1;
	}
	Mat phase;
	int num_files = static_cast<int>(h5files.size());
	//根据每个拍摄frame的拍摄起始时间对文件排序（从小到大）
	int ret; string start_time; double start;
	Mat stime(1, num_files, CV_64F), order;
	for (int i = 0; i < num_files; i++)
	{
		if (cb && !cb(i * 20 / std::max(1, num_files), "Reading Sentinel-1 frame metadata...")) return -2;
		ret = Hdf5IO::readString(h5files[i].c_str(), "acquisition_start_time", start_time);
		if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
		ret = utc_to_gps(start_time.c_str(), &start);
		if (return_check(ret, "utc2gps()", error_head)) return -1;
		stime.at<double>(0, i) = start;
	}
	cv::sortIdx(stime, order, cv::SORT_EVERY_ROW + cv::SORT_ASCENDING);

	//检查多视倍数和最近斜距是否相同
	double start2, end1, first_pixel1, first_pixel2,
		range_spacing, prf;
	// removed unused: start1, start3, end2, end3, first_pixel3 (frame merge only uses start/end1 for overlap calc)
	int mul_az, mul_rg, mul_az1, mul_rg1;
	ret = Hdf5IO::readInt(h5files[0].c_str(), "multilook_az", &mul_az);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(h5files[0].c_str(), "multilook_rg", &mul_rg);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(h5files[0].c_str(), "slant_range_first_pixel", &first_pixel1);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(h5files[0].c_str(), "range_spacing", &range_spacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	for (int i = 1; i < num_files; i++)
	{
		ret = Hdf5IO::readInt(h5files[i].c_str(), "multilook_az", &mul_az1);
		if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
		ret = Hdf5IO::readInt(h5files[i].c_str(), "multilook_rg", &mul_rg1);
		if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
		if (mul_az != mul_az1 || mul_rg != mul_rg1)
		{
			fprintf(stderr, "S1_frame_merge(): multilook times disagree!\n");
			return -1;
		}
		ret = Hdf5IO::readDouble(h5files[i].c_str(), "slant_range_first_pixel", &first_pixel2);
		if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
		if (fabs(first_pixel2 - first_pixel1) > 0.1)
		{
			fprintf(stderr, "S1_frame_merge(): slant_range_first_pixel disagree!\n");
			return -1;
		}
	}

	Mat phase1;
	ret = Hdf5IO::createFile(merged_phase_h5);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(h5files[order.at<int>(0)].c_str(), "phase", phase);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readString(h5files[order.at<int>(0)].c_str(), "acquisition_stop_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(start_time.c_str(), &end1);
	if (return_check(ret, "utc2gps()", error_head)) return -1;
	ret = Hdf5IO::readString(h5files[order.at<int>(0)].c_str(), "acquisition_start_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(start_time.c_str(), &start);
	if (return_check(ret, "utc2gps()", error_head)) return -1;
	Hdf5IO::writeString(merged_phase_h5, "acquisition_start_time", start_time.c_str());
	ret = Hdf5IO::readString(h5files[order.at<int>(num_files - 1)].c_str(), "acquisition_stop_time", start_time);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	Hdf5IO::writeString(merged_phase_h5, "acquisition_stop_time", start_time.c_str());
	ret = Hdf5IO::readDouble(h5files[order.at<int>(0)].c_str(), "prf", &prf);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	for (int i = 1; i < num_files; i++)
	{
		ret = Hdf5IO::readArray(h5files[order.at<int>(i)].c_str(), "phase", phase1);
		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
		if (phase1.cols != phase.cols)
		{
			fprintf(stderr, "S1_frame_merge(): frame cols mismatch!\n");
			return -1;
		}
		ret = Hdf5IO::readString(h5files[order.at<int>(i)].c_str(), "acquisition_start_time", start_time);
		if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
		ret = utc_to_gps(start_time.c_str(), &start2);
		if (return_check(ret, "utc2gps()", error_head)) return -1;
		if (end1 < start2)
		{
			fprintf(stderr, "S1_frame_merge(): no overlap area between frames!\n");
			return -1;
		}
		int last_upper_count = static_cast<int>((((start2 - start) * prf + 1 + ((end1 - start2) * prf + 1) / 2.0) / (double)mul_az));
		int tmp = static_cast<int>(last_upper_count * mul_az - ((start2 - start) * prf + 1));
		tmp = static_cast<int>((end1 - start2) * prf + 1 - tmp);
		int next_lower_start = static_cast<int>(round((double)tmp / (double)mul_az));

		phase(cv::Range(0, last_upper_count), cv::Range(0, phase.cols)).copyTo(phase);
		phase1(cv::Range(next_lower_start, phase1.rows), cv::Range(0, phase1.cols)).copyTo(phase1);
		cv::vconcat(phase, phase1, phase);

		ret = Hdf5IO::readString(h5files[order.at<int>(i)].c_str(), "acquisition_stop_time", start_time);
		if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
		ret = utc_to_gps(start_time.c_str(), &end1);
		if (return_check(ret, "utc2gps()", error_head)) return -1;
	}
	Hdf5IO::writeArray(merged_phase_h5, "phase", phase);
	Hdf5IO::writeInt(merged_phase_h5, "multilook_az", mul_az);
	Hdf5IO::writeInt(merged_phase_h5, "multilook_rg", mul_rg);
	Hdf5IO::writeInt(merged_phase_h5, "azimuth_len", phase.rows);
	Hdf5IO::writeInt(merged_phase_h5, "range_len", phase.cols);
	Hdf5IO::writeDouble(merged_phase_h5, "prf", prf);
	Hdf5IO::writeDouble(merged_phase_h5, "range_spacing", range_spacing);
	Hdf5IO::writeDouble(merged_phase_h5, "slant_range_first_pixel", first_pixel1);
	if (cb && !cb(100, "Sentinel-1 frame merge complete.")) return -2;
	return 0;
}

int Utils::S1_frame_merge(const char* frame1_h5, const char* frame2_h5, const char* outframe_h5, NewtonProgressCallback cb)
{
	if (cb && !cb(0, "Preparing Sentinel-1 two-frame merge...")) return -2;
	if (!frame1_h5 || !frame2_h5 || !outframe_h5)
	{
		fprintf(stderr, "S1_frame_merge(): input check failed!\n");
		return -1;
	}
	//检查是否属于同一轨道相邻frame
	int ret;
	double start1, start2, end1, end2, prf, slant_range_first_pixel1, slant_range_first_pixel2;
	string start_time1, end_time1, start_time2, end_time2;
	ret = Hdf5IO::readString(frame1_h5, "acquisition_start_time", start_time1);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(start_time1.c_str(), &start1);
	ret = Hdf5IO::readString(frame1_h5, "acquisition_stop_time", end_time1);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(end_time1.c_str(), &end1);
	ret = Hdf5IO::readString(frame2_h5, "acquisition_start_time", start_time2);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(start_time2.c_str(), &start2);
	ret = Hdf5IO::readString(frame2_h5, "acquisition_stop_time", end_time2);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	ret = utc_to_gps(end_time2.c_str(), &end2);
	if (start1 < start2)
	{
		if (start2 > end1)
		{
			fprintf(stderr, "S1_frame_merge(): not adjacent frames!\n");
			return -1;
		}
	}
	else
	{
		if (start1 > end2)
		{
			fprintf(stderr, "S1_frame_merge(): not adjacent frames!\n");
			return -1;
		}
	}
	ret = Hdf5IO::readDouble(frame1_h5, "slant_range_first_pixel", &slant_range_first_pixel1);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readDouble(frame2_h5, "slant_range_first_pixel", &slant_range_first_pixel2);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	if (fabs(slant_range_first_pixel1 - slant_range_first_pixel2) > 0.1)
	{
		fprintf(stderr, "S1_frame_merge(): not the same track!\n");
		return -1;
	}

	ret = Hdf5IO::createFile(outframe_h5);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	//融合azimuthFmRateList
	Mat azimuthFmRateList1, azimuthFmRateList2;
	ret = Hdf5IO::readArray(frame1_h5, "azimuthFmRateList", azimuthFmRateList1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "azimuthFmRateList", azimuthFmRateList2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		int t_remain = azimuthFmRateList1.rows - 1;
	for (int i = 0; i < azimuthFmRateList1.rows; i++)
	{
		if (cb && i % 64 == 0 && !cb(30, "Merging azimuth FM rate lists...")) return -2;
			if (azimuthFmRateList2.at<double>(0, 0) <= azimuthFmRateList1.at<double>(i, 0))
			{
				t_remain = i; break;
			}
		}
		azimuthFmRateList1(cv::Range(0, t_remain), cv::Range(0, azimuthFmRateList1.cols)).copyTo(azimuthFmRateList1);
		cv::vconcat(azimuthFmRateList1, azimuthFmRateList2, azimuthFmRateList1);
		Hdf5IO::writeArray(outframe_h5, "azimuthFmRateList", azimuthFmRateList1);
	}
	else
	{
		int t_remain = azimuthFmRateList2.rows - 1;
	for (int i = 0; i < azimuthFmRateList2.rows; i++)
	{
		if (cb && i % 64 == 0 && !cb(30, "Merging azimuth FM rate lists...")) return -2;
			if ((azimuthFmRateList1.at<double>(0, 0) <= azimuthFmRateList2.at<double>(i, 0)))
			{
				t_remain = i; break;
			}
		}
		azimuthFmRateList2(cv::Range(0, t_remain), cv::Range(0, azimuthFmRateList2.cols)).copyTo(azimuthFmRateList2);
		cv::vconcat(azimuthFmRateList2, azimuthFmRateList1, azimuthFmRateList1);
		Hdf5IO::writeArray(outframe_h5, "azimuthFmRateList", azimuthFmRateList1);
	}
	//融合dcEstimateList
	Mat dcEstimateList1, dcEstimateList2;
	ret = Hdf5IO::readArray(frame1_h5, "dcEstimateList", dcEstimateList1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "dcEstimateList", dcEstimateList2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		int t_remain = dcEstimateList1.rows - 1;
	for (int i = 0; i < dcEstimateList1.rows; i++)
	{
		if (cb && i % 64 == 0 && !cb(55, "Merging Doppler estimate lists...")) return -2;
			if ((dcEstimateList2.at<double>(0, 0) <= dcEstimateList1.at<double>(i, 0)))
			{
				t_remain = i; break;
			}
		}
		dcEstimateList1(cv::Range(0, t_remain), cv::Range(0, dcEstimateList1.cols)).copyTo(dcEstimateList1);
		cv::vconcat(dcEstimateList1, dcEstimateList2, dcEstimateList1);
		Hdf5IO::writeArray(outframe_h5, "dcEstimateList", dcEstimateList1);
	}
	else
	{
		int t_remain = dcEstimateList2.rows - 1;
	for (int i = 0; i < dcEstimateList2.rows; i++)
	{
		if (cb && i % 64 == 0 && !cb(55, "Merging Doppler estimate lists...")) return -2;
			if ((dcEstimateList1.at<double>(0, 0) <= dcEstimateList2.at<double>(i, 0)))
			{
				t_remain = i; break;
			}
		}
		dcEstimateList2(cv::Range(0, t_remain), cv::Range(0, dcEstimateList2.cols)).copyTo(dcEstimateList2);
		cv::vconcat(dcEstimateList2, dcEstimateList1, dcEstimateList1);
		Hdf5IO::writeArray(outframe_h5, "dcEstimateList", dcEstimateList1);
	}
	//融合state_vec
	Mat state_vec1, state_vec2;
	ret = Hdf5IO::readArray(frame1_h5, "state_vec", state_vec1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "state_vec", state_vec2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		int t_remain = state_vec1.rows - 1;
		for (int i = 0; i < state_vec1.rows; i++)
		{
			if (cb && i % 64 == 0 && !cb(72, "Merging Sentinel-1 orbit vectors...")) return -2;
			if (state_vec2.at<double>(0, 0) <= state_vec1.at<double>(i, 0))
			{
				t_remain = i; break;
			}
		}
		if (t_remain != 0)
		{
			state_vec1(cv::Range(0, t_remain), cv::Range(0, state_vec1.cols)).copyTo(state_vec1);
			cv::vconcat(state_vec1, state_vec2, state_vec1);
		}
		else
		{
			state_vec2.copyTo(state_vec1);
		}
		Hdf5IO::writeArray(outframe_h5, "state_vec", state_vec1);
	}
	else
	{
		int t_remain = state_vec2.rows - 1;
		for (int i = 0; i < state_vec2.rows; i++)
		{
			if (cb && i % 64 == 0 && !cb(72, "Merging Sentinel-1 orbit vectors...")) return -2;
			if (state_vec1.at<double>(0, 0) <= state_vec2.at<double>(i, 0))
			{
				t_remain = i; break;
			}
		}
		if (t_remain != 0)
		{
			state_vec2(cv::Range(0, t_remain), cv::Range(0, state_vec2.cols)).copyTo(state_vec2);
			cv::vconcat(state_vec2, state_vec1, state_vec1);
		}
		else
		{

		}
		
		Hdf5IO::writeArray(outframe_h5, "state_vec", state_vec1);
	}
	//融合fine_state_vec
	Mat fine_state_vec1, fine_state_vec2;
	ret = Hdf5IO::readArray(frame1_h5, "fine_state_vec", fine_state_vec1);
	ret += Hdf5IO::readArray(frame2_h5, "fine_state_vec", fine_state_vec2);
	if (ret == 0)
	{
		if (start1 < start2)
		{
			int t_remain = fine_state_vec1.rows - 1;
			for (int i = 0; i < fine_state_vec1.rows; i++)
			{
				if (cb && i % 64 == 0 && !cb(76, "Merging Sentinel-1 precise orbit vectors...")) return -2;
				if (fine_state_vec2.at<double>(0, 0) <= fine_state_vec1.at<double>(i, 0))
				{
					t_remain = i; break;
				}
			}
			if (t_remain != 0)
			{
				fine_state_vec1(cv::Range(0, t_remain), cv::Range(0, fine_state_vec1.cols)).copyTo(fine_state_vec1);
				cv::vconcat(fine_state_vec1, fine_state_vec2, fine_state_vec1);
			}
			else
			{
				fine_state_vec2.copyTo(fine_state_vec1);
			}
			Hdf5IO::writeArray(outframe_h5, "fine_state_vec", fine_state_vec1);
		}
		else
		{
			int t_remain = fine_state_vec2.rows - 1;
			for (int i = 0; i < fine_state_vec2.rows; i++)
			{
				if (cb && i % 64 == 0 && !cb(76, "Merging Sentinel-1 precise orbit vectors...")) return -2;
				if (fine_state_vec1.at<double>(0, 0) <= fine_state_vec2.at<double>(i, 0))
				{
					t_remain = i; break;
				}
			}
			if (t_remain != 0)
			{
				fine_state_vec2(cv::Range(0, t_remain), cv::Range(0, fine_state_vec2.cols)).copyTo(fine_state_vec2);
				cv::vconcat(fine_state_vec2, fine_state_vec1, fine_state_vec1);
			}
			
			Hdf5IO::writeArray(outframe_h5, "fine_state_vec", fine_state_vec1);
		}
	}
	
	//融合gcps
	if (cb && !cb(82, "Merging Sentinel-1 geolocation points...")) return -2;
	Mat gcps1, gcps2;
	int rows1, rows2;
	ret = Hdf5IO::readInt(frame1_h5, "azimuth_len", &rows1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(frame2_h5, "azimuth_len", &rows2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame1_h5, "gcps", gcps1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "gcps", gcps2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		int t;
		for (int i = 0; i < gcps2.rows; i++)
		{
			if (cb && i % 64 == 0 && !cb(84, "Merging Sentinel-1 control points...")) return -2;
			if (fabs(gcps2.at<double>(i, 3)) > 1.0)
			{
				t = i; break;
			}
		}
		gcps2(cv::Range(t, gcps2.rows), cv::Range(0, gcps2.cols)).copyTo(gcps2);
		Mat temp = gcps2(cv::Range(0, gcps2.rows), cv::Range(3, 4)) + gcps1.at<double>(gcps1.rows - 1, 3);
		temp.copyTo(gcps2(cv::Range(0, gcps2.rows), cv::Range(3, 4)));
		cv::vconcat(gcps1, gcps2, gcps1);
		Hdf5IO::writeArray(outframe_h5, "gcps", gcps1);
	}
	else
	{
		int t;
		for (int i = 0; i < gcps1.rows; i++)
		{
			if (cb && i % 64 == 0 && !cb(84, "Merging Sentinel-1 control points...")) return -2;
			if (fabs(gcps1.at<double>(i, 3)) > 1.0)
			{
				t = i; break;
			}
		}
		gcps1(cv::Range(t, gcps1.rows), cv::Range(0, gcps1.cols)).copyTo(gcps1);
		Mat temp = gcps1(cv::Range(0, gcps1.rows), cv::Range(3, 4)) + gcps2.at<double>(gcps2.rows - 1, 3);
		temp.copyTo(gcps1(cv::Range(0, gcps1.rows), cv::Range(3, 4)));
		cv::vconcat(gcps2, gcps1, gcps1);
		Hdf5IO::writeArray(outframe_h5, "gcps", gcps1);
	}
	//融合burstAzimuthTime
	Mat burstAzimuthTime1, burstAzimuthTime2;
	ret = Hdf5IO::readArray(frame1_h5, "burstAzimuthTime", burstAzimuthTime1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "burstAzimuthTime", burstAzimuthTime2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		cv::vconcat(burstAzimuthTime1, burstAzimuthTime2, burstAzimuthTime1);
	}
	else
	{
		cv::vconcat(burstAzimuthTime2, burstAzimuthTime1, burstAzimuthTime1);
	}
	Hdf5IO::writeArray(outframe_h5, "burstAzimuthTime", burstAzimuthTime1);
	//融合firstValidLine
	Mat firstValidLine1, firstValidLine2;
	ret = Hdf5IO::readArray(frame1_h5, "firstValidLine", firstValidLine1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "firstValidLine", firstValidLine2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		cv::vconcat(firstValidLine1, firstValidLine2, firstValidLine1);
	}
	else
	{
		cv::vconcat(firstValidLine2, firstValidLine1, firstValidLine1);
	}
	Hdf5IO::writeArray(outframe_h5, "firstValidLine", firstValidLine1);
	//融合firstValidSample
	Mat firstValidSample1, firstValidSample2;
	ret = Hdf5IO::readArray(frame1_h5, "firstValidSample", firstValidSample1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "firstValidSample", firstValidSample2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		cv::vconcat(firstValidSample1, firstValidSample2, firstValidSample1);
	}
	else
	{
		cv::vconcat(firstValidSample2, firstValidSample1, firstValidSample1);
	}
	Hdf5IO::writeArray(outframe_h5, "firstValidSample", firstValidSample1);
	//融合lastValidLine
	Mat lastValidLine1, lastValidLine2;
	ret = Hdf5IO::readArray(frame1_h5, "lastValidLine", lastValidLine1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "lastValidLine", lastValidLine2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		cv::vconcat(lastValidLine1, lastValidLine2, lastValidLine1);
	}
	else
	{
		cv::vconcat(lastValidLine2, lastValidLine1, lastValidLine1);
	}
	Hdf5IO::writeArray(outframe_h5, "lastValidLine", lastValidLine1);
	//融合lastValidSample
	Mat lastValidSample1, lastValidSample2;
	ret = Hdf5IO::readArray(frame1_h5, "lastValidSample", lastValidSample1);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readArray(frame2_h5, "lastValidSample", lastValidSample2);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	if (start1 < start2)
	{
		cv::vconcat(lastValidSample1, lastValidSample2, lastValidSample1);
	}
	else
	{
		cv::vconcat(lastValidSample2, lastValidSample1, lastValidSample1);
	}
	Hdf5IO::writeArray(outframe_h5, "lastValidSample", lastValidSample1);
	//融合拍摄时间
	if (start1 < start2)
	{
		Hdf5IO::writeString(outframe_h5, "acquisition_start_time", start_time1.c_str());
		Hdf5IO::writeString(outframe_h5, "acquisition_stop_time", end_time2.c_str());
	}
	else
	{
		Hdf5IO::writeString(outframe_h5, "acquisition_start_time", start_time2.c_str());
		Hdf5IO::writeString(outframe_h5, "acquisition_stop_time", end_time1.c_str());
	}
	//融合azimuthSteeringRate
	double azimuthSteeringRate;
	ret = Hdf5IO::readDouble(frame1_h5, "azimuthSteeringRate", &azimuthSteeringRate);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	Hdf5IO::writeDouble(outframe_h5, "azimuthSteeringRate", azimuthSteeringRate);
	//融合azimuth_len，range_len
	int azimuth_len1, range_len1, azimuth_len2, range_len2;
	ret = Hdf5IO::readInt(frame1_h5, "azimuth_len", &azimuth_len1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(frame1_h5, "range_len", &range_len1);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(frame2_h5, "azimuth_len", &azimuth_len2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = Hdf5IO::readInt(frame2_h5, "range_len", &range_len2);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	azimuth_len1 += azimuth_len2;
	Hdf5IO::writeInt(outframe_h5, "azimuth_len", azimuth_len1);
	int range_len = range_len1 >= range_len2 ? range_len1 : range_len2;
	Hdf5IO::writeInt(outframe_h5, "range_len", range_len1);
	//融合azimuth_spacing，range_spacing
	double azimuth_spacing, range_spacing;
	ret = Hdf5IO::readDouble(frame1_h5, "azimuth_spacing", &azimuth_spacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	Hdf5IO::writeDouble(outframe_h5, "azimuth_spacing", azimuth_spacing);
	ret = Hdf5IO::readDouble(frame1_h5, "range_spacing", &range_spacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	Hdf5IO::writeDouble(outframe_h5, "range_spacing", range_spacing);
	//burstCount, carrier_frequency, heading, incidence_center, linesPerBurst, prf, samplesPerBurst, 
	double carrier_frequency, heading, incidence_center, slant_range_first_pixel;
	int burstCount1, burstCount2, linesPerBurst;
	// removed unused: samplesPerBurst (H5 read commented out)
	Hdf5IO::readDouble(frame1_h5, "carrier_frequency", &carrier_frequency);
	Hdf5IO::readDouble(frame1_h5, "slant_range_first_pixel", &slant_range_first_pixel);
	Hdf5IO::readDouble(frame1_h5, "inc_center", &incidence_center);
	Hdf5IO::readDouble(frame1_h5, "heading", &heading);
	Hdf5IO::readDouble(frame1_h5, "prf", &prf);
	Hdf5IO::readInt(frame1_h5, "burstCount", &burstCount1);
	Hdf5IO::readInt(frame2_h5, "burstCount", &burstCount2);
	Hdf5IO::readInt(frame1_h5, "linesPerBurst", &linesPerBurst);
	//conversion.read_int_from_h5(frame1_h5, "samplesPerBurst", &samplesPerBurst);
	Hdf5IO::writeDouble(outframe_h5, "slant_range_first_pixel", slant_range_first_pixel);
	Hdf5IO::writeDouble(outframe_h5, "carrier_frequency", carrier_frequency);
	Hdf5IO::writeDouble(outframe_h5, "inc_center", incidence_center);
	Hdf5IO::writeDouble(outframe_h5, "heading", heading);
	Hdf5IO::writeDouble(outframe_h5, "prf", prf);
	Hdf5IO::writeInt(outframe_h5, "burstCount", burstCount1 + burstCount2);
	Hdf5IO::writeInt(outframe_h5, "linesPerBurst", linesPerBurst);
	Hdf5IO::writeInt(outframe_h5, "samplesPerBurst", range_len);

	//sensor, swath, polarization, orbit_dir, imaging_mode;
	string sensor, swath, polarization, orbit_dir, imaging_mode;
	Hdf5IO::readString(frame1_h5, "sensor", sensor);
	Hdf5IO::readString(frame1_h5, "swath", swath);
	Hdf5IO::readString(frame1_h5, "polarization", polarization);
	Hdf5IO::readString(frame1_h5, "orbit_dir", orbit_dir);
	Hdf5IO::readString(frame1_h5, "imaging_mode", imaging_mode);

	Hdf5IO::writeString(outframe_h5, "sensor", sensor.c_str());
	Hdf5IO::writeString(outframe_h5, "swath", swath.c_str());
	Hdf5IO::writeString(outframe_h5, "polarization", polarization.c_str());
	Hdf5IO::writeString(outframe_h5, "orbit_dir", orbit_dir.c_str());
	Hdf5IO::writeString(outframe_h5, "imaging_mode", imaging_mode.c_str());

	//s_re, s_im
	if (cb && !cb(92, "Writing merged Sentinel-1 SLC...")) return -2;
	Mat s_re, s_re2;
	Hdf5IO::readArray(frame1_h5, "s_re", s_re);
	Hdf5IO::readArray(frame2_h5, "s_re", s_re2);
	if (range_len1 != range_len2)
	{
		if (range_len1 > range_len2)
		{
			cv::copyMakeBorder(s_re2, s_re2, 0, 0, 0, range_len1 - range_len2, BORDER_CONSTANT, cv::Scalar(0));
		}
		else
		{
			cv::copyMakeBorder(s_re, s_re, 0, 0, 0, range_len2 - range_len1, BORDER_CONSTANT, cv::Scalar(0));
		}
	}
	if (start1 < start2)
	{
		cv::vconcat(s_re, s_re2, s_re);
	}
	else
	{
		cv::vconcat(s_re2, s_re, s_re);
	}
	Hdf5IO::writeArray(outframe_h5, "s_re", s_re);

	Hdf5IO::readArray(frame1_h5, "s_im", s_re);
	Hdf5IO::readArray(frame2_h5, "s_im", s_re2);
	if (range_len1 != range_len2)
	{
		if (range_len1 > range_len2)
		{
			cv::copyMakeBorder(s_re2, s_re2, 0, 0, 0, range_len1 - range_len2, BORDER_CONSTANT, cv::Scalar(0));
		}
		else
		{
			cv::copyMakeBorder(s_re, s_re, 0, 0, 0, range_len2 - range_len1, BORDER_CONSTANT, cv::Scalar(0));
		}
	}
	if (start1 < start2)
	{
		cv::vconcat(s_re, s_re2, s_re);
	}
	else
	{
		cv::vconcat(s_re2, s_re, s_re);
	}
	Hdf5IO::writeArray(outframe_h5, "s_im", s_re);
	if (cb && !cb(96, "Fitting merged Sentinel-1 metadata...")) return -2;


	//拟合系数

	{
		Mat lon_coefficient, lat_coefficient, inc_coefficient, row_coefficient, col_coefficient;
		int numberOfSamples = range_len;
		double mean_lon, mean_lat, mean_inc, max_lon, max_lat, max_inc, min_lon, min_lat, min_inc;
		Mat lon, lat, inc, row, col, gcps;
		gcps1.copyTo(gcps);
		gcps(cv::Range(0, gcps.rows), cv::Range(0, 1)).copyTo(lon);
		gcps(cv::Range(0, gcps.rows), cv::Range(1, 2)).copyTo(lat);
		gcps(cv::Range(0, gcps.rows), cv::Range(3, 4)).copyTo(row);
		gcps(cv::Range(0, gcps.rows), cv::Range(4, 5)).copyTo(col);
		gcps(cv::Range(0, gcps.rows), cv::Range(5, 6)).copyTo(inc);
		mean_lon = cv::mean(lon)[0];
		mean_lat = cv::mean(lat)[0];
		mean_inc = cv::mean(inc)[0];
		cv::minMaxLoc(lon, &min_lon, &max_lon);
		cv::minMaxLoc(lat, &min_lat, &max_lat);
		cv::minMaxLoc(inc, &min_inc, &max_inc);
		lon = (lon - mean_lon) / (max_lon - min_lon + 1e-10);
		lat = (lat - mean_lat) / (max_lat - min_lat + 1e-10);
		inc = (inc - mean_inc) / (max_inc - min_inc + 1e-10);
		row = (row + 1 - double(numberOfSamples) * 0.5) / (double(numberOfSamples) + 1e-10);//sentinel行列起点为0，+1统一为1.
		col = (col + 1 - double(numberOfSamples) * 0.5) / (double(numberOfSamples) + 1e-10);

		//拟合经度

		Mat A, B, b, temp, coefficient, error, eye, b_t, a, a_t;
		double rms;
		lon.copyTo(b);
		A = Mat::ones(lon.rows, 25, CV_64F);
		row.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
		temp = row.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

		col.copyTo(temp);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

		col.copyTo(temp);
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

		col.copyTo(temp);
		temp = temp.mul(col);
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

		col.copyTo(temp);
		temp = temp.mul(col);
		temp = temp.mul(col);
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

		cv::transpose(A, temp);
		B = temp * b;
		A.copyTo(a);
		cv::transpose(a, a_t);
		A = temp * A;
		rms = -1.0;
		if (cv::invert(A, error, cv::DECOMP_LU) > 0)
		{
			cv::transpose(b, b_t);
			error = b_t * b - (b_t * a) * error * (a_t * b);
			//error = b_t * (eye - a * error * a_t) * b;
			rms = sqrt(error.at<double>(0, 0) / double(b.rows));
		}
		if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
		{
			temp.create(1, 32, CV_64F);
			temp.at<double>(0, 0) = mean_lon;
			temp.at<double>(0, 1) = max_lon - min_lon + 1e-10;
			temp.at<double>(0, 2) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 3) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 4) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 5) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 31) = rms;
			cv::transpose(coefficient, coefficient);
			coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
			temp.copyTo(lon_coefficient);
		}

		//拟合纬度

		lat.copyTo(b);

		A = Mat::ones(lon.rows, 25, CV_64F);
		row.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
		temp = row.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

		col.copyTo(temp);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

		col.copyTo(temp);
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

		col.copyTo(temp);
		temp = temp.mul(col);
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

		col.copyTo(temp);
		temp = temp.mul(col);
		temp = temp.mul(col);
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
		temp = temp.mul(row);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

		cv::transpose(A, temp);
		B = temp * b;
		A.copyTo(a);
		cv::transpose(a, a_t);
		A = temp * A;
		rms = -1.0;
		if (cv::invert(A, error, cv::DECOMP_LU) > 0)
		{
			cv::transpose(b, b_t);
			error = b_t * b - (b_t * a) * error * (a_t * b);
			//error = b_t * (eye - a * error * a_t) * b;
			rms = sqrt(error.at<double>(0, 0) / double(b.rows));
		}
		if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
		{
			temp.create(1, 32, CV_64F);
			temp.at<double>(0, 0) = mean_lat;
			temp.at<double>(0, 1) = max_lat - min_lat + 1e-10;
			temp.at<double>(0, 2) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 3) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 4) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 5) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 31) = rms;
			cv::transpose(coefficient, coefficient);
			coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
			temp.copyTo(lat_coefficient);
		}

		//拟合下视角

		inc.copyTo(b);
		A = Mat::ones(inc.rows, 6, CV_64F);
		col.copyTo(A(cv::Range(0, inc.rows), cv::Range(1, 2)));
		temp = col.mul(col);
		temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(2, 3)));
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(3, 4)));
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(4, 5)));
		temp = temp.mul(col);
		temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(5, 6)));
		cv::transpose(A, temp);
		B = temp * b;
		A.copyTo(a);
		cv::transpose(a, a_t);
		A = temp * A;
		rms = -1.0;
		if (cv::invert(A, error, cv::DECOMP_LU) > 0)
		{
			cv::transpose(b, b_t);
			error = b_t * b - (b_t * a) * error * (a_t * b);
			//error = b_t * (eye - a * error * a_t) * b;
			rms = sqrt(error.at<double>(0, 0) / double(b.rows));
		}
		if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
		{
			temp.create(1, 11, CV_64F);
			temp.at<double>(0, 0) = mean_inc;
			temp.at<double>(0, 1) = max_inc - min_inc + 1e-10;
			temp.at<double>(0, 2) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 3) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 10) = rms;
			cv::transpose(coefficient, coefficient);
			coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(4, 10)));
			temp.copyTo(inc_coefficient);
		}

		//拟合行坐标

		row.copyTo(b);
		A = Mat::ones(lon.rows, 25, CV_64F);
		lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
		temp = lon.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

		lat.copyTo(temp);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

		lat.copyTo(temp);
		temp = temp.mul(lat);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

		lat.copyTo(temp);
		temp = temp.mul(lat);
		temp = temp.mul(lat);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

		lat.copyTo(temp);
		temp = temp.mul(lat);
		temp = temp.mul(lat);
		temp = temp.mul(lat);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

		cv::transpose(A, temp);
		B = temp * b;
		A.copyTo(a);
		cv::transpose(a, a_t);
		A = temp * A;
		rms = -1.0;
		if (cv::invert(A, error, cv::DECOMP_LU) > 0)
		{
			cv::transpose(b, b_t);
			error = b_t * b - (b_t * a) * error * (a_t * b);
			//error = b_t * (eye - a * error * a_t) * b;
			rms = sqrt(error.at<double>(0, 0) / double(b.rows));
		}
		if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
		{
			temp.create(1, 32, CV_64F);
			temp.at<double>(0, 0) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 1) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 2) = mean_lon;
			temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
			temp.at<double>(0, 4) = mean_lat;
			temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
			temp.at<double>(0, 31) = rms;
			cv::transpose(coefficient, coefficient);
			coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
			temp.copyTo(row_coefficient);
		}

		//拟合列坐标

		col.copyTo(b);
		A = Mat::ones(lon.rows, 25, CV_64F);
		lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
		temp = lon.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

		lat.copyTo(temp);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

		lat.copyTo(temp);
		temp = temp.mul(lat);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

		lat.copyTo(temp);
		temp = temp.mul(lat);
		temp = temp.mul(lat);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

		lat.copyTo(temp);
		temp = temp.mul(lat);
		temp = temp.mul(lat);
		temp = temp.mul(lat);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
		temp = temp.mul(lon);
		temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

		cv::transpose(A, temp);
		B = temp * b;
		A.copyTo(a);
		cv::transpose(a, a_t);
		A = temp * A;
		rms = -1.0;
		if (cv::invert(A, error, cv::DECOMP_LU) > 0)
		{
			cv::transpose(b, b_t);
			error = b_t * b - (b_t * a) * error * (a_t * b);
			//error = b_t * (eye - a * error * a_t) * b;
			rms = sqrt(error.at<double>(0, 0) / double(b.rows));
		}
		if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
		{
			temp.create(1, 32, CV_64F);
			temp.at<double>(0, 0) = double(numberOfSamples) * 0.5;
			temp.at<double>(0, 1) = double(numberOfSamples) + 1e-10;
			temp.at<double>(0, 2) = mean_lon;
			temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
			temp.at<double>(0, 4) = mean_lat;
			temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
			temp.at<double>(0, 31) = rms;
			cv::transpose(coefficient, coefficient);
			coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
			temp.copyTo(col_coefficient);
		}
		Hdf5IO::writeArray(outframe_h5, "lon_coefficient", lon_coefficient);
		Hdf5IO::writeArray(outframe_h5, "lat_coefficient", lat_coefficient);
		Hdf5IO::writeArray(outframe_h5, "inc_coefficient", inc_coefficient);
		Hdf5IO::writeArray(outframe_h5, "row_coefficient", row_coefficient);
		Hdf5IO::writeArray(outframe_h5, "col_coefficient", col_coefficient);
	}
	
	if (cb && !cb(100, "Sentinel-1 two-frame merge complete.")) return -2;
	return 0;
}

