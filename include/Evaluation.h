#pragma once
#ifndef __EVALUATION_H__
#define __EVALUATION_H__
#include<Package.h>
#include<ComplexMat.h>
#include<Utils.h>
using namespace cv;

class InSAR_API Evaluation
{
public:
	Evaluation();
	~Evaluation();
	/*@brief 干涉相位保相性
	* @param master_h5				   主图像h5文件
	* @param slave_h5				   辅图像h5文件
	* @param Output                    输出结果（rad）
	* @return 成功返回0，否则返回-1
	*/
	int PhasePreserve(const char* master_h5,
		const char* slave_h5,
		double* Output);
	/*@brief 配准评估
	* @param master_h5				   主图像h5文件
	* @param slave_regis_h5			   配准后辅图像h5文件
	* @param coherence				   相关系数矩阵
	* @param regis_error               配准误差矩阵（左方位向右距离向）
	* @return 成功返回0，否则返回-1
	*/
	int Regis(const char* master_h5,
		const char* slave_regis_h5,
		Mat& coherence,
		Mat& regis_error);
	/*@brief 干涉相位保相性
	* @param master_h5				   解缠后相位h5文件
	* @param slave_h5				   解缠前相位h5文件
	* @param Output                    输出结果（rad）
	* @return 成功返回0，否则返回-1
	*/
	int Unwrap(const char* master_h5, 
		const char* slave_regis_h5, 
		const char* phase_unwrapped_h5, double* Output);
	/*@brief 绝对定位精度评估
	* @param unwrapped_phase_file      解缠相位h5文件
	* @param project_path              工程目录
	* @param GCP_path                  GCP控制点h5文件
	* @param lat_abs                   输出纬度绝对误差（度）
	* @param lat_rel                   输出纬度相对误差（度）
	* @param lon_abs                   输出经度绝对误差（度）
	* @param lon_rel                   输出经度相对误差（度）
	* @param height_abs                输出高程绝对误差（米）
	* @param height_rel                输出高程相对误差（米）
	* @param cb                        进度回调函数
	* @return 成功返回0，否则返回-1
	*/
	int Pos(const char* unwrapped_phase_file, const char* project_path, const char* GCP_path,
		double* lat_abs, double* lat_rel,
		double* lon_abs, double* lon_rel,
		double* height_abs, double* height_rel,
		NewtonProgressCallback cb = nullptr);
private:
	int FFT2(ComplexMat src, ComplexMat& dst, int win_size, int interp_times);
	char error_head[256];
	char parallel_error_head[256];
};
#endif