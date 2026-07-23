#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\tinyxml.h"
#include "gdal_priv.h"
#include "gdal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <mutex>
#include <SensAPI.h>
#include <string>
#include <vector>
#include <urlmon.h>
#include <windows.h>

#pragma comment(lib, "URlmon")
#pragma comment(lib, "Sensapi.lib")

using namespace cv;
using namespace std;

static void setupProjSearchPaths()
{
	const char* projData = getenv("PROJ_DATA");
	if (!projData) projData = getenv("PROJ_LIB");
	if (projData)
	{
		const char* path[] = { projData, nullptr };
		OSRSetPROJSearchPaths(path);
	}
}

static std::once_flag g_gdal_proj_init_flag;
void InitializeGDALAndProjOnce()
{
	std::call_once(g_gdal_proj_init_flag, []()
	{
		GDALAllRegister();
		setupProjSearchPaths();
	});
}

static int read_srtm_geotiff(const char* filename, Mat& outDEM)
{
	if (!filename) return -1;
	InitializeGDALAndProjOnce();
	GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpen(filename, GA_ReadOnly));
	if (!dataset) return -1;
	GDALRasterBand* band = dataset->GetRasterCount() == 1 ? dataset->GetRasterBand(1) : nullptr;
	if (!band || band->GetXSize() <= 0 || band->GetYSize() <= 0)
	{
		GDALClose(dataset);
		return -1;
	}
	const int columns = band->GetXSize();
	const int rows = band->GetYSize();
	outDEM.create(rows, columns, CV_16S);
	if (band->RasterIO(GF_Read, 0, 0, columns, rows, outDEM.data, columns, rows, GDT_Int16, 0, 0) != CE_None)
	{
		GDALClose(dataset);
		return -1;
	}
	GDALClose(dataset);
#pragma omp parallel for schedule(guided)
	for (int row = 0; row < outDEM.rows; ++row)
	{
		short* values = outDEM.ptr<short>(row);
		for (int column = 0; column < outDEM.cols; ++column)
			if (values[column] < 0) values[column] = 0;
	}
	return 0;
}

static int unzip_srtm_archive(const char* sourceFile, const char* destinationPath)
{
	if (!sourceFile || !destinationPath) return -1;
	if (GetFileAttributesA(destinationPath) == INVALID_FILE_ATTRIBUTES && _mkdir(destinationPath) < 0)
		return -1;
	char executablePath[MAX_PATH + 1] = {};
	if (!GetModuleFileNameA(nullptr, executablePath, MAX_PATH)) return -1;
	std::string executableDirectory(executablePath);
	const size_t separator = executableDirectory.rfind('\\');
	if (separator == std::string::npos) return -1;
	std::string commandLine = executableDirectory.substr(0, separator) + "\\unzip.exe " + sourceFile + " " + destinationPath;
	std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
	mutableCommand.push_back('\0');
	STARTUPINFOA startupInfo = {};
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags = STARTF_USESHOWWINDOW;
	startupInfo.wShowWindow = FALSE;
	PROCESS_INFORMATION processInfo = {};
	if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startupInfo, &processInfo))
		return -1;
	WaitForSingleObject(processInfo.hProcess, INFINITE);
	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	return 0;
}

int Utils::computeImageGeoBoundry(
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
	if (lon_coefficient.rows != 1 ||
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

int Utils::computeImageGeoBoundry(
	double topleft_lon,
	double topleft_lat,
	double topright_lon, 
	double topright_lat,
	double bottomleft_lon, 
	double bottomleft_lat, 
	double bottomright_lon,
	double bottomright_lat, 
	double* lonMax,
	double* latMax,
	double* lonMin,
	double* latMin
)
{
	Mat lon, lat;
	lon.create(1, 4, CV_64F);
	lat.create(1, 4, CV_64F);

	lon.at<double>(0, 0) = topleft_lon;
	lon.at<double>(0, 1) = topright_lon;
	lon.at<double>(0, 2) = bottomleft_lon;
	lon.at<double>(0, 3) = bottomright_lon;

	lat.at<double>(0, 0) = topleft_lat;
	lat.at<double>(0, 1) = topright_lat;
	lat.at<double>(0, 2) = bottomleft_lat;
	lat.at<double>(0, 3) = bottomright_lat;

	cv::minMaxLoc(lon, lonMin, lonMax);
	cv::minMaxLoc(lat, latMin, latMax);
	double extra = 5.0 / 6000;
	*lonMin = *lonMin - extra * 30;
	*lonMax = *lonMax + extra * 30;
	*latMin = *latMin - extra * 30;
	*latMax = *latMax + extra * 30;
	return 0;
}

int Utils::computeImageGeoBoundry_no_expansion(double topleft_lon, double topleft_lat, double topright_lon, double topright_lat, double bottomleft_lon, double bottomleft_lat, double bottomright_lon, double bottomright_lat, double* lonMax, double* latMax, double* lonMin, double* latMin)
{
	Mat lon, lat;
	lon.create(1, 4, CV_64F);
	lat.create(1, 4, CV_64F);

	lon.at<double>(0, 0) = topleft_lon;
	lon.at<double>(0, 1) = topright_lon;
	lon.at<double>(0, 2) = bottomleft_lon;
	lon.at<double>(0, 3) = bottomright_lon;

	lat.at<double>(0, 0) = topleft_lat;
	lat.at<double>(0, 1) = topright_lat;
	lat.at<double>(0, 2) = bottomleft_lat;
	lat.at<double>(0, 3) = bottomright_lat;

	cv::minMaxLoc(lon, lonMin, lonMax);
	cv::minMaxLoc(lat, latMin, latMax);
	double extra = 5.0 / 6000;
	//*lonMin = *lonMin - extra * 30;
	//*lonMax = *lonMax + extra * 30;
	//*latMin = *latMin - extra * 30;
	//*latMax = *latMax + extra * 30;
	return 0;
}

int Utils::getSRTMDEM(
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
	double latSpacing = 5.0 / 6000.0;
	double lonSpacing = 5.0 / 6000.0;
	if (!filepath || !lonUL || !latUL) return -1;

	// 1. 判断是目录还是文件
	DWORD dwAttrs = GetFileAttributesA(filepath);
	bool isDirectory = false;
	if (dwAttrs != INVALID_FILE_ATTRIBUTES)
	{
		isDirectory = ((dwAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
	}
	else
	{
		// 路径不存在时，如果无常见文件扩展名，则视为目录以触发原有自动创建和下载逻辑
		std::string pathStr(filepath);
		size_t dotPos = pathStr.find_last_of(".");
		size_t sepPos = pathStr.find_last_of("\\/");
		if (dotPos == std::string::npos || (sepPos != std::string::npos && dotPos < sepPos))
		{
			isDirectory = true;
		}
	}

	if (!isDirectory)
	{
		// ----------------------------------------------------
		// 现代单文件模式 (使用 GDAL 统一读取单文件/VRT，并自动规整分辨率为 5.0 / 6000.0)
		// ----------------------------------------------------
		InitializeGDALAndProjOnce();
		GDALDataset* poDataset = (GDALDataset*)GDALOpen(filepath, GA_ReadOnly);
		if (poDataset == nullptr)
		{
			return -1; // 无法打开 DEM 文件
		}

		GDALRasterBand* poBand = poDataset->GetRasterBand(1);
		if (poBand == nullptr)
		{
			GDALClose(poDataset);
			return -1;
		}

		double adfGeoTransform[6];
		poDataset->GetGeoTransform(adfGeoTransform);

		int demTotalWidth = poDataset->GetRasterXSize();
		int demTotalHeight = poDataset->GetRasterYSize();

		// 假定等经纬度投影无旋转 (gt[2] == 0, gt[4] == 0, gt[5] < 0)
		int xOff = static_cast<int>(std::floor((lonMin - adfGeoTransform[0]) / adfGeoTransform[1]));
		int yOff = static_cast<int>(std::floor((latMax - adfGeoTransform[3]) / adfGeoTransform[5]));
		int xSize = static_cast<int>(std::ceil((lonMax - lonMin) / adfGeoTransform[1]));
		int ySize = static_cast<int>(std::ceil((latMin - latMax) / adfGeoTransform[5]));

		// 端点裁剪保护（彻底解决边缘坐标平移 Bug）
		int srcXOff = std::max(0, xOff);
		int srcYOff = std::max(0, yOff);
		int srcXEnd = std::min(demTotalWidth, xOff + xSize);
		int srcYEnd = std::min(demTotalHeight, yOff + ySize);
		int srcXSize = srcXEnd - srcXOff;
		int srcYSize = srcYEnd - srcYOff;

		if (srcXSize <= 0 || srcYSize <= 0)
		{
			GDALClose(poDataset);
			return -2; // 经纬度范围与该 DEM 无交集
		}

		// 计算读取区域 of the actual geo boundary
		double lonStart = adfGeoTransform[0] + srcXOff * adfGeoTransform[1];
		double latStart = adfGeoTransform[3] + srcYOff * adfGeoTransform[5];
		double lonEnd = adfGeoTransform[0] + srcXEnd * adfGeoTransform[1];
		double latEnd = adfGeoTransform[3] + srcYEnd * adfGeoTransform[5];

		// 重采样规整：计算符合 5.0/6000.0 分辨率的目标矩阵大小（彻底解决分辨率漂移 Bug）
		const double targetSpacing = 5.0 / 6000.0;
		int targetXSize = std::max(1, static_cast<int>(std::round((lonEnd - lonStart) / targetSpacing)));
		int targetYSize = std::max(1, static_cast<int>(std::round((latStart - latEnd) / targetSpacing)));

		// 初始化输出矩阵为 short (CV_16S)，契合底层算子
		DEM_out.create(targetYSize, targetXSize, CV_16S);

		// 使用 GDAL 自动将 srcXSize*srcYSize 的区域插值读取到 targetXSize*targetYSize 的内存中
		// 并显式指定双线性插值以获得更好的地形模拟平滑度
		GDALRasterIOExtraArg extraArgs;
		INIT_RASTERIO_EXTRA_ARG(extraArgs);
		extraArgs.eResampleAlg = GRIORA_Bilinear;

		CPLErr err = poBand->RasterIO(
			GF_Read, 
			srcXOff, srcYOff, 
			srcXSize, srcYSize, 
			DEM_out.data, 
			targetXSize, targetYSize, 
			GDT_Int16, 
			0, 0,
			&extraArgs
		);

		if (err != CE_None)
		{
			GDALClose(poDataset);
			return -3; // 读取数据失败
		}

		// 输出契合 90m 等经纬度网格起点的地理坐标
		*lonUL = lonStart;
		*latUL = latStart;

		GDALClose(poDataset);
		return 0; // 读取成功
	}

	//this->DEMPath = filepath;
	if (GetFileAttributesA(filepath) == -1)
	{
		if (_mkdir(filepath) != 0) return -1;
	}
	string DEMPath = filepath;
	vector<string> srtmFileName;
	vector<bool> bAlreadyExist;
	int ret = getSRTMFileName(lonMin, lonMax, latMin, latMax, srtmFileName);
	if (ret < 0)//不在SRTM数据范围内（-60°,60°）,则以0填充
	{
		int rows = static_cast<int>((latMax - latMin) / latSpacing);
		int cols = static_cast<int>((lonMax - lonMin) / lonSpacing);
		Mat temp = Mat::zeros(rows, cols, CV_16S);
		temp.copyTo(DEM_out);
		*lonUL = lonMin;
		*latUL = latMax;
		return 0;
	}
	//if (return_check(ret, "getSRTMFileName()", error_head)) return -1;
	//判断文件是否已经存在
	for (int i = 0; i < srtmFileName.size(); i++)
	{
		string tmp = DEMPath + "\\" + srtmFileName[i];
		std::replace(tmp.begin(), tmp.end(), '/', '\\');
		if (-1 != GetFileAttributesA(tmp.c_str()))bAlreadyExist.push_back(true);
		else bAlreadyExist.push_back(false);
	}
	//不存在则下载
	for (int i = 0; i < srtmFileName.size(); i++)
	{
		if (!bAlreadyExist[i])
		{
			ret = downloadSRTM(srtmFileName[i].c_str(), DEMPath.c_str());
			if (ret < 0)//未下载到DEM数据,则以0填充
			{
				int rows = static_cast<int>((latMax - latMin) / latSpacing);
				int cols = static_cast<int>((lonMax - lonMin) / lonSpacing);
				Mat temp = Mat::zeros(rows, cols, CV_16S);
				temp.copyTo(DEM_out);
				*lonUL = lonMin;
				*latUL = latMax;
				return 0;
			}
		}
	}
	//解压文件
	for (int i = 0; i < srtmFileName.size(); i++)
	{
		string folderName = srtmFileName[i];
		folderName = folderName.substr(0, folderName.length() - 4);
		string path = DEMPath + string("\\") + folderName;
		std::replace(path.begin(), path.end(), '/', '\\');
		if (-1 != GetFileAttributesA(path.c_str())) continue;
		string srcFile = DEMPath + "\\" + srtmFileName[i];
		std::replace(srcFile.begin(), srcFile.end(), '/', '\\');
		if (GetFileAttributesA(srcFile.c_str()) == -1) continue;
		ret = unzip_srtm_archive(srcFile.c_str(), path.c_str());
		if (return_check(ret, "unzip()", error_head)) return -1;
	}


	int startRow, startCol, endRow, endCol;
	double lonUpperLeft, lonLowerRight, latUpperLeft, latLowerRight;
	int total_rows, total_cols;

	//DEM在一个SRTM方格内
	if (srtmFileName.size() == 1)
	{
		total_rows = 6000, total_cols = 6000;
		int xx, yy;
		sscanf(srtmFileName[0].c_str(), "srtm_%d_%d.zip", &xx, &yy);
		latUpperLeft = 60.0 - (yy - 1) * 5.0;
		latLowerRight = latUpperLeft - 5.0;
		lonUpperLeft = -180.0 + (xx - 1) * 5.0;
		lonLowerRight = lonUpperLeft + 5.0;

		startRow = static_cast<int>((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = static_cast<int>((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		startCol = static_cast<int>((lonMin - lonUpperLeft) / lonSpacing);
		startCol = startCol < 1 ? 1 : startCol;
		startCol = startCol > total_cols ? total_cols : startCol;
		endCol = static_cast<int>((lonMax - lonUpperLeft) / lonSpacing);
		endCol = endCol < 1 ? 1 : endCol;
		endCol = endCol > total_cols ? total_cols : endCol;

		string folderName = srtmFileName[0];
		folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
		string path = DEMPath + string("\\") + folderName;
		path = path + string("\\") + folderName + string(".tif");
		Mat outDEM = Mat::zeros(6000, 6000, CV_16S);
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = read_srtm_geotiff(path.c_str(), outDEM);
		//if (return_check(ret, "geotiffread()", error_head)) return -1;
		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
	}
	//DEM在2个方格内
	else if (srtmFileName.size() == 2)
	{
		int xx, yy, xx2, yy2;
		sscanf(srtmFileName[0].c_str(), "srtm_%d_%d.zip", &xx, &yy);
		sscanf(srtmFileName[1].c_str(), "srtm_%d_%d.zip", &xx2, &yy2);
		//同一列
		if (xx == xx2)
		{
			total_rows = 6000 * 2; total_cols = 6000;
			latUpperLeft = 60.0 - ((yy < yy2 ? yy : yy2) - 1) * 5.0;
			latLowerRight = latUpperLeft - 10.0;
			lonUpperLeft = -180.0 + (xx - 1) * 5.0;
			lonLowerRight = lonUpperLeft + 5.0;

			startRow = static_cast<int>((latUpperLeft - latMax) / latSpacing);
			startRow = startRow < 1 ? 1 : startRow;
			startRow = startRow > total_rows ? total_rows : startRow;
			endRow = static_cast<int>((latUpperLeft - latMin) / latSpacing);
			endRow = endRow < 1 ? 1 : endRow;
			endRow = endRow > total_rows ? total_rows : endRow;
			startCol = static_cast<int>((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = static_cast<int>((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;


			Mat outDEM, outDEM2;

			if (yy < yy2)
			{
				string folderName = srtmFileName[0];
				folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
				string path = DEMPath + string("\\") + folderName;
				path = path + string("\\") + folderName + string(".tif");
				std::replace(path.begin(), path.end(), '/', '\\');
				outDEM = Mat::zeros(6000, 6000, CV_16S);
				ret = read_srtm_geotiff(path.c_str(), outDEM);
				//if (return_check(ret, "geotiffread()", error_head)) return -1;

				folderName = srtmFileName[1];
				folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
				path = DEMPath + string("\\") + folderName;
				path = path + string("\\") + folderName + string(".tif");
				std::replace(path.begin(), path.end(), '/', '\\');
				outDEM2 = Mat::zeros(6000, 6000, CV_16S);
				ret = read_srtm_geotiff(path.c_str(), outDEM2);
				//if (return_check(ret, "geotiffread()", error_head)) return -1;
				cv::vconcat(outDEM, outDEM2, outDEM);
			}
			else
			{
				string folderName = srtmFileName[1];
				folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
				string path = DEMPath + string("\\") + folderName;
				path = path + string("\\") + folderName + string(".tif");
				std::replace(path.begin(), path.end(), '/', '\\');
				outDEM = Mat::zeros(6000, 6000, CV_16S);
				ret = read_srtm_geotiff(path.c_str(), outDEM);
				//if (return_check(ret, "geotiffread()", error_head)) return -1;

				folderName = srtmFileName[0];
				folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
				path = DEMPath + string("\\") + folderName;
				path = path + string("\\") + folderName + string(".tif");
				std::replace(path.begin(), path.end(), '/', '\\');
				outDEM2 = Mat::zeros(6000, 6000, CV_16S);
				ret = read_srtm_geotiff(path.c_str(), outDEM2);
				//if (return_check(ret, "geotiffread()", error_head)) return -1;
				cv::vconcat(outDEM, outDEM2, outDEM);
			}

			outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
			*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
			*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		}
		//同一行
		else if (yy == yy2)
		{
			total_cols = 6000 * 2; total_rows = 6000;
			//跨越-180.0/180.0线
			if ((xx == 1 && xx2 == 72) || (xx == 72 && xx2 == 1))
			{
				latUpperLeft = 60.0 - (yy - 1) * 5.0;
				latLowerRight = latUpperLeft - 5.0;
				lonUpperLeft = 175.0;
				lonLowerRight = -175.0;
				startRow = static_cast<int>((latUpperLeft - latMax) / latSpacing);
				startRow = startRow < 1 ? 1 : startRow;
				startRow = startRow > total_rows ? total_rows : startRow;
				endRow = static_cast<int>((latUpperLeft - latMin) / latSpacing);
				endRow = endRow < 1 ? 1 : endRow;
				endRow = endRow > total_rows ? total_rows : endRow;
				startCol = static_cast<int>((lonMax - lonUpperLeft) / lonSpacing);
				startCol = startCol < 1 ? 1 : startCol;
				startCol = startCol > total_cols ? total_cols : startCol;
				endCol = static_cast<int>((lonMin - lonUpperLeft + 360.0) / lonSpacing);
				endCol = endCol < 1 ? 1 : endCol;
				endCol = endCol > total_cols ? total_cols : endCol;

				Mat outDEM, outDEM2;

				if (xx > xx2)
				{
					string folderName = srtmFileName[0];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					string path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;

					folderName = srtmFileName[1];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM2 = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM2);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;
					cv::hconcat(outDEM, outDEM2, outDEM);
				}
				else
				{
					string folderName = srtmFileName[1];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					string path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;

					folderName = srtmFileName[0];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM2 = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM2);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;
					cv::hconcat(outDEM, outDEM2, outDEM);
				}

				outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
				*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
				*latUL = latUpperLeft - (startRow - 1) * latSpacing;
			}
			else
			{
				latUpperLeft = 60.0 - (yy - 1) * 5.0;
				latLowerRight = latUpperLeft - 5.0;
				lonUpperLeft = -180.0 + ((xx < xx2 ? xx : xx2) - 1) * 5.0;
				lonLowerRight = lonUpperLeft + 10.0;

				startRow = static_cast<int>((latUpperLeft - latMax) / latSpacing);
				startRow = startRow < 1 ? 1 : startRow;
				startRow = startRow > total_rows ? total_rows : startRow;
				endRow = static_cast<int>((latUpperLeft - latMin) / latSpacing);
				endRow = endRow < 1 ? 1 : endRow;
				endRow = endRow > total_rows ? total_rows : endRow;
				startCol = static_cast<int>((lonMin - lonUpperLeft) / lonSpacing);
				startCol = startCol < 1 ? 1 : startCol;
				startCol = startCol > total_cols ? total_cols : startCol;
				endCol = static_cast<int>((lonMax - lonUpperLeft) / lonSpacing);
				endCol = endCol < 1 ? 1 : endCol;
				endCol = endCol > total_cols ? total_cols : endCol;


				Mat outDEM, outDEM2;

				if (xx < xx2)
				{
					string folderName = srtmFileName[0];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					string path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;

					folderName = srtmFileName[1];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM2 = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM2);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;
					cv::hconcat(outDEM, outDEM2, outDEM);
				}
				else
				{
					string folderName = srtmFileName[1];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					string path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;

					folderName = srtmFileName[0];
					folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
					path = DEMPath + string("\\") + folderName;
					path = path + string("\\") + folderName + string(".tif");
					std::replace(path.begin(), path.end(), '/', '\\');
					outDEM2 = Mat::zeros(6000, 6000, CV_16S);
					ret = read_srtm_geotiff(path.c_str(), outDEM2);
					//if (return_check(ret, "geotiffread()", error_head)) return -1;
					cv::hconcat(outDEM, outDEM2, outDEM);
				}

				outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
				*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
				*latUL = latUpperLeft - (startRow - 1) * latSpacing;
			}
		}
		else
		{
			return -1;
		}



	}
	//DEM在4个方格内
	else if (srtmFileName.size() == 4)
	{
		int xx, yy, xx2, yy2, xx3, yy3, xx4, yy4;
		// removed unused: temp (declared but never assigned or read)
		sscanf(srtmFileName[0].c_str(), "srtm_%d_%d.zip", &xx, &yy);
		sscanf(srtmFileName[1].c_str(), "srtm_%d_%d.zip", &xx2, &yy2);
		sscanf(srtmFileName[2].c_str(), "srtm_%d_%d.zip", &xx3, &yy3);
		sscanf(srtmFileName[3].c_str(), "srtm_%d_%d.zip", &xx4, &yy4);
		total_rows = 6000 * 2; total_cols = 6000 * 2;
		//跨越-180.0/180.0线
		if (lonMax * lonMin < 0 && (fabs(lonMin) + fabs(lonMax)) > 180.0)
		{
			startRow = (int)((60.0 - latMax) / 5.0) + 1;
			endRow = (int)((60.0 - latMin) / 5.0) + 1;
			endCol = (int)((lonMin + 180.0) / 5.0) + 1;
			startCol = (int)((lonMax + 180.0) / 5.0) + 1;
			latUpperLeft = 60.0 - (startRow - 1) * 5.0;
			latLowerRight = latUpperLeft - 10.0;
			lonUpperLeft = 175.0;
			lonLowerRight = -175.0;



			Mat outDEM, outDEM2, outDEM3;

			char tmpstr[512];
			const char* format = NULL;
			if (startCol < 10 && startRow < 10) format = "srtm_0%d_0%d.zip";
			else if (startCol >= 10 && startRow < 10) format = "srtm_%d_0%d.zip";
			else if (startCol < 10 && startRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, startCol, startRow);
			string folderName(tmpstr);
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			string path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;

			if (endCol < 10 && startRow < 10) format = "srtm_0%d_0%d.zip";
			else if (endCol >= 10 && startRow < 10) format = "srtm_%d_0%d.zip";
			else if (endCol < 10 && startRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, endCol, startRow);
			folderName = tmpstr;
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM2 = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM2);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;
			cv::hconcat(outDEM, outDEM2, outDEM);

			if (startCol < 10 && endRow < 10) format = "srtm_0%d_0%d.zip";
			else if (startCol >= 10 && endRow < 10) format = "srtm_%d_0%d.zip";
			else if (startCol < 10 && endRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, startCol, endRow);
			folderName = tmpstr;
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM2 = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM2);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;

			if (endCol < 10 && endRow < 10) format = "srtm_0%d_0%d.zip";
			else if (endCol >= 10 && endRow < 10) format = "srtm_%d_0%d.zip";
			else if (endCol < 10 && endRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, endCol, endRow);
			folderName = tmpstr;
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM3 = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM3);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;
			cv::hconcat(outDEM2, outDEM3, outDEM2);

			cv::vconcat(outDEM, outDEM2, outDEM);


			startRow = static_cast<int>((latUpperLeft - latMax) / latSpacing);
			startRow = startRow < 1 ? 1 : startRow;
			startRow = startRow > total_rows ? total_rows : startRow;
			endRow = static_cast<int>((latUpperLeft - latMin) / latSpacing);
			endRow = endRow < 1 ? 1 : endRow;
			endRow = endRow > total_rows ? total_rows : endRow;
			startCol = static_cast<int>((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = static_cast<int>((lonMin - lonUpperLeft + 360.0) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;

			outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
			*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
			*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		}
		else
		{
			startRow = (int)((60.0 - latMax) / 5.0) + 1;
			endRow = (int)((60.0 - latMin) / 5.0) + 1;
			startCol = (int)((lonMin + 180.0) / 5.0) + 1;
			endCol = (int)((lonMax + 180.0) / 5.0) + 1;
			latUpperLeft = 60.0 - (startRow - 1) * 5.0;
			latLowerRight = latUpperLeft - 10.0;
			lonUpperLeft = -180.0 + (startCol - 1) * 5.0;
			lonLowerRight = lonUpperLeft + 10.0;



			Mat outDEM, outDEM2, outDEM3;

			char tmpstr[512];
			const char* format = NULL;
			if (startCol < 10 && startRow < 10) format = "srtm_0%d_0%d.zip";
			else if (startCol >= 10 && startRow < 10) format = "srtm_%d_0%d.zip";
			else if (startCol < 10 && startRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, startCol, startRow);
			string folderName(tmpstr);
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			string path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;

			if (endCol < 10 && startRow < 10) format = "srtm_0%d_0%d.zip";
			else if (endCol >= 10 && startRow < 10) format = "srtm_%d_0%d.zip";
			else if (endCol < 10 && startRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, endCol, startRow);
			folderName = tmpstr;
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM2 = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM2);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;
			cv::hconcat(outDEM, outDEM2, outDEM);

			if (startCol < 10 && endRow < 10) format = "srtm_0%d_0%d.zip";
			else if (startCol >= 10 && endRow < 10) format = "srtm_%d_0%d.zip";
			else if (startCol < 10 && endRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, startCol, endRow);
			folderName = tmpstr;
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM2 = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM2);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;

			if (endCol < 10 && endRow < 10) format = "srtm_0%d_0%d.zip";
			else if (endCol >= 10 && endRow < 10) format = "srtm_%d_0%d.zip";
			else if (endCol < 10 && endRow >= 10) format = "srtm_0%d_%d.zip";
			else format = "srtm_%d_%d.zip";
			sprintf(tmpstr, format, endCol, endRow);
			folderName = tmpstr;
			folderName = folderName.substr(0, folderName.length() - 4);//去掉.zip后缀
			path = DEMPath + string("\\") + folderName;
			path = path + string("\\") + folderName + string(".tif");
			std::replace(path.begin(), path.end(), '/', '\\');
			outDEM3 = Mat::zeros(6000, 6000, CV_16S);
			ret = read_srtm_geotiff(path.c_str(), outDEM3);
			//if (return_check(ret, "geotiffread()", error_head)) return -1;
			cv::hconcat(outDEM2, outDEM3, outDEM2);

			cv::vconcat(outDEM, outDEM2, outDEM);

			startRow = static_cast<int>((latUpperLeft - latMax) / latSpacing);
			startRow = startRow < 1 ? 1 : startRow;
			startRow = startRow > total_rows ? total_rows : startRow;
			endRow = static_cast<int>((latUpperLeft - latMin) / latSpacing);
			endRow = endRow < 1 ? 1 : endRow;
			endRow = endRow > total_rows ? total_rows : endRow;
			startCol = static_cast<int>((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = static_cast<int>((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;

			outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
			*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
			*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		}

	}
	else return -1;
	return 0;
}

int Utils::getCopernicusDEM(
	const char* filepath,
	Mat& DEM_out,
	double* lonUL,
	double* latUL,
	double* lon_spacing,
	double* lat_spacing,
	double lonMin,
	double lonMax,
	double latMin,
	double latMax
)
{
	double latSpacing = 1.0 / 3600.0;
	double lonSpacing = 1.0 / 3600.0;
	if (!filepath || !lonUL || !latUL) return -1;
	if (GetFileAttributesA(filepath) == -1)
	{
		if (_mkdir(filepath) != 0) return -1;
	}
	string DEMPath = filepath;
	vector<string> CopernicusDEMFileName;
	vector<bool> bAlreadyExist;
	int ret = getCopernicusDEMFileName(lonMin, lonMax, latMin, latMax, CopernicusDEMFileName);
	if (ret < 0)//以0填充
	{
		int rows = cvRound((latMax - latMin) / latSpacing);
		int cols = cvRound((lonMax - lonMin) / lonSpacing);
		if (fabs(lonMax - lonMin) > 180.0)
		{
			cols = cvRound((- lonMax + lonMin + 360.0) / lonSpacing);
		}
		Mat temp = Mat::zeros(rows, cols, CV_32F);
		temp.copyTo(DEM_out);
		*lonUL = fabs(lonMax - lonMin) < 180.0 ? lonMin : lonMax;
		*latUL = latMax;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
		return 0;
	}
	//判断文件是否已经存在
	for (int i = 0; i < CopernicusDEMFileName.size(); i++)
	{
		string tmp = DEMPath + "\\" + CopernicusDEMFileName[i];
		std::replace(tmp.begin(), tmp.end(), '/', '\\');
		if (-1 != GetFileAttributesA(tmp.c_str()))bAlreadyExist.push_back(true);
		else bAlreadyExist.push_back(false);
	}
	//不存在则下载
	for (int i = 0; i < CopernicusDEMFileName.size(); i++)
	{
		if (!bAlreadyExist[i])
		{
			ret = downloadCopernicusDEM(CopernicusDEMFileName[i].c_str(), DEMPath.c_str());
			//if (ret < 0)//未下载到DEM数据,则以0填充
			//{
			//	int rows = (latMax - latMin) / latSpacing;
			//	int cols = (lonMax - lonMin) / lonSpacing;
			//	if (fabs(lonMax - lonMin) > 180.0)
			//	{
			//		cols = (-lonMax + lonMin + 360.0) / lonSpacing;
			//	}
			//	Mat temp = Mat::zeros(rows, cols, CV_32F);
			//	temp.copyTo(DEM_out);
			//	*lonUL = fabs(lonMax - lonMin) < 180.0 ? lonMin : lonMax;
			//	*latUL = latMax;
			//	*lon_spacing = lonSpacing;
			//	*lat_spacing = latSpacing;
			//	return 0;
			//}
		}
	}



	int startRow, startCol, endRow, endCol;
	double lonUpperLeft, lonLowerRight, latUpperLeft, latLowerRight;
	int total_rows, total_cols;

	//DEM在一个SRTM方格内
	if (CopernicusDEMFileName.size() == 1)
	{
		int xx, yy;
		
		if (CopernicusDEMFileName[0].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx, &yy);
		}
		else if (CopernicusDEMFileName[0].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx, &yy);
			yy = -yy;
		}
		else if (CopernicusDEMFileName[0].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx, &yy);
			xx = -xx;
		}
		else if (CopernicusDEMFileName[0].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx, &yy);
			yy = -yy;
			xx = -xx;
		}
		else
		{
			return -1;
		}
		latUpperLeft = xx + 1.0;
		latLowerRight = xx;
		lonUpperLeft = yy;
		lonLowerRight = yy + 1.0;


		string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
		Mat outDEM;
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
		if (ret < 0)//未下载到用0填充
		{
			outDEM = Mat::zeros(3600, 3600, CV_32F);
		}
		latSpacing = 1.0 / double(outDEM.rows);
		lonSpacing = 1.0 / double(outDEM.cols);

		total_rows = outDEM.rows;
		total_cols = outDEM.cols;

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	//DEM在2个方格内
	else if (CopernicusDEMFileName.size() == 2)
	{
		int xx, yy, xx2, yy2;

		if (CopernicusDEMFileName[0].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx, &yy);
		}
		else if (CopernicusDEMFileName[0].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx, &yy);
			yy = -yy;
		}
		else if (CopernicusDEMFileName[0].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx, &yy);
			xx = -xx;
		}
		else if (CopernicusDEMFileName[0].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
		{
			sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx, &yy);
			yy = -yy;
			xx = -xx;
		}
		else
		{
			return -1;
		}

		if (CopernicusDEMFileName[1].substr(22, 1) == string("N") && CopernicusDEMFileName[1].substr(29, 1) == string("E"))
		{
			sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx2, &yy2);
		}
		else if (CopernicusDEMFileName[1].substr(22, 1) == string("N") && CopernicusDEMFileName[1].substr(29, 1) == string("W"))
		{
			sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx2, &yy2);
			yy2 = -yy2;
		}
		else if (CopernicusDEMFileName[1].substr(22, 1) == string("S") && CopernicusDEMFileName[1].substr(29, 1) == string("E"))
		{
			sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx2, &yy2);
			xx2 = -xx2;
		}
		else if (CopernicusDEMFileName[1].substr(22, 1) == string("S") && CopernicusDEMFileName[1].substr(29, 1) == string("W"))
		{
			sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx2, &yy2);
			yy2 = -yy2;
			xx2 = -xx2;
		}
		else
		{
			return -1;
		}

		//同一列
		if (yy == yy2)
		{
			Mat outDEM, outDEM2;

			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM2.rows);
			lonSpacing = 1.0 / double(outDEM2.cols);

			if (outDEM.size() != outDEM2.size())
			{
				resize(outDEM, outDEM, outDEM2.size());
			}

			cv::vconcat(outDEM, outDEM2, outDEM);
			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx + 1;
			latLowerRight = xx - 1;
			lonUpperLeft = yy;
			lonLowerRight = yy + 1;

			startRow = cvRound((latUpperLeft - latMax) / latSpacing);
			startRow = startRow < 1 ? 1 : startRow;
			startRow = startRow > total_rows ? total_rows : startRow;
			endRow = cvRound((latUpperLeft - latMin) / latSpacing);
			endRow = endRow < 1 ? 1 : endRow;
			endRow = endRow > total_rows ? total_rows : endRow;
			if (fabs(lonMax - lonMin) > 180.0)
			{
				startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
				startCol = startCol < 1 ? 1 : startCol;
				startCol = startCol > total_cols ? total_cols : startCol;
				endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
				endCol = endCol < 1 ? 1 : endCol;
				endCol = endCol > total_cols ? total_cols : endCol;
			}
			else
			{
				startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
				startCol = startCol < 1 ? 1 : startCol;
				startCol = startCol > total_cols ? total_cols : startCol;
				endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
				endCol = endCol < 1 ? 1 : endCol;
				endCol = endCol > total_cols ? total_cols : endCol;
			}

			outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
			*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
			*latUL = latUpperLeft - (startRow - 1) * latSpacing;
			*lon_spacing = lonSpacing;
			*lat_spacing = latSpacing;
		}
		//同一行
		else if (xx == xx2)
		{
			Mat outDEM, outDEM2;

			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM2.rows);
			lonSpacing = 1.0 / double(outDEM2.cols);

			if (outDEM.size() != outDEM2.size())
			{
				resize(outDEM, outDEM, outDEM2.size());
			}

			cv::hconcat(outDEM, outDEM2, outDEM);
			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			lonUpperLeft = yy;
			lonLowerRight = yy + 2;
			latUpperLeft = xx + 1;
			latLowerRight = xx;

			startRow = cvRound((latUpperLeft - latMax) / latSpacing);
			startRow = startRow < 1 ? 1 : startRow;
			startRow = startRow > total_rows ? total_rows : startRow;
			endRow = cvRound((latUpperLeft - latMin) / latSpacing);
			endRow = endRow < 1 ? 1 : endRow;
			endRow = endRow > total_rows ? total_rows : endRow;
			if (fabs(lonMax - lonMin) > 180.0)
			{
				startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
				startCol = startCol < 1 ? 1 : startCol;
				startCol = startCol > total_cols ? total_cols : startCol;
				endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
				endCol = endCol < 1 ? 1 : endCol;
				endCol = endCol > total_cols ? total_cols : endCol;
			}
			else
			{
				startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
				startCol = startCol < 1 ? 1 : startCol;
				startCol = startCol > total_cols ? total_cols : startCol;
				endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
				endCol = endCol < 1 ? 1 : endCol;
				endCol = endCol > total_cols ? total_cols : endCol;
			}


			outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
			*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
			*latUL = latUpperLeft - (startRow - 1) * latSpacing;
			*lon_spacing = lonSpacing;
			*lat_spacing = latSpacing;
		}
		else
		{
			return -1;
		}



	}
	//DEM在3个方格内
	else if (CopernicusDEMFileName.size() == 3)
	{
	int xx, yy, xx2, yy2, xx3,yy3;

	if (CopernicusDEMFileName[0].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
	{
		sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx, &yy);
	}
	else if (CopernicusDEMFileName[0].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
	{
		sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx, &yy);
		yy = -yy;
	}
	else if (CopernicusDEMFileName[0].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
	{
		sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx, &yy);
		xx = -xx;
	}
	else if (CopernicusDEMFileName[0].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
	{
		sscanf(CopernicusDEMFileName[0].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx, &yy);
		yy = -yy;
		xx = -xx;
	}
	else
	{
		return -1;
	}

	if (CopernicusDEMFileName[1].substr(22, 1) == string("N") && CopernicusDEMFileName[1].substr(29, 1) == string("E"))
	{
		sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx2, &yy2);
	}
	else if (CopernicusDEMFileName[1].substr(22, 1) == string("N") && CopernicusDEMFileName[1].substr(29, 1) == string("W"))
	{
		sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx2, &yy2);
		yy2 = -yy2;
	}
	else if (CopernicusDEMFileName[1].substr(22, 1) == string("S") && CopernicusDEMFileName[1].substr(29, 1) == string("E"))
	{
		sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx2, &yy2);
		xx2 = -xx2;
	}
	else if (CopernicusDEMFileName[1].substr(22, 1) == string("S") && CopernicusDEMFileName[1].substr(29, 1) == string("W"))
	{
		sscanf(CopernicusDEMFileName[1].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx2, &yy2);
		yy2 = -yy2;
		xx2 = -xx2;
	}
	else
	{
		return -1;
	}

	if (CopernicusDEMFileName[2].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
	{
		sscanf(CopernicusDEMFileName[2].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx3, &yy3);
	}
	else if (CopernicusDEMFileName[2].substr(22, 1) == string("N") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
	{
		sscanf(CopernicusDEMFileName[2].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx3, &yy3);
		yy3 = -yy3;
	}
	else if (CopernicusDEMFileName[2].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("E"))
	{
		sscanf(CopernicusDEMFileName[2].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx3, &yy3);
		xx3 = -xx3;
	}
	else if (CopernicusDEMFileName[2].substr(22, 1) == string("S") && CopernicusDEMFileName[0].substr(29, 1) == string("W"))
	{
		sscanf(CopernicusDEMFileName[2].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx3, &yy3);
		yy3 = -yy3;
		xx3 = -xx3;
	}
	else
	{
		return -1;
	}

	//同一列
	if (yy == yy2)
	{
		Mat outDEM, outDEM2, outDEM3;

		string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
		if (ret < 0)//未下载到用0填充
		{
			outDEM = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[1];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
		if (ret < 0)//未下载到用0填充
		{
			outDEM2 = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[2];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
		if (ret < 0)//未下载到用0填充
		{
			outDEM3 = Mat::zeros(3600, 3600, CV_32F);
		}

		latSpacing = 1.0 / double(outDEM3.rows);
		lonSpacing = 1.0 / double(outDEM3.cols);

		if (outDEM.size() != outDEM3.size())
		{
			resize(outDEM, outDEM, outDEM3.size());
		}

		if (outDEM2.size() != outDEM3.size())
		{
			resize(outDEM2, outDEM2, outDEM3.size());
		}

		cv::vconcat(outDEM, outDEM2, outDEM);
		cv::vconcat(outDEM, outDEM3, outDEM);
		total_rows = outDEM.rows;
		total_cols = outDEM.cols;

		latUpperLeft = xx + 1;
		latLowerRight = xx - 2;
		lonUpperLeft = yy;
		lonLowerRight = yy + 1;

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	//同一行
	else if (xx == xx2)
	{
		Mat outDEM, outDEM2, outDEM3;

		string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
		if (ret < 0)//未下载到用0填充
		{
			outDEM = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[1];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
		if (ret < 0)//未下载到用0填充
		{
			outDEM2 = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[2];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
		if (ret < 0)//未下载到用0填充
		{
			outDEM3 = Mat::zeros(3600, 3600, CV_32F);
		}

		latSpacing = 1.0 / double(outDEM3.rows);
		lonSpacing = 1.0 / double(outDEM3.cols);

		if (outDEM.size() != outDEM3.size())
		{
			resize(outDEM, outDEM, outDEM3.size());
		}
		if (outDEM2.size() != outDEM3.size())
		{
			resize(outDEM2, outDEM2, outDEM3.size());
		}

		cv::hconcat(outDEM, outDEM2, outDEM);
		cv::hconcat(outDEM, outDEM3, outDEM);
		total_rows = outDEM.rows;
		total_cols = outDEM.cols;

		lonUpperLeft = yy;
		lonLowerRight = yy + 3;
		latUpperLeft = xx + 1;
		latLowerRight = xx;

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}


		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	else
	{
		return -1;
	}



	}
	//DEM在4个方格内
	else if (CopernicusDEMFileName.size() == 4)
	{
		int xx[4], yy[4]/*, temp*/;

		for (int i = 0; i < 4; i++)
		{
			if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
				xx[i] = -xx[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
				xx[i] = -xx[i];
			}
			else
			{
				return -1;
			}
		}

		

		Mat outDEM, outDEM2, outDEM3;

		string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
		if (ret < 0)//未下载到用0填充
		{
			outDEM = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[1];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
		if (ret < 0)//未下载到用0填充
		{
			outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
		}

		cv::hconcat(outDEM, outDEM2, outDEM);

		path = DEMPath + string("\\") + CopernicusDEMFileName[2];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
		if (ret < 0)//未下载到用0填充
		{
			outDEM2 = Mat::zeros(3600, 3600, CV_32F);
		}
		latSpacing = 1.0 / double(outDEM2.rows);
		lonSpacing = 1.0 / double(outDEM2.cols);

		path = DEMPath + string("\\") + CopernicusDEMFileName[3];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
		if (ret < 0)//未下载到用0填充
		{
			outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
		}


		cv::hconcat(outDEM2, outDEM3, outDEM2);

		if (outDEM.size() != outDEM2.size())
		{
			resize(outDEM, outDEM, outDEM2.size());
		}

		cv::vconcat(outDEM, outDEM2, outDEM);

		total_rows = outDEM.rows;
		total_cols = outDEM.cols;



		latUpperLeft = xx[0] + 1;
		latLowerRight = xx[0] - 1;
		lonUpperLeft = yy[0];
		lonLowerRight = yy[0] + 2;

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;

	}
	//DEM在6个方格内	
	else if (CopernicusDEMFileName.size() == 6)
	{
		int xx[6], yy[6]/*, temp*/;

		for (int i = 0; i < 6; i++)
		{
			if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
				xx[i] = -xx[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
				xx[i] = -xx[i];
			}
			else
			{
				return -1;
			}
		}
		Mat outDEM, outDEM2, outDEM3, outDEM4;
		//三行两列
		if (xx[0] != xx[2] && xx[0] == xx[1])
		{
			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			cv::hconcat(outDEM, outDEM2, outDEM);

			path = DEMPath + string("\\") + CopernicusDEMFileName[2];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}
			
			path = DEMPath + string("\\") + CopernicusDEMFileName[3];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}
			
			cv::hconcat(outDEM2, outDEM3, outDEM2);

			path = DEMPath + string("\\") + CopernicusDEMFileName[4];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[5];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM3.rows);
			lonSpacing = 1.0 / double(outDEM3.cols);

			cv::hconcat(outDEM3, outDEM4, outDEM3);

			

			if (outDEM.size() != outDEM3.size())
			{
				resize(outDEM, outDEM, outDEM3.size());
			}
			if (outDEM2.size() != outDEM3.size())
			{
				resize(outDEM2, outDEM2, outDEM3.size());
			}

			cv::vconcat(outDEM, outDEM2, outDEM);
			cv::vconcat(outDEM, outDEM3, outDEM);

			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx[0] + 1;
			latLowerRight = xx[0] - 2;
			lonUpperLeft = yy[0];
			lonLowerRight = yy[0] + 2;
		}
		//两行三列
		else if(xx[0] == xx[2] && xx[0] != xx[3])
		{
			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[2];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			cv::hconcat(outDEM, outDEM2, outDEM);
			cv::hconcat(outDEM, outDEM3, outDEM);

			path = DEMPath + string("\\") + CopernicusDEMFileName[3];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[4];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[5];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM4.rows);
			lonSpacing = 1.0 / double(outDEM4.cols);

			cv::hconcat(outDEM2, outDEM3, outDEM2);
			cv::hconcat(outDEM2, outDEM4, outDEM2);




			if (outDEM.size() != outDEM2.size())
			{
				resize(outDEM, outDEM, outDEM2.size());
			}

			cv::vconcat(outDEM, outDEM2, outDEM);

			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx[0] + 1;
			latLowerRight = xx[0] - 1;
			lonUpperLeft = yy[0];
			lonLowerRight = yy[0] + 3;
		}




		

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	//DEM在8个方格内	
	else if (CopernicusDEMFileName.size() == 8)
	{
		int xx[8], yy[8]/*, temp*/;

		for (int i = 0; i < 8; i++)
		{
			if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
				xx[i] = -xx[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
				xx[i] = -xx[i];
			}
			else
			{
				return -1;
			}
		}
		Mat outDEM, outDEM2, outDEM3, outDEM4, outDEM5;
		//四行两列
		if (xx[0] != xx[2] && xx[0] == xx[1])
		{
			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			cv::hconcat(outDEM, outDEM2, outDEM);

			path = DEMPath + string("\\") + CopernicusDEMFileName[2];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[3];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}

			cv::hconcat(outDEM2, outDEM3, outDEM2);

			path = DEMPath + string("\\") + CopernicusDEMFileName[4];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[5];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}
		

			cv::hconcat(outDEM3, outDEM4, outDEM3);

			path = DEMPath + string("\\") + CopernicusDEMFileName[6];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[7];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
			if (ret < 0)//未下载到用0填充
			{
				outDEM5 = Mat::zeros(outDEM4.rows, outDEM4.cols, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM5.rows);
			lonSpacing = 1.0 / double(outDEM5.cols);

			cv::hconcat(outDEM4, outDEM5, outDEM4);



			if (outDEM.size() != outDEM4.size())
			{
				resize(outDEM, outDEM, outDEM4.size());
			}
			if (outDEM2.size() != outDEM4.size())
			{
				resize(outDEM2, outDEM2, outDEM4.size());
			}
			if (outDEM3.size() != outDEM4.size())
			{
				resize(outDEM3, outDEM3, outDEM4.size());
			}

			cv::vconcat(outDEM, outDEM2, outDEM);
			cv::vconcat(outDEM, outDEM3, outDEM);
			cv::vconcat(outDEM, outDEM4, outDEM);

			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx[0] + 1;
			latLowerRight = xx[0] - 3;
			lonUpperLeft = yy[0];
			lonLowerRight = yy[0] + 2;
		}
		//两行四列
		else
		{
			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[2];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[3];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}

			cv::hconcat(outDEM, outDEM2, outDEM);
			cv::hconcat(outDEM, outDEM3, outDEM);
			cv::hconcat(outDEM, outDEM4, outDEM);

			path = DEMPath + string("\\") + CopernicusDEMFileName[4];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[5];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[6];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[7];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
			if (ret < 0)//未下载到用0填充
			{
				outDEM5 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM5.rows);
			lonSpacing = 1.0 / double(outDEM5.cols);

			cv::hconcat(outDEM2, outDEM3, outDEM2);
			cv::hconcat(outDEM2, outDEM4, outDEM2);
			cv::hconcat(outDEM2, outDEM5, outDEM2);




			if (outDEM.size() != outDEM2.size())
			{
				resize(outDEM, outDEM, outDEM2.size());
			}

			cv::vconcat(outDEM, outDEM2, outDEM);

			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx[0] + 1;
			latLowerRight = xx[0] - 1;
			lonUpperLeft = yy[0];
			lonLowerRight = yy[0] + 4;
		}






		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	//DEM在9个方格内
	else if (CopernicusDEMFileName.size() == 9)
	{
		int xx[9], yy[9]/*, temp*/;

		for (int i = 0; i < 9; i++)
		{
			if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
				xx[i] = -xx[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
				xx[i] = -xx[i];
			}
			else
			{
				return -1;
			}
		}
		Mat outDEM, outDEM2, outDEM3, outDEM4, outDEM5;
		//三行三列
		string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
		if (ret < 0)//未下载到用0填充
		{
			outDEM = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[1];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
		if (ret < 0)//未下载到用0填充
		{
			outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
		}

		

		path = DEMPath + string("\\") + CopernicusDEMFileName[2];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
		if (ret < 0)//未下载到用0填充
		{
			outDEM3 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
		}

		cv::hconcat(outDEM, outDEM2, outDEM);
		cv::hconcat(outDEM, outDEM3, outDEM);


		path = DEMPath + string("\\") + CopernicusDEMFileName[3];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
		if (ret < 0)//未下载到用0填充
		{
			outDEM2 = Mat::zeros(3600, 3600, CV_32F);
		}


		path = DEMPath + string("\\") + CopernicusDEMFileName[4];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
		if (ret < 0)//未下载到用0填充
		{
			outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
		}


		path = DEMPath + string("\\") + CopernicusDEMFileName[5];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
		if (ret < 0)//未下载到用0填充
		{
			outDEM4 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
		}

		cv::hconcat(outDEM2, outDEM3, outDEM2);
		cv::hconcat(outDEM2, outDEM4, outDEM2);

		path = DEMPath + string("\\") + CopernicusDEMFileName[6];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
		if (ret < 0)//未下载到用0填充
		{
			outDEM3 = Mat::zeros(3600, 3600, CV_32F);
		}

		path = DEMPath + string("\\") + CopernicusDEMFileName[7];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
		if (ret < 0)//未下载到用0填充
		{
			outDEM4 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
		}


		path = DEMPath + string("\\") + CopernicusDEMFileName[8];
		std::replace(path.begin(), path.end(), '/', '\\');
		ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
		if (ret < 0)//未下载到用0填充
		{
			outDEM5 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
		}
		latSpacing = 1.0 / double(outDEM5.rows);
		lonSpacing = 1.0 / double(outDEM5.cols);

		cv::hconcat(outDEM3, outDEM4, outDEM3);
		cv::hconcat(outDEM3, outDEM5, outDEM3);



		if (outDEM.size() != outDEM3.size())
		{
			resize(outDEM, outDEM, outDEM3.size());
		}
		if (outDEM2.size() != outDEM3.size())
		{
			resize(outDEM2, outDEM2, outDEM3.size());
		}

		cv::vconcat(outDEM, outDEM2, outDEM);
		cv::vconcat(outDEM, outDEM3, outDEM);

		total_rows = outDEM.rows;
		total_cols = outDEM.cols;

		latUpperLeft = xx[0] + 1;
		latLowerRight = xx[0] - 2;
		lonUpperLeft = yy[0];
		lonLowerRight = yy[0] + 3;

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	//DEM在12个方格内
	else if (CopernicusDEMFileName.size() == 12)
	{
		int xx[12], yy[12]/*, temp*/;

		for (int i = 0; i < 12; i++)
		{
			if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("N") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_N%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("E"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_E%d_00_DEM.tif", &xx[i], &yy[i]);
				xx[i] = -xx[i];
			}
			else if (CopernicusDEMFileName[i].substr(22, 1) == string("S") && CopernicusDEMFileName[i].substr(29, 1) == string("W"))
			{
				sscanf(CopernicusDEMFileName[i].c_str(), "Copernicus_DSM_COG_10_S%d_00_W%d_00_DEM.tif", &xx[i], &yy[i]);
				yy[i] = -yy[i];
				xx[i] = -xx[i];
			}
			else
			{
				return -1;
			}
		}
		Mat outDEM, outDEM2, outDEM3, outDEM4, outDEM5, outDEM6;
		//三行四列
		if (xx[0] != xx[4] && xx[0] == xx[3])
		{
			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[2];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[3];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}


			cv::hconcat(outDEM, outDEM2, outDEM);
			cv::hconcat(outDEM, outDEM3, outDEM);
			cv::hconcat(outDEM, outDEM4, outDEM);


			path = DEMPath + string("\\") + CopernicusDEMFileName[4];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[5];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}



			path = DEMPath + string("\\") + CopernicusDEMFileName[6];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[7];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
			if (ret < 0)//未下载到用0填充
			{
				outDEM5 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}

			cv::hconcat(outDEM2, outDEM3, outDEM2);
			cv::hconcat(outDEM2, outDEM4, outDEM2);
			cv::hconcat(outDEM2, outDEM5, outDEM2);

			path = DEMPath + string("\\") + CopernicusDEMFileName[8];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[9];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[10];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
			if (ret < 0)//未下载到用0填充
			{
				outDEM5 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}

			path = DEMPath + string("\\") + CopernicusDEMFileName[11];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM6);
			if (ret < 0)//未下载到用0填充
			{
				outDEM6 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}

			latSpacing = 1.0 / double(outDEM6.rows);
			lonSpacing = 1.0 / double(outDEM6.cols);

			cv::hconcat(outDEM3, outDEM4, outDEM3);
			cv::hconcat(outDEM3, outDEM5, outDEM3);
			cv::hconcat(outDEM3, outDEM6, outDEM3);



			if (outDEM.size() != outDEM3.size())
			{
				resize(outDEM, outDEM, outDEM3.size());
			}
			if (outDEM2.size() != outDEM3.size())
			{
				resize(outDEM2, outDEM2, outDEM3.size());
			}


			cv::vconcat(outDEM, outDEM2, outDEM);
			cv::vconcat(outDEM, outDEM3, outDEM);

			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx[0] + 1;
			latLowerRight = xx[0] - 2;
			lonUpperLeft = yy[0];
			lonLowerRight = yy[0] + 4;
		}
		//四行三列
		else
		{
			string path = DEMPath + string("\\") + CopernicusDEMFileName[0];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM);
			if (ret < 0)//未下载到用0填充
			{
				outDEM = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[1];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[2];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM.rows, outDEM.cols, CV_32F);
			}



			cv::hconcat(outDEM, outDEM2, outDEM);
			cv::hconcat(outDEM, outDEM3, outDEM);


			path = DEMPath + string("\\") + CopernicusDEMFileName[3];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM2);
			if (ret < 0)//未下载到用0填充
			{
				outDEM2 = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[4];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}



			path = DEMPath + string("\\") + CopernicusDEMFileName[5];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM2.rows, outDEM2.cols, CV_32F);
			}


			cv::hconcat(outDEM2, outDEM3, outDEM2);
			cv::hconcat(outDEM2, outDEM4, outDEM2);

			path = DEMPath + string("\\") + CopernicusDEMFileName[6];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM3);
			if (ret < 0)//未下载到用0填充
			{
				outDEM3 = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[7];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}



			path = DEMPath + string("\\") + CopernicusDEMFileName[8];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
			if (ret < 0)//未下载到用0填充
			{
				outDEM5 = Mat::zeros(outDEM3.rows, outDEM3.cols, CV_32F);
			}

			cv::hconcat(outDEM3, outDEM4, outDEM3);
			cv::hconcat(outDEM3, outDEM5, outDEM3);

			path = DEMPath + string("\\") + CopernicusDEMFileName[9];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM4);
			if (ret < 0)//未下载到用0填充
			{
				outDEM4 = Mat::zeros(3600, 3600, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[10];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM5);
			if (ret < 0)//未下载到用0填充
			{
				outDEM5 = Mat::zeros(outDEM4.rows, outDEM4.cols, CV_32F);
			}


			path = DEMPath + string("\\") + CopernicusDEMFileName[11];
			std::replace(path.begin(), path.end(), '/', '\\');
			ret = CopernicusDEM_geotiffread(path.c_str(), outDEM6);
			if (ret < 0)//未下载到用0填充
			{
				outDEM6 = Mat::zeros(outDEM4.rows, outDEM4.cols, CV_32F);
			}
			latSpacing = 1.0 / double(outDEM6.rows);
			lonSpacing = 1.0 / double(outDEM6.cols);

			cv::hconcat(outDEM4, outDEM5, outDEM4);
			cv::hconcat(outDEM4, outDEM6, outDEM4);



			if (outDEM.size() != outDEM4.size())
			{
				resize(outDEM, outDEM, outDEM4.size());
			}
			if (outDEM2.size() != outDEM4.size())
			{
				resize(outDEM2, outDEM2, outDEM4.size());
			}
			if (outDEM3.size() != outDEM4.size())
			{
				resize(outDEM3, outDEM3, outDEM4.size());
			}


			cv::vconcat(outDEM, outDEM2, outDEM);
			cv::vconcat(outDEM, outDEM3, outDEM);
			cv::vconcat(outDEM, outDEM4, outDEM);

			total_rows = outDEM.rows;
			total_cols = outDEM.cols;

			latUpperLeft = xx[0] + 1;
			latLowerRight = xx[0] - 3;
			lonUpperLeft = yy[0];
			lonLowerRight = yy[0] + 3;
		}

		startRow = cvRound((latUpperLeft - latMax) / latSpacing);
		startRow = startRow < 1 ? 1 : startRow;
		startRow = startRow > total_rows ? total_rows : startRow;
		endRow = cvRound((latUpperLeft - latMin) / latSpacing);
		endRow = endRow < 1 ? 1 : endRow;
		endRow = endRow > total_rows ? total_rows : endRow;
		if (fabs(lonMax - lonMin) > 180.0)
		{
			startCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMin + 360.0 - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}
		else
		{
			startCol = cvRound((lonMin - lonUpperLeft) / lonSpacing);
			startCol = startCol < 1 ? 1 : startCol;
			startCol = startCol > total_cols ? total_cols : startCol;
			endCol = cvRound((lonMax - lonUpperLeft) / lonSpacing);
			endCol = endCol < 1 ? 1 : endCol;
			endCol = endCol > total_cols ? total_cols : endCol;
		}

		outDEM(cv::Range(startRow - 1, endRow), cv::Range(startCol - 1, endCol)).copyTo(DEM_out);
		*lonUL = lonUpperLeft + (startCol - 1) * lonSpacing;
		*latUL = latUpperLeft - (startRow - 1) * latSpacing;
		*lon_spacing = lonSpacing;
		*lat_spacing = latSpacing;
	}
	else return -1;
	return 0;
}

int Utils::getSRTMFileName(double lonMin, double lonMax, double latMin, double latMax, vector<string>& name)
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
	int maxRows = 24; int maxCols = 72; int startRow, endRow, startCol, endCol;
	double spacing = 5.0;
	startRow = (int)((60.0 - latMax) / spacing) + 1;
	endRow = (int)((60.0 - latMin) / spacing) + 1;
	startCol = (int)((lonMin + 180.0) / spacing) + 1;
	endCol = (int)((lonMax + 180.0) / spacing) + 1;
	if (startRow == endRow)
	{
		if (startCol == endCol)
		{
			memset(tmp, 0, 512);
			if (startCol < 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", startCol, startRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", startCol, startRow);
			}
			name.push_back(string(tmp));
		}
		else
		{
			memset(tmp, 0, 512);
			if (startCol < 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", startCol, startRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", startCol, startRow);
			}
			name.push_back(string(tmp));


			memset(tmp, 0, 512);
			if (endCol < 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", endCol, startRow);
			}
			else if (endCol >= 10 && startRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", endCol, startRow);
			}
			else if (endCol >= 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", endCol, startRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", endCol, startRow);
			}
			name.push_back(string(tmp));
		}
	}
	else
	{
		if (startCol == endCol)
		{
			memset(tmp, 0, 512);
			if (startCol < 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", startCol, startRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", startCol, startRow);
			}
			name.push_back(string(tmp));

			memset(tmp, 0, 512);
			if (startCol < 10 && endRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", startCol, endRow);
			}
			else if (startCol >= 10 && endRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", startCol, endRow);
			}
			else if (startCol >= 10 && endRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", startCol, endRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", startCol, endRow);
			}
			name.push_back(string(tmp));
		}
		else
		{
			memset(tmp, 0, 512);
			if (startCol < 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", startCol, startRow);
			}
			else if (startCol >= 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", startCol, startRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", startCol, startRow);
			}
			name.push_back(string(tmp));


			memset(tmp, 0, 512);
			if (endCol < 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", endCol, startRow);
			}
			else if (endCol >= 10 && startRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", endCol, startRow);
			}
			else if (endCol >= 10 && startRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", endCol, startRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", endCol, startRow);
			}
			name.push_back(string(tmp));


			memset(tmp, 0, 512);
			if (endCol < 10 && endRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", endCol, endRow);
			}
			else if (endCol >= 10 && endRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", endCol, endRow);
			}
			else if (endCol >= 10 && endRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", endCol, endRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", endCol, endRow);
			}
			name.push_back(string(tmp));

			memset(tmp, 0, 512);
			if (startCol < 10 && endRow < 10)
			{
				sprintf(tmp, "srtm_0%d_0%d.zip", startCol, endRow);
			}
			else if (startCol >= 10 && endRow >= 10)
			{
				sprintf(tmp, "srtm_%d_%d.zip", startCol, endRow);
			}
			else if (startCol >= 10 && endRow < 10)
			{
				sprintf(tmp, "srtm_%d_0%d.zip", startCol, endRow);
			}
			else
			{
				sprintf(tmp, "srtm_0%d_%d.zip", startCol, endRow);
			}
			name.push_back(string(tmp));
		}
	}
	return 0;
}

int Utils::getCopernicusDEMFileName(double lonMin, double lonMax, double latMin, double latMax, vector<string>& name)
{
	if (fabs(lonMin) > 180.0 ||
		fabs(lonMax) > 180.0 ||
		fabs(latMin) >= 90.0 ||
		fabs(latMax) >= 90.0
		)
	{
		fprintf(stderr, "getCopernicusDEMFileName(): input check failed!\n");
		return -1;
	}
	name.clear();
	char tmp[512];
	int startLat, endLat, startLon, startLon2, endLon, endLon2;
	startLat = static_cast<int>(ceil(latMax));
	endLat = static_cast<int>(floor(latMin));
	startLon = static_cast<int>(floor(lonMin));
	startLon2 = startLon;
	endLon = static_cast<int>(ceil(lonMax));
	endLon2 = endLon;
	if (fabs(lonMax - lonMin) > 180.0)//crossing the 180°/-180° longitude line
	{
		endLon2 = static_cast<int>(ceil(lonMax + fabs(lonMax - lonMin)));
		startLon2 = static_cast<int>(floor(lonMax));
	}
	for (int i = int(startLat); i > int(endLat); i--)
	{
		string NS_sign = i - 1 >= 0 ? "N" : "S";
		memset(tmp, 0, 512);
		if (abs(i - 1) < 10)
		{
			sprintf(tmp, "0%d", abs(i - 1));
		}
		else
		{
			sprintf(tmp, "%d", abs(i - 1));
		}
		NS_sign = NS_sign + tmp;
		for (int j = int(startLon2); j < int(endLon2); j++)
		{	
			if (j >= 180) j = j - 360;
			string EW_sign = j >= 0 ? "E" : "W";
			memset(tmp, 0, 512);
			snprintf(tmp, sizeof(tmp), "%03d", abs(j));
			EW_sign = EW_sign + tmp;
			string filename = "Copernicus_DSM_COG_10_" + NS_sign + "_00_" + EW_sign + "_00_DEM.tif";
			name.push_back(filename);
		}
	}
	return 0;
}

int Utils::downloadSRTM(const char* name, const char* DEMpath)
{
	bool isConnect;
	DWORD dw;
	isConnect = IsNetworkAlive(&dw);
	if (!isConnect)
	{
		fprintf(stderr, "downloadSRTM(): network is not connected!\n");
		return -1;
	}
	// removed unused: ret (URLDownloadToFileA returns HRESULT, not int)
	string url = string(SRTMURL) + name;
	string savefile = DEMpath + string("\\") + name;
	std::replace(savefile.begin(), savefile.end(), '/', '\\');
	HRESULT Result = URLDownloadToFileA(NULL, url.c_str(), savefile.c_str(), 0, NULL);
	if (Result != S_OK)
	{
		fprintf(stderr, "downloadSRTM(): download failded!\n");
		return -1;
	}
	return 0;
}

int Utils::downloadCopernicusDEM(const char* name, const char* DEMpath)
{
	bool isConnect;
	DWORD dw;
	isConnect = IsNetworkAlive(&dw);
	if (!isConnect)
	{
		fprintf(stderr, "downloadCopernicusDEM(): network is not connected!\n");
		return -1;
	}
	/*int ret;*/
	string folder = name;
	folder = folder.substr(0, folder.length() - 4);
	string url = string(CopernicusDEMURL) + folder + "/" + name;
	string savefile = DEMpath + string("/") + name;
	std::replace(savefile.begin(), savefile.end(), '/', '\\');
	HRESULT Result = URLDownloadToFileA(NULL, url.c_str(), savefile.c_str(), 0, NULL);
	if (Result != S_OK)
	{
		fprintf(stderr, "downloadCopernicusDEM(): download failded!\n");
		return -1;
	}
	return 0;
}

int Utils::CopernicusDEM_geotiffread(const char* filename, Mat& outDEM)
{
	if (!filename)
		return -1;

	InitializeGDALAndProjOnce();   // 线程安全注册驱动与配置 PROJ 路径

	GDALDatasetH hDataset = GDALOpen(filename, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr,
			"CopernicusDEM_geotiffread(): failed to open %s!\n",
			filename);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand != 1)
	{
		fprintf(stderr,
			"CopernicusDEM_geotiffread(): number of Bands != 1\n");
		GDALClose(hDataset);
		return -1;
	}

	GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
	if (hBand == NULL)
	{
		fprintf(stderr,
			"CopernicusDEM_geotiffread(): failed to get band!\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand);   // cols
	int ysize = GDALGetRasterBandYSize(hBand);   // rows

	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr,
			"CopernicusDEM_geotiffread(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	GDALDataType dataType = GDALGetRasterDataType(hBand);
	/* Copernicus DEM 通常是 GDT_Int16 或 GDT_Float32 */

	float* pbuf = (float*)malloc(sizeof(float) * xsize * ysize);
	if (!pbuf)
	{
		fprintf(stderr,
			"CopernicusDEM_geotiffread(): out of memory!\n");
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Float32,   /* 直接转成 float */
		0, 0) != CE_None)
	{
		fprintf(stderr,
			"CopernicusDEM_geotiffread(): RasterIO failed!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	outDEM.create(ysize, xsize, CV_32F);
	memcpy(outDEM.data, pbuf, sizeof(float) * xsize * ysize);

	free(pbuf);
	GDALClose(hDataset);

	return 0;
}

int Utils::writeOverlayKML(
	double BottomLeft_lon,
	double BottomLeft_lat,
	double BottomRight_lon,
	double BottomRight_lat,
	double TopRight_lon,
	double TopRight_lat, 
	double TopLeft_lon,
	double TopLeft_lat, 
	double Reference_lon,
	double Reference_lat,
	const char* image_file,
	const char* KML_file, 
	const char* Legend_file
)
{
	if (fabs(BottomLeft_lat) > 90.0 ||
		fabs(BottomRight_lat) > 90.0 ||
		fabs(TopLeft_lat) > 90.0 ||
		fabs(TopRight_lat) > 90.0 ||
		fabs(Reference_lat) > 90.0 ||
		fabs(BottomLeft_lon) > 180.0 ||
		fabs(BottomRight_lon) > 180.0 ||
		fabs(TopLeft_lon) > 180.0 ||
		fabs(TopRight_lon) > 180.0 ||
		fabs(Reference_lon) > 180.0 ||
		!image_file ||
		!KML_file
		)
	{
		fprintf(stderr, "writeOverlayKML(): input check failed!\n");
		return -1;
	}
	TiXmlDocument doc;
	TiXmlDeclaration* declaration = new TiXmlDeclaration("1.0", "UTF-8", "yes");
	doc.LinkEndChild(declaration);
	TiXmlElement* Root = new TiXmlElement("kml");
	Root->SetAttribute("xmlns:gx", "http://www.google.com/kml/ext/2.2");
	doc.LinkEndChild(Root);
	TiXmlElement* Document = new TiXmlElement("Document");
	Root->LinkEndChild(Document);

	TiXmlElement* name = new TiXmlElement("name");
	TiXmlText* content = new TiXmlText("SatExplorer Product Map Overlay");
	name->LinkEndChild(content);
	Document->LinkEndChild(name);

	TiXmlElement* Folder = new TiXmlElement("Folder");
	Document->LinkEndChild(Folder);

	TiXmlElement* name2 = new TiXmlElement("name");
	TiXmlText* content2 = new TiXmlText("SatExplorer Product Scene Overlay");
	name2->LinkEndChild(content2);
	Folder->LinkEndChild(name2);

	TiXmlElement* GroundOverlay = new TiXmlElement("GroundOverlay");
	Folder->LinkEndChild(GroundOverlay);

	TiXmlElement* name3 = new TiXmlElement("name");
	TiXmlText* content3 = new TiXmlText("SatExplorer Product Image Overlay");
	name3->LinkEndChild(content3);
	GroundOverlay->LinkEndChild(name3);

	TiXmlElement* Icon = new TiXmlElement("Icon");
	GroundOverlay->LinkEndChild(Icon);

	TiXmlElement* href1 = new TiXmlElement("href");
	TiXmlText* content4 = new TiXmlText(image_file);
	href1->LinkEndChild(content4);
	Icon->LinkEndChild(href1);

	TiXmlElement* LatLonQuad = new TiXmlElement("gx:LatLonQuad");
	GroundOverlay->LinkEndChild(LatLonQuad);

	TiXmlElement* coordinates = new TiXmlElement("coordinates");

	char coordinates_content[1024];
	sprintf(coordinates_content, "%lf,%lf %lf,%lf %lf,%lf %lf,%lf", BottomLeft_lon, BottomLeft_lat,
		BottomRight_lon, BottomRight_lat, TopRight_lon, TopRight_lat, TopLeft_lon, TopLeft_lat);

	TiXmlText* content5 = new TiXmlText(coordinates_content);
	coordinates->LinkEndChild(content5);
	LatLonQuad->LinkEndChild(coordinates);
	
	//如果有图例文件
	if (Legend_file)
	{
		TiXmlElement* Folder2 = new TiXmlElement("Folder");
		Document->LinkEndChild(Folder2);

		TiXmlElement* name22 = new TiXmlElement("name");
		TiXmlText* content22 = new TiXmlText("SatExplorer Product Screen Overlays");
		name22->LinkEndChild(content22);
		Folder2->LinkEndChild(name22);

		TiXmlElement* ScreenOverlay = new TiXmlElement("ScreenOverlay");
		Folder2->LinkEndChild(ScreenOverlay);

		TiXmlElement* name32 = new TiXmlElement("name");
		TiXmlText* content32 = new TiXmlText("SatExplorer Product Image Legend");
		name32->LinkEndChild(content32);
		ScreenOverlay->LinkEndChild(name32);

		TiXmlElement* Icon2 = new TiXmlElement("Icon");
		ScreenOverlay->LinkEndChild(Icon2);

		TiXmlElement* href2 = new TiXmlElement("href");
		TiXmlText* content42 = new TiXmlText(Legend_file);
		href2->LinkEndChild(content42);
		Icon2->LinkEndChild(href2);

		TiXmlElement* overlayXY = new TiXmlElement("overlayXY");
		overlayXY->SetAttribute("x", "0");
		overlayXY->SetAttribute("y", "1");
		overlayXY->SetAttribute("xunits", "fraction");
		overlayXY->SetAttribute("yunits", "fraction");
		ScreenOverlay->LinkEndChild(overlayXY);

		TiXmlElement* screenXY = new TiXmlElement("screenXY");
		screenXY->SetAttribute("x", "0");
		screenXY->SetAttribute("y", "1");
		screenXY->SetAttribute("xunits", "fraction");
		screenXY->SetAttribute("yunits", "fraction");
		ScreenOverlay->LinkEndChild(screenXY);

		TiXmlElement* rotationXY = new TiXmlElement("rotationXY");
		rotationXY->SetAttribute("x", "0");
		rotationXY->SetAttribute("y", "0");
		rotationXY->SetAttribute("xunits", "fraction");
		rotationXY->SetAttribute("yunits", "fraction");
		ScreenOverlay->LinkEndChild(rotationXY);

		TiXmlElement* size = new TiXmlElement("size");
		size->SetAttribute("x", "0");
		size->SetAttribute("y", "0");
		size->SetAttribute("xunits", "fraction");
		size->SetAttribute("yunits", "fraction");
		ScreenOverlay->LinkEndChild(size);
	}

	//参考点标记
	TiXmlElement* Placemark = new TiXmlElement("Placemark");
	Document->LinkEndChild(Placemark);

	TiXmlElement* name23 = new TiXmlElement("name");
	TiXmlText* content23 = new TiXmlText("Reference Point");
	name23->LinkEndChild(content23);
	Placemark->LinkEndChild(name23);

	TiXmlElement* Point = new TiXmlElement("Point");
	Placemark->LinkEndChild(Point);

	TiXmlElement* coordinates_ref = new TiXmlElement("coordinates");
	sprintf(coordinates_content, "%lf,%lf", Reference_lon, Reference_lat);
	TiXmlText* content55 = new TiXmlText(coordinates_content);
	coordinates_ref->LinkEndChild(content55);
	Point->LinkEndChild(coordinates_ref);


	if (!doc.SaveFile(KML_file))
	{
		fprintf(stderr, "writeOverlayKML(): failed to save %s! \n", KML_file);
		return -1;
	}
	return 0;
}
