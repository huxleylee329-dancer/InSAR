#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\Hdf5IO.h"
#include "..\include\tinyxml.h"
#include "gdal_priv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace cv;
using namespace std;

extern void InitializeGDALAndProjOnce();

int Utils::SAR2UTM(
	Mat& mapped_lon,
	Mat& mapped_lat,
	Mat& phase, 
	Mat& mapped_phase,
	int interpolation_method,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south
)
{
	if (mapped_lat.size() != mapped_lon.size() ||
		mapped_lat.size() != phase.size() ||
		phase.rows < 2 ||
		phase.cols < 2 ||
		phase.type() != CV_64F ||
		mapped_lat.type() != mapped_lon.type() ||
		(mapped_lon.type() != CV_32F && mapped_lon.type() != CV_64F)
		)
	{
		fprintf(stderr, "SAR2UTM(): input check failed!\n");
		return -1;
	}
	//确定经纬度覆盖范围
	double max_lon = -380.0, min_lon = 380.0, max_lat = -380.0, min_lat = 180.0;
	if (mapped_lat.type() == CV_32F)
	{
		for (int i = 0; i < mapped_lat.rows; i++)
		{
			for (int j = 0; j < mapped_lat.cols; j++)
			{
				if (mapped_lat.at<float>(i, j) < 350.0)
				{
					min_lat = min_lat > mapped_lat.at<float>(i, j) ? mapped_lat.at<float>(i, j) : min_lat;
					max_lat = max_lat < mapped_lat.at<float>(i, j) ? mapped_lat.at<float>(i, j) : max_lat;
					min_lon = min_lon > mapped_lon.at<float>(i, j) ? mapped_lon.at<float>(i, j) : min_lon;
					max_lon = max_lon < mapped_lon.at<float>(i, j) ? mapped_lon.at<float>(i, j) : max_lon;
				}
			}
		}
	}
	else
	{
		for (int i = 0; i < mapped_lat.rows; i++)
		{
			for (int j = 0; j < mapped_lat.cols; j++)
			{
				if (mapped_lat.at<double>(i, j) < 350.0)
				{
					min_lat = min_lat > mapped_lat.at<double>(i, j) ? mapped_lat.at<double>(i, j) : min_lat;
					max_lat = max_lat < mapped_lat.at<double>(i, j) ? mapped_lat.at<double>(i, j) : max_lat;
					min_lon = min_lon > mapped_lon.at<double>(i, j) ? mapped_lon.at<double>(i, j) : min_lon;
					max_lon = max_lon < mapped_lon.at<double>(i, j) ? mapped_lon.at<double>(i, j) : max_lon;
				}
			}
		}
	}
	//cv::minMaxLoc(mapped_lon, &min_lon, &max_lon);
	//cv::minMaxLoc(mapped_lat, &min_lat, &max_lat);
	double west = max_lon - min_lon > 180.0 ? max_lon : min_lon;
	if (lon_west) *lon_west = west;
	
	//确定经纬度采样间隔
	double lon_interval, lat_interval;
	Mat temp1, temp2;
	int rows_start = mapped_lon.rows / 4;
	int rows_end = rows_start + mapped_lon.rows / 4;
	mapped_lon(cv::Range(rows_start, rows_end), cv::Range(0, mapped_lon.cols)).copyTo(temp1);
	mapped_lon(cv::Range(rows_start + 1, rows_end + 1), cv::Range(0, mapped_lon.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = temp1 / 180.0 * PI;
	wrap(temp1, temp1);
	temp1 = temp1 / PI * 180.0;
	temp1 = cv::abs(temp1);
	lon_interval = cv::mean(temp1)[0];
	double sigma1, sigma2;
	std(temp1, &sigma1);

	mapped_lon(cv::Range(rows_start, rows_end), cv::Range(0, mapped_lon.cols - 1)).copyTo(temp1);
	mapped_lon(cv::Range(rows_start, rows_end), cv::Range(1, mapped_lon.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = temp1 / 180.0 * PI;
	wrap(temp1, temp1);
	temp1 = temp1 / PI * 180.0;
	temp1 = cv::abs(temp1);
	std(temp1, &sigma2);
	lon_interval = (cv::mean(temp1)[0] + fabs(lon_interval)) / 2.0;
	lon_interval = lon_interval + 1.0 * (sigma1 + sigma2) / 2.0;

	mapped_lat(cv::Range(rows_start, rows_end), cv::Range(0, mapped_lat.cols)).copyTo(temp1);
	mapped_lat(cv::Range(rows_start + 1, rows_end + 1), cv::Range(0, mapped_lat.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = cv::abs(temp1);
	lat_interval = cv::mean(temp1)[0];
	std(temp1, &sigma1);

	mapped_lat(cv::Range(rows_start, rows_end), cv::Range(0, mapped_lat.cols - 1)).copyTo(temp1);
	mapped_lat(cv::Range(rows_start, rows_end), cv::Range(1, mapped_lat.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = cv::abs(temp1);
	lat_interval = (cv::mean(temp1)[0] + lat_interval) / 2.0;
	std(temp1, &sigma2);
	lat_interval = lat_interval + 1.0 * (sigma1 + sigma2) / 2.0;

	int rows = phase.rows; int cols = phase.cols;
	

	//计算UTM坐标系相位尺寸
	int UTM_rows = static_cast<int>((max_lat - min_lat) / lat_interval);
	UTM_rows += 2;
	double south = max_lat - (double)(UTM_rows - 1) * lat_interval;
	double north = max_lat;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = south;
	double max_lon_temp = max_lon - min_lon;
	max_lon_temp = max_lon_temp > 180.0 ? 360.0 - max_lon_temp : max_lon_temp;
	int UTM_cols = static_cast<int>(max_lon_temp / lon_interval);
	UTM_cols += 2;
	double east = west + (double)(UTM_cols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	Mat b_filled(UTM_rows, UTM_cols, CV_8U); b_filled = 0;
	mapped_phase.create(UTM_rows, UTM_cols, CV_64F); mapped_phase = 0.0;
	//开始地理编码
	if (mapped_lat.type() == CV_32F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				double lon, lat;
				int row, col;
				lon = mapped_lon.at<float>(i, j);
				if (lon > 350.0) continue;
				lat = mapped_lat.at<float>(i, j);
				row = (int)round((lat - min_lat) / lat_interval);
				lon = fabs(lon - west);
				lon = lon > 180.0 ? 360.0 - lon : lon;
				col = (int)round(lon / lon_interval);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				b_filled.at<uchar>(row, col) = 1;
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				double lon, lat;
				int row, col;
				lon = mapped_lon.at<double>(i, j);
				if (lon > 350.0) continue;
				lat = mapped_lat.at<double>(i, j);
				row = (int)round((lat - min_lat) / lat_interval);
				lon = fabs(lon - west);
				lon = lon > 180.0 ? 360.0 - lon : lon;
				col = (int)round(lon / lon_interval);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				b_filled.at<uchar>(row, col) = 1;
			}
		}
	}
	

	//插值
	if (interpolation_method == 0)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < UTM_rows; i++)
		{
			for (int j = 0; j < UTM_cols; j++)
			{
				if (b_filled.at<uchar>(i, j) != 0) continue;
				int up, down, left, right;
				// removed unused: up_count, down_count, left_count, right_count, value1, value2, ratio1, ratio2 (nearest-neighbor method)
				//寻找上面有值的点
				up = i;
				while (true)
				{
					up--;
					if (up < 0) break;
					if (b_filled.at<uchar>(up, j) != 0) break;
				}
				//寻找下面有值的点
				down = i;
				while (true)
				{
					down++;
					if (down > UTM_rows - 1) break;
					if (b_filled.at<uchar>(down, j) != 0) break;
				}
				//寻找左边有值的点
				left = j;
				while (true)
				{
					left--;
					if (left < 0) break;
					if (b_filled.at<uchar>(i, left) != 0) break;
				}
				//寻找右边有值的点
				right = j;
				while (true)
				{
					right++;
					if (right > UTM_cols - 1) break;
					if (b_filled.at<uchar>(i, right) != 0) break;
				}

				//上下左右都有值
				if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
				{
					int x_i = i, x_j = j;
					int down_distance = down - i;
					int up_distance = i - up;
					int left_distance = j - left;
					int right_distance = right - j;
					if (down_distance < up_distance && down_distance < left_distance && down_distance < right_distance)
					{
						x_i = down; x_j = j;
					}
					else if (up_distance < down_distance && up_distance < left_distance && up_distance < right_distance)
					{
						x_i = up; x_j = j;
					}
					else if (left_distance < down_distance && left_distance < up_distance && left_distance < right_distance)
					{
						x_i = i; x_j = left;
					}
					else
					{
						x_i = i; x_j = right;
					}
					mapped_phase.at<double>(i, j) = mapped_phase.at<double>(x_i, x_j);
					continue;
				}
				//上下有值
				if (up >= 0 && down <= UTM_rows - 1)
				{
					int x_i = i, x_j = j;
					int down_distance = down - i;
					int up_distance = i - up;
					if (down_distance < up_distance)
					{
						x_i = down; x_j = j;
					}
					else
					{
						x_i = down; x_j = j;
					}
					mapped_phase.at<double>(i, j) = mapped_phase.at<double>(x_i, x_j);
					continue;
				}
				//左右有值
				if (left >= 0 && right <= UTM_cols - 1)
				{
					int x_i = i, x_j = j;
					int left_distance = j - left;
					int right_distance = right - j;
					if (left_distance < right_distance)
					{
						x_i = i; x_j = left;
					}
					else
					{
						x_i = i; x_j = right;
					}
					mapped_phase.at<double>(i, j) = mapped_phase.at<double>(x_i, x_j);
					continue;
				}
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < UTM_rows; i++)
		{
			for (int j = 0; j < UTM_cols; j++)
			{
				if (b_filled.at<uchar>(i, j) != 0) continue;
				int up, down, left, right;
				// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
				double value1, value2, ratio1, ratio2;
				//寻找上面有值的点
				up = i;
				while (true)
				{
					up--;
					if (up < 0) break;
					if (b_filled.at<uchar>(up, j) != 0) break;
				}
				//寻找下面有值的点
				down = i;
				while (true)
				{
					down++;
					if (down > UTM_rows - 1) break;
					if (b_filled.at<uchar>(down, j) != 0) break;
				}
				//寻找左边有值的点
				left = j;
				while (true)
				{
					left--;
					if (left < 0) break;
					if (b_filled.at<uchar>(i, left) != 0) break;
				}
				//寻找右边有值的点
				right = j;
				while (true)
				{
					right++;
					if (right > UTM_cols - 1) break;
					if (b_filled.at<uchar>(i, right) != 0) break;
				}

				//上下左右都有值
				if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
				{
					
					ratio1 = double(j - left) / double(right - left);
					value1 = double(mapped_phase.at<double>(i, left)) +
						double(mapped_phase.at<double>(i, right) - mapped_phase.at<double>(i, right)) * ratio1;
					ratio2 = double(i - up) / double(down - up);
					value2 = double(mapped_phase.at<double>(up, j)) +
						double(mapped_phase.at<double>(down, j) - mapped_phase.at<double>(up, j)) * ratio2;
					mapped_phase.at<double>(i, j) = (value1 + value2) / 2.0;
					continue;
				}
				//上下有值
				if (up >= 0 && down <= UTM_rows - 1)
				{
					ratio2 = double(i - up) / double(down - up);
					value2 = double(mapped_phase.at<double>(up, j)) +
						double(mapped_phase.at<double>(down, j) - mapped_phase.at<double>(up, j)) * ratio2;
					mapped_phase.at<double>(i, j) = value2;
					continue;
				}
				//左右有值
				if (left >= 0 && right <= UTM_cols - 1)
				{
					ratio1 = double(j - left) / double(right - left);
					value1 = double(mapped_phase.at<double>(i, left)) +
						double(mapped_phase.at<double>(i, right) - mapped_phase.at<double>(i, right)) * ratio1;
					mapped_phase.at<double>(i, j) = value1;
					continue;
				}
			}
		}
	}
	

	cv::flip(mapped_phase, mapped_phase, 0);
	return 0;
}

int Utils::SAR2UTM(Mat& mapped_lon, Mat& mapped_lat, Mat& phase, Mat& mapped_phase, double grid_size, int interpolation_method, double* lon_east, double* lon_west, double* lat_north, double* lat_south)
{
	if (mapped_lat.size() != mapped_lon.size() ||
		mapped_lat.size() != phase.size() ||
		phase.rows < 2 ||
		phase.cols < 2 ||
		phase.type() != CV_64F ||
		mapped_lat.type() != mapped_lon.type() ||
		(mapped_lon.type() != CV_32F && mapped_lon.type() != CV_64F)
		)
	{
		fprintf(stderr, "SAR2UTM(): input check failed!\n");
		return -1;
	}
	//确定经纬度覆盖范围
	double max_lon = -380.0, min_lon = 380.0, max_lat = -380.0, min_lat = 180.0;
	if (mapped_lat.type() == CV_32F)
	{
		for (int i = 0; i < mapped_lat.rows; i++)
		{
			for (int j = 0; j < mapped_lat.cols; j++)
			{
				if (mapped_lat.at<float>(i, j) < 350.0)
				{
					min_lat = min_lat > mapped_lat.at<float>(i, j) ? mapped_lat.at<float>(i, j) : min_lat;
					max_lat = max_lat < mapped_lat.at<float>(i, j) ? mapped_lat.at<float>(i, j) : max_lat;
					min_lon = min_lon > mapped_lon.at<float>(i, j) ? mapped_lon.at<float>(i, j) : min_lon;
					max_lon = max_lon < mapped_lon.at<float>(i, j) ? mapped_lon.at<float>(i, j) : max_lon;
				}
			}
		}
	}
	else
	{
		for (int i = 0; i < mapped_lat.rows; i++)
		{
			for (int j = 0; j < mapped_lat.cols; j++)
			{
				if (mapped_lat.at<double>(i, j) < 350.0)
				{
					min_lat = min_lat > mapped_lat.at<double>(i, j) ? mapped_lat.at<double>(i, j) : min_lat;
					max_lat = max_lat < mapped_lat.at<double>(i, j) ? mapped_lat.at<double>(i, j) : max_lat;
					min_lon = min_lon > mapped_lon.at<double>(i, j) ? mapped_lon.at<double>(i, j) : min_lon;
					max_lon = max_lon < mapped_lon.at<double>(i, j) ? mapped_lon.at<double>(i, j) : max_lon;
				}
			}
		}
	}
	//cv::minMaxLoc(mapped_lon, &min_lon, &max_lon);
	//cv::minMaxLoc(mapped_lat, &min_lat, &max_lat);
	double west = max_lon - min_lon > 180.0 ? max_lon : min_lon;
	if (lon_west) *lon_west = west;

	//确定经纬度采样间隔
	double lon_interval, lat_interval;
	lon_interval = 5.0 / 6000.0 / (90.0 / grid_size);
	lat_interval = lon_interval;

	int rows = phase.rows; int cols = phase.cols;


	//计算UTM坐标系相位尺寸
	int UTM_rows = static_cast<int>((max_lat - min_lat) / lat_interval);
	UTM_rows += 2;
	double south = max_lat - (double)(UTM_rows - 1) * lat_interval;
	double north = max_lat;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = south;
	double max_lon_temp = max_lon - min_lon;
	max_lon_temp = max_lon_temp > 180.0 ? 360.0 - max_lon_temp : max_lon_temp;
	int UTM_cols = static_cast<int>(max_lon_temp / lon_interval);
	UTM_cols += 2;
	double east = west + (double)(UTM_cols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	Mat b_filled(UTM_rows, UTM_cols, CV_8U); b_filled = 0;
	mapped_phase.create(UTM_rows, UTM_cols, CV_64F); mapped_phase = 0.0;
	//开始地理编码
	if (mapped_lat.type() == CV_32F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				double lon, lat;
				int row, col;
				lon = mapped_lon.at<float>(i, j);
				if (lon > 350.0) continue;
				lat = mapped_lat.at<float>(i, j);
				row = (int)round((max_lat - lat) / lat_interval);
				lon = fabs(lon - west);
				lon = lon > 180.0 ? 360.0 - lon : lon;
				col = (int)round(lon / lon_interval);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				b_filled.at<uchar>(row, col) = b_filled.at<uchar>(row, col) + 1;
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				double lon, lat;
				int row, col;
				lon = mapped_lon.at<double>(i, j);
				if (lon > 350.0) continue;
				lat = mapped_lat.at<double>(i, j);
				row = (int)round((max_lat - lat) / lat_interval);
				lon = fabs(lon - west);
				lon = lon > 180.0 ? 360.0 - lon : lon;
				col = (int)round(lon / lon_interval);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				b_filled.at<uchar>(row, col) = b_filled.at<uchar>(row, col) + 1;
			}
		}
	}

	//插值
	if (interpolation_method == 0)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < UTM_rows; i++)
		{
			for (int j = 0; j < UTM_cols; j++)
			{
				if (b_filled.at<uchar>(i, j) != 0) continue;
				int up, down, left, right;
				// removed unused: up_count, down_count, left_count, right_count, value1, value2, ratio1, ratio2 (nearest-neighbor method)
				//寻找上面有值的点
				up = i;
				while (true)
				{
					up--;
					if (up < 0) break;
					if (b_filled.at<uchar>(up, j) != 0) break;
				}
				//寻找下面有值的点
				down = i;
				while (true)
				{
					down++;
					if (down > UTM_rows - 1) break;
					if (b_filled.at<uchar>(down, j) != 0) break;
				}
				//寻找左边有值的点
				left = j;
				while (true)
				{
					left--;
					if (left < 0) break;
					if (b_filled.at<uchar>(i, left) != 0) break;
				}
				//寻找右边有值的点
				right = j;
				while (true)
				{
					right++;
					if (right > UTM_cols - 1) break;
					if (b_filled.at<uchar>(i, right) != 0) break;
				}

				//上下左右都有值
				if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
				{
					int x_i = i, x_j = j;
					int down_distance = down - i;
					int up_distance = i - up;
					int left_distance = j - left;
					int right_distance = right - j;
					if (down_distance < up_distance && down_distance < left_distance && down_distance < right_distance)
					{
						x_i = down; x_j = j;
					}
					else if (up_distance < down_distance && up_distance < left_distance && up_distance < right_distance)
					{
						x_i = up; x_j = j;
					}
					else if (left_distance < down_distance && left_distance < up_distance && left_distance < right_distance)
					{
						x_i = i; x_j = left;
					}
					else
					{
						x_i = i; x_j = right;
					}
					mapped_phase.at<double>(i, j) = mapped_phase.at<double>(x_i, x_j);
					continue;
				}
				//上下有值
				if (up >= 0 && down <= UTM_rows - 1)
				{
					int x_i = i, x_j = j;
					int down_distance = down - i;
					int up_distance = i - up;
					if (down_distance < up_distance)
					{
						x_i = down; x_j = j;
					}
					else
					{
						x_i = down; x_j = j;
					}
					mapped_phase.at<double>(i, j) = mapped_phase.at<double>(x_i, x_j);
					continue;
				}
				//左右有值
				if (left >= 0 && right <= UTM_cols - 1)
				{
					int x_i = i, x_j = j;
					int left_distance = j - left;
					int right_distance = right - j;
					if (left_distance < right_distance)
					{
						x_i = i; x_j = left;
					}
					else
					{
						x_i = i; x_j = right;
					}
					mapped_phase.at<double>(i, j) = mapped_phase.at<double>(x_i, x_j);
					continue;
				}
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < UTM_rows; i++)
		{
			for (int j = 0; j < UTM_cols; j++)
			{
				if (b_filled.at<uchar>(i, j) != 0) continue;
				int up, down, left, right;
				// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
				double value1, value2, ratio1, ratio2;
				//寻找上面有值的点
				up = i;
				while (true)
				{
					up--;
					if (up < 0) break;
					if (b_filled.at<uchar>(up, j) != 0) break;
				}
				//寻找下面有值的点
				down = i;
				while (true)
				{
					down++;
					if (down > UTM_rows - 1) break;
					if (b_filled.at<uchar>(down, j) != 0) break;
				}
				//寻找左边有值的点
				left = j;
				while (true)
				{
					left--;
					if (left < 0) break;
					if (b_filled.at<uchar>(i, left) != 0) break;
				}
				//寻找右边有值的点
				right = j;
				while (true)
				{
					right++;
					if (right > UTM_cols - 1) break;
					if (b_filled.at<uchar>(i, right) != 0) break;
				}

				//上下左右都有值
				if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
				{

					ratio1 = double(j - left) / double(right - left);
					value1 = double(mapped_phase.at<double>(i, left)) +
						double(mapped_phase.at<double>(i, right) - mapped_phase.at<double>(i, right)) * ratio1;
					ratio2 = double(i - up) / double(down - up);
					value2 = double(mapped_phase.at<double>(up, j)) +
						double(mapped_phase.at<double>(down, j) - mapped_phase.at<double>(up, j)) * ratio2;
					mapped_phase.at<double>(i, j) = (value1 + value2) / 2.0;
					continue;
				}
				//上下有值
				if (up >= 0 && down <= UTM_rows - 1)
				{
					ratio2 = double(i - up) / double(down - up);
					value2 = double(mapped_phase.at<double>(up, j)) +
						double(mapped_phase.at<double>(down, j) - mapped_phase.at<double>(up, j)) * ratio2;
					mapped_phase.at<double>(i, j) = value2;
					continue;
				}
				//左右有值
				if (left >= 0 && right <= UTM_cols - 1)
				{
					ratio1 = double(j - left) / double(right - left);
					value1 = double(mapped_phase.at<double>(i, left)) +
						double(mapped_phase.at<double>(i, right) - mapped_phase.at<double>(i, right)) * ratio1;
					mapped_phase.at<double>(i, j) = value1;
					continue;
				}
			}
		}
	}


	//cv::flip(mapped_phase, mapped_phase, 0);
	return 0;
}

int Utils::SAR2UTM(
	Mat& mapped_lon,
	Mat& mapped_lat,
	ComplexMat& slc, 
	ComplexMat& mapped_slc, 
	int interpolation_method,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south
)
{
	if (mapped_lat.size() != mapped_lon.size() ||
		mapped_lat.size() != slc.re.size() ||
		mapped_lat.size() != slc.im.size() ||
		slc.GetRows() < 2 ||
		slc.GetCols() < 2 ||
		(slc.type() != CV_16S && slc.type() != CV_32F) ||
		mapped_lat.type() != mapped_lon.type() ||
		(mapped_lon.type() != CV_32F && mapped_lon.type() != CV_64F)
		)
	{
		fprintf(stderr, "SAR2UTM(): input check failed!\n");
		return -1;
	}
	//确定经纬度覆盖范围
	double max_lon, min_lon, max_lat, min_lat;
	cv::minMaxLoc(mapped_lon, &min_lon, &max_lon);
	cv::minMaxLoc(mapped_lat, &min_lat, &max_lat);
	double west = max_lon - min_lon > 180.0 ? max_lon : min_lon;
	if (lon_west) *lon_west = west;
	//确定经纬度采样间隔
	double lon_interval, lat_interval;
	Mat temp1, temp2;
	mapped_lon(cv::Range(0, mapped_lon.rows - 1), cv::Range(0, mapped_lon.cols)).copyTo(temp1);
	mapped_lon(cv::Range(1, mapped_lon.rows), cv::Range(0, mapped_lon.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = temp1 / 180.0 * PI;
	wrap(temp1, temp1);
	temp1 = temp1 / PI * 180.0;
	temp1 = cv::abs(temp1);
	lon_interval = cv::mean(temp1)[0];
	double sigma1, sigma2;
	std(temp1, &sigma1);

	mapped_lon(cv::Range(0, mapped_lon.rows), cv::Range(0, mapped_lon.cols - 1)).copyTo(temp1);
	mapped_lon(cv::Range(0, mapped_lon.rows), cv::Range(1, mapped_lon.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = temp1 / 180.0 * PI;
	wrap(temp1, temp1);
	temp1 = temp1 / PI * 180.0;
	temp1 = cv::abs(temp1);
	std(temp1, &sigma2);
	lon_interval = (cv::mean(temp1)[0] + fabs(lon_interval)) / 2.0;
	lon_interval = lon_interval + 1.0 * (sigma1 + sigma2) / 2.0;

	mapped_lat(cv::Range(0, mapped_lat.rows - 1), cv::Range(0, mapped_lat.cols)).copyTo(temp1);
	mapped_lat(cv::Range(1, mapped_lat.rows), cv::Range(0, mapped_lat.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = cv::abs(temp1);
	lat_interval = cv::mean(temp1)[0];
	std(temp1, &sigma1);

	mapped_lat(cv::Range(0, mapped_lat.rows), cv::Range(0, mapped_lat.cols - 1)).copyTo(temp1);
	mapped_lat(cv::Range(0, mapped_lat.rows), cv::Range(1, mapped_lat.cols)).copyTo(temp2);
	temp1 = temp2 - temp1;
	temp1 = cv::abs(temp1);
	lat_interval = (cv::mean(temp1)[0] + lat_interval) / 2.0;
	std(temp1, &sigma2);
	lat_interval = lat_interval + 1.0 * (sigma1 + sigma2) / 2.0;

	int rows = slc.GetRows(); int cols = slc.GetCols();

	
	
	//计算UTM坐标系相位尺寸
	int UTM_rows = static_cast<int>((max_lat - min_lat) / lat_interval);
	UTM_rows += 2;
	double south = max_lat - (double)(UTM_rows - 1) * lat_interval;
	double north = max_lat;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = south;
	double max_lon_temp = max_lon - min_lon;
	max_lon_temp = max_lon_temp > 180.0 ? 360.0 - max_lon_temp : max_lon_temp;
	int UTM_cols = static_cast<int>(max_lon_temp / lon_interval);
	UTM_cols += 2;
	double east = west + (double)(UTM_cols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	Mat b_filled(UTM_rows, UTM_cols, CV_8U); b_filled = 0;
	mapped_slc.re.create(UTM_rows, UTM_cols, slc.type()); mapped_slc.im.create(UTM_rows, UTM_cols, slc.type());
	mapped_slc.re = 0;
	mapped_slc.im = 0;
	//开始地理编码
	if (slc.type() == CV_16S)
	{
		if (mapped_lon.type() == CV_32F)
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					double lon, lat;
					int row, col;
					lon = mapped_lon.at<float>(i, j);
					lat = mapped_lat.at<float>(i, j);
					row = (int)round((lat - min_lat) / lat_interval);
					lon = fabs(lon - west);
					lon = lon > 180.0 ? 360.0 - lon : lon;
					col = (int)round(lon / lon_interval);
					mapped_slc.re.at<short>(row, col) = slc.re.at<short>(i, j);
					mapped_slc.im.at<short>(row, col) = slc.im.at<short>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
		else
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					double lon, lat;
					int row, col;
					lon = mapped_lon.at<double>(i, j);
					lat = mapped_lat.at<double>(i, j);
					row = (int)round((lat - min_lat) / lat_interval);
					lon = fabs(lon - west);
					lon = lon > 180.0 ? 360.0 - lon : lon;
					col = (int)round(lon / lon_interval);
					mapped_slc.re.at<short>(row, col) = slc.re.at<short>(i, j);
					mapped_slc.im.at<short>(row, col) = slc.im.at<short>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
	}
	else
	{
		if (mapped_lon.type() == CV_32F)
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					double lon, lat;
					int row, col;
					lon = mapped_lon.at<float>(i, j);
					lat = mapped_lat.at<float>(i, j);
					row = (int)round((lat - min_lat) / lat_interval);
					lon = fabs(lon - west);
					lon = lon > 180.0 ? 360.0 - lon : lon;
					col = (int)round(lon / lon_interval);
					mapped_slc.re.at<float>(row, col) = slc.re.at<float>(i, j);
					mapped_slc.im.at<float>(row, col) = slc.im.at<float>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
		else
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					double lon, lat;
					int row, col;
					lon = mapped_lon.at<double>(i, j);
					lat = mapped_lat.at<double>(i, j);
					row = (int)round((lat - min_lat) / lat_interval);
					lon = fabs(lon - west);
					lon = lon > 180.0 ? 360.0 - lon : lon;
					col = (int)round(lon / lon_interval);
					mapped_slc.re.at<float>(row, col) = slc.re.at<float>(i, j);
					mapped_slc.im.at<float>(row, col) = slc.im.at<float>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
	}
	

	//插值
	if (interpolation_method == 0)
	{
		if (slc.type() == CV_16S)
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count, value1, value2, ratio1, ratio2 (nearest-neighbor method)
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) != 0) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) != 0) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) != 0) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) != 0) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						int left_distance = j - left;
						int right_distance = right - j;
						if (down_distance < up_distance && down_distance < left_distance && down_distance < right_distance)
						{
							x_i = down; x_j = j;
						}
						else if (up_distance < down_distance && up_distance < left_distance && up_distance < right_distance)
						{
							x_i = up; x_j = j;
						}
						else if (left_distance < down_distance && left_distance < up_distance && left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(x_i, x_j);
						mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(x_i, x_j);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						if (down_distance < up_distance)
						{
							x_i = down; x_j = j;
						}
						else
						{
							x_i = down; x_j = j;
						}
						mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(x_i, x_j);
						mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(x_i, x_j);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						int x_i = i, x_j = j;
						int left_distance = j - left;
						int right_distance = right - j;
						if (left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(x_i, x_j);
						mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(x_i, x_j);
						continue;
					}
				}
			}
		}
		else
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
					// removed unused: double value1, value2, ratio1, ratio2;
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) != 0) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) != 0) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) != 0) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) != 0) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						int left_distance = j - left;
						int right_distance = right - j;
						if (down_distance < up_distance && down_distance < left_distance && down_distance < right_distance)
						{
							x_i = down; x_j = j;
						}
						else if (up_distance < down_distance && up_distance < left_distance && up_distance < right_distance)
						{
							x_i = up; x_j = j;
						}
						else if (left_distance < down_distance && left_distance < up_distance && left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(x_i, x_j);
						mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(x_i, x_j);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						if (down_distance < up_distance)
						{
							x_i = down; x_j = j;
						}
						else
						{
							x_i = down; x_j = j;
						}
						mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(x_i, x_j);
						mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(x_i, x_j);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						int x_i = i, x_j = j;
						int left_distance = j - left;
						int right_distance = right - j;
						if (left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(x_i, x_j);
						mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(x_i, x_j);
						continue;
					}
				}
			}
		}
		
	}
	else
	{
		if (slc.type() == CV_16S)
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
					double value1, value2, ratio1, ratio2;
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) != 0) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) != 0) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) != 0) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) != 0) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{

						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<short>(i, left)) +
							double(mapped_slc.re.at<short>(i, right) - mapped_slc.re.at<short>(i, right)) * ratio1;
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<short>(up, j)) +
							double(mapped_slc.re.at<short>(down, j) - mapped_slc.re.at<short>(up, j)) * ratio2;
						mapped_slc.re.at<short>(i, j) = static_cast<short>((value1 + value2) / 2.0);

						value1 = double(mapped_slc.im.at<short>(i, left)) +
							double(mapped_slc.im.at<short>(i, right) - mapped_slc.im.at<short>(i, right)) * ratio1;
						value2 = double(mapped_slc.im.at<short>(up, j)) +
							double(mapped_slc.im.at<short>(down, j) - mapped_slc.im.at<short>(up, j)) * ratio2;
						mapped_slc.im.at<short>(i, j) = static_cast<short>((value1 + value2) / 2.0);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<short>(up, j)) +
							double(mapped_slc.re.at<short>(down, j) - mapped_slc.re.at<short>(up, j)) * ratio2;
						mapped_slc.re.at<short>(i, j) = static_cast<short>(value2);

						value2 = double(mapped_slc.im.at<short>(up, j)) +
							double(mapped_slc.im.at<short>(down, j) - mapped_slc.im.at<short>(up, j)) * ratio2;
						mapped_slc.im.at<short>(i, j) = static_cast<short>(value2);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<short>(i, left)) +
							double(mapped_slc.re.at<short>(i, right) - mapped_slc.re.at<short>(i, right)) * ratio1;
						mapped_slc.re.at<short>(i, j) = static_cast<short>(value1);

						value1 = double(mapped_slc.im.at<short>(i, left)) +
							double(mapped_slc.im.at<short>(i, right) - mapped_slc.im.at<short>(i, right)) * ratio1;
						mapped_slc.im.at<short>(i, j) = static_cast<short>(value1);
						continue;
					}
				}
			}
		}
		else
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
					double value1, value2, ratio1, ratio2;
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) != 0) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) != 0) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) != 0) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) != 0) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{

						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<float>(i, left)) +
							double(mapped_slc.re.at<float>(i, right) - mapped_slc.re.at<float>(i, right)) * ratio1;
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<float>(up, j)) +
							double(mapped_slc.re.at<float>(down, j) - mapped_slc.re.at<float>(up, j)) * ratio2;
						mapped_slc.re.at<float>(i, j) = static_cast<float>((value1 + value2) / 2.0);

						value1 = double(mapped_slc.im.at<float>(i, left)) +
							double(mapped_slc.im.at<float>(i, right) - mapped_slc.im.at<float>(i, right)) * ratio1;
						value2 = double(mapped_slc.im.at<float>(up, j)) +
							double(mapped_slc.im.at<float>(down, j) - mapped_slc.im.at<float>(up, j)) * ratio2;
						mapped_slc.im.at<float>(i, j) = static_cast<float>((value1 + value2) / 2.0);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<float>(up, j)) +
							double(mapped_slc.re.at<float>(down, j) - mapped_slc.re.at<float>(up, j)) * ratio2;
						mapped_slc.re.at<float>(i, j) = static_cast<float>(value2);

						value2 = double(mapped_slc.im.at<float>(up, j)) +
							double(mapped_slc.im.at<float>(down, j) - mapped_slc.im.at<float>(up, j)) * ratio2;
						mapped_slc.im.at<float>(i, j) = static_cast<float>(value2);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<float>(i, left)) +
							double(mapped_slc.re.at<float>(i, right) - mapped_slc.re.at<float>(i, right)) * ratio1;
						mapped_slc.re.at<float>(i, j) = static_cast<float>(value1);

						value1 = double(mapped_slc.im.at<float>(i, left)) +
							double(mapped_slc.im.at<float>(i, right) - mapped_slc.im.at<float>(i, right)) * ratio1;
						mapped_slc.im.at<float>(i, j) = static_cast<float>(value1);
						continue;
					}
				}
			}
		}
	}


	cv::flip(mapped_slc.re, mapped_slc.re, 0);
	cv::flip(mapped_slc.im, mapped_slc.im, 0);
	return 0;
}

int Utils::geocode(
	Mat& DEM84,
	Mat& input,
	double mapped_resolution_x,
	double mapped_resolution_y,
	Mat& mapped_result,
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
	double lon_spacing,
	double lat_spacing,
	double* lon_east,
	double* lon_west,
	double* lat_north, 
	double* lat_south
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_16S ||
		(input.type() != CV_32F && input.type() != CV_64F)||
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
		fprintf(stderr, "geocode(): input check failed!\n");
		return -1;
	}
	//计算DEM插值倍数
	int interp_times_x, interp_times_y;
	double a = 6378137, b = 6356752;
	double C_short = (a + b) * PI;//经线一圈长度
	double C_long = a * 2 * PI;
	double lon_per_meter = 360.0 / C_short;//经线上每米多少度
	double lat_per_meter = 360.0 / (C_long * cos(lat_upperleft / 180.0 * PI));//纬线上每米多少度
	interp_times_x = static_cast<int>(lon_spacing / lon_per_meter / mapped_resolution_x);
	interp_times_y = static_cast<int>(lat_spacing / lat_per_meter / mapped_resolution_y);
	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times_x = interp_times_x < 1 ? 1 : interp_times_x;
	interp_times_y = interp_times_y < 1 ? 1 : interp_times_y;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times_x, DEM84.rows * interp_times_y));
	lon_spacing = lon_spacing / (double)interp_times_x;
	lat_spacing = lat_spacing / (double)interp_times_y;
	//初始化轨道类
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime);
	stateVectors.applyOrbit();
	// removed unused: ret (geocode loop uses inline computation, no H5/return calls)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	mapped_result.create(DEM.rows, DEM.cols, input.type()); mapped_result = 0.0;
	//地理编码
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
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
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
				continue;
			}
			int azimuthIndex = static_cast<int>(floor((zeroDopplerTime - acquisitionStartTime) / time_interval));
			int rangeIndex = static_cast<int>(floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing));
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 2 || rangeIndex < 0 || rangeIndex > sceneWidth - 2)
			{

			}
			else
			{
				//双线性插值计算
				double ratio_x = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing - floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
				double ratio_y = (zeroDopplerTime - acquisitionStartTime) / time_interval - floor((zeroDopplerTime - acquisitionStartTime) / time_interval);
				
				if (input.type() == CV_32F)
				{
					double upper = (double)input.at<float>(azimuthIndex, rangeIndex) + double(input.at<float>(azimuthIndex, rangeIndex + 1)
						- input.at<float>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)input.at<float>(azimuthIndex + 1, rangeIndex) + double(input.at<float>(azimuthIndex + 1, rangeIndex + 1)
						- input.at<float>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_result.at<float>(i, j) = static_cast<float>(upper + (lower - upper) * ratio_y);

					/*mapped_result.at<float>(i, j) = input.at<float>(azimuthIndex + 1, rangeIndex + 1) + input.at<float>(azimuthIndex + 1, rangeIndex) +
						input.at<float>(azimuthIndex, rangeIndex + 1) + input.at<float>(azimuthIndex, rangeIndex);
					mapped_result.at<float>(i, j) = mapped_result.at<float>(i, j) / 4.0;*/
				}
				else
				{
					double upper = (double)input.at<double>(azimuthIndex, rangeIndex) + double(input.at<double>(azimuthIndex, rangeIndex + 1)
						- input.at<double>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)input.at<double>(azimuthIndex + 1, rangeIndex) + double(input.at<double>(azimuthIndex + 1, rangeIndex + 1)
						- input.at<double>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_result.at<double>(i, j) = upper + (lower - upper) * ratio_y;

					/*mapped_result.at<double>(i, j) = input.at<double>(azimuthIndex + 1, rangeIndex + 1) + input.at<double>(azimuthIndex + 1, rangeIndex) +
						input.at<double>(azimuthIndex, rangeIndex + 1) + input.at<double>(azimuthIndex, rangeIndex);
					mapped_result.at<double>(i, j) = mapped_result.at<double>(i, j) / 4.0;*/
				}
			}
		}
	}
	if (lat_north && lat_south && lon_east && lon_west)
	{
		*lat_north = lat_upperleft;
		*lat_south = lat_upperleft - (double)(DEM.rows - 1) * lat_spacing;
		*lon_east = lon_upperleft;
		*lon_west = lon_upperleft + (double)(DEM.cols - 1) * lon_spacing;
		*lon_west = *lon_west > 180.0 ? (*lon_west - 360.0) : *lon_west;
	}
	return 0;
}

int Utils::geocode(
	Mat& DEM84,
	ComplexMat& slc,
	double mapped_resolution_x,
	double mapped_resolution_y,
	ComplexMat& mapped_slc, 
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
	double lon_spacing,
	double lat_spacing,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_16S ||
		(slc.type() != CV_32F && slc.type() != CV_16S) ||
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
		fprintf(stderr, "geocode(): input check failed!\n");
		return -1;
	}
	//计算DEM插值倍数
	int interp_times_x, interp_times_y;
	double a = 6378137, b = 6356752;
	double C_short = (a + b) * PI;//经线一圈长度
	double C_long = a * 2 * PI;
	double lon_per_meter = 360.0 / C_short;//经线上每米多少度
	double lat_per_meter = 360.0 / (C_long * cos(lat_upperleft / 180.0 * PI));//纬线上每米多少度

	interp_times_x = static_cast<int>(lon_spacing / lon_per_meter / mapped_resolution_x); 
	interp_times_y = static_cast<int>(lat_spacing / lat_per_meter / mapped_resolution_y); 
	//考虑DEM像素中心与边缘差异
	lat_upperleft = lat_upperleft + lat_spacing / 2.0 - lat_spacing / (double)interp_times_y * 0.5;                                                           
	lon_upperleft = lon_upperleft - lon_spacing / 2.0 + lon_spacing / (double)interp_times_x * 0.5;   

	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times_x = interp_times_x < 1 ? 1 : interp_times_x;
	interp_times_y = interp_times_y < 1 ? 1 : interp_times_y;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times_x, DEM84.rows * interp_times_y));
	lon_spacing = lon_spacing / (double)interp_times_x;
	lat_spacing = lat_spacing / (double)interp_times_y;
	//初始化轨道类
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime);
	stateVectors.applyOrbit();
	// removed unused: ret (geocode loop uses inline computation, no H5/return calls)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	mapped_slc.re.create(DEM.rows, DEM.cols, slc.type()); mapped_slc.re = 0.0;
	mapped_slc.im.create(DEM.rows, DEM.cols, slc.type()); mapped_slc.im = 0.0;
	//地理编码
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
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
			if (!Utils::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance, 0.01)) {
				continue;
			}
			int azimuthIndex = static_cast<int>(floor((zeroDopplerTime - acquisitionStartTime) / time_interval));
			int rangeIndex = static_cast<int>(floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing));
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 2 || rangeIndex < 0 || rangeIndex > sceneWidth - 2)
			{

			}
			else
			{
				//双线性插值计算
				double ratio_x = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing - floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
				double ratio_y = (zeroDopplerTime - acquisitionStartTime) / time_interval - floor((zeroDopplerTime - acquisitionStartTime) / time_interval);
				if (slc.type() == CV_32F)
				{
					double upper = (double)slc.re.at<float>(azimuthIndex, rangeIndex) + double(slc.re.at<float>(azimuthIndex, rangeIndex + 1)
						- slc.re.at<float>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)slc.re.at<float>(azimuthIndex + 1, rangeIndex) + double(slc.re.at<float>(azimuthIndex + 1, rangeIndex + 1)
						- slc.re.at<float>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.re.at<float>(i, j) = static_cast<float>(upper + (lower - upper) * ratio_y);

					upper = (double)slc.im.at<float>(azimuthIndex, rangeIndex) + double(slc.im.at<float>(azimuthIndex, rangeIndex + 1)
						- slc.im.at<float>(azimuthIndex, rangeIndex)) * ratio_x;
					lower = (double)slc.im.at<float>(azimuthIndex + 1, rangeIndex) + double(slc.im.at<float>(azimuthIndex + 1, rangeIndex + 1)
						- slc.im.at<float>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.im.at<float>(i, j) = static_cast<float>(upper + (lower - upper) * ratio_y);

					/*mapped_slc.im.at<float>(i, j) = slc.im.at<float>(azimuthIndex + 1, rangeIndex + 1) + slc.im.at<float>(azimuthIndex + 1, rangeIndex) +
						slc.im.at<float>(azimuthIndex, rangeIndex + 1) + slc.im.at<float>(azimuthIndex, rangeIndex);
					mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(i, j) / 4.0;

					mapped_slc.re.at<float>(i, j) = slc.re.at<float>(azimuthIndex + 1, rangeIndex + 1) + slc.re.at<float>(azimuthIndex + 1, rangeIndex) +
						slc.re.at<float>(azimuthIndex, rangeIndex + 1) + slc.re.at<float>(azimuthIndex, rangeIndex);
					mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(i, j) / 4.0;*/
				}
				else
				{
					double upper = (double)slc.re.at<short>(azimuthIndex, rangeIndex) + double(slc.re.at<short>(azimuthIndex, rangeIndex + 1)
						- slc.re.at<short>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)slc.re.at<short>(azimuthIndex + 1, rangeIndex) + double(slc.re.at<short>(azimuthIndex + 1, rangeIndex + 1)
						- slc.re.at<short>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.re.at<short>(i, j) = static_cast<short>(upper + (lower - upper) * ratio_y);

					upper = (double)slc.im.at<short>(azimuthIndex, rangeIndex) + double(slc.im.at<short>(azimuthIndex, rangeIndex + 1)
						- slc.im.at<short>(azimuthIndex, rangeIndex)) * ratio_x;
					lower = (double)slc.im.at<short>(azimuthIndex + 1, rangeIndex) + double(slc.im.at<short>(azimuthIndex + 1, rangeIndex + 1)
						- slc.im.at<short>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.im.at<short>(i, j) = static_cast<short>(upper + (lower - upper) * ratio_y);

					/*mapped_slc.im.at<short>(i, j) = slc.im.at<short>(azimuthIndex + 1, rangeIndex + 1) + slc.im.at<short>(azimuthIndex + 1, rangeIndex) +
						slc.im.at<short>(azimuthIndex, rangeIndex + 1) + slc.im.at<short>(azimuthIndex, rangeIndex);
					mapped_slc.im.at<short>(i, j) = double(mapped_slc.im.at<short>(i, j)) / 4.0;

					mapped_slc.re.at<short>(i, j) = slc.re.at<short>(azimuthIndex + 1, rangeIndex + 1) + slc.re.at<short>(azimuthIndex + 1, rangeIndex) +
						slc.re.at<short>(azimuthIndex, rangeIndex + 1) + slc.re.at<short>(azimuthIndex, rangeIndex);
					mapped_slc.re.at<short>(i, j) = double(mapped_slc.re.at<short>(i, j)) / 4.0;*/
				}
			}
		}
	}
	if (lat_north && lat_south && lon_east && lon_west)
	{
		*lat_north = lat_upperleft;
		*lat_south = lat_upperleft - (double)(DEM.rows - 1) * lat_spacing;
		*lon_west = lon_upperleft;
		*lon_east = lon_upperleft + (double)(DEM.cols - 1) * lon_spacing;
		*lon_east = *lon_east > 180.0 ? (*lon_east - 360.0) : *lon_east;
	}
	return 0;
}

int Utils::geo_transformation(
	const char* grille_file,
	Mat DTM, double xllcorner,
	double yllcorner,
	Mat& prior_DTM,
	Mat& mapped_DTM,
	int SAR_extent_x,
	int SAR_extent_y,
	NewtonProgressCallback cb
)
{
	if (cb && !cb(0, "Preparing geographic transformation...")) return -2;
	if (!grille_file || DTM.empty() || xllcorner < 0 || yllcorner < 0)
	{
		fprintf(stderr, "geo_transformation(): input check failed!\n");
		return -1;
	}
	int ret;
	if (DTM.type() != CV_64F)
	{
		DTM.convertTo(DTM, CV_64F);
	}
	Mat row_matrix, col_matrix, utm_x, utm_y;
	vector<Mat> lon_matrix;
	vector<Mat> lat_matrix;
	vector<double> height_vector;
	mapped_DTM.create(SAR_extent_y, SAR_extent_x, CV_64F);
	mapped_DTM = -1.0;
	ret = read_grille(grille_file, row_matrix, col_matrix, lon_matrix, lat_matrix, height_vector);
	if (return_check(ret, "read_grille()", error_head)) return -1;
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\row_matrix.bin", row_matrix);
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\col_matrix.bin", col_matrix);
	//经纬度转UTM
	for (int i = 0; i < height_vector.size(); i++)
	{
		ret = lonlat2utm(lon_matrix[i], lat_matrix[i], utm_x, utm_y);
		if (return_check(ret, "lonlat2utm()", error_head)) return -1;
		//conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5");
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "X", utm_x);
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "Y", utm_y);
		utm_x.copyTo(lon_matrix[i]);
		utm_y.copyTo(lat_matrix[i]);
	}
	int DTM_rows = DTM.rows;
	int DTM_cols = DTM.cols;
	//DTM逐点转换
	Mat DTM_mapped_X, DTM_mapped_Y;
	DTM_mapped_X = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_Y = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_X = -1;
	DTM_mapped_Y = -1;
	std::atomic<int> count(0);
	std::atomic<bool> cancel_flag(false);
	Mat mask = Mat::zeros(DTM_rows, DTM_cols, CV_8UC1);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < SAR_extent_y; i++)
	{
		if (cancel_flag.load(std::memory_order_relaxed)) continue;
		if (cb && i % 16 == 0 && !cb(i * 100 / std::max(1, SAR_extent_y), "Transforming geographic grid...")) {
			cancel_flag.store(true, std::memory_order_relaxed);
			continue;
		}
		for (int j = 0; j < SAR_extent_x; j++)
		{
			//首先确定DTM值是否有效
			double h = prior_DTM.at<double>(i, j);
			//double h = 20.6;
			if (h < -9000)
			{
				continue;
			}
			mask.at<uchar>(i, j) = 1;
			//定位相邻的网格层
			int low_ix, high_ix;
			for (int k = 0; k < height_vector.size() - 1; k++)
			{
				if (h >= height_vector[k] && h <= height_vector[k + 1])
				{
					low_ix = k;
					high_ix = k + 1;
					//对下层网格寻找定位点
					bool located1 = false;
					for (int ii = 0; ii < row_matrix.rows - 1; ii++)
					{
						for (int jj = 0; jj < row_matrix.cols - 1; jj++)
						{
							//located1 = false; located2 = false;
							//判断该点是否在四个点中间
							double Ax, Ay, Bx, Dy;
							// removed unused: Mx, My, By, Cx, Cy, Dx (only Ax/Ay/Bx/Dy used for bounding box check)
							Ax = row_matrix.at<double>(ii, jj); Ay = col_matrix.at<double>(ii, jj);
							Bx = row_matrix.at<double>(ii + 1, jj); Dy = col_matrix.at<double>(ii, jj + 1);

							if (i >= Ax && 
								i <= Bx &&
								j >= Ay &&
								j <= Dy &&
								!located1
								)
							{
								located1 = true;
								//线性插值得到在下层网格上的UTM坐标
								//UTM_x插值
								double UTM_x_upleft = lon_matrix[low_ix].at<double>(ii, jj);
								double UTM_x_upright = lon_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_x_lowleft = lon_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_x_lowright = lon_matrix[low_ix].at<double>(ii + 1, jj + 1);
								double upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) * 
									(j - col_matrix.at<double>(ii, jj));
								double lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								double UTM_y_upleft = lat_matrix[low_ix].at<double>(ii, jj);
								double UTM_y_upright = lat_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_y_lowleft = lat_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_y_lowright = lat_matrix[low_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//线性插值得到在上层网格上的UTM坐标
								//UTM_x插值
								UTM_x_upleft = lon_matrix[high_ix].at<double>(ii, jj);
								UTM_x_upright = lon_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_x_lowleft = lon_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_x_lowright = lon_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								UTM_y_upleft = lat_matrix[high_ix].at<double>(ii, jj);
								UTM_y_upright = lat_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_y_lowleft = lat_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_y_lowright = lat_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));



								//上下两层之间插值得到UTM_x和UTM_y
								double UTM_x_final = UTM_x_final_lower + (UTM_x_final_higher - UTM_x_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);
								double UTM_y_final = UTM_y_final_lower + (UTM_y_final_higher - UTM_y_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);



								//通过插值得到的UTM_x和UTM_y再次插值得到DTM
								if (UTM_x_final >= xllcorner &&
									UTM_x_final < (xllcorner + DTM_cols - 1) &&
									UTM_y_final >= yllcorner &&
									UTM_y_final < (yllcorner + DTM_rows - 1)
									)
								{
									int row = static_cast<int>(floor((yllcorner + DTM_rows - 1) - UTM_y_final));
									int col = static_cast<int>(floor(UTM_x_final - xllcorner));
									if (DTM.at<double>(row, col) > -10 &&
										DTM.at<double>(row + 1, col) > -10 &&
										DTM.at<double>(row + 1, col + 1) > -10 &&
										DTM.at<double>(row, col + 1) > -10
										)
									{
										upper = DTM.at<double>(row, col) + (DTM.at<double>(row, col + 1) - DTM.at<double>(row, col)) / 1 * ((yllcorner + DTM_rows - 1) - UTM_y_final - row);
										lower = DTM.at<double>(row + 1, col) + (DTM.at<double>(row + 1, col + 1) - DTM.at<double>(row + 1, col)) / 1 *
											((yllcorner + DTM_rows - 1) - UTM_y_final - row);
										mapped_DTM.at<double>(i, j) = lower + (upper - lower) / 1 * (UTM_x_final - xllcorner - col);
									}
									
								}
								break;
							}
						}
						if (located1)break;
					}
				}
			}
		}
		count++;
		if (count % 10 == 0)
		{
			printf("\r估计进度1：%lf%%", double(count) / double(SAR_extent_y) * 100.0);
			fflush(stdout);
		}
	}
	
//	conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "X", DTM_mapped_X);
//	conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "Y", DTM_mapped_Y);
//
//	conversion.read_array_from_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "X", DTM_mapped_X);
//	conversion.read_array_from_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "Y", DTM_mapped_Y);
//
//	
//	//最邻近插值法得到SAR坐标系DTM
////#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < DTM_rows; i++)
//	{
//		for (int j = 0; j < DTM_cols; j++)
//		{
//			if (DTM_mapped_X.at<double>(i, j) > 0 && DTM_mapped_Y.at<double>(i, j) > 0 && DTM.at<double>(i, j) > -1)
//			{
//				int col = round(DTM_mapped_Y.at<double>(i, j));
//				int row = round(DTM_mapped_X.at<double>(i, j));
//				mapped_DTM.at<double>(row, col) = DTM.at<double>(i, j);
//			}
//		}
//	}
	
//	//插值得到SAR坐标系的DTM
//	
//	double minX1, maxX1, minY1, maxY1;
//	cv::minMaxIdx(DTM_mapped_X, &minX1, &maxX1, NULL, NULL, mask);
//	cv::minMaxIdx(DTM_mapped_Y, &minY1, &maxY1, NULL, NULL, mask);
//	int start_y = floor(minY1) < 0 ? 0 : floor(minY1);
//	int end_y = ceil(maxY1);
//	int start_x = floor(minX1) < 0 ? 0 : floor(minX1);
//	int end_x = ceil(maxX1);
//	count = 0;
//#pragma omp parallel for schedule(guided)
//	for (int i = start_y; i < end_y; i++)
//	{
//		for (int j = start_x; j < end_x; j++)
//		{
//			bool located = false;
//			for (int ii = 0; ii < DTM_rows - 1; ii++)
//			{
//				for (int jj = 0; jj < DTM_cols - 1; jj++)
//				{
//					Mat X_tmp = Mat::zeros(1, 4, CV_64F), Y_tmp = Mat::zeros(1, 4, CV_64F);
//					X_tmp.at<double>(0, 0) = DTM_mapped_X.at<double>(ii, jj); Y_tmp.at<double>(0, 0) = DTM_mapped_Y.at<double>(ii, jj);
//					X_tmp.at<double>(0, 1) = DTM_mapped_X.at<double>(ii, jj + 1); Y_tmp.at<double>(0, 1) = DTM_mapped_Y.at<double>(ii, jj + 1);
//					X_tmp.at<double>(0, 2) = DTM_mapped_X.at<double>(ii + 1, jj + 1); Y_tmp.at<double>(0, 2) = DTM_mapped_Y.at<double>(ii + 1, jj + 1);
//					X_tmp.at<double>(0, 3) = DTM_mapped_X.at<double>(ii + 1, jj); Y_tmp.at<double>(0, 3) = DTM_mapped_Y.at<double>(ii + 1, jj);
//					double minX, minY, maxX, maxY;
//					cv::minMaxIdx(X_tmp, &minX, &maxX);
//					cv::minMaxIdx(Y_tmp, &minY, &maxY);
//					if (j >= minX &&
//						j <= maxX &&
//						i >= minY &&
//						i <= maxY
//						)
//					{
//						located = true;
//						//(反距离权重法插值)
//						double d1 = sqrt((j - DTM_mapped_X.at<double>(ii, jj)) * (j - DTM_mapped_X.at<double>(ii, jj)) +
//							(i - DTM_mapped_Y.at<double>(ii, jj)) * (i - DTM_mapped_Y.at<double>(ii, jj)));
//						double d2 = sqrt((j - DTM_mapped_X.at<double>(ii + 1, jj)) * (j - DTM_mapped_X.at<double>(ii + 1, jj)) +
//							(i - DTM_mapped_Y.at<double>(ii + 1, jj)) * (i - DTM_mapped_Y.at<double>(ii + 1, jj)));
//						double d3 = sqrt((j - DTM_mapped_X.at<double>(ii, jj + 1)) * (j - DTM_mapped_X.at<double>(ii, jj + 1)) +
//							(i - DTM_mapped_Y.at<double>(ii, jj + 1)) * (i - DTM_mapped_Y.at<double>(ii, jj + 1)));
//						double d4 = sqrt((j - DTM_mapped_X.at<double>(ii + 1, jj + 1)) * (j - DTM_mapped_X.at<double>(ii + 1, jj + 1)) +
//							(i - DTM_mapped_Y.at<double>(ii + 1, jj + 1)) * (i - DTM_mapped_Y.at<double>(ii + 1, jj + 1)));
//
//						mapped_DTM.at<double>(j, i) = (DTM.at<double>(ii, jj) / d1 + DTM.at<double>(ii + 1, jj) / d2 + DTM.at<double>(ii, jj + 1) / d3 + DTM.at<double>(ii + 1, jj + 1) / d4) /
//							(1 / d1 + 1 / d2 + 1 / d3 + 1 / d4);
//						break;
//					}
//				}
//				if (located)break;
//			}
//			count++;
//			if (count % 1 == 0)
//			{
//				printf("\r估计进度2：%lf%%", double(count) / double(end_y - start_y + 1) / double(end_x - start_x + 1) * 100.0);
//				fflush(stdout);
//			}
//		}
//		/*count++;
//		if (count % 10 == 0)
//		{
//			printf("\r估计进度2：%lf%%", double(count) / double(end_y - start_y + 1) * 100.0);
//			fflush(stdout);
//		}*/
//	}
	

	if (cancel_flag.load(std::memory_order_relaxed)) return -2;
	if (cb && !cb(100, "Geographic transformation complete.")) return -2;
	return 0;
}

int Utils::geo_transformation(
	const char* grille_file,
	Mat DTM,
	double lon_upleft,
	double lat_upleft,
	double lon_interval,
	double lat_interval,
	Mat& prior_DTM,
	Mat& mapped_DTM,
	int SAR_extent_x,
	int SAR_extent_y,
	NewtonProgressCallback cb
)
{
	if (cb && !cb(0, "Preparing geographic transformation...")) return -2;
	if (!grille_file || DTM.empty() || fabs(lon_upleft) > 180.0 || fabs(lat_upleft) > 90.0)
	{
		fprintf(stderr, "geo_transformation(): input check failed!\n");
		return -1;
	}
	int ret;
	if (DTM.type() != CV_64F)
	{
		DTM.convertTo(DTM, CV_64F);
	}
	Mat row_matrix, col_matrix, utm_x, utm_y;
	vector<Mat> lon_matrix;
	vector<Mat> lat_matrix;
	vector<double> height_vector;
	mapped_DTM.create(SAR_extent_y, SAR_extent_x, CV_64F);
	mapped_DTM = -1.0;
	ret = read_grille(grille_file, row_matrix, col_matrix, lon_matrix, lat_matrix, height_vector);
	if (return_check(ret, "read_grille()", error_head)) return -1;
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\row_matrix.bin", row_matrix);
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\col_matrix.bin", col_matrix);
	//经纬度转UTM
	for (int i = 0; i < height_vector.size(); i++)
	{
		ret = lonlat2utm(lon_matrix[i], lat_matrix[i], utm_x, utm_y);
		if (return_check(ret, "lonlat2utm()", error_head)) return -1;
		//conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5");
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "X", utm_x);
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "Y", utm_y);
		utm_x.copyTo(lon_matrix[i]);
		utm_y.copyTo(lat_matrix[i]);
	}

	int DTM_rows = DTM.rows;
	int DTM_cols = DTM.cols;
	//DTM逐点转换
	Mat DTM_mapped_X, DTM_mapped_Y;
	DTM_mapped_X = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_Y = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_X = -1;
	DTM_mapped_Y = -1;
	std::atomic<int> count(0);
	std::atomic<bool> cancel_flag(false);
	//Mat mask = Mat::zeros(DTM_rows, DTM_cols, CV_8UC1);
	InitializeGDALAndProjOnce();
	OGRSpatialReference monUtm;
	monUtm.SetWellKnownGeogCS("WGS84");
	monUtm.SetUTM(22, 1);
	OGRSpatialReference monGeo;
	monGeo.SetWellKnownGeogCS("WGS84");
	int max_threads = omp_get_max_threads();
	std::vector<OGRCoordinateTransformation*> coordTransList(max_threads, nullptr);
	for (int t = 0; t < max_threads; ++t)
	{
		coordTransList[t] = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
		if (coordTransList[t] == nullptr)
		{
			for (int k = 0; k < t; ++k) delete coordTransList[k];
			fprintf(stderr, "Error: OGRCreateCoordinateTransformation failed!\n");
			return -1;
		}
	}
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < SAR_extent_y; i++)
	{
		if (cancel_flag.load(std::memory_order_relaxed)) continue;
		if (cb && i % 16 == 0 && !cb(i * 100 / std::max(1, SAR_extent_y), "Transforming geographic grid...")) {
			cancel_flag.store(true, std::memory_order_relaxed);
			continue;
		}
		for (int j = 0; j < SAR_extent_x; j++)
		{
			//首先确定DTM值是否有效
			double h = prior_DTM.at<double>(i, j);
			//double h = 20.6;
			if (h < -9000)
			{
				continue;
			}
			//mask.at<uchar>(i, j) = 1;
			//定位相邻的网格层
			int low_ix, high_ix;
			for (int k = 0; k < height_vector.size() - 1; k++)
			{
				if (h >= height_vector[k] && h <= height_vector[k + 1])
				{
					low_ix = k;
					high_ix = k + 1;
					//对下层网格寻找定位点
					bool located1 = false;
					for (int ii = 0; ii < row_matrix.rows - 1; ii++)
					{
						for (int jj = 0; jj < row_matrix.cols - 1; jj++)
						{
							//located1 = false; located2 = false;
							//判断该点是否在四个点中间
							double Ax, Ay, Bx, Dy;
							Ax = row_matrix.at<double>(ii, jj); Ay = col_matrix.at<double>(ii, jj);
							Bx = row_matrix.at<double>(ii + 1, jj); Dy = col_matrix.at<double>(ii, jj + 1);

							if (i >= Ax &&
								i <= Bx &&
								j >= Ay &&
								j <= Dy &&
								!located1
								)
							{
								located1 = true;
								//对分层网格寻找下层定位点的UTM值
								//UTM_x插值
								double UTM_x_upleft = lon_matrix[low_ix].at<double>(ii, jj);
								double UTM_x_upright = lon_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_x_lowleft = lon_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_x_lowright = lon_matrix[low_ix].at<double>(ii + 1, jj + 1);
								double upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								double lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								double UTM_y_upleft = lat_matrix[low_ix].at<double>(ii, jj);
								double UTM_y_upright = lat_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_y_lowleft = lat_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_y_lowright = lat_matrix[low_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//对分层网格寻找上层定位点的UTM值
								//UTM_x插值
								UTM_x_upleft = lon_matrix[high_ix].at<double>(ii, jj);
								UTM_x_upright = lon_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_x_lowleft = lon_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_x_lowright = lon_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								UTM_y_upleft = lat_matrix[high_ix].at<double>(ii, jj);
								UTM_y_upright = lat_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_y_lowleft = lat_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_y_lowright = lat_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//高程插值得到UTM_x和UTM_y
								double UTM_x_final = UTM_x_final_lower + (UTM_x_final_higher - UTM_x_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);
								double UTM_y_final = UTM_y_final_lower + (UTM_y_final_higher - UTM_y_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);

								double lat_x, lon_y;
								lat_x = UTM_x_final;
								lon_y = UTM_y_final;
								int thread_num = omp_get_thread_num();
								int reprojected = coordTransList[thread_num]->Transform(1, &lat_x, &lon_y);

								//通过插值得到lat_x和lon_y后再插值得到DTM
								if (lat_x <= lat_upleft &&
									lat_x >= (lat_upleft - (DTM_rows - 1)*lat_interval) &&
									lon_y <= (lon_upleft + (DTM_cols - 1)*lon_interval) &&
									lon_y >= lon_upleft
									)
								{
									int row = static_cast<int>(floor((lat_upleft - lat_x) / lat_interval));
									double delta_row = ((lat_upleft - lat_x) / lat_interval - row);
									int col = static_cast<int>(floor((lon_y - lon_upleft) / lon_interval));
									double delta_col = ((lon_y - lon_upleft) / lon_interval - col);
									if (row < 0 || row >= DTM_rows - 1 || col < 0 || col >= DTM_cols - 1) continue;
									if (DTM.at<double>(row, col) > -1000 &&
										DTM.at<double>(row + 1, col) > -1000 &&
										DTM.at<double>(row + 1, col + 1) > -1000 &&
										DTM.at<double>(row, col + 1) > -1000
										)
									{
										upper = DTM.at<double>(row, col) + (DTM.at<double>(row, col + 1) - DTM.at<double>(row, col)) * delta_row;
										lower = DTM.at<double>(row + 1, col) + (DTM.at<double>(row + 1, col + 1) - DTM.at<double>(row + 1, col)) * delta_row;
										mapped_DTM.at<double>(i, j) = lower + (upper - lower) * delta_col;
									}

								}
								break;
							}
						}
						if (located1)break;
					}
				}
			}
		}
		count++;
		if (count % 10 == 0)
		{
			printf("\r估计进度1：%lf%%", double(count) / double(SAR_extent_y) * 100.0);
			fflush(stdout);
		}
	}
	for (int t = 0; t < max_threads; ++t)
	{
		if (coordTransList[t] != nullptr)
		{
			delete coordTransList[t];
		}
	}
	if (cancel_flag.load(std::memory_order_relaxed)) return -2;
	if (cb && !cb(100, "Geographic transformation complete.")) return -2;
	return 0;
}

int Utils::read_grille(
	const char* grille_file,
	Mat& row_matrix, 
	Mat& col_matrix,
	vector<Mat>& lon_matrix,
	vector<Mat>& lat_matrix, 
	vector<double>& height_vector
)
{
	if (!grille_file)
	{
		fprintf(stderr, "read_grille(): input check failed!\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(grille_file, "rt");
	if (!fp)
	{
		fprintf(stderr, "read_grille(): cannot open grille file!\n");
		return -1;
	}
	char data[1024];
	char* ptr;
	// removed unused: val (grille parsing uses strtol directly)
	fgets(data, 1024, fp);
	fgets(data, 1024, fp);
	fgets(data, 1024, fp);
	memset(data, 0, 1024);
	fgets(data, 1024, fp);
	int rows = strtol(data, &ptr, 10);
	fgets(data, 1024, fp);
	int cols = strtol(data, &ptr, 10);
	fgets(data, 1024, fp);
	int height_num = strtol(data, &ptr, 10);
	size_t total = rows * cols * height_num;
	Mat zeroMat = Mat::zeros(rows, cols, CV_64F);
	zeroMat.copyTo(row_matrix);
	zeroMat.copyTo(col_matrix);
	lon_matrix.resize(height_num);
	lat_matrix.resize(height_num);
	height_vector.resize(height_num);
	for (int i = 0; i < height_num; i++)
	{
		zeroMat.copyTo(lon_matrix[i]);
		zeroMat.copyTo(lat_matrix[i]);
	}
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			for (int k = 0; k < height_num; k++)
			{
				fgets(data, 1024, fp);
				row_matrix.at<double>(i, j) = strtod(data, &ptr);
				col_matrix.at<double>(i, j) = strtod(ptr, &ptr);
				height_vector[k] = strtod(ptr, &ptr);
				lon_matrix[k].at<double>(i, j) = strtod(ptr, &ptr);
				lat_matrix[k].at<double>(i, j) = strtod(ptr, &ptr);
			}
		}
	}
	fclose(fp);
	return 0;
}

int Utils::lonlat2utm(Mat lon, Mat lat, Mat& UTM_X, Mat& UTM_Y)
{
	if (lon.empty() || lat.empty() || lon.size() != lat.size() || lon.type() != lat.type() || lon.type() != CV_64F)
	{
		fprintf(stderr, "lonlat2utm(): input check failed!\n");
		return -1;
	}
	InitializeGDALAndProjOnce();
	OGRSpatialReference monUtm;
	monUtm.SetWellKnownGeogCS("WGS84");
	monUtm.SetUTM(22, true);

	OGRSpatialReference monGeo;
	monGeo.SetWellKnownGeogCS("WGS84");

	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monGeo, &monUtm);

	int nr = lon.rows;
	int nc = lon.cols;
	lon.copyTo(UTM_X);
	lon.copyTo(UTM_Y);
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double x, y;
			x = lat.at<double>(i, j);
			y = lon.at<double>(i, j);
			int reprojected = coordTrans->Transform(1, &x, &y);
			UTM_X.at<double>(i, j) = x;
			UTM_Y.at<double>(i, j) = y;
		}
	}
	delete coordTrans;
	return 0;
}

int Utils::read_LVIS(
	vector<string>& LVIS2_filelist,
	Mat& DTM,
	Mat& DSM,
	Mat& RH100,
	Mat& RH95,
	Mat& lat,
	Mat& lon
)
{
	if (LVIS2_filelist.empty())
	{
		fprintf(stderr, "read_LVIS(): input check failed!\n");
		return -1;
	}
	int n_files = static_cast<int>(LVIS2_filelist.size());
	int LVIS_cols = 42;
	vector<double> dtm, dsm, rh100, rh95, lo, la;
	FILE* fp = NULL;
	char str[1024];
	char* ret = NULL;
	char* ptr = NULL;
	double val;
	for (int i = 0; i < n_files; i++)
	{
		fp = fopen(LVIS2_filelist[i].c_str(), "rt");
		if (!fp)
		{
			fprintf(stderr, "read_LVIS(): cannot open %s!\n", LVIS2_filelist[i].c_str());
			return -1;
		}
		for (int j = 0; j < 18; j++)
		{
			ret = fgets(str, 1024, fp);
		}
		while (ret)
		{
			ret=fgets(str, 1024, fp);
			vector<double> tmp;
			ptr = str;
			for (int k = 0; k < LVIS_cols; k++)
			{
				val = strtod(ptr, &ptr);
				tmp.push_back(val);
			}
			lo.push_back(tmp[3]);
			la.push_back(tmp[4]);
			dtm.push_back(tmp[5]);
			dsm.push_back(tmp[11]);
			rh100.push_back(tmp[33]);
			rh95.push_back(tmp[28]);
			tmp.clear();
		}
		fclose(fp);
		fp = NULL;
	}
	DTM.create(static_cast<int>(dtm.size()), 1, CV_64F);
	DTM = 0;
	DTM.copyTo(DSM);
	DTM.copyTo(lon);
	DTM.copyTo(lat);
	DTM.copyTo(RH100);
	DTM.copyTo(RH95);
	for (int i = 0; i < dtm.size(); i++)
	{
		DTM.at<double>(i, 0) = dtm[i];
		DSM.at<double>(i, 0) = dsm[i];
		lon.at<double>(i, 0) = lo[i];
		lat.at<double>(i, 0) = la[i];
		RH100.at<double>(i, 0) = rh100[i];
		RH95.at<double>(i, 0) = rh95[i];
	}
	return 0;
}

int Utils::geo2sar_DLR(
	double east_min,
	double north_min,
	double east_max,
	double north_max,
	int projection_zone,
	double pixel_spacing,
	Mat& sr2geo_az,
	Mat& sr2geo_rg,
	Mat& sr2geo_h_ref,
	Mat& sr2geo3d_rg_o1,
	Mat& sr2geo3d_rg_o2,
	Mat& sr2geo3d_az_o1,
	Mat& sr2geo3d_az_o2,
	Mat& DTM,
	Mat& DTM_lon,
	Mat& DTM_lat,
	Mat& mapped_slc_rg,
	Mat& mapped_slc_az,
	Mat& mapped_h0
)
{
	if (pixel_spacing < 0 ||
		sr2geo_az.empty() || sr2geo_az.type() != CV_64F ||
		sr2geo_az.size() != sr2geo_rg.size() || sr2geo_az.size() != sr2geo_h_ref.size() || sr2geo_az.size() != sr2geo3d_rg_o1.size() || 
		sr2geo_az.size() != sr2geo3d_rg_o2.size() || sr2geo_az.size() != sr2geo3d_az_o1.size() || sr2geo_az.size() != sr2geo3d_az_o2.size() ||
		DTM.empty() || DTM.type() != CV_64F ||
		DTM.size() != DTM_lon.size() || DTM.size() != DTM_lat.size()
		)
	{
		fprintf(stderr, "geo2sar_DLR(): input check failed!\n");
		return -1;
	}
	//确定场景的经纬度范围
	Mat lon, lat;
	lon.create(4, 1, CV_64F); lon = 0.0; lon.copyTo(lat);
	InitializeGDALAndProjOnce();
	OGRSpatialReference monUtm;
	monUtm.SetWellKnownGeogCS("WGS84");
	monUtm.SetUTM(abs(projection_zone), projection_zone>0);
	OGRSpatialReference monGeo;
	monGeo.SetWellKnownGeogCS("WGS84");
	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
	double x, y;
	x = east_min; y = north_min;
	int reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(0, 0) = y; lat.at<double>(0, 0) = x;

	x = east_min; y = north_max;
	reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(1, 0) = y; lat.at<double>(1, 0) = x;

	x = east_max; y = north_min;
	reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(2, 0) = y; lat.at<double>(2, 0) = x;

	x = east_max; y = north_max;
	reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(3, 0) = y; lat.at<double>(3, 0) = x;

	double lon_min, lon_max, lat_min, lat_max;
	cv::minMaxLoc(lon, &lon_min, &lon_max);
	cv::minMaxLoc(lat, &lat_min, &lat_max);
	delete coordTrans;

	
	mapped_slc_rg.create(DTM.rows, DTM.cols, CV_64F); mapped_slc_rg = -1;
	mapped_slc_az.create(DTM.rows, DTM.cols, CV_64F); mapped_slc_az = -1;
	mapped_h0.create(DTM.rows, DTM.cols, CV_64F); mapped_h0 = -1;
	Mat UTM_x, UTM_y;
	DTM_lat.copyTo(UTM_x);
	DTM_lon.copyTo(UTM_y);
	OGRCoordinateTransformation* coordTrans2 = OGRCreateCoordinateTransformation(&monGeo , &monUtm);
	//将DTM的经纬度转换为UTM
	for (int i = 0; i < DTM.rows; i++)
	{
		for (int j = 0; j < DTM.cols; j++)
		{
			if (DTM.at<double>(i, j) < -9000.0) continue;
			double xx, yy;
			xx = DTM_lat.at<double>(i, j);
			yy = DTM_lon.at<double>(i, j);
			int reprojected = coordTrans2->Transform(1, &xx, &yy);
			UTM_x.at<double>(i, j) = xx;
			UTM_y.at<double>(i, j) = yy;
		}
	}
	delete coordTrans2;
	//开始投影
	for (int i = 0; i < DTM.rows; i++)
	{
		for (int j = 0; j < DTM.cols; j++)
		{
			if (DTM.at<double>(i, j) < -9000.0) continue;
			double utm_x, utm_y, h;
			utm_x = UTM_x.at<double>(i, j);
			utm_y = UTM_y.at<double>(i, j);
			h = DTM.at<double>(i, j);
			if (DTM_lon.at<double>(i, j) <= lon_min || DTM_lon.at<double>(i, j) >= lon_max ||
				DTM_lat.at<double>(i, j) <= lat_min || DTM_lat.at<double>(i, j) >= lat_max
				)
			{
				continue;
			}
			//插值得到2D地理编码下的距离方位坐标，参考DEM和3D地理编码系数
			double easting, northing, rg0, az0, h0, rg_o1, rg_o2, az_o1, az_o2, upper, lower;
			int row, col;
			easting = (utm_x - east_min) / pixel_spacing;
			northing = (utm_y - north_min) / pixel_spacing;

			row = sr2geo_h_ref.rows - 1 - int(floor(northing));
			col = int(floor(easting));
			if (row < 1 || row > sr2geo_h_ref.rows - 1 || col < 0 || col >= sr2geo_h_ref.cols - 1)
			{
				continue;
			}
			if (sr2geo_h_ref.at<double>(row, col) < -9000 ||
				sr2geo_h_ref.at<double>(row, col + 1) < -9000 ||
				sr2geo_h_ref.at<double>(row - 1, col) < -9000 ||
				sr2geo_h_ref.at<double>(row - 1, col + 1) < -9000
				)
			{
				continue;
			}
				


			lower = sr2geo_rg.at<double>(row, col) + (easting - double(col)) * (sr2geo_rg.at<double>(row, col + 1) - sr2geo_rg.at<double>(row, col));
			upper = sr2geo_rg.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo_rg.at<double>(row - 1, col + 1) - sr2geo_rg.at<double>(row - 1, col));
			rg0 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo_az.at<double>(row, col) + (easting - double(col)) * (sr2geo_az.at<double>(row, col + 1) - sr2geo_az.at<double>(row, col));
			upper = sr2geo_az.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo_az.at<double>(row - 1, col + 1) - sr2geo_az.at<double>(row - 1, col));
			az0 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo_h_ref.at<double>(row, col) + (easting - double(col)) * (sr2geo_h_ref.at<double>(row, col + 1) - sr2geo_h_ref.at<double>(row, col));
			upper = sr2geo_h_ref.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo_h_ref.at<double>(row - 1, col + 1) - sr2geo_h_ref.at<double>(row - 1, col));
			h0 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_rg_o1.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_rg_o1.at<double>(row, col + 1) - sr2geo3d_rg_o1.at<double>(row, col));
			upper = sr2geo3d_rg_o1.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_rg_o1.at<double>(row - 1, col + 1) - sr2geo3d_rg_o1.at<double>(row - 1, col));
			rg_o1 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_rg_o2.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_rg_o2.at<double>(row, col + 1) - sr2geo3d_rg_o2.at<double>(row, col));
			upper = sr2geo3d_rg_o2.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_rg_o2.at<double>(row - 1, col + 1) - sr2geo3d_rg_o2.at<double>(row - 1, col));
			rg_o2 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_az_o1.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_az_o1.at<double>(row, col + 1) - sr2geo3d_az_o1.at<double>(row, col));
			upper = sr2geo3d_az_o1.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_az_o1.at<double>(row - 1, col + 1) - sr2geo3d_az_o1.at<double>(row - 1, col));
			az_o1 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_az_o2.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_az_o2.at<double>(row, col + 1) - sr2geo3d_az_o2.at<double>(row, col));
			upper = sr2geo3d_az_o2.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_az_o2.at<double>(row - 1, col + 1) - sr2geo3d_az_o2.at<double>(row - 1, col));
			az_o2 = lower + (upper - lower) * (northing - floor(northing));

			double delta_h, rg_new, az_new;
			delta_h = h - h0;
			rg_new = rg0 + rg_o1 * delta_h + rg_o2 * delta_h * delta_h;
			az_new = az0 + az_o1 * delta_h + az_o2 * delta_h * delta_h;
	
			mapped_slc_rg.at<double>(i, j) = rg0;
			mapped_slc_az.at<double>(i, j) = az0;
			mapped_h0.at<double>(i, j) = h0;
		}
	}
	return 0;
}

// 根据经纬度获取大地水准面高差
double Utils::getGeoidHeight(const std::string& geoidFilePath, double lon, double lat) {

	// 注册 GDAL/PROJ 驱动与路径

	InitializeGDALAndProjOnce();



	// 打开 Geoid 文件

	GDALDataset* poDataset = (GDALDataset*)GDALOpen(geoidFilePath.c_str(), GA_ReadOnly);

	if (poDataset == nullptr) {

		std::cerr << "无法打开 Geoid 文件: " << geoidFilePath << std::endl;

		return 0.0;

	}



	// 获取第一个波段（Geoid 数据）

	GDALRasterBand* poBand = poDataset->GetRasterBand(1);

	if (poBand == nullptr) {

		std::cerr << "无法获取波段数据" << std::endl;

		GDALClose(poDataset);

		return 0.0;

	}



	// 获取 Geoid 文件的地理变换信息

	double adfGeoTransform[6];

	if (poDataset->GetGeoTransform(adfGeoTransform) != CE_None) {

		std::cerr << "无法获取地理变换信息" << std::endl;

		GDALClose(poDataset);

		return 0.0;

	}



	// 将经纬度转换为像素坐标

	double x = (lon - adfGeoTransform[0]) / adfGeoTransform[1];

	double y = (lat - adfGeoTransform[3]) / adfGeoTransform[5];



	// 插值获取 Geoid Height

	float geoidHeight = 0.0;

	if (poBand->RasterIO(GF_Read, static_cast<int>(x), static_cast<int>(y), 1, 1,

		&geoidHeight, 1, 1, GDT_Float32, 0, 0) != CE_None) {

		std::cerr << "无法读取 Geoid 数据" << std::endl;

		GDALClose(poDataset);

		return 0.0;

	}



	// 关闭数据集

	GDALClose(poDataset);



	return static_cast<double>(geoidHeight);

}


















tri_node::tri_node(int row, int col, int num_neigh_edge, double phi)
{
	this->rows = row;
	this->cols = col;
	this->phase = phi;
	this->b_unwrapped = false;
	this->b_residue = false;
	this->b_balanced = true;
	this->epsilon_height = 0.0;
	this->vel = 0.0;
	if (num_neigh_edge > 0)
	{
		this->neigh_edges.assign(num_neigh_edge, -1);
	}
}

int tri_node::get_phase(double* phi) const
{
	if (phi == NULL)
	{
		fprintf(stderr, "get_phase(): input check failed!\n\n");
		return -1;
	}
	*phi = this->phase;
	return 0;
}

int tri_node::get_pos(int* rows, int* cols) const
{
	if (rows == NULL ||
		cols == NULL)
	{
		fprintf(stderr, "tri_node::get_pos(): input check failed!\n\n");
		return -1;
	}
	*rows = this->rows;
	*cols = this->cols;
	return 0;
}

int tri_node::set_phase(double phi)
{
	this->phase = phi;
	return 0;
}

const std::vector<long>& tri_node::get_neigh_edges() const
{
	return this->neigh_edges;
}

int tri_node::add_neigh_edge(long edge_idx)
{
	for (auto& edge : this->neigh_edges)
	{
		if (edge == -1)
		{
			edge = edge_idx;
			return 0;
		}
	}
	return -1;
}

int tri_node::set_status(bool b_unwrapped)
{
	this->b_unwrapped = b_unwrapped;
	return 0;
}

int tri_node::set_balance(bool b_balanced)
{
	this->b_balanced = b_balanced;
	return 0;
}

int tri_node::print_neighbour() const
{
	if (this->neigh_edges.empty())
	{
		fprintf(stdout, "no neighbour edges!\n");
		return 0;
	}
	for (long edge : this->neigh_edges)
	{
		fprintf(stdout, "%ld ", edge);
	}
	fprintf(stdout, "\n");
	return 0;
}

int tri_node::get_num_neigh(int* num_neigh) const
{
	if (num_neigh == NULL)
	{
		return -1;
	}
	*num_neigh = static_cast<int>(this->neigh_edges.size());
	return 0;
}

int tri_node::get_distance(const tri_node& node, double* distance) const
{
	*distance = sqrt(((double)node.rows - (double)this->rows) * ((double)node.rows - (double)this->rows) +
		((double)node.cols - (double)this->cols) * ((double)node.cols - (double)this->cols));
	return 0;
}

bool tri_node::get_status() const
{
	return this->b_unwrapped;
}

bool tri_node::get_balance() const
{
	return this->b_balanced;
}

bool tri_node::is_residue_node() const
{
	return this->b_residue;
}

int tri_node::set_residue(bool b_res)
{
	this->b_residue = b_res;
	return 0;
}

double tri_node::get_vel() const
{
	return this->vel;
}

double tri_node::get_height() const
{
	return this->epsilon_height;
}

int tri_node::set_vel(double vel)
{
	this->vel = vel;
	return 0;
}

int tri_node::set_height(double height)
{
	this->epsilon_height = height;
	return 0;
}






