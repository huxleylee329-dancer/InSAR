// test.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include<stdlib.h>
#include<iostream>
#include<fstream>
#include<vector>
#include<stdio.h>
#include<io.h>
#include <atlconv.h>
#include<Windows.h>
#include"opencv2\core\core.hpp"
#include"opencv2\highgui\highgui.hpp"
#include"opencv2\imgproc\imgproc.hpp"
#include"opencv2\opencv.hpp"
#include <omp.h>  /*多线程计算库*/
#include"..\include\ComplexMat.h"
#include"..\include\Utils.h"
#include"..\include\Unwrap.h"
#include"..\include\Registration.h"
#include"..\include\Filter.h"
#include"..\include\Deflat.h"
#include"..\include\Dem.h"
#include"..\include\FormatConversion.h"
#include"..\include\sar_comm.h"
#include"gdal.h"
#include"gdal_priv.h"
#include<time.h>
#include<queue>

#ifdef _DEBUG
#pragma comment(lib, "Utils_d.lib")
#pragma comment(lib, "Unwrap_d.lib")
#pragma comment(lib, "Registration_d.lib")
#pragma comment(lib, "Filter_d.lib")
#pragma comment(lib, "Deflat_d.lib")
#pragma comment(lib, "Dem_d.lib")
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "FormatConversion_d.lib")
#else
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "Unwrap.lib")
#pragma comment(lib, "Registration.lib")
#pragma comment(lib, "Filter.lib")
#pragma comment(lib, "Deflat.lib")
#pragma comment(lib, "Dem.lib")
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "FormatConversion.lib")
#endif // _DEBUG
using cv::Range;
typedef union FLOAT_CONV
{
	float				f;
	char				c[4];
}FLOATCONV;
float BLEndianFloat(float fValue)
{
	FLOAT_CONV			d1, d2;

	d1.f = fValue;
	d2.c[0] = d1.c[3];
	d2.c[1] = d1.c[2];
	d2.c[2] = d1.c[1];
	d2.c[3] = d1.c[0];

	return d2.f;
}
#ifndef Big2Little16
#define Big2Little16(A) ((uint16_t)(A&0xff00)>>8|(uint16_t)(A&0x00ff)<<8)
#endif
#define Big2Little32(A) ((uint32_t)(A&0xff000000)>>24|(uint32_t)(A&0x00ff0000)>>8 | (uint32_t)(A&0x0000ff00)<<8|(uint32_t)(A&0x000000ff)<<24)
#define Big2Little64(A) ((uint64_t)(A&0xff00000000000000)>>56|(A&0x00ff000000000000)>>40|(A&0x0000ff0000000000)>>24|(A&0x000000ff00000000)>>8|(A&0x00000000ff000000)<<8|(A&0x0000000000ff0000)<<24|(A&0x000000000000ff00)<<40|(A&0x00000000000000ff)<<56)
inline float ReverseFloat(const float inFloat)
{
	float retVal;
	unsigned char* floatToConvert = (unsigned char*)&inFloat;
	unsigned char* returnFloat = (unsigned char*)&retVal;

	// swap the bytes into a temporary buffer
	returnFloat[0] = floatToConvert[3];
	returnFloat[1] = floatToConvert[2];
	returnFloat[2] = floatToConvert[1];
	returnFloat[3] = floatToConvert[0];

	return retVal;
}
inline double Reversedouble(const double inDouble)
{
	double retVal;
	unsigned char* doubleToConvert = (unsigned char*)&inDouble;
	unsigned char* returnDouble = (unsigned char*)&retVal;

	// swap the bytes into a temporary buffer
	returnDouble[0] = doubleToConvert[7];
	returnDouble[1] = doubleToConvert[6];
	returnDouble[2] = doubleToConvert[5];
	returnDouble[3] = doubleToConvert[4];
	returnDouble[4] = doubleToConvert[3];
	returnDouble[5] = doubleToConvert[2];
	returnDouble[6] = doubleToConvert[1];
	returnDouble[7] = doubleToConvert[0];

	return retVal;
}

/*读取BioSAR2008数据*/
//int main(int argc, char* argv[])
//{
//	//FormatConversion conversion; Utils util; Unwrap unwrap; Filter filter;
//	//Mat phase, phase_filter, phase_unwrap, residue, coherence, R_main, R_slave, phase_real;
//	//conversion.read_array_from_h5("G:\\14_project\\test_dualfreqpingpong2\\aoi3\\master_master2_regis_cut2.h5", "R_main", R_main);
//	//conversion.read_array_from_h5("G:\\14_project\\test_dualfreqpingpong2\\aoi3\\master_master2_regis_cut2.h5", "R_slave", R_slave);
//	//double lambda = VEL_C / 9.649999315E9;
//	//phase_real = (R_slave - R_main) / lambda * 4 * PI;
//	//util.cvmat2bin("G:\\14_project\\test_dualfreqpingpong2\\aoi3_ifg2\\phase_real.bin", phase_real);
//	//string phase_file = "G:\\14_project\\test_dualfreqpingpong2\\aoi3_ifg2\\master_master_regis_cut2_slave_slave_regis_cut2.h5";
//	//conversion.read_array_from_h5(phase_file.c_str(), "phase", phase);
//	//filter.Goldstein_filter(phase, phase_filter, 0.8, 128, 32);
//	//util.residue(phase_filter, residue);
//	//util.phase_coherence(phase_filter, coherence);
//	//unwrap.MCF(phase_filter, phase_unwrap, coherence, residue, "G:\\14_project\\test_dualfreqpingpong2\\aoi3_ifg2\\mcf_problem.net",
//	//	"E:\\working_dir\\projects\\software\\14_software\\bin");
//	//util.cvmat2bin("G:\\14_project\\test_dualfreqpingpong2\\aoi3_ifg2\\phase_unwrap.bin", phase_unwrap);
//	//util.bin2cvmat("G:\\14_project\\test_dualfreqpingpong2\\aoi3_ifg2\\phase_unwrap.bin", phase_unwrap);
//	//util.savephase("G:\\14_project\\test_dualfreqpingpong2\\aoi3_ifg2\\phase_unwrap.jpg", "jet", phase_unwrap);
//
//	//return 0;
//	//ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	/*读取BioSAR2008数据*/
//	//FILE* fp = NULL;
//	//string data_file2 = "D:\\BiOSAR2008\\RGI\\08biosar0211x1_t01\\i08biosar0211x1_ch3_t01_slc.dat.final";
//	//string data_file = "D:\\BiOSAR2008\\RGI\\08biosar0201x1_t01\\i08biosar0201x1_ch3_t01_slc.dat";
//	//string flat_phase_file = "D:\\BiOSAR2008\\RGI\\08biosar0211x1_t01\\phi_flat08biosar0201x1_ch3_t01_08biosar0211x1_ch3_t01.dat";
//	//fp = fopen(data_file.c_str(), "rb");
//	//int N_rows = 0, N_cols = 0;
//	//fread(&N_cols, 4, 1, fp);
//	//fread(&N_rows, 4, 1, fp);
//	//N_rows = Big2Little32(N_rows);
//	//N_cols = Big2Little32(N_cols);
//	//slc.re.create(N_rows, N_cols, CV_64F);
//	//slc.im.create(N_rows, N_cols, CV_64F);
//	//float* data_point = (float*)malloc(N_rows * N_cols * 8);
//	//fread(data_point, sizeof(float), 2 * N_rows * N_cols, fp);
//	//fclose(fp);
//	//size_t offset = 0;
//	//for (int i = 0; i < N_rows; i++)
//	//{
//	//	for (int j = 0; j < N_cols; j++)
//	//	{
//	//		slc.re.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//	//		offset++;
//	//		slc.im.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//	//		offset++;
//	//	}
//	//}
//	//fp = fopen(data_file2.c_str(), "rb");
//	//N_rows = 0, N_cols = 0;
//	//fread(&N_cols, 4, 1, fp);
//	//fread(&N_rows, 4, 1, fp);
//	//N_rows = Big2Little32(N_rows);
//	//N_cols = Big2Little32(N_cols);
//	//slc2.re.create(N_rows, N_cols, CV_64F);
//	//slc2.im.create(N_rows, N_cols, CV_64F);
//	//fread(data_point, sizeof(float), 2 * N_rows * N_cols, fp);
////fclose(fp);
////offset = 0;
////for (int i = 0; i < N_rows; i++)
////{
////	for (int j = 0; j < N_cols; j++)
////	{
////		slc2.re.at<double>(i, j) = ReverseFloat(*(data_point + offset));
////		offset++;
////		slc2.im.at<double>(i, j) = ReverseFloat(*(data_point + offset));
////		offset++;
////	}
////}
////free(data_point);
////Mat phase, flat_phase;
////flat_phase = Mat::zeros(N_rows, N_cols, CV_64F);
////fp = fopen(flat_phase_file.c_str(), "rb");
////int h;
////fread(&h, sizeof(int), 1, fp);
////h = Big2Little32(h);
////double* flat_phase_pointer = NULL;
////flat_phase_pointer = (double*)malloc(N_cols * sizeof(double));
////fread(flat_phase_pointer, sizeof(double), N_cols, fp);
////offset = 0;
////for (int i = 0; i < N_cols; i++)
////{
////	for (int j = 0; j < N_rows; j++)
////	{
////		double tt = Reversedouble(*(flat_phase_pointer + offset));
////		flat_phase.at<double>(j, i) = Reversedouble(*(flat_phase_pointer + offset));
////	}
////	offset++;
////}
////free(flat_phase_pointer);
////fclose(fp);
////ComplexMat temp;
////util.phase2cos(flat_phase, temp.re, temp.im);
////util.wrap(flat_phase, flat_phase);
////slc2.Mul(temp, slc2, false);
////string master = "E:\\working_dir\\others\\wangyuan\\BioSAR2008\\L_band\\master_VV.h5";
////string slave = "E:\\working_dir\\others\\wangyuan\\BioSAR2008\\L_band\\slave5_VV.h5";
////string flat_phase_file_out = "E:\\working_dir\\others\\wangyuan\\BioSAR2008\\L_band\\slave5_VV_flat_phase.h5";
////conversion.creat_new_h5(master.c_str());
////conversion.creat_new_h5(slave.c_str());
////conversion.creat_new_h5(flat_phase_file_out.c_str());
////conversion.write_slc_to_h5(master.c_str(), slc);
////conversion.write_slc_to_h5(slave.c_str(), slc2);
////conversion.write_array_to_h5(flat_phase_file_out.c_str(), "flat_phase", flat_phase);
////util.Multilook(slc, slc2, 1, 3, phase);
////util.savephase("E:\\working_dir\\others\\wangyuan\\BioSAR2008\\L_band\\BioSAR_phase_L_VV_5.jpg", "jet", phase);
//////util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\BioSAR_flat_phase.jpg", "jet", flat_phase);
//////util.saveSLC("E:\\working_dir\\projects\\software\\InSAR\\bin\\BioSAR.jpg", 65, slc);
//
////////conversion.read_slc_from_h5("G:\\beijing\\deramped_vv\\20171210_iw3vv_regis_deramp.h5", slc);
////////conversion.read_slc_from_h5("G:\\beijing\\deramped_vv\\20180103_iw3vv_regis_deramp.h5", slc2);
////////Mat phase, amplitude;
////////util.Multilook(slc, slc2, 4, 1, phase);
////////amplitude = slc.GetMod();
////////util.multilook_SAR(amplitude, amplitude, 4, 1);
////////util.saveAmplitude("E:\\working_dir\\projects\\software\\InSAR\\bin\\amplitude.jpg", amplitude);
////////util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\phase.jpg", "jet", phase);
////return 0;
//
////double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing,
////	nearRangeTime, wavelength, prf, start, end;
////int sceneHeight, sceneWidth, offset_row = 0, offset_col = 0, multilook_rg, multilook_az;
////Mat lon_coef, lat_coef, dem, mappedDem, statevec;
////string start_time, end_time, master_file;
////FormatConversion conversion; Utils util;
////int ret;
////master_file = "G:\\stl\\SanAndreas\\20121010.h5";
////string demPath = "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem";
////ComplexMat slc, mapped_slc;
////ret = conversion.read_slc_from_h5(master_file.c_str(), slc);
////ret = conversion.read_int_from_h5(master_file.c_str(), "range_len", &sceneWidth);
////ret = conversion.read_int_from_h5(master_file.c_str(), "azimuth_len", &sceneHeight);
////ret = conversion.read_int_from_h5(master_file.c_str(), "offset_row", &offset_row);
////ret = conversion.read_int_from_h5(master_file.c_str(), "offset_col", &offset_col);
////ret = conversion.read_array_from_h5(master_file.c_str(), "lon_coefficient", lon_coef);
////ret = conversion.read_array_from_h5(master_file.c_str(), "lat_coefficient", lat_coef);
////ret = conversion.read_double_from_h5(master_file.c_str(), "prf", &prf);
////ret = conversion.read_double_from_h5(master_file.c_str(), "carrier_frequency", &wavelength);
////wavelength = VEL_C / wavelength;
////ret = conversion.read_double_from_h5(master_file.c_str(), "range_spacing", &rangeSpacing);
////ret = conversion.read_double_from_h5(master_file.c_str(), "slant_range_first_pixel", &nearRangeTime);
////nearRangeTime = 2.0 * nearRangeTime / VEL_C;
////ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_start_time", start_time);
////ret = conversion.utc2gps(start_time.c_str(), &start);
////ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_stop_time", end_time);
////ret = conversion.utc2gps(end_time.c_str(), &end);
////ret = conversion.read_array_from_h5(master_file.c_str(), "state_vec", statevec);
////ret = Utils::computeImageGeoBoundry(lat_coef, lon_coef, sceneHeight, sceneWidth, offset_row, offset_col,
////	&lonMax, &latMax, &lonMin, &latMin);
////ret = Utils::getSRTMDEM(demPath.c_str(), dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
////ret = util.geocode(dem, slc, 2, 2, mapped_slc, lonMin, latMax, offset_row, offset_col, sceneHeight, sceneWidth, prf, rangeSpacing,
////	wavelength, nearRangeTime, start, end, statevec, 5.0 / 6000.0, 5.0 / 6000.0);
//////conversion.creat_new_h5("G:\\test2\\greek\\geocoded_slc.h5");
//////conversion.write_slc_to_h5("G:\\test2\\greek\\geocoded_slc.h5", mapped_slc);
////util.SAR_image_quantify("G:\\stl\\SanAndreas\\\\geocoded_slc.jpg", 50, mapped_slc);
////return 0;
//
//{
//	////读取TropiSAR数据
//	//ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	//int nr = 6570, nc = 4000;
//	//slc.re.create(nr, nc, CV_32F);
//	//slc.im.create(nr, nc, CV_32F);
//	//FILE* fp = NULL;
//	//string file = "D:\\data\\TropiSAR\\Nouragues1\\P_Band\\tomo_data\\tropi0207\\Interfero_geom_0201\\tropi0207_Pproj_Hv_slc.dat";
//	//string h5_file = "D:\\data\\TropiSAR\\Nouragues1\\P_Band\\tomo_data\\tropi0207\\Interfero_geom_0201\\tropi0207_Pproj_Hv_slc.h5";
//	//fp = fopen(file.c_str(), "rb+");
//	//int magic; float dummy; short dd;
//	//float* data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	//void* data0 = malloc(nc * sizeof(float)*2 + 4);
//	//fread(data0, sizeof(char), nc * sizeof(float) * 2 + 4, fp);
//	//free(data0);
//	//fread((void*)data, sizeof(float) * nr * nc * 2, 1, fp);
//	//fclose(fp);
//	//size_t offset = 0;
//	//for (int i = 0; i < nr; i++)
//	//{
//	//	for (int j = 0; j < nc; j++)
//	//	{
//	//		slc.re.at<float>(i, j) = *(data + offset);
//	//		offset++;
//	//		slc.im.at<float>(i, j) = *(data + offset);
//	//		offset++;
//	//	}
//	//}
//
//	//free(data);
//	//conversion.creat_new_h5(h5_file.c_str());
//	//conversion.write_slc_to_h5(h5_file.c_str(), slc);
//	////util.saveSLC("D:\\data\\TropiSAR\\Nouragues1\\P_Band\\tomo_data\\tropi0201\\slc.jpg", 60, slc);
//	//return 0;
//}
//{
//	////读取AfriSAR数据
//	//ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	//string file, h5file;
//	//int nr = 12032, nc = 3772;
//	//string phase_dem_file = "D:\\data\\AfriSAR\\DLR\\FL06\\PS07\\TP01\\INF\\INF-SR\\pha_dem_16afrisr0602_16afrisr0607_P_tP01.rat";
//	//Mat phase(nr, nc, CV_32F);
//	//FILE* fp1 = NULL;
//	//fp1 = fopen(phase_dem_file.c_str(), "rb+");
//	//float* data00 = (float*)malloc(nr * nc * sizeof(float));
//	//void* data000 = malloc(1000);
//	//fread(data000, sizeof(char), 1000, fp1);
//	//free(data000);
//	//fread((void*)data00, sizeof(float)* nr* nc, 1, fp1);
//	//fclose(fp1);
//	//size_t offset = 0;
//	//for (int i = 0; i < nr; i++)
//	//{
//	//	for (int j = 0; j < nc; j++)
//	//	{
//	//		phase.at<float>(i, j) = *(data00 + offset);
//	//		offset++;
//	//	}
//	//}
//
//	//free(data00);
//	//ComplexMat flat;
//	//util.phase2cos(phase, flat.re, flat.im);
//
//
//
//	//file = "D:\\data\\AfriSAR\\DLR\\FL06\\PS07\\TP01\\INF\\INF-SR\\slc_coreg_16afrisr0602_16afrisr0607_Phh_tP01.rat";
//	//h5file = "D:\\data\\AfriSAR\\DLR\\FL06\\PS07\\TP01\\INF\\INF-SR\\slc_coreg_16afrisr0602_16afrisr0607_Phh_tP01.h5";
//	//
//	//slc.re.create(nr, nc, CV_32F);
//	//slc.im.create(nr, nc, CV_32F);
//	//FILE* fp = NULL;
//	//fp = fopen(file.c_str(), "rb+");
//	//int magic; float dummy; short dd;
//	//float* data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	//void* data0 = malloc(1000);
//	//fread(data0, sizeof(char), 1000, fp);
//	//free(data0);
//	//fread((void*)data, sizeof(float)* nr* nc * 2, 1, fp);
//	//fclose(fp);
//	//size_t offset2 = 0;
//	//for (int i = 0; i < nr; i++)
//	//{
//	//	for (int j = 0; j < nc; j++)
//	//	{
//	//		slc.re.at<float>(i, j) = *(data + offset2);
//	//		offset2++;
//	//		slc.im.at<float>(i, j) = *(data + offset2);
//	//		offset2++;
//	//	}
//	//}
//
//	//free(data);
//	//slc.Mul(flat, slc, false);
//	//conversion.creat_new_h5(h5file.c_str());
//	//conversion.write_slc_to_h5(h5file.c_str(), slc);
//	////util.saveSLC("D:\\data\\AfriSAR\\DLR\\FL06\\PS07\\TP01\\INF\\INF-SR\\slc_hh.jpg", 60, slc);
//	//return 0;
//}
//{
//	////读取TropiSAR DTM数据
//	//Utils util; FormatConversion conversion;
//	//int ncols = 2953; int nrows = 4369;
//	//Mat DTM(nrows, ncols, CV_64F); DTM = 0.0;
//	//const char* file = "D:\\data\\TropiSAR\\LIDAR_data_over_Paracaou\\DTM.asc";
//	//const char* h5file = "D:\\data\\TropiSAR\\LIDAR_data_over_Paracaou\\DTM.h5";
//	//FILE* fp = NULL;
//	//fp = fopen(file, "rt");
//	//char* data = (char*)malloc(ncols * 10);
//	//char* ptr;
//	//double val;
//	//fgets(data, ncols * 10, fp);
//	//fgets(data, ncols * 10, fp);
//	//fgets(data, ncols * 10, fp);
//	//fgets(data, ncols * 10, fp);
//	//fgets(data, ncols * 10, fp);
//	//fgets(data, ncols * 10, fp);
//	//memset(data, 0, ncols * 10);
//	//for (int i = 0; i < nrows; i++)
//	//{
//	//	fgets(data, ncols * 10, fp);
//	//	val = strtod(data, &ptr);
//	//	DTM.at<double>(i, 0) = val < -10.0 ? 0.0 : val;
//	//	for (int j = 1; j < ncols; j++)
//	//	{
//	//		val = strtod(ptr, &ptr);
//	//		DTM.at<double>(i, j) = val < -10.0 ? 0.0 : val;
//	//	}
//	//}
//	//conversion.creat_new_h5(h5file);
//	//conversion.write_array_to_h5(h5file, "DTM", DTM);
//	//util.savephase("D:\\data\\TropiSAR\\LIDAR_data_over_Paracaou\\DTM.jpg", "jet", DTM);
//	//return 0;
//}
//}

/*读取BioSAR2008数据*/
//int main(int argc, char* argv[])
//{
//	ComplexMat slc, slc2; FormatConversion conversion; Utils util;
//	FILE* fp = NULL;
//	string data_file2 = "D:\\data\\INDREX_2_data\\indrex_2\\mawas_c\\rgi\\rgi_04indrex1108x1_t11\\i04indrex1108x1_ch1_t11_slc.dat";
//	string data_file = "D:\\data\\INDREX_2_data\\indrex_2\\mawas_c\\rgi\\rgi_04indrex1108x1_t11\\i04indrex1108x1_ch1_t11_slc.dat";
//	//string master = "D:\\data\\BioSAR2008\\RGI\\08biosar0103x1_t01\\P_band_0103_HH.h5";
//	string slave = "D:\\data\\INDREX_2_data\\indrex_2\\mawas_c\\rgi\\rgi_04indrex1108x1_t11\\i04indrex1108x1_ch1_t11_slc.h5";
//	//string flat_phase_file = "D:\\data\\INDREX_2_data\\indrex_2\\mawas_c\\gtc\\gtc_04indrex1108x1_t11\\phi_flat08biosar0103x1_ch1_t01_08biosar0111x1_ch1_t01.dat";
//	//string kz_file = "D:\\data\\BioSAR2008\\RGI\\08biosar0105x1_t01\\kz08biosar0103x1_t01_08biosar0105x1_t01_slc.dat";
//	//string kz_h5file = "D:\\data\\BioSAR2008\\RGI\\08biosar0105x1_t01\\kz_0105.h5";
//	
//	//主图像
//	fp = fopen(data_file.c_str(), "rb");
//	int N_rows = 0, N_cols = 0;
//	fread(&N_cols, 4, 1, fp);
//	fread(&N_rows, 4, 1, fp);
//	N_rows = Big2Little32(N_rows);
//	N_cols = Big2Little32(N_cols);
//	slc.re.create(N_rows, N_cols, CV_64F);
//	slc.im.create(N_rows, N_cols, CV_64F);
//	float* data_point = (float*)malloc(N_rows * N_cols * 8);
//	fread(data_point, sizeof(float), 2 * N_rows * N_cols, fp);
//	fclose(fp);
//	size_t offset = 0;
//	for (int i = 0; i < N_rows; i++)
//	{
//		for (int j = 0; j < N_cols; j++)
//		{
//			slc.re.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//			offset++;
//			slc.im.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//			offset++;
//		}
//	}
//	//读取辅图像
//	fp = fopen(data_file2.c_str(), "rb");
//	N_rows = 0, N_cols = 0;
//	fread(&N_cols, 4, 1, fp);
//	fread(&N_rows, 4, 1, fp);
//	N_rows = Big2Little32(N_rows);
//	N_cols = Big2Little32(N_cols);
//	slc2.re.create(N_rows, N_cols, CV_64F);
//	slc2.im.create(N_rows, N_cols, CV_64F);
//	data_point = (float*)malloc(N_rows * N_cols * 8);
//	fread(data_point, sizeof(float), 2 * N_rows * N_cols, fp);
//	fclose(fp);
//	offset = 0;
//	for (int i = 0; i < N_rows; i++)
//	{
//		for (int j = 0; j < N_cols; j++)
//		{
//			slc2.re.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//			offset++;
//			slc2.im.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//			offset++;
//		}
//	}
//	free(data_point);
//	/*Mat phase, flat_phase;
//	flat_phase = Mat::zeros(N_rows, N_cols, CV_64F);
//	fp = fopen(flat_phase_file.c_str(), "rb");
//	int h;
//	fread(&h, sizeof(int), 1, fp);
//	h = Big2Little32(h);
//	double* flat_phase_pointer = NULL;
//	flat_phase_pointer = (double*)malloc(N_cols * sizeof(double));
//	fread(flat_phase_pointer, sizeof(double), N_cols, fp);
//	offset = 0;
//	for (int i = 0; i < N_cols; i++)
//	{
//		for (int j = 0; j < N_rows; j++)
//		{
//			double tt = Reversedouble(*(flat_phase_pointer + offset));
//			flat_phase.at<double>(j, i) = Reversedouble(*(flat_phase_pointer + offset));
//		}
//		offset++;
//	}
//	free(flat_phase_pointer);
//	fclose(fp);*/
//
//	////读取Kz
//	//Mat Kz;
//	//Kz = Mat::zeros(N_rows, N_cols, CV_32F);
//	//fp = fopen(kz_file.c_str(), "rb");
//	//fread(&N_cols, 4, 1, fp);
//	//fread(&N_rows, 4, 1, fp);
//	//N_rows = Big2Little32(N_rows);
//	//N_cols = Big2Little32(N_cols);
//	//float* Kz_pointer = NULL;
//	//Kz_pointer = (float*)malloc(N_cols * N_rows * sizeof(float));
//	//fread(Kz_pointer, sizeof(float), N_cols * N_rows, fp);
//	//offset = 0;
//	//for (int i = 0; i < N_rows; i++)
//	//{
//	//	for (int j = 0; j < N_cols; j++)
//	//	{
//	//		double tt = ReverseFloat(*(Kz_pointer + offset));
//	//		Kz.at<float>(i, j) = ReverseFloat(*(Kz_pointer + offset));
//	//	}
//	//	offset++;
//	//}
//	//free(Kz_pointer);
//	//fclose(fp);
//
//
//	//ComplexMat temp;
//	//util.phase2cos(flat_phase, temp.re, temp.im);
//	//slc2.Mul(temp, slc2, false);
//	//util.multilook(slc, slc2, 1, 1, phase);
//	//conversion.creat_new_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\phase.h5");
//	//conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\phase.h5", "phase", phase);
//
//
//	//conversion.creat_new_h5(master.c_str());
//	conversion.creat_new_h5(slave.c_str());
//	//conversion.creat_new_h5(kz_h5file.c_str());
//	//conversion.write_array_to_h5(kz_h5file.c_str(), "kz", Kz);
//	//conversion.write_slc_to_h5(master.c_str(), slc);
//	conversion.write_slc_to_h5(slave.c_str(), slc2);
//	util.saveSLC("D:\\data\\INDREX_2_data\\indrex_2\\mawas_c\\rgi\\rgi_04indrex1108x1_t11\\slc.jpg", 60, slc2);
//	return 0;
//}


////读取TropiSAR模糊高数据

//int main(int argc, char* argv[])
//{
//	int nr = 15190; int nc = 2600;
//	FormatConversion conversion;
//	Mat Ha = Mat::zeros(nr, nc, CV_64F);
//	FILE* fp = NULL;
//	string file = "D:\\data\\TropiSAR\\Paracou\\L_Band\\tomo_data\\tropi0407\\Interfero_geom_0402\\tropi0402cons_0407proj_traj0407L_Ha.1";
//	string h5file = "D:\\data\\TropiSAR\\Paracou\\L_Band\\tomo_data\\tropi0407\\Interfero_geom_0402\\tropi0402cons_0407proj_traj0407L_Ha.h5";
//	fp = fopen(file.c_str(), "rb");
//	float* data = (float*)malloc(sizeof(float) * nr * nc);
//	fread(data, sizeof(float), nr * nc, fp);
//	fclose(fp);
//	size_t offset = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			Ha.at<double>(i, j) = *(data + offset);
//			offset++;
//		}
//	}
//	free(data);
//	conversion.creat_new_h5(h5file.c_str());
//	conversion.write_array_to_h5(h5file.c_str(), "Ha", Ha);
//	return 0;
//}

//读取AfriSAR数据

//int main(int argc, char* argv[])
//{
//	ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	string file, h5file;
//	int nr = 12288, nc = 3772;
//	/*string phase_dem_file = "D:\\data\\AfriSAR\\DLR\\FL06\\PS11\\TL02\\INF\\INF-SR\\pha_dem_16afrisr0602_16afrisr0611_L_tL02.rat";
//	Mat phase(nr, nc, CV_32F);
//	FILE* fp1 = NULL;
//	fp1 = fopen(phase_dem_file.c_str(), "rb+");
//	float* data00 = (float*)malloc(nr * nc * sizeof(float));
//	void* data000 = malloc(1000);
//	fread(data000, sizeof(char), 1000, fp1);
//	free(data000);
//	fread((void*)data00, sizeof(float)* nr* nc, 1, fp1);
//	fclose(fp1);
//	size_t offset = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			phase.at<float>(i, j) = *(data00 + offset);
//			offset++;
//		}
//	}
//
//	free(data00);
//	ComplexMat flat;
//	util.phase2cos(phase, flat.re, flat.im);*/
//
//	//读取模糊高
//	/*string kz_file = "D:\\data\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\RGI\\RGI-SR\\incidence_16afrisr0502_P_tP01.rat";
//	string kz_h5file = "D:\\data\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\RGI\\RGI-SR\\incidence_16afrisr0502_P_tP01.h5";
//	Mat kz(nr, nc, CV_32F);
//	fp1 = fopen(kz_file.c_str(), "rb+");
//    data00 = (float*)malloc(nr * nc * sizeof(float));
//	data000 = malloc(1000);
//	fread(data000, sizeof(char), 1000, fp1);
//	free(data000);
//	fread((void*)data00, sizeof(float) * nr * nc, 1, fp1);
//	fclose(fp1);
//	offset = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			kz.at<float>(i, j) = *(data00 + offset);
//			offset++;
//		}
//	}
//
//	free(data00);
//	conversion.creat_new_h5(kz_h5file.c_str());
//	conversion.write_array_to_h5(kz_h5file.c_str(), "kz", kz);
//	return 0;*/
//
//
//	file = "E:\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\RGI\\RGI-SR\\slc_16afrisr0502_Pvh_tP01.rat";
//	h5file = "E:\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\RGI\\RGI-SR\\slc_16afrisr0502_Pvh_tP01.h5";
//	
//	slc.re.create(nr, nc, CV_32F);
//	slc.im.create(nr, nc, CV_32F);
//	FILE* fp = NULL;
//	fp = fopen(file.c_str(), "rb+");
//	int magic; float dummy; short dd;
//	float* data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	void* data0 = malloc(1000);
//	fread(data0, sizeof(char), 1000, fp);
//	free(data0);
//	fread((void*)data, sizeof(float)* nr* nc * 2, 1, fp);
//	fclose(fp);
//	size_t offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc.re.at<float>(i, j) = *(data + offset2);
//			offset2++;
//			slc.im.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//
//	free(data);
//	//slc.Mul(flat, slc, false);
//	conversion.creat_new_h5(h5file.c_str());
//	conversion.write_slc_to_h5(h5file.c_str(), slc);
//	//util.saveSLC("D:\\data\\AfriSAR\\DLR\\FL06\\PS11\\TL02\\INF\\INF-SR\\slc_hh.jpg", 60, slc);
//	return 0;
//}

//读取AfriSAR地理编码数据

//int main(int argc, char* argv[])
//{
//	ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	//string file, h5file;
//	int nr = 5800, nc = 5605;
//	string h0_file = "E:\\AfriSAR\\DLR\\FL06\\PS02\\TP01\\GTC\\GTC-LUT\\sr2geo3d_h0_16afrisr0602_P_tP01.rat";
//	Mat h0(nr, nc, CV_32F);
//	FILE* fp1 = NULL;
//	fp1 = fopen(h0_file.c_str(), "rb+");
//	float* data00 = (float*)malloc(nr * nc * sizeof(float));
//	void* data000 = malloc(1000);
//	fread(data000, sizeof(char), 1000, fp1);
//	free(data000);
//	fread((void*)data00, sizeof(float) * nr * nc, 1, fp1);
//	fclose(fp1);
//	size_t offset = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			h0.at<float>(i, j) = *(data00 + offset);
//			offset++;
//		}
//	}
//	free(data00);
//	h0.convertTo(h0, CV_64F);
//	//conversion.creat_new_h5("E:\\AfriSAR\\DLR\\FL06\\PS02\\TP01\\GTC\\GTC-LUT\\geocode_info.h5");
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL06\\PS02\\TP01\\GTC\\GTC-LUT\\geocode_info.h5", "h_ref", h0);
//	//util.cvmat2bin("D:\\data\\AfriSAR\\DLR\\FL04\\PS02\\TP01\\GTC\\GTC-LUT\\sr2geo3d_h0_16afrisr0602_P_tP01.bin", h0);
//	//util.savephase("D:\\data\\AfriSAR\\DLR\\FL04\\PS02\\TP01\\GTC\\GTC-LUT\\sr2geo3d_h0_16afrisr0602_P_tP01.jpg", "jet", h0);
//	return 0;
//
//	Mat DTM, DSM, RH100, RH95, lon, lat, az, rg, h_ref, az_o1, az_o2, rg_o1, rg_o2, mapped_slc_rg, mapped_slc_az, mapped_h0;
//
//	//vector<string> filelist;
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_038002.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_038456.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_038915.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_039434.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_039855.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_040483.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_040932.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_041404.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_041991.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_042643.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_044165.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_044874.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_045612.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_046644.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_047404.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_048492.txt");
//	//filelist.push_back("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308_R1808_049095.txt");
//
//	//util.read_LVIS(filelist, DTM, DSM, RH100, RH95, lat, lon);
//	//conversion.creat_new_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5");
//	//conversion.write_array_to_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5", "DTM", DTM);
//	//conversion.write_array_to_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5", "DSM", DSM);
//	//conversion.write_array_to_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5", "RH100", RH100);
//	//conversion.write_array_to_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5", "RH95", RH95);
//	//conversion.write_array_to_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5", "lon", lon);
//	//conversion.write_array_to_h5("D:\\data\\AfriSAR_LVIS\\0308\\LVIS2_Gabon2016_0308.h5", "lat", lat);
//	//return 0;
//	//string file = "E:\\AfriSAR_LVIS\\0302\\LVIS2_Gabon2016_0302.h5";
//	//string file = "D:\\data\\AfriSAR\\DLR\\FL07\\PS02\\TP01\\GTC\\GTC-LUT\\new.h5";
//	//string file = "D:\\data\\AfriSAR\\DLR\\FL07\\PS02\\TP01\\GTC\\GTC-LUT\\new_CHM.h5";
//	string file = "E:\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\GTC\\GTC-LUT\\new_interpolated.h5";
//	string geocode_info = "E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\geocode_info.h5";
//	conversion.read_array_from_h5(file.c_str(), "DTM", DTM);
//	conversion.read_array_from_h5(file.c_str(), "RH100", RH100);
//	conversion.read_array_from_h5(file.c_str(), "lon", lon);
//	conversion.read_array_from_h5(file.c_str(), "lat", lat);
//	conversion.read_array_from_h5(geocode_info.c_str(), "az", az);
//	conversion.read_array_from_h5(geocode_info.c_str(), "rg", rg);
//	conversion.read_array_from_h5(geocode_info.c_str(), "h_ref", h_ref);
//	conversion.read_array_from_h5(geocode_info.c_str(), "az_o1", az_o1);
//	conversion.read_array_from_h5(geocode_info.c_str(), "az_o2", az_o2);
//	conversion.read_array_from_h5(geocode_info.c_str(), "rg_o1", rg_o1);
//	conversion.read_array_from_h5(geocode_info.c_str(), "rg_o2", rg_o2);
//
//	//conversion.read_array_from_h5(file.c_str(), "CHM", DTM);
//	//////DTM.copyTo(lon); DTM.copyTo(lat);
//	//lon.create(6051, 7551, CV_64F); lon = -9999.0; lon.copyTo(lat);
//	double easting_min, easting_max, northing_min, northing_max;
//
//	easting_min = 532693.5962354151;
//	easting_max = 541611.5962354151;
//	northing_min = 57847.2796692298;
//	northing_max = 64695.2796692298;
//
//	//确定场景的经纬度范围
//	Mat lon2, lat2;
//	int projection_zone = 32;
//	lon2.create(4, 1, CV_64F); lon2 = 0.0; lon2.copyTo(lat2);
//	double utm_x, utm_y;
//	const char* path[] = { "D:\\softwarepackages\\release-1928-x64-gdal-3-3-1-mapserver-7-6-4\\bin\\proj7\\share" ,nullptr };
//	OSRSetPROJSearchPaths(path);
//	OGRSpatialReference monUtm;
//	monUtm.SetWellKnownGeogCS("WGS84");
//	monUtm.SetUTM(abs(projection_zone), projection_zone > 0);
//	OGRSpatialReference monGeo;
//	monGeo.SetWellKnownGeogCS("WGS84");
//	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
//	double x, y;
//	x = easting_min; y = northing_min;
//	int reprojected = coordTrans->Transform(1, &x, &y);
//	lon2.at<double>(0, 0) = y; lat2.at<double>(0, 0) = x;
//
//	x = easting_min; y = northing_max;
//	reprojected = coordTrans->Transform(1, &x, &y);
//	lon2.at<double>(1, 0) = y; lat2.at<double>(1, 0) = x;
//
//	x = easting_max; y = northing_min;
//	reprojected = coordTrans->Transform(1, &x, &y);
//	lon2.at<double>(2, 0) = y; lat2.at<double>(2, 0) = x;
//
//	x = easting_max; y = northing_max;
//	reprojected = coordTrans->Transform(1, &x, &y);
//	lon2.at<double>(3, 0) = y; lat2.at<double>(3, 0) = x;
//
//	double lon_min, lon_max, lat_min, lat_max;
//	cv::minMaxLoc(lon2, &lon_min, &lon_max);
//	cv::minMaxLoc(lat2, &lat_min, &lat_max);
//	delete coordTrans;
//
//	////double utm_x, utm_y;
//	////const char* path[] = { "D:\\softwarepackages\\release-1928-x64-gdal-3-3-1-mapserver-7-6-4\\bin\\proj7\\share" ,nullptr };
//	////OSRSetPROJSearchPaths(path);
//	////OGRSpatialReference monUtm;
//	////monUtm.SetWellKnownGeogCS("WGS84");
//	////monUtm.SetUTM(abs(32), false);
//	////OGRSpatialReference monGeo;
//	////monGeo.SetWellKnownGeogCS("WGS84");
//	//OGRCoordinateTransformation* coordTrans2 = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
//	////easting_min = 529975.0;
//	////northing_min = 10069825.0/*-10000000.0*/;
//	////easting_min = 664527.95716461726;
//	////northing_min = 9921401.0191002619;
//	///*easting_min = 664950.0;
//	//northing_min = 9921052.0;*/
//	//h_ref.copyTo(lon); h_ref.copyTo(lat);
//	//for (int i = 0; i < lon.rows; i++)
//	//{
//	//	for (int j = 0; j < lon.cols; j++)
//	//	{
//	//		if (h_ref.at<double>(i, j) < -9000) continue;
//	//		double x, y;
//	//		x = easting_min + 10.0*0 + double(j)*2.0; y = northing_min + 10.0*0 + double(i) * 2.0;/*y = northing_min + 10.0*0 + double(lon.rows - 1 - i)*20.0*/;
//	//		int reprojected = coordTrans2->Transform(1, &x, &y);
//	//		lat.at<double>(i, j) = x;
//	//		lon.at<double>(i, j) = y;
//	//	}
//	//}
//	//delete coordTrans2;
//	//conversion.write_array_to_h5(geocode_info.c_str(), "/lon_ref", lon);
//	//conversion.write_array_to_h5(geocode_info.c_str(), "/lat_ref", lat);
//	//return 0;
//	//DTM = DTM + RH100;
//	util.geo2sar_DLR(easting_min, northing_min, easting_max, northing_max, projection_zone, 2.0, az, rg, h_ref, rg_o1, rg_o1, az_o1, az_o2, DTM, lon, lat,
//		mapped_slc_rg, mapped_slc_az, mapped_h0);
//	conversion.creat_new_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5");
//
//	conversion.write_double_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "lon_min", lon_min);
//	conversion.write_double_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "lon_max", lon_max);
//	conversion.write_double_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "lat_min", lat_min);
//	conversion.write_double_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "lat_max", lat_max);
//
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_slc_rg", mapped_slc_rg);
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_slc_az", mapped_slc_az);
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_h0", mapped_h0);
//	Mat slc_az, slc_rg, mapped_DTM, mapped_h_ref, mapped_DSM;
//	slc_az.create(12288, 3772, CV_64F); slc_az = 0.0; slc_az.copyTo(slc_rg); slc_az.copyTo(mapped_DTM); slc_az.copyTo(mapped_h_ref); slc_az.copyTo(mapped_DSM);
//	for (int i = 0; i < DTM.rows; i++)
//	{
//		for (int j = 0; j < DTM.cols; j++)
//		{
//			if (DTM.at<double>(i, j) < -100.0) continue;
//			int row = round(mapped_slc_az.at<double>(i, j));
//			int col = round(mapped_slc_rg.at<double>(i, j));
//			if (row >= 0 && row < 12288 && col >= 0 && col < 3772)
//			{
//				slc_az.at<double>(row, col) = 1;
//				slc_rg.at<double>(row, col) = 1;
//				mapped_DTM.at<double>(row, col) = DTM.at<double>(i, j)/* - RH100.at<double>(i, j)*/;
//				mapped_DSM.at<double>(row, col) = RH100.at<double>(i, j);
//				mapped_h_ref.at<double>(row, col) = mapped_h0.at<double>(i, j);
//			}
//		}
//	}
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "slc_az", slc_az);
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "slc_rg", slc_rg);
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_DTM", mapped_DTM);
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_CHM", mapped_DSM);
//	//conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_h_ref", mapped_h_ref);
//	return 0;
//
//}

//int main(int argc, char* argv[])
//{
//	Mat DTM, DSM, RH100, RH95, lon, lat, az, rg, h_ref, az_o1, az_o2, rg_o1, rg_o2, mapped_slc_rg, mapped_slc_az, mapped_h0;
//	FormatConversion conversion;
//	string geocode_info = "E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\geocode_info.h5";
//	conversion.read_array_from_h5(geocode_info.c_str(), "az", az);
//	conversion.read_array_from_h5(geocode_info.c_str(), "rg", rg);
//	conversion.read_array_from_h5(geocode_info.c_str(), "h_ref", h_ref);
//
//	Mat mapped_h_ref;
//	mapped_h_ref.create(12288, 3772, CV_64F); mapped_h_ref = 0.0;
//	for (int i = 0; i < h_ref.rows; i++)
//	{
//		for (int j = 0; j < h_ref.cols; j++)
//		{
//			if (h_ref.at<double>(i, j) < -100.0) continue;
//			int row = round(az.at<double>(i, j));
//			int col = round(rg.at<double>(i, j));
//			if (row >= 0 && row < 12288 && col >= 0 && col < 3772)
//			{
//				mapped_h_ref.at<double>(row, col) = h_ref.at<double>(i, j);
//			}
//		}
//	}
//	//conversion.creat_new_h5("E:\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\GTC\\GTC-LUT\\mapped_tdx.h5");
//	conversion.write_array_to_h5("E:\\AfriSAR\\DLR\\FL02\\PS02\\TP01\\GTC\\GTC-LUT\\mapped.h5", "mapped_h_ref", mapped_h_ref);
//	return 0;
//}

//读取TropiSAR DTM数据

//int main(int argc, char* argv[])
//{
//	//读取TropiSAR DTM数据
//	Utils util; FormatConversion conversion;
//	int ncols = 2953; int nrows = 4369;
//	Mat DTM(nrows, ncols, CV_64F); DTM = 0.0;
//	const char* file = "D:\\data\\TropiSAR\\LIDAR_data_over_Paracaou\\DTM.asc";
//	const char* h5file = "D:\\data\\TropiSAR\\LIDAR_data_over_Paracaou\\DTM.h5";
//	FILE* fp = NULL;
//	fp = fopen(file, "rt");
//	char* data = (char*)malloc(ncols * 10);
//	char* ptr;
//	double val;
//	fgets(data, ncols * 10, fp);
//	fgets(data, ncols * 10, fp);
//	fgets(data, ncols * 10, fp);
//	fgets(data, ncols * 10, fp);
//	fgets(data, ncols * 10, fp);
//	fgets(data, ncols * 10, fp);
//	memset(data, 0, ncols * 10);
//	for (int i = 0; i < nrows; i++)
//	{
//		fgets(data, ncols * 10, fp);
//		val = strtod(data, &ptr);
//		DTM.at<double>(i, 0) = val < -10.0 ? -9999 : val;
//		for (int j = 1; j < ncols; j++)
//		{
//			val = strtod(ptr, &ptr);
//			DTM.at<double>(i, j) = val < -10.0 ? -9999 : val;
//		}
//	}
//	conversion.creat_new_h5(h5file);
//	conversion.write_array_to_h5(h5file, "DTM", DTM);
//	util.savephase("D:\\data\\TropiSAR\\LIDAR_data_over_Paracaou\\DTM2.jpg", "jet", DTM);
//	return 0;
//}


////测试TropiSAR DTM投影

//int main(int argc, char* argv[])
//{
//	Utils util; FormatConversion conversion;
//	
//	Mat DEM, DEM_mapped, DEM_prior;
//	double lon_interval = 1.0 / 12000.0;
//	double lat_interval = 1.0 / 12000.0;
//	double lon_upleft = -52.700166666666670; double lat_upleft = 4.100083333333333;
//	DEM_prior.create(6570, 4000, CV_64F); DEM_prior = 80.0;
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\SM_separation\\TropiSAR\\Nouragues1\\DEM_mapped.h5", "DEM_mapped", DEM_prior);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\SM_separation\\TropiSAR\\Nouragues1\\TanDEM-X-30m.h5", "DEM", DEM);
//	util.geo_transformation("D:\\data\\TropiSAR\\Nouragues1\\P_Band\\tomo_data\\tropi0201\\tropi0201_Pcons_slc.grille", 
//		DEM, lon_upleft, lat_upleft, lon_interval, lat_interval, DEM_prior, DEM_mapped, 4000, 6570);
//	conversion.creat_new_h5("D:\\working_dir\\papers\\SM_separation\\TropiSAR\\Nouragues1\\DEM_mapped.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\SM_separation\\TropiSAR\\Nouragues1\\DEM_mapped.h5", "DEM_mapped", DEM_mapped);
//	return 0;
//}

/*读取定标的TropiSAR数据*/
//int main(int argc, char* argv[])
//{
//	ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	string file, h5file;
//	int nr = 8896, nc = 4000;
//
//	file = "D:\\data\\TropiSAR\\slc_cal\\Vh\\tropi0407_Pproj_Vh_slc.dat";
//	h5file = "D:\\data\\TropiSAR\\slc_cal\\Vh\\tropi0407_Pproj_Vh_slc.h5";
//	
//	slc.re.create(nr, nc, CV_32F);
//	slc.im.create(nr, nc, CV_32F);
//	FILE* fp = NULL;
//	fp = fopen(file.c_str(), "rb+");
//	int magic; float dummy; short dd;
//	fread(&dummy, sizeof(int), 1, fp);
//	fread(&dummy, sizeof(int), 1, fp);
//	float* data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	fread((void*)data, sizeof(float)* nr* nc * 2, 1, fp);
//	fclose(fp);
//	size_t offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc.re.at<float>(i, j) = *(data + offset2);
//			offset2++;
//			slc.im.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//
//	free(data);
//	conversion.creat_new_h5(h5file.c_str());
//	conversion.write_slc_to_h5(h5file.c_str(), slc);
//	return 0;
//}

/*AfriSAR ONERA数据 相位校正*/
//int main(int argc, char* argv[])
//{
//	ComplexMat slc, slc2; Utils util; FormatConversion conversion;
//	string file, file2, file_h5, file2_h5;
//	Mat Kz, Kz2;
//	int nr = 5306, nc = 1300;
//	file = "E:\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\RabiTomostack\\20150706-9_sar_UHF50MHzHAM_Vv_rad.dat";
//	file2 = "E:\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\RabiTomostack\\20150706-10_sar_UHF50MHzHAM_Vv_rad.dat";
//	file_h5 = "E:\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\RabiTomostack\\9_Vv.h5";
//	file2_h5 = "E:\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\RabiTomostack\\10_Vv.h5";
//	slc.re.create(nr, nc, CV_32F);
//	slc.im.create(nr, nc, CV_32F);
//	slc2.re.create(nr, nc, CV_32F);
//	slc2.im.create(nr, nc, CV_32F);
//	FILE* fp = NULL;
//	fp = fopen(file.c_str(), "rb+");
//	int magic; float dummy; short dd;
//	fseek(fp, (nc) * 4 * 2 + 4, SEEK_SET);
//	float* data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	fread((void*)data, sizeof(float)* nr* nc * 2, 1, fp);
//	fclose(fp);
//	size_t offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc.re.at<float>(i, j) = *(data + offset2);
//			offset2++;
//			slc.im.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//	free(data);
//	fp = fopen(file2.c_str(), "rb+");
//	fseek(fp, (nc) * 4 * 2 + 4, SEEK_SET);
//	data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	fread((void*)data, sizeof(float) * nr * nc * 2, 1, fp);
//	fclose(fp);
//	offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc2.re.at<float>(i, j) = *(data + offset2);
//			offset2++;
//			slc2.im.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//	free(data);
//	/*Kz.create(nr, nc, CV_32F);
//	FILE* fp = NULL;
//	fp = fopen(file.c_str(), "rb+");
//	fseek(fp, (nc) * 4 + 4, SEEK_SET);
//	float* data = (float*)malloc(nr * nc * sizeof(float));
//	fread((void*)data, sizeof(float) * nr * nc, 1, fp);
//	fclose(fp);
//	size_t offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			Kz.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//	free(data);
//	Kz2.create(nr, nc, CV_32F);
//	fp = fopen(file2.c_str(), "rb+");
//	fseek(fp, (nc) * 4 + 4, SEEK_SET);
//	data = (float*)malloc(nr * nc * sizeof(float));
//	fread((void*)data, sizeof(float) * nr * nc, 1, fp);
//	fclose(fp);
//	offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			Kz2.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//	free(data);*/
//
//	////配准
//	//Registration regis;
//	//int offset_row, offset_col;
//	//slc2.convertTo(slc2, CV_64F);
//	//slc.convertTo(slc, CV_64F);
//	//regis.real_coherent(slc2, slc, &offset_row, &offset_col);
//	//ComplexMat slc_coregis;
//	//slc_coregis.re = Mat::zeros(slc2.GetRows(), slc2.GetCols(), CV_32F);
//	//slc_coregis.im = Mat::zeros(slc2.GetRows(), slc2.GetCols(), CV_32F);
//	//slc.re(cv::Range(0, slc2.GetRows()), cv::Range(0, slc2.GetCols() - 12)).copyTo(slc_coregis.re(cv::Range(0, slc2.GetRows()), cv::Range(12, slc2.GetCols())));
//	//slc.im(cv::Range(0, slc2.GetRows()), cv::Range(0, slc2.GetCols() - 12)).copyTo(slc_coregis.im(cv::Range(0, slc2.GetRows()), cv::Range(12, slc2.GetCols())));
//	//slc = slc_coregis;
//	conversion.creat_new_h5(file_h5.c_str());
//	conversion.creat_new_h5(file2_h5.c_str());
//	//conversion.write_array_to_h5(file_h5.c_str(), "lon", Kz);
//	//conversion.write_array_to_h5(file2_h5.c_str(), "lat", Kz2);
//	//conversion.write_array_to_h5(file2_h5.c_str(), "lon", Kz2);
//	conversion.write_slc_to_h5(file_h5.c_str(), slc);
//	conversion.write_slc_to_h5(file2_h5.c_str(), slc2);
//	//Mat phase, amplitude, amplitude2;
//	//util.Multilook(slc, slc2, 4, 4, phase);
//	//util.savephase("D:\\data\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\L_band\\Mondah_Tomostack\\SLC\\phase7_8.jpg", "jet", phase);
//	////amplitude = slc.GetMod();
//	////util.multilook_SAR(amplitude, amplitude2, 1, 4);
//	////util.saveAmplitude("D:\\working_dir\\projects\\software\\InSAR\\bin\\rabi_slc2.jpg", amplitude2);
//	//util.saveSLC("D:\\data\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\L_band\\Rabi_Tomostack\\SLC\\slc9.jpg", 50, slc);
//	//util.saveSLC("D:\\data\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\L_band\\Mondah_Tomostack\\SLC\\slc8.jpg", 50, slc2);
//	return 0;
//}



/*ONERA AfriSAR WGS lonlat == 》UTM*/
//int main(int agrc, char* argv[])
//{
//	FormatConversion conversion;
//	double easting_min, easting_max, northing_min, northing_max;
//	Mat lon, lat, utm_x, utm_y;
//	string file = "D:\\data\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\L_band\\Rabi_Tomostack\\SLC\\UTM.h5";
//	conversion.read_array_from_h5("D:\\data\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\L_band\\Rabi_Tomostack\\SLC\\lon.h5", "lon", lon);
//	conversion.read_array_from_h5("D:\\data\\AfriSAR\\ONERA\\ESA_SETHI_Gabon_DATA_Final\\L_band\\Rabi_Tomostack\\SLC\\lat.h5", "lat", lat);
//	utm_x = Mat::zeros(lon.rows, lon.cols, CV_64F); utm_x = utm_x - 1.0;
//	utm_y = Mat::zeros(lon.rows, lon.cols, CV_64F); utm_y = utm_y - 1.0;
//	const char* path[] = { "D:\\softwarepackages\\release-1928-x64-gdal-3-3-1-mapserver-7-6-4\\bin\\proj7\\share" ,nullptr };
//	OSRSetPROJSearchPaths(path);
//	OGRSpatialReference monUtm;
//	monUtm.SetWellKnownGeogCS("WGS84");
//	monUtm.SetUTM(32, false);
//	OGRSpatialReference monGeo;
//	monGeo.SetWellKnownGeogCS("WGS84");
//	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monGeo , &monUtm);
//	for (int i = 0; i < lon.rows; i++)
//	{
//		for (int j = 0; j < lon.cols; j++)
//		{
//			double x, y;
//			x = lat.at<float>(i, j);
//			y = lon.at<float>(i, j);
//			int reprojected = coordTrans->Transform(1, &x, &y);
//			utm_x.at<double>(i, j) = x;
//			utm_y.at<double>(i, j) = y;
//		}
//	}
//	conversion.creat_new_h5(file.c_str());
//	conversion.write_array_to_h5(file.c_str(), "/utm_x", utm_x);
//	conversion.write_array_to_h5(file.c_str(), "/utm_y", utm_y);
//	return 0;
//}


/*AfriSAR UAVSAR 测试*/
//int main(int argc, char* argv[])
//{
//	string file = "D:\\data\\AfriSAR_UAVSAR\\lopenp_TM140_16008_004_160225_L090HH_03_BC_s1_1x1.slc";
//	string file2 = "D:\\data\\AfriSAR_UAVSAR\\lopenp_TM140_16008_003_160225_L090HH_03_BC_s1_1x1.slc";
//	Utils util; FormatConversion conversion;
//	int nr = 68940; int nc = 9843;
//	ComplexMat slc, slc2;
//	slc.re.create(nr, nc, CV_32F);
//	slc.im.create(nr, nc, CV_32F);
//	slc2.re.create(nr, nc, CV_32F);
//	slc2.im.create(nr, nc, CV_32F);
//	FILE* fp = NULL;
//	fp = fopen(file.c_str(), "rb+");
//	float* data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	fread((void*)data, sizeof(float)* nr* nc * 2, 1, fp);
//	fclose(fp);
//	size_t offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc.re.at<float>(i, j) = *(data + offset2);
//			offset2++;
//			slc.im.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//	free(data);
//	fp = fopen(file2.c_str(), "rb+");
//	data = (float*)malloc(nr * nc * sizeof(float) * 2);
//	fread((void*)data, sizeof(float) * nr * nc * 2, 1, fp);
//	fclose(fp);
//	offset2 = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc2.re.at<float>(i, j) = *(data + offset2);
//			offset2++;
//			slc2.im.at<float>(i, j) = *(data + offset2);
//			offset2++;
//		}
//	}
//	free(data);
//	//slc.convertTo(slc, CV_64F);
//	//slc2.convertTo(slc2, CV_64F);
//	//Mat mod, mod2;
//	//mod = slc.GetMod();
//	//util.multilook_SAR(mod, mod2, 1, 4);
//	//util.saveAmplitude("D:\\data\\AfriSAR_UAVSAR\\slc.jpg", mod2);
//	Mat phase;
//	util.Multilook(slc, slc2, 1, 4, phase);
//	util.savephase("D:\\data\\AfriSAR_UAVSAR\\phase.jpg", "jet", phase);
//	return 0;
//}


/*读取TropiSAR 3D地理编码数据文件.grille*/
//int main(int argc, char* argv[])
//{
//	Utils util; FormatConversion conversion;
//	Mat row_matrix, col_matrix, utm_x, utm_y, height;
//	vector<Mat> lon_matrix;
//	vector<Mat> lat_matrix;
//	vector<double> height_vector;
//	util.read_grille("D:\\data\\TropiSAR\\Paracou\\P_Band\\tomo_data\\tropi0402\\tropi0402_Pcons_slc.grille", row_matrix, col_matrix, lon_matrix, lat_matrix, height_vector);
//	string h5_file = "D:\\data\\TropiSAR\\Paracou\\P_Band\\tomo_data\\tropi0402\\grille.h5";
//	conversion.creat_new_h5(h5_file.c_str());
//	conversion.write_array_to_h5(h5_file.c_str(), "row_matrix", row_matrix);
//	conversion.write_array_to_h5(h5_file.c_str(), "col_matrix", col_matrix);
//	height.create(1, height_vector.size(), CV_64F);
//	for (int i = 0; i < height_vector.size(); i++)
//	{
//		char datasetname[256];
//		sprintf(datasetname, "lon_%d", i + 1);
//		conversion.write_array_to_h5(h5_file.c_str(), datasetname, lon_matrix[i]);
//
//		char datasetname2[256];
//		sprintf(datasetname2, "lat_%d", i + 1);
//		conversion.write_array_to_h5(h5_file.c_str(), datasetname2, lat_matrix[i]);
//
//		height.at<double>(0, i) = height_vector[i];
//	}
//	conversion.write_array_to_h5(h5_file.c_str(), "height", height);
//	return 0;
//}



/*读取BioSAR2008参数文件*/
//int main(int argc, char* argv[])
//{
//	ComplexMat slc, slc2; FormatConversion conversion; Utils util;
//	string file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t11\\range_slc08biosar0201x1_t11_int.dat";
//	string file_h5 = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t11\\range_slc08biosar0201x1_t11_int.h5";
//	FILE* fp = NULL;
//	int N_rows, N_cols, tmp;
//	//读取Kz
//	Mat Kz;
//	
//	fp = fopen(file.c_str(), "rb");
//	fread(&N_cols, 4, 1, fp);
//	fread(&N_rows, 4, 1, fp);
//	//N_rows = Big2Little32(N_rows);
//	//N_cols = Big2Little32(N_cols);
//	//fread(&tmp, 4, 1, fp);
//	//fread(&tmp, 4, 1, fp);
//	//fread(&tmp, 4, 1, fp);
//	//fread(&tmp, 4, 1, fp);
//	Kz = Mat::zeros(N_rows, N_cols, CV_32S);
//	//N_rows = Big2Little32(N_rows);
//	//N_cols = Big2Little32(N_cols);
//	uint16_t* Kz_pointer = NULL;
//	Kz_pointer = (uint16_t*)malloc(N_cols * N_rows * 2);
//	fread(Kz_pointer, 1, N_cols * N_rows * 2, fp);
//	size_t offset = 0;
//	for (int i = 0; i < N_rows; i++)
//	{
//		for (int j = 0; j < N_cols; j++)
//		{
//			uint16_t tt = /*ReverseFloat*/((*(Kz_pointer + offset)));
//			Kz.at<int>(N_rows - i - 1, j) = tt;
//			offset++;
//		}
//		
//	}
//	free(Kz_pointer);
//	fclose(fp);
//	conversion.creat_new_h5(file_h5.c_str());
//	conversion.write_array_to_h5(file_h5.c_str(), "rangepos", Kz);
//	return 0;
//}

/*TanDEM-X 30m DEM 投影至BioSAR2008场景*/
//int main(int argc, char* argv[])
//{
//	Utils util; FormatConversion conversion;
//	Mat tdx30_dem, UTM_DEM, mapped_DEM, tdx30_lon, tdx30_lat, UTM_lon, UTM_lat, mapped_lat, mapped_lon;
//	int utm_rows = 5100; int utm_cols = 5100;
////	double easting_max, easting_min, northing_max, northing_min, lon_upleft, lat_upleft, delta_lon, delta_lat;
////	easting_min = 441782;
////	northing_max = 7124832;
////	string demfile = "D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\TanDEM-X-30m.h5";
////	conversion.read_array_from_h5(demfile.c_str(), "DEM", tdx30_dem);
////	conversion.read_double_from_h5(demfile.c_str(), "lon_upleft", &lon_upleft);
////	conversion.read_double_from_h5(demfile.c_str(), "lat_upleft", &lat_upleft);
////	conversion.read_double_from_h5(demfile.c_str(), "delta_lon", &delta_lon);
////	conversion.read_double_from_h5(demfile.c_str(), "delta_lat", &delta_lat);
////	int DEM_rows = tdx30_dem.rows;
////	int DEM_cols = tdx30_dem.cols;
////	UTM_DEM = Mat::zeros(utm_rows, utm_cols, CV_64F);
////	UTM_lon = Mat::zeros(utm_rows, utm_cols, CV_64F);
////	UTM_lat = Mat::zeros(utm_rows, utm_cols, CV_64F);
////	tdx30_lon = Mat::zeros(DEM_rows, DEM_cols, CV_64F);
////	tdx30_lat = Mat::zeros(DEM_rows, DEM_cols, CV_64F);
////	const char* path[] = { "D:\\softwarepackages\\release-1928-x64-gdal-3-3-1-mapserver-7-6-4\\bin\\proj7\\share" ,nullptr };
////	OSRSetPROJSearchPaths(path);
////	OGRSpatialReference monUtm;
////	monUtm.SetWellKnownGeogCS("WGS84");
////	monUtm.SetUTM(34, 1);
////	OGRSpatialReference monGeo;
////	monGeo.SetWellKnownGeogCS("WGS84");
////	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
////	for (int i = 0; i < DEM_rows; i++)
////	{
////		for (int j = 0; j < DEM_cols; j++)
////		{
////			tdx30_lon.at<double>(i, j) = lon_upleft + j * delta_lon;
////			tdx30_lat.at<double>(i, j) = lat_upleft - i * delta_lat;
////		}
////	}
////	//双线性插值
////#pragma omp parallel for schedule(guided)
////	for (int i = 0; i < utm_rows; i++)
////	{
////		for (int j = 0; j < utm_cols; j++)
////		{
////			double lat_x, lon_y, upper, lower;
////			lat_x = easting_min + (j - 1) * 1.0;
////			lon_y = northing_max - (i - 1) * 1.0;
////			int reprojected = coordTrans->Transform(1, &lat_x, &lon_y);
////			//通过插值得到的lat_x和lon_y再次插值得到DTM
////			if (lat_x <= lat_upleft &&
////				lat_x >= (lat_upleft - (DEM_rows - 1) * delta_lat) &&
////				lon_y <= (lon_upleft + (DEM_cols - 1) * delta_lon) &&
////				lon_y >= lon_upleft
////				)
////			{
////				int row = floor((lat_upleft - lat_x) / delta_lat);
////				double delta_row = ((lat_upleft - lat_x) / delta_lat - row);
////				int col = floor((lon_y - lon_upleft) / delta_lon);
////				double delta_col = ((lon_y - lon_upleft) / delta_lon - col);
////				if (row < 0 || row >= DEM_rows - 1 || col < 0 || col >= DEM_cols - 1) continue;
////				if (tdx30_dem.at<double>(row, col) > -1000 &&
////					tdx30_dem.at<double>(row + 1, col) > -1000 &&
////					tdx30_dem.at<double>(row + 1, col + 1) > -1000 &&
////					tdx30_dem.at<double>(row, col + 1) > -1000
////					)
////				{
////					upper = tdx30_dem.at<double>(row, col) + (tdx30_dem.at<double>(row, col + 1) - tdx30_dem.at<double>(row, col)) * delta_row;
////					lower = tdx30_dem.at<double>(row + 1, col) + (tdx30_dem.at<double>(row + 1, col + 1) - tdx30_dem.at<double>(row + 1, col)) * delta_row;
////					UTM_DEM.at<double>(i, j) = lower + (upper - lower) * delta_col;
////
////					upper = tdx30_lon.at<double>(row, col) + (tdx30_lon.at<double>(row, col + 1) - tdx30_lon.at<double>(row, col)) * delta_row;
////					lower = tdx30_lon.at<double>(row + 1, col) + (tdx30_lon.at<double>(row + 1, col + 1) - tdx30_lon.at<double>(row + 1, col)) * delta_row;
////					UTM_lon.at<double>(i, j) = lower + (upper - lower) * delta_col;
////
////					upper = tdx30_lat.at<double>(row, col) + (tdx30_lat.at<double>(row, col + 1) - tdx30_lat.at<double>(row, col)) * delta_row;
////					lower = tdx30_lat.at<double>(row + 1, col) + (tdx30_lat.at<double>(row + 1, col + 1) - tdx30_lat.at<double>(row + 1, col)) * delta_row;
////					UTM_lat.at<double>(i, j) = lower + (upper - lower) * delta_col;
////				}
////
////			}
////		}
////	}
////	//conversion.creat_new_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5");
////	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_DEM1", UTM_DEM);
////	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lon1", UTM_lon);
////	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lat1", UTM_lat);
//
//	string rangepos_file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t31\\range_slc08biosar0201x1_t31_int.h5";
//	string azimuth_file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t31\\azimuth_slc08biosar0201x1_t31_int.h5";
//	Mat rangepos, azimuthpos;
//	conversion.read_array_from_h5(rangepos_file.c_str(), "rangepos", rangepos);
//	conversion.read_array_from_h5(azimuth_file.c_str(), "azimuthpos", azimuthpos);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_DEM3", UTM_DEM);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lon3", UTM_lon);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lat3", UTM_lat);
//	int nr_SAR = 22198; int nc_SAR = 1510;
//	Mat count = Mat::zeros(nr_SAR, nc_SAR, CV_32S);
//	mapped_DEM = Mat::zeros(nr_SAR, nc_SAR, CV_64F);
//	mapped_lon = Mat::zeros(nr_SAR, nc_SAR, CV_64F);
//	mapped_lat = Mat::zeros(nr_SAR, nc_SAR, CV_64F);
//	for (int i = 0; i < utm_rows; i++)
//	{
//		for (int j = 0; j < utm_cols; j++)
//		{
//			int col = rangepos.at<int>(i, j) - 32768;
//			int row = azimuthpos.at<int>(i, j) - 32768;
//			if (row < 0 || row > nr_SAR - 1 || col < 0 || col > nc_SAR) continue;
//			count.at<int>(row, col) += 1;
//			mapped_DEM.at<double>(row, col) += UTM_DEM.at<double>(i, j);
//			mapped_lon.at<double>(row, col) += UTM_lon.at<double>(i, j);
//			mapped_lat.at<double>(row, col) += UTM_lat.at<double>(i, j);
//		}
//	}
//	rangepos_file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t21\\range_slc08biosar0201x1_t21_int.h5";
//	azimuth_file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t21\\azimuth_slc08biosar0201x1_t21_int.h5";
//	conversion.read_array_from_h5(rangepos_file.c_str(), "rangepos", rangepos);
//	conversion.read_array_from_h5(azimuth_file.c_str(), "azimuthpos", azimuthpos);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_DEM2", UTM_DEM);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lon2", UTM_lon);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lat2", UTM_lat);
//	for (int i = 0; i < utm_rows; i++)
//	{
//		for (int j = 0; j < utm_cols; j++)
//		{
//			int col = rangepos.at<int>(i, j) - 32768;
//			int row = azimuthpos.at<int>(i, j) - 32768;
//			if (row < 0 || row > nr_SAR - 1 || col < 0 || col > nc_SAR) continue;
//			count.at<int>(row, col) += 1;
//			mapped_DEM.at<double>(row, col) += UTM_DEM.at<double>(i, j);
//			mapped_lon.at<double>(row, col) += UTM_lon.at<double>(i, j);
//			mapped_lat.at<double>(row, col) += UTM_lat.at<double>(i, j);
//		}
//	}
//	rangepos_file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t11\\range_slc08biosar0201x1_t11_int.h5";
//	azimuth_file = "D:\\data\\BioSAR2008\\GTC\\08biosar0201x1_t11\\azimuth_slc08biosar0201x1_t11_int.h5";
//	conversion.read_array_from_h5(rangepos_file.c_str(), "rangepos", rangepos);
//	conversion.read_array_from_h5(azimuth_file.c_str(), "azimuthpos", azimuthpos);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_DEM1", UTM_DEM);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lon1", UTM_lon);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008\\UTM_DEM.h5", "UTM_lat1", UTM_lat);
//	for (int i = 0; i < utm_rows; i++)
//	{
//		for (int j = 0; j < utm_cols; j++)
//		{
//			int col = rangepos.at<int>(i, j) - 32768;
//			int row = azimuthpos.at<int>(i, j) - 32768;
//			if (row < 0 || row > nr_SAR - 1 || col < 0 || col > nc_SAR) continue;
//			count.at<int>(row, col) += 1;
//			mapped_DEM.at<double>(row, col) += UTM_DEM.at<double>(i, j);
//			mapped_lon.at<double>(row, col) += UTM_lon.at<double>(i, j);
//			mapped_lat.at<double>(row, col) += UTM_lat.at<double>(i, j);
//		}
//	}
//	for (int i = 0; i < nr_SAR; i++)
//	{
//		for (int j = 0; j < nc_SAR; j++)
//		{
//			if (count.at<int>(i, j) <= 0) continue;
//			mapped_DEM.at<double>(i, j) /= (double)count.at<int>(i, j);
//			mapped_lon.at<double>(i, j) /= (double)count.at<int>(i, j);
//			mapped_lat.at<double>(i, j) /= (double)count.at<int>(i, j);
//		}
//	}
//	conversion.creat_new_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR.h5", "DEM", mapped_DEM);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR.h5", "lon", mapped_lon);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR.h5", "lat", mapped_lat);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR.h5", "count_mask", count);
//	return 0;
//}

/*BioSAR2008去除参考相位*/
//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Utils util;
//	
//	string trackfile = "D:\\data\\BioSAR2008\\RGI\\08biosar0201x1_t01\\track_wgs84_slc08biosar0201x1_ch1_t01.dat";
//	string track_smoothed_file = "D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\track_smoothed.h5";
//	string slcfile = "D:\\data\\BioSAR2008\\RGI\\08biosar0201x1_t01\\i08biosar0201x1_ch1_t01_slc.dat";
//	string slcfileh5 = "D:\\data\\BioSAR2008\\RGI\\08biosar0201x1_t01\\i08biosar0201x1_ch1_t01_slc.h5";
//
//	FILE* fp = NULL;
//	int N_rows, N_cols;
//	size_t offset = 0;
//	//读取轨道数据
//	Mat track, track_smoothed;
//	ComplexMat slc;
//	/*fp = fopen(trackfile.c_str(), "rb");
//	fread(&N_cols, 4, 1, fp);
//	fread(&N_rows, 4, 1, fp);
//	N_rows = Big2Little32(N_rows);
//	N_cols = Big2Little32(N_cols);
//	track = Mat::zeros(N_rows, N_cols, CV_64F);
//	double* Kz_pointer = NULL;
//	Kz_pointer = (double*)malloc(N_cols * N_rows * sizeof(double));
//	fread(Kz_pointer, 1, N_cols * N_rows * sizeof(double), fp);
//	for (int i = 0; i < N_rows; i++)
//	{
//		for (int j = 0; j < N_cols; j++)
//		{
//			double tt = Reversedouble((*(Kz_pointer + offset)));
//			track.at<double>(i, j) = tt;
//			offset++;
//		}
//		
//	}
//	free(Kz_pointer);
//	fclose(fp);
//	conversion.creat_new_h5(slcfileh5.c_str());
//	conversion.write_array_to_h5(slcfileh5.c_str(), "/track", track); return 0;*/
//	//读取平滑后的track数据
//	conversion.read_array_from_h5(track_smoothed_file.c_str(), "track01", track_smoothed);
//
//	//读取slc数据
//	fp = fopen(slcfile.c_str(), "rb");
//	N_rows = 0, N_cols = 0;
//	fread(&N_cols, 4, 1, fp);
//	fread(&N_rows, 4, 1, fp);
//	N_rows = Big2Little32(N_rows);
//	N_cols = Big2Little32(N_cols);
//	slc.re.create(N_rows, N_cols, CV_64F);
//	slc.im.create(N_rows, N_cols, CV_64F);
//	float* data_point = (float*)malloc(N_rows * N_cols * 8);
//	fread(data_point, sizeof(float), 2 * N_rows * N_cols, fp);
//	fclose(fp);
//	offset = 0;
//	for (int i = 0; i < N_rows; i++)
//	{
//		for (int j = 0; j < N_cols; j++)
//		{
//			double tt = ReverseFloat(*(data_point + offset));
//			slc.re.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//			offset++;
//			slc.im.at<double>(i, j) = ReverseFloat(*(data_point + offset));
//			offset++;
//		}
//	}
//	free(data_point);
//
//	//读取dem坐标
//	Mat DEM, lon, lat;
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR2.h5", "DEM", DEM);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR2.h5", "lon", lon);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\baseline_error_correction\\BioSAR2008-L\\mapped_DEM_SAR2.h5", "lat", lat);
//
//	//计算并去除参考相位
//	Mat ref_phase = Mat::zeros(N_rows, N_cols, CV_64F);
//	double lambda = VEL_C / 1300e6;
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < N_rows; i++)
//	{
//		for (int j = 0; j < N_cols; j++)
//		{
//			double R = 0, phi, re1, re2, im1, im2, longitude, latitude, elev;
//			Position xyz;
//			longitude = lon.at<double>(i, j);
//			latitude = lat.at<double>(i, j);
//			elev = DEM.at<double>(i, j);
//			Utils::ell2xyz(longitude, latitude, elev, xyz);
//			R = R + ((xyz.x - track_smoothed.at<double>(i, 0)) * (xyz.x - track_smoothed.at<double>(i, 0)));
//			R = R + ((xyz.y - track_smoothed.at<double>(i, 1)) * (xyz.y - track_smoothed.at<double>(i, 1)));
//			R = R + ((xyz.z - track_smoothed.at<double>(i, 2)) * (xyz.z - track_smoothed.at<double>(i, 2)));
//			R = sqrt(R);
//			phi = 4 * PI * R / lambda;
//			re1 = slc.re.at<double>(i, j);
//			im1 = slc.im.at<double>(i, j);
//			re2 = cos(phi);
//			im2 = sin(phi);
//			slc.re.at<double>(i, j) = re1 * re2 - im1 * im2;
//			slc.im.at<double>(i, j) = re1 * im2 + im1 * re2;
//			ref_phase.at<double>(i, j) = atan2(cos(phi), sin(phi));
//		}
//	}
//	//conversion.creat_new_h5(slcfileh5.c_str());
//	conversion.write_slc_to_h5(slcfileh5.c_str(), slc);
//	conversion.write_array_to_h5(slcfileh5.c_str(), "/ref_phase", ref_phase);
//	return 0;
//}



/*质量图法解缠IEEE sensors journal论文实验*/
//int main(int argc, char* argv[])
//{
//	Mat phase, unwrapped_phase, PDV, mask;
//	Unwrap unwrap; Utils util;
//	util.bin2cvmat("D:\\working_dir\\papers\\PhaseUnwrap\\airborne\\phase_filter.bin", phase);
//	//util.phase_derivatives_variance(phase, PDV, 5);
//
//	util.phase_derivatives_variance(phase, PDV);
//	util.gen_mask_pdv(PDV, mask, 3, 0.1);
//
//	//unwrap.qualityGuided(phase, unwrapped_phase, PDV);
//	mask.convertTo(mask, CV_64F);
//	util.cvmat2bin("D:\\working_dir\\papers\\PhaseUnwrap\\airborne\\mask.bin", mask);
//	return 0;
//}


/*航天宏图热带雨林数据处理(高程反演)*/

//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Registration coregis; Deflat flat; Filter filter; Unwrap unwrap;
//    Utils util; ComplexMat slc1, slc2;
//	string master_file = "D:\\data\\tmp\\satexplorer\\HT_rainforest2\\import\\HT1-A.h5";
//	string master_deramped_file = "D:\\data\\tmp\\satexplorer\\HT_rainforest2\\deramped\\HT1-A_regis_deramp.h5";
//	string slave_file = "D:\\data\\tmp\\satexplorer\\HT_rainforest2\\import\\HT1-D.h5";
//	string slave_coregis_file = "D:\\data\\tmp\\satexplorer\\HT_rainforest2\\deramped\\HT1-D_regis.h5";
//	string slave_coregis_deramped_file = "D:\\data\\tmp\\satexplorer\\HT_rainforest2\\deramped\\HT1-D_regis_deramp.h5";
//	string demPath = "D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem";
//	conversion.read_slc_from_h5(master_file.c_str(), slc1);
//	conversion.read_slc_from_h5(slave_file.c_str(), slc2);
//
//
//	slc1 = slc1(cv::Range(0, 5000), cv::Range(0, 5000));
//	slc2 = slc2(cv::Range(0, 5000), cv::Range(0, 5000));
//	slc1.convertTo(slc1, CV_64F);
//	slc2.convertTo(slc2, CV_64F);
//	int move_r, move_c;
//	coregis.real_coherent(slc1, slc2, &move_r, &move_c);
//
//
//
//	/*slc1.convertTo(slc1, CV_32F);
//	slc2.convertTo(slc2, CV_32F);
//    Mat phase, coherence;
//	util.Multilook(slc1, slc2, 4, 2, phase);
//	util.phase_coherence(phase, 5, 5, coherence);
//	conversion.creat_new_h5("D:\\data\\HTHT_data\\coherence_A_D.h5");
//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\coherence_A_D.h5", "jet", coherence);
//	util.savephase("D:\\data\\HTHT_data\\coherence_A_D.jpg", "jet", coherence); return 0;*/
//    /*coregis.coregistration_subpixel(slc1, slc2, 512, 8);
//	fprintf(stdout, "coregistration finished!\n");
//	conversion.creat_new_h5(slave_coregis_file.c_str());
//	conversion.write_slc_to_h5(slave_coregis_file.c_str(), slc2);
//	conversion.write_int_to_h5(slave_coregis_file.c_str(), "range_len", slc2.GetCols());
//	conversion.write_int_to_h5(slave_coregis_file.c_str(), "azimuth_len", slc2.GetRows());
//	conversion.Copy_para_from_h5_2_h5(slave_file.c_str(), slave_coregis_file.c_str());
//	slc1.convertTo(slc1, CV_32F);
//	slc2.convertTo(slc2, CV_32F);
//    util.Multilook(slc1, slc2, 4, 2, phase);
//	util.savephase("D:\\data\\HTHT_data\\phase_A_D.jpg", "jet", phase);*/
//	
//
//	int ret, sceneWidth, sceneHeight, offset_col = 0, offset_row = 0;
//	double prf, wavelength, rangeSpacing, nearRangeTime, start, end, lon_upperleft, lat_upperleft, lonMin, lonMax, latMin, latMax;
//	string start_time, end_time;
//	Mat statevec, dem;
//	ret = conversion.read_int_from_h5(master_file.c_str(), "range_len", &sceneWidth);
//	ret = conversion.read_int_from_h5(master_file.c_str(), "azimuth_len", &sceneHeight);
//
//	ret = conversion.read_double_from_h5(master_file.c_str(), "prf", &prf);
//	ret = conversion.read_double_from_h5(master_file.c_str(), "carrier_frequency", &wavelength);
//	wavelength = VEL_C / wavelength;
//	ret = conversion.read_double_from_h5(master_file.c_str(), "range_spacing", &rangeSpacing);
//	ret = conversion.read_double_from_h5(master_file.c_str(), "slant_range_first_pixel", &nearRangeTime);
//	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_start_time", start_time);
//	ret = conversion.utc2gps(start_time.c_str(), &start);
//	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_stop_time", end_time);
//	ret = conversion.utc2gps(end_time.c_str(), &end);
//	ret = conversion.read_array_from_h5(master_file.c_str(), "state_vec", statevec);
//
//	//计算主图像轨道参数
//	Mat sate_pos1, sate_pos2, sate_vel1, sate_vel2;
//	sate_pos1.create(sceneHeight, 3, CV_64F);
//	sate_vel1.create(sceneHeight, 3, CV_64F);
//	sate_pos2.create(sceneHeight, 3, CV_64F);
//	sate_vel2.create(sceneHeight, 3, CV_64F);
//	Position pos; Velocity vel;
//	orbitStateVectors stateVectors(statevec, start, end);
//	stateVectors.applyOrbit();
//	for (int i = 0; i < sceneHeight; i++)
//	{
//		double time = start + (double)i * (1.0 / prf);
//		stateVectors.getPosition(time, pos);
//		stateVectors.getVelocity(time, vel);
//		sate_pos1.at<double>(i, 0) = pos.x;
//		sate_pos1.at<double>(i, 1) = pos.y;
//		sate_pos1.at<double>(i, 2) = pos.z;
//		sate_vel1.at<double>(i, 0) = vel.vx;
//		sate_vel1.at<double>(i, 1) = vel.vy;
//		sate_vel1.at<double>(i, 2) = vel.vz;
//	}
//	ret = conversion.read_str_from_h5(slave_file.c_str(), "acquisition_start_time", start_time);
//	ret = conversion.utc2gps(start_time.c_str(), &start);
//	ret = conversion.read_str_from_h5(slave_file.c_str(), "acquisition_stop_time", end_time);
//	ret = conversion.utc2gps(end_time.c_str(), &end);
//	ret = conversion.read_array_from_h5(slave_file.c_str(), "state_vec", statevec);
//	orbitStateVectors stateVectors2(statevec, start, end);
//	stateVectors2.applyOrbit();
//	for (int i = 0; i < sceneHeight; i++)
//	{
//		double time = start + (double)(i + move_r) * (1.0 / prf);
//		stateVectors2.getPosition(time, pos);
//		stateVectors2.getVelocity(time, vel);
//		sate_pos2.at<double>(i, 0) = pos.x;
//		sate_pos2.at<double>(i, 1) = pos.y;
//		sate_pos2.at<double>(i, 2) = pos.z;
//		sate_vel2.at<double>(i, 0) = vel.vx;
//		sate_vel2.at<double>(i, 1) = vel.vy;
//		sate_vel2.at<double>(i, 2) = vel.vz;
//	}
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_pos1", sate_pos1);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_pos2", sate_pos2);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_vel1", sate_vel1);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_vel2", sate_vel2);
//	//return 0;
//
//	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
//		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat;
//	int ret2 = 0;
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topLeftLon", &topleft_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topLeftLat", &topleft_lat);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topRightLon", &topright_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topRightLat", &topright_lat);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomLeftLon", &bottomleft_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomLeftLat", &bottomleft_lat);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomRightLon", &bottomright_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomRightLat", &bottomright_lat);
//	if (ret2 == 0)
//	{
//		Utils::computeImageGeoBoundry(topleft_lon, topleft_lat, topright_lon, topright_lat, bottomleft_lon, bottomleft_lat, bottomright_lon, bottomright_lat,
//			&lonMax, &latMax, &lonMin, &latMin);
//	}
//
//	Mat mappedLon, mappedLat, mappedDem;
//	//fprintf(stdout, "dem mapping start!\n");
//	//ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, 0, 0, sceneHeight, sceneWidth,
//	//	prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 50);
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\mapped_dem.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLat", mappedLat);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLon", mappedLon);
//
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "mapped_dem", mappedDem);
//	//mappedDem = 0.0;
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "mapped_lat", mappedLat);
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "mapped_lon", mappedLon);
//
//	conversion.read_slc_from_h5(master_deramped_file.c_str(), slc1);
//	conversion.read_slc_from_h5(slave_coregis_deramped_file.c_str(), slc2);
//	slc1.convertTo(slc1, CV_32F);
//	slc2.convertTo(slc2, CV_32F);
//	//fprintf(stdout, "deramping start!\n");
//	//flat.SLC_deramp(slc1, mappedDem, mappedLat, mappedLon, master_file.c_str(), 2);
//	//fprintf(stdout, "master file deramping finished!\n");
//	//flat.SLC_deramp(slc2, mappedDem, mappedLat, mappedLon, slave_coregis_file.c_str(), 2);
//	//fprintf(stdout, "slave file deramping finished!\n");
//	//conversion.creat_new_h5(master_deramped_file.c_str());
//	//conversion.write_slc_to_h5(master_deramped_file.c_str(), slc1);
//	//conversion.write_int_to_h5(master_deramped_file.c_str(), "range_len", slc1.GetCols());
//	//conversion.write_int_to_h5(master_deramped_file.c_str(), "azimuth_len", slc1.GetRows());
//	//conversion.Copy_para_from_h5_2_h5(master_file.c_str(), master_deramped_file.c_str());
//
//	//conversion.creat_new_h5(slave_coregis_deramped_file.c_str());
//	//conversion.write_slc_to_h5(slave_coregis_deramped_file.c_str(), slc2);
//	//conversion.write_int_to_h5(slave_coregis_deramped_file.c_str(), "range_len", slc2.GetCols());
//	//conversion.write_int_to_h5(slave_coregis_deramped_file.c_str(), "azimuth_len", slc2.GetRows());
//	//conversion.Copy_para_from_h5_2_h5(slave_coregis_file.c_str(), slave_coregis_deramped_file.c_str());
//
//	Mat phase;
//	util.Multilook(slc1, slc2, 8, 8, phase);
//	util.savephase("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_A_D.jpg", "jet", phase);
//	Mat phase_filter;
//	filter.Goldstein_filter(phase, phase_filter, 0.8, 128, 16);
//	//filter.slope_adaptive_filter(phase, phase_filter, 9, 9);
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_filter_A_D.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_filter_A_D.h5", "phase", phase_filter);
//	util.savephase("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_filter_A_D.jpg", "jet", phase_filter);
//	//return 0;
//	//解缠
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5", "phase", phase_filter);
//	Mat unwrapped_phase;
//	unwrap.snaphu(phase_filter, unwrapped_phase, "D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess");
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_filter_unwrapped_A_D.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_filter_unwrapped_A_D.h5", "phase", unwrapped_phase);
//	util.savephase("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_deramped_filter_unwrapped_A_D.jpg", "jet", unwrapped_phase);
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5", "phase", unwrapped_phase);
//	cv::resize(unwrapped_phase, unwrapped_phase, cv::Size(unwrapped_phase.cols * 8, unwrapped_phase.rows * 8));
//	Mat unwrapped_phase_final(slc1.GetRows(), slc1.GetCols(), CV_64F);
//	unwrapped_phase_final = 0.0;
//	unwrapped_phase.copyTo(unwrapped_phase_final(cv::Range(0, unwrapped_phase.rows), cv::Range(0, unwrapped_phase.cols)));
//	//return 0;
//	//校正至绝对相位
//	//解算斜距
//	Mat /*sate_pos1, sate_pos2, sate_vel1, sate_vel2,*/ R1, R2, phase_ref, phase_flat, R1_flat, R2_flat;
//	flat.slantrange_compute(R1, sate_pos1, sate_vel1, mappedDem, mappedLat, mappedLon, master_deramped_file.c_str());
//	flat.slantrange_compute(R2, sate_pos2, sate_vel2, mappedDem, mappedLat, mappedLon, slave_coregis_deramped_file.c_str());
//	//mappedDem = 0.0;
//	//flat.slantrange_compute(R1_flat, sate_pos1, sate_vel1, mappedDem, mappedLat, mappedLon, master_deramped_file.c_str());
//	//flat.slantrange_compute(R2_flat, sate_pos2, sate_vel2, mappedDem, mappedLat, mappedLon, slave_coregis_deramped_file.c_str());
//
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel.h5", "sate_pos1", sate_pos1);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel.h5", "sate_pos2", sate_pos2);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel.h5", "sate_vel1", sate_vel1);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel.h5", "sate_vel2", sate_vel2);
//
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\R.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\R.h5", "R1", R1);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\R.h5", "R2", R2);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R1_flat", R1_flat);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R2_flat", R2_flat);
//
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);
//
//	phase_ref.create(R1.rows, R1.cols, CV_64F); phase_ref = 0;
//	phase_flat.create(R1.rows, R1.cols, CV_64F); phase_flat = 0;
//	for (int i = 0; i < R1.rows; i++)
//	{
//		for (int j = 0; j < R1.cols; j++)
//		{
//			double r = R2.at<double>(i, j) - R1.at<double>(i, j);
//			phase_ref.at<double>(i, j) = r / wavelength * 2 * PI;
//			//r = R2_flat.at<double>(i, j) - R1_flat.at<double>(i, j);
//			//phase_flat.at<double>(i, j) = r / wavelength * 2 * PI;
//		}
//	}
//	unwrapped_phase_final = unwrapped_phase_final + phase_ref;
//	int model_ix;
//	Mat delta_phi = (unwrapped_phase_final - phase_ref) / (2 * PI);
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\delta_phi_A_D.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\delta_phi_A_D.h5", "delta_phi", delta_phi);
//	for (int i = 0; i < R1.rows; i++)
//	{
//		for (int j = 0; j < R1.cols; j++)
//		{
//			delta_phi.at<double>(i, j) = round(delta_phi.at<double>(i, j));
//		}
//	}
//	delta_phi.convertTo(delta_phi, CV_32S);
//	util.get_mode_index(delta_phi, &model_ix);
//	unwrapped_phase_final = unwrapped_phase_final - double(model_ix) * 2 * PI;
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_abs_A_D.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\phase_abs_A_D.h5", "phase_abs", unwrapped_phase_final);
//	//return 0;
//
//
//	//反演高程
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5", "phase_abs", unwrapped_phase_final);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_pos1", sate_pos1);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_pos2", sate_pos2);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\pos_vel2.h5", "sate_vel1", sate_vel1);
//	//offset_col = 0;
//	//sceneWidth = 5000;
//	//unwrapped_phase(Range(0, unwrapped_phase.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(unwrapped_phase);
//	//mappedDem(Range(0, mappedDem.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(mappedDem);
//	//mappedLat(Range(0, mappedLat.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(mappedLat);
//	//mappedLon(Range(0, mappedLon.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(mappedLon);
//	double lambda = wavelength;
//	Mat R_M(1, sceneWidth, CV_64F);
//	for (int i = 0; i < sceneWidth; i++)
//	{
//		R_M.at<double>(0, i) =  nearRangeTime * VEL_C / 2 + rangeSpacing * double(i + offset_col);
//
//	}
//	Mat ones = Mat::ones(sceneHeight, 1, CV_64F);
//	R_M = ones * R_M;
//	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase_final / (2 * PI);
//	Mat Satellite_M_T_Position = sate_pos1;//主星发射位置
//	Mat Satellite_S_T_Position;
//	Satellite_S_T_Position = sate_pos1;//辅星发射位置
//	Mat Satellite_M_R_Position = sate_pos1;//主星接收位置
//	Mat Satellite_S_R_Position = sate_pos2;//辅星接收位置
//	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
//	Mat Vs = sate_vel1;
//	ones = Mat::ones(sceneHeight, sceneWidth, CV_64F);
//	Mat dem_x, dem_y, dem_z;
//	dem_x = Mat::zeros(sceneHeight, sceneWidth, CV_64F);
//	dem_x.copyTo(dem_y); dem_x.copyTo(dem_z);
//	for (int i = 0; i < sceneHeight; i++)
//	{
//		for (int j = 0; j < sceneWidth; j++)
//		{
//			Position pos;
//			util.ell2xyz(mappedLon.at<double>(i, j), mappedLat.at<double>(i, j), mappedDem.at<short>(i, j), pos);
//			dem_x.at<double>(i, j) = pos.x;
//			dem_y.at<double>(i, j) = pos.y;
//			dem_z.at<double>(i, j) = pos.z;
//		}
//	}
//	
//	Mat M_T, M_R, S_T, S_R;
//	Mat f1, f2, f3;
//	Mat det_Df, Df_ni11, Df_ni12, Df_ni13, Df_ni21, Df_ni22, Df_ni23;
//	Mat Df11, Df12, Df13, Df21, Df22, Df23, Df31, Df32, Df33, Df_ni31, Df_ni32, Df_ni33;
//	Mat delta_Rt1, delta_Rt2, delta_Rt3;
//	ones = Mat::ones(1, sceneWidth, CV_64F);
//	Mat temp_var, temp_var1;
//	Mat fd = Mat::zeros(1, sceneWidth, CV_64F);
//	int iters = 15;
//	for (int i = 0; i < iters; i++)
//	{
//		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		temp_var = temp_var.mul(temp_var);
//		temp_var.copyTo(M_T);
//		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		temp_var = temp_var.mul(temp_var);
//		M_T = M_T + temp_var;
//		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		temp_var = temp_var.mul(temp_var);
//		M_T = M_T + temp_var;
//		cv::sqrt(M_T, f1);
//		f1 = f1 * 2 - 2 * R_M;
//
//		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		temp_var = temp_var.mul(temp_var);
//		temp_var.copyTo(S_T);
//		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		temp_var = temp_var.mul(temp_var);
//		S_T = S_T + temp_var;
//		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		temp_var = temp_var.mul(temp_var);
//		S_T = S_T + temp_var;
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - dem_x;
//		temp_var = temp_var.mul(temp_var);
//		temp_var.copyTo(S_R);
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - dem_y;
//		temp_var = temp_var.mul(temp_var);
//		S_R = S_R + temp_var;
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - dem_z;
//		temp_var = temp_var.mul(temp_var);
//		S_R = S_R + temp_var;
//
//		cv::sqrt(S_T, f2);
//		cv::sqrt(S_R, temp_var);
//		f2 = f2 + temp_var - R_F;
//
//
//		temp_var = Vs(Range(0, Vs.rows), Range(0, 1)) * ones;
//		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(0, 1)) * ones - dem_x;
//		f3 = temp_var.mul(temp_var1);
//		temp_var = Vs(Range(0, Vs.rows), Range(1, 2)) * ones;
//		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(1, 2)) * ones - dem_y;
//		f3 = f3 + temp_var.mul(temp_var1);
//		temp_var = Vs(Range(0, Vs.rows), Range(2, 3)) * ones;
//		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(2, 3)) * ones - dem_z;
//		f3 = f3 + temp_var.mul(temp_var1);
//		ones = Mat::ones(sceneHeight, 1, CV_64F);
//		temp_var = ones * fd;
//		temp_var1 = R_M * lambda / 2.0;
//		f3 = f3 + temp_var.mul(temp_var1);
//
//		//Dff
//		//第一行：f(1)的x，y，z的导数
//		ones = Mat::ones(1, sceneWidth, CV_64F);
//		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df11 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(0, 1)) * ones - dem_x;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df11 = Df11 + temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df12 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(1, 2)) * ones - dem_y;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df12 = Df12 + temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df13 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(2, 3)) * ones - dem_z;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df13 = Df13 + temp_var.mul(temp_var1);
//
//
//		//第二行：f(2)的x，y，z的导数
//		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		cv::sqrt(S_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df21 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - dem_x;
//		cv::sqrt(S_R, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df21 = Df21 + temp_var.mul(temp_var1);
//
//
//		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		cv::sqrt(S_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df22 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - dem_y;
//		cv::sqrt(S_R, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df22 = Df22 + temp_var.mul(temp_var1);
//
//
//		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		cv::sqrt(S_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df23 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - dem_z;
//		cv::sqrt(S_R, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df23 = Df23 + temp_var.mul(temp_var1);
//
//		//第三行：f(3)的x，y，z的导数
//		Df31 = -Vs(Range(0, Vs.rows), Range(0, 1)) * ones;
//		Df32 = -Vs(Range(0, Vs.rows), Range(1, 2)) * ones;
//		Df33 = -Vs(Range(0, Vs.rows), Range(2, 3)) * ones;
//
//		temp_var = Df11.mul(Df22);
//		temp_var = temp_var.mul(Df33);
//		temp_var.copyTo(det_Df);
//
//		temp_var = Df12.mul(Df23);
//		temp_var = temp_var.mul(Df31);
//		det_Df = det_Df + temp_var;
//
//		temp_var = Df13.mul(Df21);
//		temp_var = temp_var.mul(Df32);
//		det_Df = det_Df + temp_var;
//
//		temp_var = Df31.mul(Df22);
//		temp_var = temp_var.mul(Df13);
//		det_Df = det_Df - temp_var;
//
//		temp_var = Df32.mul(Df23);
//		temp_var = temp_var.mul(Df11);
//		det_Df = det_Df - temp_var;
//
//		temp_var = Df33.mul(Df21);
//		temp_var = temp_var.mul(Df12);
//		det_Df = det_Df - temp_var;
//
//		Df_ni11 = (Df22.mul(Df33) - Df32.mul(Df23)) / det_Df;
//		Df_ni12 = -(Df12.mul(Df33) - Df32.mul(Df13)) / det_Df;
//		Df_ni13 = (Df12.mul(Df23) - Df22.mul(Df13)) / det_Df;
//		delta_Rt1 = Df_ni11.mul(f1) + Df_ni12.mul(f2) + Df_ni13.mul(f3);
//
//
//		Df_ni21 = -(Df21.mul(Df33) - Df31.mul(Df23)) / det_Df;
//		Df_ni22 = (Df11.mul(Df33) - Df31.mul(Df13)) / det_Df;
//		Df_ni23 = -(Df11.mul(Df23) - Df21.mul(Df13)) / det_Df;
//		delta_Rt2 = Df_ni21.mul(f1) + Df_ni22.mul(f2) + Df_ni23.mul(f3);
//
//		Df_ni31 = (Df21.mul(Df32) - Df31.mul(Df22)) / det_Df;
//		Df_ni32 = -(Df11.mul(Df32) - Df31.mul(Df12)) / det_Df;
//		Df_ni33 = (Df22.mul(Df11) - Df21.mul(Df12)) / det_Df;
//		delta_Rt3 = Df_ni31.mul(f1) + Df_ni32.mul(f2) + Df_ni33.mul(f3);
//
//		dem_x = dem_x - delta_Rt1;
//		dem_y = dem_y - delta_Rt2;
//		dem_z = dem_z - delta_Rt3;
//		fprintf(stdout, "%d/%d\n", i + 1, iters);
//	}
//	delta_Rt1.release(); delta_Rt2.release(); delta_Rt3.release(); Df_ni31.release(); Df_ni32.release();
//	Df_ni33.release(); Df_ni21.release(); Df_ni22.release(); Df_ni23.release(); Df_ni11.release(); Df_ni12.release();
//	Df_ni13.release();
//	volatile bool parallel_flag = true;
//	dem.create(sceneHeight, sceneWidth, CV_64F);
//	Mat lon(sceneHeight, sceneWidth, CV_64F); Mat lat(sceneHeight, sceneWidth, CV_64F);
//#pragma omp parallel for schedule(guided) \
//	private(ret)
//	for (int i = 0; i < sceneHeight; i++)
//	{
//		Utils util;
//		for (int j = 0; j < sceneWidth; j++)
//		{
//			Mat xyz, llh;
//			xyz = Mat::zeros(1, 3, CV_64F);
//			xyz.at<double>(0, 0) = dem_x.at<double>(i, j);
//			xyz.at<double>(0, 1) = dem_y.at<double>(i, j);
//			xyz.at<double>(0, 2) = dem_z.at<double>(i, j);
//			ret = util.xyz2ell(xyz, llh);
//			dem.at<double>(i, j) = llh.at<double>(0, 2);
//			lat.at<double>(i, j) = llh.at<double>(0, 0);
//			lon.at<double>(i, j) = llh.at<double>(0, 1);
//		}
//	}
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5", "dem", dem);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5", "lat", lat);
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5", "lon", lon);
//	return 0;
//
//	//地理编码
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5", "dem", dem);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5", "lat", lat);
//	conversion.read_array_from_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\inversion_result_A_D2.h5", "lon", lon);
//	Mat dem_geocoded;
//	double lon_east, lon_west, lat_north, lat_south;
//	util.SAR2UTM(lon, lat, dem, dem_geocoded, 3, 1, &lon_east, &lon_west, &lat_north, &lat_south);
//	conversion.creat_new_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\geocoded_dem_A_D2.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\geocoded_dem_A_D2.h5", "dem_geocoded", dem_geocoded);
//	conversion.write_double_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\geocoded_dem_A_D2.h5", "lon_east", lon_east);
//	conversion.write_double_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\geocoded_dem_A_D2.h5", "lon_west", lon_west);
//	conversion.write_double_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\geocoded_dem_A_D2.h5", "lat_north", lat_north);
//	conversion.write_double_to_h5("D:\\working_dir\\papers\\multi-static_InSAR_forest_height\\preprocess\\geocoded_dem_A_D2.h5", "lat_south", lat_south);
//    return 0;
//}

/*宏图热带雨林数据处理(计算下视角、基线)*/
//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Registration coregis; Utils util; Deflat flat;
//	string master_deramped_file = "D:\\data\\HT\\nonforest\\deramped\\HT1-A-20240407_regis_deramp.h5";
//	string slave_coregis_deramped_file = "D:\\data\\HT\\nonforest\\deramped\\HT1-D-20240407_regis_deramp.h5";
//	Mat mapped_dem, mapped_lon, mapped_lat, baseline_v, baseline_p, incident_angle, sate_pos1, state_vec1, state_vec2, sate_vel1,
//		R1;
//	double B_effect, B_parallel, prf;
//	int sceneWidth, sceneHeight;
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "mapped_dem", mapped_dem);
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "mapped_lon", mapped_lon);
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "mapped_lat", mapped_lat);
//
//	//flat.slantrange_compute(R1, sate_pos1, sate_vel1, mapped_dem, mapped_lat, mapped_lon, master_deramped_file.c_str());
//	//conversion.creat_new_h5("D:\\data\\HT\\nonforest\\deramped\\R.h5");
//	//conversion.write_array_to_h5("D:\\data\\HT\\nonforest\\deramped\\R.h5", "R", R1);
//	//conversion.write_array_to_h5("D:\\data\\HT\\nonforest\\deramped\\R.h5", "sate_pos", sate_pos1);
//	//conversion.write_array_to_h5("D:\\data\\HT\\nonforest\\deramped\\R.h5", "sate_vel", sate_vel1);
//	//return 0;
//	conversion.read_array_from_h5(master_deramped_file.c_str(), "state_vec", state_vec1);
//	conversion.read_array_from_h5(slave_coregis_deramped_file.c_str(), "state_vec", state_vec2);
//	conversion.read_int_from_h5(master_deramped_file.c_str(), "azimuth_len", &sceneHeight);
//	conversion.read_int_from_h5(master_deramped_file.c_str(), "range_len", &sceneWidth);
//	conversion.read_double_from_h5(master_deramped_file.c_str(), "prf", &prf);
//	conversion.read_array_from_h5("D:\\data\\HT\\nonforest\\deramped\\R.h5", "sate_pos", sate_pos1);
//	baseline_v.create(sceneHeight, 1, CV_64F); baseline_v = 0.0;
//	baseline_p.create(sceneHeight, 1, CV_64F); baseline_p = 0.0;
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < sceneHeight; i++)
//	{
//		util.baseline_estimation(state_vec1, state_vec2, mapped_lon.at<double>(i, int(sceneWidth / 2)), mapped_lat.at<double>(i, int(sceneWidth / 2)),
//			mapped_dem.at<short>(i, int(sceneWidth / 2)), 0, 0, sceneWidth, sceneHeight, 1.0 / prf, 1.0 / prf,  &B_effect, &B_parallel);
//		baseline_v.at<double>(i, 0) = B_effect;
//		baseline_p.at<double>(i, 0) = B_parallel;
//		//fprintf(stdout, "%d\n", i + 1);
//	}
//	conversion.creat_new_h5("D:\\data\\HT\\nonforest\\deramped\\baseline_A_D.h5");
//	conversion.write_array_to_h5("D:\\data\\HT\\nonforest\\deramped\\baseline_A_D.h5", "B_effect", baseline_v);
//	conversion.write_array_to_h5("D:\\data\\HT\\nonforest\\deramped\\baseline_A_D.h5", "B_parallel", baseline_p);
//
////	incident_angle.create(1, sceneWidth, CV_64F); incident_angle = 0.0;
////#pragma omp parallel for schedule(guided)
////	for (int j = 0; j < sceneWidth; j++)
////	{
////		Mat p1(1, 3, CV_64F); Mat p2(1, 3, CV_64F); Mat p3(1, 3, CV_64F), tmp, xyz;
////		tmp = Mat::zeros(1, 3, CV_64F);
////		tmp.at<double>(0, 0) = mapped_lat.at<double>(int(sceneHeight / 2), j);
////		tmp.at<double>(0, 1) = mapped_lon.at<double>(int(sceneHeight / 2), j);;
////		tmp.at<double>(0, 2) = mapped_dem.at<short>(int(sceneHeight / 2), j);;
////		util.ell2xyz(tmp, xyz);
////		p1 = 0.0;
////		xyz.copyTo(p2);
////		sate_pos1(Range(int(sceneHeight / 2), int(sceneHeight / 2) + 1), Range(0, 3)).copyTo(p3);
////
////		Mat a = p2 - p3;
////		Mat b = p1 - p3;
////		Mat c = p1 - p2;
////		double a_norm = sqrt(sum(a.mul(a))[0]);
////		double b_norm = sqrt(sum(b.mul(b))[0]);
////		double c_norm = sqrt(sum(c.mul(c))[0]);
////		double theta = acos((a_norm * a_norm + b_norm * b_norm - c_norm * c_norm) / (2.0 * a_norm * b_norm)) / PI * 180.0;
////		incident_angle.at<double>(0, j) = theta;
////	}
////
////	conversion.creat_new_h5("D:\\data\\HT\\nonforest\\deramped\\incident_angle.h5");
////	conversion.write_array_to_h5("D:\\data\\HT\\nonforest\\deramped\\incident_angle.h5", "incident_angle", incident_angle);
//	
//
//
//	return 0;
//}

/*GEDI数据处理*/

//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Utils util; Deflat flat;
//	Mat zt, zg, rh100, lon, lat, dem, quality_index, zt_tmp, zg_tmp, rh100_tmp, lon_tmp, lat_tmp, dem_tmp, quality_index_tmp;
//	const char* file_name_list = "D:\\data\\GEDI\\French Guiana\\filename_list.txt";
//	char buffer[512];
//	memset(buffer, 0, 512);
//	FILE* fp = fopen(file_name_list, "rt");
//	int count = 0;
//	string str;
//	while (fgets(buffer, 512, fp))
//	{
//		str = buffer;
//		str = str.substr(str.rfind('/') + 1, str.length() - str.rfind('/') - 2);
//		str = "D:\\data\\GEDI\\French Guiana\\" + str;
//		conversion.read_height_metric_from_GEDI_L2B(str.c_str(), rh100_tmp, zg_tmp, zt_tmp, lon_tmp, lat_tmp, dem_tmp, quality_index_tmp);
//		if (count == 0)
//		{
//			rh100_tmp.copyTo(rh100);
//			zg_tmp.copyTo(zg);
//			zt_tmp.copyTo(zt);
//			lon_tmp.copyTo(lon);
//			lat_tmp.copyTo(lat);
//			dem_tmp.copyTo(dem);
//			quality_index_tmp.copyTo(quality_index);
//		}
//		else
//		{
//			cv::vconcat(rh100, rh100_tmp, rh100);
//			cv::vconcat(zg, zg_tmp, zg);
//			cv::vconcat(zt, zt_tmp, zt);
//			cv::vconcat(lon, lon_tmp, lon);
//			cv::vconcat(lat, lat_tmp, lat);
//			cv::vconcat(dem, dem_tmp, dem);
//			cv::vconcat(quality_index, quality_index_tmp, quality_index);
//		}
//		memset(buffer, 0, 512);
//		count++;
//	}
//	fclose(fp);
//	conversion.creat_new_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5");
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "rh100", rh100);
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "zg", zg);
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "zt", zt);
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "lon", lon);
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "lat", lat);
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "dem", dem);
//	conversion.write_array_to_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "quality_index", quality_index);
//
//	return 0;
//}

/*GEDI数据投影至雷达坐标系*/
//int main(int argc, char* argv[])
//{
//	FormatConversion conversion;
//	Mat zt, zg, rh100, dem, quality_index, longitude, latitude, stateVector;
//	string start_time, end_time;
//	double prf, rangeSpacing, wavelength, nearRangeTime, acquisitionStartTime, acquisitionStopTime;
//	int sceneHeight, sceneWidth;
//	conversion.read_array_from_h5("E:\\AfriSAR_LVIS\\0304\\LVIS2_Gabon2016_0304.h5", "RH100", rh100);
//	conversion.read_array_from_h5("E:\\AfriSAR_LVIS\\0304\\LVIS2_Gabon2016_0304.h5", "DTM", zg);
//	conversion.read_array_from_h5("E:\\AfriSAR_LVIS\\0304\\LVIS2_Gabon2016_0304.h5", "DSM", zt);
//	conversion.read_array_from_h5("E:\\AfriSAR_LVIS\\0304\\LVIS2_Gabon2016_0304.h5", "lon", longitude);
//	conversion.read_array_from_h5("E:\\AfriSAR_LVIS\\0304\\LVIS2_Gabon2016_0304.h5", "lat", latitude);
//	conversion.read_array_from_h5("E:\\AfriSAR_LVIS\\0304\\LVIS2_Gabon2016_0304.h5", "DSM", dem);
//	//conversion.read_array_from_h5("D:\\data\\GEDI\\French Guiana\\retrieved_info.h5", "quality_index", quality_index);
//	const char* slc_file = "D:\\data\\HT\\pongara\\deramped\\HT1-A-20240812_regis_deramp.h5";
//	conversion.read_int_from_h5(slc_file, "range_len", &sceneWidth);
//	conversion.read_int_from_h5(slc_file, "azimuth_len", &sceneHeight);
//	conversion.read_array_from_h5(slc_file, "state_vec", stateVector);
//	conversion.read_double_from_h5(slc_file, "prf", &prf);
//	conversion.read_double_from_h5(slc_file, "range_spacing", &rangeSpacing);
//	conversion.read_double_from_h5(slc_file, "carrier_frequency", &wavelength);
//	wavelength = VEL_C / wavelength;
//	conversion.read_double_from_h5(slc_file, "slant_range_first_pixel", &nearRangeTime);
//	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	conversion.read_str_from_h5(slc_file, "acquisition_start_time", start_time);
//	conversion.utc2gps(start_time.c_str(), &acquisitionStartTime);
//	conversion.read_str_from_h5(slc_file, "acquisition_stop_time", end_time);
//	conversion.utc2gps(end_time.c_str(), &acquisitionStopTime);
//	//84坐标系DEM插值
//	Mat DEM, stateVector_interp, rh100_out, zt_out, zg_out, quality_index_out;
//
//	rh100_out.create(sceneHeight, sceneWidth, CV_32F); rh100_out = 0;
//	zg_out.create(sceneHeight, sceneWidth, CV_32F); zg_out = 0.0;
//	zt_out.create(sceneHeight, sceneWidth, CV_32F); zt_out = 0.0;
//	quality_index_out.create(sceneHeight, sceneWidth, CV_8U); quality_index_out = 0.0;
//	short invalid = -9999;
//	rh100_out = rh100_out + invalid;
//	zg_out = zg_out - 9999.0;
//	zt_out = zt_out - 9999.0;
//	//初始化轨道类
//	double delta_t = stateVector.at<double>(1, 0) - stateVector.at<double>(0, 0);
//	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime, delta_t);
//	stateVectors.applyOrbit();
//	int ret;
//	double time_interval = 1.0 / prf;
//
//	int DEM_rows = rh100.rows; int DEM_cols = rh100.cols;
//	double dopplerFrequency = 0.0;
//	double lon_min = 9.260316-0.1; double lon_max = 9.498184+0.1;
//	double lat_min = 0.068961-0.1; double lat_max = 0.302019+0.1;
//	//采用迭代计算每个DEM点在SAR图像中的坐标，以减小计算量
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < DEM_rows; i++)
//	{
//		for (int j = 0; j < DEM_cols; j++)
//		{
//			Position groundPosition;
//			double lat, lon, height;
//			lat = latitude.at<double>(i, j);
//			lon = longitude.at<double>(i, j);
//			lon = lon > 180.0 ? (lon - 360.0) : lon;
//			height = dem.at<double>(i, j);
//			if (lat < lat_min || lat > lat_max || lon < lon_min || lon > lon_max /*|| quality_index.at<short>(i, j) < 1*/) continue;
//			Utils::ell2xyz(lon, lat, height, groundPosition);
//			int numOrbitVec = stateVectors.newStateVectors.rows;
//			double firstVecTime = 0.0;
//			double secondVecTime = 0.0;
//			double firstVecFreq = 0.0;
//			double secondVecFreq = 0.0;
//			double currentFreq, xdiff, ydiff, zdiff, distance = 1.0, zeroDopplerTime;
//			for (int ii = 0; ii < numOrbitVec; ii++) {
//				Position orb_pos(stateVectors.newStateVectors.at<double>(ii, 1), stateVectors.newStateVectors.at<double>(ii, 2),
//					stateVectors.newStateVectors.at<double>(ii, 3));
//				Velocity orb_vel(stateVectors.newStateVectors.at<double>(ii, 4), stateVectors.newStateVectors.at<double>(ii, 5),
//					stateVectors.newStateVectors.at<double>(ii, 6));
//				currentFreq = 0;
//				xdiff = groundPosition.x - orb_pos.x;
//				ydiff = groundPosition.y - orb_pos.y;
//				zdiff = groundPosition.z - orb_pos.z;
//				distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
//				currentFreq = 2.0 * (xdiff * orb_vel.vx + ydiff * orb_vel.vy + zdiff * orb_vel.vz) / (wavelength * distance);
//				if (ii == 0 || (firstVecFreq - dopplerFrequency) * (currentFreq - dopplerFrequency) > 0) {
//					firstVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
//					firstVecFreq = currentFreq;
//				}
//				else {
//					secondVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
//					secondVecFreq = currentFreq;
//					break;
//				}
//			}
//
//			if ((firstVecFreq - dopplerFrequency) * (secondVecFreq - dopplerFrequency) >= 0.0) {
//				continue;
//			}
//
//			double lowerBoundTime = firstVecTime;
//			double upperBoundTime = secondVecTime;
//			double lowerBoundFreq = firstVecFreq;
//			double upperBoundFreq = secondVecFreq;
//			double midTime, midFreq;
//			double diffTime = fabs(upperBoundTime - lowerBoundTime);
//			double absLineTimeInterval = time_interval;
//
//			int totalIterations = (int)(diffTime / absLineTimeInterval) + 1;
//			int numIterations = 0; Position pos; Velocity vel;
//			while (diffTime > absLineTimeInterval * 0.1 && numIterations <= totalIterations) {
//
//				midTime = (upperBoundTime + lowerBoundTime) / 2.0;
//				stateVectors.getPosition(midTime, pos);
//				stateVectors.getVelocity(midTime, vel);
//				xdiff = groundPosition.x - pos.x;
//				ydiff = groundPosition.y - pos.y;
//				zdiff = groundPosition.z - pos.z;
//				distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
//				midFreq = 2.0 * (xdiff * vel.vx + ydiff * vel.vy + zdiff * vel.vz) / (wavelength * distance);
//				if ((midFreq - dopplerFrequency) * (lowerBoundFreq - dopplerFrequency) > 0.0) {
//					lowerBoundTime = midTime;
//					lowerBoundFreq = midFreq;
//				}
//				else if ((midFreq - dopplerFrequency) * (upperBoundFreq - dopplerFrequency) > 0.0) {
//					upperBoundTime = midTime;
//					upperBoundFreq = midFreq;
//				}
//				else if (fabs(midFreq - dopplerFrequency) < 0.01) {
//					zeroDopplerTime = midTime;
//					break;
//				}
//
//				diffTime = fabs(upperBoundTime - lowerBoundTime);
//				numIterations++;
//			}
//
//
//			zeroDopplerTime = lowerBoundTime - lowerBoundFreq * (upperBoundTime - lowerBoundTime) / (upperBoundFreq - lowerBoundFreq);
//			int azimuthIndex = (zeroDopplerTime - acquisitionStartTime) / time_interval;
//			int rangeIndex = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing;
//			azimuthIndex = azimuthIndex - 0;
//			rangeIndex = rangeIndex - 0;
//			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 1 || rangeIndex < 0 || rangeIndex > sceneWidth - 1)
//			{
//
//			}
//			else
//			{
//				rh100_out.at<float>(azimuthIndex, rangeIndex) = rh100.at<double>(i, j);
//				zg_out.at<float>(azimuthIndex, rangeIndex) = zg.at<double>(i, j);
//				zt_out.at<float>(azimuthIndex, rangeIndex) = zt.at<double>(i, j);
//				//quality_index_out.at<short>(azimuthIndex, rangeIndex) = quality_index_out.at<short>(i, j);
//			}
//		}
//	}
//	conversion.creat_new_h5("D:\\data\\HT\\pongara\\deramped\\LVIS_mapped.h5");
//	conversion.write_array_to_h5("D:\\data\\HT\\pongara\\deramped\\LVIS_mapped.h5", "rh100_geocoded", rh100_out);
//	conversion.write_array_to_h5("D:\\data\\HT\\pongara\\deramped\\LVIS_mapped.h5", "zg_geocoded", zg_out);
//	conversion.write_array_to_h5("D:\\data\\HT\\pongara\\deramped\\LVIS_mapped.h5", "zt_geocoded", zt_out);
//	return 0;
//}

/*宏图数据地理编码*/
//int main(int argc, char* argv[])
//{
//	Utils util; FormatConversion conversion;
//	const char* file = "D:\\data\\HT\\mondah\\deramped\\HT1-A-20240812_regis_deramp.h5";
//	const char* file_mapped = "D:\\data\\HT\\mondah\\deramped\\slc_geocoded.jpg";
//	const char* result_file = "D:\\data\\HT\\mondah\\deramped\\slc_geocoded.h5";
//	Mat mapped_lon, mapped_lat, phase, mapped_phase;
//	ComplexMat slc;
//	double lon_east, lon_west, lat_north, lat_south;
//	conversion.read_array_from_h5(file, "mapped_lon", mapped_lon);
//	conversion.read_array_from_h5(file, "mapped_lat", mapped_lat);
//	conversion.read_slc_from_h5(file, slc);
//	phase = slc.GetMod();
//	phase.convertTo(phase, CV_64F);
//	util.SAR2UTM(mapped_lon, mapped_lat, phase, mapped_phase, 1, &lon_east, &lon_west, &lat_north, &lat_south);
//	util.saveAmplitude(file_mapped, mapped_phase);
//	conversion.creat_new_h5(result_file);
//	conversion.write_double_to_h5(result_file, "lon_east", lon_east);
//	conversion.write_double_to_h5(result_file, "lon_west", lon_west);
//	conversion.write_double_to_h5(result_file, "lat_north", lat_north);
//	conversion.write_double_to_h5(result_file, "lat_south", lat_south);
//	conversion.write_array_to_h5(result_file, "geocoded_slc", mapped_phase);
//	return 0;
//}

/*TanDEM-X投影至宏图数据雷达坐标系*/
//int main(int argc, char* argv[])
//{
//	const char* slc_file = "D:\\data\\HT\\mondah\\deramped\\HT1-A-20240812_regis_deramp.h5";
//	const char* tdx_file = "D:\\data\\HT\\mondah\\deramped\\tdx_dem_egm.h5";
//	Deflat flat; FormatConversion conversion;
//	string start_time, stop_time;
//	Mat dem84, mapped_dem, mapped_lon, mapped_lat, state_vec;
//	int sceneHeight, sceneWidth;
//	double wavelength, prf, range_spacing, nearRangeTime, acquisitionStartTime,
//		acquisitionStopTime, lat_upperleft, lon_upperleft, lon_spacing, lat_spacing;
//	conversion.read_array_from_h5(tdx_file, "dem", dem84);
//	dem84.convertTo(dem84, CV_16S);
//	conversion.read_array_from_h5(slc_file, "state_vec", state_vec);
//	conversion.read_double_from_h5(slc_file, "carrier_frequency", &wavelength);
//	wavelength = VEL_C / wavelength;
//	conversion.read_double_from_h5(slc_file, "prf", &prf);
//	conversion.read_double_from_h5(slc_file, "slant_range_first_pixel", &nearRangeTime);
//	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	conversion.read_double_from_h5(slc_file, "range_spacing", &range_spacing);
//	conversion.read_double_from_h5(tdx_file, "lat_upperleft", &lat_upperleft);
//	conversion.read_double_from_h5(tdx_file, "lon_upperleft", &lon_upperleft);
//	conversion.read_double_from_h5(tdx_file, "lon_spacing", &lon_spacing);
//	conversion.read_double_from_h5(tdx_file, "lat_spacing", &lat_spacing);
//	conversion.read_str_from_h5(slc_file, "acquisition_start_time", start_time);
//	conversion.utc2gps(start_time.c_str(), &acquisitionStartTime);
//	conversion.read_str_from_h5(slc_file, "acquisition_stop_time", stop_time);
//	conversion.utc2gps(stop_time.c_str(), &acquisitionStopTime);
//	conversion.read_int_from_h5(slc_file, "range_len", &sceneWidth);
//	conversion.read_int_from_h5(slc_file, "azimuth_len", &sceneHeight);
//
//	flat.demMapping(dem84, mapped_dem, mapped_lat, mapped_lon, lon_upperleft, lat_upperleft, 0, 0, sceneHeight,
//		sceneWidth, prf, range_spacing, wavelength, nearRangeTime, acquisitionStartTime, acquisitionStopTime, 
//		state_vec, 20, lon_spacing, lat_spacing);
//
//	conversion.write_array_to_h5(tdx_file, "mapped_dem", mapped_dem);
//
//
//
//	return 0;
//}

/*radarsat-1数据测试*/
//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Deflat flat;
//	ComplexMat slc1, slc2;
//	Utils util; Registration coreg;
//
//	//string acquisition_start_time = "2001-09-19T23:54:51.251";
//	//string acquisition_stop_time = "2001-09-19T23:55:09.548";
//	//string acquisition_start_time2 = "2002-01-17T23:54:40.737";
//	//string acquisition_stop_time2 = "2002-01-17T23:54:59.034";
//	//conversion.creat_new_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919.h5");
//	//conversion.creat_new_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117.h5");
//	//conversion.write_str_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919.h5", "acquisition_start_time", acquisition_start_time.c_str());
//	//conversion.write_str_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919.h5", "acquisition_stop_time", acquisition_stop_time.c_str());
//	//conversion.write_str_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117.h5", "acquisition_start_time", acquisition_start_time2.c_str());
//	//conversion.write_str_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117.h5", "acquisition_stop_time", acquisition_stop_time2.c_str());
//	//return 0;
//	conversion.read_slc_from_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5", slc1);
//	conversion.read_slc_from_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5", slc2);
//	//conversion.creat_new_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5");
//	//conversion.creat_new_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5");
//	//conversion.write_slc_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5", slc1);
//	//conversion.Copy_para_from_h5_2_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919.h5", "D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5");
//	//conversion.Copy_para_from_h5_2_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117.h5", "D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5");
//	//conversion.write_int_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5", "range_len", slc1.GetCols());
//	//conversion.write_int_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5", "azimuth_len", slc1.GetRows());
//	//conversion.write_int_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5", "range_len", slc1.GetCols());
//	//conversion.write_int_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5", "azimuth_len", slc1.GetRows());
//	////return 0;
//	//cout << "配准开始..."<<endl;
//	//coreg.coregistration_subpixel(slc1, slc2, 256, 8);
//	//cout << "配准完成..." << endl;
//	//conversion.write_slc_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5", slc2);
//	//cout << "配准数据写入完成..." << endl;
//	////return 0;
//
//	double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing,
//		nearRangeTime, wavelength, prf, start, end;
//	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
//		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat;
//	int sceneHeight, sceneWidth, offset_row = 0, offset_col = 0, TR_mode, ret;
//	Mat lon_coef, lat_coef, dem, mappedDem, statevec;
//	ComplexMat slc;
//	string start_time, end_time, master_file;
//	master_file = "D:\\data\\NewRadarsat-1\\2019725919\\20010919.h5";
//	TR_mode = 1;
//	sceneWidth = slc1.GetCols();
//	sceneHeight = slc1.GetRows();
//	//ret = conversion.read_int_from_h5(master_file.c_str(), "range_len", &sceneWidth);
//	//ret = conversion.read_int_from_h5(master_file.c_str(), "azimuth_len", &sceneHeight);
//	ret = conversion.read_double_from_h5(master_file.c_str(), "prf", &prf);
//	ret = conversion.read_double_from_h5(master_file.c_str(), "carrier_frequency", &wavelength);
//	wavelength = VEL_C / wavelength;
//	ret = conversion.read_double_from_h5(master_file.c_str(), "range_spacing", &rangeSpacing);
//	ret = conversion.read_double_from_h5(master_file.c_str(), "slant_range_first_pixel", &nearRangeTime);
//	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_start_time", start_time);
//	ret = conversion.utc2gps(start_time.c_str(), &start);
//	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_stop_time", end_time);
//	ret = conversion.utc2gps(end_time.c_str(), &end);
//	ret = conversion.read_array_from_h5(master_file.c_str(), "state_vec", statevec);
//
//	int ret2 = 0;
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topLeftLon", &topleft_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topLeftLat", &topleft_lat);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topRightLon", &topright_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topRightLat", &topright_lat);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomLeftLon", &bottomleft_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomLeftLat", &bottomleft_lat);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomRightLon", &bottomright_lon);
//	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomRightLat", &bottomright_lat);
//	Utils::computeImageGeoBoundry(topleft_lon, topleft_lat, topright_lon, topright_lat, bottomleft_lon, bottomleft_lat, bottomright_lon, bottomright_lat,
//		&lonMax, &latMax, &lonMin, &latMin);
//	lonMax = lonMax + 2;
//	latMax = latMax + 2;
//	lonMin = lonMin - 2;
//	latMin = latMin - 2;
//	ret = Utils::getSRTMDEM("D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//	double geoidHeight = 0.0;
//	string geofile_path = "D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\other\\egm96_15.gtx";
//	geoidHeight = Utils::getGeoidHeight(geofile_path.c_str(), lon_upperleft, lat_upperleft);
//	dem = short(round(geoidHeight)) + dem;
//	Mat mappedLon, mappedLat;
//	cout << "获取SRTM DEM成功..." << endl;
//	ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
//		prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 2);
//	cout << "DEM mapping成功..." << endl;
//	return 0;
//	conversion.write_array_to_h5(master_file.c_str(), "mapped_dem", mappedDem);
//	conversion.write_array_to_h5(master_file.c_str(), "mapped_lon", mappedLon);
//	conversion.write_array_to_h5(master_file.c_str(), "mapped_lat", mappedLat);
//
//	ret = flat.SLC_deramp(slc1, mappedDem, mappedLat, mappedLon, master_file.c_str(), TR_mode);
//	conversion.creat_new_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_deramped.h5");
//	conversion.write_slc_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_deramped.h5", slc1);
//	conversion.Copy_para_from_h5_2_h5("D:\\data\\NewRadarsat-1\\2019725919\\20010919_regis.h5", "D:\\data\\NewRadarsat-1\\2019725919\\20010919_deramped.h5");
//	cout << "主图SLC_deramp成功..." << endl;
//	ret = flat.SLC_deramp(slc2, mappedDem, mappedLat, mappedLon, "D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5", TR_mode);
//	conversion.creat_new_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_deramped.h5");
//	conversion.write_slc_to_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_deramped.h5", slc2);
//	conversion.Copy_para_from_h5_2_h5("D:\\data\\NewRadarsat-1\\2019725919\\20020117_regis.h5", "D:\\data\\NewRadarsat-1\\2019725919\\20020117_deramped.h5");
//	cout << "辅图SLC_deramp成功..." << endl;
//
//	
//
//	Mat phase;
//	util.Multilook(slc1, slc2, 4, 8, phase);
//	util.savephase("D:\\data\\NewRadarsat-1\\2019725919\\phase_deramped.jpg", "jet", phase);
//
//
//    return 0;
//}

/*AfriSAR数据地理编码（给Dinh）*/
int main() 
{
	Utils util; FormatConversion conversion; Unwrap unwrap;
	Mat az, rg, h_ref, az_o1, az_o2, rg_o1, rg_o2, kz, inc, R, kz_geocoded, inc_geocoded, R_geocoded;
	ComplexMat slc, slc_geocoded;
	double easting_min = 532811.0488658018;
	double easting_max = 541603.0488658018;
	double northing_min = 57847.2851774608;
	double northing_max = 64695.2851774608;
	double spacing = 2.0;
	const char* geocoded_file = "C:\\Users\\zengg\\Desktop\\geocoded\\Mondah\\Mondah_VV_11.h5";
	const char* slc_file = "E:\\AfriSAR\\DLR\\FL02\\PS12\\TP01\\INF\\INF-SR\\slc_coreg_16afrisr0502_16afrisr0212_Pvv_tP01.h5";
	const char* kz_file = "E:\\AfriSAR\\DLR\\FL02\\PS12\\TP01\\INF\\INF-SR\\kz_16afrisr0502_16afrisr0212_Phh_tP01.h5";
	const char* inc_file = "E:\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\RGI\\RGI-SR\\incidence_16afrisr0502_P_tP01.h5";
	const char* geocode_info = "E:\\AfriSAR\\DLR\\FL05\\PS02\\TP01\\GTC\\GTC-LUT\\geocode_info.h5";
	conversion.read_array_from_h5(geocode_info, "az", az);
	conversion.read_array_from_h5(geocode_info, "rg", rg);
	conversion.read_array_from_h5(geocode_info, "h_ref", h_ref);
	h_ref.convertTo(h_ref, CV_32F);
	//conversion.read_array_from_h5(geocode_info, "az_o1", az_o1);
	//conversion.read_array_from_h5(geocode_info, "az_o2", az_o2);
	//conversion.read_array_from_h5(geocode_info, "rg_o1", rg_o1);
	//conversion.read_array_from_h5(geocode_info, "rg_o2", rg_o2);
	conversion.read_slc_from_h5(slc_file, slc);
	conversion.read_array_from_h5(kz_file, "kz", kz);
	conversion.read_array_from_h5(inc_file, "kz", inc);

	int rows = az.rows; int cols = az.cols;
	int rg_n = slc.GetCols(); int az_n = slc.GetRows();
	Mat mask = Mat::ones(rows, cols, CV_32S);
	kz_geocoded = Mat::zeros(rows, cols, CV_32F) - 9999.0;
	inc_geocoded = Mat::zeros(rows, cols, CV_32F) - 9999.0;
	R_geocoded = Mat::zeros(rows, cols, CV_32F) - 9999.0;
	slc_geocoded.re.create(rows, cols, CV_32F); slc_geocoded.re = 0.0;
	slc_geocoded.im.create(rows, cols, CV_32F); slc_geocoded.im = 0.0;


	double R0 = 6536.3352;
	double delta_r = 1.1988876;
	R = Mat::zeros(az_n, rg_n, CV_32F);
	for (int i = 0; i < az_n; i++)
	{
		for (int j = 0; j < rg_n; j++)
		{
			R.at<float>(i, j) = static_cast<float>(R0 + double(j - 1) * delta_r);
		}
	}

	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			double rg_ix, az_ix, upper, lower;
			int row_lower, row_upper, col_left, col_right;
			rg_ix = rg.at<double>(i, j);
			az_ix = az.at<double>(i, j);
			row_lower = int(az_ix);
			row_upper = row_lower + 1;
			col_left = int(rg_ix);
			col_right = col_left + 1;
			if (row_lower < 0 || row_upper > az_n - 1 || col_left < 0 || col_right > rg_n - 1)
			{
				mask.at<int>(i, j) = 0;
				continue;
			}

			//slc插值
			//实部
			upper = slc.re.at<float>(row_upper, col_left) + (rg_ix - col_left) * (slc.re.at<float>(row_upper, col_right) - slc.re.at<float>(row_upper, col_left));
			lower = slc.re.at<float>(row_lower, col_left) + (rg_ix - col_left) * (slc.re.at<float>(row_lower, col_right) - slc.re.at<float>(row_lower, col_left));
			slc_geocoded.re.at<float>(i, j) = static_cast<float>(lower + (az_ix - row_lower) * (upper - lower));
			//虚部
			upper = slc.im.at<float>(row_upper, col_left) + (rg_ix - col_left) * (slc.im.at<float>(row_upper, col_right) - slc.im.at<float>(row_upper, col_left));
			lower = slc.im.at<float>(row_lower, col_left) + (rg_ix - col_left) * (slc.im.at<float>(row_lower, col_right) - slc.im.at<float>(row_lower, col_left));
			slc_geocoded.im.at<float>(i, j) = static_cast<float>(lower + (az_ix - row_lower) * (upper - lower));

			//kz插值
			upper = kz.at<float>(row_upper, col_left) + (rg_ix - col_left) * (kz.at<float>(row_upper, col_right) - kz.at<float>(row_upper, col_left));
			lower = kz.at<float>(row_lower, col_left) + (rg_ix - col_left) * (kz.at<float>(row_lower, col_right) - kz.at<float>(row_lower, col_left));
			kz_geocoded.at<float>(i, j) = static_cast<float>(lower + (az_ix - row_lower) * (upper - lower));

			//inc插值
			upper = inc.at<float>(row_upper, col_left) + (rg_ix - col_left) * (inc.at<float>(row_upper, col_right) - inc.at<float>(row_upper, col_left));
			lower = inc.at<float>(row_lower, col_left) + (rg_ix - col_left) * (inc.at<float>(row_lower, col_right) - inc.at<float>(row_lower, col_left));
			inc_geocoded.at<float>(i, j) = static_cast<float>(lower + (az_ix - row_lower) * (upper - lower));
			//R插值
			upper = R.at<float>(row_upper, col_left) + (rg_ix - col_left) * (R.at<float>(row_upper, col_right) - R.at<float>(row_upper, col_left));
			lower = R.at<float>(row_lower, col_left) + (rg_ix - col_left) * (R.at<float>(row_lower, col_right) - R.at<float>(row_lower, col_left));
			R_geocoded.at<float>(i, j) = static_cast<float>(lower + (az_ix - row_lower) * (upper - lower));
		}
	}

	conversion.creat_new_h5(geocoded_file);
	conversion.write_slc_to_h5(geocoded_file, slc_geocoded);
	conversion.write_array_to_h5(geocoded_file, "R", R_geocoded);
	conversion.write_array_to_h5(geocoded_file, "kz", kz_geocoded);
	conversion.write_array_to_h5(geocoded_file, "inc", inc_geocoded);
	conversion.write_array_to_h5(geocoded_file, "dem", h_ref);
	conversion.write_array_to_h5(geocoded_file, "mask", mask);
	conversion.write_double_to_h5(geocoded_file, "spacing", spacing);
	conversion.write_double_to_h5(geocoded_file, "easting_min", easting_min);
	conversion.write_double_to_h5(geocoded_file, "easting_max", easting_max);
	conversion.write_double_to_h5(geocoded_file, "northing_min", northing_min);
	conversion.write_double_to_h5(geocoded_file, "northing_max", northing_max);
	
	return 0;
}

//int main(int argc, char* argv[])
//{
//	const char* slc_file = "D:\\data\\HT\\pongara\\deramped\\HT1-A-20240812_regis_deramp.h5";
//	const char* tdx_file = "D:\\data\\HT\\pongara\\deramped\\tdx_dem_egm.h5";
//	const char* egm_file = "D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\other\\egm96_15.gtx";
//	const char* file_mapped = "D:\\data\\HT\\pongara\\deramped\\geocoded2.jpg";
//	const char* result_file = "D:\\data\\HT\\pongara\\deramped\\geocoded2.h5";
//	Deflat flat; FormatConversion conversion; Utils util;
//	string start_time, stop_time;
//	Mat dem84, mapped_dem, mapped_lon, mapped_lat, state_vec;
//	int sceneHeight, sceneWidth;
//	double wavelength, prf, range_spacing, nearRangeTime, acquisitionStartTime,
//		acquisitionStopTime, lat_upperleft, lon_upperleft, lon_spacing, lat_spacing;
//	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
//		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat, lonMax, lonMin, latMax, latMin;
//	conversion.read_array_from_h5(slc_file, "state_vec", state_vec);
//	conversion.read_double_from_h5(slc_file, "carrier_frequency", &wavelength);
//	wavelength = VEL_C / wavelength;
//	conversion.read_double_from_h5(slc_file, "prf", &prf);
//	conversion.read_double_from_h5(slc_file, "slant_range_first_pixel", &nearRangeTime);
//	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	conversion.read_double_from_h5(slc_file, "range_spacing", &range_spacing);
//	conversion.read_str_from_h5(slc_file, "acquisition_start_time", start_time);
//	conversion.utc2gps(start_time.c_str(), &acquisitionStartTime);
//	conversion.read_str_from_h5(slc_file, "acquisition_stop_time", stop_time);
//	conversion.utc2gps(stop_time.c_str(), &acquisitionStopTime);
//	conversion.read_int_from_h5(slc_file, "range_len", &sceneWidth);
//	conversion.read_int_from_h5(slc_file, "azimuth_len", &sceneHeight);
//
//	int ret2 = 0;
//	ret2 += conversion.read_double_from_h5(slc_file, "topLeftLon", &topleft_lon);
//	ret2 += conversion.read_double_from_h5(slc_file, "topLeftLat", &topleft_lat);
//	ret2 += conversion.read_double_from_h5(slc_file, "topRightLon", &topright_lon);
//	ret2 += conversion.read_double_from_h5(slc_file, "topRightLat", &topright_lat);
//	ret2 += conversion.read_double_from_h5(slc_file, "bottomLeftLon", &bottomleft_lon);
//	ret2 += conversion.read_double_from_h5(slc_file, "bottomLeftLat", &bottomleft_lat);
//	ret2 += conversion.read_double_from_h5(slc_file, "bottomRightLon", &bottomright_lon);
//	ret2 += conversion.read_double_from_h5(slc_file, "bottomRightLat", &bottomright_lat);
//	Utils::computeImageGeoBoundry(topleft_lon, topleft_lat, topright_lon, topright_lat, bottomleft_lon, bottomleft_lat, bottomright_lon, bottomright_lat,
//		&lonMax, &latMax, &lonMin, &latMin);
//
//	Utils::getSRTMDEM("D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", dem84, 
//		&lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//	double geoidHeight = 0.0;
//	geoidHeight = Utils::getGeoidHeight(egm_file, lonMax, latMax);
//	dem84 = short(round(geoidHeight)) + dem84;
//	std::cout << "dem mapping...\n";
//	flat.demMapping(dem84, mapped_dem, mapped_lat, mapped_lon, lon_upperleft, lat_upperleft, 0, 0, sceneHeight,
//		sceneWidth, prf, range_spacing, wavelength, nearRangeTime, acquisitionStartTime, acquisitionStopTime,
//		state_vec, 20);
//
//	Mat phase, mapped_phase;
//	ComplexMat slc;
//	double lon_east, lon_west, lat_north, lat_south;
//	conversion.read_slc_from_h5(slc_file, slc);
//	phase = slc.GetMod();
//	phase.convertTo(phase, CV_64F);
//	std::cout << "geocoding...\n";
//	util.SAR2UTM(mapped_lon, mapped_lat, phase, mapped_phase, 1, &lon_east, &lon_west, &lat_north, &lat_south);
//	util.saveAmplitude(file_mapped, mapped_phase);
//	conversion.creat_new_h5(result_file);
//	conversion.write_double_to_h5(result_file, "lon_east", lon_east);
//	conversion.write_double_to_h5(result_file, "lon_west", lon_west);
//	conversion.write_double_to_h5(result_file, "lat_north", lat_north);
//	conversion.write_double_to_h5(result_file, "lat_south", lat_south);
//	conversion.write_array_to_h5(result_file, "geocoded_slc", mapped_phase);
//
//
//
//	return 0;
//}