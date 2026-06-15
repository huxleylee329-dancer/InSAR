// test.cpp : 定义控制台应用程序的入口点。
//

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
#include"..\include\sar_comm.h"
#include"..\include\SBAS.h"
#include"..\include\FormatConversion.h"
#include"..\include\SLC_simulator.h"
#include"gdal_priv.h"
#include<time.h>
#include<SensAPI.h>
#include<urlmon.h>
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
#pragma comment(lib, "SBAS_d.lib")
#pragma comment(lib, "simulation_d.lib")
#else
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "Unwrap.lib")
#pragma comment(lib, "Registration.lib")
#pragma comment(lib, "Filter.lib")
#pragma comment(lib, "Deflat.lib")
#pragma comment(lib, "Dem.lib")
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "FormatConversion.lib")
#pragma comment(lib, "SBAS.lib")
#pragma comment(lib, "simulation.lib")
#endif // _DEBUG
#pragma comment(lib,"URlmon")
#pragma comment(lib, "Sensapi.lib")


using cv::Mat;
using cv::Range;

enum ConvolutionType {
	/* Return the full convolution, including border */
	CONVOLUTION_FULL,

	/* Return only the part that corresponds to the original image */
	CONVOLUTION_SAME,
	/* Return only the submatrix containing elements that were not influenced by the border */
	CONVOLUTION_VALID
};
Mat conv2(const Mat& img, const Mat& ikernel, ConvolutionType type)
{
	Mat dest;
	Mat kernel;
	flip(ikernel, kernel, -1);
	Mat source = img;
	if (CONVOLUTION_FULL == type)
	{
		source = Mat();
		const int additionalRows = kernel.rows - 1, additionalCols = kernel.cols - 1;
		copyMakeBorder(img, source, (additionalRows + 1) / 2, additionalRows / 2, (additionalCols + 1) / 2, additionalCols / 2, cv::BORDER_CONSTANT, cv::Scalar(0));
	}
	cv::Point anchor(kernel.cols - kernel.cols / 2 - 1, kernel.rows - kernel.rows / 2 - 1);
	int borderMode = cv::BORDER_CONSTANT;
	filter2D(source, dest, img.depth(), kernel, anchor, 0, borderMode);

	if (CONVOLUTION_VALID == type)
	{
		dest = dest.colRange((kernel.cols - 1) / 2, dest.cols - kernel.cols / 2).rowRange((kernel.rows - 1) / 2, dest.rows - kernel.rows / 2);
	}
	return dest;
}

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

//int main(int argc, char* argv[])
//{
//	Utils util; Unwrap unwrap; Deflat flat; FormatConversion conversion; Registration regis; SBAS sbas; SLC_simulator simulator;
//	Filter filter; Dem DEM;
//	//const char* filename = "D:\\data\\AfriSAR\\Mabounie\\CHM_Main_2m.tif";
//	//GDALAllRegister();	//注册已知驱动
//	//GDALDataset* poDataset = (GDALDataset*)GDALOpen(filename, GA_ReadOnly);	//打开cos文件
//	//if (poDataset == NULL)
//	//{
//	//	fprintf(stderr, "read_slc_from_TSXcos(): failed to open %s!\n", filename);
//	//	GDALDestroyDriverManager();
//	//	return -1;
//	//}
//	//int nBand = poDataset->GetRasterCount();	//获取波段数（cos应为1）
//	//nBand = poDataset->GetLayerCount();
//	//string x = poDataset->GetProjectionRef();
//	//double xx[6];
//	//poDataset->GetGeoTransform(&xx[0]);
//	//int xsize = poDataset->GetRasterXSize();
//	//int ysize = poDataset->GetRasterYSize();
//	//GDALRasterBand* poBand = poDataset->GetRasterBand(1);	//获取指向波段1的指针
//	//GDALDataType dataType = poBand->GetRasterDataType();	//数据存储类型，cos应为GDT_CInt16
//	//Mat dtm(ysize, xsize, CV_64F);
//	//poBand->RasterIO(GF_Read, 0, 0, xsize, ysize, (void*)dtm.data, xsize, ysize, dataType, 0, 0);		//读取复图像数据到pbuf中
//	//dtm.convertTo(dtm, CV_64F);
//	//util.cvmat2bin("D:\\working_dir\\papers\\SM_separation\\AfriSAR\\P_band\\Mabounie\\dtm.bin", dtm);
//	return 0;
//
//
//	//三角网络解缠
//	/*const char* usage = "delaunay_unwrap.exe phase_file mask_file out_folder\n";
//	if (argc != 4)
//	{
//		fprintf(stderr, usage);
//		return -1;
//	}
//	Mat mask, phase, coherence;
//	SBAS sbas; Utils util; Unwrap unwrap;
//	int ret;
//	char szFilePath[MAX_PATH + 1] = { 0 };
//	GetModuleFileNameA(NULL, szFilePath, MAX_PATH);
//	string path1(szFilePath);
//	path1 = path1.substr(0, path1.rfind("\\"));
//	string node_file = path1 + "\\high_coherence.node";
//	string edge_file = path1 + "\\high_coherence.1.edge";
//	string ele_file = path1 + "\\high_coherence.1.ele";
//	string neigh_file = path1 + "\\high_coherence.1.neigh";
//	string mcf_problem = path1 + "\\mcf_problem.net";
//	string mcf_solution = path1 + "\\mcf_problem.net.sol";
//	string out = string(argv[3]) + "\\unwrapped_phase.bin";
//	vector<SBAS_edge> edges;
//	vector<SBAS_node> nodes;
//	vector<SBAS_triangle> triangles;
//	vector<int> node_neighbours;
//
//	ret = util.bin2cvmat(argv[1], phase);
//	if (ret < 0)
//	{
//		fprintf(stderr, "can't read %s\n", argv[1]);
//		return -1;
//	}
//	ret = util.bin2cvmat(argv[2], mask);
//	if (ret < 0)
//	{
//		fprintf(stderr, "can't read %s\n", argv[2]);
//		return -1;
//	}
//	mask.convertTo(mask, CV_32S);
//	int nonzero = cv::countNonZero(mask);
//	sbas.write_high_coherence_node(mask, node_file.c_str());
//	util.gen_delaunay(node_file.c_str(), path1.c_str());
//	sbas.read_edges(edge_file.c_str(), nonzero, edges, node_neighbours);
//	sbas.init_SBAS_node(nodes, edges, node_neighbours);
//	sbas.init_SBAS_triangle(ele_file.c_str(), neigh_file.c_str(), triangles, edges, nodes);
//	sbas.set_high_coherence_node_coordinate(mask, nodes);
//
//	util.phase_coherence(phase, coherence);
//	sbas.set_high_coherence_node_phase(mask, nodes, edges, phase);
//	sbas.set_weight_by_coherence(coherence, nodes, edges);
//	sbas.compute_high_coherence_residue(nodes, edges, triangles);
//	int num_residues = 0; double obj;
//	sbas.residue_num(triangles, &num_residues);
//	if (num_residues > 0)
//	{
//		sbas.writeDIMACS_spatial(mcf_problem.c_str(), nodes, edges, triangles);
//		unwrap.mcf_delaunay(mcf_problem.c_str(), path1.c_str());
//		sbas.readDIMACS(mcf_solution.c_str(), nodes, edges, triangles, &obj);
//	}
//	sbas.floodFillUnwrap(nodes, edges, 1, false);
//	sbas.retrieve_unwrapped_phase(nodes, phase);
//	util.cvmat2bin(out.c_str(), phase);
//	return 0;*/
//
//	//多基线相位估计之特征值分解法
//	/*FormatConversion conversion; Utils util;
//	const char* usage = "usage: EVD.exe number_of_images blocksize wndsize parameter_file\n";
//	if (argc != 5)
//	{
//		fprintf(stderr, usage);
//		return -1;
//	}
//	int ret, numOfImages, blocksize, wndsize;
//	int max_filename_length = 256;
//	ret = sscanf(argv[1], "%d", &numOfImages);
//	if (ret != 1)
//	{
//		fprintf(stderr, "invalide input format!\n");
//		return -1;
//	}
//	ret = sscanf(argv[2], "%d", &blocksize);
//	if (ret != 1)
//	{
//		fprintf(stderr, "invalide input format!\n");
//		return -1;
//	}
//	ret = sscanf(argv[3], "%d", &wndsize);
//	if (ret != 1)
//	{
//		fprintf(stderr, "invalide input format!\n");
//		return -1;
//	}
//	if (wndsize % 2 == 0) wndsize++;
//	if (wndsize >= blocksize)
//	{
//		fprintf(stderr, "wndsize should be smaller than blocksize!\n");
//		return -1;
//	}
//	FILE* fp = NULL; char str[256]; memset(str, 0, 256);
//	char str2[256]; memset(str2, 0, 256);
//	vector<string> input_files, output_files;
//	fp = fopen(argv[4], "rt");
//	if (!fp)
//	{
//		fprintf(stderr, "can't open file %s!\n", argv[4]);
//		return -1;
//	}
//	while (fgets(str, max_filename_length, fp))
//	{
//		sscanf(str, "%s", str2);
//		input_files.push_back(str2);
//	}
//	fclose(fp);
//	if ((size_t)numOfImages != input_files.size())
//	{
//		fprintf(stderr, "number of images mismatch!\n");
//		return -1;
//	}
//	for (int i = 0; i < numOfImages; i++)
//	{
//		string out = input_files[i].substr(0, input_files[i].length() - 3) + "_out.h5";
//		output_files.push_back(out);
//	}
//	double thresh_c1_to_c2 = 0.7;
//	int nr, nc, blocksize_row = blocksize, blocksize_col = blocksize;
//	int homotest_radius = (wndsize - 1) / 2;
//	int left, right, top, bottom, block_num_row, block_num_col, left_pad, right_pad, top_pad, bottom_pad;
//	vector<ComplexMat> slc_series, slc_series_filter;
//	ComplexMat slc;
//	//预先填充
//	ret = conversion.read_slc_from_h5(input_files[0].c_str(), slc);
//	nr = slc.GetRows(); nc = slc.GetCols();
//	Mat ph, phase, egv(numOfImages, nr * nc, CV_64F),
//		coh_mat_re(numOfImages * numOfImages, nr * nc, CV_64F),
//		coh_mat_im(numOfImages * numOfImages, nr * nc, CV_64F); coh_mat_re = 0.0; coh_mat_im = 0.0, egv = 0.0;
//	for (int i = 0; i < numOfImages; i++)
//	{
//		ret = conversion.creat_new_h5(output_files[i].c_str());
//		ret = conversion.write_slc_to_h5(output_files[i].c_str(), slc);
//	}
//	if (nr % blocksize_row == 0) block_num_row = nr / blocksize_row;
//	else block_num_row = int(floor((double)nr / (double)blocksize_row)) + 1;
//	if (nc % blocksize_col == 0) block_num_col = nc / blocksize_col;
//	else block_num_col = int(floor((double)nc / (double)blocksize_col)) + 1;
//	for (int i = 0; i < block_num_row; i++)
//	{
//		for (int j = 0; j < block_num_col; j++)
//		{
//			top = i * blocksize_row;
//			top_pad = top - homotest_radius; top_pad = top_pad < 0 ? 0 : top_pad;
//			bottom = top + blocksize_row; bottom = bottom > nr ? nr : bottom;
//			bottom_pad = bottom + homotest_radius; bottom_pad = bottom_pad > nr ? nr : bottom_pad;
//			left = j * blocksize_col;
//			left_pad = left - homotest_radius; left_pad = left_pad < 0 ? 0 : left_pad;
//			right = left + blocksize_col; right = right > nc ? nc : right;
//			right_pad = right + homotest_radius; right_pad = right_pad > nc ? nc : right_pad;
//			//读取数据
//			for (int k = 0; k < numOfImages; k++)
//			{
//				ret = conversion.read_subarray_from_h5(input_files[k].c_str(), "s_re",
//					top_pad, left_pad, bottom_pad - top_pad, right_pad - left_pad, slc.re);
//				ret = conversion.read_subarray_from_h5(input_files[k].c_str(), "s_im",
//					top_pad, left_pad, bottom_pad - top_pad, right_pad - left_pad, slc.im);
//				if (slc.type() != CV_64F) slc.convertTo(slc, CV_64F);
//				slc_series.push_back(slc);
//				slc_series_filter.push_back(slc);
//			}
//			//计算
//#pragma omp parallel for schedule(guided)
//			for (int ii = (top - top_pad); ii < (bottom - top_pad); ii++)
//			{
//				ComplexMat coherence_matrix, eigenvector; Mat eigenvalue; int ret1;
//				Mat real1, imag1, real2, imag2, real, imag, coh_tmp;
//				double value1, value2;
//				for (int jj = (left - left_pad); jj < (right - left_pad); jj++)
//				{
//					ret1 = util.coherence_matrix_estimation(slc_series, coherence_matrix, wndsize, wndsize, ii, jj, false, true);
//					if (ret1 == 0)
//					{
//						ret1 = util.HermitianEVD(coherence_matrix, eigenvalue, eigenvector);
//						int xrow = (ii + top_pad); int xcol = (jj + left_pad);
//						xrow = xrow > nr - 1 ? nr - 1 : xrow;
//						xcol = xcol > nc - 1 ? nc - 1 : xcol;
//						eigenvalue.copyTo(egv(cv::Range(0, numOfImages), cv::Range(nc * xrow + xcol, nc * xrow + xcol + 1)));
//						coh_tmp = coherence_matrix.re.reshape(0, numOfImages * numOfImages);
//						coh_tmp.copyTo(coh_mat_re(cv::Range(0, numOfImages* numOfImages), cv::Range(nc * xrow + xcol, nc * xrow + xcol + 1)));
//						coh_tmp = coherence_matrix.im.reshape(0, numOfImages * numOfImages);
//						coh_tmp.copyTo(coh_mat_im(cv::Range(0, numOfImages * numOfImages), cv::Range(nc * xrow + xcol, nc * xrow + xcol + 1)));
//						if (!eigenvalue.empty() && ret1 == 0)
//						{
//							if (eigenvalue.at<double>(1, 0) / (eigenvalue.at<double>(0, 0) + 1e-10) < thresh_c1_to_c2)
//							{
//								for (int kk = 0; kk < numOfImages; kk++)
//								{
//									slc_series_filter[kk].re.at<double>(ii, jj) = eigenvector.re.at<double>(kk, 0);
//									slc_series_filter[kk].im.at<double>(ii, jj) = eigenvector.im.at<double>(kk, 0);
//								}
//							}
//						}
//					}
//				}
//			}
//			//储存
//			for (int kk = 0; kk < numOfImages; kk++)
//			{
//				slc_series_filter[kk].re(cv::Range(top - top_pad, bottom - top_pad), cv::Range(left - left_pad, right - left_pad)).copyTo(ph);
//				//ph.convertTo(ph, CV_32F);
//				ret = conversion.write_subarray_to_h5(output_files[kk].c_str(), "s_re", ph, top, left, bottom - top, right - left);
//				slc_series_filter[kk].im(cv::Range(top - top_pad, bottom - top_pad), cv::Range(left - left_pad, right - left_pad)).copyTo(ph);
//				//ph.convertTo(ph, CV_32F);
//				ret = conversion.write_subarray_to_h5(output_files[kk].c_str(), "s_im", ph, top, left, bottom - top, right - left);
//			}
//			slc_series.clear();
//			slc_series_filter.clear();
//			printf("\r估计进度：%lf%%", double(i * block_num_col + j + 1) / double((block_num_col) * (block_num_row)) * 100.0);
//			fflush(stdout);
//		}
//	}	
//	size_t pos = input_files[0].rfind("\\");
//	string egv_out_file = input_files[0].substr(0, pos) + "\\eigenvalue.h5";
//	conversion.creat_new_h5(egv_out_file.c_str());
//	conversion.write_array_to_h5(egv_out_file.c_str(), "eigenvalue", egv);
//	conversion.write_array_to_h5(egv_out_file.c_str(), "coh_mat_re", coh_mat_re);
//	conversion.write_array_to_h5(egv_out_file.c_str(), "coh_mat_im", coh_mat_im);
//	return 0;*/
//
//	//ComplexMat slc, slc22;
//	//int row = 512 * 53; int col = 512 * 26;
//	//slc.re.create(row, col, CV_16S); slc.im.create(row, col, CV_16S);
//	//slc22.re.create(row, col, CV_16S); slc22.im.create(row, col, CV_16S);
//	////const char* file = "I:\\TSX_sample_data\\SanAndreasFault\\TSX_20121010T141549.977_SanAndreasFault_C177_O129_D_R_SM003_SSC\\TSX1_SAR__SSC______SM_S_SRA_20121010T141549_20121010T141557\\IMAGEDATA\\IMAGE_HH_SRA_strip_003.cos";
//	////const char* file2 = "I:\\TSX_sample_data\\SanAndreasFault\\TSX_20121101T141550.257_SanAndreasFault_C179_O129_D_R_SM003_SSC\\TSX1_SAR__SSC______SM_S_SRA_20121101T141550_20121101T141558\\IMAGEDATA\\IMAGE_HH_SRA_strip_003.cos";
//	////conversion.read_slc_from_TSXcos(file, slc);
//	////conversion.read_slc_from_TSXcos(file2, slc2);
//	//
//	//const char* real_file = "E:\\working_dir\\projects\\software\\InSAR\\bin\\master_real.dat";
//	//const char* imag_file = "E:\\working_dir\\projects\\software\\InSAR\\bin\\master_imag.dat";
//	//const char* real_file2 = "E:\\working_dir\\projects\\software\\InSAR\\bin\\reg_slave_real_0.data";
//	//const char* imag_file2 = "E:\\working_dir\\projects\\software\\InSAR\\bin\\reg_slave_imag_0.data";
//	//FILE* fp = NULL;
//	//fp = fopen(real_file, "rb");
//	//if (!fp) return -1;
//	//fread(slc.re.data, sizeof(short), row * col, fp);
//	//fclose(fp); fp = NULL;
//
//	//fp = fopen(imag_file, "rb");
//	//if (!fp) return -1;
//	//fread(slc.im.data, sizeof(short), row * col, fp);
//	//fclose(fp);
//
//	//fp = fopen(real_file2, "rb");
//	//if (!fp) return -1;
//	//fread(slc22.re.data, sizeof(short), row * col, fp);
//	//fclose(fp); fp = NULL;
//
//	//fp = fopen(imag_file2, "rb");
//	//if (!fp) return -1;
//	//fread(slc22.im.data, sizeof(short), row * col, fp);
//	//fclose(fp);
//	//
//	////conversion.read_slc_from_h5("G:\\Sandreas\\coregis2\\20121010_regis.h5", slc);
//	////conversion.read_slc_from_h5("G:\\Sandreas\\coregis2\\20121101_regis.h5", slc22);
//
//	//slc.convertTo(slc, CV_32F);
//	//slc22.convertTo(slc22, CV_32F);
//
//	//Mat phase11, coh;
//	//util.Multilook(slc, slc22, 1, 1, phase11);
//	//phase11 = phase11(cv::Range(5000, 5999), cv::Range(5000, 5999));
//	//util.phase_coherence(phase11, coh);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\coh2.bin", coh);
//	////util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\phase.jpg", "jet", phase11);
//	////util.saveSLC("E:\\working_dir\\projects\\software\\InSAR\\bin\\slc.jpg", 60, slc);
//	////util.saveSLC("E:\\working_dir\\projects\\software\\InSAR\\bin\\slc2.jpg", 60, slc22);
//
//	//return 0;
//
//	////////////////////////////////////////////SBAS自适应多视干涉相位生成测试////////////////////////////////////////
//	
//	//ComplexMat xx;
//	//vector<ComplexMat> x1, x2;
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\real.bin", xx.re);
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\imag.bin", xx.im);
//	//util.SKP_decomposition(xx, 3, 3, 10, 10, x1, x2);
//	//for (int i = 0; i < 9; i++)
//	//{
//	//	char strings[256];
//	//	sprintf(strings, "E:\\working_dir\\projects\\software\\InSAR\\bin\\x1_real_%d.bin", i + 1);
//	//	util.cvmat2bin(strings, x1[i].re);
//	//	sprintf(strings, "E:\\working_dir\\projects\\software\\InSAR\\bin\\x1_imag_%d.bin", i + 1);
//	//	util.cvmat2bin(strings, x1[i].im);
//	//	sprintf(strings, "E:\\working_dir\\projects\\software\\InSAR\\bin\\x2_real_%d.bin", i + 1);
//	//	util.cvmat2bin(strings, x2[i].re);
//	//	sprintf(strings, "E:\\working_dir\\projects\\software\\InSAR\\bin\\x2_imag_%d.bin", i + 1);
//	//	util.cvmat2bin(strings, x2[i].im);
//	//	//cout << x1[i].re << "\n\n" << x1[i].im << "\n\n\n\n";
//	//	//cout << x2[i].re << "\n\n" << x2[i].im << "\n\n\n\n";
//	//}
//	//return 0;
//
//	//FILE* fp = NULL; char str[256]; memset(str, 0, 256);
//	//char str2[256]; memset(str2, 0, 256);
//	//vector<string> input_files, output_files;
//
//	////for (int i = 0; i < 5; i++)
//	////{
//	////	char str[512];
//	////	sprintf(str, "E:\\working_dir\\papers\\homogeneous_selection\\slc_filtered_%d.h5", i + 1);
//	////	output_files.push_back(str);
//	////	conversion.creat_new_h5(output_files[i].c_str());
//	////}
//
//	//fp = fopen("G:\\beijing\\yanjiao\\file.txt", "rt");
//	////fp = fopen("E:\\working_dir\\papers\\multibaseline_polarimetric\\realistic_data\\beijing\\deramped_vv\\file.txt", "rt");
//	//while (fgets(str, 260, fp))
//	//{
//	//	sscanf(str, "%s", str2);
//	//	input_files.push_back(str2);
//	//}
//	//fclose(fp);
//	//vector<ComplexMat> slc_series, slc_series_filter;
//	//ComplexMat slc;
//	////int offset_rows = 5480 - 1; int offset_cols = 12750 - 1;
//	//for (int i = 0; i < input_files.size(); i++)
//	//{
//	//	conversion.read_slc_from_h5(input_files[i].c_str(), slc);
//	//	//slc = slc(cv::Range(offset_rows, offset_rows + 909), cv::Range(offset_cols, offset_cols + 774));
//	//	slc_series.push_back(slc);
//	//}
//	////util.homogeneous_selection_and_phase_linking(slc_series, slc_series_filter);
//	//
//	////for (int i = 0; i < 5; i++)
//	////{
//	////	
//	////	conversion.write_slc_to_h5(output_files[i].c_str(), slc_series_filter[i]);
//	////}
//	////return 0;
//	//Mat homo_index, homo_num;
//	//util.homogeneous_test(slc_series, 11, 11, homo_num, homo_index, 0.01);
//	//conversion.creat_new_h5("E:\\working_dir\\papers\\ESM coherence matrix\\realdata\\Sentinel\\homo_KS.h5");
//	//conversion.write_array_to_h5("E:\\working_dir\\papers\\ESM coherence matrix\\realdata\\Sentinel\\homo_KS.h5",
//	//	"homo_num", homo_num);
//	//conversion.write_array_to_h5("E:\\working_dir\\papers\\ESM coherence matrix\\realdata\\Sentinel\\homo_KS.h5",
//	//	"homo_index", homo_index);
//	//return 0;
//	
//
//
//
//	//Mat d;
//	//DEM.dem_newton_iter_test("G:/simulation/ifg_AOI5_unwrapped/20111111_regis_cut2_20111122_regis_cut2_phase_unwrapped.h5",
//	//	d, "G:/simulation", 20, 1);
//	//return 0;
//
//
//	//Mat phase, gcps, error;
//
//	//conversion.read_array_from_h5("G:\\simulation\\ifg888\\20111111_regis_20111122_regis.h5", "phase", phase);
//	//conversion.read_array_from_h5("G:\\tmp\\master.h5", "gcps", gcps);
//	//error.create(gcps.rows, 1, CV_64F);
//	//for (int i = 0; i < gcps.rows; i++)
//	//{
//	//	int row = (int)gcps.at<double>(i, 0) - 1;
//	//	int col = (int)gcps.at<double>(i, 1) - 1;
//	//	double delta_r = gcps.at<double>(i, 5) - gcps.at<double>(i, 6);
//	//	error.at<double>(i, 0) = phase.at<double>(row, col) + 4 * PI * delta_r / (VEL_C / 9.649999315E9);
//	//}
//	//util.wrap(error, error);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\error.bin", error);
//	//return 0;
//	//conversion.read_slc_from_h5("G:\\tmp\\slave.h5", slc);
//
//
//	//////DEM投影
//	//double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing, rangeSpacing2, azimuth_spacing, azimuth_spacing2,
//	//	nearRangeTime, nearRangeTime2, wavelength, prf, start, end, start2, end2;
//	//int sceneHeight, sceneWidth, sceneHeight2, sceneWidth2, offset_row = 0, offset_col = 0, ret;
//	//Mat lon_coef, lat_coef, dem, mappedDem, statevec, statevec2, GCPs1, GCPs2, mappedLat, mappedLon, R1, R2;
//	//ComplexMat slc1, slc2, slc3, slc4;
//	//string start_time, end_time, master_file;
//	//string demPath = "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem";
//	//master_file = "G:\\new_simulation\\tsx\\NorthAnatolia\\coregis\\20111111_regis.h5";
//	//string slave_file = "G:\\new_simulation\\tsx\\NorthAnatolia\\coregis\\20111122_regis.h5";
//	//ret = conversion.read_int_from_h5(master_file.c_str(), "range_len", &sceneWidth);
//	//ret = conversion.read_int_from_h5(master_file.c_str(), "azimuth_len", &sceneHeight);
//	//ret = conversion.read_int_from_h5(slave_file.c_str(), "range_len", &sceneWidth2);
//	//ret = conversion.read_int_from_h5(slave_file.c_str(), "azimuth_len", &sceneHeight2);
//	//ret = conversion.read_int_from_h5(master_file.c_str(), "offset_row", &offset_row);
//	//ret = conversion.read_int_from_h5(master_file.c_str(), "offset_col", &offset_col);
//	//ret = conversion.read_array_from_h5(master_file.c_str(), "lon_coefficient", lon_coef);
//	//ret = conversion.read_array_from_h5(master_file.c_str(), "lat_coefficient", lat_coef);
//	//ret = conversion.read_double_from_h5(master_file.c_str(), "prf", &prf);
//	//ret = conversion.read_double_from_h5(master_file.c_str(), "carrier_frequency", &wavelength);
//	//double wavelength2 = VEL_C / 9.4e9;
//	////Mat w(1, 1, CV_64F); w.at<double>(0, 0) = wavelength;
//	////conversion.write_subarray_to_h5("G:\\simulation\\coregis_low3\\20111122_regis.h5", "carrier_frequency", w, 0, 0, 1, 1); 
//	////return 0;
//	//wavelength = VEL_C / wavelength;
//	////ret = conversion.read_double_from_h5(slave_file.c_str(), "range_spacing", &rangeSpacing2);
//	////ret = conversion.read_double_from_h5(slave_file.c_str(), "azimuth_spacing", &azimuth_spacing2);
//	//ret = conversion.read_double_from_h5(master_file.c_str(), "range_spacing", &rangeSpacing);
//	//ret = conversion.read_double_from_h5(master_file.c_str(), "azimuth_spacing", &azimuth_spacing);
//	//ret = conversion.read_double_from_h5(slave_file.c_str(), "slant_range_first_pixel", &nearRangeTime2);
//	//ret = conversion.read_double_from_h5(master_file.c_str(), "slant_range_first_pixel", &nearRangeTime);
//	////nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	////nearRangeTime2 = 2.0 * nearRangeTime2 / VEL_C;
//	//ret = conversion.read_str_from_h5(slave_file.c_str(), "acquisition_start_time", start_time);
//	//ret = conversion.utc2gps(start_time.c_str(), &start2);
//	//ret = conversion.read_str_from_h5(slave_file.c_str(), "acquisition_stop_time", end_time);
//	//ret = conversion.utc2gps(end_time.c_str(), &end2);
//	//ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_start_time", start_time);
//	//ret = conversion.utc2gps(start_time.c_str(), &start);
//	//ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_stop_time", end_time);
//	//ret = conversion.utc2gps(end_time.c_str(), &end);
//	//ret = conversion.read_array_from_h5(master_file.c_str(), "state_vec", statevec);
//	//ret = conversion.read_array_from_h5(slave_file.c_str(), "state_vec", statevec2);
//	//ret = Utils::computeImageGeoBoundry(lat_coef, lon_coef, sceneHeight, sceneWidth, offset_row, offset_col,
//	//	&lonMax, &latMax, &lonMin, &latMin);
//	//ret = Utils::getSRTMDEM(demPath.c_str(), dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//	////ret = flat.demMapping(dem, mappedDem, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
//	////	prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 20);
//	////conversion.creat_new_h5("G:\\tmp\\dem.h5");
//	////conversion.write_array_to_h5("G:\\tmp\\dem.h5", "dem", mappedDem);
//	//// return 0;
//	////simulator.generateSLC(statevec, dem, lon_upperleft, lat_upperleft, sceneHeight, sceneWidth,
//	//	//nearRangeTime, prf, wavelength, rangeSpacing, azimuth_spacing, start, end, 5, slc1, GCPs1);
//	////conversion.write_array_to_h5("G:\\tmp\\dual_phase.h5", "GCP", GCPs1);
//	////return 0;
//	////simulator.generateSlantrange(statevec, statevec2, dem, lon_upperleft, lat_upperleft, sceneHeight, sceneWidth, nearRangeTime,
//	////	prf, wavelength, rangeSpacing, azimuth_spacing, start, end, start2, end2, R1, R2);
//	////return 0;
//	////conversion.creat_new_h5("G:\\tmp\\R.h5");
//	////conversion.write_array_to_h5("G:\\tmp\\R.h5", "R1", R1);
//	////conversion.write_array_to_h5("G:\\tmp\\R.h5", "R2", R2);
//	////return 0;
//	////ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
//	////	prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 20);
//
//
//	//simulator.generateSLC(statevec, statevec2, dem, lon_upperleft, lat_upperleft, sceneHeight, sceneWidth, sceneHeight2, sceneWidth2,
//	//	nearRangeTime, nearRangeTime2, prf, wavelength, rangeSpacing, azimuth_spacing, start, end, start2, end2, 5, slc1, slc2, slc3,
//	//	slc4, GCPs1, GCPs2);
//
//	//conversion.creat_new_h5("G:\\new_simulation\\master_master_high.h5");
//	//conversion.write_slc_to_h5("G:\\new_simulation\\master_master_high.h5", slc1);
//	//conversion.write_array_to_h5("G:\\new_simulation\\master_master_high.h5", "gcps", GCPs1);
//
//	//conversion.creat_new_h5("G:\\new_simulation\\master_slave_high.h5");
//	//conversion.write_slc_to_h5("G:\\new_simulation\\master_slave_high.h5", slc2);
//
//	//conversion.creat_new_h5("G:\\new_simulation\\slave_slave_high.h5");
//	//conversion.write_slc_to_h5("G:\\new_simulation\\slave_slave_high.h5", slc3);
//	//conversion.write_array_to_h5("G:\\new_simulation\\slave_slave_high.h5", "gcps", GCPs2);
//
//	//conversion.creat_new_h5("G:\\new_simulation\\slave_master_high.h5");
//	//conversion.write_slc_to_h5("G:\\new_simulation\\slave_master_high.h5", slc4);
//
//	////simulator.generateSLC(statevec, statevec2, dem, lon_upperleft, lat_upperleft, sceneHeight, sceneWidth, sceneHeight2, sceneWidth2,
//	////	nearRangeTime, nearRangeTime2, prf, wavelength2, rangeSpacing, azimuth_spacing, start, end, start2, end2, 5, slc1, slc2, slc3,
//	////	slc4, GCPs1, GCPs2);
//
//	////conversion.creat_new_h5("G:\\new_simulation\\slc1_low.h5");
//	////conversion.write_slc_to_h5("G:\\new_simulation\\slc1_low.h5", slc1);
//	////conversion.write_array_to_h5("G:\\new_simulation\\slc1_low.h5", "gcps", GCPs1);
//
//	////conversion.creat_new_h5("G:\\new_simulation\\slc2_low.h5");
//	////conversion.write_slc_to_h5("G:\\new_simulation\\slc2_low.h5", slc2);
//
//	////conversion.creat_new_h5("G:\\new_simulation\\slc3_low.h5");
//	////conversion.write_slc_to_h5("G:\\new_simulation\\slc3_low.h5", slc3);
//	////conversion.write_array_to_h5("G:\\new_simulation\\slc3_low.h5", "gcps", GCPs2);
//
//	////conversion.creat_new_h5("G:\\new_simulation\\slc4_low.h5");
//	////conversion.write_slc_to_h5("G:\\new_simulation\\slc4_low.h5", slc4);
//
//	////conversion.write_double_to_h5("G:\\new_simulation\\slc1_low.h5", "carrier_frequency", 9.4e9);
//	////conversion.write_double_to_h5("G:\\new_simulation\\slc2_low.h5", "carrier_frequency", 9.4e9);
//	////conversion.write_double_to_h5("G:\\new_simulation\\slc3_low.h5", "carrier_frequency", 9.4e9);
//	////conversion.write_double_to_h5("G:\\new_simulation\\slc4_low.h5", "carrier_frequency", 9.4e9);
//
//	//return 0;
//
//	//const char* slc_file1 = "G:\\tmp\\slc1.h5";
//	//const char* slc_file4 = "G:\\tmp\\slc4.h5";
//	//const char* slc_file2 = "G:\\simulation\\coregis_high2\\20111122_regis.h5";
//	//const char* slc_file3 = "G:\\simulation\\coregis_high3\\20111122_regis.h5";
//	//const char* slc_file5 = "G:\\tmp\\slc_low1.h5";
//	//const char* slc_file8 = "G:\\tmp\\slc_low4.h5";
//	//const char* slc_file6 = "G:\\simulation\\coregis_low2\\20111122_regis.h5";
//	//const char* slc_file7 = "G:\\simulation\\coregis_low3\\20111122_regis.h5";
//
//
//	//const char* slc_file1_out = "G:\\tmp\\slc1_out.h5";
//	//const char* slc_file2_out = "G:\\tmp\\slc2_out.h5";
//	//const char* slc_file3_out = "G:\\tmp\\slc3_out.h5";
//	//const char* slc_file4_out = "G:\\tmp\\slc4_out.h5";
//	//const char* slc_file5_out = "G:\\tmp\\slc_low1_out.h5";
//	//const char* slc_file6_out = "G:\\tmp\\slc_low2_out.h5";
//	//const char* slc_file7_out = "G:\\tmp\\slc_low3_out.h5";
//	//const char* slc_file8_out = "G:\\tmp\\slc_low4_out.h5";
//
//
//	//const char* slc_file1_out_filter = "G:\\tmp\\slc1_out_filter.h5";
//	//const char* slc_file2_out_filter = "G:\\tmp\\slc2_out_filter.h5";
//	//const char* slc_file3_out_filter = "G:\\tmp\\slc3_out_filter.h5";
//	//const char* slc_file4_out_filter = "G:\\tmp\\slc4_out_filter.h5";
//	//const char* slc_file5_out_filter = "G:\\tmp\\slc1_out_filter2.h5";
//	//const char* slc_file6_out_filter = "G:\\tmp\\slc2_out_filter2.h5";
//	//const char* slc_file7_out_filter = "G:\\tmp\\slc3_out_filter2.h5";
//	//const char* slc_file8_out_filter = "G:\\tmp\\slc4_out_filter2.h5";
//
//	//const char* slc_file1_reramp = "G:\\tmp\\slc1_reramp.h5";
//	//const char* slc_file2_reramp = "G:\\tmp\\slc2_reramp.h5";
//	//const char* slc_file3_reramp = "G:\\tmp\\slc3_reramp.h5";
//	//const char* slc_file4_reramp = "G:\\tmp\\slc4_reramp.h5";
//	//const char* slc_file5_reramp = "G:\\tmp\\slc1_reramp_low.h5";
//	//const char* slc_file6_reramp = "G:\\tmp\\slc2_reramp_low.h5";
//	//const char* slc_file7_reramp = "G:\\tmp\\slc3_reramp_low.h5";
//	//const char* slc_file8_reramp = "G:\\tmp\\slc4_reramp_low.h5";
//
//	//const char* slc_file1_out2 = "G:\\tmp\\slc1_out2.h5";
//	//const char* slc_file2_out2 = "G:\\tmp\\slc2_out2.h5";
//	//const char* slc_file3_out2 = "G:\\tmp\\slc3_out2.h5";
//	//const char* slc_file4_out2 = "G:\\tmp\\slc4_out2.h5";
//
//	//const char* slc_file1_reramp2 = "G:\\tmp\\slc1_reramp3.h5";
//	//const char* slc_file2_reramp2 = "G:\\tmp\\slc2_reramp3.h5";
//	//const char* slc_file3_reramp2 = "G:\\tmp\\slc3_reramp3.h5";
//	//const char* slc_file4_reramp2 = "G:\\tmp\\slc4_reramp3.h5";
//
//	//Mat phase_ref, wrapped_phase_low, wrapped_phase_high, outphase;
//	//conversion.read_array_from_h5("G:\\tmp\\unwrapped_phase.h5", "phase", phase_ref);
//
//	////conversion.read_array_from_h5("G:/simulation/ifg_AOI6/20111111_regis_deramp_cut2_20111122_regis_deramp_cut2.h5", "phase", outphase);
//	////util.phase_coherence(outphase, 11, 11, wrapped_phase_high);
//	////conversion.creat_new_h5("G:\\tmp\\coh.h5");
//	////conversion.write_array_to_h5("G:\\tmp\\coh.h5", "coherence", wrapped_phase_high);
//	////return 0;
//
//	//ret = conversion.read_int_from_h5("G:/simulation/AOI5/20111111_regis_cut2.h5", "offset_row", &offset_row);
//	//ret = conversion.read_int_from_h5("G:/simulation/AOI5/20111111_regis_cut2.h5", "offset_col", &offset_col);
//
//	//conversion.read_slc_from_h5(slc_file1_reramp, slc1);
//	//slc1 = slc1(cv::Range(offset_row, offset_row + phase_ref.rows), cv::Range(offset_col, offset_col + phase_ref.cols));
//	//conversion.read_slc_from_h5(slc_file2_reramp, slc2);
//	//slc2 = slc2(cv::Range(offset_row, offset_row + phase_ref.rows), cv::Range(offset_col, offset_col + phase_ref.cols));
//
//	//util.Multilook(slc1, slc2, 1, 1, wrapped_phase_high);
//	//util.savephase("G:\\tmp\\test.jpg", "jet", wrapped_phase_high);
//	//return 0;
//	////conversion.read_slc_from_h5(slc_file5_reramp, slc1);
//	////slc1 = slc1(cv::Range(offset_row, offset_row + phase_ref.rows), cv::Range(offset_col, offset_col + phase_ref.cols));
//	////conversion.read_slc_from_h5(slc_file7_reramp, slc2);
//	////slc2 = slc2(cv::Range(offset_row, offset_row + phase_ref.rows), cv::Range(offset_col, offset_col + phase_ref.cols));
//
//	////util.Multilook(slc1, slc2, 1, 1, wrapped_phase_high);
//	////util.savephase("G:\\tmp\\wrapped_phase_low.jpg", "jet", wrapped_phase_high);
//	////return 0;
//	////util.wrap(phase_ref, phase_ref);
//	////wrapped_phase_high -= phase_ref;
//	////util.wrap(wrapped_phase_high, wrapped_phase_high);
//	////util.cvmat2bin("G:\\tmp\\wrapped_phase_high.bin", wrapped_phase_high); return 0;
//
//	////conversion.read_array_from_h5("G:\\tmp\\R.h5", "R1", R1);
//	////conversion.read_array_from_h5("G:\\tmp\\R.h5", "R2", R2);
//
//	////for (int i = 0; i <  phase_ref.rows; i++)
//	////{
//	////	for (int j = 0; j < phase_ref.cols; j++)
//	////	{
//	////		phase_ref.at<double>(i, j) = -4.0 * PI * (R1.at<double>(i + offset_row, j + offset_col) -
//	////			R2.at<double>(i + offset_row, j + offset_col)) / wavelength;
//	////	}
//	////}
//
//	////simulator.pingpong_MLE(phase_ref, wrapped_phase_low, wrapped_phase_high, outphase, lat_coef, lon_coef,  VEL_C / 9.4e9, wavelength,
//	////	nearRangeTime, rangeSpacing, azimuth_spacing, offset_row, offset_col, start, end, start2, end2, statevec, statevec2, prf,
//	////	demPath.c_str()); 
//	////conversion.creat_new_h5("G:\\tmp\\outphase.h5");
//	////conversion.write_array_to_h5("G:\\tmp\\outphase.h5", "phase", outphase);
//	////return 0;
//	////simulator.MB_phase_estimation(21, slc_file5_out, slc_file6_out, slc_file7_out, slc_file8_out,
//	////	slc_file5_out_filter, slc_file6_out_filter, slc_file7_out_filter, slc_file8_out_filter);
//	////return 0;
//
//	////simulator.MB_phase_estimation(21, slc_file1_out, slc_file2_out, slc_file3_out, slc_file4_out, slc_file5_out, slc_file6_out,
//	////	slc_file7_out, slc_file8_out,
//	////	slc_file1_out_filter, slc_file2_out_filter, slc_file3_out_filter, slc_file4_out_filter,
//	////	slc_file5_out_filter, slc_file6_out_filter, slc_file7_out_filter, slc_file8_out_filter);
//	////return 0;
//	////simulator.SLC_reramp(mappedDem, mappedLat, mappedLon, slc_file5_out_filter, slc_file6_out_filter,
//	////	slc_file7_out_filter, slc_file8_out_filter, slc_file5_reramp, slc_file6_reramp, slc_file7_reramp, slc_file8_reramp);
//	////return 0;
//	////simulator.SLC_reramp(mappedDem, mappedLat, mappedLon, slc_file5_out_filter, slc_file6_out_filter,
//	////	slc_file7_out_filter, slc_file8_out_filter, slc_file5_reramp, slc_file6_reramp, slc_file7_reramp, slc_file8_reramp);
//
//	////simulator.SLC_reramp(mappedDem, mappedLat, mappedLon, slc_file1_out2, slc_file2_out2,
//	////	slc_file3_out2, slc_file4_out2, slc_file1_reramp2, slc_file2_reramp2, slc_file3_reramp2, slc_file4_reramp2);
//	////return 0;
//	////simulator.SLC_reramp(mappedDem, mappedLat, mappedLon, slc_file1_out_filter, slc_file2_out_filter,
//	////	slc_file3_out_filter, slc_file4_out_filter, slc_file1_reramp, slc_file2_reramp, slc_file3_reramp, slc_file4_reramp);
//
//	////ret = simulator.SLC_deramp(mappedDem, mappedLat, mappedLon, slc_file1, slc_file2, slc_file3, slc_file4,
//	////	slc_file1_out, slc_file2_out, slc_file3_out, slc_file4_out);
//	////return 0;
//
//	////simulator.MB_phase_estimation(21, slc_file1_out, slc_file2_out, slc_file3_out, slc_file4_out,
//	////	slc_file1_out_filter, slc_file2_out_filter, slc_file3_out_filter, slc_file4_out_filter);
//	////return 0;
//	////simulator.SLC_reramp(mappedDem, mappedLat, mappedLon, slc_file1_out_filter, slc_file2_out_filter,
//	////	slc_file3_out_filter, slc_file4_out_filter, slc_file1_reramp, slc_file2_reramp, slc_file3_reramp, slc_file4_reramp);
//
//	//Mat phase = wrapped_phase_high;
//	//conversion.read_array_from_h5("G:\\tmp\\outphase.h5", "phase", phase);
//	//util.savephase("G:\\tmp\\pingpong_dual_phase.jpg", "jet", phase); return 0;
//	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R1", R1);
//	//conversion.read_array_from_h5("G:\\tmp\\R.h5", "R2", R2);
//	//R1(cv::Range(offset_row, offset_row + phase.rows), cv::Range(offset_col, offset_col + phase.cols)).copyTo(R1);
//	//R2(cv::Range(offset_row, offset_row + phase.rows), cv::Range(offset_col, offset_col + phase.cols)).copyTo(R2);
//	//Mat error, gcps;
//	////ComplexMat slc1, slc2;
//	//
//	////error.create(gcps.rows, 1, CV_64F);
//
//	////conversion.read_slc_from_h5(slc_file1_reramp, slc1);
//	////slc1 = slc1(cv::Range(1001, 2000), cv::Range(1001, 2000));
//	////conversion.read_slc_from_h5(slc_file3_reramp, slc2);
//	////slc2 = slc2(cv::Range(1001, 2000), cv::Range(1001, 2000));
//	////slc1.convertTo(slc1, CV_64F);
//	////slc2.convertTo(slc2, CV_64F);
//	////util.complex_coherence(slc1, slc2, 21, 21, phase);
//	////util.cvmat2bin("G:\\tmp\\coherence.bin", phase);
//	////return 0;
//	////util.Multilook(slc1, slc2, 1, 1, phase);
//	////util.savephase("G:\\tmp\\phase_same2.jpg", "jet", phase); return 0;
//	////util.multilook(slc1, slc2, 21, 21, phase);
//	////filter.Goldstein_filter(phase, phase, 0.7, 128, 16);
//	///*phase.convertTo(phase, CV_32F);
//	//phase = 0.5 * phase;
//	//util.phase2cos(phase, slc1.re, slc1.im);
//	//conversion.creat_new_h5(slc_file1_out2);
//	//conversion.write_slc_to_h5(slc_file1_out2, slc1);
//	//phase = -1.0 * phase;
//	//util.phase2cos(phase, slc1.re, slc1.im);
//	//conversion.creat_new_h5(slc_file3_out2);
//	//conversion.write_slc_to_h5(slc_file3_out2, slc1);
//
//	//conversion.creat_new_h5(slc_file2_out2);
//	//conversion.write_slc_to_h5(slc_file2_out2, slc1);
//
//	//conversion.creat_new_h5(slc_file4_out2);
//	//conversion.write_slc_to_h5(slc_file4_out2, slc1); return 0;*/
//
//	////util.savephase("G:\\tmp\\phase_goldstein.jpg", "jet", phase); return 0;
//	//int count = 0, count2;
//	//for (int i = 0; i < R1.rows; i++)
//	//{
//	//	for (int j = 0; j < R1.cols; j++)
//	//	{
//	//		if (R1.at<double>(i, j) > 10.0) count++;
//	//	}
//	//}
//	//count2 = count;
//	//error.create(count2, 1, CV_64F); error = 0.0;
//	//count = 0;
//	//for (int i = 0; i < phase.rows; i++)
//	//{
//	//	for (int j = 0; j < phase.cols; j++)
//	//	{
//	//		if (R1.at<double>(i, j) > 10.0)
//	//		{
//	//			double delta_r = R1.at<double>(i, j) - R2.at<double>(i, j);
//	//			error.at<double>(count++, 0) = phase.at<double>(i, j) + 4 * PI * delta_r / (VEL_C / /*9.4E9*/ 9.649999315E9);
//	//		}
//	//	}
//	//}
//
//	////for (int i = 0; i < gcps.rows; i++)
//	////{
//	////	int row = (int)gcps.at<double>(i, 0) - 1;
//	////	int col = (int)gcps.at<double>(i, 1) - 1;
//	////	double delta_r = gcps.at<double>(i, 5) - gcps.at<double>(i, 6);
//	////	error.at<double>(i, 0) = phase.at<double>(row, col) + 4 * PI * delta_r / (VEL_C / 9.649999315E9);
//	////}
//	//util.wrap(error, error);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\error_dual_pingpong.bin", error);
//	//return 0;
//
//
//	////conversion.read_slc_from_h5(slc_file1, slc1);
//	////slc1 = slc1(cv::Range(1001, 6000), cv::Range(1001, 6000));
//	////conversion.read_slc_from_h5(slc_file3, slc2);
//	////slc2 = slc2(cv::Range(1001, 6000), cv::Range(1001, 6000));
//	////util.Multilook(slc1, slc2, 1, 1, phase);
//	////slc1.release(); slc2.release();
//	////util.savephase("G:\\tmp\\phase1.jpg", "jet", phase);
//	////filter.Goldstein_filter(phase, phase, 0.7, 128, 16);
//	////count = 0;
//	////for (int i = 0; i < phase.rows; i++)
//	////{
//	////	for (int j = 0; j < phase.cols; j++)
//	////	{
//	////		if (R1.at<double>(i, j) > 10.0)
//	////		{
//	////			double delta_r = R1.at<double>(i, j) - R2.at<double>(i, j);
//	////			error.at<double>(count++, 0) = phase.at<double>(i, j) + 4 * PI * delta_r / (VEL_C / 9.649999315E9);
//	////		}
//	////	}
//	////}
//
//	///*for (int i = 0; i < gcps.rows; i++)
//	//{
//	//	int row = (int)gcps.at<double>(i, 0) - 1;
//	//	int col = (int)gcps.at<double>(i, 1) - 1;
//	//	double delta_r = gcps.at<double>(i, 5) - gcps.at<double>(i, 6);
//	//	error.at<double>(i, 0) = phase.at<double>(row, col) + 4 * PI * delta_r / (VEL_C / 9.649999315E9);
//	//}*/
//	//util.wrap(error, error);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\error3.bin", error);
//	////util.savephase("G:\\tmp\\phase3.jpg", "jet", phase);
//
//	//
//
//	////conversion.creat_new_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\simulated_slc3.h5");
//	////conversion.write_slc_to_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\simulated_slc3.h5", slc3);
//
//	////conversion.creat_new_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\simulated_slc4.h5");
//	////conversion.write_slc_to_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\simulated_slc4.h5", slc4);
//
//	////simulator.generateSLC(statevec, dem, lon_upperleft, lat_upperleft, sceneHeight, sceneWidth, nearRangeTime, prf, wavelength,
//	////	rangeSpacing, azimuth_spacing, start, end, 5, slc, GCPs1);
//	////conversion.creat_new_h5("G:\\tmp\\master.h5");
//	////conversion.write_slc_to_h5("G:\\tmp\\master.h5", slc);
//	////conversion.write_array_to_h5("G:\\tmp\\master.h5", "gcps", GCPs1);
//	////util.saveSLC("E:\\working_dir\\projects\\software\\InSAR\\bin\\slc3.jpg", 65, slc);
//
//	////simulator.generateSLC(statevec2, dem, lon_upperleft, lat_upperleft, sceneHeight2, sceneWidth2, nearRangeTime2, prf, wavelength,
//	////	rangeSpacing2, azimuth_spacing2, start2, end2, 5, slc, GCPs2);
//	////conversion.creat_new_h5("G:\\tmp\\slave.h5");
//	////conversion.write_slc_to_h5("G:\\tmp\\slave.h5", slc);
//	////util.saveSLC("E:\\working_dir\\projects\\software\\InSAR\\bin\\slc4.jpg", 65, slc);
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\gcps.bin", GCPs1);
//	////conversion.read_slc_from_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\simulated_slc2.h5", slc);
//	////ComplexMat x;
//	////x = slc(cv::Range(7700, 9528), cv::Range(3478, 5000));
//	////util.saveSLC("E:\\working_dir\\projects\\software\\InSAR\\bin\\slc.jpg", 65, x);
//	//
//	////Mat mappedLon, mappedLat;
//	////ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
//	////	prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 10);
//	////conversion.creat_new_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\shanxi_dem.h5");
//	////conversion.write_array_to_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\shanxi_dem.h5", "dem", mappedDem);
//	////mappedDem.convertTo(mappedDem, CV_64F);
//	////util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\shanxi_dem.jpg", "jet", mappedDem);
//	////cin.get();
//	return 0;
//}


//int main(int argc, char* argv[])
//{
//	double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing, rangeSpacing2,
//		nearRangeTime, nearRangeTime2, wavelength, prf, prf2,
//		start, end, start2, end2, a0, a1, a2, b0, b1, b2;
//	int sceneHeight, sceneWidth, sceneHeight2, sceneWidth2, offset_row, offset_col, offset_row2, offset_col2;
//	Mat lon_coef, lat_coef, dem, statevec, rangePos, azimuthPos,
//		lon_coef2, lat_coef2, statevec2, rangePos2, azimuthPos2, slaveRangeOffset, slaveAzimuthOffset;
//	string start_time, end_time;
//	ComplexMat master, slave;
//	FormatConversion conversion; Registration coregis;
//	offset_row = offset_col = 0;
//	const char* master_file = "D:\\working_dir\\others\\sentinel\\slc_deramp\\20180103_iw1vv_regis_deramp.h5";
//	const char* slave_file = "D:\\working_dir\\others\\test\\import\\20120306.h5";
//	//const char* master_file = "D:\\working_dir\\others\\test\\import\\20120213_cut.h5";
//	//const char* slave_file = "D:\\working_dir\\others\\test\\import\\20120306_cut.h5";
//	//conversion.read_slc_from_h5(master_file, master);
//	//conversion.read_slc_from_h5(slave_file, slave);
//	//master = master(cv::Range(0, 5000), cv::Range(0, 5000));
//	//slave = slave(cv::Range(0, 5000), cv::Range(0, 5000));
//	//conversion.creat_new_h5(master_file_new);
//	//conversion.creat_new_h5(slave_file_new);
//
//	
//
//
//
//
//
//	conversion.read_int_from_h5(master_file, "range_len", &sceneWidth);
//	conversion.read_int_from_h5(master_file, "azimuth_len", &sceneHeight);
//	conversion.read_array_from_h5(master_file, "lon_coefficient", lon_coef);
//	conversion.read_array_from_h5(master_file, "lat_coefficient", lat_coef);
//	conversion.read_double_from_h5(master_file, "prf", &prf);
//	conversion.read_double_from_h5(master_file, "carrier_frequency", &wavelength);
//	wavelength = VEL_C / wavelength;
//	conversion.read_double_from_h5(master_file, "range_spacing", &rangeSpacing);
//	conversion.read_double_from_h5(master_file, "slant_range_first_pixel", &nearRangeTime);
//	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	conversion.read_str_from_h5(master_file, "acquisition_start_time", start_time);
//	conversion.utc2gps(start_time.c_str(), &start);
//	conversion.read_str_from_h5(master_file, "acquisition_stop_time", end_time);
//	conversion.utc2gps(end_time.c_str(), &end);
//	conversion.read_array_from_h5(master_file, "state_vec", statevec);
//	conversion.read_slc_from_h5(master_file, master);
//
//	
//
//	//conversion.write_slc_to_h5(master_file_new, master);
//	//conversion.write_int_to_h5(master_file_new, "range_len", 5000);
//	//conversion.write_int_to_h5(master_file_new, "azimuth_len", 5000);
//	//conversion.write_array_to_h5(master_file_new, "lon_coefficient", lon_coef);
//	//conversion.write_array_to_h5(master_file_new, "lat_coefficient", lat_coef);
//	//conversion.write_double_to_h5(master_file_new, "prf", prf);
//	//conversion.write_double_to_h5(master_file_new, "carrier_frequency", wavelength);
//	//conversion.write_double_to_h5(master_file_new, "range_spacing", rangeSpacing);
//	//conversion.write_double_to_h5(master_file_new, "slant_range_first_pixel", nearRangeTime);
//	//conversion.write_str_to_h5(master_file_new, "acquisition_start_time", start_time.c_str());
//	//conversion.write_str_to_h5(master_file_new, "acquisition_stop_time", end_time.c_str());
//	//conversion.write_array_to_h5(master_file_new, "state_vec", statevec);
//
//	Utils::computeImageGeoBoundry(lat_coef, lon_coef, sceneHeight, sceneWidth, offset_row, offset_col,
//		&lonMax, &latMax, &lonMin, &latMin);
//	Utils::getSRTMDEM("D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//
//	Utils util;
//	ComplexMat master_mapped;
//	util.geocode(dem, master, 20, 20, master_mapped, lon_upperleft, lat_upperleft, 0, 0, sceneHeight, sceneWidth, 
//		prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 5.0 / 6000.0, 5.0 / 6000.0);
//
//	util.saveSLC("D:\\working_dir\\others\\sentinel\\slc_deramp\\slc_geocoded.jpg", 65, master_mapped);
//	return 0;
//
//	Deflat flat;
//	Mat mappedLon, mappedLat, mappedDem;
//	int ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
//		prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 20);
//	conversion.creat_new_h5("D:\\working_dir\\others\\test\\import\\mapped.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\others\\test\\import\\mapped.h5", "mappedDEM", mappedDem);
//	return 0;
//	coregis.getDEMRgAzPos(dem, statevec, rangePos, azimuthPos, lon_upperleft, lat_upperleft, offset_row, offset_col,
//		sceneHeight, sceneWidth, prf, rangeSpacing, wavelength, nearRangeTime, start, end, 5.0 / 6000.0, 5.0 / 6000.0);
//	
//
//	int offset_r, offset_c;
//	offset_row2 = offset_col2 = 0;
//	conversion.read_int_from_h5(slave_file, "range_len", &sceneWidth2);
//	conversion.read_int_from_h5(slave_file, "azimuth_len", &sceneHeight2);
//	conversion.read_array_from_h5(slave_file, "lon_coefficient", lon_coef2);
//	conversion.read_array_from_h5(slave_file, "lat_coefficient", lat_coef2);
//	conversion.read_double_from_h5(slave_file, "prf", &prf2);
//	conversion.read_double_from_h5(slave_file, "range_spacing", &rangeSpacing2);
//	conversion.read_double_from_h5(slave_file, "slant_range_first_pixel", &nearRangeTime2);
//	nearRangeTime2 = 2.0 * nearRangeTime2 / VEL_C;
//	conversion.read_str_from_h5(slave_file, "acquisition_start_time", start_time);
//	conversion.utc2gps(start_time.c_str(), &start2);
//	conversion.read_str_from_h5(slave_file, "acquisition_stop_time", end_time);
//	conversion.utc2gps(end_time.c_str(), &end2);
//	conversion.read_array_from_h5(slave_file, "state_vec", statevec2);
//	conversion.read_slc_from_h5(slave_file, slave);
//
//	//conversion.write_slc_to_h5(slave_file_new, slave);
//	//conversion.write_int_to_h5(slave_file_new, "range_len", 5000);
//	//conversion.write_int_to_h5(slave_file_new, "azimuth_len", 5000);
//	//conversion.write_array_to_h5(slave_file_new, "lon_coefficient", lon_coef2);
//	//conversion.write_array_to_h5(slave_file_new, "lat_coefficient", lat_coef2);
//	//conversion.write_double_to_h5(slave_file_new, "prf", prf2);
//	//conversion.write_double_to_h5(slave_file_new, "carrier_frequency", wavelength);
//	//conversion.write_double_to_h5(slave_file_new, "range_spacing", rangeSpacing2);
//	//conversion.write_double_to_h5(slave_file_new, "slant_range_first_pixel", nearRangeTime2);
//	//conversion.write_str_to_h5(slave_file_new, "acquisition_start_time", start_time.c_str());
//	//conversion.write_str_to_h5(slave_file_new, "acquisition_stop_time", end_time.c_str());
//	//conversion.write_array_to_h5(slave_file_new, "state_vec", statevec2);
//
//	coregis.getDEMRgAzPos(dem, statevec2, rangePos2, azimuthPos2, lon_upperleft, lat_upperleft, offset_row2, offset_col2,
//		sceneHeight2, sceneWidth2, prf2, rangeSpacing2, wavelength, nearRangeTime2, start2, end2, 5.0 / 6000.0, 5.0 / 6000.0);
//
//	coregis.computeSlaveOffset(rangePos, azimuthPos, rangePos2, azimuthPos2, slaveAzimuthOffset, slaveRangeOffset);
//	coregis.fitSlaveOffset(slaveAzimuthOffset, rangePos, azimuthPos, &a0, &a1, &a2);
//	coregis.fitSlaveOffset(slaveRangeOffset, rangePos, azimuthPos, &b0, &b1, &b2);
//	coregis.performBilinearResampling(slave, sceneHeight, sceneWidth, b0, b1, b2, a0, a1, a2, &offset_r, &offset_c);
//
//	master.convertTo(master, CV_64F);
//	slave.convertTo(slave, CV_64F);
//
//	Mat phase, phase_filter;
//	//Utils util;
//	util.Multilook(master, slave, 4, 4, phase);
//	util.savephase("D:\\working_dir\\others\\test\\import\\phase.jpg", "jet", phase);
//	Filter filter;
//	filter.Goldstein_filter(phase, phase_filter, 0.85, 256, 16);
//	util.savephase("D:\\working_dir\\others\\test\\import\\phase_filter.jpg", "jet", phase_filter);
//	return 0;
//}

int main(int argc, char* argv[])
{
	// removed unused: rangeSpacing2, nearRangeTime2, prf2, start2, end2, a0, a1, a2, b0, b1, b2, sceneHeight2, sceneWidth2, offset_row2, offset_col2 (all references in commented-out code)
	double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing,
		nearRangeTime, wavelength, prf,
		start, end;
	int sceneHeight, sceneWidth, offset_row, offset_col;
	Mat lon_coef, lat_coef, dem, statevec, rangePos, azimuthPos,
		lon_coef2, lat_coef2, statevec2, rangePos2, azimuthPos2, slaveRangeOffset, slaveAzimuthOffset;
	string start_time, end_time;
	ComplexMat master, slave;
	FormatConversion conversion; Registration coregis;
	offset_row = offset_col = 0;
	lonMax = 102.92222339 + 0.01; lonMin = 102.90357707 - 0.01;
	latMax = 29.93391223 +0.01; latMin = 29.91459173 - 0.01;

	Utils::getSRTMDEM("D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
	//dem = 1142;
	Utils util;
	ComplexMat master_mapped;

	sceneHeight = 13112;
	sceneWidth = 12800;
	wavelength = VEL_C / 35e9;
	rangeSpacing = 0.124913521111012;
	prf = 500.0;
	nearRangeTime = 4.152840444720790e+03 * 2 / VEL_C;
	start = 2.793905088882601e+04;
	end = start + double(sceneHeight - 1) / prf;
	//util.bin2cvmat("D:\\working_dir\\others\\LJW\\dem.bin", dem);
	//dem.convertTo(dem, CV_16S);
	//lon_upperleft = 102.6008333333333;
	//lat_upperleft = 30.259166666666665;
	util.bin2cvmat("D:\\working_dir\\others\\LJW\\state_vec_S205.bin", statevec);
	Deflat flat;
	Mat mappedLon, mappedLat, mappedDem;
	int ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
		prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 500);
	conversion.creat_new_h5("D:\\working_dir\\others\\LJW\\S205_mapped.h5");
	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\S205_mapped.h5", "mappedDEM", mappedDem);
	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\S205_mapped.h5", "mappedLat", mappedLat);
	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\S205_mapped.h5", "mappedLon", mappedLon);
	return 0;
}

/*23所数据高程反演*/
//int main(int argc, char* argv[])
//{
//	/*
//	* 校正至绝对相位
//	*/
//
//	FormatConversion conversion; Utils util;
//	int nr, nc, ret, offset_row, offset_col;
//	double time_interval1, time_interval2, acquisitionStartTime1, acquisitionStartTime2, acquisitionStartTime0, acquisitionStopTime1,
//		acquisitionStopTime2, acquisitionStopTime0, wavelength, nearRange, prf1, prf2, range_spacing;
//	string source_1, source_2, tmp, start_time, end_time;
//	Mat unwrapped_phase, flat_phase_coefficient, gcps, temp, stateVec0,
//		stateVec1, stateVec2, lat_coefficient, lon_coefficient, carrier_frequency;
//	ret = util.bin2cvmat("D:\\working_dir\\others\\LJW\\unwrapped_phaseT.bin",unwrapped_phase);
//	nr = unwrapped_phase.rows; nc = unwrapped_phase.cols;
//	offset_row = offset_col = 0;
//	int sceneHeight = 21604;
//	int sceneWidth = 14336;
//	int mode = 2;
//	int iter_times = 5;
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\gcps.bin", gcps);
//	range_spacing = 0.124913521111012;
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\state_vec0.bin", stateVec0);
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\state_vec1.bin", stateVec1);
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\state_vec2.bin", stateVec2);
//	prf1 = 1000.0;
//	prf2 = 1000.0;
//	time_interval2 = 1.0 / prf2;
//	time_interval1 = 1.0 / prf1;
//	double prf = 1000.0;
//	wavelength = VEL_C / 35e9;
//	acquisitionStartTime1 = 5.368613613317703e+05;
//	acquisitionStopTime1 = acquisitionStartTime1 + double(sceneHeight - 1) / prf;
//	acquisitionStartTime2 = 5.368613613317703e+05;
//	acquisitionStopTime2 = acquisitionStartTime2 + double(sceneHeight - 1) / prf;
//	acquisitionStartTime0 = 5.368613613317703e+05;
//	acquisitionStopTime0 = acquisitionStartTime0 + double(sceneHeight - 1) / prf;
//	nearRange = 6489.875033809203;
//
//
//	//寻找图像范围内的控制点信息
//
//	int num_gcps = gcps.rows;
//	int row, col, i_gcp = 0, count = 0;
//	bool b_gcp = false;
//	vector<int> valid_row;
//	for (i_gcp = 0; i_gcp < num_gcps; i_gcp++)
//	{
//		row = (int)gcps.at<double>(i_gcp, 0);
//		col = (int)gcps.at<double>(i_gcp, 1);
//		if ((row - offset_row - 1) >= 0 && (row - offset_row - 1) < nr && (col - offset_col - 1) >= 0 && (col - offset_col - 1) < nc)
//		{
//			b_gcp = true;
//			valid_row.push_back(i_gcp);
//			//break;
//		}
//	}
//	Mat llh(1, 3, CV_64F), xyz_ground(1, 3, CV_64F);
//	if (b_gcp)
//	{
//		i_gcp = valid_row[0];
//		row = (int)gcps.at<double>(i_gcp, 0);
//		col = (int)gcps.at<double>(i_gcp, 1);
//		llh.at<double>(0, 0) = gcps.at<double>(i_gcp, 3);
//		llh.at<double>(0, 1) = gcps.at<double>(i_gcp, 2);
//		llh.at<double>(0, 2) = gcps.at<double>(i_gcp, 4);
//		row = row - offset_row; col = col - offset_col;
//	}
//	else
//	{
//		return -1;
//	}
//	ret = util.ell2xyz(llh, xyz_ground);
//
//
//	///*
//	//* 轨道插值
//	//*/
//	//orbitStateVectors stateVectors1(stateVec1, acquisitionStartTime1, acquisitionStopTime1);
//	//stateVectors1.applyOrbit();
//	//orbitStateVectors stateVectors2(stateVec2, acquisitionStartTime2, acquisitionStopTime2);
//	//stateVectors2.applyOrbit();
//	//orbitStateVectors stateVectors0(stateVec0, acquisitionStartTime0, acquisitionStopTime0);
//	//stateVectors0.applyOrbit();
//	/*
//	* 寻找图像左上角成像卫星位置
//	*/
//	Mat sate1 = Mat::zeros(nr, 3, CV_64F);//主星位置
//	Mat satev1 = Mat::zeros(nr, 3, CV_64F);//主星位置
//	Mat sate2 = Mat::zeros(nr, 3, CV_64F);//辅星位置
//	Mat sate0 = Mat::zeros(nr, 3, CV_64F);//辅星位置
//
//	sate0 = stateVec0;
//	sate1 = stateVec1;
//	sate2 = stateVec2;
//
//	////找到零多普勒位置
//	//Position pos; Velocity vel;
//	//for (int i = 0; i < nr; i++)
//	//{
//	//	stateVectors1.getPosition(acquisitionStartTime1 + double(offset_row + i) * time_interval1, pos);
//	//	stateVectors1.getVelocity(acquisitionStartTime1 + double(offset_row + i) * time_interval1, vel);
//	//	sate1.at<double>(i, 0) = pos.x;
//	//	sate1.at<double>(i, 1) = pos.y;
//	//	sate1.at<double>(i, 2) = pos.z;
//	//	satev1.at<double>(i, 0) = vel.vx;
//	//	satev1.at<double>(i, 1) = vel.vy;
//	//	satev1.at<double>(i, 2) = vel.vz;
//
//	//	stateVectors0.getPosition(acquisitionStartTime0 + double(offset_row + i) * time_interval1, pos);
//	//	sate0.at<double>(i, 0) = pos.x;
//	//	sate0.at<double>(i, 1) = pos.y;
//	//	sate0.at<double>(i, 2) = pos.z;
//
//	//	stateVectors2.getPosition(acquisitionStartTime2 + double(offset_row + i) * time_interval1, pos);
//	//	sate2.at<double>(i, 0) = pos.x;
//	//	sate2.at<double>(i, 1) = pos.y;
//	//	sate2.at<double>(i, 2) = pos.z;
//	//}
//	Mat dem_x, dem_y, dem_z;
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\dem_x.bin", dem_x);
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\dem_y.bin", dem_y);
//	util.bin2cvmat("D:\\working_dir\\others\\matlab_functions\\dem_z.bin", dem_z);
//	Mat phase0 = Mat::zeros(nr, nc, CV_64F);
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			Mat ground(1, 3, CV_64F);
//			ground.at<double>(0, 0) = dem_x.at<double>(i, j);
//			ground.at<double>(0, 1) = dem_y.at<double>(i, j);
//			ground.at<double>(0, 2) = dem_z.at<double>(i, j);
//			double r_main = sqrt(sum((sate1(cv::Range(i, i+1), cv::Range(0, 3)) - ground).mul(sate1(cv::Range(i, i + 1), cv::Range(0, 3)) - ground))[0]);
//			double r_slave = sqrt(sum((sate2(cv::Range(i, i+1), cv::Range(0, 3)) - ground).mul(sate2(cv::Range(i, i + 1), cv::Range(0, 3)) - ground))[0]);
//			phase0.at<double>(i, j) = (r_slave - r_main) / wavelength * 2 * PI;
//		}
//	}
//	util.wrap(phase0, phase0);
//	conversion.creat_new_h5("D:\\working_dir\\others\\LJW\\phase_real.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\phase_real.h5", "phase", phase0);
//	return 0;
//	//控制点绝对相位计算
//	double lambda = wavelength;
//	double K = 0.0;
//	Mat KK = Mat::zeros(1, valid_row.size(), CV_64F);
//	for (int i = 0; i < valid_row.size(); i++)
//	{
//		int rrr = (int)gcps.at<double>(valid_row[i], 0) - offset_row;
//		int ccc = (int)gcps.at<double>(valid_row[i], 1) - offset_col;
//		Mat ground(1, 3, CV_64F), llh_temp(1, 3, CV_64F);
//		llh_temp.at<double>(0, 0) = gcps.at<double>(valid_row[i], 3);
//		llh_temp.at<double>(0, 1) = gcps.at<double>(valid_row[i], 2);
//		llh_temp.at<double>(0, 2) = gcps.at<double>(valid_row[i], 4);
//		util.ell2xyz(llh_temp, ground);
//		double r_main = sqrt(sum((sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate1(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
//		double r_slave = sqrt(sum((sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground).mul(sate2(cv::Range(rrr - 1, rrr), cv::Range(0, 3)) - ground))[0]);
//		double C = mode == 1 ? 4 * PI : 2 * PI;
//		double phase_real = (r_slave - r_main) / lambda * C;
//		KK.at<double>(0, i) = ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
//		K += ((phase_real - unwrapped_phase.at<double>(rrr - 1, ccc - 1)) / (2 * PI));
//	}
//	conversion.creat_new_h5("D:\\working_dir\\others\\LJW\\k.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\k.h5", "k", KK);
//	K /= (double)valid_row.size();
//	unwrapped_phase = unwrapped_phase + round(K) * 2 * PI;//相位校正
//
//
//	/*
//	* 反演高程
//	*/
//
//	Mat R_M(1, nc, CV_64F);
//	for (int i = 0; i < nc; i++)
//	{
//		R_M.at<double>(0, i) = nearRange + range_spacing * double(i + offset_col);
//
//	}
//	Mat ones = Mat::ones(nr, 1, CV_64F);
//	R_M = ones * R_M;
//	Mat R_F = R_M * 1.0 + lambda * unwrapped_phase / (2 * PI);
//	Mat Satellite_M_T_Position = sate0;//主星发射位置
//	Mat Satellite_S_T_Position = sate0;//辅星发射位置
//	Mat Satellite_M_R_Position = sate1;//主星接收位置
//	Mat Satellite_S_R_Position = sate2;//辅星接收位置
//	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
//	Mat Vs = satev1;
//	ones = Mat::ones(nr, nc, CV_64F);
//	/*Mat P1 = ones * xyz_ground.at<double>(0, 0);
//	Mat P2 = ones * xyz_ground.at<double>(0, 1);
//	Mat P3 = ones * xyz_ground.at<double>(0, 2);*/
//	
//	Mat M_T, M_R, S_T, S_R;
//	Mat f1, f2, f3;
//	Mat det_Df, Df_ni11, Df_ni12, Df_ni13, Df_ni21, Df_ni22, Df_ni23;
//	Mat Df11, Df12, Df13, Df21, Df22, Df23, Df31, Df32, Df33, Df_ni31, Df_ni32, Df_ni33;
//	Mat delta_Rt1, delta_Rt2, delta_Rt3;
//	ones = Mat::ones(1, nc, CV_64F);
//	Mat temp_var, temp_var1;
//	Mat fd = Mat::zeros(1, nc, CV_64F);
//	for (int i = 0; i < iter_times; i++)
//	{
//		fprintf(stdout, "%d/%d\n", i + 1, iter_times);
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(0, 1)) * ones - dem_x;
//		temp_var = temp_var.mul(temp_var);
//		temp_var.copyTo(M_T);
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(1, 2)) * ones - dem_y;
//		temp_var = temp_var.mul(temp_var);
//		M_T = M_T + temp_var;
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(2, 3)) * ones - dem_z;
//		temp_var = temp_var.mul(temp_var);
//		M_T = M_T + temp_var;
//		cv::sqrt(M_T, f1);
//		f1 = f1 - R_M;
//
//		//temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		//temp_var = temp_var.mul(temp_var);
//		//temp_var.copyTo(S_T);
//		//temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		//temp_var = temp_var.mul(temp_var);
//		//S_T = S_T + temp_var;
//		//temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		//temp_var = temp_var.mul(temp_var);
//		//S_T = S_T + temp_var;
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
//		//cv::sqrt(S_T, f2);
//		cv::sqrt(S_R, temp_var);
//		f2 = temp_var - R_F;
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
//		ones = Mat::ones(nr, 1, CV_64F);
//		temp_var = ones * fd;
//		temp_var1 = R_M * lambda / 2.0;
//		f3 = f3 + temp_var.mul(temp_var1);
//
//		//Dff
//		//第一行：f(1)的x，y，z的导数
//		ones = Mat::ones(1, nc, CV_64F);
//		//temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		//cv::sqrt(M_T, temp_var1);
//		//temp_var1 = 1 / temp_var1;
//		//temp_var1 = -temp_var1;
//		//Df11 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(0, 1)) * ones - dem_x;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df11 = temp_var.mul(temp_var1);
//
//		//temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		//cv::sqrt(M_T, temp_var1);
//		//temp_var1 = 1 / temp_var1;
//		//temp_var1 = -temp_var1;
//		//Df12 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(1, 2)) * ones - dem_y;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df12 = temp_var.mul(temp_var1);
//
//		//temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		//cv::sqrt(M_T, temp_var1);
//		//temp_var1 = 1 / temp_var1;
//		//temp_var1 = -temp_var1;
//		//Df13 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(2, 3)) * ones - dem_z;
//		cv::sqrt(M_T, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df13 = temp_var.mul(temp_var1);
//
//
//		//第二行：f(2)的x，y，z的导数
//		//temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - dem_x;
//		//cv::sqrt(S_T, temp_var1);
//		//temp_var1 = 1 / temp_var1;
//		//temp_var1 = -temp_var1;
//		//Df21 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - dem_x;
//		cv::sqrt(S_R, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df21 = temp_var.mul(temp_var1);
//
//
//		//temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - dem_y;
//		//cv::sqrt(S_T, temp_var1);
//		//temp_var1 = 1 / temp_var1;
//		//temp_var1 = -temp_var1;
//		//Df22 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - dem_y;
//		cv::sqrt(S_R, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df22 = temp_var.mul(temp_var1);
//
//
//		//temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - dem_z;
//		//cv::sqrt(S_T, temp_var1);
//		//temp_var1 = 1 / temp_var1;
//		//temp_var1 = -temp_var1;
//		//Df23 = temp_var.mul(temp_var1);
//
//		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - dem_z;
//		cv::sqrt(S_R, temp_var1);
//		temp_var1 = 1 / temp_var1;
//		temp_var1 = -temp_var1;
//		Df23 = temp_var.mul(temp_var1);
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
//	}
//	delta_Rt1.release(); delta_Rt2.release(); delta_Rt3.release(); Df_ni31.release(); Df_ni32.release();
//	Df_ni33.release(); Df_ni21.release(); Df_ni22.release(); Df_ni23.release(); Df_ni11.release(); Df_ni12.release();
//	Df_ni13.release();
//	volatile bool parallel_flag = true;
//	Mat dem, lon, lat;
//	dem.create(nr, nc, CV_64F);
//	lon.create(nr, nc, CV_64F); lat.create(nr, nc, CV_64F);
//#pragma omp parallel for schedule(guided) \
//	private(ret)
//	for (int i = 0; i < nr; i++)
//	{
//		Utils util;
//		for (int j = 0; j < nc; j++)
//		{
//			double lat_val, lon_val, h_val;
//			ret = Utils::xyz2ell(dem_x.at<double>(i, j), dem_y.at<double>(i, j), dem_z.at<double>(i, j), lat_val, lon_val, h_val);
//			dem.at<double>(i, j) = h_val;
//			lat.at<double>(i, j) = lat_val;
//			lon.at<double>(i, j) = lon_val;
//		}
//	}
//	conversion.creat_new_h5("D:\\working_dir\\others\\LJW\\inversion.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\inversion.h5", "dem", dem);
//	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\inversion.h5", "lat", lat);
//	conversion.write_array_to_h5("D:\\working_dir\\others\\LJW\\inversion.h5", "lon", lon);
//	return 0;
//}
