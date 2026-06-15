// test3.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//
#include<Windows.h>
#include<complex.h>
#include <iostream>
#include<fstream>
//#include<WinInet.h>
#include<SensAPI.h>
//#include<WinInet.h>
//#include"hdf5.h"
//#include"gdal_priv.h"
#include"Utils.h"
#include"Registration.h"
#include"FormatConversion.h"
#include"Deflat.h"
#include"Filter.h"
#include"Unwrap.h"
#include"Dem.h"
#include"SBAS.h"
#include<tchar.h>
#include<atlstr.h>
#include<atlconv.h>
#include<time.h>
#include"Eigen/Dense"
#define Big2Little32(A) ((uint32_t)(A&0xff000000)>>24|(uint32_t)(A&0x00ff0000)>>8 | (uint32_t)(A&0x0000ff00)<<8|(uint32_t)(A&0x000000ff)<<24)
#define GET_NEXT_LINE \
{ \
    if( !fgets( instring, 256, fp ) ) \
        ch = 0; \
	    else \
        ch = *instring; \
}
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
#endif // _DEBUG
#pragma comment(lib, "Sensapi.lib")

using namespace std;
using cv::Range;

//int main(int argc, char* argv[])
//{
//	///////////////////////////////SBAS测试/////////////////////////////////////////
//	Utils util; Unwrap unwrap; Deflat flat; FormatConversion conversion; Registration regis; SBAS sbas;
//	
//	const char* file = "E:\\working_dir\\projects\\software\\InSAR\\bin\\files4.txt";
//
//	char str[1024];
//	char str1[1024];
//	vector<string> fileslist, filelist2;
//	FILE* fp = NULL;
//	fp = fopen(file, "rt");
//	if (!fp) return -1;
//	for (int i = 0; i < 24; i++)
//	{
//		memset(str, 0, 1024);
//		fgets(str, 1023, fp);
//		sscanf(str, "%s", str1);
//		fileslist.push_back(str1);
//		memset(str, 0, 1024);
//		sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\deramp2\\%d_deramp.h5", i + 1);
//		filelist2.push_back(str);
//	}
//	fclose(fp);
//
//	/*去除参考相位*/
//	//flat.SLCs_deramp(fileslist, 16, "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", filelist2);
//
//	Mat temporal, spatial, formation_matrix, spatial_baseline, temporal_baseline;
//	//util.spatialTemporalBaselineEstimation(fileslist, 16, temporal, spatial);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.bin", temporal);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatial.bin", spatial);
//	util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.bin", temporal);
//	util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatial.bin", spatial);
//
//	sbas.get_formation_matrix(spatial, temporal, 100, 0.8, formation_matrix, spatial_baseline, temporal_baseline);
//	formation_matrix.convertTo(formation_matrix, CV_64F);
//	util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\formation_matrix.bin", formation_matrix);
//	//return 0;
//	formation_matrix.convertTo(formation_matrix, CV_32S);
//	//sbas.generate_interferograms(filelist2, formation_matrix, spatial_baseline, temporal_baseline, 4, 4,
//	//	"E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_sbas2", true);
//	//return 0;
//	/*解缠*/
//	vector<SBAS_edge> edges;
//	vector<SBAS_node> nodes;
//	vector<SBAS_triangle> triangles;
//	vector<int> node_neighbours;
//	vector<string> phaseFiles;
//	int n_images = formation_matrix.rows;
//	for (int i = 0; i < n_images; i++)
//	{
//		for (int j = 0; j < i; j++)
//		{
//			if (formation_matrix.at<int>(i, j) == 1)
//			{
//				memset(str, 0, 1024);
//				sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_sbas2\\%d_%d.h5", i + 1, j + 1);
//				phaseFiles.push_back(str);
//			}
//		}
//	}
//
//	/*计算高相干点*/
//	Mat mask;
//	cout << "计算高相干点" << endl;
//	//sbas.generate_high_coherence_mask(phaseFiles, 3, 3, 0.8, 0.8, mask);
//	//mask.convertTo(mask, CV_64F);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\mask.bin", mask);
//	//return 0;
//	/*生成高相干三角网络*/
//	cout << "生成高相干点网络" << endl;
//	util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\mask.bin", mask); 
//	mask.convertTo(mask, CV_32S);
//	int nonzero = cv::countNonZero(mask);
//	sbas.write_high_coherence_node(mask, "E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.node");
//	util.gen_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.node",
//		"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	sbas.read_edges("E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.1.edge", nonzero, edges, node_neighbours);
//	sbas.init_SBAS_node(nodes, edges, node_neighbours);
//	sbas.init_SBAS_triangle("E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.1.ele",
//		"E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.1.neigh",
//		triangles, edges, nodes);
//	sbas.set_high_coherence_node_coordinate(mask, nodes);
//
//	/*测试二维解缠*/
//	cout << "解缠" << endl;
//	Mat phase, coherence;
//	double obj;
//	//for (int i = 0; i < phaseFiles.size(); i++)
//	//{
//	//	conversion.read_array_from_h5(phaseFiles[i].c_str(), "phase", phase);
//	//	util.phase_coherence(phase, coherence);
//	//	sbas.set_high_coherence_node_phase(mask, nodes, edges, phase);
//	//	sbas.set_weight_by_coherence(coherence, nodes, edges);
//	//	sbas.compute_high_coherence_residue(nodes, edges, triangles);
//	//	sbas.writeDIMACS_spatial("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net", nodes, edges, triangles);
//	//	unwrap.mcf_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net",
//	//		"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	//	sbas.readDIMACS("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net.sol", nodes, edges, triangles, &obj);
//	//	sbas.floodFillUnwrap(nodes, edges, 1);
//	//	sbas.retrieve_unwrapped_phase(nodes, phase);
//	//	memset(str, 0, 1024);
//	//	int ii, jj;
//	//	sscanf(phaseFiles[i].c_str(), "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_sbas2\\%d_%d.h5", &ii, &jj);
//	//	sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_unwrapped2\\%d_%d.bin", ii, jj);
//	//	util.cvmat2bin(str, phase);
//	//	for (int j = 0; j < nodes.size(); j++)
//	//	{
//	//		nodes[j].b_unwrapped = false;
//	//	}
//	//	fprintf(stdout, "解缠进度:%.4lf\n", double(i + 1) / phaseFiles.size() * 100.0);
//	//}
//
//	/*最小二乘法求解线性形变速率和高程残差*/
//
//	//首先确定矩阵B
//	int M = phaseFiles.size();//干涉图幅数
//	int N = filelist2.size() - 1;//时间序列数
//	Mat B(M, N, CV_64F); B = 0.0;
//	Mat one = Mat::ones(N, 1, CV_64F);
//	for (int i = 0; i < M; i++)
//	{
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_sbas2\\%d_%d.h5", &ii, &jj);
//		for (int j = jj; j < ii; j++)
//		{
//			B.at<double>(i, j - 1) = (temporal.at<double>(0, j) - temporal.at<double>(0, j - 1)) / 365.0;
//		}
//	}
//	util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\B.bin", B);
//	B = B * one;
//	//确定矩阵c
//	Mat c(M, 1, CV_64F); c = 0.0;
//	vector<Mat> phase_vec, phase_vec2;
//	phase_vec.resize(M); phase_vec2.resize(N + 1);
//	
//	int offset_col, row, col;
//	double nearRange, theta, spacing, wavelength, B_spatial, B_temporal;
//	for (int i = 0; i < M; i++)
//	{
//		Mat temp;
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_sbas2\\%d_%d.h5", &ii, &jj);
//		memset(str, 0, 1024);
//		sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_sbas2\\%d_%d.h5", ii, jj);
//		conversion.read_int_from_h5(str, "offset_col", &offset_col);
//		conversion.read_double_from_h5(str, "slant_range_first_pixel", &nearRange);
//		conversion.read_double_from_h5(str, "range_spacing", &spacing);
//		conversion.read_double_from_h5(str, "B_spatial", &B_spatial);
//		conversion.read_double_from_h5(str, "B_temporal", &B_temporal);
//		conversion.read_double_from_h5(str, "carrier_frequency", &wavelength);
//		wavelength = VEL_C / wavelength;
//		conversion.read_array_from_h5(str, "inc_coefficient", temp);
//		theta = temp.at<double>(0, 0) / 180.0 * PI;
//		double r = nearRange + double(offset_col) * spacing;
//		c.at<double>(i, 0) = 4 * PI / wavelength * B_spatial / sin(theta) / r;
//		memset(str, 0, 1024);
//		sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_unwrapped2\\%d_%d.bin", ii, jj);
//		util.bin2cvmat(str, phase);
//		phase.copyTo(phase_vec[i]);
//	}
//	Mat xssa = Mat::zeros(phase.rows, phase.cols, CV_64F);
//	for (int i = 0; i < N + 1; i++)
//	{
//		xssa.copyTo(phase_vec2[i]);
//	}
//
//	Mat BMc, temp(M, 1, CV_64F); temp = 0.0;
//	Mat v(phase.rows, phase.cols, CV_64F), z(phase.rows, phase.cols, CV_64F), temporal_coh(phase.rows, phase.cols, CV_64F);
//	v = 0.0; z = 0.0; temporal_coh = 0.0;
//	cv::hconcat(B, c, BMc);
//	//cout << c << endl;
//	double coh;
//	for (int i = 0; i < phase.rows; i++)
//	{
//		for (int j = 0; j < phase.cols; j++)
//		{
//			if (mask.at<int>(i, j) > 0)
//			{
//				for (int k = 0; k < M; k++)
//				{
//					temp.at<double>(k, 0) = phase_vec[k].at<double>(i, j);
//				}
//				//最小二乘法求解
//				Mat A_t, A, b;
//				BMc.copyTo(A);
//				temp.copyTo(b);
//				//cout << A << endl;
//				transpose(A, A_t);
//				A = A_t * A;
//				b = A_t * b;
//				Mat x;
//				//cout << A << endl;
//				//cout << temp << endl;
//				if (!cv::solve(A, b, x, cv::DECOMP_LU))
//				{
//					fprintf(stderr, "_PS_deflat(): can't solve least square problem!\n");
//				}
//				else
//				{
//					//cout << x << endl;
//					//Mat velocity_vec(N, 1, CV_64F); velocity_vec = 0.0;
//					//Mat phi_vec(N + 1, 1, CV_64F); phi_vec = 0.0;
//					//for (int k = 1; k < N + 1; k++)
//					//{
//					//	phase_vec2[k].at<double>(i, j) = x.at<double>(k - 1, 0) * 
//					//		(temporal.at<double>(0, k) - temporal.at<double>(0, k - 1)) / 365.0
//					//		+ phase_vec2[k - 1].at<double>(i, j);
//					//}
//					v.at<double>(i, j) = x.at<double>(0, 0);
//					z.at<double>(i, j) = x.at<double>(1, 0);
//				}
//				////计算时间相关系数
//				//x = B * x;
//				//coh = 0.0;
//				//sbas.compute_temporal_coherence(x, temp, &coh);
//				//temporal_coh.at<double>(i, j) = coh;
//			}
//		}
//	}
//	
//	//for (int i = 0; i < N + 1; i++)
//	//{
//	//	memset(str, 0, 1024);
//	//	sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_unwrapped2\\series_%d.bin", i + 1);
//	//	util.cvmat2bin(str, phase_vec2[i]);
//	//}
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal_coh.bin", temporal_coh);
//	util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\z.bin", z);
//	util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\v.bin", v);
//
//	return 0;
//	
//	
//	///////////////////////////////SBAS测试/////////////////////////////////////////
//
//
//
//
//	///////////////////////////////EMCF-SBAS时间维解缠测试/////////////////////////////////////////
//
//	//Utils util; Unwrap unwrap; Deflat flat; FormatConversion conversion; Registration regis; SBAS sbas;
//
//	//const char* file = "E:\\working_dir\\projects\\software\\InSAR\\bin\\files2.txt";
//
//	//char str[1024];
//	//char str1[1024];
//	//vector<string> fileslist, filelist2;
//	//FILE* fp = NULL;
//	//fp = fopen(file, "rt");
//	//if (!fp) return -1;
//	//for (int i = 0; i < 31; i++)
//	//{
//	//	memset(str, 0, 1024);
//	//	fgets(str, 1023, fp);
//	//	sscanf(str, "%s", str1);
//	//	fileslist.push_back(str1);
//	//	memset(str, 0, 1024);
//	//	sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\deramp2\\%d_deramp.h5", i + 1);
//	//	filelist2.push_back(str);
//	//}
//	//fclose(fp);
//	//Mat temporal, spatial;
//	////util.spatialTemporalBaselineEstimation(fileslist, 16, temporal, spatial);
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.bin", temporal);
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatial.bin", spatial);
//
//	///*去除参考相位*/
//	////flat.SLCs_deramp(fileslist, 16, "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", filelist2);
//
//	///*生成时空基线三角网*/
//	//vector<SBAS_edge> edges1, edges2;
//	//vector<SBAS_node> nodes1, nodes2;
//	//vector<SBAS_triangle> triangles1, triangles2;
//	//vector<int> node_neighbours;
//	//sbas.write_spatialTemporal_node("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatialTemporal.node", temporal, spatial);
//	//util.gen_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatialTemporal.node", 
//	//	"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	//sbas.read_edges("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatialTemporal.1.edge", temporal.cols, edges1, node_neighbours);
//	//sbas.init_SBAS_node(nodes1, edges1, node_neighbours);
//	//sbas.init_SBAS_triangle("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatialTemporal.1.ele", 
//	//	"E:\\working_dir\\projects\\software\\InSAR\\bin\\spatialTemporal.1.neigh",
//	//	triangles1, edges1, nodes1);
//	//sbas.set_spatialTemporalBaseline(nodes1, temporal, spatial);
//	///*生成差分干涉相位堆栈*/
//	////sbas.generate_interferograms(filelist2, edges1, nodes1, 4, 4, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg2", true);
//
//	//vector<string> phaseFiles;
//	//for (int i = 0; i < edges1.size(); i++)
//	//{
//	//	memset(str, 0, 1024);
//	//	sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg2\\%d.h5", i + 1);
//	//	phaseFiles.push_back(str);
//	//}
//
//	///*计算高相干点*/
//	//Mat mask;
//	////sbas.generate_high_coherence_mask(phaseFiles, 3, 3, 0.8, 0.95, mask);
//	////mask.convertTo(mask, CV_64F);
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\mask.bin", mask);
//	////return 0;
//	////util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\mask.jpg", "jet", mask);
//
//	///*生成高相干三角网络*/
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\mask.bin", mask); 
//	//mask.convertTo(mask, CV_32S);
//	//int nonzero = cv::countNonZero(mask);
//	//sbas.write_high_coherence_node(mask, "E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.node");
//	//util.gen_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.node",
//	//	"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	//sbas.read_edges("E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.1.edge", nonzero, edges2, node_neighbours);
//	//sbas.init_SBAS_node(nodes2, edges2, node_neighbours);
//	//sbas.init_SBAS_triangle("E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.1.ele",
//	//	"E:\\working_dir\\projects\\software\\InSAR\\bin\\high_coherence.1.neigh",
//	//	triangles2, edges2, nodes2);
//	//sbas.set_high_coherence_node_coordinate(mask, nodes2);
//	////sbas.saveGradientStack(phaseFiles, mask, nodes2, edges2, "E:\\working_dir\\projects\\software\\InSAR\\bin\\gradientStack2.h5");
//
//	///*测试二维解缠*/
//	//Mat phase, coherence;
//	//double obj;
//	//conversion.read_array_from_h5(phaseFiles[31].c_str(), "phase", phase);
//	////util.phase_coherence(phase, coherence);
//	////sbas.set_high_coherence_node_phase(mask, nodes2, edges2, phase);
//	////sbas.set_weight_by_coherence(coherence, nodes2, edges2);
//	////sbas.compute_high_coherence_residue(nodes2, edges2, triangles2);
//	////sbas.writeDIMACS_spatial("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net", nodes2, edges2, triangles2);
//	////unwrap.mcf_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net",
//	////	"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	////sbas.readDIMACS("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net.sol", nodes2, edges2, triangles2, &obj);
//	////sbas.floodFillUnwrap(nodes2, edges2, 1);
//	////sbas.retrieve_unwrapped_phase(nodes2, phase);
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\unwrapped_phase.bin", phase);
//	////return 0;
//	///*时间维解缠*/
//	////model adjustment
//	//double delta_v = 0.01;//(-1.0cm/year至1.0cm/year)
//	//double delta_v_interval = 0.005;
//	//double delta_h = 10.0;//(-50m至50m)
//	//double delta_h_interval = 5.0;
//	//Mat c1 = Mat::zeros(edges1.size(), 1, CV_64F);
//	//Mat c2 = Mat::zeros(edges1.size(), 1, CV_64F);
//	//Mat m = Mat::zeros(edges1.size(), 1, CV_64F);
//	//Mat gradient(edges1.size(), edges2.size(), CV_64F);
//	//Mat weight = Mat::ones(edges2.size(), 1, CV_64F);
//	//Mat temp;
//	//int offset_col, row, col;
//	//double nearRange, theta, spacing, wavelength, B_spatial, B_temporal;
//	////for (int i = 0; i < edges1.size(); i++)
//	////{
//	////	memset(str, 0, 1024);
//	////	sprintf(str, "ifg_gradient_%d", i + 1);
//	////	conversion.read_array_from_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\gradientStack2.h5", str, temp);
//	////	temp.copyTo(gradient(cv::Range(i, i + 1), cv::Range(0, temp.cols)));
//	////	memset(str, 0, 1024);
//	////	sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg2\\%d.h5", i + 1);
//	////	conversion.read_int_from_h5(str, "offset_col", &offset_col);
//	////	conversion.read_double_from_h5(str, "slant_range_first_pixel", &nearRange);
//	////	conversion.read_double_from_h5(str, "range_spacing", &spacing);
//	////	conversion.read_double_from_h5(str, "B_spatial", &B_spatial);
//	////	conversion.read_double_from_h5(str, "B_temporal", &B_temporal);
//	////	conversion.read_double_from_h5(str, "carrier_frequency", &wavelength);
//	////	wavelength = VEL_C / wavelength;
//	////	conversion.read_array_from_h5(str, "inc_coefficient", temp);
//	////	theta = temp.at<double>(0, 0) / 180.0 * PI;
//	////	double r = nearRange + double(offset_col) * spacing;
//	////	c1.at<double>(i, 0) = 4 * PI / wavelength * B_spatial / sin(theta) / r;
//	////	c2.at<double>(i, 0) = 4 * PI / wavelength * B_temporal / 365.0;
//	////}
//	////for (int i = 0; i < edges2.size(); i++)
//	////{
//	////	double delta1, delta2, obj_value, flow_sum, obj_min = 100.0 * edges1.size();
//	////	gradient(cv::Range(0, edges1.size()), cv::Range(i, i + 1)).copyTo(temp);
//	////	int num_residue, residue_num_min = triangles1.size() + 2;
//	////	Mat mm;
//	////	for (int j = 0; j < 5; j++)
//	////	{
//	////		for (int k = 0; k < 5; k++)
//	////		{
//	////			
//	////			delta1 = -delta_h + j * delta_h_interval;
//	////			delta2 = -delta_v + k * delta_v_interval;
//	////			m = c1 * delta1 + c2 * delta2;
//	////			m = temp - m;
//	////			util.wrap(m, m);
//	////			m = m + temp;
//	////			for (int ii = 0; ii < edges1.size(); ii++)
//	////			{
//	////				edges1[ii].phase_gradient = m.at<double>(ii, 0);
//	////			}
//	////			sbas.compute_spatialTemporal_residue(nodes1, edges1, triangles1);
//	////			
//	////			sbas.residue_num(triangles1, &num_residue);
//	////			if (num_residue < residue_num_min)
//	////			{
//	////				residue_num_min = num_residue;
//	////				m.copyTo(mm);
//	////			}
//	////		}
//	////	}
//	////	for (int ii = 0; ii < edges1.size(); ii++)
//	////	{
//	////		edges1[ii].phase_gradient = mm.at<double>(ii, 0);
//	////	}
//	////	sbas.compute_spatialTemporal_residue(nodes1, edges1, triangles1);
//
//	////	sbas.residue_num(triangles1, &num_residue);
//	////	if (num_residue != 0)
//	////	{
//	////		sbas.writeDIMACS_temporal("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.net",
//	////			nodes1, edges1, triangles1);
//	////		unwrap.mcf_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.net",
//	////			"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	////		sbas.readDIMACS("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.net.sol",
//	////			nodes1, edges1, triangles1, &obj_value, &flow_sum);
//	////		obj_min = flow_sum;
//	////		weight.at<double>(i, 0) = flow_sum;
//	////		
//	////	}
//	////	for (int iii = 0; iii < edges1.size(); iii++)
//	////	{
//	////		m.at<double>(iii, 0) = edges1[iii].phase_gradient;
//	////	}
//	////	m.copyTo(gradient(cv::Range(0, edges1.size()), cv::Range(i, i + 1)));
//
//
//	////	fprintf(stdout, "进度:%.4lf\n", double(i) / edges2.size() * 100.0);
//	////}
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\gradient.bin", gradient);
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\weight.bin", weight);
//	///*空间解缠*/
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\gradient.bin", gradient);
//	//util.bin2cvmat("E:\\working_dir\\projects\\software\\InSAR\\bin\\weight.bin", weight);
//	//phase = 0.0;
//	//for (int i = 0; i < edges1.size(); i++)
//	//{
//	//	for (int j = 0; j < edges2.size(); j++)
//	//	{
//	//		edges2[j].phase_gradient = gradient.at<double>(i, j);
//	//		edges2[j].weight = weight.at<double>(j, 0);
//	//	}
//	//	sbas.compute_high_coherence_residue_by_gradient(nodes2, edges2, triangles2);
//	//	sbas.writeDIMACS_spatial("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net", nodes2, edges2, triangles2);
//	//	unwrap.mcf_delaunay("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net",
//	//		"E:\\working_dir\\projects\\software\\InSAR\\bin");
//	//	sbas.readDIMACS("E:\\working_dir\\projects\\software\\InSAR\\bin\\mcf_problem.net.sol", nodes2, edges2, triangles2, &obj);
//	//	sbas.floodFillUnwrap(nodes2, edges2, 1);
//	//	sbas.retrieve_unwrapped_phase(nodes2, phase);
//	//	memset(str, 0, 1024);
//	//	sprintf(str, "E:\\working_dir\\projects\\software\\InSAR\\bin\\ifg_unwrapped\\%d.bin", i + 1);
//	//	util.cvmat2bin(str, phase);
//	//	for (int j = 0; j < nodes2.size(); j++)
//	//	{
//	//		nodes2[j].b_unwrapped = false;
//	//	}
//	//}
//
//	//return 0;
//
//	///////////////////////////////EMCF-SBAS时间维解缠测试/////////////////////////////////////////
//
//
//	//Utils util; Unwrap unwrap; Deflat flat; FormatConversion conversion; Registration regis;
//
//	////const char* file = "E:\\working_dir\\projects\\software\\InSAR\\bin\\files.txt";
//
//	////char str[1024];
//	////char str1[1024];
//	////vector<string> fileslist;
//	////FILE* fp = NULL;
//	////fp = fopen(file, "rt");
//	////if (!fp) return -1;
//	////for (int i = 0; i < 31; i++)
//	////{
//	////	memset(str, 0, 1024);
//	////	fgets(str, 1023, fp);
//	////	sscanf(str, "%s", str1);
//	////	fileslist.push_back(str1);
//	////}
//	////fclose(fp);
//	////Mat temporal, spatial;
//	////util.spatialTemporalBaselineEstimation(fileslist, 16, temporal, spatial);
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.bin", temporal);
//	////util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatial.bin", spatial);
//	////return 0;
//	///*SLC参考相位去除测试程序*/
//
//	////获取DEM
//	//
//	//const char* master_file = "D:\\experiment\\SatExplorer\\marmar\\AOI2\\20110102_regis_cut2.h5";
//	//const char* slave_file = "D:\\experiment\\SatExplorer\\marmar\\AOI2\\20110124_regis_cut2.h5";
//	//const char* dempath = "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem";
//	//double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing, rangeSpacing2,
//	//	nearRangeTime, nearRangeTime2, wavelength, prf, prf2,
//	//	start, end, start2, end2, a0, a1, a2, b0, b1, b2;
//	//int sceneHeight, sceneWidth, sceneHeight2, sceneWidth2, offset_row, offset_col, offset_row2, offset_col2;
//	//Mat lon_coef, lat_coef, dem, mappedDem, statevec, rangePos, azimuthPos,
//	//	lon_coef2, lat_coef2, statevec2, rangePos2, azimuthPos2, slaveRangeOffset, slaveAzimuthOffset;
//	//string start_time, end_time;
//	//ComplexMat master, slave;
//	//conversion.read_int_from_h5(master_file, "range_len", &sceneWidth);
//	//conversion.read_int_from_h5(master_file, "azimuth_len", &sceneHeight);
//	//conversion.read_int_from_h5(master_file, "offset_row", &offset_row);
//	//conversion.read_int_from_h5(master_file, "offset_col", &offset_col);
//	//conversion.read_array_from_h5(master_file, "lon_coefficient", lon_coef);
//	//conversion.read_array_from_h5(master_file, "lat_coefficient", lat_coef);
//	//conversion.read_double_from_h5(master_file, "prf", &prf);
//	//conversion.read_double_from_h5(master_file, "carrier_frequency", &wavelength);
//	//wavelength = VEL_C / wavelength;
//	//conversion.read_double_from_h5(master_file, "range_spacing", &rangeSpacing);
//	//conversion.read_double_from_h5(master_file, "slant_range_first_pixel", &nearRangeTime);
//	//nearRangeTime = 2.0 * nearRangeTime / VEL_C;
//	//conversion.read_str_from_h5(master_file, "acquisition_start_time", start_time);
//	//conversion.utc2gps(start_time.c_str(), &start);
//	//conversion.read_str_from_h5(master_file, "acquisition_stop_time", end_time);
//	//conversion.utc2gps(end_time.c_str(), &end);
//	//conversion.read_array_from_h5(master_file, "state_vec", statevec);
//	//conversion.read_slc_from_h5(master_file, master);
//
//	//conversion.read_slc_from_h5(slave_file, slave);
//
//	//Utils::computeImageGeoBoundry(lat_coef, lon_coef, sceneHeight, sceneWidth, offset_row, offset_col,
//	//	&lonMax, &latMax, &lonMin, &latMin);
//
//	//Utils::getSRTMDEM(dempath, dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//	//Mat mappedLon, mappedLat;
//	//flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, offset_row, offset_col, sceneHeight, sceneWidth,
//	//	prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 20);
//
//	//Mat dem1;
//	//if (mappedDem.type() != CV_64F) mappedDem.convertTo(dem1, CV_64F);
//	////conversion.creat_new_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\dem.h5");
//	////conversion.write_array_to_h5("E:\\working_dir\\projects\\software\\InSAR\\bin\\dem.h5", "dem", dem1);
//	//util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\dem.jpg", "jet", dem1);
//	////return 0;
//	//flat.SLC_deramp(master, mappedDem, mappedLat, mappedLon, master_file);
//	//flat.SLC_deramp(slave, mappedDem, mappedLat, mappedLon, slave_file);
//	//Mat phase, phase2;
//	//if (master.type() != CV_64F) master.convertTo(master, CV_64F);
//	//if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
//	//util.multilook(master, slave, 1, 1, phase2);
//	//util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\phase2.jpg", "jet", phase2);
//	////conversion.read_array_from_h5("D:\\experiment\\SatExplorer\\beijing\\ifg6\\20120122_regis_cut2_20120328_regis_cut2.h5", "phase", phase);
//	////phase2 = phase2 - phase;
//	////util.wrap(phase2, phase2);
//	////util.savephase("E:\\working_dir\\projects\\software\\InSAR\\bin\\delta_phase.jpg", "jet", phase2);
//	//return 0;
//
//
//
//
//	
//	//Mat wrapped_phase, unwrapped_phase, residue, k1, k2, coherence, phase_derivatives_variance1, phase_derivatives_variance;
//
//	////util.read_DIMACS("E:\\zgb1\\functions\\mcf_problem.net.sol", k1, k2, 3006, 3006);
//
//
//	//util.bin2cvmat("E:\\zgb1\\functions\\wrapped_phase.bin", wrapped_phase);
//	////util.phase_coherence(wrapped_phase, coherence);
//	////util.cvmat2bin("E:\\zgb1\\functions\\coherence.bin", coherence); return 0;
//	//util.phase_derivatives_variance(wrapped_phase, phase_derivatives_variance);
//	////cv::normalize(phase_derivatives_variance, phase_derivatives_variance1, 1, 0, cv::NORM_MINMAX);
//	////phase_derivatives_variance1 = phase_derivatives_variance1 * 255.0;
//	////phase_derivatives_variance1.convertTo(phase_derivatives_variance1, CV_8U);
//	////cv::threshold(phase_derivatives_variance1, phase_derivatives_variance1, 10, 1, cv::THRESH_OTSU);
//	////phase_derivatives_variance1.convertTo(phase_derivatives_variance1, CV_64F);
//	////util.cvmat2bin("E:\\zgb1\\functions\\phase_derivatives_variance1.bin", phase_derivatives_variance1);
//	////util.cvmat2bin("E:\\zgb1\\functions\\phase_derivatives_variance.bin", phase_derivatives_variance); return 0;
//	//util.residue(wrapped_phase, residue);
//	//util.cvmat2bin("E:\\zgb1\\functions\\residue.bin", residue);
//	////util.phase_derivatives_variance(wrapped_phase, coherence, 3);
//	////util.phase_coherence(wrapped_phase, coherence);
//	////coherence = 1.0 - coherence;
//	////unwrap.qualityGuided(wrapped_phase, unwrapped_phase, coherence);
//
//	////unwrap.snaphu(wrapped_phase, unwrapped_phase, "E:\\zgb1\\functions");
//	////util.cvmat2bin("E:\\zgb1\\functions\\unwrapped_phase.bin", unwrapped_phase);
//	////return 0;
//	//unwrap.MCF_improved(wrapped_phase, unwrapped_phase, "E:\\zgb1\\functions\\mcf_problem.net",
//	//	"E:\\working_dir\\projects\\software\\InSAR\\bin", 0.1);
//	//util.cvmat2bin("E:\\zgb1\\functions\\unwrapped_phase.bin", unwrapped_phase);
//	////return 0;
//
//	////vector<double> from, to;
//	////long f, t; double x;
//	////FILE* fp = NULL;
//	////fp = fopen("E:\\zgb1\\functions\\mcf_problem.net.sol", "rt");
//	////if (!fp) return -1;
//	////char instring[256];
//	////char ch;
//	////GET_NEXT_LINE; GET_NEXT_LINE; GET_NEXT_LINE; GET_NEXT_LINE; GET_NEXT_LINE; GET_NEXT_LINE;
//	////while (ch == 'f')
//	////{
//	////	sscanf(instring, "f %ld %ld %lf", &f, &t, &x);
//	////	from.push_back(f); to.push_back(t);
//	////	GET_NEXT_LINE;
//	////}
//	////fclose(fp);
//	////Mat m(from.size(), 1, CV_64F), n(from.size(), 1, CV_64F);
//	////for (int i = 0; i < from.size(); i++)
//	////{
//	////	m.at<double>(i, 0) = from[i];
//	////	n.at<double>(i, 0) = to[i];
//	////}
//	////util.cvmat2bin("E:\\zgb1\\functions\\from.bin", m);
//	////util.cvmat2bin("E:\\zgb1\\functions\\to.bin", n);
//	////return 0;
//
//
//
//	
//
//	
//
//
//}

/*王媛tgrs论文小基线集处理*/
//int main(int argc, char* argv[])
//{
//	Utils util; Unwrap unwrap; Deflat flat; FormatConversion conversion; Registration regis; SBAS sbas;
//	
//
//	char str[1024];
//	char str1[1024];
//	vector<string> fileslist, filelist2;
//	for (int i = 0; i < 33; i++)
//	{
//		sprintf(str, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\SAR_images\\(%d).h5", i + 1);
//		filelist2.push_back(str);
//	}
//
//	/*去除参考相位*/
//	//flat.SLCs_deramp(fileslist, 16, "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", filelist2);
//
//	Mat temporal, spatial, formation_matrix, spatial_baseline, temporal_baseline;
//	//util.spatialTemporalBaselineEstimation(fileslist, 16, temporal, spatial);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\temporal.bin", temporal);
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\spatial.bin", spatial);
//	util.bin2cvmat("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\temporal.bin", temporal_baseline);
//	util.bin2cvmat("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\spatial.bin", spatial_baseline);
//	util.bin2cvmat("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\temporal_baseline.bin", temporal);
//	util.bin2cvmat("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\spatial_baseline.bin", spatial);
//	util.bin2cvmat("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\formation_matrix.bin", formation_matrix);
//	//return 0;
//	formation_matrix.convertTo(formation_matrix, CV_32S);
//	sbas.generate_interferograms(filelist2, formation_matrix, spatial_baseline, temporal_baseline, 4, 4,
//		"D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg", true);
//	//return 0;
//	/*解缠*/
//	vector<SBAS_edge> edges;
//	vector<SBAS_node> nodes;
//	vector<SBAS_triangle> triangles;
//	vector<int> node_neighbours;
//	vector<string> phaseFiles;
//	int n_images = formation_matrix.rows;
//	for (int i = 0; i < n_images; i++)
//	{
//		for (int j = 0; j < i; j++)
//		{
//			if (formation_matrix.at<int>(i, j) == 1)
//			{
//				memset(str, 0, 1024);
//				sprintf(str, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg\\%d_%d.h5", i + 1, j + 1);
//				phaseFiles.push_back(str);
//			}
//		}
//	}
//
//	/*计算高相干点*/
//	Mat mask;
//	cout << "计算高相干点" << endl;
//	//sbas.generate_high_coherence_mask(phaseFiles, 3, 3, 0.4, 0.4, mask);
//	//mask.convertTo(mask, CV_64F);
//	//util.cvmat2bin("D:\\working_dir\\others\\wangyuan\\tgrs\\Sentinel_VH_SBAS_compare\\mask.bin", mask);
//	//return 0;
//	/*生成高相干三角网络*/
//	cout << "生成高相干点网络" << endl;
//	util.bin2cvmat("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\mask.bin", mask); 
//	mask.convertTo(mask, CV_32S);
//	int nonzero = cv::countNonZero(mask);
//	sbas.write_high_coherence_node(mask, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\high_coherence.node");
//	util.gen_delaunay("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\high_coherence.node",
//		"D:\\working_dir\\projects\\software\\InSAR\\bin");
//	sbas.read_edges("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\high_coherence.1.edge", nonzero, edges, node_neighbours);
//	sbas.init_SBAS_node(nodes, edges, node_neighbours);
//	sbas.init_SBAS_triangle("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\high_coherence.1.ele",
//		"D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\high_coherence.1.neigh",
//		triangles, edges, nodes);
//	sbas.set_high_coherence_node_coordinate(mask, nodes);
//
//	/*测试二维解缠*/
//	cout << "解缠" << endl;
//	Mat phase, coherence;
//	double obj;
//	for (int i = 0; i < phaseFiles.size(); i++)
//	{
//		conversion.read_array_from_h5(phaseFiles[i].c_str(), "phase", phase);
//		util.phase_coherence(phase, coherence);
//		sbas.set_high_coherence_node_phase(mask, nodes, edges, phase);
//		sbas.set_weight_by_coherence(coherence, nodes, edges);
//		sbas.compute_high_coherence_residue(nodes, edges, triangles);
//		sbas.writeDIMACS_spatial("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\mcf_problem.net", nodes, edges, triangles);
//		unwrap.mcf_delaunay("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\mcf_problem.net",
//			"D:\\working_dir\\projects\\software\\InSAR\\bin");
//		sbas.readDIMACS("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\mcf_problem.net.sol", nodes, edges, triangles, &obj);
//		sbas.floodFillUnwrap(nodes, edges, 1);
//		sbas.retrieve_unwrapped_phase(nodes, phase);
//		memset(str, 0, 1024);
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg\\%d_%d.h5", &ii, &jj);
//		sprintf(str, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg_unwrapped\\%d_%d.bin", ii, jj);
//		util.cvmat2bin(str, phase);
//		for (int j = 0; j < nodes.size(); j++)
//		{
//			nodes[j].b_unwrapped = false;
//		}
//		fprintf(stdout, "解缠进度:%.4lf\n", double(i + 1) / phaseFiles.size() * 100.0);
//	}
//	//return 0;
//	/*最小二乘法求解线性形变速率和高程残差*/
//
//	//首先确定矩阵B
//	int M = phaseFiles.size();//干涉图幅数
//	int N = filelist2.size() - 1;//时间序列数
//	Mat B(M, N, CV_64F); B = 0.0;
//	Mat one = Mat::ones(N, 1, CV_64F);
//	for (int i = 0; i < M; i++)
//	{
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg\\%d_%d.h5", &ii, &jj);
//		for (int j = jj; j < ii; j++)
//		{
//			B.at<double>(i, j - 1) = (temporal.at<double>(0, j) - temporal.at<double>(0, j - 1)) / 365.0;
//		}
//	}
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\B.bin", B);
//	Mat B1 = B * one;
//	//确定矩阵c
//	Mat c(M, 1, CV_64F); c = 0.0;
//	vector<Mat> phase_vec, phase_vec2, coh_vec;
//	phase_vec.resize(M); phase_vec2.resize(N + 1); coh_vec.resize(M);
//	/*int ref_i = 29, ref_j = 209;*/
//	int ref_i = 107, ref_j = 469;
//	int offset_col, row, col;
//	double nearRange, theta, spacing, wavelength, B_spatial, B_temporal;
//	for (int i = 0; i < M; i++)
//	{
//		Mat temp;
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg\\%d_%d.h5", &ii, &jj);
//		memset(str, 0, 1024);
//		sprintf(str, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg\\%d_%d.h5", ii, jj);
//		conversion.read_int_from_h5(str, "offset_col", &offset_col);
//		conversion.read_double_from_h5(str, "slant_range_first_pixel", &nearRange);
//		conversion.read_double_from_h5(str, "range_spacing", &spacing);
//		conversion.read_double_from_h5(str, "B_spatial", &B_spatial);
//		conversion.read_double_from_h5(str, "B_temporal", &B_temporal);
//		conversion.read_double_from_h5(str, "carrier_frequency", &wavelength);
//		wavelength = VEL_C / wavelength;
//		conversion.read_array_from_h5(str, "inc_coefficient", temp);
//		theta = temp.at<double>(0, 0) / 180.0 * PI;
//		double r = nearRange + double(offset_col) * spacing;
//		c.at<double>(i, 0) = 4 * PI / wavelength * B_spatial / sin(theta) / r;
//		conversion.read_array_from_h5(phaseFiles[i].c_str(), "coherence", coh_vec[i]);
//		memset(str, 0, 1024);
//		sprintf(str, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\ifg_unwrapped\\%d_%d.bin", ii, jj);
//		util.bin2cvmat(str, phase);
//		phase = phase - phase.at<double>(ref_i, ref_j);
//		phase.copyTo(phase_vec[i]);
//	}
//	Mat xssa = Mat::zeros(phase.rows, phase.cols, CV_64F);
//	for (int i = 0; i < N + 1; i++)
//	{
//		xssa.copyTo(phase_vec2[i]);
//	}
//
//	Mat BMc;
//	Mat v(phase.rows, phase.cols, CV_64F), z(phase.rows, phase.cols, CV_64F);
//	v = 0.0; z = 0.0; Mat temporal_coh(phase.rows, phase.cols, CV_64F); temporal_coh = 0.0;
//	cv::hconcat(B1, c, BMc);
//	//cout << c << endl;
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < phase.rows; i++)
//	{
//		Mat temp_coh(M, M, CV_64F); temp_coh = 0.0; double coh; Mat temp(M, 1, CV_64F); temp = 0.0;
//		for (int j = 0; j < phase.cols; j++)
//		{
//			if (mask.at<int>(i, j) > 0)
//			{
//				for (int k = 0; k < M; k++)
//				{
//					temp.at<double>(k, 0) = phase_vec[k].at<double>(i, j);
//					temp_coh.at<double>(k, k) = coh_vec[k].at<double>(i, j);
//				}
//				//最小二乘法求解
//				Mat A_t, A, b;
//				BMc.copyTo(A);
//				temp.copyTo(b);
//				//cout << A << endl;
//				transpose(A, A_t);
//				A = A_t * temp_coh * A;
//				b = A_t * temp_coh * b;
//				Mat x;
//				//cout << A << endl;
//				//cout << temp << endl;
//				if (!cv::solve(A, b, x, cv::DECOMP_LU))
//				{
//					fprintf(stderr, "_PS_deflat(): can't solve least square problem!\n");
//				}
//				else
//				{
//					//cout << x << endl;
//					//Mat velocity_vec(N, 1, CV_64F); velocity_vec = 0.0;
//					//Mat phi_vec(N + 1, 1, CV_64F); phi_vec = 0.0;
//					//for (int k = 1; k < N + 1; k++)
//					//{
//					//	phase_vec2[k].at<double>(i, j) = x.at<double>(k - 1, 0) * 
//					//		(temporal.at<double>(0, k) - temporal.at<double>(0, k - 1)) / 365.0
//					//		+ phase_vec2[k - 1].at<double>(i, j);
//					//}
//					v.at<double>(i, j) = x.at<double>(0, 0);
//					z.at<double>(i, j) = x.at<double>(1, 0);
//				}
//				//减去地形误差相位
//				temp = temp - x.at<double>(1, 0) * c;
//				B.copyTo(A);
//				temp.copyTo(b);
//				cv::transpose(A, A_t);
//				A = A_t * temp_coh * A;
//				b = A_t * temp_coh * b;
//				if (!cv::solve(A, b, x, cv::DECOMP_SVD))
//				{
//					fprintf(stderr, "SBAS_time_series(): can't solve SVD!\n");
//				}
//				else
//				{
//					for (int k = 1; k < N + 1; k++)
//					{
//						phase_vec2[k].at<double>(i, j) = x.at<double>(k - 1, 0) *
//							(temporal.at<double>(0, k) - temporal.at<double>(0, k - 1)) / 365.0
//							+ phase_vec2[k - 1].at<double>(i, j);
//					}
//				}
//				//计算时间相关系数
//				x = B * x;
//				coh = 0.0;
//				sbas.compute_temporal_coherence(x, temp, &coh);
//				temporal_coh.at<double>(i, j) = coh;
//				//计算时间相关系数
//				//x = B * x;
//				//coh = 0.0;
//				//sbas.compute_temporal_coherence(x, temp, &coh);
//				//temporal_coh.at<double>(i, j) = coh;
//			}
//		}
//	}
//	
//	for (int i = 0; i < N + 1; i++)
//	{
//		memset(str, 0, 1024);
//		sprintf(str, "D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\result\\series_%d.bin", i + 1);
//		util.cvmat2bin(str, phase_vec2[i]);
//	}
//	util.cvmat2bin("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\result\\temporal_coh.bin", temporal_coh);
//	util.cvmat2bin("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\result\\z.bin", z);
//	util.cvmat2bin("D:\\working_dir\\others\\wangyuan\\tgrs\\TSX_SBAS_compare\\result\\v.bin", v);
//
//	return 0;
//}


/*jstars论文小基线集处理*/
//int main(int argc, char* argv[])
//{
//	Utils util; Unwrap unwrap; Deflat flat; FormatConversion conversion; Registration regis; SBAS sbas;
//
//
//	char str[1024];
//	char str1[1024];
//	vector<string> fileslist, filelist2;
//	//FILE* fp = fopen("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\yanjiao\\file.txt", "rt");
//
//	for (int i = 0; i < 30; i++)
//	{
//		//fgets(str1, 1024, fp);
//		//string ss = str1;
//		//ss = ss.substr(0, ss.length() - 1);
//		sprintf(str1, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\proposed\\slc_%d.h5", i + 1);
//		sprintf(str, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\slc_%d.h5", i + 1);
//		filelist2.push_back(str);
//		//conversion.Copy_para_from_h5_2_h5(str1, str);
//	}
//	//fclose(fp);
//	
//	/*去除参考相位*/
//	//flat.SLCs_deramp(fileslist, 16, "E:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", filelist2);
//
//	Mat temporal, spatial, formation_matrix, spatial_baseline, temporal_baseline;
//	util.bin2cvmat("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\proposed\\temporal_baseline.bin", temporal);
//	util.bin2cvmat("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\proposed\\spatial_baseline.bin", spatial);
//	sbas.get_formation_matrix(spatial, temporal, 100, 60, 1000/365.0, formation_matrix, spatial_baseline, temporal_baseline);
//	//util.cvmat2bin("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\proposed\\formation_matrix.bin", formation_matrix);
//	//return 0;
//	formation_matrix.convertTo(formation_matrix, CV_32S);
//	//sbas.generate_interferograms(filelist2, formation_matrix, spatial_baseline, temporal_baseline, 2, 2,
//	//	"D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\ifg", true);
//	//return 0;
//	/*解缠*/
//	vector<SBAS_edge> edges;
//	vector<SBAS_node> nodes;
//	vector<SBAS_triangle> triangles;
//	vector<int> node_neighbours;
//	vector<string> phaseFiles;
//	int n_images = formation_matrix.rows;
//	for (int i = 0; i < n_images; i++)
//	{
//		for (int j = 0; j < i; j++)
//		{
//			if (formation_matrix.at<int>(i, j) == 1)
//			{
//				memset(str, 0, 1024);
//				sprintf(str, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\ifg\\%d_%d.h5", i + 1, j + 1);
//				phaseFiles.push_back(str);
//			}
//		}
//	}
//
//	/*计算高相干点*/
//	Mat mask;
//	cout << "计算高相干点" << endl;
//	//sbas.generate_high_coherence_mask(phaseFiles, 3, 3, 0.5, 0.5, mask);
//	//mask.convertTo(mask, CV_64F);
//	//util.cvmat2bin("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\proposed\\mask.bin", mask);
//	//return 0;
//	/*生成高相干三角网络*/
//	cout << "生成高相干点网络" << endl;
//	util.bin2cvmat("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\proposed\\mask.bin", mask);
//	mask.convertTo(mask, CV_32S);
//	int nonzero = cv::countNonZero(mask);
//	sbas.write_high_coherence_node(mask, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\high_coherence.node");
//	util.gen_delaunay("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\high_coherence.node",
//		"D:\\working_dir\\projects\\software\\InSAR\\bin");
//	sbas.read_edges("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\high_coherence.1.edge", nonzero, edges, node_neighbours);
//	sbas.init_SBAS_node(nodes, edges, node_neighbours);
//	sbas.init_SBAS_triangle("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\high_coherence.1.ele",
//		"D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\high_coherence.1.neigh",
//		triangles, edges, nodes);
//	sbas.set_high_coherence_node_coordinate(mask, nodes);
//
//	/*测试二维解缠*/
//	cout << "解缠" << endl;
//	Mat phase, coherence;
//	double obj;
//	//for (int i = 0; i < phaseFiles.size(); i++)
//	//{
//	//	conversion.read_array_from_h5(phaseFiles[i].c_str(), "phase", phase);
//	//	util.phase_coherence(phase, coherence);
//	//	sbas.set_high_coherence_node_phase(mask, nodes, edges, phase);
//	//	sbas.set_weight_by_coherence(coherence, nodes, edges);
//	//	sbas.compute_high_coherence_residue(nodes, edges, triangles);
//	//	sbas.writeDIMACS_spatial("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\mcf_problem.net", nodes, edges, triangles);
//	//	unwrap.mcf_delaunay("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\mcf_problem.net",
//	//		"D:\\working_dir\\projects\\software\\InSAR\\bin");
//	//	sbas.readDIMACS("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\mcf_problem.net.sol", nodes, edges, triangles, &obj);
//	//	sbas.floodFillUnwrap(nodes, edges, 1);
//	//	sbas.retrieve_unwrapped_phase(nodes, phase);
//	//	memset(str, 0, 1024);
//	//	int ii, jj;
//	//	sscanf(phaseFiles[i].c_str(), "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\ifg\\%d_%d.h5", &ii, &jj);
//	//	sprintf(str, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\ESPO\\ifg_unwrapped\\%d_%d.bin", ii, jj);
//	//	util.cvmat2bin(str, phase);
//	//	for (int j = 0; j < nodes.size(); j++)
//	//	{
//	//		nodes[j].b_unwrapped = false;
//	//	}
//	//	fprintf(stdout, "解缠进度:%.4lf\n", double(i + 1) / phaseFiles.size() * 100.0);
//	//}
//	//return 0;
//	/*最小二乘法求解线性形变速率和高程残差*/
//
//	//首先确定矩阵B
//	int M = phaseFiles.size();//干涉图幅数
//	int N = filelist2.size() - 1;//时间序列数
//	Mat B(M, N, CV_64F); B = 0.0;
//	Mat one = Mat::ones(N, 1, CV_64F);
//	for (int i = 0; i < M; i++)
//	{
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\ifg\\%d_%d.h5", &ii, &jj);
//		for (int j = jj; j < ii; j++)
//		{
//			B.at<double>(i, j - 1) = (temporal.at<double>(0, j) - temporal.at<double>(0, j - 1)) / 365.0;
//		}
//	}
//	//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\B.bin", B);
//	Mat B1 = B * one;
//	//确定矩阵c
//	Mat c(M, 1, CV_64F); c = 0.0;
//	vector<Mat> phase_vec, phase_vec2, coh_vec;
//	phase_vec.resize(M); phase_vec2.resize(N + 1); coh_vec.resize(M);
//	int ref_i = 359, ref_j = 360;
//	int offset_col, row, col;
//	double nearRange, theta, spacing, wavelength, B_spatial, B_temporal;
//	for (int i = 0; i < M; i++)
//	{
//		Mat temp;
//		int ii, jj;
//		sscanf(phaseFiles[i].c_str(), "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\ifg\\%d_%d.h5", &ii, &jj);
//		memset(str, 0, 1024);
//		sprintf(str, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\ifg\\%d_%d.h5", ii, jj);
//		conversion.read_int_from_h5(str, "offset_col", &offset_col);
//		conversion.read_double_from_h5(str, "slant_range_first_pixel", &nearRange);
//		conversion.read_double_from_h5(str, "range_spacing", &spacing);
//		conversion.read_double_from_h5(str, "B_spatial", &B_spatial);
//		conversion.read_double_from_h5(str, "B_temporal", &B_temporal);
//		conversion.read_double_from_h5(str, "carrier_frequency", &wavelength);
//		wavelength = VEL_C / wavelength;
//		conversion.read_array_from_h5(str, "inc_coefficient", temp);
//		theta = temp.at<double>(0, 0) / 180.0 * PI;
//		double r = nearRange + double(offset_col) * spacing;
//		c.at<double>(i, 0) = 4 * PI / wavelength * B_spatial / sin(theta) / r;
//		conversion.read_array_from_h5(phaseFiles[i].c_str(), "coherence", coh_vec[i]);
//		memset(str, 0, 1024);
//		sprintf(str, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\ifg_unwrapped\\%d_%d.bin", ii, jj);
//		util.bin2cvmat(str, phase);
//		phase = phase - phase.at<double>(ref_i, ref_j);
//		phase.copyTo(phase_vec[i]);
//	}
//	Mat xssa = Mat::zeros(phase.rows, phase.cols, CV_64F);
//	for (int i = 0; i < N + 1; i++)
//	{
//		xssa.copyTo(phase_vec2[i]);
//	}
//
//	Mat BMc;
//	Mat v(phase.rows, phase.cols, CV_64F), z(phase.rows, phase.cols, CV_64F);
//	v = 0.0; z = 0.0; Mat temporal_coh(phase.rows, phase.cols, CV_64F); temporal_coh = 0.0;
//	cv::hconcat(B1, c, BMc);
//	//cout << c << endl;
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < phase.rows; i++)
//	{
//		Mat temp_coh(M, M, CV_64F); temp_coh = 0.0; double coh; Mat temp(M, 1, CV_64F); temp = 0.0;
//		for (int j = 0; j < phase.cols; j++)
//		{
//			if (mask.at<int>(i, j) > 0)
//			{
//				for (int k = 0; k < M; k++)
//				{
//					temp.at<double>(k, 0) = phase_vec[k].at<double>(i, j);
//					temp_coh.at<double>(k, k) = coh_vec[k].at<double>(i, j);
//				}
//				//最小二乘法求解
//				Mat A_t, A, b;
//				BMc.copyTo(A);
//				temp.copyTo(b);
//				//cout << A << endl;
//				transpose(A, A_t);
//				A = A_t * temp_coh * A;
//				b = A_t * temp_coh * b;
//				Mat x;
//				//cout << A << endl;
//				//cout << temp << endl;
//				if (!cv::solve(A, b, x, cv::DECOMP_LU))
//				{
//					fprintf(stderr, "_PS_deflat(): can't solve least square problem!\n");
//				}
//				else
//				{
//					//cout << x << endl;
//					//Mat velocity_vec(N, 1, CV_64F); velocity_vec = 0.0;
//					//Mat phi_vec(N + 1, 1, CV_64F); phi_vec = 0.0;
//					//for (int k = 1; k < N + 1; k++)
//					//{
//					//	phase_vec2[k].at<double>(i, j) = x.at<double>(k - 1, 0) * 
//					//		(temporal.at<double>(0, k) - temporal.at<double>(0, k - 1)) / 365.0
//					//		+ phase_vec2[k - 1].at<double>(i, j);
//					//}
//					v.at<double>(i, j) = x.at<double>(0, 0);
//					z.at<double>(i, j) = x.at<double>(1, 0);
//				}
//				//减去地形误差相位
//				//temp = temp - x.at<double>(1, 0) * c;
//				B.copyTo(A);
//				temp.copyTo(b);
//				cv::transpose(A, A_t);
//				A = A_t * temp_coh * A;
//				b = A_t * temp_coh * b;
//				if (!cv::solve(A, b, x, cv::DECOMP_SVD))
//				{
//					fprintf(stderr, "SBAS_time_series(): can't solve SVD!\n");
//				}
//				else
//				{
//					for (int k = 1; k < N + 1; k++)
//					{
//						phase_vec2[k].at<double>(i, j) = x.at<double>(k - 1, 0) *
//							(temporal.at<double>(0, k) - temporal.at<double>(0, k - 1)) / 365.0
//							+ phase_vec2[k - 1].at<double>(i, j);
//					}
//				}
//				//计算时间相关系数
//				x = B * x;
//				coh = 0.0;
//				sbas.compute_temporal_coherence(x, temp, &coh);
//				temporal_coh.at<double>(i, j) = coh;
//				//计算时间相关系数
//				//x = B * x;
//				//coh = 0.0;
//				//sbas.compute_temporal_coherence(x, temp, &coh);
//				//temporal_coh.at<double>(i, j) = coh;
//			}
//		}
//	}
//
//	for (int i = 0; i < N + 1; i++)
//	{
//		memset(str, 0, 1024);
//		sprintf(str, "D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\result\\series_%d.bin", i + 1);
//		util.cvmat2bin(str, phase_vec2[i]);
//	}
//	util.cvmat2bin("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\result\\temporal_coh.bin", temporal_coh);
//	util.cvmat2bin("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\result\\z.bin", z);
//	util.cvmat2bin("D:\\working_dir\\papers\\ESM_coherence_matrix\\realdata\\Sentinel\\TP\\result\\v.bin", v);
//
//	return 0;
//}


//航天宏图数据读取
//int main(int argc, char* argv[])
//{
//    FormatConversion conversion;
//    Utils util; ComplexMat slc;
//    const char* tiff_file = "D:\\data\\HTHT_data\\L1B_SLP\\HT1-D_HYKS1201_IM_SLP_1BSH_20240206T120917_E99.5_N38.0_004739_XOQ0_D\\HT1-D_HYKS1201_IM_SLP_1BHH_20240206T120917_E99.5_N38.0_004739_XOQ0_D.tiff";
//    const char* xmld_file = "D:\\data\\HTHT_data\\L1B_SLP\\HT1-D_HYKS1201_IM_SLP_1BSH_20240206T120917_E99.5_N38.0_004739_XOQ0_D\\HT1-D_HYKS1201_IM_SLP_1BSH_20240206T120917_E99.5_N38.0_004739_XOQ0_D.xml";
//    HTHT_reader reader(tiff_file, xmld_file);
//    reader.init();
//    reader.write_to_h5("D:\\data\\HTHT_data\\L1B_SLP\\HT1-D_HYKS1201_IM_SLP_1BSH_20240206T120917_E99.5_N38.0_004739_XOQ0_D\\HT1-D.h5");
//    conversion.read_slc_from_h5("D:\\data\\HTHT_data\\L1B_SLP\\HT1-D_HYKS1201_IM_SLP_1BSH_20240206T120917_E99.5_N38.0_004739_XOQ0_D\\HT1-D.h5", slc);
//    util.saveSLC("D:\\data\\HTHT_data\\L1B_SLP\\HT1-D_HYKS1201_IM_SLP_1BSH_20240206T120917_E99.5_N38.0_004739_XOQ0_D\\HT1-D.jpg", 50, slc);
//    return 0;
//}


/*航天宏图数据测试*/
//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Registration coregis; Deflat flat; Filter filter; Unwrap unwrap;
//    Utils util; ComplexMat slc1, slc2;
//	string master_file = "D:\\data\\HTHT_data\\HT1-A.h5";
//	string master_deramped_file = "D:\\data\\HTHT_data\\HT1-A_deramped.h5";
//	string slave_file = "D:\\data\\HTHT_data\\HT1-D.h5";
//	string slave_coregis_file = "D:\\data\\HTHT_data\\HT1-D_coregis.h5";
//	string slave_coregis_deramped_file = "D:\\data\\HTHT_data\\HT1-D_coregis_deramped.h5";
//	string demPath = "D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem";
//	//conversion.read_slc_from_h5(master_file.c_str(), slc1);
//	//conversion.read_slc_from_h5(slave_file.c_str(), slc2);
//
//
//	/*slc1 = slc1(cv::Range(0, 5000), cv::Range(0, 5000));
//	slc2 = slc2(cv::Range(0, 5000), cv::Range(0, 5000));
//	slc1.convertTo(slc1, CV_64F);
//	slc2.convertTo(slc2, CV_64F);
//	int move_r, move_c;
//	coregis.real_coherent(slc1, slc2, &move_r, &move_c);*/
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
//	/*Mat sate_pos1, sate_pos2, sate_vel1, sate_vel2;
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
//	conversion.creat_new_h5("D:\\data\\HTHT_data\\pos_vel2.h5");
//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos1", sate_pos1);
//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos2", sate_pos2);
//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_vel1", sate_vel1);
//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_vel2", sate_vel2);
//	return 0;*/
//
//
//	lonMin = 99.287040-0.05; lonMax = 99.607278+0.05;
//	latMin = 37.846180 - 0.05; latMax = 38.099384+0.05;
//
//	//ret = Utils::getSRTMDEM(demPath.c_str(), dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//	Mat mappedLon, mappedLat, mappedDem;
//	//fprintf(stdout, "dem mapping start!\n");
//	//ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, 0, 0, sceneHeight, sceneWidth,
//	//	prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 50);
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\mapped_dem.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLat", mappedLat);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLon", mappedLon);
//
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);
//	//mappedDem = 0.0;
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLat", mappedLat);
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLon", mappedLon);
//
//	//fprintf(stdout, "dem mapping finished!\n");
//	//conversion.read_slc_from_h5(master_deramped_file.c_str(), slc1);
//	//conversion.read_slc_from_h5(slave_coregis_deramped_file.c_str(), slc2);
//	//slc1.convertTo(slc1, CV_32F);
//	//slc2.convertTo(slc2, CV_32F);
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
//	//util.Multilook(slc1, slc2, 8, 4, phase);
//	//util.savephase("D:\\data\\HTHT_data\\phase_flatten_A_D.jpg", "jet", phase);
//	//Mat phase_filter;
//	////filter.Goldstein_filter(phase, phase_filter, 0.8, 128, 16);
//	//filter.slope_adaptive_filter(phase, phase_filter, 9, 9);
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5", "phase", phase_filter);
//	//util.savephase("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.jpg", "jet", phase_filter);
//	//return 0;
//	//解缠
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5", "phase", phase_filter);
//	Mat unwrapped_phase;
//	//unwrap.snaphu(phase_filter, unwrapped_phase, "D:\\data\\HTHT_data");
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5", "phase", unwrapped_phase);
//	//util.savephase("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.jpg", "jet", unwrapped_phase);
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5", "phase", unwrapped_phase);
//	//cv::resize(unwrapped_phase, unwrapped_phase, cv::Size(unwrapped_phase.cols * 8, unwrapped_phase.rows * 4));
//	//Mat unwrapped_phase_final(slc1.GetRows(), slc1.GetCols(), CV_64F);
//	//unwrapped_phase_final = 0.0;
//	//unwrapped_phase.copyTo(unwrapped_phase_final(cv::Range(0, unwrapped_phase.rows), cv::Range(0, unwrapped_phase.cols)));
//	//return 0;
//	//校正至绝对相位
//	//解算斜距
//	Mat sate_pos1, sate_pos2, sate_vel1, sate_vel2, R1, R2, phase_ref, phase_flat, R1_flat, R2_flat;
//	//flat.slantrange_compute(R1, sate_pos1, sate_vel1, mappedDem, mappedLat, mappedLon, master_deramped_file.c_str());
//	//flat.slantrange_compute(R2, sate_pos2, sate_vel2, mappedDem, mappedLat, mappedLon, slave_coregis_deramped_file.c_str());
//	//mappedDem = 0.0;
//	//flat.slantrange_compute(R1_flat, sate_pos1, sate_vel1, mappedDem, mappedLat, mappedLon, master_deramped_file.c_str());
//	//flat.slantrange_compute(R2_flat, sate_pos2, sate_vel2, mappedDem, mappedLat, mappedLon, slave_coregis_deramped_file.c_str());
//
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\pos_vel.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_pos1", sate_pos1);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_pos2", sate_pos2);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_vel1", sate_vel1);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_vel2", sate_vel2);
//
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\R.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R1", R1);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R2", R2);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R1_flat", R1_flat);
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R2_flat", R2_flat);
//
//	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);
//
//	//phase_ref.create(R1.rows, R1.cols, CV_64F); phase_ref = 0;
//	//phase_flat.create(R1.rows, R1.cols, CV_64F); phase_flat = 0;
//	//for (int i = 0; i < R1.rows; i++)
//	//{
//	//	for (int j = 0; j < R1.cols; j++)
//	//	{
//	//		double r = R2.at<double>(i, j) - R1.at<double>(i, j);
//	//		phase_ref.at<double>(i, j) = r / wavelength * 2 * PI;
//	//		r = R2_flat.at<double>(i, j) - R1_flat.at<double>(i, j);
//	//		phase_flat.at<double>(i, j) = r / wavelength * 2 * PI;
//	//	}
//	//}
//	//unwrapped_phase_final = unwrapped_phase_final + phase_flat;
//	//int model_ix;
//	//Mat delta_phi = (unwrapped_phase_final - phase_ref) / (2 * PI);
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\delta_phi_A_D.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\delta_phi_A_D.h5", "delta_phi", delta_phi);
//	//for (int i = 0; i < R1.rows; i++)
//	//{
//	//	for (int j = 0; j < R1.cols; j++)
//	//	{
//	//		delta_phi.at<double>(i, j) = round(delta_phi.at<double>(i, j));
//	//	}
//	//}
//	//delta_phi.convertTo(delta_phi, CV_32S);
//	//util.get_mode_index(delta_phi, &model_ix);
//	//unwrapped_phase_final = unwrapped_phase_final - double(model_ix) * 2 * PI;
//	//conversion.creat_new_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5");
//	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5", "phase_abs", unwrapped_phase_final);
//	//return 0;
//
//
//	//反演高程
////	conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5", "phase_abs", unwrapped_phase);
////	conversion.read_array_from_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos1", sate_pos1);
////	conversion.read_array_from_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos2", sate_pos2);
////	conversion.read_array_from_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_vel1", sate_vel1);
////	//offset_col = 0;
////	//sceneWidth = 5000;
////	//unwrapped_phase(Range(0, unwrapped_phase.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(unwrapped_phase);
////	//mappedDem(Range(0, mappedDem.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(mappedDem);
////	//mappedLat(Range(0, mappedLat.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(mappedLat);
////	//mappedLon(Range(0, mappedLon.rows), Range(offset_col, offset_col + sceneWidth)).copyTo(mappedLon);
////	double lambda = wavelength;
////	Mat R_M(1, sceneWidth, CV_64F);
////	for (int i = 0; i < sceneWidth; i++)
////	{
////		R_M.at<double>(0, i) =  nearRangeTime * VEL_C / 2 + rangeSpacing * double(i + offset_col);
////
////	}
////	Mat ones = Mat::ones(sceneHeight, 1, CV_64F);
////	R_M = ones * R_M;
////	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase / (2 * PI);
////	Mat Satellite_M_T_Position = sate_pos1;//主星发射位置
////	Mat Satellite_S_T_Position;
////	Satellite_S_T_Position = sate_pos1;//辅星发射位置
////	Mat Satellite_M_R_Position = sate_pos1;//主星接收位置
////	Mat Satellite_S_R_Position = sate_pos2;//辅星接收位置
////	Mat Satellite_M = (Satellite_M_R_Position + Satellite_M_T_Position) / 2;
////	Mat Vs = sate_vel1;
////	ones = Mat::ones(sceneHeight, sceneWidth, CV_64F);
////	Mat dem_x, dem_y, dem_z;
////	dem_x = Mat::zeros(sceneHeight, sceneWidth, CV_64F);
////	dem_x.copyTo(dem_y); dem_x.copyTo(dem_z);
////	for (int i = 0; i < sceneHeight; i++)
////	{
////		for (int j = 0; j < sceneWidth; j++)
////		{
////			Position pos;
////			util.ell2xyz(mappedLon.at<double>(i, j), mappedLat.at<double>(i, j), mappedDem.at<short>(i, j), pos);
////			dem_x.at<double>(i, j) = pos.x;
////			dem_y.at<double>(i, j) = pos.y;
////			dem_z.at<double>(i, j) = pos.z;
////		}
////	}
////	
////	Mat M_T, M_R, S_T, S_R;
////	Mat f1, f2, f3;
////	Mat det_Df, Df_ni11, Df_ni12, Df_ni13, Df_ni21, Df_ni22, Df_ni23;
////	Mat Df11, Df12, Df13, Df21, Df22, Df23, Df31, Df32, Df33, Df_ni31, Df_ni32, Df_ni33;
////	Mat delta_Rt1, delta_Rt2, delta_Rt3;
////	ones = Mat::ones(1, sceneWidth, CV_64F);
////	Mat temp_var, temp_var1;
////	Mat fd = Mat::zeros(1, sceneWidth, CV_64F);
////	int iters = 15;
////	for (int i = 0; i < iters; i++)
////	{
////		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - dem_x;
////		temp_var = temp_var.mul(temp_var);
////		temp_var.copyTo(M_T);
////		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - dem_y;
////		temp_var = temp_var.mul(temp_var);
////		M_T = M_T + temp_var;
////		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - dem_z;
////		temp_var = temp_var.mul(temp_var);
////		M_T = M_T + temp_var;
////		cv::sqrt(M_T, f1);
////		f1 = f1 * 2 - 2 * R_M;
////
////		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - dem_x;
////		temp_var = temp_var.mul(temp_var);
////		temp_var.copyTo(S_T);
////		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - dem_y;
////		temp_var = temp_var.mul(temp_var);
////		S_T = S_T + temp_var;
////		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - dem_z;
////		temp_var = temp_var.mul(temp_var);
////		S_T = S_T + temp_var;
////
////		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - dem_x;
////		temp_var = temp_var.mul(temp_var);
////		temp_var.copyTo(S_R);
////		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - dem_y;
////		temp_var = temp_var.mul(temp_var);
////		S_R = S_R + temp_var;
////		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - dem_z;
////		temp_var = temp_var.mul(temp_var);
////		S_R = S_R + temp_var;
////
////		cv::sqrt(S_T, f2);
////		cv::sqrt(S_R, temp_var);
////		f2 = f2 + temp_var - R_F;
////
////
////		temp_var = Vs(Range(0, Vs.rows), Range(0, 1)) * ones;
////		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(0, 1)) * ones - dem_x;
////		f3 = temp_var.mul(temp_var1);
////		temp_var = Vs(Range(0, Vs.rows), Range(1, 2)) * ones;
////		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(1, 2)) * ones - dem_y;
////		f3 = f3 + temp_var.mul(temp_var1);
////		temp_var = Vs(Range(0, Vs.rows), Range(2, 3)) * ones;
////		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(2, 3)) * ones - dem_z;
////		f3 = f3 + temp_var.mul(temp_var1);
////		ones = Mat::ones(sceneHeight, 1, CV_64F);
////		temp_var = ones * fd;
////		temp_var1 = R_M * lambda / 2.0;
////		f3 = f3 + temp_var.mul(temp_var1);
////
////		//Dff
////		//第一行：f(1)的x，y，z的导数
////		ones = Mat::ones(1, sceneWidth, CV_64F);
////		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - dem_x;
////		cv::sqrt(M_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df11 = temp_var.mul(temp_var1);
////
////		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(0, 1)) * ones - dem_x;
////		cv::sqrt(M_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df11 = Df11 + temp_var.mul(temp_var1);
////
////		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - dem_y;
////		cv::sqrt(M_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df12 = temp_var.mul(temp_var1);
////
////		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(1, 2)) * ones - dem_y;
////		cv::sqrt(M_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df12 = Df12 + temp_var.mul(temp_var1);
////
////		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - dem_z;
////		cv::sqrt(M_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df13 = temp_var.mul(temp_var1);
////
////		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(2, 3)) * ones - dem_z;
////		cv::sqrt(M_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df13 = Df13 + temp_var.mul(temp_var1);
////
////
////		//第二行：f(2)的x，y，z的导数
////		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - dem_x;
////		cv::sqrt(S_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df21 = temp_var.mul(temp_var1);
////
////		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - dem_x;
////		cv::sqrt(S_R, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df21 = Df21 + temp_var.mul(temp_var1);
////
////
////		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - dem_y;
////		cv::sqrt(S_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df22 = temp_var.mul(temp_var1);
////
////		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - dem_y;
////		cv::sqrt(S_R, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df22 = Df22 + temp_var.mul(temp_var1);
////
////
////		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - dem_z;
////		cv::sqrt(S_T, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df23 = temp_var.mul(temp_var1);
////
////		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - dem_z;
////		cv::sqrt(S_R, temp_var1);
////		temp_var1 = 1 / temp_var1;
////		temp_var1 = -temp_var1;
////		Df23 = Df23 + temp_var.mul(temp_var1);
////
////		//第三行：f(3)的x，y，z的导数
////		Df31 = -Vs(Range(0, Vs.rows), Range(0, 1)) * ones;
////		Df32 = -Vs(Range(0, Vs.rows), Range(1, 2)) * ones;
////		Df33 = -Vs(Range(0, Vs.rows), Range(2, 3)) * ones;
////
////		temp_var = Df11.mul(Df22);
////		temp_var = temp_var.mul(Df33);
////		temp_var.copyTo(det_Df);
////
////		temp_var = Df12.mul(Df23);
////		temp_var = temp_var.mul(Df31);
////		det_Df = det_Df + temp_var;
////
////		temp_var = Df13.mul(Df21);
////		temp_var = temp_var.mul(Df32);
////		det_Df = det_Df + temp_var;
////
////		temp_var = Df31.mul(Df22);
////		temp_var = temp_var.mul(Df13);
////		det_Df = det_Df - temp_var;
////
////		temp_var = Df32.mul(Df23);
////		temp_var = temp_var.mul(Df11);
////		det_Df = det_Df - temp_var;
////
////		temp_var = Df33.mul(Df21);
////		temp_var = temp_var.mul(Df12);
////		det_Df = det_Df - temp_var;
////
////		Df_ni11 = (Df22.mul(Df33) - Df32.mul(Df23)) / det_Df;
////		Df_ni12 = -(Df12.mul(Df33) - Df32.mul(Df13)) / det_Df;
////		Df_ni13 = (Df12.mul(Df23) - Df22.mul(Df13)) / det_Df;
////		delta_Rt1 = Df_ni11.mul(f1) + Df_ni12.mul(f2) + Df_ni13.mul(f3);
////
////
////		Df_ni21 = -(Df21.mul(Df33) - Df31.mul(Df23)) / det_Df;
////		Df_ni22 = (Df11.mul(Df33) - Df31.mul(Df13)) / det_Df;
////		Df_ni23 = -(Df11.mul(Df23) - Df21.mul(Df13)) / det_Df;
////		delta_Rt2 = Df_ni21.mul(f1) + Df_ni22.mul(f2) + Df_ni23.mul(f3);
////
////		Df_ni31 = (Df21.mul(Df32) - Df31.mul(Df22)) / det_Df;
////		Df_ni32 = -(Df11.mul(Df32) - Df31.mul(Df12)) / det_Df;
////		Df_ni33 = (Df22.mul(Df11) - Df21.mul(Df12)) / det_Df;
////		delta_Rt3 = Df_ni31.mul(f1) + Df_ni32.mul(f2) + Df_ni33.mul(f3);
////
////		dem_x = dem_x - delta_Rt1;
////		dem_y = dem_y - delta_Rt2;
////		dem_z = dem_z - delta_Rt3;
////		fprintf(stdout, "%d/%d\n", i + 1, iters);
////	}
////	delta_Rt1.release(); delta_Rt2.release(); delta_Rt3.release(); Df_ni31.release(); Df_ni32.release();
////	Df_ni33.release(); Df_ni21.release(); Df_ni22.release(); Df_ni23.release(); Df_ni11.release(); Df_ni12.release();
////	Df_ni13.release();
////	volatile bool parallel_flag = true;
////	dem.create(sceneHeight, sceneWidth, CV_64F);
//	Mat lon(sceneHeight, sceneWidth, CV_64F); Mat lat(sceneHeight, sceneWidth, CV_64F);
////#pragma omp parallel for schedule(guided) \
////	private(ret)
////	for (int i = 0; i < sceneHeight; i++)
////	{
////		Utils util;
////		for (int j = 0; j < sceneWidth; j++)
////		{
////			double lat_val, lon_val, h_val;
////			ret = Utils::xyz2ell(dem_x.at<double>(i, j), dem_y.at<double>(i, j), dem_z.at<double>(i, j), lat_val, lon_val, h_val);
////			dem.at<double>(i, j) = h_val;
////			lat.at<double>(i, j) = lat_val;
////			lon.at<double>(i, j) = lon_val;
////		}
////	}
////	conversion.creat_new_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5");
////	conversion.write_array_to_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "dem", dem);
////	conversion.write_array_to_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lat", lat);
////	conversion.write_array_to_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lon", lon);
//
//
//	//地理编码
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "dem", dem);
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lat", lat);
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lon", lon);
//	Mat dem_geocoded;
//	double lon_east, lon_west, lat_north, lat_south;
//	util.SAR2UTM(lon, lat, dem, dem_geocoded, 3, 1, &lon_east, &lon_west, &lat_north, &lat_south);
//	conversion.creat_new_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5");
//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "dem_geocoded", dem_geocoded);
//	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lon_east", lon_east);
//	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lon_west", lon_west);
//	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lat_north", lat_north);
//	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lat_south", lat_south);
//    return 0;
//}


////读取王鹏波老师仿真数据
//int main(int argc, char* argv[])
//{
//	FILE* fp = NULL;
//	const char* img_file = "D:\\working_dir\\projects\\航天宏图\\TomoSARImage\\TomoSARImage\\Output_bird_7_points_X_006\\tomo_X_1566_2.img";
//	fp = fopen(img_file, "rb");
//	long offset = 968, nr, nc;
//	fseek(fp, offset, SEEK_SET);
//	fread(&nr, sizeof(long), 1, fp);
//	fread(&nc, sizeof(long), 1, fp);
//	ComplexMat slc; Utils util;
//	slc.re.create(nr, nc, CV_32F);
//	slc.im.create(nr, nc, CV_32F);
//	offset = 2048;
//	float* pbuf = NULL;
//	pbuf = (float*)malloc(nr * nc * sizeof(float) * 2);
//	fseek(fp, offset, SEEK_SET);
//	fread(pbuf, sizeof(float), nr * nc * 2, fp);
//	size_t byte_offset = 0;
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			slc.re.at<float>(i, j) = pbuf[byte_offset];
//			byte_offset++;
//			slc.im.at<float>(i, j) = pbuf[byte_offset];
//			byte_offset++;
//		}
//	}
//	util.saveSLC("D:\\working_dir\\projects\\航天宏图\\TomoSARImage\\TomoSARImage\\Output_bird_7_points_X_006\\tomo_X_1566_2.jpg", 50, slc);
//
//	fclose(fp);
//	return 0;
//}


//天仪涪城一号数据读取与测试

//int main(int argc, char* argv[])
//{
//	FormatConversion conversion; Registration coregis; Deflat flat;
//    Utils util; ComplexMat slc;
//	//conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\test.h5");
//	conversion.write_zero_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\test.h5", "test", CV_32F, 10000, 10000);
//	return 0;
// //   const char* tiff_file = "D:\\data\\tmp\\4\\bc3-sm-slc-vv-20231109t153004-002117-000025-000845-01.tiff";
// //   const char* xmld_file = "D:\\data\\tmp\\4\\bc3-sm-slc-vv-20231109t153004-002117-000025-000845-01.xml";
// //   Spacety_reader reader(tiff_file, xmld_file);
// //   reader.init();
// //   reader.write_to_h5("D:\\data\\tmp\\4\\20231109.h5");
// //   conversion.read_slc_from_h5("D:\\data\\tmp\\4\\20231109.h5", slc);
// //   util.saveSLC("D:\\data\\tmp\\4\\20231109.jpg", 50, slc);
//
//	//return 0;
//
// 	double lonMax, lonMin, latMax, latMin, lon_upperleft, lat_upperleft, rangeSpacing, rangeSpacing2,
//		nearRangeTime, nearRangeTime2, wavelength, prf, prf2,
//		start, end, start2, end2, a0, a1, a2, b0, b1, b2;
//	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
//		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat;
//	int sceneHeight, sceneWidth, sceneHeight2, sceneWidth2, offset_row, offset_col, offset_row2, offset_col2;
//	Mat lon_coef, lat_coef, dem, statevec, rangePos, azimuthPos,
//		lon_coef2, lat_coef2, statevec2, rangePos2, azimuthPos2, slaveRangeOffset, slaveAzimuthOffset;
//	string start_time, end_time;
//	ComplexMat master, slave;
//
//	const char* slave_file = "D:\\data\\tmp\\satexplorer\\spotlight_test3\\import2\\20240304.h5";
//	const char* master_file = "D:\\data\\tmp\\satexplorer\\spotlight_test3\\import2\\20240222.h5";
//	const char* slave_coregis_file = "D:\\data\\tmp\\satexplorer\\spotlight_test3\\coregis2\\20240304_regis.h5";
//	const char* master_deramped_file = "D:\\data\\tmp\\satexplorer\\spotlight_test3\\deramped2\\20240222_regis_deramp.h5";
//	const char* slave_deramped_file = "D:\\data\\tmp\\satexplorer\\spotlight_test3\\deramped2\\20240304_regis_deramp.h5";
//	const char* mapped_demlonlat_file = "D:\\data\\tmp\\satexplorer\\spotlight_test3\\deramped2\\mapped_demlonlat.h5";
//
//	conversion.read_int_from_h5(master_file, "range_len", &sceneWidth);
//	conversion.read_int_from_h5(master_file, "azimuth_len", &sceneHeight);
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
//	conversion.read_double_from_h5(master_file, "topLeftLon", &topleft_lon);
//	conversion.read_double_from_h5(master_file, "topLeftLat", &topleft_lat);
//	conversion.read_double_from_h5(master_file, "topRightLon", &topright_lon);
//	conversion.read_double_from_h5(master_file, "topRightLat", &topright_lat);
//	conversion.read_double_from_h5(master_file, "bottomLeftLon", &bottomleft_lon);
//	conversion.read_double_from_h5(master_file, "bottomLeftLat", &bottomleft_lat);
//	conversion.read_double_from_h5(master_file, "bottomRightLon", &bottomright_lon);
//	conversion.read_double_from_h5(master_file, "bottomRightLat", &bottomright_lat);
//	Utils::computeImageGeoBoundry(topleft_lon, topleft_lat, topright_lon, topright_lat, bottomleft_lon, bottomleft_lat, bottomright_lon, bottomright_lat,
//		&lonMax, &latMax, &lonMin, &latMin);
//	Utils::getSRTMDEM("D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem", dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
//
//	//ComplexMat mapped_slc;
//	//double lon_east, lon_west, lat_north, lat_south;
//	//util.geocode(dem, master, 3, 3, mapped_slc, lon_upperleft, lat_upperleft, 0, 0, sceneHeight, sceneWidth, prf, rangeSpacing, wavelength,
//	//	nearRangeTime, start, end, statevec, 5.0 / 6000.0, 5.0 / 6000.0, &lon_east, &lon_west, &lat_north, &lat_south);
//
//	//util.saveSLC("D:\\data\\tmp\\4\\20231109_geocoded.jpg", 50, mapped_slc);
//
//	//conversion.creat_new_h5("D:\\data\\tmp\\4\\corner_lonlat.h5");
//	//conversion.write_double_to_h5("D:\\data\\tmp\\4\\corner_lonlat.h5", "lon_east", lon_east);
//	//conversion.write_double_to_h5("D:\\data\\tmp\\4\\corner_lonlat.h5", "lon_west", lon_west);
//	//conversion.write_double_to_h5("D:\\data\\tmp\\4\\corner_lonlat.h5", "lat_north", lat_north);
//	//conversion.write_double_to_h5("D:\\data\\tmp\\4\\corner_lonlat.h5", "lat_south", lat_south);
//	//return 0;
//
//	Mat mappedDEM, mappedLat, mappedLon;
//	flat.demMapping(dem, mappedDEM, mappedLat, mappedLon, lon_upperleft, lat_upperleft, 0, 0, 
//		sceneHeight, sceneWidth, prf, rangeSpacing, wavelength, nearRangeTime, start,end, statevec, 50);
//	conversion.creat_new_h5(mapped_demlonlat_file);
//	conversion.write_array_to_h5(mapped_demlonlat_file, "mappedDem", mappedDEM);
//	conversion.write_array_to_h5(mapped_demlonlat_file, "mappedLon", mappedLon);
//	conversion.write_array_to_h5(mapped_demlonlat_file, "mappedLat", mappedLat);
//	return 0;
//
//	flat.SLC_deramp(master, mappedDEM, mappedLat, mappedLon, master_file);
//	
//	conversion.creat_new_h5(master_deramped_file);
//	conversion.write_int_to_h5(master_deramped_file, "range_len", sceneWidth);
//	conversion.write_int_to_h5(master_deramped_file, "azimuth_len", sceneHeight);
//
//	conversion.write_double_to_h5(master_deramped_file, "prf", prf);
//	wavelength = VEL_C / wavelength;
//	conversion.write_double_to_h5(master_deramped_file, "carrier_frequency", wavelength);
//	
//	conversion.write_double_to_h5(master_deramped_file, "range_spacing", rangeSpacing);
//	nearRangeTime = nearRangeTime * VEL_C / 2.0;
//	conversion.write_double_to_h5(master_deramped_file, "slant_range_first_pixel", nearRangeTime);
//	conversion.write_str_to_h5(master_deramped_file, "acquisition_start_time", start_time.c_str());
//	conversion.write_str_to_h5(master_deramped_file, "acquisition_stop_time", end_time.c_str());
//	conversion.write_array_to_h5(master_deramped_file, "state_vec", statevec);
//	conversion.write_slc_to_h5(master_deramped_file, master);
//
//	int offset_r, offset_c;
//	offset_row2 = offset_col2 = 0;
//	conversion.read_int_from_h5(slave_coregis_file, "range_len", &sceneWidth2);
//	conversion.read_int_from_h5(slave_coregis_file, "azimuth_len", &sceneHeight2);
//	conversion.read_double_from_h5(slave_coregis_file, "prf", &prf2);
//	conversion.read_double_from_h5(slave_coregis_file, "range_spacing", &rangeSpacing2);
//	conversion.read_double_from_h5(slave_coregis_file, "slant_range_first_pixel", &nearRangeTime2);
//	conversion.read_str_from_h5(slave_coregis_file, "acquisition_start_time", start_time);
//	conversion.utc2gps(start_time.c_str(), &start2);
//	conversion.read_str_from_h5(slave_coregis_file, "acquisition_stop_time", end_time);
//	conversion.utc2gps(end_time.c_str(), &end2);
//	conversion.read_array_from_h5(slave_coregis_file, "state_vec", statevec2);
//	conversion.read_slc_from_h5(slave_coregis_file, slave);
//
//
//	//master.convertTo(master, CV_64F);
//	//slave.convertTo(slave, CV_64F);
//	//coregis.coregistration_subpixel(master, slave, 512, 8);
//
//	
//	//conversion.creat_new_h5(slave_coregis_file);
//	//conversion.write_int_to_h5(slave_coregis_file, "range_len", sceneWidth2);
//	//conversion.write_int_to_h5(slave_coregis_file, "azimuth_len", sceneHeight2);
//
//	//conversion.write_double_to_h5(slave_coregis_file, "prf", prf2);
//	//conversion.write_double_to_h5(slave_coregis_file, "carrier_frequency", wavelength);
//
//	//conversion.write_double_to_h5(slave_coregis_file, "range_spacing", rangeSpacing2);
//	//conversion.write_double_to_h5(slave_coregis_file, "slant_range_first_pixel", nearRangeTime2);
//	//conversion.write_str_to_h5(slave_coregis_file, "acquisition_start_time", start_time.c_str());
//	//conversion.write_str_to_h5(slave_coregis_file, "acquisition_stop_time", end_time.c_str());
//	//conversion.write_array_to_h5(slave_coregis_file, "state_vec", statevec2);
//	//conversion.write_slc_to_h5(slave_coregis_file, slave);
//
//	flat.SLC_deramp(slave, mappedDEM, mappedLat, mappedLon, slave_coregis_file);
//	
//	conversion.creat_new_h5(slave_deramped_file);
//	conversion.write_int_to_h5(slave_deramped_file, "range_len", sceneWidth2);
//	conversion.write_int_to_h5(slave_deramped_file, "azimuth_len", sceneHeight2);
//	conversion.write_double_to_h5(slave_deramped_file, "prf", prf2);
//	conversion.write_double_to_h5(slave_deramped_file, "carrier_frequency", wavelength);
//	conversion.write_double_to_h5(slave_deramped_file, "range_spacing", rangeSpacing2);
//	conversion.write_double_to_h5(slave_deramped_file, "slant_range_first_pixel", nearRangeTime2);
//	conversion.write_str_to_h5(slave_deramped_file, "acquisition_start_time", start_time.c_str());
//	conversion.write_str_to_h5(slave_deramped_file, "acquisition_stop_time", end_time.c_str());
//	conversion.write_array_to_h5(slave_deramped_file, "state_vec", statevec2);
//	conversion.write_slc_to_h5(slave_deramped_file, slave);
//
//	//conversion.read_slc_from_h5(master_file, master);
//	//conversion.read_slc_from_h5(slave_coregis_file, slave);
//	master.convertTo(master, CV_64F);
//	slave.convertTo(slave, CV_64F);
//
//	//util.saveSLC("D:\\data\\tmp\\slave1.jpg", 50, slave);
//	
//
//	Mat phase;
//	util.Multilook(master, slave, 4, 4, phase);
//	util.savephase("D:\\data\\tmp\\satexplorer\\spotlight_test\\deramped3\\phase_deramped.jpg", "jet", phase);
//
//    return 0;
//}


/*航天宏图森林数据测试*/
int main(int argc, char* argv[])
{
	FormatConversion conversion; Registration coregis; Deflat flat; Filter filter; Unwrap unwrap;
	Utils util; ComplexMat slc1, slc2;


	string master_file = "D:\\data\\tomosense\\tomosense\\coregis\\HT1-A-20240716_regis.h5";
	string master_deramped_file = "D:\\data\\HTHT_data\\HT1-A_deramped.h5";
	string slave_file = "D:\\data\\HTHT_data\\HT1-D.h5";
	string slave_coregis_file = "D:\\data\\tomosense\\tomosense\\coregis\\HT1-B-20240716_regis.h5";
	string slave_coregis_deramped_file = "D:\\data\\HTHT_data\\HT1-D_coregis_deramped.h5";
	string demPath = "D:\\working_dir\\projects\\software\\InSAR_UI\\bin\\dem";
	//conversion.read_slc_from_h5(master_file.c_str(), slc1);
	//conversion.read_slc_from_h5(slave_file.c_str(), slc2);


	/*slc1 = slc1(cv::Range(0, 5000), cv::Range(0, 5000));
	slc2 = slc2(cv::Range(0, 5000), cv::Range(0, 5000));
	slc1.convertTo(slc1, CV_64F);
	slc2.convertTo(slc2, CV_64F);
	int move_r, move_c;
	coregis.real_coherent(slc1, slc2, &move_r, &move_c);*/



	/*slc1.convertTo(slc1, CV_32F);
	slc2.convertTo(slc2, CV_32F);
	Mat phase, coherence;
	util.Multilook(slc1, slc2, 4, 2, phase);
	util.phase_coherence(phase, 5, 5, coherence);
	conversion.creat_new_h5("D:\\data\\HTHT_data\\coherence_A_D.h5");
	conversion.write_array_to_h5("D:\\data\\HTHT_data\\coherence_A_D.h5", "jet", coherence);
	util.savephase("D:\\data\\HTHT_data\\coherence_A_D.jpg", "jet", coherence); return 0;*/
	/*coregis.coregistration_subpixel(slc1, slc2, 512, 8);
	fprintf(stdout, "coregistration finished!\n");
	conversion.creat_new_h5(slave_coregis_file.c_str());
	conversion.write_slc_to_h5(slave_coregis_file.c_str(), slc2);
	conversion.write_int_to_h5(slave_coregis_file.c_str(), "range_len", slc2.GetCols());
	conversion.write_int_to_h5(slave_coregis_file.c_str(), "azimuth_len", slc2.GetRows());
	conversion.Copy_para_from_h5_2_h5(slave_file.c_str(), slave_coregis_file.c_str());
	slc1.convertTo(slc1, CV_32F);
	slc2.convertTo(slc2, CV_32F);
	util.Multilook(slc1, slc2, 4, 2, phase);
	util.savephase("D:\\data\\HTHT_data\\phase_A_D.jpg", "jet", phase);*/


	int ret, sceneWidth, sceneHeight, offset_col = 0, offset_row = 0;
	double prf, wavelength, rangeSpacing, nearRangeTime, start, end, lon_upperleft, lat_upperleft, lonMin, lonMax, latMin, latMax;
	string start_time, end_time;
	Mat statevec, dem;
	ret = conversion.read_int_from_h5(master_file.c_str(), "range_len", &sceneWidth);
	ret = conversion.read_int_from_h5(master_file.c_str(), "azimuth_len", &sceneHeight);

	ret = conversion.read_double_from_h5(master_file.c_str(), "prf", &prf);
	ret = conversion.read_double_from_h5(master_file.c_str(), "carrier_frequency", &wavelength);
	wavelength = VEL_C / wavelength;
	ret = conversion.read_double_from_h5(master_file.c_str(), "range_spacing", &rangeSpacing);
	ret = conversion.read_double_from_h5(master_file.c_str(), "slant_range_first_pixel", &nearRangeTime);
	nearRangeTime = 2.0 * nearRangeTime / VEL_C;
	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(master_file.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(master_file.c_str(), "state_vec", statevec);

	//计算主图像轨道参数
	/*Mat sate_pos1, sate_pos2, sate_vel1, sate_vel2;
	sate_pos1.create(sceneHeight, 3, CV_64F);
	sate_vel1.create(sceneHeight, 3, CV_64F);
	sate_pos2.create(sceneHeight, 3, CV_64F);
	sate_vel2.create(sceneHeight, 3, CV_64F);
	Position pos; Velocity vel;
	orbitStateVectors stateVectors(statevec, start, end);
	stateVectors.applyOrbit();
	for (int i = 0; i < sceneHeight; i++)
	{
		double time = start + (double)i * (1.0 / prf);
		stateVectors.getPosition(time, pos);
		stateVectors.getVelocity(time, vel);
		sate_pos1.at<double>(i, 0) = pos.x;
		sate_pos1.at<double>(i, 1) = pos.y;
		sate_pos1.at<double>(i, 2) = pos.z;
		sate_vel1.at<double>(i, 0) = vel.vx;
		sate_vel1.at<double>(i, 1) = vel.vy;
		sate_vel1.at<double>(i, 2) = vel.vz;
	}
	ret = conversion.read_str_from_h5(slave_file.c_str(), "acquisition_start_time", start_time);
	ret = conversion.utc2gps(start_time.c_str(), &start);
	ret = conversion.read_str_from_h5(slave_file.c_str(), "acquisition_stop_time", end_time);
	ret = conversion.utc2gps(end_time.c_str(), &end);
	ret = conversion.read_array_from_h5(slave_file.c_str(), "state_vec", statevec);
	orbitStateVectors stateVectors2(statevec, start, end);
	stateVectors2.applyOrbit();
	for (int i = 0; i < sceneHeight; i++)
	{
		double time = start + (double)(i + move_r) * (1.0 / prf);
		stateVectors2.getPosition(time, pos);
		stateVectors2.getVelocity(time, vel);
		sate_pos2.at<double>(i, 0) = pos.x;
		sate_pos2.at<double>(i, 1) = pos.y;
		sate_pos2.at<double>(i, 2) = pos.z;
		sate_vel2.at<double>(i, 0) = vel.vx;
		sate_vel2.at<double>(i, 1) = vel.vy;
		sate_vel2.at<double>(i, 2) = vel.vz;
	}
	conversion.creat_new_h5("D:\\data\\HTHT_data\\pos_vel2.h5");
	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos1", sate_pos1);
	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos2", sate_pos2);
	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_vel1", sate_vel1);
	conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_vel2", sate_vel2);
	return 0;*/

	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon,
		topleft_lat, topright_lat, bottomleft_lat, bottomright_lat;
	int ret2 = 0;
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topLeftLon", &topleft_lon);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topLeftLat", &topleft_lat);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topRightLon", &topright_lon);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "topRightLat", &topright_lat);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomLeftLon", &bottomleft_lon);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomLeftLat", &bottomleft_lat);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomRightLon", &bottomright_lon);
	ret2 += conversion.read_double_from_h5(master_file.c_str(), "bottomRightLat", &bottomright_lat);
	if (ret2 == 0)
	{
		Utils::computeImageGeoBoundry(topleft_lon, topleft_lat, topright_lon, topright_lat, bottomleft_lon, bottomleft_lat, bottomright_lon, bottomright_lat,
			&lonMax, &latMax, &lonMin, &latMin);
	}

	ret = Utils::getSRTMDEM(demPath.c_str(), dem, &lon_upperleft, &lat_upperleft, lonMin, lonMax, latMin, latMax);
	Mat mappedLon, mappedLat, mappedDem;
	//fprintf(stdout, "dem mapping start!\n");
	ret = flat.demMapping(dem, mappedDem, mappedLat, mappedLon, lon_upperleft, lat_upperleft, 0, 0, sceneHeight, sceneWidth,
		prf, rangeSpacing, wavelength, nearRangeTime, start, end, statevec, 50);
	conversion.creat_new_h5("D:\\data\\hongtu_new\\tomosense\\tomosense\\coregis_coh\\mapped_dem.h5");
	conversion.write_array_to_h5("D:\\data\\hongtu_new\\tomosense\\tomosense\\coregis_coh\\mapped_dem.h5", "mappedDem", mappedDem);
	conversion.write_array_to_h5("D:\\data\\hongtu_new\\tomosense\\tomosense\\coregis_coh\\mapped_dem.h5", "mappedLat", mappedLat);
	conversion.write_array_to_h5("D:\\data\\hongtu_new\\tomosense\\tomosense\\coregis_coh\\mapped_dem.h5", "mappedLon", mappedLon);
	return 0;
	conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);
	//mappedDem = 0.0;
	conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLat", mappedLat);
	conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedLon", mappedLon);

	//fprintf(stdout, "dem mapping finished!\n");
	//conversion.read_slc_from_h5(master_deramped_file.c_str(), slc1);
	//conversion.read_slc_from_h5(slave_coregis_deramped_file.c_str(), slc2);
	//slc1.convertTo(slc1, CV_32F);
	//slc2.convertTo(slc2, CV_32F);
	//fprintf(stdout, "deramping start!\n");
	//flat.SLC_deramp(slc1, mappedDem, mappedLat, mappedLon, master_file.c_str(), 2);
	//fprintf(stdout, "master file deramping finished!\n");
	//flat.SLC_deramp(slc2, mappedDem, mappedLat, mappedLon, slave_coregis_file.c_str(), 2);
	//fprintf(stdout, "slave file deramping finished!\n");
	//conversion.creat_new_h5(master_deramped_file.c_str());
	//conversion.write_slc_to_h5(master_deramped_file.c_str(), slc1);
	//conversion.write_int_to_h5(master_deramped_file.c_str(), "range_len", slc1.GetCols());
	//conversion.write_int_to_h5(master_deramped_file.c_str(), "azimuth_len", slc1.GetRows());
	//conversion.Copy_para_from_h5_2_h5(master_file.c_str(), master_deramped_file.c_str());

	//conversion.creat_new_h5(slave_coregis_deramped_file.c_str());
	//conversion.write_slc_to_h5(slave_coregis_deramped_file.c_str(), slc2);
	//conversion.write_int_to_h5(slave_coregis_deramped_file.c_str(), "range_len", slc2.GetCols());
	//conversion.write_int_to_h5(slave_coregis_deramped_file.c_str(), "azimuth_len", slc2.GetRows());
	//conversion.Copy_para_from_h5_2_h5(slave_coregis_file.c_str(), slave_coregis_deramped_file.c_str());

	//util.Multilook(slc1, slc2, 8, 4, phase);
	//util.savephase("D:\\data\\HTHT_data\\phase_flatten_A_D.jpg", "jet", phase);
	//Mat phase_filter;
	////filter.Goldstein_filter(phase, phase_filter, 0.8, 128, 16);
	//filter.slope_adaptive_filter(phase, phase_filter, 9, 9);
	//conversion.creat_new_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5");
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5", "phase", phase_filter);
	//util.savephase("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.jpg", "jet", phase_filter);
	//return 0;
	//解缠
	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_A_D.h5", "phase", phase_filter);
	Mat unwrapped_phase;
	//unwrap.snaphu(phase_filter, unwrapped_phase, "D:\\data\\HTHT_data");
	//conversion.creat_new_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5");
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5", "phase", unwrapped_phase);
	//util.savephase("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.jpg", "jet", unwrapped_phase);
	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_unwrapped_A_D.h5", "phase", unwrapped_phase);
	//cv::resize(unwrapped_phase, unwrapped_phase, cv::Size(unwrapped_phase.cols * 8, unwrapped_phase.rows * 4));
	//Mat unwrapped_phase_final(slc1.GetRows(), slc1.GetCols(), CV_64F);
	//unwrapped_phase_final = 0.0;
	//unwrapped_phase.copyTo(unwrapped_phase_final(cv::Range(0, unwrapped_phase.rows), cv::Range(0, unwrapped_phase.cols)));
	//return 0;
	//校正至绝对相位
	//解算斜距
	Mat sate_pos1, sate_pos2, sate_vel1, sate_vel2, R1, R2, phase_ref, phase_flat, R1_flat, R2_flat;
	//flat.slantrange_compute(R1, sate_pos1, sate_vel1, mappedDem, mappedLat, mappedLon, master_deramped_file.c_str());
	//flat.slantrange_compute(R2, sate_pos2, sate_vel2, mappedDem, mappedLat, mappedLon, slave_coregis_deramped_file.c_str());
	//mappedDem = 0.0;
	//flat.slantrange_compute(R1_flat, sate_pos1, sate_vel1, mappedDem, mappedLat, mappedLon, master_deramped_file.c_str());
	//flat.slantrange_compute(R2_flat, sate_pos2, sate_vel2, mappedDem, mappedLat, mappedLon, slave_coregis_deramped_file.c_str());

	//conversion.creat_new_h5("D:\\data\\HTHT_data\\pos_vel.h5");
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_pos1", sate_pos1);
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_pos2", sate_pos2);
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_vel1", sate_vel1);
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\pos_vel.h5", "sate_vel2", sate_vel2);

	//conversion.creat_new_h5("D:\\data\\HTHT_data\\R.h5");
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R1", R1);
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R2", R2);
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R1_flat", R1_flat);
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\R.h5", "R2_flat", R2_flat);

	//conversion.read_array_from_h5("D:\\data\\HTHT_data\\mapped_dem.h5", "mappedDem", mappedDem);

	//phase_ref.create(R1.rows, R1.cols, CV_64F); phase_ref = 0;
	//phase_flat.create(R1.rows, R1.cols, CV_64F); phase_flat = 0;
	//for (int i = 0; i < R1.rows; i++)
	//{
	//	for (int j = 0; j < R1.cols; j++)
	//	{
	//		double r = R2.at<double>(i, j) - R1.at<double>(i, j);
	//		phase_ref.at<double>(i, j) = r / wavelength * 2 * PI;
	//		r = R2_flat.at<double>(i, j) - R1_flat.at<double>(i, j);
	//		phase_flat.at<double>(i, j) = r / wavelength * 2 * PI;
	//	}
	//}
	//unwrapped_phase_final = unwrapped_phase_final + phase_flat;
	//int model_ix;
	//Mat delta_phi = (unwrapped_phase_final - phase_ref) / (2 * PI);
	//conversion.creat_new_h5("D:\\data\\HTHT_data\\delta_phi_A_D.h5");
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\delta_phi_A_D.h5", "delta_phi", delta_phi);
	//for (int i = 0; i < R1.rows; i++)
	//{
	//	for (int j = 0; j < R1.cols; j++)
	//	{
	//		delta_phi.at<double>(i, j) = round(delta_phi.at<double>(i, j));
	//	}
	//}
	//delta_phi.convertTo(delta_phi, CV_32S);
	//util.get_mode_index(delta_phi, &model_ix);
	//unwrapped_phase_final = unwrapped_phase_final - double(model_ix) * 2 * PI;
	//conversion.creat_new_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5");
	//conversion.write_array_to_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5", "phase_abs", unwrapped_phase_final);
	//return 0;


	//反演高程
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\phase_flatten_filter_phase_abs_A_D.h5", "phase_abs", unwrapped_phase);
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos1", sate_pos1);
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_pos2", sate_pos2);
//	conversion.read_array_from_h5("D:\\data\\HTHT_data\\pos_vel2.h5", "sate_vel1", sate_vel1);
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
//	Mat R_F = R_M * 2.0 + lambda * unwrapped_phase / (2 * PI);
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
	Mat lon(sceneHeight, sceneWidth, CV_64F); Mat lat(sceneHeight, sceneWidth, CV_64F);
	//#pragma omp parallel for schedule(guided) \
	//	private(ret)
	//	for (int i = 0; i < sceneHeight; i++)
	//	{
	//		Utils util;
	//		for (int j = 0; j < sceneWidth; j++)
	//		{
	//			double lat_val, lon_val, h_val;
	//			ret = Utils::xyz2ell(dem_x.at<double>(i, j), dem_y.at<double>(i, j), dem_z.at<double>(i, j), lat_val, lon_val, h_val);
	//			dem.at<double>(i, j) = h_val;
	//			lat.at<double>(i, j) = lat_val;
	//			lon.at<double>(i, j) = lon_val;
	//		}
	//	}
	//	conversion.creat_new_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5");
	//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "dem", dem);
	//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lat", lat);
	//	conversion.write_array_to_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lon", lon);


		//地理编码
	conversion.read_array_from_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "dem", dem);
	conversion.read_array_from_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lat", lat);
	conversion.read_array_from_h5("D:\\data\\HTHT_data\\inversion_result_A_D2.h5", "lon", lon);
	Mat dem_geocoded;
	double lon_east, lon_west, lat_north, lat_south;
	util.SAR2UTM(lon, lat, dem, dem_geocoded, 3, 1, &lon_east, &lon_west, &lat_north, &lat_south);
	conversion.creat_new_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5");
	conversion.write_array_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "dem_geocoded", dem_geocoded);
	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lon_east", lon_east);
	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lon_west", lon_west);
	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lat_north", lat_north);
	conversion.write_double_to_h5("D:\\data\\HTHT_data\\geocoded_dem_A_D2.h5", "lat_south", lat_south);
	return 0;
}