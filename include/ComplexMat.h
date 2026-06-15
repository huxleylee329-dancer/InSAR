#pragma once
#include"..\include\Package.h"
#include<complex>

#pragma warning(push)
#pragma warning(disable: 4251)

using cv::Mat;
using namespace std;
class InSAR_API ComplexMat
{
public:
	ComplexMat();
	ComplexMat(Mat& real, Mat& imagine);
	ComplexMat(int rows, int cols);
	/// @brief 拷贝构造函数
	ComplexMat(const ComplexMat& b);
	~ComplexMat();
	void SetRe(Mat& re);
	void SetIm(Mat& im);
	Mat GetRe() const;
	Mat GetIm() const;
	Mat GetMod() const;
	/// @brief 计算复矩阵的相位
	Mat GetPhase() const;
	/// @brief 释放数据占用内存
	void release();
	int type() const;
	int GetRows() const;
	int GetCols() const;
	/// @brief 计算复矩阵(共轭)乘法
	int mul(const ComplexMat& Src, ComplexMat& Dst, bool bConj = false);
	/// @brief 计算复数（共轭）点乘
	int Mul(const ComplexMat& Src, ComplexMat& Dst, bool bConj) const;
	/// @brief 计算复数乘积(点乘,elementwise)
	ComplexMat operator*(const ComplexMat& b) const;
	/// @brief 复数矩阵与实数矩阵对应相乘
	ComplexMat operator*(const Mat& a) const;
	/// @brief 复数矩阵乘以常数
	ComplexMat operator*(const double& a) const;
	/// @brief 取出部分复数矩阵
	ComplexMat operator()(cv::Range _rowRange, cv::Range _colRange) const;
	/// @brief 将复数矩阵部分进行赋值
	int SetValue(cv::Range _rowRange, cv::Range _colRange, ComplexMat& src);
	/// @brief 复数矩阵加法
	ComplexMat operator+(const ComplexMat& b) const;
	/// @brief 深拷贝赋值
	ComplexMat& operator=(const ComplexMat&);
	/// @brief 复数矩阵内求和
	/// @param dim 求和方向（0为沿着每列求和，1为沿着每行求和）
	ComplexMat sum(int dim = 0) const;
	/// @brief 求取复矩阵行列式
	complex<double> determinant() const;
	/// @brief 求取复共轭
	ComplexMat conj() const;
	/// @brief 求取(共轭)转置
	ComplexMat transpose(bool conj=true) const;
	/// @brief MATLAB式reshape函数
	int reshape(int rows, int cols, ComplexMat& dst);
	/// @brief 计算非零元素个数
	int countNonzero() const;
	/// @brief 数组是否为空
	bool isEmpty()const;
	/// @brief 转换类型
	void convertTo(ComplexMat& out, int type) const;
	/// @brief 拷贝复制
	ComplexMat clone() const;
	Mat re;
	Mat im;
private:
};


#pragma warning(pop)


