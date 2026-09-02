#include<complex.h>
#include "stdafx.h"
#include"..\include\Utils.h"
#include <mutex>
#include <atomic>
#include <vector>
#include <cfloat>
#include <cmath>
#include <omp.h>
#include<direct.h>
#include<SensAPI.h>
#include<urlmon.h>
#include<Windows.h>
#include<tchar.h>
#include <atlconv.h>
#include"gdal_priv.h"
#include"gdal.h"
#include"../include/Hdf5IO.h"
#include"../include/tinyxml.h"
#include"Eigen/Dense"

#pragma comment(lib,"URlmon")
#pragma comment(lib, "Sensapi.lib")

#ifdef _DEBUG
#pragma comment(lib,"ComplexMat_d.lib")
#pragma comment(lib, "Hdf5IO_d.lib")
#else
#pragma comment(lib,"ComplexMat.lib")
#pragma comment(lib, "Hdf5IO.lib")
#endif // _DEBUG

using namespace cv;

int utc_to_gps(const char* utc_time, double* gps_time)
{
	if (!utc_time || !gps_time) return -1;
	int year, month, day, hour, minute;
	double seconds = 0.0;
	if (sscanf(utc_time, "%d-%d-%dT%d:%d:%lf", &year, &month, &day, &hour, &minute, &seconds) != 6)
		return -1;
	tm timeValue = {};
	timeValue.tm_year = year - 1900;
	timeValue.tm_mon = month - 1;
	timeValue.tm_mday = day;
	timeValue.tm_hour = hour;
	timeValue.tm_min = minute;
	timeValue.tm_sec = static_cast<int>(floor(seconds));
	timeValue.tm_isdst = 0;
	*gps_time = static_cast<double>(_mkgmtime(&timeValue) - 315964809) + (seconds - floor(seconds));
	return 0;
}

/*宏定义*/
#define RETURN_MSG \
{ \
    if( fp ) fclose( fp ); \
    return( -1 ); \
}

#define GET_NEXT_LINE \
{ \
    if( !fgets( instring, 256, fp ) ) \
        ch = 0; \
    else \
        ch = *instring; \
}

namespace
{
	constexpr double kDimacsCostScale = 1000000.0;

	bool toDimacsSupply(double value, long long& output)
	{
		if (!std::isfinite(value) || value < static_cast<double>(LONG_MIN) || value > static_cast<double>(LONG_MAX)) return false;
		const long long rounded = llround(value);
		if (static_cast<double>(rounded) != value) return false;
		output = rounded;
		return true;
	}

	bool toDimacsCost(double value, long long& output)
	{
		if (!std::isfinite(value) || value < 0.0) return false;
		const long double scaled = static_cast<long double>(value) * static_cast<long double>(kDimacsCostScale);
		if (scaled > static_cast<long double>(LLONG_MAX) - 0.5L) return false;
		output = llround(scaled);
		return true;
	}

	bool validateDimacsMatrices(const Mat& residue, const Mat& cost, double threshold)
	{
		for (int row = 0; row < residue.rows; ++row)
			for (int column = 0; column < residue.cols; ++column)
			{
				const double value = residue.at<double>(row, column);
				if (!std::isfinite(value)) return false;
				if (fabs(value) > threshold)
				{
					long long supply = 0;
					if (!toDimacsSupply(value, supply)) return false;
				}
			}
		for (int row = 0; row < cost.rows; ++row)
			for (int column = 0; column < cost.cols; ++column)
			{
				long long scaled = 0;
				if (!toDimacsCost(cost.at<double>(row, column), scaled)) return false;
			}
		return true;
	}

	bool validateDimacsCostMatrix(const Mat& cost)
	{
		for (int row = 0; row < cost.rows; ++row)
			for (int column = 0; column < cost.cols; ++column)
			{
				long long scaled = 0;
				if (!toDimacsCost(cost.at<double>(row, column), scaled)) return false;
			}
		return true;
	}

	long long dimacsCostText(double value)
	{
		long long scaled = 0;
		return toDimacsCost(value, scaled) ? scaled : -1;
	}

	FILE* openUtf8File(const char* utf8Path, const wchar_t* mode)
	{
		if (!utf8Path || !mode) return nullptr;
		std::wstring widePath;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(utf8Path, widePath, &error)) return nullptr;
		FILE* file = nullptr;
		return _wfopen_s(&file, widePath.c_str(), mode) == 0 ? file : nullptr;
	}
}
inline bool read_check(long ret, long ret_ref, const char* detail_info, const char* error_head)
{
	if (ret != ret_ref)
	{
		fprintf(stderr, "%s %s\n\n", error_head, detail_info);
		return true;
	}
	return false;
}


Utils::Utils()
{

}

Utils::~Utils()
{
}

bool Utils::findZeroDopplerTime(
	orbitStateVectors& stateVectors,
	const Position& groundPosition,
	double wavelength,
	double time_interval,
	double dopplerFrequency,
	double& zeroDopplerTime,
	double& distance,
	double dopplerThreshold)
{
	return orbitStateVectors::findZeroDopplerTime(stateVectors, groundPosition, wavelength, time_interval, dopplerFrequency, zeroDopplerTime, distance, dopplerThreshold);
}

namespace
{
	struct LegacyNewtonProgressContext
	{
		NewtonProgressCallback callback = nullptr;
	};

	bool __stdcall forwardLegacyNewtonProgress(int progress, const char* message, void* userData)
	{
		const LegacyNewtonProgressContext* context = static_cast<const LegacyNewtonProgressContext*>(userData);
		return !context || !context->callback || context->callback(progress, message);
	}
}

bool Utils::newton_iter_core(
	int iter_times,
	Mat& P1, Mat& P2, Mat& P3,
	const Mat& Satellite_M_T_Position,
	const Mat& Satellite_S_T_Position,
	const Mat& Satellite_S_R_Position,
	const Mat& Satellite_M_R_Position,
	const Mat& Satellite_M,
	const Mat& Vs,
	const Mat& R_M,
	const Mat& R_F,
	const Mat& fd,
	double lambda,
	NewtonProgressCallback cb)
{
	LegacyNewtonProgressContext context;
	context.callback = cb;
	return newton_iter_core_ex(iter_times, P1, P2, P3, Satellite_M_T_Position, Satellite_S_T_Position,
		Satellite_S_R_Position, Satellite_M_R_Position, Satellite_M, Vs, R_M, R_F, fd, lambda,
		cb ? forwardLegacyNewtonProgress : nullptr, cb ? &context : nullptr);
}

bool Utils::newton_iter_core_ex(
	int iter_times,
	Mat& P1, Mat& P2, Mat& P3,
	const Mat& Satellite_M_T_Position,
	const Mat& Satellite_S_T_Position,
	const Mat& Satellite_S_R_Position,
	const Mat& Satellite_M_R_Position,
	const Mat& Satellite_M,
	const Mat& Vs,
	const Mat& R_M,
	const Mat& R_F,
	const Mat& fd,
	double lambda,
	NewtonProgressCallbackEx cb,
	void* userData)
{
	int nr = P1.rows;
	int nc = P1.cols;

	Mat M_T, S_T, S_R;
	Mat f1, f2, f3;
	Mat det_Df, Df_ni11, Df_ni12, Df_ni13, Df_ni21, Df_ni22, Df_ni23;
	Mat Df11, Df12, Df13, Df21, Df22, Df23, Df31, Df32, Df33, Df_ni31, Df_ni32, Df_ni33;
	Mat delta_Rt1, delta_Rt2, delta_Rt3;

	Mat ones = Mat::ones(1, nc, CV_64F);
	Mat temp_var, temp_var1;

	for (int i = 0; i < iter_times; i++)
	{
		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - P1;
		temp_var = temp_var.mul(temp_var);
		temp_var.copyTo(M_T);
		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - P2;
		temp_var = temp_var.mul(temp_var);
		M_T = M_T + temp_var;
		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - P3;
		temp_var = temp_var.mul(temp_var);
		M_T = M_T + temp_var;
		cv::sqrt(M_T, f1);
		f1 = f1 * 2 - 2 * R_M;

		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - P1;
		temp_var = temp_var.mul(temp_var);
		temp_var.copyTo(S_T);
		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - P2;
		temp_var = temp_var.mul(temp_var);
		S_T = S_T + temp_var;
		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - P3;
		temp_var = temp_var.mul(temp_var);
		S_T = S_T + temp_var;

		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - P1;
		temp_var = temp_var.mul(temp_var);
		temp_var.copyTo(S_R);
		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - P2;
		temp_var = temp_var.mul(temp_var);
		S_R = S_R + temp_var;
		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - P3;
		temp_var = temp_var.mul(temp_var);
		S_R = S_R + temp_var;

		cv::sqrt(S_T, f2);
		cv::sqrt(S_R, temp_var);
		f2 = f2 + temp_var - R_F;

		temp_var = Vs(Range(0, Vs.rows), Range(0, 1)) * ones;
		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(0, 1)) * ones - P1;
		f3 = temp_var.mul(temp_var1);
		temp_var = Vs(Range(0, Vs.rows), Range(1, 2)) * ones;
		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(1, 2)) * ones - P2;
		f3 = f3 + temp_var.mul(temp_var1);
		temp_var = Vs(Range(0, Vs.rows), Range(2, 3)) * ones;
		temp_var1 = Satellite_M(Range(0, Satellite_M.rows), Range(2, 3)) * ones - P3;
		f3 = f3 + temp_var.mul(temp_var1);

		Mat ones_col = Mat::ones(nr, 1, CV_64F);
		temp_var = ones_col * fd;
		temp_var1 = R_M * lambda / 2.0;
		f3 = f3 + temp_var.mul(temp_var1);

		//Dff
		//第一行：f(1)的x，y，z的导数
		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(0, 1)) * ones - P1;
		cv::sqrt(M_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df11 = temp_var.mul(temp_var1);

		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(0, 1)) * ones - P1;
		cv::sqrt(M_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df11 = Df11 + temp_var.mul(temp_var1);

		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(1, 2)) * ones - P2;
		cv::sqrt(M_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df12 = temp_var.mul(temp_var1);

		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(1, 2)) * ones - P2;
		cv::sqrt(M_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df12 = Df12 + temp_var.mul(temp_var1);

		temp_var = Satellite_M_T_Position(Range(0, Satellite_M_T_Position.rows), Range(2, 3)) * ones - P3;
		cv::sqrt(M_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df13 = temp_var.mul(temp_var1);

		temp_var = Satellite_M_R_Position(Range(0, Satellite_M_R_Position.rows), Range(2, 3)) * ones - P3;
		cv::sqrt(M_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df13 = Df13 + temp_var.mul(temp_var1);


		//第二行：f(2)的x，y，z的导数
		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(0, 1)) * ones - P1;
		cv::sqrt(S_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df21 = temp_var.mul(temp_var1);

		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(0, 1)) * ones - P1;
		cv::sqrt(S_R, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df21 = Df21 + temp_var.mul(temp_var1);


		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(1, 2)) * ones - P2;
		cv::sqrt(S_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df22 = temp_var.mul(temp_var1);

		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(1, 2)) * ones - P2;
		cv::sqrt(S_R, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df22 = Df22 + temp_var.mul(temp_var1);


		temp_var = Satellite_S_T_Position(Range(0, Satellite_S_T_Position.rows), Range(2, 3)) * ones - P3;
		cv::sqrt(S_T, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df23 = temp_var.mul(temp_var1);

		temp_var = Satellite_S_R_Position(Range(0, Satellite_S_R_Position.rows), Range(2, 3)) * ones - P3;
		cv::sqrt(S_R, temp_var1);
		temp_var1 = 1 / temp_var1;
		temp_var1 = -temp_var1;
		Df23 = Df23 + temp_var.mul(temp_var1);

		//第三行：f(3)的x，y，z的导数
		Df31 = -Vs(Range(0, Vs.rows), Range(0, 1)) * ones;
		Df32 = -Vs(Range(0, Vs.rows), Range(1, 2)) * ones;
		Df33 = -Vs(Range(0, Vs.rows), Range(2, 3)) * ones;

		temp_var = Df11.mul(Df22);
		temp_var = temp_var.mul(Df33);
		temp_var.copyTo(det_Df);

		temp_var = Df12.mul(Df23);
		temp_var = temp_var.mul(Df31);
		det_Df = det_Df + temp_var;

		temp_var = Df13.mul(Df21);
		temp_var = temp_var.mul(Df32);
		det_Df = det_Df + temp_var;

		temp_var = Df31.mul(Df22);
		temp_var = temp_var.mul(Df13);
		det_Df = det_Df - temp_var;

		temp_var = Df32.mul(Df23);
		temp_var = temp_var.mul(Df11);
		det_Df = det_Df - temp_var;

		temp_var = Df33.mul(Df21);
		temp_var = temp_var.mul(Df12);
		det_Df = det_Df - temp_var;

		Df_ni11 = (Df22.mul(Df33) - Df32.mul(Df23)) / det_Df;
		Df_ni12 = -(Df12.mul(Df33) - Df32.mul(Df13)) / det_Df;
		Df_ni13 = (Df12.mul(Df23) - Df22.mul(Df13)) / det_Df;
		delta_Rt1 = Df_ni11.mul(f1) + Df_ni12.mul(f2) + Df_ni13.mul(f3);

		Df_ni21 = -(Df21.mul(Df33) - Df31.mul(Df23)) / det_Df;
		Df_ni22 = (Df11.mul(Df33) - Df31.mul(Df13)) / det_Df;
		Df_ni23 = -(Df11.mul(Df23) - Df21.mul(Df13)) / det_Df;
		delta_Rt2 = Df_ni21.mul(f1) + Df_ni22.mul(f2) + Df_ni23.mul(f3);

		Df_ni31 = (Df21.mul(Df32) - Df31.mul(Df22)) / det_Df;
		Df_ni32 = -(Df11.mul(Df32) - Df31.mul(Df12)) / det_Df;
		Df_ni33 = (Df22.mul(Df11) - Df21.mul(Df12)) / det_Df;
		delta_Rt3 = Df_ni31.mul(f1) + Df_ni32.mul(f2) + Df_ni33.mul(f3);

		P1 = P1 - delta_Rt1;
		P2 = P2 - delta_Rt2;
		P3 = P3 - delta_Rt3;

		if (cb)
		{
			int progress = (i + 1) * 90 / iter_times;
			if (!cb(progress, "Computing Newton iteration...", userData))
			{
				return false;
			}
		}
	}
	return true;
}

int Utils::createVandermondeMatrix(Mat& inArray, Mat& vandermondeMatrix, int degree)
{
	if (inArray.cols != 1 || inArray.rows < 1 || degree < 1)
	{
		fprintf(stderr, "createVandermondeMatrix(): input check failed!\n");
		return -1;
	}
	vandermondeMatrix.create(inArray.rows, degree + 1, CV_64F);
	if (inArray.type() != CV_64F) inArray.convertTo(inArray, CV_64F);
	for (int i = 0; i < inArray.rows; i++)
	{
		for (int j = 0; j < degree + 1; j++)
		{
			vandermondeMatrix.at<double>(i, j) = pow(inArray.at<double>(i, 0), (double)j);
		}
	}
	return 0;
}

int Utils::polyFit(Mat& a, Mat& B, Mat& x)
{
	Mat A, b;
	a.copyTo(A);
	B.copyTo(b);
	if (A.rows != b.rows || A.cols > A.rows || A.empty())
	{
		fprintf(stderr, "polyFit(): input check failed!\n");
		return -1;
	}
	if (A.type() != CV_64F) A.convertTo(A, CV_64F);
	if (b.type() != CV_64F) b.convertTo(b, CV_64F);
	Mat A_t;
	cv::transpose(A, A_t);
	A = A_t * A;
	b = A_t * b;
	if (!cv::solve(A, b, x, cv::DECOMP_LU))
	{
		fprintf(stderr, "polyFit(): matrix deficiency!\n");
		return -1;
	}
	return 0;
}

int Utils::polyVal(Mat& coefficient, double x, double* val)
{
	if (!val || coefficient.rows < 1 || coefficient.cols != 1) return -1;
	double sum = 0.0;
	if (coefficient.type() != CV_64F) coefficient.convertTo(coefficient, CV_64F);
	for (int i = 0; i < coefficient.rows; i++)
	{
		sum += coefficient.at<double>(i, 0) * pow(x, (double)i);
	}
	*val = sum;
	return 0;
}

int Utils::get_mode_index(const Mat& input, int* out)
{
	if (input.empty() || input.type() != CV_32S || out == NULL)
	{
		fprintf(stderr, "get_mode_index(): input check failed!\n");
		return -1;
	}
	Mat temp; input.copyTo(temp);
	int nr = temp.rows; int nc = temp.cols;
	temp = temp.reshape(0, 1);
	cv::sort(temp, temp, cv::SORT_EVERY_ROW + cv::SORT_ASCENDING);
	int total = nr * nc;
	int max_count = 0; int max_count_ix = 0; int count = 0, i = 0, j = 0;
	while (i < total - 1)
	{
		count = 0;
		for (j = i; j < total - 1; j++)
		{
			if (temp.at<int>(0, j) == temp.at<int>(0, j + 1)) count++;
			else break;
		}
		if (max_count < count)
		{
			max_count = count;
			max_count_ix = j;
		}
		j++;
		i = j;
	}
	*out = temp.at<int>(0, max_count_ix);
	return 0;
}

int Utils::diff(Mat& Src, Mat& diff1, Mat& diff2, bool same)
{
	int nr = Src.rows;
	int nc = Src.cols;
	if (nr < 2 || nc < 2 || Src.type() != CV_64F)
	{
		fprintf(stderr, "diff(): input check failed!\n\n");
		return -1;
	}

	diff1 = Src(Range(1, nr), Range(0, nc)) - Src(Range(0, nr - 1), Range(0, nc));
	diff2 = Src(Range(0, nr), Range(1, nc)) - Src(Range(0, nr), Range(0, nc - 1));
	if (same)
	{
		copyMakeBorder(diff1, diff1, 0, 1, 0, 0, BORDER_CONSTANT, Scalar(0.0));
		copyMakeBorder(diff2, diff2, 0, 0, 0, 1, BORDER_CONSTANT, Scalar(0.0));
	}
	return 0;
}



int Utils::generate_phase(const ComplexMat& Master, const ComplexMat& Slave, Mat& phase)
{
	if (Master.GetRows() < 1 ||
		Master.GetCols() < 1 ||
		Slave.GetRows() != Master.GetRows() ||
		Slave.GetCols() != Master.GetCols() ||
		Master.type() != Slave.type() ||
		(Master.type() != CV_64F && Master.type() != CV_32F))
	{
		fprintf(stderr, "generate_phase(): input check failed!\n\n");
		return -1;
	}
	int rows = Master.GetRows();
	int cols = Master.GetCols();
	phase.create(rows, cols, CV_64F);
	if (Master.type() == CV_64F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				double real = Master.re.at<double>(i, j) * Slave.re.at<double>(i, j) + Master.im.at<double>(i, j) * Slave.im.at<double>(i, j);
				double imag = Slave.re.at<double>(i, j) * Master.im.at<double>(i, j) - Master.re.at<double>(i, j) * Slave.im.at<double>(i, j);
				phase.at<double>(i, j) = atan2(imag, real);
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
				double real = Master.re.at<float>(i, j) * Slave.re.at<float>(i, j) + Master.im.at<float>(i, j) * Slave.im.at<float>(i, j);
				double imag = Slave.re.at<float>(i, j) * Master.im.at<float>(i, j) - Master.re.at<float>(i, j) * Slave.im.at<float>(i, j);
				phase.at<double>(i, j) = atan2(imag, real);
			}
		}
	}
	return 0;
}

int Utils::write_DIMACS(const char* DIMACS_file_problem, triangle* tri, int num_triangle, vector<tri_node>& nodes, tri_edge* edges, long num_edges, Mat& cost)
{
	if (DIMACS_file_problem == NULL ||
		tri == NULL ||
		num_triangle < 1 ||
		nodes.size() < 3 ||
		edges == NULL ||
		num_edges < 3||
		cost.rows < 2||
		cost.cols < 2||
		cost.channels() != 1||
		cost.type() != CV_64F
		)
	{
		fprintf(stderr, "write_DIMACS(): input check failed!\n\n");
		return -1;
	}
	if (!validateDimacsCostMatrix(cost)) return -1;
	for (int i = 0; i < num_triangle; ++i)
	{
		long long supply = 0;
		if (!std::isfinite((tri + i)->residue) ||
			(fabs((tri + i)->residue) > 0.7 && !toDimacsSupply((tri + i)->residue, supply))) return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_file_problem, L"wt");
	if (fp == NULL)
	{
		fprintf(stderr, "write_DIMACS(): can't open %s\n", DIMACS_file_problem);
		return -1;
	}
	
	// removed unused: ret (DIMACS write path never uses return value)
	int num_nodes;

	num_nodes = static_cast<int>(nodes.size());
	long num_arcs = 0;
	for (int i = 0; i < num_triangle; i++)
	{
		if ((tri + i) != NULL)
		{
			if ((tri + i)->neigh1 > 0) num_arcs++;
			if ((tri + i)->neigh2 > 0) num_arcs++;
			if ((tri + i)->neigh3 > 0) num_arcs++;
		}
	}

	//统计正负残差点并写入节点信息
	int positive, negative;
	// removed unused: total (counted but never read)
	positive = 0;
	negative = 0;
	double thresh = 0.7;
	for (int i = 0; i < num_triangle; i++)
	{
		if ((tri + i)->residue > thresh)
		{
			positive++;
		}
		if ((tri + i)->residue < -thresh)
		{
			negative++;
		}
	}
	bool b_balanced = (positive == negative);
	if (negative == 0 || positive == 0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "write_DIMACS(): no residue point!\n\n");
		return -1;
	}
	fprintf(fp, "c This is MCF problem file.\n");
	fprintf(fp, "c Problem line(nodes, links)\n");
	//统计边缘三角形个数
	long boundry_tri = 0;
	for (int i = 0; i < num_triangle; i++)
	{
		if ((tri + i) != NULL)
		{
			if ((edges + (tri + i)->edge1 - 1)->isBoundary ||
				(edges + (tri + i)->edge2 - 1)->isBoundary ||
				(edges + (tri + i)->edge3 - 1)->isBoundary)
			{
				boundry_tri++;
			}
		}
	}
	long n;
	if (!b_balanced)
	{
		n = num_triangle + 1;
		fprintf(fp, "p min %ld %ld\n", n, num_arcs + boundry_tri * 2);
	}
	else
	{
		n = num_triangle;
		fprintf(fp, "p min %ld %ld\n", n, num_arcs);
	}
	fprintf(fp, "c Node descriptor lines\n");
	positive = 0;
	negative = 0;
	int count = 0;
	// removed unused: b_positive, b_negative, is_residue (replaced by residue sign check below)
	double sum = 0.0;
	for (int i = 0; i < num_triangle; i++)
	{
		if ((tri + i) != NULL && (tri + i)->residue > thresh)
		{
			fprintf(fp, "n %d %lld\n", i + 1, llround((tri + i)->residue));
			sum += (tri + i)->residue;
		}
		if ((tri + i) != NULL && (tri + i)->residue < -thresh)
		{
			fprintf(fp, "n %d %lld\n", i + 1, llround((tri + i)->residue));
			sum += (tri + i)->residue;
		}
	}
	//写入大地节点
	if (!b_balanced)
	{
		fprintf(fp, "n %d %lld\n", num_triangle + 1, llround(-sum));
	}

	//写入流费用
	fprintf(fp, "c Arc descriptor lines(from, to, minflow, maxflow, cost)\n");
	int rows, cols;
	int lower_bound = 0;
	int upper_bound = 5;
	double cost_mean;
	int nr = cost.rows;
	int nc = cost.cols;
	for (int i = 0; i < num_triangle; i++)
	{
		if ((tri + i) != NULL &&
			(tri + i)->p1 >= 1 &&
			(tri + i)->p1 <= num_nodes &&
			(tri + i)->p2 >= 1 &&
			(tri + i)->p2 <= num_nodes &&
			(tri + i)->p3 >= 1 &&
			(tri + i)->p3 <= num_nodes
			)
		{
			cost_mean = 0.0;
			nodes[(tri + i)->p1 - 1].get_pos(&rows, &cols);
			if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
			nodes[(tri + i)->p2 - 1].get_pos(&rows, &cols);
			if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
			nodes[(tri + i)->p3 - 1].get_pos(&rows, &cols);
			if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
			cost_mean = cost_mean / 3;
			if ((tri + i)->neigh1 > 0) fprintf(fp, "a %d %d %d %d %lld\n", i + 1, (tri + i)->neigh1, lower_bound, upper_bound, dimacsCostText(cost_mean));
			if ((tri + i)->neigh2 > 0) fprintf(fp, "a %d %d %d %d %lld\n", i + 1, (tri + i)->neigh2, lower_bound, upper_bound, dimacsCostText(cost_mean));
			if ((tri + i)->neigh3 > 0) fprintf(fp, "a %d %d %d %d %lld\n", i + 1, (tri + i)->neigh3, lower_bound, upper_bound, dimacsCostText(cost_mean));
		}
	}
	if (!b_balanced)
	{
		//写入边界流费用
		for (int i = 0; i < num_triangle; i++)
		{
			if ((tri + i) != NULL &&
				(((edges + (tri + i)->edge1 - 1)->isBoundary) ||
					((edges + (tri + i)->edge2 - 1)->isBoundary) ||
					((edges + (tri + i)->edge3 - 1)->isBoundary)
					))
			{
				cost_mean = 0.0;
				nodes[(tri + i)->p1 - 1].get_pos(&rows, &cols);
				if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
				nodes[(tri + i)->p2 - 1].get_pos(&rows, &cols);
				if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
				nodes[(tri + i)->p3 - 1].get_pos(&rows, &cols);
				if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
				cost_mean = cost_mean / 3;
			fprintf(fp, "a %d %d %d %d %lld\n", i + 1, num_triangle + 1, lower_bound, upper_bound, dimacsCostText(cost_mean));
			fprintf(fp, "a %d %d %d %d %lld\n", num_triangle + 1, i + 1, lower_bound, upper_bound, dimacsCostText(cost_mean));
			}
		}
	}
	if (fp) fclose(fp);
	fp = NULL;
	return 0;
}

int Utils::write_DIMACS(
	const char* DIMACS_file_problem,
	vector<triangle>& triangle,
	vector<tri_node>& nodes,
	vector<tri_edge>& edges,
	const Mat& cost
)
{
	if (DIMACS_file_problem == NULL ||
		triangle.size() < 1||
		nodes.size() < 3 ||
		edges.size() < 3 ||
		cost.rows < 2 ||
		cost.cols < 2 ||
		cost.channels() != 1 ||
		cost.type() != CV_64F
		)
	{
		fprintf(stderr, "write_DIMACS(): input check failed!\n\n");
		return -1;
	}
	if (!validateDimacsCostMatrix(cost)) return -1;
	for (const ::triangle& item : triangle)
	{
		long long supply = 0;
		if (!std::isfinite(item.residue) || (fabs(item.residue) > 0.7 && !toDimacsSupply(item.residue, supply))) return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_file_problem, L"wt");
	if (fp == NULL)
	{
		fprintf(stderr, "write_DIMACS(): can't open %s\n", DIMACS_file_problem);
		return -1;
	}

	// removed unused: ret (DIMACS write path never uses return value)
	int num_nodes;
	int num_triangle = static_cast<int>(triangle.size());
	num_nodes = static_cast<int>(nodes.size());
	long num_arcs = 0;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].neigh1 > 0) num_arcs++;
		if (triangle[i].neigh2 > 0) num_arcs++;
		if (triangle[i].neigh3 > 0) num_arcs++;
	}

	//统计正负残差点并写入节点信息
	int positive, negative;
	// removed unused: total (counted but never read)
	positive = 0;
	negative = 0;
	double thresh = 0.7;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].residue > thresh)
		{
			positive++;
		}
		if (triangle[i].residue < -thresh)
		{
			negative++;
		}
	}
	bool b_balanced = (positive == negative);
	if (negative == 0 && positive == 0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "write_DIMACS(): no residue point!\n\n");
		return -1;
	}
	fprintf(fp, "c This is MCF problem file.\n");
	fprintf(fp, "c Problem line(nodes, links)\n");
	//统计边缘三角形个数
	long boundry_tri = 0;
	for (int i = 0; i < num_triangle; i++)
	{
		if (edges[triangle[i].edge1 - 1].isBoundary ||
			edges[triangle[i].edge2 - 1].isBoundary ||
			edges[triangle[i].edge3 - 1].isBoundary)
		{
			boundry_tri++;
		}
	}
	long n;
	if (!b_balanced)
	{
		n = num_triangle + 1;
		fprintf(fp, "p min %ld %ld\n", n, num_arcs + boundry_tri * 2);
	}
	else
	{
		n = num_triangle;
		fprintf(fp, "p min %ld %ld\n", n, num_arcs);
	}
	fprintf(fp, "c Node descriptor lines\n");
	positive = 0;
	negative = 0;
	int count = 0;
	// removed unused: b_positive, b_negative, is_residue (replaced by residue sign check below)
	double sum = 0.0;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].residue > thresh)
		{
			fprintf(fp, "n %d %lld\n", i + 1, llround(triangle[i].residue));
			sum += triangle[i].residue;
		}
		if (triangle[i].residue < -thresh)
		{
			fprintf(fp, "n %d %lld\n", i + 1, llround(triangle[i].residue));
			sum += triangle[i].residue;
		}
	}
	//写入大地节点
	if (!b_balanced)
	{
		fprintf(fp, "n %d %lld\n", num_triangle + 1, llround(-sum));
	}

	//写入流费用
	fprintf(fp, "c Arc descriptor lines(from, to, minflow, maxflow, cost)\n");
	int rows, cols;
	int lower_bound = 0;
	int upper_bound = 1;
	double cost_mean;
	int nr = cost.rows;
	int nc = cost.cols;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].p1 >= 1 &&
			triangle[i].p1 <= num_nodes &&
			triangle[i].p2 >= 1 &&
			triangle[i].p2 <= num_nodes &&
			triangle[i].p3 >= 1 &&
			triangle[i].p3 <= num_nodes
			)
		{
			cost_mean = 0.0;
			nodes[triangle[i].p1 - 1].get_pos(&rows, &cols);
			if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
			nodes[triangle[i].p2 - 1].get_pos(&rows, &cols);
			if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
			nodes[triangle[i].p3 - 1].get_pos(&rows, &cols);
			if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
			cost_mean = cost_mean / 3;
			if (triangle[i].neigh1 > 0) fprintf(fp, "a %d %d %d %d %lld\n", i + 1, triangle[i].neigh1, lower_bound, upper_bound, dimacsCostText(cost_mean));
			if (triangle[i].neigh2 > 0) fprintf(fp, "a %d %d %d %d %lld\n", i + 1, triangle[i].neigh2, lower_bound, upper_bound, dimacsCostText(cost_mean));
			if (triangle[i].neigh3 > 0) fprintf(fp, "a %d %d %d %d %lld\n", i + 1, triangle[i].neigh3, lower_bound, upper_bound, dimacsCostText(cost_mean));
		}
	}
	if (!b_balanced)
	{
		//写入边界流费用
		for (int i = 0; i < num_triangle; i++)
		{
			if (edges[triangle[i].edge1 - 1].isBoundary ||
				edges[triangle[i].edge2 - 1].isBoundary ||
				edges[triangle[i].edge3 - 1].isBoundary)
			{
				cost_mean = 0.0;
				nodes[triangle[i].p1 - 1].get_pos(&rows, &cols);
				if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
				nodes[triangle[i].p2 - 1].get_pos(&rows, &cols);
				if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
				nodes[triangle[i].p3 - 1].get_pos(&rows, &cols);
				if (rows >= 0 && rows <= nr - 1) cost_mean += cost.at<double>(rows, cols);
				cost_mean = cost_mean / 3;
			fprintf(fp, "a %d %d %d %d %lld\n", i + 1, num_triangle + 1, lower_bound, upper_bound, dimacsCostText(cost_mean));
			fprintf(fp, "a %d %d %d %d %lld\n", num_triangle + 1, i + 1, lower_bound, upper_bound, dimacsCostText(cost_mean));
			}
		}
	}
	if (fp) fclose(fp);
	fp = NULL;
	return 0;
}

int Utils::read_DIMACS(const char* DIMACS_file_solution, Mat& k1, Mat& k2, int rows, int cols)
{
	if (rows < 2 || cols < 2)
	{
		fprintf(stderr, "read_DIMACS(): input check failed!\n\n");
		return -1;
	}
	char instring[256];
	char ch;
	double obj_value = 0;
	int i, row_index, col_index, symbol;
	long from, to;
	double flow = 0;
	k1 = Mat::zeros(rows - 1, cols, CV_64F);
	k2 = Mat::zeros(rows, cols - 1, CV_64F);
	long earth_node_indx = (rows - 1) * (cols - 1) + 1;
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_file_solution, L"rt");
	if (fp == NULL)
	{
		fprintf(stderr, "read_DIMACS(): can't open file %s\n\n", DIMACS_file_solution);
		return -1;
	}
	/////////////////////读取注释///////////////////////////
	GET_NEXT_LINE;
	while (ch != 's' && ch)
	{
		if (ch != 'c')
		{
			if (fp) fclose(fp);
			fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
			return -1;
		}
		GET_NEXT_LINE;
	}
	/////////////////////读取优化目标值/////////////////////
	for (i = 1; i < 81; i++)
	{
		if (isspace((int)instring[i]) > 0)
		{
			i++;
			break;
		}
	}
	if (sscanf(&(instring[i]), "%lf", &obj_value) != 1)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
		return -1;
	}
	if (obj_value < 0.0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): this problem can't be solved(unbounded or infeasible)!\n\n");
		return -1;
	}
	////////////////////读取MCF结果/////////////////////////
	GET_NEXT_LINE;
	while (ch && ch == 'f')
	{
		if (sscanf(&(instring[2]), "%ld %ld %lf", &from, &to, &flow) != 3 ||
			flow < 0.0 || from < 0 || to < 0)
		{
			if (fp) fclose(fp);
			fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
			return -1;
		}
		//是否为接地弧
		if (from == earth_node_indx || to == earth_node_indx)
		{
			if (from == earth_node_indx)
			{
				//top
				if (to <= cols - 1)
				{
					row_index = 0;
					col_index = to - 1;
					symbol = 1;
					k2.at<double>(row_index, col_index) = k2.at<double>(row_index, col_index) + symbol * flow;
				}
				//bottom
				else if (to > (rows - 2) * (cols - 1))
				{
					symbol = -1;
					row_index = rows - 1;
					col_index = to - (rows - 2) * (cols - 1) - 1;
					k2.at<double>(row_index, col_index) = k2.at<double>(row_index, col_index) + symbol * flow;
				}
				//right
				else if (to % (cols - 1) == 0 && to < (rows - 2) * (cols - 1) && to > (cols - 1))
				{
					symbol = 1;
					row_index = to / (cols - 1) - 1;
					col_index = cols - 1;
					k1.at<double>(row_index, col_index) = k1.at<double>(row_index, col_index) + symbol * flow;
				}
				//left
				else
				{
					symbol = -1;
					row_index = to / (cols - 1);
					col_index = 0;
					k1.at<double>(row_index, col_index) = k1.at<double>(row_index, col_index) + symbol * flow;
				}
			}
			else
			{
				/*long tmp;
				tmp = from;
				from = to;
				to = tmp;*/
				//top
				if (from <= cols - 1)
				{
					symbol = -1;
					row_index = 0;
					col_index = from - 1;
					k2.at<double>(row_index, col_index) = k2.at<double>(row_index, col_index) + symbol * flow;
				}
				//bottom
				else if (from >= (rows - 2) * (cols - 1))
				{
					symbol = 1;
					row_index = rows - 1;
					col_index = from - (rows - 2) * (cols - 1) - 1;
					k2.at<double>(row_index, col_index) = k2.at<double>(row_index, col_index) + symbol * flow;
				}
				//right
				else if (from % (cols - 1) == 0 && from < (rows - 2) * (cols - 1) && from >(cols - 1))
				{
					symbol = -1;
					row_index = from / (cols - 1) - 1;
					col_index = cols - 1;
					k1.at<double>(row_index, col_index) = k1.at<double>(row_index, col_index) + symbol * flow;
				}
				//left
				else
				{
					symbol = 1;
					row_index = from / (cols - 1);
					col_index = 0;
					k1.at<double>(row_index, col_index) = k1.at<double>(row_index, col_index) + symbol * flow;
				}
			}
		}
		else
		{
			if (abs(from - to) > 1)
			{
				symbol = from > to ? 1 : -1;
				row_index = (from > to ? to : from) / long(cols - 1);
				col_index = (from > to ? to : from) % long(cols - 1);
				if (col_index == 0)
				{
					col_index = cols - 1;
					row_index--;
				}
				col_index--;
				k2.at<double>(row_index + 1, col_index) = k2.at<double>(row_index + 1, col_index) + symbol * flow;
			}
			else
			{
				symbol = from > to ? 1 : -1;
				row_index = (from > to ? to : from) / long(cols - 1);
				col_index = (from > to ? to : from) % long(cols - 1);
				k1.at<double>(row_index, col_index) = k1.at<double>(row_index, col_index) + symbol * flow;
			}
		}
		GET_NEXT_LINE;
	}
	if (ch != 'c')
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
		return -1;
	}
	if (fp)
	{
		fclose(fp);
	}
	return 0;
}

int Utils::write_DIMACS(const char* DIMACS_file_problem, Mat& residue, Mat& coherence, double thresh)
{

	if (residue.cols < 2 ||
		residue.rows < 2 ||
		coherence.cols < 2 ||
		coherence.rows < 2||
		residue.type() != CV_64F||
		coherence.type() != CV_64F||
		(coherence.rows - residue.rows) != 1||
		(coherence.cols - residue.cols) != 1||
		thresh < 0.0)
	{
		fprintf(stderr, "write_DIMACS(): input check failed!\n\n");
		return -1;
	}
	if (!validateDimacsMatrices(residue, coherence, thresh))
	{
		fprintf(stderr, "write_DIMACS(): costs must be finite, non-negative, and supplies must be integers!\n\n");
		return -1;
	}
	long nr = residue.rows;
	long nc = residue.cols;
	long i, j;
	long node_index = 1;
	double sum = 0.0;
	//统计正负残差点数
	long positive, negative, Arcs_num, Nodes_num;
	// removed unused: total (counted but never read)
	positive = 0;
	negative = 0;
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			if (residue.at<double>(i, j) > thresh)
			{
				positive++;
			}
			if (residue.at<double>(i, j) < -thresh)
			{
				negative++;
			}
		}
	}
	bool b_balanced = (positive == negative);
	if (/*!b_balanced*/true)
	{
		Nodes_num = residue.rows * residue.cols + 1;
		Arcs_num = 2 * (residue.rows - 1) * residue.cols + 2 * residue.rows * (residue.cols - 1) +
			2 * 2 * residue.cols + 2 * 2 * (residue.rows - 2);
	}
	//else
	//{
	//	Nodes_num = residue.rows * residue.cols;
	//	Arcs_num = 2 * (residue.rows - 1) * residue.cols + 2 * residue.rows * (residue.cols - 1);
	//}
	//ofstream fout; // 未使用
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_file_problem, L"wt");
	if (!fp)
	{
		fprintf(stderr, "write_DIMACS(): cant't open file %s\n\n", DIMACS_file_problem);
		return -1;
	}
	fprintf(fp, "c This is a DIMACS file, describing Minimum Cost Flow problem.\n");
	fprintf(fp, "c Problem line (nodes, links)\n");
	fprintf(fp, "p min %ld %ld\n", Nodes_num, Arcs_num);
	fprintf(fp, "c Node descriptor lines (supply+ or demand-)\n");

	
	/*
	* 写入节点的度（残差值1，-1）
	*/
	
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			if (residue.at<double>(i, j) > thresh)
			{
				node_index = i * nc + j + 1;
				fprintf(fp, "n %ld %lld\n", node_index, llround(residue.at<double>(i, j)));
				sum += residue.at<double>(i, j);
			}
			if (residue.at<double>(i, j) < -thresh)
			{
				node_index = i * nc + j + 1;
				fprintf(fp, "n %ld %lld\n", node_index, llround(residue.at<double>(i, j)));
				sum += residue.at<double>(i, j);
			}
		}
	}

	/*写接地节点*/
	
	node_index = nc * nr + 1;
	fprintf(fp, "n %ld %lld\n", node_index, llround(-sum));
	long earth_node_index = node_index;

	/*
	* 写入每个有向弧的费用（流费用）
	*/
	long lower_bound = 0;
	long upper_bound = 5;
	double mean_coherence1, mean_coherence2, mean_coherence3, mean_coherence4;
	fprintf(fp, "c Arc descriptor lines (from, to, minflow, maxflow, cost)\n");
	/*接地节点的有向弧流费用*/
	//top
	for (i = 0; i < nc; i++)
	{
		node_index = i + 1;
		mean_coherence1 = coherence.at<double>(0, i);
		mean_coherence2 = mean_coherence1;
		fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
			node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
			earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2));
	}
	//bottom
	for (i = 0; i < nc; i++)
	{
		node_index = nc * (nr - 1) + i + 1;
		mean_coherence1 = coherence.at<double>(nr - 1, i);
		mean_coherence2 = mean_coherence1;
		fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
			node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
			earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2));
	}
	//left
	for (i = 1; i < nr - 1; i++)
	{
		node_index = nc * i + 1;
		mean_coherence1 = coherence.at<double>(i, 0);
		mean_coherence2 = mean_coherence1;
		fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
			node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
			earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2));
	}
	//right
	for (i = 1; i < nr - 1; i++)
	{
		node_index = nc * (i + 1);
		mean_coherence1 = coherence.at<double>(i, nc - 1);
		mean_coherence2 = mean_coherence1;
		fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
			node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
			earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2));
	}

	/*非接地节点的有向弧流费用*/
	for (i = 0; i < nr - 1; i++)
	{
		for (j = 0; j < nc - 1; j++)
		{
			node_index = i * nc + j + 1;
			/*正向*/
			mean_coherence1 = mean(coherence(Range(i, i + 2), Range(j, j + 3))).val[0];
			/*逆向*/
			mean_coherence2 = mean(coherence(Range(i, i + 2), Range(j, j + 3))).val[0];


			/*正向*/
			mean_coherence3 = mean(coherence(Range(i, i + 3), Range(j, j + 2))).val[0];
			/*逆向*/
			mean_coherence4 = mean(coherence(Range(i, i + 3), Range(j, j + 2))).val[0];
			fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
				node_index, node_index + 1, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
				node_index + 1, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2),
				node_index, node_index + nc, lower_bound, upper_bound, dimacsCostText(mean_coherence3),
				node_index + nc, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence4));

		}
	}



	for (j = 0; j < nc - 1; j++)
	{
		node_index = (nr - 1) * nc + j + 1;
		/*正向*/
		mean_coherence1 = mean(coherence(Range(nr - 1, nr + 1), Range(j, j + 3))).val[0];
		/*逆向*/
		mean_coherence2 = mean(coherence(Range(nr - 1, nr + 1), Range(j, j + 3))).val[0];
		fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
			node_index, node_index + 1, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
			node_index + 1, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2));
	}

	for (i = 0; i < nr - 1; i++)
	{
		node_index = (i + 1) * nc;
		/*正向*/
		mean_coherence1 = mean(coherence(Range(i, i + 3), Range(nc - 1, nc + 1))).val[0];
		/*逆向*/
		mean_coherence2 = mean(coherence(Range(i, i + 3), Range(nc - 1, nc + 1))).val[0];
		fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
			node_index, node_index + nc, lower_bound, upper_bound, dimacsCostText(mean_coherence1),
			node_index + nc, node_index, lower_bound, upper_bound, dimacsCostText(mean_coherence2));
	}
	fprintf(fp, "c ");
	fprintf(fp, "c End of file");
	if (fp) fclose(fp);
	fp = NULL;
	return 0;
}

int Utils::write_DIMACS(const char* DIMACS_problem_file, const Mat& residue, Mat& mask, const Mat& cost, double thresh)
{
	if (residue.cols < 2 ||
		residue.rows < 2 ||
		cost.cols < 2 ||
		cost.rows < 2 ||
		residue.type() != CV_64F ||
		cost.type() != CV_64F ||
		mask.type() != CV_32S ||
		mask.rows != cost.rows ||
		mask.cols != cost.cols ||
		(cost.rows - residue.rows) != 1 ||
		(cost.cols - residue.cols) != 1 ||
		thresh < 0.0)
	{
		fprintf(stderr, "write_DIMACS(): input check failed!\n\n");
		return -1;
	}
	if (!validateDimacsMatrices(residue, cost, thresh))
	{
		fprintf(stderr, "write_DIMACS(): costs must be finite, non-negative, and supplies must be integers!\n\n");
		return -1;
	}
	long nr = residue.rows;
	long nc = residue.cols;
	long positive, negative, Arcs_num = 0, Nodes_num;
	// removed unused: total, feasible_node_num (counted but never read)
	Mat new_mask; mask.copyTo(new_mask);
	new_mask = 1 - new_mask;
	Mat residue_mask = Mat::zeros(nr, nc, CV_32S);
	//根据输入掩膜数据和残差点数据更新掩膜
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (fabs(residue.at<double>(i, j)) > thresh)
			{
				residue_mask.at<int>(i, j) = 1;
				new_mask.at<int>(i, j) = 1;
				new_mask.at<int>(i + 1, j) = 1;
				new_mask.at<int>(i + 1, j + 1) = 1;
				new_mask.at<int>(i, j + 1) = 1;
			}
		}
	}
	mask = 1 - new_mask;
	//根据更新的掩膜计算可行的网络节点和流数量
	for (int i = 0; i < nr + 1; i++)
	{
		for (int j = 0; j < nc + 1; j++)
		{
			if (new_mask.at<int>(i, j) == 1)
			{
				int ii, jj;
				ii = i - 1; ii = ii < 0 ? 0 : ii;
				jj = j; jj = jj > nc - 1 ? nc - 1 : jj;
				residue_mask.at<int>(ii, jj) = 1;

				ii = i - 1; ii = ii < 0 ? 0 : ii;
				jj = j - 1; jj = jj < 0 ? 0 : jj;
				residue_mask.at<int>(ii, jj) = 1;

				ii = i; ii = ii > nr - 1 ? nr - 1 : ii;
				jj = j; jj = jj > nc - 1 ? nc - 1 : jj;
				residue_mask.at<int>(ii, jj) = 1;

				ii = i; ii = ii > nr - 1 ? nr - 1 : ii;
				jj = j - 1; jj = jj < 0 ? 0 : jj;
				residue_mask.at<int>(ii, jj) = 1;
			}
		}
	}
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (residue_mask.at<int>(i, j) == 1)
			{
				int ii, jj;
				ii = i; jj = j - 1;
				if (jj >= 0 && residue_mask.at<int>(ii, jj) == 1) Arcs_num++;
				ii = i; jj = j + 1;
				if (jj < nc && residue_mask.at<int>(ii, jj) == 1) Arcs_num++;
				ii = i - 1; jj = j;
				if (ii >= 0 && residue_mask.at<int>(ii, jj) == 1) Arcs_num++;
				ii = i + 1; jj = j;
				if (ii < nr && residue_mask.at<int>(ii, jj) == 1) Arcs_num++;
			}
		}
	}
	//计算可行节点掩膜边缘点数
	int edge_node_num = 0;
	for (int j = 0; j < nc; j++)
	{
		if (residue_mask.at<int>(0, j) == 1) edge_node_num++;
		if (residue_mask.at<int>(nr - 1, j) == 1) edge_node_num++;
	}
	for (int i = 1; i < nr - 1; i++)
	{
		if (residue_mask.at<int>(i, 0) == 1) edge_node_num++;
		if (residue_mask.at<int>(i, nc - 1) == 1) edge_node_num++;
	}
	long i, j;
	long node_index = 1;
	long node_index2;
	double sum = 0.0;
	//统计正负残差点数
	positive = 0;
	negative = 0;
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			if (residue.at<double>(i, j) > thresh)
			{
				positive++;
			}
			if (residue.at<double>(i, j) < -thresh)
			{
				negative++;
			}
		}
	}
	bool b_balanced = (positive == negative);
	if (/*!b_balanced*/1)
	{
		Nodes_num = residue.rows * residue.cols + 1;
		Arcs_num += 2 * 2 * residue.cols + 2 * 2 * (residue.rows - 2);
		/*Arcs_num = 2 * (residue.rows - 1) * residue.cols + 2 * residue.rows * (residue.cols - 1) +
			2 * 2 * residue.cols + 2 * 2 * (residue.rows - 2);*/
	}
	else
	{
		Nodes_num = residue.rows * residue.cols;
		Arcs_num = Arcs_num;
		//Arcs_num = 2 * (residue.rows - 1) * residue.cols + 2 * residue.rows * (residue.cols - 1);
	}
	//ofstream fout; // 未使用
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_problem_file, L"wt");
	if (!fp)
	{
		fprintf(stderr, "write_DIMACS(): cant't open file %s\n\n", DIMACS_problem_file);
		return -1;
	}
	fprintf(fp, "c This is a DIMACS file, describing Minimum Cost Flow problem.\n");
	fprintf(fp, "c Problem line (nodes, links)\n");
	fprintf(fp, "p min %ld %ld\n", Nodes_num, Arcs_num);
	fprintf(fp, "c Node descriptor lines (supply+ or demand-)\n");


	/*
	* 写入节点的度（残差值1，-1）
	*/

	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			if (residue.at<double>(i, j) > thresh)
			{
				node_index = i * nc + j + 1;
				fprintf(fp, "n %ld %lld\n", node_index, llround(residue.at<double>(i, j)));
				sum += residue.at<double>(i, j);
			}
			if (residue.at<double>(i, j) < -thresh)
			{
				node_index = i * nc + j + 1;
				fprintf(fp, "n %ld %lld\n", node_index, llround(residue.at<double>(i, j)));
				sum += residue.at<double>(i, j);
			}
		}
	}

	/*写接地节点*/

	node_index = nc * nr + 1;
	if (/*!b_balanced*/1)
	{
		fprintf(fp, "n %ld %lld\n", node_index, llround(-sum));
	}


	long earth_node_index = node_index;

	/*
	* 写入每个有向弧的费用（流费用）
	*/
	long lower_bound = 0;
	long upper_bound = 5;
	double mean_cost;
	fprintf(fp, "c Arc descriptor lines (from, to, minflow, maxflow, cost)\n");
	/*接地节点的有向弧流费用*/
	if (/*!b_balanced*/1)
	{
		//top
		for (i = 0; i < nc; i++)
		{
			node_index = i + 1;
			mean_cost = cost.at<double>(0, i);
			fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
				node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_cost),
				earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_cost));
		}
		//bottom
		for (i = 0; i < nc; i++)
		{
			node_index = nc * (nr - 1) + i + 1;
			mean_cost = cost.at<double>(nr - 1, i);
			fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
				node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_cost),
				earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_cost));
		}
		//left
		for (i = 1; i < nr - 1; i++)
		{
			node_index = nc * i + 1;
			mean_cost = cost.at<double>(i, 0);
			fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
				node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_cost),
				earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_cost));
		}
		//right
		for (i = 1; i < nr - 1; i++)
		{
			node_index = nc * (i + 1);
			mean_cost = cost.at<double>(i, nc - 1);
			fprintf(fp, "a %ld %ld %ld %ld %lld\na %ld %ld %ld %ld %lld\n",
				node_index, earth_node_index, lower_bound, upper_bound, dimacsCostText(mean_cost),
				earth_node_index, node_index, lower_bound, upper_bound, dimacsCostText(mean_cost));
		}
	}

	/*非接地节点的有向弧流费用*/
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			if (residue_mask.at<int>(i, j) == 1)
			{
				node_index = i * nc + j + 1;
				int ii, jj;
				
				ii = i; jj = j - 1;
				if (jj >= 0 && residue_mask.at<int>(ii, jj) == 1)
				{
					node_index2 = ii * nc + jj + 1;
					mean_cost = mean(cost(Range(i, i + 1), Range(j, j + 1))).val[0];
					fprintf(fp, "a %ld %ld %ld %ld %lld\n",
						node_index, node_index2, lower_bound, upper_bound, dimacsCostText(mean_cost));
				}

				ii = i; jj = j + 1;
				if (jj < nc && residue_mask.at<int>(ii, jj) == 1)
				{
					node_index2 = ii * nc + jj + 1;
					mean_cost = mean(cost(Range(i, i + 1), Range(j, j + 1))).val[0];
					fprintf(fp, "a %ld %ld %ld %ld %lld\n",
						node_index, node_index2, lower_bound, upper_bound, dimacsCostText(mean_cost));
				}
				ii = i - 1; jj = j;
				if (ii >= 0 && residue_mask.at<int>(ii, jj) == 1)
				{
					node_index2 = ii * nc + jj + 1;
					mean_cost = mean(cost(Range(i, i + 1), Range(j, j + 1))).val[0];
					fprintf(fp, "a %ld %ld %ld %ld %lld\n",
						node_index, node_index2, lower_bound, upper_bound, dimacsCostText(mean_cost));
				}
				ii = i + 1; jj = j;
				if (ii < nr && residue_mask.at<int>(ii, jj) == 1)
				{
					node_index2 = ii * nc + jj + 1;
					mean_cost = mean(cost(Range(i, i + 1), Range(j, j + 1))).val[0];
					fprintf(fp, "a %ld %ld %ld %ld %lld\n",
						node_index, node_index2, lower_bound, upper_bound, dimacsCostText(mean_cost));
				}
			}
		}
	}

	fprintf(fp, "c ");
	fprintf(fp, "c End of file");
	if (fp) fclose(fp);
	fp = NULL;
	return 0;
}

int Utils::cumsum(Mat& phase, int dim)
{
	/*
	cumulates along the dimension specified by dim
	dim = 1,按列计算
	dim = 2,按行计算
	*/
	int rows = phase.rows;
	int cols = phase.cols;
	int i, j;
	if (rows > 0 && cols > 0)
	{
		switch (dim)
		{
		case 1:
			if (phase.rows < 2 ||
				phase.cols < 1 ||
				phase.type() != CV_64F)
			{
				fprintf(stderr, "cumsum(): input check failed!\n\n");
				return -1;
			}
			for (j = 0; j < cols; j++)
			{
				for (i = 1; i < rows; i++)
				{
					phase.at<double>(i, j) = phase.at<double>(i - 1, j) + phase.at<double>(i, j);
				}
			}
			break;
		case 2:
			if (phase.rows < 1 ||
				phase.cols < 2 ||
				phase.type() != CV_64F)
			{
				fprintf(stderr, "cumsum(): input check failed!\n\n");
				return -1;
			}
			for (i = 0; i < rows; i++)
			{
				for (j = 1; j < cols; j++)
				{
					phase.at<double>(i, j) = phase.at<double>(i, j - 1) + phase.at<double>(i, j);
				}
			}
			break;
		default:
			break;
		}
	}
	return 0;
}

int Utils::cross(Mat& vec1, Mat& vec2, Mat& out)
{
	if (vec1.cols != 3 ||
		vec1.rows < 1 ||
		vec1.type() != vec2.type() ||
		vec1.channels() != 1 ||
		vec1.cols != vec2.cols ||
		vec1.rows != vec2.rows ||
		vec2.channels() != 1 ||
		(vec2.type() != CV_64F && vec2.type() != CV_32F)
		)
	{
		fprintf(stderr, "cross(): input check failed!\n\n");
		return -1;
	}
	if (vec2.type() == CV_64F)
	{
		Mat out_tmp = Mat::zeros(vec1.rows, vec1.cols, CV_64F);
		int rows = vec1.rows;
		for (int i = 0; i < rows; i++)
		{
			out_tmp.at<double>(i, 0) = vec1.at<double>(i, 1) * vec2.at<double>(i, 2) -
				vec1.at<double>(i, 2) * vec2.at<double>(i, 1);//a(2) * b(3) - a(3) * b(2)

			out_tmp.at<double>(i, 1) = vec1.at<double>(i, 2) * vec2.at<double>(i, 0) -
				vec1.at<double>(i, 0) * vec2.at<double>(i, 2);//a(3) * b(1) - a(1) * b(3)

			out_tmp.at<double>(i, 2) = vec1.at<double>(i, 0) * vec2.at<double>(i, 1) -
				vec1.at<double>(i, 1) * vec2.at<double>(i, 0);//a(1) * b(2) - a(2) * b(1)
		}
		out_tmp.copyTo(out);
	}
	else
	{
		Mat out_tmp = Mat::zeros(vec1.rows, vec1.cols, CV_32F);
		int rows = vec1.rows;
		for (int i = 0; i < rows; i++)
		{
			out_tmp.at<float>(i, 0) = vec1.at<float>(i, 1) * vec2.at<float>(i, 2) -
				vec1.at<float>(i, 2) * vec2.at<float>(i, 1);//a(2) * b(3) - a(3) * b(2)

			out_tmp.at<float>(i, 1) = vec1.at<float>(i, 2) * vec2.at<float>(i, 0) -
				vec1.at<float>(i, 0) * vec2.at<float>(i, 2);//a(3) * b(1) - a(1) * b(3)

			out_tmp.at<float>(i, 2) = vec1.at<float>(i, 0) * vec2.at<float>(i, 1) -
				vec1.at<float>(i, 1) * vec2.at<float>(i, 0);//a(1) * b(2) - a(2) * b(1)
		}
		out_tmp.copyTo(out);
	}
	return 0;
}

int Utils::gen_mask(Mat& coherence, Mat& phase_derivatives, Mat& mask, int wnd_size, double coh_thresh, double phase_derivative_thresh)
{
	if (coherence.rows < 2 ||
		coherence.cols < 2 ||
		coherence.channels() != 1 ||
		coherence.type() != CV_64F ||
		coherence.rows != phase_derivatives.rows ||
		coherence.cols  != phase_derivatives.cols ||
		phase_derivatives.channels() != 1 ||
		phase_derivatives.type() != CV_64F ||
		wnd_size < 0 ||
		wnd_size > coherence.rows ||
		coh_thresh < 0.0 ||
		coh_thresh > 1.0||
		phase_derivative_thresh < 0.0
		)
	{
		fprintf(stderr, "gen_mask(): input check failed!\n\n");
		return -1;
	}
	int nr = coherence.rows;
	int nc = coherence.cols;
	int radius = (wnd_size - 1) / 2;
	int kernel_size = 2 * radius + 1;

	Mat mean_coh, mean_phase_derivatives;
	cv::boxFilter(coherence, mean_coh, CV_64F, cv::Size(kernel_size, kernel_size), cv::Point(-1, -1), true, cv::BORDER_DEFAULT);
	cv::boxFilter(phase_derivatives, mean_phase_derivatives, CV_64F, cv::Size(kernel_size, kernel_size), cv::Point(-1, -1), true, cv::BORDER_DEFAULT);

	Mat tmp = Mat::zeros(nr, nc, CV_32S);
	tmp.copyTo(mask);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double mean = mean_coh.at<double>(i, j);
			double mean1 = mean_phase_derivatives.at<double>(i, j);
			if (mean > coh_thresh && 
				coherence.at<double>(i, j) > coh_thresh &&
				mean1 < phase_derivative_thresh &&
				phase_derivatives.at<double>(i, j) < phase_derivative_thresh)
			{
				mask.at<int>(i, j) = 1;
			}
		}
	}
	return 0;
}

int Utils::residue_sift(Mat& residue_src, Mat& residue_dst, double thresh, long* num_residue)
{
	int rows = residue_src.rows;
	int cols = residue_src.cols;
	if ( rows < 1 ||
		 cols < 1 ||
		 residue_src.type() != CV_64F||
		 residue_src.channels() != 1||
		 thresh < 0.0 ||
		 num_residue == NULL
		)
	{
		fprintf(stderr, "residue_sift(): input check failed!\n\n");
		return -1;
	}
	Mat tmp;
	residue_src.copyTo(tmp);
	*num_residue = 0;
//#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (std::fabs(residue_src.at<double>(i, j)) > thresh)
			{
				tmp.at<double>(i, j) = 1.0;
				*num_residue = *num_residue + 1;
			}
			else
			{
				tmp.at<double>(i, j) = 0.0;
			}
		}
	}
	tmp.copyTo(residue_dst);
	return 0;
}

int Utils::wrap(Mat& Src, Mat& Dst)
{
	int rows = Src.rows;
	int cols = Src.cols;
	if (rows < 1 || cols < 1 || (Src.type() != CV_64F && Src.type() != CV_32F))
	{
		fprintf(stderr, "wrap(): input check failed!\n\n");
		return -1;
	}
	Mat tmp = Mat::zeros(rows, cols, CV_64F);
	if (Src.type() == CV_64F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				tmp.at<double>(i, j) = atan2(sin(Src.at<double>(i, j)), cos(Src.at<double>(i, j)));
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
				tmp.at<double>(i, j) = atan2(sin(Src.at<float>(i, j)), cos(Src.at<float>(i, j)));
			}
		}
	}
	//Dst = tmp;
	tmp.copyTo(Dst);
	return 0;
}

int Utils::residue(Mat& phase, Mat& residuemat)
{
	int rows = phase.rows;
	int cols = phase.cols;
	int ret;
	if (rows < 2 || cols < 2 || phase.type() != CV_64F)
	{
		fprintf(stderr, "residue(): input check failed!\n\n");
		return -1;
	}
	Mat Diff_1 = phase(Range(1, rows), Range(0, cols)) - phase(Range(0, rows - 1), Range(0, cols));
	Mat Diff_2 = phase(Range(0, rows), Range(1, cols)) - phase(Range(0, rows), Range(0, cols - 1));
	ret = this->wrap(Diff_1, Diff_1);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	ret = this->wrap(Diff_2, Diff_2);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;

	Diff_1 = Diff_1(Range(0, Diff_1.rows), Range(1, Diff_1.cols)) - 
		Diff_1(Range(0, Diff_1.rows), Range(0, Diff_1.cols - 1));

	Diff_2 = Diff_2(Range(1, Diff_2.rows), Range(0, Diff_2.cols)) - 
		Diff_2(Range(0, Diff_2.rows - 1), Range(0, Diff_2.cols));

	Diff_1 = Diff_2 - Diff_1;
	Diff_1 = Diff_1 / (2 * PI);
	//Diff_1.copyTo(residuemat);
	residuemat = Diff_1;
	return 0;
}

int Utils::residue(triangle* tri, int num_triangle, vector<tri_node>& nodes, tri_edge* edges, int num_edges)
{
	if (tri == NULL ||
		num_triangle < 1 ||
		nodes.size() < 1||
		edges == NULL||
		num_edges < 3
		)
	{
		fprintf(stderr, "residue(): input check failed!\n\n");
		return -1;
	}
	double thresh = 50.0;
	int num_nodes = static_cast<int>(nodes.size());
	int end1, end2, end3, tmp;
	double x1, y1, x2, y2, direction, delta12, delta23, delta31, phi1, phi2, phi3, distance1,
		distance2, distance3;
	// removed unused: x3, y3, residue (triangle residue computed inline, x3/y3 not needed)
	int row1, col1, row2, col2, row3, col3;
	bool b_res = false;
	for (int i = 0; i < num_triangle; i++)
	{
		if ((tri + i) != NULL)
		{
			end1 = (tri + i)->p1;
			end2 = (tri + i)->p2;
			end3 = (tri + i)->p3;
		}
		if (end1 > end2)
		{
			tmp = end1;
			end1 = end2;
			end2 = tmp;
		}
		
		nodes[end1 - 1].get_pos(&row1, &col1);
		nodes[end2 - 1].get_pos(&row2, &col2);
		nodes[end3 - 1].get_pos(&row3, &col3);

		nodes[end1 - 1].get_distance(nodes[end2 - 1], &distance1);
		nodes[end2 - 1].get_distance(nodes[end3 - 1], &distance2);
		nodes[end3 - 1].get_distance(nodes[end1 - 1], &distance3);
		b_res = true;
		if ((distance1 > thresh) || (distance2 > thresh) || (distance3 > thresh)) b_res = false;

		nodes[end1 - 1].get_phase(&phi1);
		nodes[end2 - 1].get_phase(&phi2);
		nodes[end3 - 1].get_phase(&phi3);

		x2 = double(col2 - col1);
		y2 = double(row1 - row2);
		x1 = double(col1 - col3);
		y1 = double(row3 - row1);
		direction = x1 * y2 - x2 * y1;

		delta12 = atan2(sin(phi2 - phi1), cos(phi2 - phi1));
		delta23 = atan2(sin(phi3 - phi2), cos(phi3 - phi2));
		delta31 = atan2(sin(phi1 - phi3), cos(phi1 - phi3));

		double res = (delta12 + delta23 + delta31) / 2.0 / PI;
		if (fabs(res) > 0.7 && !b_res)//标注边长超过阈值的残差边和残差节点
		{
			(edges + (tri + i)->edge1 - 1)->isResidueEdge = true;
			(edges + (tri + i)->edge2 - 1)->isResidueEdge = true;
			(edges + (tri + i)->edge3 - 1)->isResidueEdge = true;
			nodes[(tri + i)->p1 - 1].set_residue(true);
			nodes[(tri + i)->p2 - 1].set_residue(true);
			nodes[(tri + i)->p3 - 1].set_residue(true);
		}
		res = b_res ? res : 0.0;
		if (direction > 0.0)//在目标三角形中顺残差方向(残差方向定义为逆时针方向)
		{
			(tri + i)->residue = res;
		}
		else
		{
			(tri + i)->residue = -res;
		}
	}
	return 0;
}

int Utils::residue(vector<triangle>& triangle, vector<tri_node>& nodes, vector<tri_edge>& edges, double distance_thresh)
{
	if (triangle.size() < 1 ||
		nodes.size() < 1 ||
		edges.size() < 3
		)
	{
		fprintf(stderr, "residue(): input check failed!\n\n");
		return -1;
	}
	int num_triangle = static_cast<int>(triangle.size());
	double thresh;
	thresh = distance_thresh < 2.0 ? 2.0 : distance_thresh;
	int num_nodes = static_cast<int>(nodes.size());
	int end1, end2, end3, tmp;
	double x1, y1, x2, y2, direction, delta12, delta23, delta31, phi1, phi2, phi3, distance1,
		distance2, distance3;
	// removed unused: x3, y3, residue (triangle residue computed inline, x3/y3 not needed)
	int row1, col1, row2, col2, row3, col3;
	bool b_res = false;
	for (int i = 0; i < num_triangle; i++)
	{
		end1 = triangle[i].p1;
		end2 = triangle[i].p2;
		end3 = triangle[i].p3;
		if (end1 > end2)
		{
			tmp = end1;
			end1 = end2;
			end2 = tmp;
		}

		nodes[end1 - 1].get_pos(&row1, &col1);
		nodes[end2 - 1].get_pos(&row2, &col2);
		nodes[end3 - 1].get_pos(&row3, &col3);

		nodes[end1 - 1].get_distance(nodes[end2 - 1], &distance1);
		nodes[end2 - 1].get_distance(nodes[end3 - 1], &distance2);
		nodes[end3 - 1].get_distance(nodes[end1 - 1], &distance3);
		b_res = true;
		if ((distance1 > thresh) || (distance2 > thresh) || (distance3 > thresh)) b_res = false;

		nodes[end1 - 1].get_phase(&phi1);
		nodes[end2 - 1].get_phase(&phi2);
		nodes[end3 - 1].get_phase(&phi3);

		x2 = double(col2 - col1);
		y2 = -double(row1 - row2);
		x1 = double(col1 - col3);
		y1 = -double(row3 - row1);
		direction = x1 * y2 - x2 * y1;

		delta12 = atan2(sin(phi2 - phi1), cos(phi2 - phi1));
		delta23 = atan2(sin(phi3 - phi2), cos(phi3 - phi2));
		delta31 = atan2(sin(phi1 - phi3), cos(phi1 - phi3));

		double res = (delta12 + delta23 + delta31) / 2.0 / PI;
		if (fabs(res) > 0.7 && !b_res)//标注边长超过阈值的残差边和残差节点
		{
			edges[triangle[i].edge1 - 1].isResidueEdge = true;
			edges[triangle[i].edge2 - 1].isResidueEdge = true;
			edges[triangle[i].edge3 - 1].isResidueEdge = true;

			nodes[triangle[i].p1 - 1].set_residue(true);
			nodes[triangle[i].p2 - 1].set_residue(true);
			nodes[triangle[i].p3 - 1].set_residue(true);
		}
		res = b_res ? res : 0.0;
		if (direction < 0.0)//在目标三角形中顺残差方向(残差方向定义为逆时针方向)
		{
			triangle[i].residue = res;
		}
		else
		{
			triangle[i].residue = -res;
		}
	}
	return 0;
}

int Utils::gen_mask(Mat& coherence, Mat& mask, int wnd_size, double thresh)
{
	if (coherence.rows < 2 ||
		coherence.cols < 2 ||
		coherence.channels() != 1 ||
		coherence.type() != CV_64F ||
		wnd_size < 0 ||
		wnd_size > coherence.rows ||
		thresh < 0.0 ||
		thresh > 1.0
		)
	{
		fprintf(stderr, "gen_mask(): input check failed!\n\n");
		return -1;
	}
	int nr = coherence.rows;
	int nc = coherence.cols;
	int radius = (wnd_size + 1) / 2;
	int kernel_size = 2 * radius + 1;

	Mat mean_coh;
	cv::boxFilter(coherence, mean_coh, CV_64F, cv::Size(kernel_size, kernel_size), cv::Point(-1, -1), true, cv::BORDER_REFLECT);

	Mat tmp = Mat::zeros(nr, nc, CV_32S);
	tmp.copyTo(mask);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double mean = mean_coh.at<double>(i, j);
			if (mean > thresh && coherence.at<double>(i, j) > thresh) mask.at<int>(i, j) = 1;
		}
	}
	return 0;
}

int Utils::gen_mask_pdv(Mat& phase_derivatives_variance, Mat& mask, int wndsize, double thresh)
{
	if (phase_derivatives_variance.rows < 2 ||
		phase_derivatives_variance.cols < 2 ||
		phase_derivatives_variance.channels() != 1 ||
		phase_derivatives_variance.type() != CV_64F ||
		wndsize < 0 ||
		wndsize > phase_derivatives_variance.rows ||
		thresh < 0.0 ||
		thresh > 1.0
		)
	{
		fprintf(stderr, "gen_mask(): input check failed!\n\n");
		return -1;
	}
	int nr = phase_derivatives_variance.rows;
	int nc = phase_derivatives_variance.cols;
	int radius = (wndsize + 1) / 2;
	int kernel_size = 2 * radius + 1;

	Mat mean_pdv;
	cv::boxFilter(phase_derivatives_variance, mean_pdv, CV_64F, cv::Size(kernel_size, kernel_size), cv::Point(-1, -1), true, cv::BORDER_REFLECT);

	Mat tmp = Mat::zeros(nr, nc, CV_32S);
	tmp.copyTo(mask);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double mean = mean_pdv.at<double>(i, j);
			if (mean < thresh && phase_derivatives_variance.at<double>(i, j) < thresh) mask.at<int>(i, j) = 1;
		}
	}
	return 0;
}

int Utils::real_coherence(ComplexMat& Mast, ComplexMat& Slave, Mat& coherence, NewtonProgressCallback cb)
{
	int wa = 3;  //窗口方位向尺寸
	int wr = 3;  //窗口距离向尺寸

	int na = Mast.GetRows();
	int nr = Mast.GetCols();
	if ((na < 3) ||
		(nr < 3) ||
		Mast.re.type() != CV_64F ||
		Slave.re.type() != CV_64F ||
		Mast.GetCols() != Slave.GetCols()||
		Mast.GetRows() != Slave.GetRows())
	{
		fprintf(stderr, "real_coherence(): input check failed!\n\n");
		return -1;
	}

	int win_a = (wa - 1) / 2; //方位窗半径
	int win_r = (wr - 1) / 2; //距离窗半径

	int na_new = na - 2 * win_a;
	int nr_new = nr - 2 * win_r;

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, na_new / 100);

	Mat Coherence(na_new, nr_new, CV_64F, Scalar::all(0));

#pragma omp parallel for schedule(guided)
	for (int i = win_a + 1; i <= na - win_a; i++)
	{
		if (cancel_flag) continue;
		for (int j = win_r + 1; j <= nr - win_r; j++)
		{
			Mat s1, s2, sum1, sum2;

			double up, down;
			magnitude(Mast.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), Mast.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), s1);
			magnitude(Slave.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), Slave.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), s2);
			up = sum((s1.mul(s1)).mul(s2.mul(s2)))[0];
			pow(s1, 4, s1);
			pow(s2, 4, s2);
			down = sqrt(sum(s1)[0] * sum(s2)[0]);
			if (up / (down + 1e-12) > 1.0)
			{
				Coherence.at<double>(i - 1 - win_a, j - 1 - win_r) = 1;
			}
			else
			{
				Coherence.at<double>(i - 1 - win_a, j - 1 - win_r) = up / (down + 1e-12);
			}

		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / na_new, "Computing real coherence (3x3)..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	copyMakeBorder(Coherence, Coherence, 1, 1, 1, 1, BORDER_REFLECT);
	coherence = Coherence;
	return 0;
}

int Utils::real_coherence(const ComplexMat& master_image, const ComplexMat& slave_image, int est_wndsize_rg, int est_wndsize_az, Mat& coherence, NewtonProgressCallback cb)
{
	int na = master_image.GetRows();
	int nr = master_image.GetCols();
	if ((na < est_wndsize_az) ||
		(nr < est_wndsize_rg) ||
		master_image.type() != CV_64F ||
		slave_image.type() != CV_64F ||
		master_image.GetCols() != slave_image.GetCols() ||
		master_image.GetRows() != slave_image.GetRows() ||
		est_wndsize_rg % 2 == 0||
		est_wndsize_az % 2 == 0 ||
		est_wndsize_rg < 3 ||
		est_wndsize_az < 3
		)
	{
		fprintf(stderr, "real_coherence(): input check failed!\n\n");
		return -1;
	}

	int win_a = (est_wndsize_az - 1) / 2; //方位窗半径
	int win_r = (est_wndsize_rg - 1) / 2; //距离窗半径

	int na_new = na - 2 * win_a;
	int nr_new = nr - 2 * win_r;

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, na_new / 100);

	Mat Coherence(na_new, nr_new, CV_64F, Scalar::all(0));

#pragma omp parallel for schedule(guided)
	for (int i = win_a + 1; i <= na - win_a; i++)
	{
		if (cancel_flag) continue;
		for (int j = win_r + 1; j <= nr - win_r; j++)
		{
			Mat s1, s2, sum1, sum2;

			double up, down;
			magnitude(master_image.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), master_image.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), s1);
			magnitude(slave_image.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), slave_image.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)), s2);
			up = sum((s1.mul(s1)).mul(s2.mul(s2)))[0];
			pow(s1, 4, s1);
			pow(s2, 4, s2);
			down = sqrt(sum(s1)[0] * sum(s2)[0]);
			if (up / (down + 1e-12) > 1.0)
			{
				Coherence.at<double>(i - 1 - win_a, j - 1 - win_r) = 1;
			}
			else
			{
				Coherence.at<double>(i - 1 - win_a, j - 1 - win_r) = up / (down + 1e-12);
			}

		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / na_new, "Computing real coherence..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	copyMakeBorder(Coherence, Coherence, win_a, win_a, win_r, win_r, BORDER_REFLECT);
	Coherence.copyTo(coherence);
	return 0;
}

int Utils::complex_coherence(ComplexMat& Mast, ComplexMat& Slave, Mat& coherence, NewtonProgressCallback cb)
{
	int wa = 3;  //窗口方位向尺寸
	int wr = 3;  //窗口距离向尺寸

	int na = Mast.GetRows();
	int nr = Mast.GetCols();

	if ((na < 3) ||
		(nr < 3) ||
		Mast.re.type() != CV_64F ||
		Slave.re.type() != CV_64F ||
		Mast.GetCols() != Slave.GetCols() ||
		Mast.GetRows() != Slave.GetRows())
	{
		fprintf(stderr, "complex_coherence(): input check failed!\n\n");
		return -1;
	}

	int win_a = (wa - 1) / 2; //方位窗半径
	int win_r = (wr - 1) / 2; //距离窗半径

	int na_new = na - 2 * win_a;
	int nr_new = nr - 2 * win_r;

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> max_reported_pct(0);
	int step = std::max(1, na_new / 100);

	Mat Coherence(na_new, nr_new, CV_64F, Scalar::all(0));
#pragma omp parallel for schedule(guided)
	for (int i = win_a + 1; i <= na - win_a; i++)
	{
		if (cancel_flag) continue;
		for (int j = win_r + 1; j <= nr - win_r; j++)
		{
			Mat planes_master[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F) };
			Mat planes_slave[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F) };
			Mat planes[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F) };
			Mat s1, s2;
			double up, down, sum1, sum2;
			Mast.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_master[0]);
			Mast.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_master[1]);

			Slave.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_slave[0]);
			Slave.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_slave[1]);

			merge(planes_master, 2, s1);
			merge(planes_slave, 2, s2);
			mulSpectrums(s1, s2, s1, 0, true);
			split(s1, planes);
			sum1 = sum(planes[0])[0];
			sum2 = sum(planes[1])[0];
			up = sqrt(sum1 * sum1 + sum2 * sum2);
			magnitude(planes_master[0], planes_master[1], planes_master[0]);
			magnitude(planes_slave[0], planes_slave[1], planes_slave[0]);
			sum1 = sum(planes_master[0].mul(planes_master[0]))[0];
			sum2 = sum(planes_slave[0].mul(planes_slave[0]))[0];
			down = sqrt(sum1 * sum2);
			Coherence.at<double>(i - 1 - win_a, j - 1 - win_r) = up / (down + 0.0000001);
		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			int current_pct = current * 100 / na_new;
			int prev = max_reported_pct.load();
			while (current_pct > prev && !max_reported_pct.compare_exchange_weak(prev, current_pct))
			{
			}
			if (current_pct > prev)
			{
				#pragma omp critical(coherence_progress_lock)
				{
					if (!cb(current_pct, "Computing complex coherence (3x3)..."))
					{
						cancel_flag = true;
					}
				}
			}
		}
	}
	if (cancel_flag) return -2;
	copyMakeBorder(Coherence, Coherence, 1, 1, 1, 1, BORDER_REFLECT);
	coherence = Coherence;
	return 0;
}

int Utils::complex_coherence(
	const ComplexMat& master_image, 
	const ComplexMat& slave_image,
	int est_wndsize_rg, 
	int est_wndsize_az,
	Mat& coherence,
	NewtonProgressCallback cb
)
{
	int na = master_image.GetRows();
	int nr = master_image.GetCols();

	if ((na < est_wndsize_az) ||
		(nr < est_wndsize_rg) ||
		master_image.type() != slave_image.type() ||
		(slave_image.type() != CV_64F && slave_image.type() != CV_32F) ||
		master_image.GetCols() != slave_image.GetCols() ||
		master_image.GetRows() != slave_image.GetRows() ||
		est_wndsize_az % 2 == 0||
		est_wndsize_rg % 2 == 0||
		est_wndsize_rg < 3||
		est_wndsize_az < 3
		)
	{
		fprintf(stderr, "complex_coherence(): input check failed!\n\n");
		return -1;
	}

	int win_a = (est_wndsize_az - 1) / 2; //方位窗半径
	int win_r = (est_wndsize_rg - 1) / 2; //距离窗半径

	int na_new = na - 2 * win_a;
	int nr_new = nr - 2 * win_r;

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> max_reported_pct(0);
	int step = std::max(1, na_new / 100);

	if (master_image.type() == CV_64F)
	{
		Mat Coherence(na_new, nr_new, CV_64F, Scalar::all(0));
#pragma omp parallel for schedule(guided)
		for (int i = win_a + 1; i <= na - win_a; i++)
		{
			if (cancel_flag) continue;
			for (int j = win_r + 1; j <= nr - win_r; j++)
			{
				Mat planes_master[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F) };
				Mat planes_slave[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F) };
				Mat planes[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_64F) };
				Mat s1, s2;
				double up, down, sum1, sum2;
				master_image.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_master[0]);
				master_image.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_master[1]);

				slave_image.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_slave[0]);
				slave_image.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_slave[1]);

				merge(planes_master, 2, s1);
				merge(planes_slave, 2, s2);
				mulSpectrums(s1, s2, s1, 0, true);
				split(s1, planes);
				sum1 = sum(planes[0])[0];
				sum2 = sum(planes[1])[0];
				up = sqrt(sum1 * sum1 + sum2 * sum2);
				magnitude(planes_master[0], planes_master[1], planes_master[0]);
				magnitude(planes_slave[0], planes_slave[1], planes_slave[0]);
				sum1 = sum(planes_master[0].mul(planes_master[0]))[0];
				sum2 = sum(planes_slave[0].mul(planes_slave[0]))[0];
				down = sqrt(sum1 * sum2);
				Coherence.at<double>(i - 1 - win_a, j - 1 - win_r) = up / (down + 0.0000001);
			}

			int current = ++completed_rows;
			if (cb && current % step == 0)
			{
				int current_pct = current * 100 / na_new;
				int prev = max_reported_pct.load();
				while (current_pct > prev && !max_reported_pct.compare_exchange_weak(prev, current_pct))
				{
				}
				if (current_pct > prev)
				{
					#pragma omp critical(coherence_progress_64f_lock)
					{
						if (!cb(current_pct, "Computing complex coherence (64F)..."))
						{
							cancel_flag = true;
						}
					}
				}
			}
		}
		if (cancel_flag) return -2;
		copyMakeBorder(Coherence, Coherence, win_a, win_a, win_r, win_r, BORDER_REFLECT);
		Coherence.copyTo(coherence);
	}
	else
	{
		Mat Coherence(na_new, nr_new, CV_32F, Scalar::all(0));
#pragma omp parallel for schedule(guided)
		for (int i = win_a + 1; i <= na - win_a; i++)
		{
			if (cancel_flag) continue;
			for (int j = win_r + 1; j <= nr - win_r; j++)
			{
				Mat planes_master[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_32F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_32F) };
				Mat planes_slave[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_32F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_32F) };
				Mat planes[] = { Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_32F), Mat::zeros(2 * win_a + 1, 2 * win_r + 1, CV_32F) };
				Mat s1, s2;
				double up, down, sum1, sum2;
				master_image.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_master[0]);
				master_image.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_master[1]);

				slave_image.re(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_slave[0]);
				slave_image.im(Range(i - 1 - win_a, i + win_a), Range(j - 1 - win_r, j + win_r)).copyTo(planes_slave[1]);

				merge(planes_master, 2, s1);
				merge(planes_slave, 2, s2);
				mulSpectrums(s1, s2, s1, 0, true);
				split(s1, planes);
				sum1 = sum(planes[0])[0];
				sum2 = sum(planes[1])[0];
				up = sqrt(sum1 * sum1 + sum2 * sum2);
				magnitude(planes_master[0], planes_master[1], planes_master[0]);
				magnitude(planes_slave[0], planes_slave[1], planes_slave[0]);
				sum1 = sum(planes_master[0].mul(planes_master[0]))[0];
				sum2 = sum(planes_slave[0].mul(planes_slave[0]))[0];
				down = sqrt(sum1 * sum2);
				Coherence.at<float>(i - 1 - win_a, j - 1 - win_r) = static_cast<float>(up / (down + 0.0000001));
			}

			int current = ++completed_rows;
			if (cb && current % step == 0)
			{
				int current_pct = current * 100 / na_new;
				int prev = max_reported_pct.load();
				while (current_pct > prev && !max_reported_pct.compare_exchange_weak(prev, current_pct))
				{
				}
				if (current_pct > prev)
				{
					#pragma omp critical(coherence_progress_32f_lock)
					{
						if (!cb(current_pct, "Computing complex coherence (32F)..."))
						{
							cancel_flag = true;
						}
					}
				}
			}
		}
		if (cancel_flag) return -2;
		copyMakeBorder(Coherence, Coherence, win_a, win_a, win_r, win_r, BORDER_REFLECT);
		Coherence.copyTo(coherence);
	}
	
	return 0;
}

int Utils::phase_coherence(Mat& phase, Mat& coherence, NewtonProgressCallback cb)
{
	return phase_axial_concentration(phase, coherence, cb);
}

int Utils::phase_axial_concentration(Mat& phase, Mat& coherence, NewtonProgressCallback cb)
{
	if (phase.rows < 3 ||
		phase.cols < 3 ||
		phase.type() != CV_64F ||
		phase.channels() != 1)
	{
		fprintf(stderr, "phase_coherence(): input check failed!\n\n");
		return -1;
	}
	ComplexMat master, slave;
	Mat cos, sin;
	int ret;
	ret = this->phase2cos(phase, cos, sin);
	if (return_check(ret, "phase2cos(*, *, *)", error_head)) return -1;
	master.SetRe(cos);
	slave.SetRe(cos);
	master.SetIm(sin);
	sin = -sin;
	slave.SetIm(sin);
	ret = this->complex_coherence(master, slave, coherence, cb);
	if (ret == -2) return -2;
	if (return_check(ret, "complex_coherence(*, *, *)", error_head)) return -1;
	return 0;
}

int Utils::phase_coherence(const Mat& phase, int est_wndsize_rg, int est_wndsize_az, Mat& coherence, NewtonProgressCallback cb)
{
	return phase_axial_concentration(phase, est_wndsize_rg, est_wndsize_az, coherence, cb);
}

int Utils::phase_axial_concentration(const Mat& phase, int est_wndsize_rg, int est_wndsize_az, Mat& coherence, NewtonProgressCallback cb)
{
	if (phase.rows < 3 ||
		phase.cols < 3 ||
		phase.type() != CV_64F ||
		phase.channels() != 1 ||
		est_wndsize_rg % 2 == 0||
		est_wndsize_az % 2 == 0
		)
	{
		fprintf(stderr, "phase_coherence(): input check failed!\n\n");
		return -1;
	}
	ComplexMat master, slave;
	Mat cos, sin;
	int ret;
	ret = phase2cos(phase, cos, sin);
	if (return_check(ret, "phase2cos(*, *, *)", error_head)) return -1;
	master.SetRe(cos);
	slave.SetRe(cos);
	master.SetIm(sin);
	sin = -sin;
	slave.SetIm(sin);
	ret = complex_coherence(master, slave, est_wndsize_rg, est_wndsize_az, coherence, cb);
	if (ret == -2) return -2;
	if (return_check(ret, "complex_coherence(*, *, *)", error_head)) return -1;
	return 0;
}

int Utils::phase_circular_concentration(const Mat& phase, Mat& concentration, NewtonProgressCallback cb)
{
	if (phase.rows < 3 ||
		phase.cols < 3 ||
		phase.type() != CV_64F ||
		phase.channels() != 1)
	{
		fprintf(stderr, "phase_circular_concentration(): input check failed!\n\n");
		return -1;
	}
	ComplexMat master, slave;
	Mat cos, sin;
	int ret;
	ret = phase2cos(phase, cos, sin);
	if (return_check(ret, "phase2cos(*, *, *)", error_head)) return -1;
	// 主图像取单位复数 exp(i*phi)；辅图像取常量 1+0i。
	// 此时 complex_coherence() 的分子为 |sum(exp(i*phi))|，分母为 sqrt(N*N)=N，
	// 故输出恰为一阶圆统计集中度 R1 = |mean(exp(i*phi))|。
	// 注：SetRe()/SetIm() 形参为非 const 引用，常量场须先落地为具名局部变量。
	Mat slave_re = Mat::ones(phase.rows, phase.cols, CV_64F);
	Mat slave_im = Mat::zeros(phase.rows, phase.cols, CV_64F);
	master.SetRe(cos);
	master.SetIm(sin);
	slave.SetRe(slave_re);
	slave.SetIm(slave_im);
	ret = complex_coherence(master, slave, concentration, cb);
	if (ret == -2) return -2;
	if (return_check(ret, "complex_coherence(*, *, *)", error_head)) return -1;
	return 0;
}

int Utils::phase_circular_concentration(
	const Mat& phase,
	int est_wndsize_rg,
	int est_wndsize_az,
	Mat& concentration,
	NewtonProgressCallback cb
)
{
	if (phase.rows < 3 ||
		phase.cols < 3 ||
		phase.type() != CV_64F ||
		phase.channels() != 1 ||
		est_wndsize_rg % 2 == 0 ||
		est_wndsize_az % 2 == 0
		)
	{
		fprintf(stderr, "phase_circular_concentration(): input check failed!\n\n");
		return -1;
	}
	ComplexMat master, slave;
	Mat cos, sin;
	int ret;
	ret = phase2cos(phase, cos, sin);
	if (return_check(ret, "phase2cos(*, *, *)", error_head)) return -1;
	// 构造同上：辅图像为常量 1+0i，输出为一阶圆统计集中度 R1。
	// 注：SetRe()/SetIm() 形参为非 const 引用，常量场须先落地为具名局部变量。
	Mat slave_re = Mat::ones(phase.rows, phase.cols, CV_64F);
	Mat slave_im = Mat::zeros(phase.rows, phase.cols, CV_64F);
	master.SetRe(cos);
	master.SetIm(sin);
	slave.SetRe(slave_re);
	slave.SetIm(slave_im);
	ret = complex_coherence(master, slave, est_wndsize_rg, est_wndsize_az, concentration, cb);
	if (ret == -2) return -2;
	if (return_check(ret, "complex_coherence(*, *, *)", error_head)) return -1;
	return 0;
}

int Utils::complex_coherence_multilooked(
	const ComplexMat& master_image,
	const ComplexMat& slave_image,
	const Mat& input_valid_mask,
	int multilook_rg,
	int multilook_az,
	int est_wndsize_rg,
	int est_wndsize_az,
	Mat& coherence,
	Mat& valid_mask,
	Mat& valid_sample_count,
	NewtonProgressCallback cb
)
{
	const int nr = master_image.GetRows();
	const int nc = master_image.GetCols();
	if (nr < 1 || nc < 1)
	{
		coherence.release();
		valid_mask.release();
		valid_sample_count.release();
		fprintf(stderr, "complex_coherence_multilooked(): input check failed!\n\n");
		return -1;
	}
	Mat zero_reference(nr, nc, CV_64F, Scalar::all(0));
	return complex_coherence_demodulated(
		master_image, slave_image, zero_reference, input_valid_mask,
		multilook_rg, multilook_az, est_wndsize_rg, est_wndsize_az,
		coherence, valid_mask, valid_sample_count, cb);
}

int Utils::complex_coherence_demodulated(
	const ComplexMat& master_image,
	const ComplexMat& slave_image,
	const Mat& reference_phase,
	const Mat& input_valid_mask,
	int multilook_rg,
	int multilook_az,
	int est_wndsize_rg,
	int est_wndsize_az,
	Mat& coherence,
	Mat& valid_mask,
	Mat& valid_sample_count,
	NewtonProgressCallback cb
)
{
	coherence.release();
	valid_mask.release();
	valid_sample_count.release();
	const int nr = master_image.GetRows();
	const int nc = master_image.GetCols();
	if (nr < 1 || nc < 1 ||
		master_image.re.rows != nr || master_image.re.cols != nc ||
		master_image.im.rows != nr || master_image.im.cols != nc ||
		slave_image.re.rows != nr || slave_image.re.cols != nc ||
		slave_image.im.rows != nr || slave_image.im.cols != nc ||
		master_image.re.type() != master_image.im.type() ||
		slave_image.re.type() != slave_image.im.type() ||
		slave_image.GetRows() != nr ||
		slave_image.GetCols() != nc ||
		master_image.type() != slave_image.type() ||
		(master_image.type() != CV_64F && master_image.type() != CV_32F) ||
		multilook_rg < 1 || multilook_az < 1 ||
		nr < multilook_az || nc < multilook_rg ||
		est_wndsize_rg < 3 || est_wndsize_az < 3 ||
		est_wndsize_rg % 2 == 0 || est_wndsize_az % 2 == 0
		)
	{
		fprintf(stderr, "complex_coherence_demodulated(): input check failed!\n\n");
		return -1;
	}
	if (reference_phase.empty() ||
		reference_phase.rows != nr ||
		 reference_phase.cols != nc ||
		 reference_phase.type() != CV_64F ||
		 reference_phase.channels() != 1 ||
		input_valid_mask.empty() ||
		input_valid_mask.rows != nr ||
		input_valid_mask.cols != nc ||
		input_valid_mask.type() != CV_8U ||
		input_valid_mask.channels() != 1)
	{
		fprintf(stderr, "complex_coherence_demodulated(): reference phase or valid mask check failed!\n\n");
		return -1;
	}

	const int nr_new = nr / multilook_az;
	const int nc_new = nc / multilook_rg;
	const bool is64 = (master_image.type() == CV_64F);

	// 第一阶段：在多视网格上按“带幅度”方式聚合已解调复干涉量与主辅功率。
	// 注意不可先在原始网格估计 gamma 再下采样，二者不等价。
	Mat sumRe(nr_new, nc_new, CV_64F, Scalar::all(0));
	Mat sumIm(nr_new, nc_new, CV_64F, Scalar::all(0));
	Mat powM(nr_new, nc_new, CV_64F, Scalar::all(0));
	Mat powS(nr_new, nc_new, CV_64F, Scalar::all(0));
	Mat sampleCount(nr_new, nc_new, CV_32S, Scalar::all(0));

	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	std::atomic<int> max_reported_pct(0);
	const int step_a = std::max(1, nr_new / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr_new; i++)
	{
		if (cancel_flag) continue;
		double* pRe = sumRe.ptr<double>(i);
		double* pIm = sumIm.ptr<double>(i);
		double* pM = powM.ptr<double>(i);
		double* pS = powS.ptr<double>(i);
		int* pCount = sampleCount.ptr<int>(i);
		const int top = i * multilook_az;
		const int bottom = std::min(top + multilook_az, nr);
		for (int r = top; r < bottom; r++)
		{
			const double* refRow = reference_phase.ptr<double>(r);
			const uchar* inputMaskRow = input_valid_mask.ptr<uchar>(r);
			for (int j = 0; j < nc_new; j++)
			{
				const int left = j * multilook_rg;
				const int right = std::min(left + multilook_rg, nc);
				double accRe = 0.0, accIm = 0.0, accM = 0.0, accS = 0.0;
				int accCount = 0;
				for (int c = left; c < right; c++)
				{
					if (inputMaskRow[c] == 0) continue;
					double mr, mi, sr, si;
					if (is64)
					{
						mr = master_image.re.at<double>(r, c); mi = master_image.im.at<double>(r, c);
						sr = slave_image.re.at<double>(r, c);  si = slave_image.im.at<double>(r, c);
					}
					else
					{
						mr = master_image.re.at<float>(r, c); mi = master_image.im.at<float>(r, c);
						sr = slave_image.re.at<float>(r, c);  si = slave_image.im.at<float>(r, c);
					}
					const double ref = refRow[c];
					if (!std::isfinite(mr) || !std::isfinite(mi) ||
						!std::isfinite(sr) || !std::isfinite(si) || !std::isfinite(ref))
					{
						continue;
					}
					// 干涉量取 M*conj(S)：实部 Mr*Sr+Mi*Si，虚部 Sr*Mi-Mr*Si，
					// 与 Multilook()/generate_phase() 写入 H5 的 phase 定义严格一致
					double vRe = mr * sr + mi * si;
					double vIm = sr * mi - mr * si;
					// 乘以 exp(-i*phi_ref)，phi_ref 须与上述 phase 同约定
					const double cosRef = std::cos(ref);
					const double sinRef = std::sin(ref);
					const double dRe = vRe * cosRef + vIm * sinRef;
					const double dIm = vIm * cosRef - vRe * sinRef;
					vRe = dRe; vIm = dIm;
					accRe += vRe;
					accIm += vIm;
					accM += mr * mr + mi * mi;
					accS += sr * sr + si * si;
					++accCount;
				}
				pRe[j] += accRe; pIm[j] += accIm;
				pM[j] += accM;   pS[j] += accS;
				pCount[j] += accCount;
			}
		}

		const int current = ++completed_rows;
		if (cb && current % step_a == 0)
		{
			const int current_pct = current * 50 / nr_new;
			int prev = max_reported_pct.load();
			while (current_pct > prev && !max_reported_pct.compare_exchange_weak(prev, current_pct))
			{
			}
			if (current_pct > prev)
			{
				#pragma omp critical(coherence_demod_progress_lock)
				{
					if (!cb(current_pct, "Aggregating demodulated interferogram..."))
					{
						cancel_flag = true;
					}
				}
			}
		}
	}
	if (cancel_flag) return -2;

	// 第二阶段：在多视网格上做局部相干估计。
	// 边界采用窗口裁剪（部分窗口），不使用 BORDER_REFLECT，避免边缘估计值被重复计入。
	coherence.create(nr_new, nc_new, CV_64F);
	valid_mask.create(nr_new, nc_new, CV_8U);
	valid_sample_count.create(nr_new, nc_new, CV_32S);
	const int win_a = (est_wndsize_az - 1) / 2;
	const int win_r = (est_wndsize_rg - 1) / 2;
	completed_rows = 0;
	const int step_b = std::max(1, nr_new / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr_new; i++)
	{
		if (cancel_flag) continue;
		double* cohRow = coherence.ptr<double>(i);
		uchar* maskRow = valid_mask.ptr<uchar>(i);
		int* supportRow = valid_sample_count.ptr<int>(i);
		const int top = std::max(0, i - win_a);
		const int bottom = std::min(nr_new - 1, i + win_a);
		for (int j = 0; j < nc_new; j++)
		{
			const int left = std::max(0, j - win_r);
			const int right = std::min(nc_new - 1, j + win_r);
			double NRe = 0.0, NIm = 0.0, PM = 0.0, PS = 0.0;
			int support = 0;
			for (int r = top; r <= bottom; r++)
			{
				const double* pRe = sumRe.ptr<double>(r);
				const double* pIm = sumIm.ptr<double>(r);
				const double* pM = powM.ptr<double>(r);
				const double* pS = powS.ptr<double>(r);
				const int* pCount = sampleCount.ptr<int>(r);
				for (int c = left; c <= right; c++)
				{
					NRe += pRe[c]; NIm += pIm[c];
					PM += pM[c];   PS += pS[c];
					support += pCount[c];
				}
			}
			supportRow[j] = support;
			const double denom = std::sqrt(PM * PS);
			const double numeratorSquared = NRe * NRe + NIm * NIm;
			if (support <= 0 || !std::isfinite(denom) || !(denom > 0.0) ||
				!std::isfinite(numeratorSquared) || numeratorSquared < 0.0)
			{
				// 无支持、零能量或非有限窗口均显式置无效。
				cohRow[j] = 0.0;
				maskRow[j] = 0;
				continue;
			}
			double g = std::sqrt(numeratorSquared) / denom;
			if (!std::isfinite(g))
			{
				cohRow[j] = 0.0;
				maskRow[j] = 0;
				continue;
			}
			if (g > 1.0) g = 1.0;  // 浮点误差保护（Cauchy-Schwarz 保证理论上不超过 1）
			cohRow[j] = g;
			maskRow[j] = 1;
		}

		const int current = ++completed_rows;
		if (cb && current % step_b == 0)
		{
			const int current_pct = 50 + current * 50 / nr_new;
			int prev = max_reported_pct.load();
			while (current_pct > prev && !max_reported_pct.compare_exchange_weak(prev, current_pct))
			{
			}
			if (current_pct > prev)
			{
				#pragma omp critical(coherence_demod_progress_lock)
				{
					if (!cb(current_pct, "Estimating demodulated complex coherence..."))
					{
						cancel_flag = true;
					}
				}
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Utils::phase_derivatives_variance(Mat& phase, Mat& phase_derivatives_variance, int wndsize)
{
	if (phase.cols < 2 ||
		phase.rows < 2 ||
		phase.type() != CV_64F ||
		phase.channels() != 1||
		wndsize < 3||
		wndsize % 2 == 0
		)
	{
		fprintf(stderr, "phase_derivatives_variance(): input check failed!\n\n");
		return -1;
	}
	
	int nr = phase.rows;
	int nc = phase.cols;
	if (wndsize > int(nr / 10) || wndsize > int(nc / 10)) wndsize = 3;
	wndsize = (wndsize - 1) / 2;
	int ret;
	phase.copyTo(phase_derivatives_variance);
	Mat derivative_row, derivative_col;
	derivative_row = phase(Range(1, nr), Range(0, nc)) - phase(Range(0, nr - 1), Range(0, nc));
	derivative_col = phase(Range(0, nr), Range(1, nc)) - phase(Range(0, nr), Range(0, nc - 1));
	ret = wrap(derivative_row, derivative_row);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	ret = wrap(derivative_col, derivative_col);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	copyMakeBorder(derivative_col, derivative_col, 0, 1, 0, 1, BORDER_DEFAULT);
	copyMakeBorder(derivative_row, derivative_row, 0, 1, 0, 1, BORDER_DEFAULT);

	int N = 2 * wndsize + 1;
	double M = N * N;
	double sqrt_M = sqrt(M);

	Mat derivative_row_sq = derivative_row.mul(derivative_row);
	Mat derivative_col_sq = derivative_col.mul(derivative_col);

	Mat mean_row, mean_row_sq;
	Mat mean_col, mean_col_sq;

	cv::boxFilter(derivative_row, mean_row, CV_64F, cv::Size(N, N), cv::Point(-1, -1), true, cv::BORDER_DEFAULT);
	cv::boxFilter(derivative_row_sq, mean_row_sq, CV_64F, cv::Size(N, N), cv::Point(-1, -1), true, cv::BORDER_DEFAULT);
	cv::boxFilter(derivative_col, mean_col, CV_64F, cv::Size(N, N), cv::Point(-1, -1), true, cv::BORDER_DEFAULT);
	cv::boxFilter(derivative_col_sq, mean_col_sq, CV_64F, cv::Size(N, N), cv::Point(-1, -1), true, cv::BORDER_DEFAULT);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double m_r = mean_row.at<double>(i, j);
			double m_r2 = mean_row_sq.at<double>(i, j);
			double m_c = mean_col.at<double>(i, j);
			double m_c2 = mean_col_sq.at<double>(i, j);

			double var_r = m_r2 - m_r * m_r;
			double var_c = m_c2 - m_c * m_c;

			double std1 = (var_r > 0.0) ? sqrt(var_r) : 0.0;
			double std2 = (var_c > 0.0) ? sqrt(var_c) : 0.0;

			phase_derivatives_variance.at<double>(i, j) = (std1 + std2) / sqrt_M;
		}
	}
	return 0;
}

int Utils::fftshift(Mat& mag)
{
	// rearrange the quadrants of Fourier image
	// so that the origin is at the image center
	if (mag.rows < 2 ||
		mag.cols < 2 ||
		mag.channels() != 1)
	{
		fprintf(stderr, "fftshift(): input check failed!\n\n");
		return -1;
	}
	mag = mag(Rect(0, 0, mag.cols & -2, mag.rows & -2));
	int cx = mag.cols / 2;
	int cy = mag.rows / 2;
	Mat tmp;
	Mat q0(mag, Rect(0, 0, cx, cy));
	Mat q1(mag, Rect(cx, 0, cx, cy));
	Mat q2(mag, Rect(0, cy, cx, cy));
	Mat q3(mag, Rect(cx, cy, cx, cy));

	q0.copyTo(tmp);
	q3.copyTo(q0);
	tmp.copyTo(q3);

	q1.copyTo(tmp);
	q2.copyTo(q1);
	tmp.copyTo(q2);
	return 0;
}

int Utils::read_DIMACS(const char* DIMACS_file_solution, tri_edge* edges, int num_edges, vector<tri_node>& nodes, triangle* tri, int num_triangle)
{
	if (DIMACS_file_solution == NULL ||
		edges == NULL ||
		num_edges < 3 ||
		nodes.size() < 3 ||
		tri == NULL ||
		num_triangle < 1
		)
	{
		fprintf(stderr, "read_DIMACS(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_file_solution, L"rt");
	if (fp == NULL)
	{
		fprintf(stderr, "read_DIMACS(): can't open %s \n", DIMACS_file_solution);
		return -1;
	}

	char instring[256];
	char ch;
	double obj_value = 0;
	int i, tmp, end1, end2, end3, row1, col1, row2, col2, row3, col3;
	int end[3];
	double x1, y1, x2, y2, direction;
	long from, to;
	double flow = 0;
	bool flag;
	int x[3];
	int y[3];
	int target_edges;
	int num_nodes = static_cast<int>(nodes.size());
	/////////////////////读取注释///////////////////////////
	GET_NEXT_LINE;
	while (ch != 's' && ch)
	{
		if (ch != 'c')
		{
			if (fp) fclose(fp);
			fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
			return -1;
		}
		GET_NEXT_LINE;
	}
	/////////////////////读取优化目标值/////////////////////
	for (i = 1; i < 81; i++)
	{
		if (isspace((int)instring[i]) > 0)
		{
			i++;
			break;
		}
	}
	if (sscanf(&(instring[i]), "%lf", &obj_value) != 1)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
		return -1;
	}
	if (obj_value < 0.0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): this problem can't be solved(unbounded or infeasible)!\n\n");
		return -1;
	}
	////////////////////////读取MCF结果////////////////////////
	GET_NEXT_LINE;
	while (ch && ch == 'f')
	{
		if (sscanf(&(instring[2]), "%ld %ld %lf", &from, &to, &flow) != 3 ||
			flow < 0.0 || from < 0 || to < 0)
		{
			if (fp) fclose(fp);
			fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
			return -1;
		}

		if (from > 0 &&
			from <= num_triangle &&
			to > 0 &&
			to <= num_triangle)
		{
			if (/*from > 0 &&
				from <= num_triangle &&
				to > 0 &&
				to <= num_triangle &&*/
				(tri + from - 1) != NULL &&
				(tri + to - 1) != NULL
				)
			{
				end[0] = -1;
				end[1] = -1;
				x[0] = (tri + from - 1)->p1;
				x[1] = (tri + from - 1)->p2;
				x[2] = (tri + from - 1)->p3;
				y[0] = (tri + to - 1)->p1;
				y[1] = (tri + to - 1)->p2;
				y[2] = (tri + to - 1)->p3;
				i = 0; tmp = 0; flag = false;
				while (end[0] == -1 || end[1] == -1)
				{
					if (i > 2)
					{
						if (fp) fclose(fp);
						fprintf(stderr, "read_DIMACS(): illegal Delaunay triangle!\n\n");
						return -1;
					}
					for (int j = 0; j < 3; j++)
					{
						if (x[i] == y[j])
						{
							end[tmp] = x[i];
							tmp++;
							if (i == 0) flag = true;
							break;
						}
					}
					i++;

				}
			}
			if (i == 3)
			{
				if (flag) end[2] = x[1];
				else
				{
					end[2] = x[0];
				}
			}
			else
			{
				end[2] = x[2];
			}
			if (end[0] > end[1])
			{
				end1 = end[1];
				end2 = end[0];
			}
			else
			{
				end1 = end[0];
				end2 = end[1];
			}
			end3 = end[2];




			if (end1 > 0 && end1 <= num_nodes && end2 > 0 && end2 <= num_nodes && end3 > 0 && end3 <= num_nodes)
			{
				//找到边序号target_edges
				const std::vector<long>& neigh_edges = nodes[end1 - 1].get_neigh_edges();
				for (i = 0; i < neigh_edges.size(); i++)
				{
					long edge_val = neigh_edges[i];
					if (edge_val > 0 && edge_val <= num_edges)
					{
						if ((edges + edge_val - 1)->end1 == end2 || (edges + edge_val - 1)->end2 == end2)
						{
							target_edges = edge_val;
						}
					}
				}

				nodes[end1 - 1].get_pos(&row1, &col1);
				nodes[end2 - 1].get_pos(&row2, &col2);
				nodes[end3 - 1].get_pos(&row3, &col3);
				x1 = double(col1 - col3);
				y1 = double(row3 - row1);
				x2 = double(col2 - col1);
				y2 = double(row1 - row2);
				direction = x1 * y2 - x2 * y1;
				if (direction > 0.0)//在目标三角形中顺残差方向
				{
					(edges + target_edges - 1)->gain = flow;
				}
				else
				{
					(edges + target_edges - 1)->gain = -flow;
				}
			}
		}
		GET_NEXT_LINE;
	}

	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int Utils::read_DIMACS(
	const char* DIMACS_file_solution,
	vector<tri_edge>& edges, 
	vector<tri_node>& nodes,
	vector<triangle>& triangle
)
{
	if (DIMACS_file_solution == NULL ||
		edges.size() < 3 ||
		nodes.size() < 3 ||
		triangle.size() < 1
		)
	{
		fprintf(stderr, "read_DIMACS(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(DIMACS_file_solution, L"rt");
	if (fp == NULL)
	{
		fprintf(stderr, "read_DIMACS(): can't open %s \n", DIMACS_file_solution);
		return -1;
	}

	char instring[256];
	char ch;
	double obj_value = 0;
	int i, tmp, end1, end2, end3, row1, col1, row2, col2, row3, col3;
	int end[3];
	double x1, y1, x2, y2, direction;
	long from, to;
	double flow = 0;
	bool flag;
	int x[3];
	int y[3];
	int target_edges;
	int num_nodes = static_cast<int>(nodes.size());
	int num_triangle = static_cast<int>(triangle.size()); int num_edges = static_cast<int>(edges.size());
	/////////////////////读取注释///////////////////////////
	GET_NEXT_LINE;
	while (ch != 's' && ch)
	{
		if (ch != 'c')
		{
			if (fp) fclose(fp);
			fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
			return -1;
		}
		GET_NEXT_LINE;
	}
	/////////////////////读取优化目标值/////////////////////
	for (i = 1; i < 81; i++)
	{
		if (isspace((int)instring[i]) > 0)
		{
			i++;
			break;
		}
	}
	if (sscanf(&(instring[i]), "%lf", &obj_value) != 1)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
		return -1;
	}
	if (obj_value < 0.0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): this problem can't be solved(unbounded or infeasible)!\n\n");
		return -1;
	}
	////////////////////////读取MCF结果////////////////////////
	GET_NEXT_LINE;
	while (ch && ch == 'f')
	{
		if (sscanf(&(instring[2]), "%ld %ld %lf", &from, &to, &flow) != 3 ||
			flow < 0.0 || from < 0 || to < 0)
		{
			if (fp) fclose(fp);
			fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
			return -1;
		}
		//非接地边
		if (from > 0 &&
			from <= num_triangle &&
			to > 0 &&
			to <= num_triangle)
		{
			/////////寻找两个三角形的公共边//////////////
			{
				end[0] = -1;
				end[1] = -1;
				x[0] = triangle[from - 1].p1;
				x[1] = triangle[from - 1].p2;
				x[2] = triangle[from - 1].p3;
				y[0] = triangle[to - 1].p1;
				y[1] = triangle[to - 1].p2;
				y[2] = triangle[to - 1].p3;
				i = 0; tmp = 0; flag = false;
				while (end[0] == -1 || end[1] == -1)
				{
					if (i > 2)
					{
						if (fp) fclose(fp);
						fprintf(stderr, "read_DIMACS(): illegal Delaunay triangle!\n\n");
						return -1;
					}
					for (int j = 0; j < 3; j++)
					{
						if (x[i] == y[j])
						{
							end[tmp] = x[i];
							tmp++;
							if (i == 0) flag = true;
							break;
						}
					}
					i++;

				}
			}
			if (i == 3)
			{
				if (flag) end[2] = x[1];
				else
				{
					end[2] = x[0];
				}
			}
			else
			{
				end[2] = x[2];
			}
			if (end[0] > end[1])
			{
				end1 = end[1];
				end2 = end[0];
			}
			else
			{
				end1 = end[0];
				end2 = end[1];
			}
			end3 = end[2];
			/////////寻找两个三角形的公共边//////////////



			if (end1 > 0 && end1 <= num_nodes && end2 > 0 && end2 <= num_nodes && end3 > 0 && end3 <= num_nodes)
			{
				//找到边序号target_edges
				const std::vector<long>& neigh_edges = nodes[end1 - 1].get_neigh_edges();
				for (i = 0; i < neigh_edges.size(); i++)
				{
					long edge_val = neigh_edges[i];
					if (edge_val > 0 && edge_val <= num_edges)
					{
						if (edges[edge_val - 1].end1 == end2 || edges[edge_val - 1].end2 == end2)
						{
							target_edges = edge_val;
						}
					}
				}

				nodes[end1 - 1].get_pos(&row1, &col1);
				nodes[end2 - 1].get_pos(&row2, &col2);
				nodes[end3 - 1].get_pos(&row3, &col3);
				x1 = double(col1 - col3);
				y1 = -double(row3 - row1);
				x2 = double(col2 - col1);
				y2 = -double(row1 - row2);
				direction = x1 * y2 - x2 * y1;
				if (direction < 0.0)//在目标三角形中顺残差方向
				{
					edges[target_edges - 1].gain = -flow;
				}
				else
				{
					edges[target_edges - 1].gain = flow;
				}
			}
		}
		//接地边
		if (from == num_triangle + 1 || to == num_triangle + 1)
		{
			if (from == num_triangle + 1)
			{
				if (edges[triangle[to - 1].edge1 - 1].isBoundary)target_edges = triangle[to - 1].edge1;
				else if (edges[triangle[to - 1].edge2 - 1].isBoundary) target_edges = triangle[to - 1].edge2;
				else target_edges = triangle[to - 1].edge3;


				if (edges[target_edges - 1].end1 > edges[target_edges - 1].end2)
				{
					end1 = edges[target_edges - 1].end2;
					end2 = edges[target_edges - 1].end1;
				}
				else
				{
					end1 = edges[target_edges - 1].end1;
					end2 = edges[target_edges - 1].end2;
				}

				if (triangle[to - 1].p1 != end1 && triangle[to - 1].p1 != end2) end3 = triangle[to - 1].p1;
				else if (triangle[to - 1].p2 != end1 && triangle[to - 1].p2 != end2) end3 = triangle[to - 1].p2;
				else end3 = triangle[to - 1].p3;

				nodes[end1 - 1].get_pos(&row1, &col1);
				nodes[end2 - 1].get_pos(&row2, &col2);
				nodes[end3 - 1].get_pos(&row3, &col3);
				x1 = double(col1 - col3);
				y1 = -double(row3 - row1);
				x2 = double(col2 - col1);
				y2 = -double(row1 - row2);
				direction = x1 * y2 - x2 * y1;
				if (direction < 0.0)//在目标三角形中顺残差方向
				{
					edges[target_edges - 1].gain = flow;
				}
				else
				{
					edges[target_edges - 1].gain = -flow;
				}
			}
			else
			{
				if (edges[triangle[from - 1].edge1 - 1].isBoundary)target_edges = triangle[from - 1].edge1;
				else if (edges[triangle[from - 1].edge2 - 1].isBoundary) target_edges = triangle[from - 1].edge2;
				else target_edges = triangle[from - 1].edge3;


				if (edges[target_edges - 1].end1 > edges[target_edges - 1].end2)
				{
					end1 = edges[target_edges - 1].end2;
					end2 = edges[target_edges - 1].end1;
				}
				else
				{
					end1 = edges[target_edges - 1].end1;
					end2 = edges[target_edges - 1].end2;
				}

				if (triangle[from - 1].p1 != end1 && triangle[from - 1].p1 != end2) end3 = triangle[from - 1].p1;
				else if (triangle[from - 1].p2 != end1 && triangle[from - 1].p2 != end2) end3 = triangle[from - 1].p2;
				else end3 = triangle[from - 1].p3;

				nodes[end1 - 1].get_pos(&row1, &col1);
				nodes[end2 - 1].get_pos(&row2, &col2);
				nodes[end3 - 1].get_pos(&row3, &col3);
				x1 = double(col1 - col3);
				y1 = -double(row3 - row1);
				x2 = double(col2 - col1);
				y2 = -double(row1 - row2);
				direction = x1 * y2 - x2 * y1;
				if (direction < 0.0)//在目标三角形中顺残差方向
				{
					edges[target_edges - 1].gain = -flow;
				}
				else
				{
					edges[target_edges - 1].gain = flow;
				}
			}
		}
		GET_NEXT_LINE;
	}

	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int Utils::cvmat2bin(const char* filename, Mat& mat)
{
	int nr = mat.rows;
	int nc = mat.cols;
	if (nr < 1 || nc < 1 || mat.type() != CV_64F || mat.channels() != 1)
	{
		fprintf(stderr, "cvmat2bin(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fopen_s(&fp, filename, "wb");
	if (fp == NULL)
	{
		fprintf(stderr, "can't open file: %s\n", filename);
		return -1;
	}
	fwrite(&nr, sizeof(int), 1, fp);
	fwrite(&nc, sizeof(int), 1, fp);
	fwrite((double*)mat.data, sizeof(double), nr * nc, fp);
	if (fp != NULL) fclose(fp);
	return 0;
}

int Utils::bin2cvmat(const char* filename, Mat& dst)
{

	FILE* fp = NULL;
	fopen_s(&fp, filename, "rb");
	if (fp == NULL)
	{
		fprintf(stderr, "can't open file: %s\n", filename);
		return -1;
	}
	int rows, cols;
	fread(&rows, sizeof(int), 1, fp);
	fread(&cols, sizeof(int), 1, fp);
	if (rows < 1 || cols < 1)
	{
		fprintf(stderr, "unknown file format!\n");
		if (fp) fclose(fp);
		return -1;
	}
	Mat matrix(rows, cols, CV_64F);
	if (matrix.data == NULL)
	{
		fprintf(stderr, "failed to allocate cv::Mat memory for reading data from %s!\n", filename);
		if (fp) fclose(fp);
		return -1;
	}
	fread(matrix.data, sizeof(double), rows * cols, fp);
	if (fp != NULL) fclose(fp);
	dst = matrix;
	return 0;
}

int Utils::multilook(ComplexMat& Master, ComplexMat& Slave, Mat& phase, int multilook_times, NewtonProgressCallback cb)
{
	if (Master.GetRows() != Slave.GetRows() ||
		Master.GetCols() != Slave.GetCols() ||
		Master.type() != CV_64F ||
		Slave.type() != CV_64F ||
		Master.GetRows() < 1 ||
		Master.GetCols() < 1 ||
		multilook_times < 1 ||
		Master.GetRows() < multilook_times||
		Master.GetCols() < multilook_times)
	{
		fprintf(stderr, "multilook(): input check failed!\n\n");
		return -1;
	}
	int ret;
	if (multilook_times == 1)
	{
		ret = generate_phase(Master, Slave, phase);
		if (return_check(ret, "generate_phase(*, *, *)", error_head)) return -1;
		return 0;
	}
	ComplexMat tmp;
	ret = Master.Mul(Slave, tmp, true);
	if (return_check(ret, "Master.Mul(*, *, *)", error_head)) return -1;
	int nr = tmp.GetRows();
	int nc = tmp.GetCols();
	nr = (nr - (nr % multilook_times)) / multilook_times;
	nc = (nc - (nc % multilook_times)) / multilook_times;
	Mat real = Mat::zeros(nr, nc, CV_64F);
	Mat imag = Mat::zeros(nr, nc, CV_64F);

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, nr / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		if (cancel_flag) continue;
		for (int j = 0; j < nc; j++)
		{
			real.at<double>(i, j) = cv::mean(tmp.re(Range(i * multilook_times, (i + 1) * multilook_times),
				Range(j * multilook_times, (j + 1) * multilook_times)))[0];
			imag.at<double>(i, j) = cv::mean(tmp.im(Range(i * multilook_times, (i + 1) * multilook_times),
				Range(j * multilook_times, (j + 1) * multilook_times)))[0];
		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / nr, "Multilooking..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	tmp.SetRe(real);
	tmp.SetIm(imag);
	phase = tmp.GetPhase();
	return 0;
}

int Utils::multilook(const ComplexMat& master, const ComplexMat& slave, int multilook_rg, int multilook_az, Mat& phase, NewtonProgressCallback cb)
{
	if (master.GetRows() != slave.GetRows() ||
		master.GetCols() != slave.GetCols() ||
		(master.type() != CV_64F && master.type() != CV_32F) ||
		slave.type() != master.type() ||
		master.GetRows() < 1 ||
		master.GetCols() < 1 ||
		multilook_rg < 1 ||
		multilook_az < 1 ||
		master.GetRows() < multilook_az ||
		master.GetCols() < multilook_rg)
	{
		fprintf(stderr, "multilook(): input check failed!\n\n");
		return -1;
	}
	int ret;
	if (multilook_rg == 1 && multilook_az == 1)
	{
		ret = generate_phase(master, slave, phase);
		if (return_check(ret, "generate_phase(*, *, *)", error_head)) return -1;
		return 0;
	}
	ComplexMat tmp;
	tmp.re = master.re.mul(slave.re) + master.im.mul(slave.im);
	tmp.im = slave.re.mul(master.im) - master.re.mul(slave.im);
	int nr = tmp.GetRows();
	int nc = tmp.GetCols();
	int radius_rg = multilook_rg / 2;
	int radius_az = multilook_az / 2;
	phase.create(nr, nc, CV_64F);

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, nr / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		if (cancel_flag) continue;
		int left, right, bottom, top; double real, imag;
		for (int j = 0; j < nc; j++)
		{
			left = j - radius_rg; left = left < 0 ? 0 : left;
			right = left + multilook_rg; right = right > nc - 1 ? nc - 1 : right;
			top = i - radius_az; top = top < 0 ? 0 : top;
			bottom = top + multilook_az; bottom = bottom > nr - 1 ? nr - 1 : bottom;
			real = cv::mean(tmp.re(Range(top, bottom + 1),Range(left, right + 1)))[0];
			imag = cv::mean(tmp.im(Range(top, bottom + 1), Range(left, right + 1)))[0];
			phase.at<double>(i, j) = atan2(imag, real);
		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / nr, "Multilooking (sliding window)..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Utils::Multilook(
	const ComplexMat& master, 
	const ComplexMat& slave,
	int multilook_rg, 
	int multilook_az,
	Mat& phase,
	NewtonProgressCallback cb
)
{
	if (master.GetRows() != slave.GetRows() ||
		master.GetCols() != slave.GetCols() ||
		(master.type() != CV_64F && master.type() != CV_32F) ||
		(slave.type() != CV_64F && slave.type() != CV_32F) ||
		master.GetRows() < 1 ||
		master.GetCols() < 1 ||
		multilook_rg < 1 ||
		multilook_az < 1 ||
		master.GetRows() < multilook_az ||
		master.GetCols() < multilook_rg)
	{
		fprintf(stderr, "multilook(): input check failed!\n\n");
		return -1;
	}
	int ret;
	if (multilook_rg == 1 && multilook_az == 1)
	{
		ret = generate_phase(master, slave, phase);
		if (return_check(ret, "generate_phase(*, *, *)", error_head)) return -1;
		return 0;
	}
	ComplexMat tmp;
	tmp.re = master.re.mul(slave.re) + master.im.mul(slave.im);
	tmp.im = slave.re.mul(master.im) - master.re.mul(slave.im);
	int nr = tmp.GetRows();
	int nc = tmp.GetCols();
	int nr_new = nr / multilook_az;
	int nc_new = nc / multilook_rg;

	phase.create(nr_new, nc_new, CV_64F);

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, nr_new / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr_new; i++)
	{
		if (cancel_flag) continue;
		int left, right, bottom, top; double real, imag;
		top = i * multilook_az; top = top < 0 ? 0 : top;
		bottom = top + multilook_az; bottom = bottom > nr ? nr : bottom;
		for (int j = 0; j < nc_new; j++)
		{
			left = j * multilook_rg; left = left < 0 ? 0 : left;
			right = left + multilook_rg; right = right > nc ? nc : right;
			real = cv::mean(tmp.re(Range(top, bottom), Range(left, right)))[0];
			imag = cv::mean(tmp.im(Range(top, bottom), Range(left, right)))[0];
			phase.at<double>(i, j) = atan2(imag, real);
		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / nr_new, "Multilooking (window shrink)..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Utils::multilookCorrectedInterferogram(
	const ComplexMat& master, const ComplexMat& slave, const Mat& correctionPhase, const Mat& flatEarthPhase,
	const Mat& sourceRowMap, const Mat& validSampleMask, int multilookRg, int multilookAz, Mat& phase,
	Mat& effectiveFlatEarthReference,
	Mat& phaseValidMask, Mat& validSampleCount,
	NewtonProgressCallback cb)
{
	if (master.GetRows() != slave.GetRows() || master.GetCols() != slave.GetCols() ||
		(master.type() != CV_32F && master.type() != CV_64F) || slave.type() != master.type() ||
		correctionPhase.type() != CV_64F || correctionPhase.rows != master.GetRows() || correctionPhase.cols != master.GetCols() ||
		(!flatEarthPhase.empty() && (flatEarthPhase.type() != CV_64F || flatEarthPhase.size() != correctionPhase.size())) ||
		sourceRowMap.type() != CV_32S || sourceRowMap.rows != master.GetRows() || sourceRowMap.cols != 1 ||
		validSampleMask.type() != CV_8U || validSampleMask.size() != correctionPhase.size() ||
		multilookRg < 1 || multilookAz < 1 || master.GetCols() < multilookRg)
	{
		fprintf(stderr, "multilookCorrectedInterferogram(): input check failed!\n");
		return -1;
	}

	const int rows = master.GetRows();
	const int columns = master.GetCols();
	const int outputColumns = columns / multilookRg;
	if (outputColumns < 1) return -1;
	struct Run { int first; int count; };
	std::vector<Run> runs;
	for (int first = 0; first < rows;) {
		if (sourceRowMap.at<int>(first, 0) < 0) return -1;
		int end = first + 1;
		while (end < rows && sourceRowMap.at<int>(end, 0) == sourceRowMap.at<int>(end - 1, 0) + 1) ++end;
		if (end - first >= multilookAz) runs.push_back({ first, end - first });
		first = end;
	}
	int outputRows = 0;
	for (const Run& run : runs) outputRows += run.count / multilookAz;
	if (outputRows < 1) {
		fprintf(stderr, "multilookCorrectedInterferogram(): no complete source-row run for requested azimuth looks!\n");
		return -1;
	}

	phase.create(outputRows, outputColumns, CV_64F);
	phase.setTo(0.0);
	phaseValidMask.create(outputRows, outputColumns, CV_8U);
	phaseValidMask.setTo(0);
	validSampleCount.create(outputRows, outputColumns, CV_32S);
	validSampleCount.setTo(0);
	if (flatEarthPhase.empty()) effectiveFlatEarthReference.release();
	else {
		effectiveFlatEarthReference.create(outputRows, outputColumns, CV_64F);
		effectiveFlatEarthReference.setTo(0.0);
	}
	std::atomic<bool> cancelled(false);
	std::atomic<bool> invalidSelectedSample(false);
	std::atomic<int> completed(0);
	const int progressStep = std::max(1, outputRows / 100);
	int outputRowBase = 0;
	for (const Run& run : runs) {
		const int runOutputRows = run.count / multilookAz;
#pragma omp parallel for schedule(guided)
		for (int outY = 0; outY < runOutputRows; ++outY) {
			if (cancelled) continue;
			const int inputTop = run.first + outY * multilookAz;
			for (int outX = 0; outX < outputColumns; ++outX) {
				const int inputLeft = outX * multilookRg;
				double correctedReal = 0.0;
				double correctedImaginary = 0.0;
				double uncorrectedReal = 0.0;
				double uncorrectedImaginary = 0.0;
				double flatCorrectedReal = 0.0;
				double flatCorrectedImaginary = 0.0;
				int selectedSampleCount = 0;
				for (int y = inputTop; y < inputTop + multilookAz; ++y) {
					for (int x = inputLeft; x < inputLeft + multilookRg; ++x) {
						if (validSampleMask.at<uchar>(y, x) == 0) continue;
						const double masterReal = master.type() == CV_64F ? master.re.at<double>(y, x) : master.re.at<float>(y, x);
						const double masterImaginary = master.type() == CV_64F ? master.im.at<double>(y, x) : master.im.at<float>(y, x);
						const double slaveReal = slave.type() == CV_64F ? slave.re.at<double>(y, x) : slave.re.at<float>(y, x);
						const double slaveImaginary = slave.type() == CV_64F ? slave.im.at<double>(y, x) : slave.im.at<float>(y, x);
						const double correction = correctionPhase.at<double>(y, x);
						if (!std::isfinite(masterReal) || !std::isfinite(masterImaginary) ||
							!std::isfinite(slaveReal) || !std::isfinite(slaveImaginary) || !std::isfinite(correction) ||
							(!flatEarthPhase.empty() && !std::isfinite(flatEarthPhase.at<double>(y, x)))) {
							invalidSelectedSample = true;
							continue;
						}
						const double interferogramReal = masterReal * slaveReal + masterImaginary * slaveImaginary;
						const double interferogramImaginary = masterImaginary * slaveReal - masterReal * slaveImaginary;
						const double correctionCosine = cos(correction);
						const double correctionSine = sin(correction);
						++selectedSampleCount;
						uncorrectedReal += interferogramReal;
						uncorrectedImaginary += interferogramImaginary;
						correctedReal += interferogramReal * correctionCosine + interferogramImaginary * correctionSine;
						correctedImaginary += interferogramImaginary * correctionCosine - interferogramReal * correctionSine;
						if (!flatEarthPhase.empty()) {
							const double flatEarth = flatEarthPhase.at<double>(y, x);
							const double flatCosine = cos(flatEarth);
							const double flatSine = sin(flatEarth);
							flatCorrectedReal += interferogramReal * flatCosine + interferogramImaginary * flatSine;
							flatCorrectedImaginary += interferogramImaginary * flatCosine - interferogramReal * flatSine;
						}
					}
				}
				validSampleCount.at<int>(outputRowBase + outY, outX) = selectedSampleCount;
				if (selectedSampleCount == 0) continue;
				const double correctedMagnitude = hypot(correctedReal, correctedImaginary);
				if (!std::isfinite(uncorrectedReal) || !std::isfinite(uncorrectedImaginary) ||
					!std::isfinite(correctedReal) || !std::isfinite(correctedImaginary) ||
					correctedMagnitude <= DBL_EPSILON) {
					continue;
				}
				if (!flatEarthPhase.empty()) {
					const double rawMagnitude = hypot(uncorrectedReal, uncorrectedImaginary);
					const double flatMagnitude = hypot(flatCorrectedReal, flatCorrectedImaginary);
					if (!std::isfinite(flatCorrectedReal) || !std::isfinite(flatCorrectedImaginary) ||
						rawMagnitude <= DBL_EPSILON || flatMagnitude <= DBL_EPSILON) {
						continue;
					}
					const double effectiveWrapped = atan2(
						sin(atan2(uncorrectedImaginary, uncorrectedReal) - atan2(flatCorrectedImaginary, flatCorrectedReal)),
						cos(atan2(uncorrectedImaginary, uncorrectedReal) - atan2(flatCorrectedImaginary, flatCorrectedReal)));
					const double branchAnchor = flatEarthPhase.at<double>(
						inputTop + multilookAz / 2, inputLeft + multilookRg / 2);
					if (!std::isfinite(branchAnchor)) {
						continue;
					}
					if (multilookRg == 1 && multilookAz == 1) {
						// Preserve the caller's unwrapped reference value bit-for-bit at 1x1.
						effectiveFlatEarthReference.at<double>(outputRowBase + outY, outX) = branchAnchor;
					}
					else {
						const double branchDelta = effectiveWrapped - atan2(sin(branchAnchor), cos(branchAnchor));
						effectiveFlatEarthReference.at<double>(outputRowBase + outY, outX) =
							branchAnchor + atan2(sin(branchDelta), cos(branchDelta));
					}
				}
				phase.at<double>(outputRowBase + outY, outX) = atan2(correctedImaginary, correctedReal);
				phaseValidMask.at<uchar>(outputRowBase + outY, outX) = 1;
			}
			const int current = ++completed;
			if (cb && current % progressStep == 0 && !cb(current * 100 / outputRows, "Corrected interferogram multilooking...")) cancelled = true;
		}
		outputRowBase += runOutputRows;
	}
	if (cancelled) return -2;
	if (invalidSelectedSample) {
		fprintf(stderr, "multilookCorrectedInterferogram(): valid-sample mask selected a non-finite input or reference value!\n");
		return -1;
	}
	return 0;
}

int Utils::multilook(const Mat& phase, Mat& outPhase, int multi_rg, int multi_az, NewtonProgressCallback cb)
{
	if (multi_rg <= 1 && multi_az <= 1)
	{
		phase.copyTo(outPhase);
		return 0;
	}
	ComplexMat slc;
	int ret;
	phase.copyTo(outPhase);
	outPhase.convertTo(outPhase, CV_32F);//节省内存
	ret = phase2cos(outPhase, slc.re, slc.im);
	if (return_check(ret, "phase2cos()", error_head)) return -1;
	int nr = slc.GetRows();
	int nc = slc.GetCols();
	int nr_new = nr / multi_az;
	int nc_new = nc / multi_rg;
	outPhase.create(nr_new, nc_new, CV_64F);

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, nr_new / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr_new; i++)
	{
		if (cancel_flag) continue;
		int left, right, bottom, top; double real, imag;
		top = i * multi_az; top = top < 0 ? 0 : top;
		bottom = top + multi_az; bottom = bottom > nr ? nr : bottom;
		for (int j = 0; j < nc_new; j++)
		{
			left = j * multi_rg; left = left < 0 ? 0 : left;
			right = left + multi_rg; right = right > nc ? nc : right;
			real = cv::mean(slc.re(Range(top, bottom), Range(left, right)))[0];
			imag = cv::mean(slc.im(Range(top, bottom), Range(left, right)))[0];
			outPhase.at<double>(i, j) = atan2(imag, real);
		}

		int current = ++completed_rows;
		if (cb && current % step == 0)
		{
			if (!cb(current * 100 / nr_new, "Phase multilooking..."))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	return 0;
}

int Utils::multilook_SAR(const Mat& amplitude, Mat& outAmplitude, int multilook_rg, int multilook_az, NewtonProgressCallback cb)
{
	if (amplitude.empty() ||
		amplitude.rows < multilook_az ||
		amplitude.cols < multilook_rg ||
		multilook_rg < 1 || multilook_az < 1||
		(amplitude.type() != CV_64F && amplitude.type() != CV_32F)
		)
	{
		fprintf(stderr, "multilook_SAR(): input check failed!\n");
		return -1;
	}
	int nr = amplitude.rows;
	int nc = amplitude.cols;
	int nr_new = (int)((double)nr / (double)multilook_az);
	int nc_new = (int)((double)nc / (double)multilook_rg);

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, nr_new / 100);

	Mat tmp;
	if (amplitude.type() == CV_64F)
	{
		tmp = Mat::zeros(nr_new, nc_new, CV_64F);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr_new; i++)
		{
			if (cancel_flag) continue;
			int left, right, bottom, top;
			top = i * multilook_az; top = top < 0 ? 0 : top;
			bottom = top + multilook_az; bottom = bottom > nr ? nr : bottom;
			for (int j = 0; j < nc_new; j++)
			{
				left = j * multilook_rg; left = left < 0 ? 0 : left;
				right = left + multilook_rg; right = right > nc ? nc : right;
				tmp.at<double>(i, j) = cv::mean(amplitude(Range(top, bottom), Range(left, right)))[0];
			}

			int current = ++completed_rows;
			if (cb && current % step == 0)
			{
				if (!cb(current * 100 / nr_new, "SAR amplitude multilooking (64F)..."))
				{
					cancel_flag = true;
				}
			}
		}
	}
	else
	{
		tmp = Mat::zeros(nr_new, nc_new, CV_32F);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr_new; i++)
		{
			if (cancel_flag) continue;
			int left, right, bottom, top;
			top = i * multilook_az; top = top < 0 ? 0 : top;
			bottom = top + multilook_az; bottom = bottom > nr ? nr : bottom;
			for (int j = 0; j < nc_new; j++)
			{
				left = j * multilook_rg; left = left < 0 ? 0 : left;
				right = left + multilook_rg; right = right > nc ? nc : right;
				tmp.at<float>(i, j) = static_cast<float>(cv::mean(amplitude(Range(top, bottom), Range(left, right)))[0]);
			}

			int current = ++completed_rows;
			if (cb && current % step == 0)
			{
				if (!cb(current * 100 / nr_new, "SAR amplitude multilooking (32F)..."))
				{
					cancel_flag = true;
				}
			}
		}
	}
	if (cancel_flag) return -2;

	tmp.copyTo(outAmplitude);
	return 0;
}

int Utils::phase2cos(const Mat& phase, Mat& cos, Mat& sin)
{
	if (phase.rows < 1 ||
		phase.cols < 1 ||
		(phase.type() != CV_64F && phase.type() != CV_32F) ||
		phase.channels() != 1)
	{
		fprintf(stderr, "phase2cos(): input check failed!\n\n");
		return -1;
	}
	int nr = phase.rows;
	int nc = phase.cols;
	if (phase.type() == CV_32F)
	{
		cos.create(nr, nc, CV_32F);
		sin.create(nr, nc, CV_32F);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				cos.at<float>(i, j) = std::cos(phase.at<float>(i, j));
				sin.at<float>(i, j) = std::sin(phase.at<float>(i, j));
			}
		}
	}
	else
	{
		cos.create(nr, nc, CV_64F);
		sin.create(nr, nc, CV_64F);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				cos.at<double>(i, j) = std::cos(phase.at<double>(i, j));
				sin.at<double>(i, j) = std::sin(phase.at<double>(i, j));
			}
		}
	}
	return 0;
}

int Utils::xyz2ell(double x, double y, double z, double& lat, double& lon, double& h)
{
	const double epsilon = 0.000000000000001;
	const double d2r = PI / 180;
	const double r2d = 180 / PI;

	const double a = 6378137.0;		//椭球长半轴
	const double f_inverse = 298.257223563;			//扁率倒数
	const double b = a - a / f_inverse;
	//const double b = 6356752.314245;			//椭球短半轴

	const double e = sqrt(a * a - b * b) / a;

	double tmpX = x;
	double temY = y;
	double temZ = z;

	double curB = 0;
	double N = 0;
	double calB = atan2(temZ, sqrt(tmpX * tmpX + temY * temY));

	int counter = 0;
	while (abs(curB - calB) * r2d > epsilon && counter < 25)
	{
		curB = calB;
		N = a / sqrt(1 - e * e * sin(curB) * sin(curB));
		calB = atan2(temZ + N * e * e * sin(curB), sqrt(tmpX * tmpX + temY * temY));
		counter++;
	}

	lon = atan2(temY, tmpX) * r2d;
	lat = curB * r2d;
	h = temZ / sin(curB) - N * (1 - e * e);
	return 0;
}

int Utils::ell2xyz(const Mat& llh, Mat& xyz)
{
	if (llh.cols != 3 ||
		llh.rows != 1 ||
		llh.type() != CV_64F ||
		llh.channels() != 1
		)
	{
		fprintf(stderr, "ell2xyz(): input check failed!\n\n");
		return -1;
	}
	const double epsilon = 0.000000000000001;
	const double d2r = PI / 180;
	const double r2d = 180 / PI;

	const double a = 6378137.0;		//椭球长半轴
	const double f_inverse = 298.257223563;			//扁率倒数
	const double b = a - a / f_inverse;
	const double e = sqrt(a * a - b * b) / a;

	double y = llh.at<double>(0, 0);
	double x = llh.at<double>(0, 1);
	double z = llh.at<double>(0, 2);

	double L = x * d2r;
	double B = y * d2r;
	double H = z;
	double N = a / sqrt(1 - e * e * sin(B) * sin(B));
	x = (N + H) * cos(B) * cos(L);
	y = (N + H) * cos(B) * sin(L);
	z = (N * (1 - e * e) + H) * sin(B);
	Mat tmp;
	tmp.create(1, 3, CV_64F);
	tmp.at<double>(0, 0) = x;
	tmp.at<double>(0, 1) = y;
	tmp.at<double>(0, 2) = z;
	tmp.copyTo(xyz);
	return 0;
}

int Utils::ell2xyz(double lon, double lat, double elevation, Position& xyz)
{
	if (fabs(lon) > 180.0 || fabs(lat) > 90.0)
	{
		fprintf(stderr, "ell2xyz(): input check failed!\n");
		return -1;
	}
	const double epsilon = 0.000000000000001;
	const double d2r = PI / 180;
	const double r2d = 180 / PI;
	const double a = 6378137.0;		//椭球长半轴
	const double f_inverse = 298.257223563;			//扁率倒数
	const double b = a - a / f_inverse;
	const double e = sqrt(a * a - b * b) / a;
	double y = lat;
	double x = lon;
	double z = elevation;
	double L = x * d2r;
	double B = y * d2r;
	double H = z;
	double N = a / sqrt(1 - e * e * sin(B) * sin(B));
	x = (N + H) * cos(B) * cos(L);
	y = (N + H) * cos(B) * sin(L);
	z = (N * (1 - e * e) + H) * sin(B);
	xyz.x = x;
	xyz.y = y;
	xyz.z = z;
	return 0;
}





int Utils::saveSLC(const char* filename, double db, ComplexMat& SLC)
{
	if (filename == NULL ||
		db < 0 ||
		SLC.GetRows() < 1 ||
		SLC.GetCols() < 1 /*||
		SLC.type() != CV_64F*/)
	{
		fprintf(stderr, "saveSLC(): input check failed!\n\n");
		return -1;
	}
	ComplexMat tmp;
	Mat mod;
	if (SLC.type() != CV_64F && SLC.type() != CV_32F)
	{
		SLC.re.convertTo(tmp.re, CV_32F);
		SLC.im.convertTo(tmp.im, CV_32F);
		mod = tmp.GetMod();
	}
	else
	{
		mod = SLC.GetMod();
	}
	int nr = mod.rows;
	int nc = mod.cols;
	size_t imagesize = nr * nc;
	if (imagesize > 10000 * 10000)
	{
		int mul_times = (int)sqrt(double(imagesize / 10000.0 / 10000.0));
		mul_times = mul_times < 1 ? 1 : mul_times;
		multilook_SAR(mod, mod, mul_times, mul_times);
	}
	double max, min, std;
	this->std(mod, &std);
	nr = mod.rows;
	nc = mod.cols;
	double mean = cv::mean(mod)[0];
	if (SLC.type() == CV_64F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				if ((mod.at<double>(i, j) - mean) >= 3.0 * std) mod.at<double>(i, j) = mean + 3.0 * std;
				if ((mod.at<double>(i, j) - mean) < -3.0 * std) mod.at<double>(i, j) = mean - 3.0 * std;
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				if ((mod.at<float>(i, j) - mean) >= 3.0 * std) mod.at<float>(i, j) = static_cast<float>(mean + 3.0 * std);                                                   
				if ((mod.at<float>(i, j) - mean) < -3.0 * std) mod.at<float>(i, j) = static_cast<float>(mean - 3.0 * std);          
			}
		}
	}
	cv::minMaxLoc(mod, &min, &max);
	if (fabs(max - min) < 0.00000001)
	{
		fprintf(stderr, "SLC image intensity is the same for every pixel\n\n");
		return -1;
	}
	mod = (mod - min) / (max - min) * 255.0;
	mod.convertTo(mod, CV_8U);
	bool ret = cv::imwrite(filename, mod);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::SAR_image_quantify(const char* filename, double db, ComplexMat& SLC)
{
	if (filename == NULL ||
		db < 0 ||
		SLC.GetRows() < 1 ||
		SLC.GetCols() < 1 /*||
		SLC.type() != CV_64F*/)
	{
		fprintf(stderr, "SAR_image_quantify(): input check failed!\n\n");
		return -1;
	}
	ComplexMat tmp;
	Mat mod;
	if (SLC.type() != CV_64F && SLC.type() != CV_32F)
	{
		SLC.re.convertTo(tmp.re, CV_32F);
		SLC.im.convertTo(tmp.im, CV_32F);
		mod = tmp.GetMod();
	}
	else
	{
		mod = SLC.GetMod();
	}
	int nr = mod.rows;
	int nc = mod.cols;
	double max, min;
	if (SLC.type() == CV_64F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				mod.at<double>(i, j) = 20 * log10(mod.at<double>(i, j) + 0.000001);
			}
		}

		minMaxLoc(mod, &min, &max);
		if (fabs(max - min) < 0.00000001)
		{
			fprintf(stderr, "SLC image intensity is the same for every pixel\n\n");
			return -1;
		}
		min = min < -1.0 ? -1.0 : min;
		max = min + db;
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				if (mod.at<double>(i, j) >= max) mod.at<double>(i, j) = max;
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				mod.at<float>(i, j) = static_cast<float>(20 * log10(mod.at<float>(i, j) + 0.000001));
			}
		}

		minMaxLoc(mod, &min, &max);
		if (fabs(max - min) < 0.00000001)
		{
			fprintf(stderr, "SLC image intensity is the same for every pixel\n\n");
			return -1;
		}
		min = min < -1.0 ? -1.0 : min;
		max = min + db;
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				if (mod.at<float>(i, j) >= max) mod.at<float>(i, j) = static_cast<float>(max);
			}
		}
	}
	mod = (mod - min) / (max - min) * 255.0;
	mod.convertTo(mod, CV_8U);
	bool ret = cv::imwrite(filename, mod);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::saveAmplitude(const char* filename, Mat& amplitude)
{
	if (!filename || amplitude.empty() || (amplitude.type() != CV_64F && amplitude.type() != CV_32F))
	{
		fprintf(stderr, "saveAmplitude(): input check failed!\n");
		return -1;
	}
	double min, max;
	int nr = amplitude.rows;
	int nc = amplitude.cols;
	
	double mean = cv::mean(amplitude)[0];
	double std;
	this->std(amplitude, &std);
	if (amplitude.type() == CV_64F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				if ((amplitude.at<double>(i, j) - mean) >= 3.0 * std) amplitude.at<double>(i, j) = mean + 3.0 * std;
				if ((amplitude.at<double>(i, j) - mean) <= -3.0 * std) amplitude.at<double>(i, j) = mean - 3.0 * std;
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				if ((amplitude.at<float>(i, j) - mean) >= 3.0 * std) amplitude.at<float>(i, j) = static_cast<float>(mean + 3.0 * std);
				if ((amplitude.at<float>(i, j) - mean) <= -3.0 * std) amplitude.at<float>(i, j) = static_cast<float>(mean - 3.0 * std);
			}
		}
	}
	cv::minMaxLoc(amplitude, &min, &max);
	if (fabs(max - min) < 0.00000001)
	{
		fprintf(stderr, "SLC image intensity is the same for every pixel\n\n");
		return -1;
	}
	amplitude = (amplitude - min) / (max - min) * 255.0;
	amplitude.convertTo(amplitude, CV_8U);
	bool ret = cv::imwrite(filename, amplitude);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::savephase(const char* filename, const char* colormap, const Mat& phase)
{
	if (filename == NULL ||
		colormap == NULL ||
		phase.rows < 1 ||
		phase.cols < 1 ||
		phase.type() != CV_64F ||
		phase.channels() != 1)
	{
		fprintf(stderr, "savephase(): input check failed!\n\n");
		return -1;
	}
	bool gray = false;
	cv::ColormapTypes type = cv::COLORMAP_PARULA;
	if (strcmp(colormap, "jet") == 0) type = cv::COLORMAP_JET;
	if (strcmp(colormap, "hsv") == 0) type = cv::COLORMAP_HSV;
	if (strcmp(colormap, "cool") == 0) type = cv::COLORMAP_COOL;
	if (strcmp(colormap, "rainbow") == 0) type = cv::COLORMAP_RAINBOW;
	if (strcmp(colormap, "spring") == 0) type = cv::COLORMAP_SPRING;
	if (strcmp(colormap, "summer") == 0) type = cv::COLORMAP_SUMMER;
	if (strcmp(colormap, "winter") == 0) type = cv::COLORMAP_WINTER;
	if (strcmp(colormap, "autumn") == 0) type = cv::COLORMAP_AUTUMN;
	if (strcmp(colormap, "gray") == 0) gray = true;

	double min, max;
	Mat tmp;
	phase.copyTo(tmp);
	cv::minMaxLoc(tmp, &min, &max);
	if (fabs(max - min) < 0.000001)
	{
		fprintf(stderr, "phase value is the same for every pixel\n\n");
		return -1;
	}
	tmp = (tmp - min) / (max - min)*255.0;
	tmp.convertTo(tmp, CV_8U);
	if (!gray)
	{
		cv::applyColorMap(tmp, tmp, type);
	}
	bool ret = cv::imwrite(filename, tmp);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::save_coherence(const char* filename, const char* colormap, const Mat& coherence)
{
	if (filename == NULL ||
		colormap == NULL ||
		coherence.rows < 1 ||
		coherence.cols < 1 ||
		(coherence.type() != CV_32F && coherence.type() != CV_64F) ||
		coherence.channels() != 1)
	{
		fprintf(stderr, "save_coherence(): input check failed!\n\n");
		return -1;
	}
	bool gray = false;
	cv::ColormapTypes type = cv::COLORMAP_PARULA;
	if (strcmp(colormap, "jet") == 0) type = cv::COLORMAP_JET;
	if (strcmp(colormap, "hsv") == 0) type = cv::COLORMAP_HSV;
	if (strcmp(colormap, "cool") == 0) type = cv::COLORMAP_COOL;
	if (strcmp(colormap, "rainbow") == 0) type = cv::COLORMAP_RAINBOW;
	if (strcmp(colormap, "spring") == 0) type = cv::COLORMAP_SPRING;
	if (strcmp(colormap, "summer") == 0) type = cv::COLORMAP_SUMMER;
	if (strcmp(colormap, "winter") == 0) type = cv::COLORMAP_WINTER;
	if (strcmp(colormap, "autumn") == 0) type = cv::COLORMAP_AUTUMN;
	if (strcmp(colormap, "gray") == 0) gray = true;

	double min, max;
	Mat tmp;
	coherence.copyTo(tmp);
	if (coherence.type() == CV_64F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < tmp.rows; i++)
		{
			for (int j = 0; j < tmp.cols; j++)
			{
				if (tmp.at<double>(i, j) < 0.0) tmp.at<double>(i, j) = 0.0;
				if (tmp.at<double>(i, j) > 1.0) tmp.at<double>(i, j) = 1.0;
			}
		}
	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < tmp.rows; i++)
		{
			for (int j = 0; j < tmp.cols; j++)
			{
				if (tmp.at<float>(i, j) < 0.0) tmp.at<float>(i, j) = 0.0;
				if (tmp.at<float>(i, j) > 1.0) tmp.at<float>(i, j) = 1.0;
			}
		}
	}
	min = 0.0; max = 1.0;
	tmp = (tmp - min) / (max - min) * 255.0;
	tmp.convertTo(tmp, CV_8U);
	if (!gray)
	{
		cv::applyColorMap(tmp, tmp, type);
	}
	bool ret = cv::imwrite(filename, tmp);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::savephase_black(const char* filename, const char* colormap, Mat& phase, Mat& mask)
{
	if (filename == NULL ||
		colormap == NULL ||
		phase.rows < 1 ||
		phase.cols < 1 ||
		phase.type() != CV_64F ||
		phase.channels() != 1 ||
		mask.size() != phase.size() ||
		mask.type() != CV_32S
		)
	{
		fprintf(stderr, "savephase_black(): input check failed!\n\n");
		return -1;
	}
	bool gray = false;
	cv::ColormapTypes type = cv::COLORMAP_PARULA;
	if (strcmp(colormap, "jet") == 0) type = cv::COLORMAP_JET;
	if (strcmp(colormap, "hsv") == 0) type = cv::COLORMAP_HSV;
	if (strcmp(colormap, "cool") == 0) type = cv::COLORMAP_COOL;
	if (strcmp(colormap, "rainbow") == 0) type = cv::COLORMAP_RAINBOW;
	if (strcmp(colormap, "spring") == 0) type = cv::COLORMAP_SPRING;
	if (strcmp(colormap, "summer") == 0) type = cv::COLORMAP_SUMMER;
	if (strcmp(colormap, "winter") == 0) type = cv::COLORMAP_WINTER;
	if (strcmp(colormap, "autumn") == 0) type = cv::COLORMAP_AUTUMN;
	if (strcmp(colormap, "gray") == 0) gray = true;

	double min, max;
	Mat tmp;
	phase.copyTo(tmp);
	cv::minMaxLoc(tmp, &min, &max);
	if (fabs(max - min) < 0.000001)
	{
		fprintf(stderr, "savephase_black(): phase value is the same for every pixel\n\n");
		return -1;
	}
	tmp = (tmp - min) / (max - min) * 255.0;
	tmp.convertTo(tmp, CV_8UC3);
	if (!gray)
	{
		cv::applyColorMap(tmp, tmp, type);
	}
	for (int i = 0; i < tmp.rows; i++)
	{
		for (int j = 0; j < tmp.cols; j++)
		{
			if (mask.at<int>(i, j) == 0)
			{
				tmp.at<Vec<uchar, 3>>(i, j)[0] = 0;
				tmp.at<Vec<uchar, 3>>(i, j)[1] = 0;
				tmp.at<Vec<uchar, 3>>(i, j)[2] = 0;
			}
		}
	}
	bool ret = cv::imwrite(filename, tmp);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::savephase_white(const char* filename, const char* colormap, Mat& phase, Mat& mask)
{
	if (filename == NULL ||
		colormap == NULL ||
		phase.rows < 1 ||
		phase.cols < 1 ||
		phase.type() != CV_64F ||
		phase.channels() != 1 ||
		mask.size() != phase.size() ||
		mask.type() != CV_32S
		)
	{
		fprintf(stderr, "savephase_white(): input check failed!\n\n");
		return -1;
	}
	bool gray = false;
	cv::ColormapTypes type = cv::COLORMAP_PARULA;
	if (strcmp(colormap, "jet") == 0) type = cv::COLORMAP_JET;
	if (strcmp(colormap, "hsv") == 0) type = cv::COLORMAP_HSV;
	if (strcmp(colormap, "cool") == 0) type = cv::COLORMAP_COOL;
	if (strcmp(colormap, "rainbow") == 0) type = cv::COLORMAP_RAINBOW;
	if (strcmp(colormap, "spring") == 0) type = cv::COLORMAP_SPRING;
	if (strcmp(colormap, "summer") == 0) type = cv::COLORMAP_SUMMER;
	if (strcmp(colormap, "winter") == 0) type = cv::COLORMAP_WINTER;
	if (strcmp(colormap, "autumn") == 0) type = cv::COLORMAP_AUTUMN;
	if (strcmp(colormap, "gray") == 0) gray = true;

	double min, max;
	Mat tmp;
	phase.copyTo(tmp);
	cv::minMaxLoc(tmp, &min, &max);
	if (fabs(max - min) < 0.000001)
	{
		fprintf(stderr, "savephase_white() : phase value is the same for every pixel\n\n");
		return -1;
	}
	tmp = (tmp - min) / (max - min) * 255.0;
	tmp.convertTo(tmp, CV_8UC3);
	if (!gray)
	{
		cv::applyColorMap(tmp, tmp, type);
	}
	for (int i = 0; i < tmp.rows; i++)
	{
		for (int j = 0; j < tmp.cols; j++)
		{
			if (mask.at<int>(i, j) == 0)
			{
				tmp.at<Vec<uchar, 3>>(i, j)[0] = 255;
				tmp.at<Vec<uchar, 3>>(i, j)[1] = 255;
				tmp.at<Vec<uchar, 3>>(i, j)[2] = 255;
			}
		}
	}
	bool ret = cv::imwrite(filename, tmp);
	if (!ret)
	{
		fprintf(stderr, "cv::imwrite(): can't write to %s\n\n", filename);
		return -1;
	}
	return 0;
}

int Utils::resampling(const char* Src_file, const char* Dst_file, int dst_rows, int dst_cols)
{
	if (Src_file == NULL ||
		Dst_file == NULL ||
		dst_rows < 1 ||
		dst_cols < 1)
	{
		fprintf(stderr, "down_sampling(): input check failed!\n\n");
		return -1;
	}
	Mat img = cv::imread(Src_file);
	if (img.rows < 1 || img.cols < 1)
	{
		fprintf(stderr, "can't read from %s!\n\n", Dst_file);
		return -1;
	}
	cv::resize(img, img, cv::Size(dst_cols, dst_rows));
	if (cv::imwrite(Dst_file, img) == false)
	{
		fprintf(stderr, "failed to write %s\n\n", Dst_file);
		return -1;
	}
	return 0;
}

int Utils::amplitude_phase_blend(const char* amplitude_file, const char* phase_file, const char* blended_file, double SAR_ratio)
{
	if (amplitude_file == NULL ||
		phase_file == NULL ||
		blended_file == NULL
		)
	{
		fprintf(stderr, "amplitude_phase_blend(): input check failed!\n\n");
		return -1;
	}
	Mat amplitude = imread(amplitude_file);
	Mat phase = imread(phase_file);
	if (amplitude.rows < 1 ||
		amplitude.cols < 1
		)
	{
		fprintf(stderr, "amplitude_phase_blend(): can't open %s!\n\n", amplitude_file);
		return -1;
	}
	if (phase.rows < 1 ||
		phase.cols < 1
		)
	{
		fprintf(stderr, "amplitude_phase_blend(): can't open %s!\n", phase_file);
		return -1;
	}
	SAR_ratio = SAR_ratio < 0.5 ? 0.5 : SAR_ratio;
	SAR_ratio = SAR_ratio > 0.99 ? 0.99 : SAR_ratio;
	Mat blend;
	addWeighted(amplitude, SAR_ratio, phase, 1.0 - SAR_ratio, 0, blend);
	if (blend.rows < 1 || blend.cols < 1)
	{
		fprintf(stderr, "amplitude_phase_blend(): failed to blend !\n");
		return -1;
	}
	if (!imwrite(blended_file, blend))
	{
		fprintf(stderr, "amplitude_phase_blend(): failed to write to %s !\n", blended_file);
		return -1;
	}
	return 0;
}



int Utils::read_edges(const char* filename, tri_edge** edges, long* num_edges, int** neighbours, long num_nodes)
{
	if (filename == NULL ||
		num_edges == NULL ||
		num_nodes < 3||
		edges == NULL||
		neighbours == NULL)
	{
		fprintf(stderr, "read_edges(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(filename, L"rt");
	if (fp == NULL)
	{
		fprintf(stderr, "read_edges(): can't open %s\n", filename);
		return -1;
	}
	char str[1024];
	char* ptr;
	fgets(str, 1024, fp);
	*num_edges = strtol(str, &ptr, 0);
	if (*num_edges <= 0)
	{
		fprintf(stderr, "read_edges(): %s is unknown format!\n", filename);
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return -1;
	}
	*edges = (tri_edge*)malloc(*num_edges * sizeof(tri_edge));
	if (*edges == NULL)
	{
		fprintf(stderr, "read_edges(): unreasonable number of edges, out of memory!\n");
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return -1;
	}
	memset(*edges, 0, *num_edges * sizeof(tri_edge));
	*neighbours = (int*)malloc(sizeof(int) * num_nodes);
	if (*neighbours == NULL)
	{
		fprintf(stderr, "read_edges(): unreasonable number of nodes, out of memory!\n");
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		if (*edges)
		{
			free(*edges);
			*edges = NULL;
		}
		return -1;
	}
	memset(*neighbours, 0, sizeof(int) * num_nodes);
	long end1, end2, edges_number, boundry_marker;
	for (int i = 0; i < *num_edges; i++)
	{
		fgets(str, 1024, fp);
		edges_number = strtol(str, &ptr, 0);
		end1 = strtol(ptr, &ptr, 0);
		end2 = strtol(ptr, &ptr, 0);
		boundry_marker = strtol(ptr, &ptr, 0);
		(*edges + i)->end1 = end1;
		(*edges + i)->end2 = end2;
		(*edges + i)->num = i + 1;
		(*edges + i)->gain = 0;
		(*edges + i)->isResidueEdge = false;
		(*edges + i)->isBoundary = (boundry_marker == 1);
		if (end1 < 1 ||
			end1 > num_nodes ||
			end2 < 1 ||
			end2 > num_nodes)
		{
			fprintf(stderr, "read_edges(): endpoints exceed 1~num_nodes!\n");
			if (fp)
			{
				fclose(fp);
				fp = NULL;
			}
			if (*edges)
			{
				free(*edges);
				*edges = NULL;
			}
			if (*neighbours)
			{
				free(*neighbours);
				*neighbours = NULL;
			}
			return -1;
		}
		*(*neighbours + end1 - 1) = *(*neighbours + end1 - 1) + 1;//统计每个节点有多少邻接边
		*(*neighbours + end2 - 1) = *(*neighbours + end2 - 1) + 1;
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int Utils::read_edges(const char* edge_file, vector<tri_edge>& edges, std::vector<int>& node_neighbours, long num_nodes)
{
	if (edge_file == NULL ||
		num_nodes < 3)
	{
		fprintf(stderr, "read_edges(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(edge_file, L"rt");
	if (fp == NULL)
	{
		fprintf(stderr, "read_edges(): can't open %s\n", edge_file);
		return -1;
	}
	char str[1024];
	char* ptr;
	fgets(str, 1024, fp);
	long long num_edges = 0;
	num_edges = strtol(str, &ptr, 0);
	if (num_edges <= 0)
	{
		fprintf(stderr, "read_edges(): %s is unknown format!\n", edge_file);
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return -1;
	}
	edges.clear(); node_neighbours.clear();
	edges.resize(num_edges);
	node_neighbours.resize(num_nodes);
	node_neighbours.resize(num_nodes);
	for (int i = 0; i < num_nodes; i++)
	{
		node_neighbours[i] = 0;
	}
	long end1, end2, edges_number, boundry_marker;
	for (int i = 0; i < num_edges; i++)
	{
		fgets(str, 1024, fp);
		edges_number = strtol(str, &ptr, 0);
		end1 = strtol(ptr, &ptr, 0);
		end2 = strtol(ptr, &ptr, 0);
		boundry_marker = strtol(ptr, &ptr, 0);
		edges[i].end1 = end1;
		edges[i].end2 = end2;
		edges[i].num = i + 1;
		edges[i].gain = 0;
		edges[i].isResidueEdge = false;
		edges[i].phase_diff = 0.0;
		edges[i].isBoundary = (boundry_marker == 1);
		if (end1 < 1 ||
			end1 > num_nodes ||
			end2 < 1 ||
			end2 > num_nodes)
		{
			fprintf(stderr, "read_edges(): endpoints exceed 1~num_nodes!\n");
			if (fp)
			{
				fclose(fp);
				fp = NULL;
			}
			return -1;
		}
		node_neighbours[end1 - 1] += 1;//统计每个节点有多少邻接边
		node_neighbours[end2 - 1] += 1;//统计每个节点有多少邻接边
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int Utils::init_tri_node(vector<tri_node>& node_array, Mat& phase, Mat& mask, tri_edge* edges, long num_edges, int* num_neighbour, int num_nodes)
{
	if (phase.rows < 2 ||
		phase.cols < 2 ||
		phase.channels() != 1 ||
		phase.type() != CV_64F ||
		mask.rows != phase.rows ||
		mask.cols != phase.cols ||
		mask.channels() != 1 ||
		mask.type() != CV_32S ||
		edges == NULL ||
		num_edges < 3 ||
		num_neighbour == NULL ||
		num_nodes < 3)
	{
		fprintf(stderr, "init_tri_node(): input check failed!\n\n");
		return -1;
	}
	int sum = cv::countNonZero(mask);;
	if (sum != num_nodes)
	{
		fprintf(stderr, "init_tri_node(): mask and num_nodes mismatch!\n\n");
		return -1;
	}
	int rows = phase.rows;
	int cols = phase.cols;
	int count = 0;
	tri_node* ptr = NULL;
	double Phase;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (mask.at<int>(i, j) > 0)
			{
				Phase = phase.at<double>(i, j);
				ptr = new tri_node(i, j, *(num_neighbour + count), Phase);
				if (mask.at<int>(i, j) > 1)//邻接已解缠节点标记
				{
					ptr->set_status(true);
				}
				node_array.push_back(*ptr);
				delete ptr;
				ptr = NULL;
				count++;
				
			}
		}
	}


	tri_edge tmp;
	for (int i = 0; i < num_edges; i++)
	{
		tmp = *(edges + i);
		if (tmp.end1 < 0 ||
			tmp.end2 < 0 ||
			tmp.end1 > node_array.size() ||
			tmp.end2 > node_array.size())
		{
			fprintf(stderr, "init_tri_node(): edges' endpoint exceed legal value!\n\n");
			return -1;
		}
		node_array[tmp.end1 - 1].add_neigh_edge(i + 1);
		node_array[tmp.end2 - 1].add_neigh_edge(i + 1);
	}
	return 0;
}

int Utils::init_tri_node(
	vector<tri_node>& node_array,
	const Mat& phase,
	const Mat& mask,
	const vector<tri_edge>& edges,
	const vector<int>& node_neighbours,
	int num_nodes
)
{
	if (phase.rows < 2 ||
		phase.cols < 2 ||
		phase.channels() != 1 ||
		phase.type() != CV_64F ||
		mask.rows != phase.rows ||
		mask.cols != phase.cols ||
		mask.channels() != 1 ||
		mask.type() != CV_32S ||
		edges.size() < 3 ||
		(node_neighbours.size() - num_nodes) != 0 ||
		num_nodes < 3)
	{
		fprintf(stderr, "init_tri_node(): input check failed!\n\n");
		return -1;
	}
	int sum = cv::countNonZero(mask);;
	if (sum != num_nodes)
	{
		fprintf(stderr, "init_tri_node(): mask and num_nodes mismatch!\n\n");
		return -1;
	}
	long long num_edges = edges.size();
	node_array.clear();
	node_array.resize(num_nodes);
	int rows = phase.rows;
	int cols = phase.cols;
	int count = 0;
	tri_node* ptr = NULL;
	double Phase;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (mask.at<int>(i, j) > 0)
			{
				Phase = phase.at<double>(i, j);
				ptr = new tri_node(i, j, node_neighbours[count], Phase);
				if (mask.at<int>(i, j) > 1)//邻接已解缠节点标记
				{
					ptr->set_status(true);
				}
				node_array[count] = *ptr;
				delete ptr;
				ptr = NULL;
				count++;

			}
		}
	}


	tri_edge tmp;
	for (int i = 0; i < num_edges; i++)
	{
		tmp = edges[i];
		if (tmp.end1 < 0 ||
			tmp.end2 < 0 ||
			tmp.end1 > node_array.size() ||
			tmp.end2 > node_array.size())
		{
			fprintf(stderr, "init_tri_node(): edges' endpoint exceed legal value!\n\n");
			return -1;
		}
		node_array[tmp.end1 - 1].add_neigh_edge(i + 1);
		node_array[tmp.end2 - 1].add_neigh_edge(i + 1);
	}
	return 0;
}

int Utils::init_edge_phase_diff(vector<tri_edge>& edges, const vector<tri_node>& node_array)
{
	if (edges.size() < 3 || node_array.size() < 3)
	{
		fprintf(stderr, "init_edge_phase_diff(): input check failed!\n");
		return -1;
	}
	size_t num_nodes = node_array.size();
	size_t num_edges = edges.size();
	size_t end1, end2;
	double phi1, phi2;
	for (size_t i = 0; i < num_edges; i++)
	{
		end1 = edges[i].end1 < edges[i].end2 ? edges[i].end1 : edges[i].end2;
		end2 = edges[i].end1 < edges[i].end2 ? edges[i].end2 : edges[i].end1;
		node_array[end1 - 1].get_phase(&phi1);
		node_array[end2 - 1].get_phase(&phi2);
		phi1 = phi2 - phi1;
		phi1 = atan2(sin(phi1), cos(phi1));
		edges[i].phase_diff = phi1;
	}
	return 0;
}

int Utils::init_edges_quality(Mat& quality, tri_edge* edges, int num_edges, vector<tri_node>& nodes)
{
	if (quality.rows < 2 ||
		quality.cols < 2 ||
		quality.type() != CV_64F ||
		quality.channels() != 1 ||
		edges == NULL ||
		num_edges < 3||
		nodes.size() < 3
		)
	{
		fprintf(stderr, "init_edges_quality(): input check failed!\n\n");
		return -1;
	}
	int rows, cols;
	double qual = 0.0;
	for (int i = 0; i < num_edges; i++)
	{
		qual = 0.0;
		nodes[(edges + i)->end1 - 1].get_pos(&rows, &cols);
		qual += quality.at<double>(rows, cols);
		nodes[(edges + i)->end2 - 1].get_pos(&rows, &cols);
		qual += quality.at<double>(rows, cols);
		(edges + i)->quality = qual / 2.0;
	}
	return 0;
}

int Utils::init_edges_quality(const Mat& quality_map, vector<tri_edge>& edges, const vector<tri_node>& nodes)
{
	if (quality_map.rows < 2 ||
		quality_map.cols < 2 ||
		quality_map.type() != CV_64F ||
		quality_map.channels() != 1 ||
		edges.size() < 3 ||
		nodes.size() < 3
		)
	{
		fprintf(stderr, "init_edges_quality(): input check failed!\n\n");
		return -1;
	}
	int rows, cols;
	double qual = 0.0;
	size_t num_edges = edges.size();
	for (int i = 0; i < num_edges; i++)
	{
		qual = 0.0;
		nodes[edges[i].end1 - 1].get_pos(&rows, &cols);
		qual += quality_map.at<double>(rows, cols);
		nodes[edges[i].end2 - 1].get_pos(&rows, &cols);
		qual += quality_map.at<double>(rows, cols);
		edges[i].quality = qual / 2.0;
	}
	return 0;
}

int Utils::read_triangle(
	const char* ele_file,
	const char* neigh_file,
	triangle** tri,
	int* num_triangle,
	vector<tri_node>& nodes,
	tri_edge* edges,
	int num_edges
)
{
	if (ele_file == NULL ||
		neigh_file == NULL ||
		tri == NULL ||
		num_triangle == NULL||
		nodes.size() < 3||
		edges == NULL||
		num_edges < 3
		)
	{
		fprintf(stderr, "read_triangle(): input check failed!\n\n");
		return -1;
	}
	FILE* fp_ele, *fp_neigh;
	fp_ele = NULL;
	fp_neigh = NULL;
	fp_ele = openUtf8File(ele_file, L"rt");
	if (fp_ele == NULL)
	{
		fprintf(stderr, "read_triangle(): can't open %s\n", ele_file);
		return -1;
	}
	fp_neigh = openUtf8File(neigh_file, L"rt");
	if (fp_neigh == NULL)
	{
		fprintf(stderr, "read_triangle(): can't open %s\n", neigh_file);
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		return -1;
	}

	char str[INPUTMAXSIZE];
	char* ptr;
	fgets(str, INPUTMAXSIZE, fp_ele);
	*num_triangle = strtol(str, &ptr, 0);
	if (*num_triangle < 1)
	{
		fprintf(stderr, "read_triangle(): number of triangles exceed legal range!\n");
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		if (fp_neigh)
		{
			fclose(fp_neigh);
			fp_neigh = NULL;
		}
		return -1;
	}
	fgets(str, INPUTMAXSIZE, fp_neigh);

	*tri = (triangle*)malloc(*num_triangle * sizeof(triangle));
	if (*tri == NULL)
	{
		fprintf(stderr, "read_triangle(): out of memory!\n");
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		if (fp_neigh)
		{
			fclose(fp_neigh);
			fp_neigh = NULL;
		}
		return -1;
	}
	memset(*tri, 0, sizeof(triangle) * (*num_triangle));

	int p1, p2, p3, neigh1, neigh2, neigh3, num1, num2;
	for (int i = 0; i < *num_triangle; i++)
	{
		fgets(str, INPUTMAXSIZE, fp_ele);
		num1 = strtol(str, &ptr, 0);
		p1 = strtol(ptr, &ptr, 0);
		p2 = strtol(ptr, &ptr, 0);
		p3 = strtol(ptr, &ptr, 0);

		fgets(str, INPUTMAXSIZE, fp_neigh);
		num2 = strtol(str, &ptr, 0);
		neigh1 = strtol(ptr, &ptr, 0);
		neigh2 = strtol(ptr, &ptr, 0);
		neigh3 = strtol(ptr, &ptr, 0);
		(*tri + i)->p1 = p1;
		(*tri + i)->p2 = p2;
		(*tri + i)->p3 = p3;
		(*tri + i)->neigh1 = neigh1;
		(*tri + i)->neigh2 = neigh2;
		(*tri + i)->neigh3 = neigh3;
		(*tri + i)->num = num1;
	}
	if (fp_ele)
	{
		fclose(fp_ele);
		fp_ele = NULL;
	}
	if (fp_neigh)
	{
		fclose(fp_neigh);
		fp_neigh = NULL;
	}
	//获取三角形的边序号
	int count;
	int edge[3];
	memset(edge, 0, sizeof(int) * 3);
	for (int j = 0; j < *num_triangle; j++)
	{
		count = 0;
		for (long edge_val : nodes[(*tri + j)->p1 - 1].get_neigh_edges())
		{
			if ((edges + edge_val - 1)->end1 == (*tri + j)->p2 ||
				(edges + edge_val - 1)->end1 == (*tri + j)->p3 ||
				(edges + edge_val - 1)->end2 == (*tri + j)->p2 ||
				(edges + edge_val - 1)->end2 == (*tri + j)->p3
				)
			{
				edge[count] = edge_val;
				count++;
			}
		}
		for (long edge_val : nodes[(*tri + j)->p2 - 1].get_neigh_edges())
		{
			if ((edges + edge_val - 1)->end1 == (*tri + j)->p3 ||
				(edges + edge_val - 1)->end2 == (*tri + j)->p3)
			{
				edge[count] = edge_val;
				//count++;
			}
		}
		(*tri + j)->edge1 = edge[0];
		(*tri + j)->edge2 = edge[1];
		(*tri + j)->edge3 = edge[2];
	}
	
	
	
	return 0;
}

int Utils::read_triangle(
	const char* ele_file,
	const char* neigh_file, 
	vector<triangle>& triangle,
	vector<tri_node>& nodes,
	vector<tri_edge>& edges
)
{
	if (ele_file == NULL ||
		neigh_file == NULL ||
		nodes.size() < 3 ||
		edges.size() < 3
		)
	{
		fprintf(stderr, "read_triangle(): input check failed!\n\n");
		return -1;
	}
	FILE* fp_ele, * fp_neigh;
	fp_ele = NULL;
	fp_neigh = NULL;
	fp_ele = openUtf8File(ele_file, L"rt");
	if (fp_ele == NULL)
	{
		fprintf(stderr, "read_triangle(): can't open %s\n", ele_file);
		return -1;
	}
	fp_neigh = openUtf8File(neigh_file, L"rt");
	if (fp_neigh == NULL)
	{
		fprintf(stderr, "read_triangle(): can't open %s\n", neigh_file);
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		return -1;
	}

	char str[INPUTMAXSIZE];
	char* ptr;
	fgets(str, INPUTMAXSIZE, fp_ele);
	long num_triangle = strtol(str, &ptr, 0);
	if (num_triangle < 1)
	{
		fprintf(stderr, "read_triangle(): number of triangles exceed legal range!\n");
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		if (fp_neigh)
		{
			fclose(fp_neigh);
			fp_neigh = NULL;
		}
		return -1;
	}
	fgets(str, INPUTMAXSIZE, fp_neigh);
	triangle.clear();
	triangle.resize(num_triangle);

	int p1, p2, p3, neigh1, neigh2, neigh3, num1, num2;
	for (int i = 0; i < num_triangle; i++)
	{
		fgets(str, INPUTMAXSIZE, fp_ele);
		num1 = strtol(str, &ptr, 0);
		p1 = strtol(ptr, &ptr, 0);
		p2 = strtol(ptr, &ptr, 0);
		p3 = strtol(ptr, &ptr, 0);

		fgets(str, INPUTMAXSIZE, fp_neigh);
		num2 = strtol(str, &ptr, 0);
		neigh1 = strtol(ptr, &ptr, 0);
		neigh2 = strtol(ptr, &ptr, 0);
		neigh3 = strtol(ptr, &ptr, 0);
		triangle[i].p1 = p1;
		triangle[i].p2 = p2;
		triangle[i].p3 = p3;
		triangle[i].neigh1 = neigh1;
		triangle[i].neigh2 = neigh2;
		triangle[i].neigh3 = neigh3;
		triangle[i].num = num1;
	}
	if (fp_ele)
	{
		fclose(fp_ele);
		fp_ele = NULL;
	}
	if (fp_neigh)
	{
		fclose(fp_neigh);
		fp_neigh = NULL;
	}
	//获取三角形的边序号
	int count;
	int edge[3];
	memset(edge, 0, sizeof(int) * 3);
	for (int j = 0; j < num_triangle; j++)
	{
		count = 0;
		for (long edge_val : nodes[triangle[j].p1 - 1].get_neigh_edges())
		{
			if ((edges[edge_val - 1].end1 == triangle[j].p2) ||
				(edges[edge_val - 1].end1 == triangle[j].p3) ||
				(edges[edge_val - 1].end2 == triangle[j].p2)||
				(edges[edge_val - 1].end2 == triangle[j].p3)
				)
			{
				edge[count] = edge_val;
				count++;
			}
		}
		for (long edge_val : nodes[triangle[j].p2 - 1].get_neigh_edges())
		{
			if ((edges[edge_val - 1].end1 == triangle[j].p3) ||
				(edges[edge_val - 1].end2 == triangle[j].p3))
			{
				edge[count] = edge_val;
				//count++;
			}
		}
		triangle[j].edge1 = edge[0];
		triangle[j].edge2 = edge[1];
		triangle[j].edge3 = edge[2];
	}

	return 0;
}

int Utils::gen_delaunay(const char* filename, const char* exe_path)
{
	if (filename == NULL || !*filename || exe_path == NULL || !*exe_path)
	{
		fprintf(stderr, "gen_delaunay(): input check failed!\n");
		return -1;
	}
	std::wstring nodeFile;
	std::wstring executableFolder;
	PathResolver::Error pathError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(filename, nodeFile, &pathError) ||
		!PathResolver::utf8ToWide(exe_path, executableFolder, &pathError))
	{
		fprintf(stderr, "gen_delaunay(): invalid UTF-8 path.\n");
		return -1;
	}
	HANDLE input = CreateFileW(nodeFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (input == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "gen_delaunay(): can't open input node file.\n");
		return -1;
	}
	CloseHandle(input);
	if (!executableFolder.empty() && executableFolder.back() != L'\\' && executableFolder.back() != L'/') executableFolder.push_back(L'\\');
	const std::wstring executable = executableFolder + L"delaunay.exe";
	auto quoteArgument = [](const std::wstring& argument) {
		std::wstring quoted = L"\"";
		size_t slashes = 0;
		for (wchar_t character : argument)
		{
			if (character == L'\\') { ++slashes; continue; }
			if (character == L'\"') quoted.append(slashes * 2 + 1, L'\\');
			else quoted.append(slashes, L'\\');
			quoted.push_back(character);
			slashes = 0;
		}
		quoted.append(slashes * 2, L'\\');
		quoted.push_back(L'\"');
		return quoted;
	};
	std::wstring commandLine = quoteArgument(executable) + L" -en " + quoteArgument(nodeFile);
	std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
	commandLineBuffer.push_back(L'\0');

	STARTUPINFOW si = {};
	PROCESS_INFORMATION p_i = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = FALSE;
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (!job)
	{
		fprintf(stderr, "gen_delaunay(): CreateJobObjectW failed (%lu).\n", GetLastError());
		return -1;
	}
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		{
			fprintf(stderr, "gen_delaunay(): SetInformationJobObject failed (%lu).\n", GetLastError());
			CloseHandle(job);
			return -1;
		}
	}
	if (!CreateProcessW(executable.c_str(), commandLineBuffer.data(), nullptr, nullptr, FALSE,
		CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
		nullptr, nullptr, &si, &p_i))
	{
		fprintf(stderr, "gen_delaunay(): CreateProcessW failed (%lu).\n", GetLastError());
		if (job) CloseHandle(job);
		return -1;
	}
	if (!AssignProcessToJobObject(job, p_i.hProcess))
	{
		const DWORD error = GetLastError();
		TerminateProcess(p_i.hProcess, static_cast<UINT>(-3));
		WaitForSingleObject(p_i.hProcess, INFINITE);
		CloseHandle(p_i.hThread);
		CloseHandle(p_i.hProcess);
		CloseHandle(job);
		fprintf(stderr, "gen_delaunay(): AssignProcessToJobObject failed (%lu).\n", error);
		return -1;
	}
	if (ResumeThread(p_i.hThread) == static_cast<DWORD>(-1))
	{
		const DWORD error = GetLastError();
		TerminateJobObject(job, static_cast<UINT>(-3));
		WaitForSingleObject(p_i.hProcess, INFINITE);
		CloseHandle(p_i.hThread);
		CloseHandle(p_i.hProcess);
		CloseHandle(job);
		fprintf(stderr, "gen_delaunay(): ResumeThread failed (%lu).\n", error);
		return -1;
	}
	WaitForSingleObject(p_i.hProcess, INFINITE);
	DWORD exitCode = 0;
	const bool success = GetExitCodeProcess(p_i.hProcess, &exitCode) && exitCode == 0;
	CloseHandle(p_i.hThread);
	CloseHandle(p_i.hProcess);
	CloseHandle(job);
	if (!success) { fprintf(stderr, "gen_delaunay(): delaunay.exe failed (exit=%lu).\n", exitCode); return -1; }
	return 0;
}


int Utils::write_node_file(const char* filename, const Mat& mask)
{
	if (filename == NULL ||
		mask.rows < 2 ||
		mask.cols < 2 ||
		mask.channels() != 1 ||
		mask.type() != CV_32S
		)
	{
		fprintf(stderr, "write_node_file(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = openUtf8File(filename, L"wt");
	if (fp == NULL)
	{
		fprintf(stderr, "write_node_file(): can't open %s\n", filename);
		return -1;
	}
	int nonzero = cv::countNonZero(mask);
	if (nonzero <= 0)
	{
		fprintf(stderr, "write_node_file(): no node exist!\n");
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return -1;
	}
	int dim, attr1, attr2, count;
	dim = 2;
	attr1 = 0;
	attr2 = 0;
	fprintf(fp, "%d %d %d %d\n", nonzero, dim, attr1, attr2);
	int rows = mask.rows;
	int cols = mask.cols;
	count = 1;
	double x, y;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (mask.at<int>(i, j) > 0)
			{
				x = double(i + 1);
				y = double(j + 1);
				fprintf(fp, "%d %lf %lf\n", count, x, y);
				count++;
			}
		}
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int Utils::PS_amp_dispersion(const vector<Mat>& amplitude, double thresh, Mat& mask)
{
	if (
		amplitude.size() < 3 ||
		thresh < 0.0
		)
	{
		fprintf(stderr, "PS_amp_dispersion(): input check failed!\n\n");
		return -1;
	}
	if (amplitude[0].rows < 1 || amplitude[0].cols < 1 || amplitude[0].channels() != 1 || amplitude[0].type() != CV_64F)
	{
		fprintf(stderr, "PS_amp_dispersion(): input check failed!\n\n");
		return -1;
	}
	int nr = amplitude[0].rows;
	int nc = amplitude[0].cols;
	int num_images = static_cast<int>(amplitude.size());
	Mat tmp1 = Mat::zeros(nr, nc, CV_64F);
	tmp1.copyTo(mask);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		double mean, tmp;
		for (int j = 0; j < nc; j++)
		{
			mean = 0;
			tmp = 0;
			for (int k = 0; k < num_images; k++)
			{
				mean += amplitude[k].at<double>(i, j);
				//mean += (amplitude + k)->at<double>(i, j);
			}
			mean = mean / num_images;
			for (int m = 0; m < num_images; m++)
			{
				tmp += (amplitude[m].at<double>(i, j) - mean) * (amplitude[m].at<double>(i, j) - mean);
				//tmp += ((amplitude + m)->at<double>(i, j) - mean) * ((amplitude + m)->at<double>(i, j) - mean);
			}
			tmp = sqrt(tmp / num_images);
			if (mean < 1e-8) mean = 1e-8;
			tmp = tmp / mean;
			if (tmp <= thresh)
			{
				mask.at<double>(i, j) = 1.0;
			}

		}
	}
	return 0;
}

int Utils::butter_lowpass(int grid_size, int n_win, double low_pass_wavelength, Mat& lowpass)
{
	if (grid_size < 3 ||
		n_win < 3||
		low_pass_wavelength < 1
		)
	{
		fprintf(stderr, "butter_lowpass(): input check failed!\n\n");
		return -1;
	}
	double freq0 = 1 / low_pass_wavelength;
	double start = -(n_win) / grid_size / n_win / 2;
	double interval = 1 / grid_size / n_win;
	double end = (n_win - 2) / grid_size / n_win / 2;
	Mat freq_i = Mat::zeros(1, n_win, CV_64F);
	for (int i = 0; i < n_win; i++)
	{
		freq_i.at<double>(0, i) = start + i * interval;
	}
	freq_i = freq_i / freq0;
	cv::pow(freq_i, 10, freq_i);
	freq_i = freq_i + 1.0;
	freq_i = 1 / freq_i;
	Mat tmp;
	cv::transpose(freq_i, tmp);
	tmp = tmp * freq_i;
	int ret = fftshift2(tmp);
	if (return_check(ret, "fftshift2(*)", error_head)) return -1;
	tmp.copyTo(lowpass);
	//freq_i = -(n_win) / grid_size / n_win / 2:1 / grid_size / n_win : (n_win - 2) / grid_size / n_win / 2;
	//butter_i = 1. / (1 + (freq_i / freq0). ^ (2 * 5));
	//low_pass = butter_i'*butter_i;
	//	low_pass = fftshift(low_pass);
	return 0;
}

int Utils::circshift(Mat& out, const cv::Point& delta)
{
	Size sz = out.size();
	if (sz.height <= 0 ||
		sz.width <= 0
		)
	{
		fprintf(stderr, "circshift(): input check failed!\n\n");
		return -1;
	}
	if ((sz.height == 1 && sz.width == 1)||
		(delta.x == 0 && delta.y == 0)
		)
	{
		return 0;
	}
	int x = delta.x;
	int y = delta.y;
	if (x > 0) x = x % sz.width;
	if (y > 0) y = y % sz.height;
	if (x < 0) x = x % sz.width + sz.width;
	if (y < 0) y = y % sz.height + sz.height;
	vector<Mat> planes;
	split(out, planes);
	for (int i = 0; i < planes.size(); i++)
	{
		Mat tmp0, tmp1, tmp2, tmp3;
		Mat q0(planes[i], Rect(0, 0, sz.width, sz.height - y));
		Mat q1(planes[i], Rect(0, sz.height - y, sz.width, y));
		q0.copyTo(tmp0);
		q1.copyTo(tmp1);
		tmp0.copyTo(planes[i](Rect(0, y, sz.width, sz.height - y)));
		tmp1.copyTo(planes[i](Rect(0, 0, sz.width, y)));


		Mat q2(planes[i], Rect(0, 0, sz.width - x, sz.height));
		Mat q3(planes[i], Rect(sz.width - x, 0, x, sz.height));
		q2.copyTo(tmp2);
		q3.copyTo(tmp3);
		tmp2.copyTo(planes[i](Rect(x, 0, sz.width - x, sz.height)));
		tmp3.copyTo(planes[i](Rect(0, 0, x, sz.height)));
	}
	merge(planes, out);
	return 0;
}

int Utils::fftshift2(Mat& out)
{
	Size sz = out.size();
	Point pt(0, 0);
	pt.x = (int)floor(sz.width / 2.0);
	pt.y = (int)floor(sz.height / 2.0);
	int ret = circshift(out, pt);
	if (return_check(ret, "circshift(*)", error_head)) return -1;
	return 0;
}

int Utils::ifftshift(Mat& out)
{
	Size sz = out.size();
	Point pt(0, 0);
	pt.x = (int)ceil(sz.width / 2.0);
	pt.y = (int)ceil(sz.height / 2.0);
	int ret = circshift(out, pt);
	if (return_check(ret, "circshift(*)", error_head)) return -1;
	return 0;
}

int Utils::fft2(Mat& Src, Mat& Dst)
{
	if (Src.rows < 1 ||
		Src.cols < 1 ||
		Src.channels() != 1 ||
		Src.type() != CV_64F)
	{
		fprintf(stderr, "fft2(): input check failed!\n\n");
		return -1;
	}
	Mat planes[] = { Mat_<double>(Src), Mat::zeros(Src.size(), CV_64F) };
	Mat complexImg;
	merge(planes, 2, complexImg);
	dft(complexImg, Dst, DFT_COMPLEX_OUTPUT);
	return 0;
}

int Utils::fft2(ComplexMat& src, ComplexMat& Dst)
{
	if (src.GetCols() < 1 ||
		src.GetRows() < 1 ||
		src.type() != CV_64F
		)
	{
		fprintf(stderr, "fft2(): input check failed!\n\n");
		return -1;
	}
	Mat re, im;
	src.GetIm().copyTo(im);
	src.GetRe().copyTo(re);
	Mat planes[] = { re, im };
	Mat complexImg;
	merge(planes, 2, complexImg);
	dft(complexImg, complexImg, DFT_COMPLEX_INPUT);
	split(complexImg, planes);
	Dst.SetRe(planes[0]);
	Dst.SetIm(planes[1]);
	return 0;
}

int Utils::ifft2(ComplexMat& src, ComplexMat& dst)
{
	if (src.GetCols() < 1 ||
		src.GetRows() < 1 ||
		src.type() != CV_64F
		)
	{
		fprintf(stderr, "ifft2(): input check failed!\n\n");
		return -1;
	}
	Mat re, im;
	im = src.GetIm();
	re = src.GetRe();
	Mat planes[] = { re, im };
	Mat complexImg;
	merge(planes, 2, complexImg);
	idft(complexImg, complexImg, DFT_COMPLEX_OUTPUT);
	split(complexImg, planes);
	dst.SetRe(planes[0]);
	dst.SetIm(planes[1]);
	return 0;
}

int Utils::std(const Mat& input, double* std)
{
	if (input.rows < 1 ||
		input.cols < 1 ||
		input.channels() != 1 ||
		//input.type() != CV_64F ||
		std == NULL
		)
	{
		fprintf(stderr, "std(): input check failed!\n\n");
		return -1;
	}
	Mat data;
	double mean_v = cv::mean(input)[0];
	data = input - mean_v;
	data = data.mul(data);
	double sum_v = cv::sum(data)[0];
	int x = data.rows * data.cols - 1;
	if (x > 0)
	{
		*std = sqrt(sum_v / double(x));
	}
	else
	{
		*std = 0.0;
	}
	
	return 0;
}

//int Utils::stack_coregistration(
//	vector<ComplexMat>& SAR_images,
//	Mat& offset,
//	int Master_index,
//	int coh_method,
//	int interp_times,
//	int blocksize
//)
//{
//	if (SAR_images.size() < 2||
//		Master_index < 1 ||
//		Master_index > SAR_images.size() ||
//		coh_method < 0 ||
//		coh_method > 1 ||
//		interp_times < 1||
//		blocksize < 4
//		)
//	{
//		fprintf(stderr, "stack_coregistration(): input check failed!\n\n");
//		return -1;
//	}
//	vector<ComplexMat> SAR_images_out;
//	SAR_images_out.resize(SAR_images.size());
//	Registration coregis;
//	int nr = SAR_images[Master_index - 1].GetRows();
//	int nc = SAR_images[Master_index - 1].GetCols();
//	int n_images = SAR_images.size();
//	Mat move_row = Mat::zeros(1, n_images, CV_32S);
//	Mat move_col = Mat::zeros(1, n_images, CV_32S);
//	Mat Slave_indx = Mat::zeros(1, n_images - 1, CV_32S);
//	Mat offset_topleft = Mat::zeros(n_images, 2, CV_32S);
//	int count = 0;
//	for (int i = 0; i < n_images; i++)
//	{
//		if (i != Master_index - 1)
//		{
//			Slave_indx.at<int>(0, count) = i;
//			count++;
//		}
//	}
//	////////////粗配准///////////////
//	ComplexMat master;
//	master = SAR_images[Master_index - 1];
//	volatile bool parallel_flag = true;
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < n_images - 1; i++)
//	{
//		if (!parallel_flag) continue;
//		ComplexMat slave;
//		int ret, move_r, move_c;
//		int slave_img = Slave_indx.at<int>(0, i);
//		slave = SAR_images[slave_img];
//		ret = coregis.real_coherent(master, slave, &move_r, &move_c);
//		if (ret < 0)
//		{
//			parallel_flag = false;
//			continue;
//		}
//		move_row.at<int>(0, slave_img) = move_r;
//		move_col.at<int>(0, slave_img) = move_c;
//	}
//	if (parallel_check(parallel_flag, "stack_coregistration()", parallel_error_head)) return -1;
//	////////////粗配准公共部分裁剪///////////////
//	int move_r_min, move_r_max, move_c_min, move_c_max;
//	move_r_min = 100000000;
//	move_r_max = -100000000;
//	move_c_min = 100000000;
//	move_c_max = -100000000;
//
//	int cut_rows;
//	for (int i = 0; i < n_images; i++)
//	{
//		move_r_min = move_r_min < move_row.at<int>(0, i) ? move_r_min : move_row.at<int>(0, i);
//		move_c_min = move_c_min < move_col.at<int>(0, i) ? move_c_min : move_col.at<int>(0, i);
//		move_r_max = move_r_max > move_row.at<int>(0, i) ? move_r_max : move_row.at<int>(0, i);
//		move_c_max = move_c_max > move_col.at<int>(0, i) ? move_c_max : move_col.at<int>(0, i);
//	}
//	if (abs(move_r_max) >= nr ||
//		abs(move_r_min) >= nr ||
//		abs(move_c_min) >= nc ||
//		abs(move_c_max) >= nc ||
//		abs(move_r_max - move_r_min) >= nr||
//		abs(move_c_max - move_c_min) >= nc
//		)
//	{
//		fprintf(stderr, "stack_coregistration(): SAR images have no common area!\n\n");
//		return -1;
//	}
//	if (move_r_min >= 0)
//	{
//		SAR_images_out[Master_index - 1] = SAR_images[Master_index - 1](Range(0, nr - move_r_max), Range(0, nc));
//		offset_topleft.at<int>(Master_index - 1, 0) = 0;
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < n_images - 1; i++)
//		{
//			int slave_ix = Slave_indx.at<int>(0, i);
//			int row_start = move_row.at<int>(0, slave_ix);
//			int row_end = nr + move_row.at<int>(0, slave_ix) - move_r_max;
//			int col_start = 0;
//			int col_end = SAR_images[slave_ix].GetCols();
//			SAR_images_out[slave_ix] = SAR_images[slave_ix](Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 0) = move_row.at<int>(0, slave_ix);
//		}
//	}
//	if (move_r_max <= 0 && move_r_min < 0)
//	{
//		SAR_images_out[Master_index - 1] = SAR_images[Master_index - 1](Range(-move_r_min, nr), Range(0, nc));
//		offset_topleft.at<int>(Master_index - 1, 0) = -move_r_min;
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < n_images - 1; i++)
//		{
//			int slave_ix = Slave_indx.at<int>(0, i);
//			int row_start = move_row.at<int>(0, slave_ix) - move_r_min;
//			int row_end = nr + move_row.at<int>(0, slave_ix);
//			int col_start = 0;
//			int col_end = SAR_images[slave_ix].GetCols();
//			SAR_images_out[slave_ix] = SAR_images[slave_ix](Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 0) = move_row.at<int>(0, slave_ix) - move_r_min;
//		}
//	}
//	if (move_r_max > 0 && move_r_min < 0)
//	{
//		SAR_images_out[Master_index - 1] = SAR_images[Master_index - 1](Range(-move_r_min, nr - move_r_max), Range(0, nc));
//		offset_topleft.at<int>(Master_index - 1, 0) = -move_r_min;
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < n_images - 1; i++)
//		{
//			int slave_ix = Slave_indx.at<int>(0, i);
//			int row_start = move_row.at<int>(0, slave_ix) - move_r_min;
//			int row_end = nr + move_row.at<int>(0, slave_ix) - move_r_max;
//			int col_start = 0;
//			int col_end = SAR_images[slave_ix].GetCols();
//			SAR_images_out[slave_ix] = SAR_images[slave_ix](Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 0) = move_row.at<int>(0, slave_ix) - move_r_min;
//		}
//	}
//
//
//	if (move_c_min >= 0)
//	{
//		cut_rows = SAR_images_out[Master_index - 1].GetRows();
//		SAR_images_out[Master_index - 1] = SAR_images_out[Master_index - 1](Range(0, cut_rows), Range(0, nc - move_c_max));
//		offset_topleft.at<int>(Master_index - 1, 1) = 0;
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < n_images - 1; i++)
//		{
//			int slave_ix = Slave_indx.at<int>(0, i);
//			int row_start = 0;
//			int row_end = SAR_images_out[slave_ix].GetRows();
//			int col_start = move_col.at<int>(0, slave_ix);
//			int col_end = nc + move_col.at<int>(0, slave_ix) - move_c_max;
//			SAR_images_out[slave_ix] = SAR_images_out[slave_ix](Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 1) = move_col.at<int>(0, slave_ix);
//		}
//	}
//	if (move_c_max <= 0 && move_c_min < 0)
//	{
//		cut_rows = SAR_images_out[Master_index - 1].GetRows();
//		SAR_images_out[Master_index - 1] = SAR_images_out[Master_index - 1](Range(0, cut_rows), Range(-move_c_min, nc));
//		offset_topleft.at<int>(Master_index - 1, 1) = -move_c_min;
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < n_images - 1; i++)
//		{
//			int slave_ix = Slave_indx.at<int>(0, i);
//			int row_start = 0;
//			int row_end = SAR_images_out[slave_ix].GetRows();
//			int col_start = move_col.at<int>(0, slave_ix) - move_c_min;
//			int col_end = nc + move_col.at<int>(0, slave_ix);
//			SAR_images_out[slave_ix] = SAR_images_out[slave_ix](Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 1) = move_col.at<int>(0, slave_ix) - move_c_min;
//		}
//	}
//	if (move_c_max > 0 && move_c_min < 0)
//	{
//		cut_rows = SAR_images_out[Master_index - 1].GetRows();
//		SAR_images_out[Master_index - 1] = SAR_images_out[Master_index - 1](Range(0, cut_rows), Range(-move_c_min, nc - move_c_max));
//		offset_topleft.at<int>(Master_index - 1, 1) = -move_c_min;
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < n_images - 1; i++)
//		{
//			int slave_ix = Slave_indx.at<int>(0, i);
//			int row_start = 0;
//			int row_end = SAR_images_out[slave_ix].GetRows();
//			int col_start = move_col.at<int>(0, slave_ix) - move_c_min;
//			int col_end = nc + move_col.at<int>(0, slave_ix) - move_c_max;
//			SAR_images_out[slave_ix] = SAR_images_out[slave_ix](Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 1) = move_col.at<int>(0, slave_ix) - move_c_min;
//		}
//	}
//	offset_topleft.copyTo(offset);
//
//	////////////////////////精配准////////////////////////////
//	SAR_images[Master_index - 1] = SAR_images_out[Master_index - 1];
//	master = SAR_images_out[Master_index - 1];
//	//这里并行加速，因为子函数已经进行了加速
////#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < n_images - 1; i++)
//	{
//		ComplexMat slave;
//		int slave_ix = Slave_indx.at<int>(0, i);
//		slave = SAR_images_out[slave_ix];
//		int ret;
//		ret = coregis.coregistration_subpixel(master, slave, blocksize, interp_times);
//		if (return_check(ret, "registration_subpixel(*, *, *, *)", error_head)) return -1;
//		SAR_images[slave_ix] = slave;
//		//cout << i + 1 << "/" << n_images - 1 << "\n";
//	}
//	return 0;
//}
//
//int Utils::stack_coregistration(
//	vector<string>& SAR_images,
//	vector<string>& SAR_images_out,
//	Mat& offset,
//	int Master_index,
//	int interp_times,
//	int blocksize
//)
//{
//	if (SAR_images.size() < 2 ||
//		Master_index < 1 ||
//		Master_index > SAR_images.size() ||
//		SAR_images_out.size() != SAR_images.size() ||
//		interp_times < 1 ||
//		blocksize < 16
//		)
//	{
//		fprintf(stderr, "stack_coregistration(): input check failed!\n\n");
//		return -1;
//	}
//
//	Registration coregis; FormatConversion conversion;
//	int n_images = SAR_images.size(), ret;
//	Mat move_row = Mat::zeros(1, n_images, CV_32S);
//	Mat move_col = Mat::zeros(1, n_images, CV_32S);
//	Mat Slave_indx = Mat::zeros(1, n_images - 1, CV_32S);
//	Mat offset_topleft = Mat::zeros(n_images, 2, CV_32S);
//	int count = 0;
//	//创建配准后输出的h5文件
//	for (int i = 0; i < n_images; i++)
//	{
//		ret = conversion.creat_new_h5(SAR_images_out[i].c_str());
//		if (return_check(ret, "creat_new_h5()", error_head)) return -1;
//	}
//	for (int i = 0; i < n_images; i++)
//	{
//		if (i != Master_index - 1)
//		{
//			Slave_indx.at<int>(0, count) = i;
//			count++;
//		}
//	}
//	//检查并确保SAR图像尺寸大小一致
//	int min_row = 10000000, min_col = 10000000;
//	Mat azimuth_len, range_len;
//	for (int i = 0; i < n_images; i++)
//	{
//		ret = conversion.read_array_from_h5(SAR_images[i].c_str(), "azimuth_len", azimuth_len);
//		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//		ret = conversion.read_array_from_h5(SAR_images[i].c_str(), "range_len", range_len);
//		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//		min_row = azimuth_len.at<int>(0, 0) > min_row ? min_row : azimuth_len.at<int>(0, 0);
//		min_col = range_len.at<int>(0, 0) > min_col ? min_col : range_len.at<int>(0, 0);
//	}
//	if (min_col < 1 || min_row < 1)
//	{
//		fprintf(stderr, "invalid SAR images size\n");
//		return -1;
//	}
//	
//	////////////粗配准///////////////
//	ComplexMat master, slave, master_out, slave_out;
//	ret = conversion.read_slc_from_h5(SAR_images[Master_index - 1].c_str(), master);
//	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
//	master = master(cv::Range(0, min_row), cv::Range(0, min_col));
//	if (master.type() != CV_64F) master.convertTo(master, CV_64F);
//	int nr = master.GetRows();
//	int nc = master.GetCols();
//	for (int i = 0; i < n_images - 1; i++)
//	{
//		
//		int move_r, move_c;
//		int slave_img = Slave_indx.at<int>(0, i);
//		ret = conversion.read_slc_from_h5(SAR_images[slave_img].c_str(), slave);
//		if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
//		slave = slave(cv::Range(0, min_row), cv::Range(0, min_col));
//		if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
//		ret = coregis.real_coherent(master, slave, &move_r, &move_c);
//		if (return_check(ret, "real_coherent()", error_head)) return -1;
//		move_row.at<int>(0, slave_img) = move_r;
//		move_col.at<int>(0, slave_img) = move_c;
//	}
//	////////////粗配准公共部分裁剪///////////////
//	int move_r_min, move_r_max, move_c_min, move_c_max;
//	move_r_min = 100000000;
//	move_r_max = -100000000;
//	move_c_min = 100000000;
//	move_c_max = -100000000;
//
//	int cut_rows;
//	for (int i = 0; i < n_images; i++)
//	{
//		move_r_min = move_r_min < move_row.at<int>(0, i) ? move_r_min : move_row.at<int>(0, i);
//		move_c_min = move_c_min < move_col.at<int>(0, i) ? move_c_min : move_col.at<int>(0, i);
//		move_r_max = move_r_max > move_row.at<int>(0, i) ? move_r_max : move_row.at<int>(0, i);
//		move_c_max = move_c_max > move_col.at<int>(0, i) ? move_c_max : move_col.at<int>(0, i);
//	}
//	if (abs(move_r_max) >= nr ||
//		abs(move_r_min) >= nr ||
//		abs(move_c_min) >= nc ||
//		abs(move_c_max) >= nc ||
//		abs(move_r_max - move_r_min) >= nr ||
//		abs(move_c_max - move_c_min) >= nc
//		)
//	{
//		fprintf(stderr, "stack_coregistration(): SAR images have no common area!\n\n");
//		return -1;
//	}
//
//	//主图像裁剪
//	if (move_r_min >= 0)
//	{
//		master = master(Range(0, nr - move_r_max), Range(0, nc));
//		offset_topleft.at<int>(Master_index - 1, 0) = 0;
//	}
//	if (move_r_max <= 0 && move_r_min < 0)
//	{
//		master = master(Range(-move_r_min, nr), Range(0, nc));
//		offset_topleft.at<int>(Master_index - 1, 0) = -move_r_min;
//	}
//	if (move_r_max > 0 && move_r_min < 0)
//	{
//		master = master(Range(-move_r_min, nr - move_r_max), Range(0, nc));
//		offset_topleft.at<int>(Master_index - 1, 0) = -move_r_min;
//	}
//	if (move_c_min >= 0)
//	{
//		cut_rows = master.GetRows();
//		master = master(Range(0, cut_rows), Range(0, nc - move_c_max));
//		offset_topleft.at<int>(Master_index - 1, 1) = 0;
//	}
//	if (move_c_max <= 0 && move_c_min < 0)
//	{
//		cut_rows = master.GetRows();
//		master = master(Range(0, cut_rows), Range(-move_c_min, nc));
//		offset_topleft.at<int>(Master_index - 1, 1) = -move_c_min;
//	}
//	if (move_c_max > 0 && move_c_min < 0)
//	{
//		cut_rows = master.GetRows();
//		master = master(Range(0, cut_rows), Range(-move_c_min, nc - move_c_max));
//		offset_topleft.at<int>(Master_index - 1, 1) = -move_c_min;
//	}
//	ret = conversion.write_slc_to_h5(SAR_images_out[Master_index - 1].c_str(), master);
//	if (return_check(ret, "write_slc_to_h5()", error_head)) return -1;
//	azimuth_len.at<int>(0, 0) = master.GetRows();
//	range_len.at<int>(0, 0) = master.GetCols();
//	ret = conversion.write_array_to_h5(SAR_images_out[Master_index - 1].c_str(), "azimuth_len", azimuth_len);
//	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//	ret = conversion.write_array_to_h5(SAR_images_out[Master_index - 1].c_str(), "range_len", range_len);
//	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//	//辅图像裁剪
//	for (int i = 0; i < n_images - 1; i++)
//	{
//		int slave_ix, row_start, row_end, col_start, col_end;
//		slave_ix = Slave_indx.at<int>(0, i);
//		ret = conversion.read_slc_from_h5(SAR_images[slave_ix].c_str(), slave);
//		if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
//		if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
//		if (move_r_min >= 0)
//		{
//			
//			row_start = move_row.at<int>(0, slave_ix);
//			row_end = nr + move_row.at<int>(0, slave_ix) - move_r_max;
//			col_start = 0;
//			col_end = slave.GetCols();
//			slave = slave(Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 0) = move_row.at<int>(0, slave_ix);
//		}
//		if (move_r_max <= 0 && move_r_min < 0)
//		{
//			row_start = move_row.at<int>(0, slave_ix) - move_r_min;
//			row_end = nr + move_row.at<int>(0, slave_ix);
//			col_start = 0;
//			col_end = slave.GetCols();
//			slave = slave(Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 0) = move_row.at<int>(0, slave_ix) - move_r_min;
//		}
//		if (move_r_max > 0 && move_r_min < 0)
//		{
//			row_start = move_row.at<int>(0, slave_ix) - move_r_min;
//			row_end = nr + move_row.at<int>(0, slave_ix) - move_r_max;
//			col_start = 0;
//			col_end = slave.GetCols();
//			slave = slave(Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 0) = move_row.at<int>(0, slave_ix) - move_r_min;
//		}
//
//
//		if (move_c_min >= 0)
//		{
//			row_start = 0;
//			row_end = slave.GetRows();
//			col_start = move_col.at<int>(0, slave_ix);
//			col_end = nc + move_col.at<int>(0, slave_ix) - move_c_max;
//			slave = slave(Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 1) = move_col.at<int>(0, slave_ix);
//		}
//		if (move_c_max <= 0 && move_c_min < 0)
//		{
//			row_start = 0;
//			row_end = slave.GetRows();
//			col_start = move_col.at<int>(0, slave_ix) - move_c_min;
//			col_end = nc + move_col.at<int>(0, slave_ix);
//			slave = slave(Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 1) = move_col.at<int>(0, slave_ix) - move_c_min;
//		}
//		if (move_c_max > 0 && move_c_min < 0)
//		{
//			row_start = 0;
//			row_end = slave.GetRows();
//			col_start = move_col.at<int>(0, slave_ix) - move_c_min;
//			col_end = nc + move_col.at<int>(0, slave_ix) - move_c_max;
//			slave = slave(Range(row_start, row_end), Range(col_start, col_end));
//			offset_topleft.at<int>(slave_ix, 1) = move_col.at<int>(0, slave_ix) - move_c_min;
//		}
//
//		//精配准
//		ret = coregis.coregistration_subpixel(master, slave, blocksize, interp_times);
//		if (return_check(ret, "coregistration_subpixel()", error_head)) return -1;
//		//写出
//		ret = conversion.write_slc_to_h5(SAR_images_out[slave_ix].c_str(), slave);
//		if (return_check(ret, "write_slc_to_h5()", error_head)) return -1;
//		azimuth_len.at<int>(0, 0) = slave.GetRows();
//		range_len.at<int>(0, 0) = slave.GetCols();
//		ret = conversion.write_array_to_h5(SAR_images_out[slave_ix].c_str(), "azimuth_len", azimuth_len);
//		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//		ret = conversion.write_array_to_h5(SAR_images_out[slave_ix].c_str(), "range_len", range_len);
//		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//	}
//	offset_topleft.copyTo(offset);
//
//
//	return 0;
//}
//
//int Utils::stack_coregistration(
//	vector<string>& SAR_images, 
//	vector<string>& SAR_images_out,
//	int Master_index,
//	int interp_times,
//	int blocksize
//)
//{
//	if (SAR_images.size() < 2 ||
//		Master_index < 1 ||
//		Master_index > SAR_images.size() ||
//		SAR_images_out.size() != SAR_images.size() ||
//		interp_times < 1 ||
//		blocksize < 16
//		)
//	{
//		fprintf(stderr, "stack_coregistration(): input check failed!\n\n");
//		return -1;
//	}
//	//获取各图像的尺寸，并创建输出h5文件
//	FormatConversion conversion;
//	int ret, type;
//	int n_images = SAR_images.size();
//	Mat images_rows, images_cols, tmp;
//	images_rows = Mat::zeros(n_images, 1, CV_32S); images_cols = Mat::zeros(n_images, 1, CV_32S);
//	for (int i = 0; i < n_images; i++)
//	{
//		ret = conversion.creat_new_h5(SAR_images_out[i].c_str());
//		if (return_check(ret, "creat_new_h5()", error_head)) return -1;
//
//		ret = conversion.read_array_from_h5(SAR_images[i].c_str(), "range_len", tmp);
//		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//		images_cols.at<int>(i, 0) = tmp.at<int>(0, 0);
//
//		ret = conversion.read_array_from_h5(SAR_images[i].c_str(), "azimuth_len", tmp);
//		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//		images_rows.at<int>(i, 0) = tmp.at<int>(0, 0);
//	}
//	//分块读取数据并求取偏移量
//	Utils util; Registration regis;
//	int rows = images_rows.at<int>(Master_index - 1, 0); int cols = images_cols.at<int>(Master_index - 1, 0);
//	int m = rows / blocksize;
//	int n = cols / blocksize;
//	if (m * n < 10)
//	{
//		fprintf(stderr, "stack_coregistration(): try smaller blocksize!\n");
//		return -1;
//	}
//	Mat offset_r = Mat::zeros(m, n, CV_64F); Mat offset_c = Mat::zeros(m, n, CV_64F);
//	Mat offset_coord_row = Mat::zeros(m, n, CV_64F);
//	Mat offset_coord_col = Mat::zeros(m, n, CV_64F);
//	//子块中心坐标
//	for (int i = 0; i < m; i++)
//	{
//		for (int j = 0; j < n; j++)
//		{
//			offset_coord_row.at<double>(i, j) = ((double)blocksize) / 2 * (double)(2 * i + 1);
//			offset_coord_col.at<double>(i, j) = ((double)blocksize) / 2 * (double)(2 * j + 1);
//		}
//	}
//	//根据输入图像尺寸大小判断是否分块读取（超过20000×20000则分块读取，否则一次性读取）
//	ComplexMat master_w, slave_w;
//	bool b_block = true; bool master_read = false;
//	if (rows * cols < 20000 * 20000) b_block = false;
//	for (int ii = 0; ii < n_images; ii++)
//	{
//		if (ii == Master_index - 1) continue;
//		if (!b_block)//不分块读取
//		{
//			if (!master_read)
//			{
//				ret = conversion.read_slc_from_h5(SAR_images[Master_index - 1].c_str(), master_w);
//				if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
//				master_read = true;
//				type = master_w.type();
//				ret = conversion.write_slc_to_h5(SAR_images_out[Master_index - 1].c_str(), master_w);//写主图像
//			}
//			
//			ret = conversion.read_slc_from_h5(SAR_images[ii].c_str(), slave_w);
//			if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
//			if (type != slave_w.type())
//			{
//				fprintf(stderr, "stack_coregistration(): images type mismatch!\n");
//				return -1;
//			}
//			if (type != CV_16S && type != CV_64F)
//			{
//				fprintf(stderr, "stack_coregistration(): data type not supported!\n");
//				return -1;
//			}
//		}
//		//分块读取并计算偏移量
//		int mm, nn;
//		mm = images_rows.at<int>(ii, 0) / blocksize;
//		nn = images_cols.at<int>(ii, 0) / blocksize;
//		if (!b_block)
//		{
//# pragma omp parallel for schedule(guided)
//			
//			for (int j = 0; j < m; j++)
//			{
//				int offset_row, offset_col, move_r, move_c;
//				ComplexMat master, slave, master_interp, slave_interp;
//				for (int k = 0; k < n; k++)
//				{
//					offset_row = j * blocksize; offset_col = k * blocksize;
//					if ((j + 1) * blocksize < images_rows.at<int>(ii, 0) && (k + 1) * blocksize < images_cols.at<int>(ii, 0))
//					{
//						master = master_w(cv::Range(offset_row, offset_row + blocksize), cv::Range(offset_col, offset_col + blocksize));
//						slave = slave_w(cv::Range(offset_row, offset_row + blocksize), cv::Range(offset_col, offset_col + blocksize));
//
//
//
//						//计算偏移量
//						if (master.type() != CV_64F) master.convertTo(master, CV_64F);
//						if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
//						move_r = 0; move_c = 0;
//						ret = regis.interp_paddingzero(master, master_interp, interp_times);
//						//if (return_check(ret, "interp_paddingzero()", error_head)) return -1;
//						ret = regis.interp_paddingzero(slave, slave_interp, interp_times);
//						//if (return_check(ret, "interp_paddingzero()", error_head)) return -1;
//						ret = regis.real_coherent(master_interp, slave_interp, &move_r, &move_c);
//						//if (return_check(ret, "real_coherent()", error_head)) return -1;
//						offset_r.at<double>(j, k) = double(move_r) / double(interp_times);
//						offset_c.at<double>(j, k) = double(move_c) / double(interp_times);
//					}
//
//				}
//			}
//			
//		}
//		else
//		{
//			int offset_row, offset_col, move_r, move_c;
//			ComplexMat master, slave, master_interp, slave_interp;
//			for (int j = 0; j < m; j++)
//			{
//				for (int k = 0; k < n; k++)
//				{
//					offset_row = j * blocksize; offset_col = k * blocksize;
//					if ((j + 1) * blocksize < images_rows.at<int>(ii, 0) && (k + 1) * blocksize < images_cols.at<int>(ii, 0))
//					{
//						//mm = j + 1; nn = k + 1;//记录实际的子块行列数
//						ret = conversion.read_subarray_from_h5(SAR_images[Master_index - 1].c_str(), "s_im", offset_row, offset_col, blocksize, blocksize, master.im);
//						if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
//						ret = conversion.read_subarray_from_h5(SAR_images[Master_index - 1].c_str(), "s_re", offset_row, offset_col, blocksize, blocksize, master.re);
//						if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
//						ret = conversion.read_subarray_from_h5(SAR_images[ii].c_str(), "s_im", offset_row, offset_col, blocksize, blocksize, slave.im);
//						if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
//						ret = conversion.read_subarray_from_h5(SAR_images[ii].c_str(), "s_re", offset_row, offset_col, blocksize, blocksize, slave.re);
//						if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
//
//						//计算偏移量
//						if (master.type() != CV_64F) master.convertTo(master, CV_64F);
//						if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
//
//						ret = regis.interp_paddingzero(master, master_interp, interp_times);
//						if (return_check(ret, "interp_paddingzero()", error_head)) return -1;
//						ret = regis.interp_paddingzero(slave, slave_interp, interp_times);
//						if (return_check(ret, "interp_paddingzero()", error_head)) return -1;
//						ret = regis.real_coherent(master_interp, slave_interp, &move_r, &move_c);
//						if (return_check(ret, "real_coherent()", error_head)) return -1;
//						offset_r.at<double>(j, k) = double(move_r) / double(interp_times);
//						offset_c.at<double>(j, k) = double(move_c) / double(interp_times);
//					}
//
//				}
//			}
//		}
//		
//
//		//剔除outliers
//		m = mm; n = nn;//更新实际子块行列数
//		Mat sentinel = Mat::zeros(m, n, CV_64F);
//		int ix, iy, count = 0, c = 0; double delta, thresh = 2.0;
//		for (int i = 0; i < m; i++)
//		{
//			for (int j = 0; j < n; j++)
//			{
//				count = 0;
//				//上
//				ix = j;
//				iy = i - 1; iy = iy < 0 ? 0 : iy;
//				delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
//				delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
//				if (fabs(delta) >= thresh) count++;
//				//下
//				ix = j;
//				iy = i + 1; iy = iy > m - 1 ? m - 1 : iy;
//				delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
//				delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
//				if (fabs(delta) >= thresh) count++;
//				//左
//				ix = j - 1; ix = ix < 0 ? 0 : ix;
//				iy = i;
//				delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
//				delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
//				if (fabs(delta) >= thresh) count++;
//				//右
//				ix = j + 1; ix = ix > n - 1 ? n - 1 : ix;
//				iy = i;
//				delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
//				delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
//				if (fabs(delta) >= thresh) count++;
//
//				if (count > 2) { sentinel.at<double>(i, j) = 1.0; c++; }
//			}
//		}
//		Mat offset_c_0, offset_r_0, offset_coord_row_0, offset_coord_col_0;
//		offset_c_0 = Mat::zeros(m * n - c, 1, CV_64F);
//		offset_r_0 = Mat::zeros(m * n - c, 1, CV_64F);
//		offset_coord_row_0 = Mat::zeros(m * n - c, 1, CV_64F);
//		offset_coord_col_0 = Mat::zeros(m * n - c, 1, CV_64F);
//		count = 0;
//		for (int i = 0; i < m; i++)
//		{
//			for (int j = 0; j < n; j++)
//			{
//				if (sentinel.at<double>(i, j) < 0.5)
//				{
//					offset_r_0.at<double>(count, 0) = offset_r.at<double>(i, j);
//					offset_c_0.at<double>(count, 0) = offset_c.at<double>(i, j);
//					offset_coord_row_0.at<double>(count, 0) = offset_coord_row.at<double>(i, j);
//					offset_coord_col_0.at<double>(count, 0) = offset_coord_col.at<double>(i, j);
//					count++;
//				}
//			}
//		}
//
//
//		m = 1; n = count;
//		if (count < 10)
//		{
//			fprintf(stderr, "stack_coregistration(): insufficient valide sub blocks!\n");
//			return -1;
//		}
//		//偏移量拟合（坐标做归一化处理）
//		//拟合公式为 offser_row / offser_col = a0 + a1 * x + a2 * y;
//		double offset_x = (double)cols / 2;
//		double offset_y = (double)rows / 2;
//		double scale_x = (double)cols;
//		double scale_y = (double)rows;
//		offset_coord_row_0 -= offset_y;
//		offset_coord_col_0 -= offset_x;
//		offset_coord_row_0 /= scale_y;
//		offset_coord_col_0 /= scale_x;
//		Mat A = Mat::ones(m * n, 3, CV_64F);
//		Mat temp, A_t;
//		offset_coord_col_0.copyTo(A(Range(0, m * n), Range(1, 2)));
//
//		offset_coord_row_0.copyTo(A(Range(0, m * n), Range(2, 3)));
//
//	
//		cv::transpose(A, A_t);
//
//		Mat b_r, b_c, coef_r, coef_c, error_r, error_c, b_t, a, a_t;
//
//		A.copyTo(a);
//		cv::transpose(a, a_t);
//		offset_r_0.copyTo(b_r);
//		b_r = A_t * b_r;
//
//		offset_c_0.copyTo(b_c);
//		b_c = A_t * b_c;
//
//		A = A_t * A;
//
//		double rms1 = -1.0; double rms2 = -1.0;
//		Mat eye = Mat::zeros(m * n, m * n, CV_64F);
//		for (int i = 0; i < m * n; i++)
//		{
//			eye.at<double>(i, i) = 1.0;
//		}
//		if (cv::invert(A, error_r, cv::DECOMP_LU) > 0)
//		{
//			cv::transpose(offset_r_0, b_t);
//			error_r = b_t * (eye - a * error_r * a_t) * offset_r_0;
//			rms1 = sqrt(error_r.at<double>(0, 0) / double(m * n));
//		}
//		if (cv::invert(A, error_c, cv::DECOMP_LU) > 0)
//		{
//			cv::transpose(offset_c_0, b_t);
//			error_c = b_t * (eye - a * error_c * a_t) * offset_c_0;
//			rms2 = sqrt(error_c.at<double>(0, 0) / double(m * n));
//		}
//		if (!cv::solve(A, b_r, coef_r, cv::DECOMP_NORMAL))
//		{
//			fprintf(stderr, "stack_coregistration(): matrix deficiency!\n");
//			return -1;
//		}
//		if (!cv::solve(A, b_c, coef_c, cv::DECOMP_NORMAL))
//		{
//			fprintf(stderr, "stack_coregistration(): matrix deficiency!\n");
//			return -1;
//		}
//
//		/*---------------------------------------*/
//	    /*    双线性插值获取重采样后的辅图像     */
//	    /*---------------------------------------*/
//		ComplexMat slave1;
//		ret = conversion.read_slc_from_h5(SAR_images[ii].c_str(), slave1);
//		if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
//		//if (slave1.type() != CV_16S) slave1.convertTo(slave1, CV_16S);
//		int rows_slave, cols_slave;
//		rows_slave = slave1.GetRows(); cols_slave = slave1.GetCols();
//		type = slave1.type();
//		ComplexMat slave_tmp; slave_tmp.re = Mat::zeros(rows, cols, type); slave_tmp.im = Mat::zeros(rows, cols, type);
//#pragma omp parallel for schedule(guided)
//		for (int i = 0; i < rows; i++)
//		{
//			double x, y, iiii, jjjj; Mat tmp(1, 3, CV_64F); Mat result;
//			int mm0, nn0, mm1, nn1;
//			double offset_rows, offset_cols, upper, lower;
//			for (int j = 0; j < cols; j++)
//			{
//				jjjj = (double)j;
//				iiii = (double)i;
//				x = (jjjj - offset_x) / scale_x;
//				y = (iiii - offset_y) / scale_y;
//				tmp.at<double>(0, 0) = 1.0;
//				tmp.at<double>(0, 1) = x;
//				tmp.at<double>(0, 2) = y;
//				//tmp.at<double>(0, 3) = x * y;
//				//tmp.at<double>(0, 4) = x * x;
//				//tmp.at<double>(0, 5) = y * y;
//				result = tmp * coef_r;
//				offset_rows = result.at<double>(0, 0);
//				result = tmp * coef_c;
//				offset_cols = result.at<double>(0, 0);
//
//				iiii += offset_rows;
//				jjjj += offset_cols;
//
//				mm0 = (int)floor(iiii); nn0 = (int)floor(jjjj);
//				if (mm0 < 0 || nn0 < 0 || mm0 > rows_slave - 1 || nn0 > cols_slave - 1)
//				{
//					if (type == CV_64F)
//					{
//						slave_tmp.re.at<double>(i, j) = 0;
//						slave_tmp.im.at<double>(i, j) = 0;
//					}
//					else
//					{
//						slave_tmp.re.at<short>(i, j) = 0;
//						slave_tmp.im.at<short>(i, j) = 0;
//					}
//				}
//				else
//				{
//					mm1 = mm0 + 1; nn1 = nn0 + 1;
//					mm1 = mm1 >= rows_slave - 1 ? rows_slave - 1 : mm1;
//					nn1 = nn1 >= cols_slave - 1 ? cols_slave - 1 : nn1;
//					if (type == CV_16S)
//					{
//						//实部插值
//						upper = (double)slave1.re.at<short>(mm0, nn0) + double(slave1.re.at<short>(mm0, nn1) - slave1.re.at<short>(mm0, nn0)) * (jjjj - (double)nn0);
//						lower = (double)slave1.re.at<short>(mm1, nn0) + double(slave1.re.at<short>(mm1, nn1) - slave1.re.at<short>(mm1, nn0)) * (jjjj - (double)nn0);
//						slave_tmp.re.at<short>(i, j) = upper + double(lower - upper) * (iiii - (double)mm0);
//						//虚部插值
//						upper = (double)slave1.im.at<short>(mm0, nn0) + double(slave1.im.at<short>(mm0, nn1) - slave1.im.at<short>(mm0, nn0)) * (jjjj - (double)nn0);
//						lower = (double)slave1.im.at<short>(mm1, nn0) + double(slave1.im.at<short>(mm1, nn1) - slave1.im.at<short>(mm1, nn0)) * (jjjj - (double)nn0);
//						slave_tmp.im.at<short>(i, j) = upper + double(lower - upper) * (iiii - (double)mm0);
//					}
//					else
//					{
//						//实部插值
//						upper = slave1.re.at<double>(mm0, nn0) + (slave1.re.at<double>(mm0, nn1) - slave1.re.at<double>(mm0, nn0)) * (jjjj - (double)nn0);
//						lower = slave1.re.at<double>(mm1, nn0) + (slave1.re.at<double>(mm1, nn1) - slave1.re.at<double>(mm1, nn0)) * (jjjj - (double)nn0);
//						slave_tmp.re.at<double>(i, j) = upper + (lower - upper) * (iiii - (double)mm0);
//						//虚部插值
//						upper = slave1.im.at<double>(mm0, nn0) + (slave1.im.at<double>(mm0, nn1) - slave1.im.at<double>(mm0, nn0)) * (jjjj - (double)nn0);
//						lower = slave1.im.at<double>(mm1, nn0) + (slave1.im.at<double>(mm1, nn1) - slave1.im.at<double>(mm1, nn0)) * (jjjj - (double)nn0);
//						slave_tmp.im.at<double>(i, j) = upper + (lower - upper) * (iiii - (double)mm0);
//					}
//					
//				}
//
//			}
//		}
//
//		ret = conversion.write_slc_to_h5(SAR_images_out[ii].c_str(), slave_tmp);
//
//
//	}
//	return 0;
//}

int Utils::hist(Mat& input, double lowercenter, double uppercenter, double interval, Mat& out)
{
	if (input.rows != 1 ||
		input.cols < 3 ||
		input.channels() != 1 ||
		input.type() != CV_64F ||
		interval >(uppercenter - lowercenter) ||
		lowercenter > uppercenter ||
		interval <= 0.0
		)
	{
		fprintf(stderr, "hist(): input check failed!\n\n");
		return -1;
	}
	Mat tmp;
	input.copyTo(tmp);
	cv::sort(tmp, tmp, SORT_ASCENDING + SORT_EVERY_ROW);
	int len = int(floor((uppercenter - lowercenter) / interval)) + 1;
	int nc = input.cols;
	Mat H = Mat::zeros(1, len, CV_64F);
	double cmp = lowercenter + interval*0.5;
	int count = 0;
	int idx = 0;
	int k;
	for (k = 0; k < nc; k++)
	{
		if (tmp.at<double>(0, k) < cmp) count++;
		else
		{
			H.at<double>(0, idx) = double(count);
			idx++;
			count = 1;
			cmp += interval;
			if (idx >= len - 1) break;
		}
	}
	H.at<double>(0, len - 1) = H.at<double>(0, len - 1) + double(nc - k);
	H.copyTo(out);
	return 0;
}

int Utils::hist(Mat& input, double lowerbound, double upperbound, double interval, Mat& out_x, Mat& out_y)
{
	if (input.empty() ||
		input.channels() != 1 ||
		lowerbound >= upperbound ||
		interval >= (upperbound - lowerbound)
		)
	{
		fprintf(stderr, "hist(): input check failed!\n");
		return -1;
	}
	int num_bins = static_cast<int>((upperbound - lowerbound) / interval + 1);
	double cmp = lowerbound + interval;
	out_x.create(1, num_bins, CV_64F);
	out_y.create(1, num_bins, CV_64F);
	for (int i = 0; i < num_bins; i++)
	{
		out_x.at<double>(i) = lowerbound + interval * (0.5 + i);
		out_y.at<double>(i) = 0.0;
	}
	Mat tmp;
	input.copyTo(tmp);
	if (tmp.rows != 1) tmp = tmp.reshape(0, 1);
	if (tmp.type() != CV_64F) tmp.convertTo(tmp, CV_64F);
	int num_element = tmp.cols; int count = 0; int bin_count = 0; int bin_num_count = 0;
	cv::sort(tmp, tmp, SORT_ASCENDING + SORT_EVERY_ROW);
	while (count < num_element)
	{
		if (tmp.at<double>(count) >= cmp)
		{
			out_y.at<double>(bin_count) = bin_num_count;
			bin_num_count = 0;
			bin_count++;
			cmp += interval;
		}
		bin_num_count++;
		count++;
	}
	out_y.at<double>(num_bins - 1) = out_y.at<double>(num_bins - 2);

	return 0;
}

int Utils::gaussian_curve_fit(Mat& input_x, Mat& input_y, double* mu, double* sigma_square, double* scale)
{
	if (input_x.empty() ||
		input_y.size() != input_x.size() ||
		input_x.type() != CV_64F ||
		input_y.type() != CV_64F ||
		!mu || !sigma_square || !scale
		)
	{
		fprintf(stderr, "gaussian_curve_fit(): input check failed!\n");
		return -1;
	}
	if (input_x.rows != 1)
	{
		input_x = input_x.reshape(0, 1);
		input_y = input_y.reshape(0, 1);
	}
	int cols = input_x.cols;
	Mat b(cols, 1, CV_64F), A(cols, 3, CV_64F); A = 1.0;
	for (int i = 0; i < cols; i++)
	{
		b.at<double>(i) = log(input_y.at<double>(i));
		A.at<double>(i, 1) = input_x.at<double>(i);
		A.at<double>(i, 2) = input_x.at<double>(i) * input_x.at<double>(i);
	}
	Mat A_t;
	cv::transpose(A, A_t);
	A = A_t * A;
	b = A_t * b;
	Mat x;
	if (!cv::solve(A, b, x, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "gaussian_curve_fit(): can't solve LS problem!\n");
		return -1;
	}
	*sigma_square = -1.0 / x.at<double>(2) / 2.0;
	*mu = *sigma_square * x.at<double>(1);
	*scale = exp(x.at<double>(0) + (*mu) * (*mu) / 2.0 / *sigma_square);
	return 0;
}

int Utils::stateVec_interp(Mat& stateVec, double time_interval, Mat& stateVec_interp)
{
	if (stateVec.empty() ||
		stateVec.cols != 7||
		stateVec.rows < 7||
		time_interval < 0.0)
	{
		fprintf(stderr, "stateVec_interp(): input check failed!\n");
		return -1;
	}
	Mat statevec;
	if (stateVec.type() != CV_64F) stateVec.convertTo(statevec, CV_64F);
	else stateVec.copyTo(statevec);

	int rows = statevec.rows; int cols = statevec.cols;
	Mat time; statevec(cv::Range(0, rows), cv::Range(0, 1)).copyTo(time);
	time = time - time.at<double>(0, 0);
	Mat A = Mat::ones(rows, 6, CV_64F);
	Mat temp, b;
	//拟合x
	Mat x; statevec(cv::Range(0, rows), cv::Range(1, 2)).copyTo(x);
	time.copyTo(A(cv::Range(0, rows), cv::Range(1, 2)));
	temp = time.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(2, 3)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(3, 4)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(4, 5)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(5, 6)));
	transpose(A, temp);
	b = temp * x;
	A = temp * A;
	if (!cv::solve(A, b, x, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "stateVec_interp(): matrix deficiency!\n");
		return -1;
	}

	//拟合y
	A = Mat::ones(rows, 6, CV_64F);
	Mat y; statevec(cv::Range(0, rows), cv::Range(2, 3)).copyTo(y);
	time.copyTo(A(cv::Range(0, rows), cv::Range(1, 2)));
	temp = time.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(2, 3)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(3, 4)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(4, 5)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(5, 6)));
	transpose(A, temp);
	b = temp * y;
	A = temp * A;
	if (!cv::solve(A, b, y, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "stateVec_interp(): matrix deficiency!\n");
		return -1;
	}

	//拟合z

	A = Mat::ones(rows, 6, CV_64F);
	Mat z; statevec(cv::Range(0, rows), cv::Range(3, 4)).copyTo(z);
	time.copyTo(A(cv::Range(0, rows), cv::Range(1, 2)));
	temp = time.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(2, 3)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(3, 4)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(4, 5)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(5, 6)));
	transpose(A, temp);
	b = temp * z;
	A = temp * A;
	if (!cv::solve(A, b, z, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "stateVec_interp(): matrix deficiency!\n");
		return -1;
	}

	//拟合vx

	A = Mat::ones(rows, 6, CV_64F);
	Mat vx; statevec(cv::Range(0, rows), cv::Range(4, 5)).copyTo(vx);
	time.copyTo(A(cv::Range(0, rows), cv::Range(1, 2)));
	temp = time.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(2, 3)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(3, 4)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(4, 5)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(5, 6)));
	transpose(A, temp);
	b = temp * vx;
	A = temp * A;
	if (!cv::solve(A, b, vx, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "stateVec_interp(): matrix deficiency!\n");
		return -1;
	}

	//拟合vy

	A = Mat::ones(rows, 6, CV_64F);
	Mat vy; statevec(cv::Range(0, rows), cv::Range(5, 6)).copyTo(vy);
	time.copyTo(A(cv::Range(0, rows), cv::Range(1, 2)));
	temp = time.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(2, 3)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(3, 4)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(4, 5)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(5, 6)));
	transpose(A, temp);
	b = temp * vy;
	A = temp * A;
	if (!cv::solve(A, b, vy, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "stateVec_interp(): matrix deficiency!\n");
		return -1;
	}

	//拟合vz

	A = Mat::ones(rows, 6, CV_64F);
	Mat vz; statevec(cv::Range(0, rows), cv::Range(6, 7)).copyTo(vz);
	time.copyTo(A(cv::Range(0, rows), cv::Range(1, 2)));
	temp = time.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(2, 3)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(3, 4)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(4, 5)));
	temp = temp.mul(time);
	temp.copyTo(A(cv::Range(0, rows), cv::Range(5, 6)));
	transpose(A, temp);
	b = temp * vz;
	A = temp * A;
	if (!cv::solve(A, b, vz, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "stateVec_interp(): matrix deficiency!\n");
		return -1;
	}

	//插值

	int count = 1;
	double t = 0;
	while (t <= time.at<double>(rows - 1, 0))
	{
		count++;
		t += time_interval;
	}
	stateVec_interp.create(count, 7, CV_64F);
	Mat tt = Mat::ones(count, 6, CV_64F);
	tt(cv::Range(0, count), cv::Range(0, 1)).copyTo(stateVec_interp(cv::Range(0, count), cv::Range(0, 1)));
	t = 0.0;
	for (int i = 0; i < count; i++)
	{
		tt.at<double>(i, 1) = t;
		tt.at<double>(i, 2) = t * t;
		tt.at<double>(i, 3) = t * t * t;
		tt.at<double>(i, 4) = t * t * t * t;
		tt.at<double>(i, 5) = t * t * t * t * t;
		t += time_interval;
	}
	x = tt * x;
	y = tt * y;
	z = tt * z;
	vx = tt * vx;
	vy = tt * vy;
	vz = tt * vz;
	x.copyTo(stateVec_interp(cv::Range(0, count), cv::Range(1, 2)));
	y.copyTo(stateVec_interp(cv::Range(0, count), cv::Range(2, 3)));
	z.copyTo(stateVec_interp(cv::Range(0, count), cv::Range(3, 4)));
	vx.copyTo(stateVec_interp(cv::Range(0, count), cv::Range(4, 5)));
	vy.copyTo(stateVec_interp(cv::Range(0, count), cv::Range(5, 6)));
	vz.copyTo(stateVec_interp(cv::Range(0, count), cv::Range(6, 7)));
	return 0;
}

int Utils::coord_conversion(Mat& coefficient, Mat& coord_in_1, Mat& coord_in_2, Mat& coord_out)
{
	if (coefficient.cols != 32 ||
		coefficient.rows != 1 ||
		coefficient.type() != CV_64F ||
		coord_in_1.empty() ||
		coord_in_2.empty() ||
		coord_in_1.type() != CV_64F ||
		coord_in_2.type() != CV_64F||
		coord_in_1.rows != coord_in_2.rows||
		coord_in_1.cols != coord_in_2.cols)
	{
		fprintf(stderr, "coord_conversion(): input check failed!\n");
		return -1;
	}
	double offset_out, scale_out, offset_in_1, offset_in_2, scale_in_1, scale_in_2;
	double a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11,
		a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24;
	offset_out = coefficient.at<double>(0, 0);
	scale_out = coefficient.at<double>(0, 1);
	offset_in_1 = coefficient.at<double>(0, 2);
	scale_in_1 = coefficient.at<double>(0, 3);
	offset_in_2 = coefficient.at<double>(0, 4);
	scale_in_2 = coefficient.at<double>(0, 5);

	a0 = coefficient.at<double>(0, 6);
	a1 = coefficient.at<double>(0, 7);
	a2 = coefficient.at<double>(0, 8);
	a3 = coefficient.at<double>(0, 9);
	a4 = coefficient.at<double>(0, 10);
	a5 = coefficient.at<double>(0, 11);
	a6 = coefficient.at<double>(0, 12);
	a7 = coefficient.at<double>(0, 13);
	a8 = coefficient.at<double>(0, 14);
	a9 = coefficient.at<double>(0, 15);
	a10 = coefficient.at<double>(0, 16);
	a11 = coefficient.at<double>(0, 17);
	a12 = coefficient.at<double>(0, 18);
	a13 = coefficient.at<double>(0, 19);
	a14 = coefficient.at<double>(0, 20);
	a15 = coefficient.at<double>(0, 21);
	a16 = coefficient.at<double>(0, 22);
	a17 = coefficient.at<double>(0, 23);
	a18 = coefficient.at<double>(0, 24);
	a19 = coefficient.at<double>(0, 25);
	a20 = coefficient.at<double>(0, 26);
	a21 = coefficient.at<double>(0, 27);
	a22 = coefficient.at<double>(0, 28);
	a23 = coefficient.at<double>(0, 29);
	a24 = coefficient.at<double>(0, 30);
	int rows = coord_in_1.rows; int cols = coord_in_1.cols;
	Mat coord_out1(rows, cols, CV_64F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		double in1, in2, out;
		for (int j = 0; j < cols; j++)
		{
			in1 = (coord_in_1.at<double>(i, j) - offset_in_1) / scale_in_1;
			in2 = (coord_in_2.at<double>(i, j) - offset_in_2) / scale_in_2;
			out = a0 + a1 * in1 + a2 * in1 * in1 + a3 * in1 * in1 * in1 + a4 * in1 * in1 * in1 * in1 +
				a5 * in2 + a6 * in2 * in1 + a7 * in2 * in1 * in1 + a8 * in2 * in1 * in1 * in1 + a9 * in2 * in1 * in1 * in1 * in1 +
				a10 * in2 * in2 + a11 * in2 * in2 * in1 + a12 * in2 * in2 * in1 * in1 + a13 * in2 * in2 * in1 * in1 * in1 + a14 * in2 * in2 * in1 * in1 * in1 * in1 +
				a15 * in2 * in2 * in2 + a16 * in2 * in2 * in2 * in1 + a17 * in2 * in2 * in2 * in1 * in1 + a18 * in2 * in2 * in2 * in1 * in1 * in1 + a19 * in2 * in2 * in2 * in1 * in1 * in1 * in1 +
				a20 * in2 * in2 * in2 * in2 + a21 * in2 * in2 * in2 * in2 * in1 + a22 * in2 * in2 * in2 * in2 * in1 * in1 + a23 * in2 * in2 * in2 * in2 * in1 * in1 * in1 + a24 * in2 * in2 * in2 * in2 * in1 * in1 * in1 * in1;
			out = out * scale_out + offset_out;
			coord_out1.at<double>(i, j) = out;
		}
	}
	coord_out1.copyTo(coord_out);
	return 0;
}
