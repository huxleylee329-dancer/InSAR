// Registration.cpp : 定义 DLL 应用程序的导出函数。
//

#include<string.h>
#include "stdafx.h"
#include<math.h>
#include <atomic>
#include"..\include\Registration.h"
#include"..\include\FormatConversion.h"
#ifdef _DEBUG
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#pragma comment(lib, "FormatConversion_d.lib")
#else
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "FormatConversion.lib")
#endif // _DEBUG
using namespace cv;

namespace
{
	constexpr double INSAR_PI = 3.141592653589793238462643383279502884;

	inline double sinc_func(double x)
	{
		if (std::abs(x) < 1.0e-12) return 1.0;
		double pix = INSAR_PI * x;
		return std::sin(pix) / pix;
	}

	inline double hamming_window(double x, int radius)
	{
		double ax = std::abs(x);
		if (ax > static_cast<double>(radius)) return 0.0;

		// Hamming window: center = 1, edge ≈ 0.08
		return 0.54 + 0.46 * std::cos(INSAR_PI * ax / static_cast<double>(radius));
	}

	inline double windowed_sinc_weight(double x, int radius)
	{
		if (std::abs(x) > static_cast<double>(radius)) return 0.0;
		return sinc_func(x) * hamming_window(x, radius);
	}

	inline double mat_get_as_double(const cv::Mat& img, int r, int c)
	{
		switch (img.depth())
		{
		case CV_64F:
			return img.at<double>(r, c);
		case CV_32F:
			return static_cast<double>(img.at<float>(r, c));
		case CV_16S:
			return static_cast<double>(img.at<short>(r, c));
		default:
			return 0.0;
		}
	}

	inline void mat_set_from_double(cv::Mat& img, int r, int c, double v)
	{
		switch (img.depth())
		{
		case CV_64F:
			img.at<double>(r, c) = v;
			break;
		case CV_32F:
			img.at<float>(r, c) = static_cast<float>(v);
			break;
		case CV_16S:
			img.at<short>(r, c) = cv::saturate_cast<short>(v);
			break;
		default:
			break;
		}
	}

	inline double sinc_interp2d(const cv::Mat& img, double row, double col, int radius)
	{
		const int rows = img.rows;
		const int cols = img.cols;

		// 坐标完全越界，直接置零
		if (row < 0.0 || col < 0.0 || row > static_cast<double>(rows - 1) || col > static_cast<double>(cols - 1))
		{
			return 0.0;
		}

		// 为避免边界处 sinc 核不完整导致伪影，靠近边缘的像元直接置零
		if (row < radius || col < radius ||
			row > static_cast<double>(rows - 1 - radius) ||
			col > static_cast<double>(cols - 1 - radius))
		{
			return 0.0;
		}

		int r0 = static_cast<int>(std::floor(row));
		int c0 = static_cast<int>(std::floor(col));

		double sum_val = 0.0;
		double sum_w = 0.0;

		for (int rr = r0 - radius; rr <= r0 + radius; rr++)
		{
			double wr = windowed_sinc_weight(row - static_cast<double>(rr), radius);
			if (std::abs(wr) < 1.0e-15) continue;

			for (int cc = c0 - radius; cc <= c0 + radius; cc++)
			{
				double wc = windowed_sinc_weight(col - static_cast<double>(cc), radius);
				if (std::abs(wc) < 1.0e-15) continue;

				double w = wr * wc;
				sum_val += mat_get_as_double(img, rr, cc) * w;
				sum_w += w;
			}
		}

		if (std::abs(sum_w) < 1.0e-14) return 0.0;

		// 归一化，避免有限窗截断导致幅度偏移
		return sum_val / sum_w;
	}

	void padBorder(const cv::Mat& src, cv::Mat& dst)
	{
		int nr = src.rows;
		int nc = src.cols;
		dst = cv::Mat::zeros(nr + 3, nc + 3, CV_64F);

		src(cv::Range(0, 1), cv::Range(0, 1)).copyTo(dst(cv::Range(0, 1), cv::Range(0, 1)));
		src(cv::Range(0, 1), cv::Range(0, nc)).copyTo(dst(cv::Range(0, 1), cv::Range(1, nc + 1)));
		src(cv::Range(0, 1), cv::Range(nc - 1, nc)).copyTo(dst(cv::Range(0, 1), cv::Range(nc + 1, nc + 2)));
		src(cv::Range(0, 1), cv::Range(nc - 1, nc)).copyTo(dst(cv::Range(0, 1), cv::Range(nc + 2, nc + 3)));
		src(cv::Range(0, nr), cv::Range(0, 1)).copyTo(dst(cv::Range(1, nr + 1), cv::Range(0, 1)));
		src(cv::Range(nr - 1, nr), cv::Range(0, 1)).copyTo(dst(cv::Range(nr + 1, nr + 2), cv::Range(0, 1)));
		src(cv::Range(nr - 1, nr), cv::Range(0, 1)).copyTo(dst(cv::Range(nr + 2, nr + 3), cv::Range(0, 1)));
		src(cv::Range(nr - 1, nr), cv::Range(0, nc)).copyTo(dst(cv::Range(nr + 1, nr + 2), cv::Range(1, nc + 1)));
		src(cv::Range(nr - 1, nr), cv::Range(nc - 1, nc)).copyTo(dst(cv::Range(nr + 1, nr + 2), cv::Range(nc + 2, nc + 3)));
		dst(cv::Range(nr + 1, nr + 2), cv::Range(0, nc + 3)).copyTo(dst(cv::Range(nr + 2, nr + 3), cv::Range(0, nc + 3)));
		src(cv::Range(0, nr), cv::Range(nc - 1, nc)).copyTo(dst(cv::Range(1, nr + 1), cv::Range(nc + 1, nc + 2)));
		src(cv::Range(nr - 1, nr), cv::Range(nc - 1, nc)).copyTo(dst(cv::Range(nr + 2, nr + 3), cv::Range(nc + 1, nc + 2)));
		src(cv::Range(nr - 1, nr), cv::Range(nc - 1, nc)).copyTo(dst(cv::Range(nr + 2, nr + 3), cv::Range(nc + 2, nc + 3)));
		dst(cv::Range(0, nr + 3), cv::Range(nc + 1, nc + 2)).copyTo(dst(cv::Range(0, nr + 3), cv::Range(nc + 2, nc + 3)));
		src(cv::Range(0, nr), cv::Range(0, nc)).copyTo(dst(cv::Range(1, nr + 1), cv::Range(1, nc + 1)));
	}

	inline double bilinear_interp2d(const cv::Mat& img, double row, double col)
	{
		int rows = img.rows;
		int cols = img.cols;
		int mm = static_cast<int>(std::floor(row));
		int nn = static_cast<int>(std::floor(col));

		if (mm < 0 || nn < 0 || mm > rows - 1 || nn > cols - 1)
		{
			return 0.0;
		}

		int mm1 = mm + 1;
		int nn1 = nn + 1;
		mm1 = mm1 >= rows - 1 ? rows - 1 : mm1;
		nn1 = nn1 >= cols - 1 ? cols - 1 : nn1;

		double v00, v01, v10, v11;
		switch (img.depth())
		{
		case CV_64F:
			v00 = img.at<double>(mm, nn);
			v01 = img.at<double>(mm, nn1);
			v10 = img.at<double>(mm1, nn);
			v11 = img.at<double>(mm1, nn1);
			break;
		case CV_32F:
			v00 = img.at<float>(mm, nn);
			v01 = img.at<float>(mm, nn1);
			v10 = img.at<float>(mm1, nn);
			v11 = img.at<float>(mm1, nn1);
			break;
		case CV_16S:
			v00 = img.at<short>(mm, nn);
			v01 = img.at<short>(mm, nn1);
			v10 = img.at<short>(mm1, nn);
			v11 = img.at<short>(mm1, nn1);
			break;
		default:
			return 0.0;
		}

		double upper = v00 + (v01 - v00) * (col - nn);
		double lower = v10 + (v11 - v10) * (col - nn);
		return upper + (lower - upper) * (row - mm);
	}

	bool ComputeSubBlockCoherence(
		const cv::Mat& M_re, const cv::Mat& M_im,
		const cv::Mat& S_re, const cv::Mat& S_im,
		cv::Mat& out_coherence,
		double& out_mean_coh
	) {
		if (M_re.empty() || M_im.empty() || S_re.empty() || S_im.empty()) return false;

		// 1. Element-wise intermediate products
		cv::Mat num_re, num_im, den_M, den_S;
		cv::multiply(M_re, S_re, num_re);
		cv::multiply(M_im, S_im, num_im);
		cv::Mat M_re_S_im, M_im_S_re;
		cv::multiply(M_re, S_im, M_re_S_im);
		cv::multiply(M_im, S_re, M_im_S_re);

		// num_re = M_re * S_re + M_im * S_im
		num_re = num_re + num_im;
		// num_im = M_im * S_re - M_re * S_im
		num_im = M_im_S_re - M_re_S_im;

		// den_M = M_re^2 + M_im^2
		cv::multiply(M_re, M_re, den_M);
		cv::Mat tmp;
		cv::multiply(M_im, M_im, tmp);
		den_M = den_M + tmp;

		// den_S = S_re^2 + S_im^2
		cv::multiply(S_re, S_re, den_S);
		cv::multiply(S_im, S_im, tmp);
		den_S = den_S + tmp;

		// 2. Box filtering over a 5x5 window (normalize = false means sum)
		cv::Size ksize(5, 5);
		cv::Mat sum_num_re, sum_num_im, sum_den_M, sum_den_S;
		cv::boxFilter(num_re, sum_num_re, CV_32F, ksize, cv::Point(-1, -1), false);
		cv::boxFilter(num_im, sum_num_im, CV_32F, ksize, cv::Point(-1, -1), false);
		cv::boxFilter(den_M, sum_den_M, CV_32F, ksize, cv::Point(-1, -1), false);
		cv::boxFilter(den_S, sum_den_S, CV_32F, ksize, cv::Point(-1, -1), false);

		// 3. Magnitude and Denominator square root
		cv::Mat num_mag;
		cv::magnitude(sum_num_re, sum_num_im, num_mag);

		cv::Mat den_prod;
		cv::multiply(sum_den_M, sum_den_S, den_prod);
		cv::Mat den_sqrt;
		cv::sqrt(den_prod, den_sqrt);

		// 4. Divide (coherence = num_mag / (den_sqrt + eps))
		out_coherence = num_mag / (den_sqrt + 1e-12f);

		// 5. Threshold coherence values to [0.0, 1.0] just in case of numerical noise
		cv::threshold(out_coherence, out_coherence, 1.0, 1.0, cv::THRESH_TRUNC);
		cv::threshold(out_coherence, out_coherence, 0.0, 0.0, cv::THRESH_TOZERO);

		// 6. Compute mean on the valid inner region to avoid border artifacts
		// boxFilter with 5x5 size has 2 pixels border artifacts
		int border = 2;
		if (out_coherence.rows > 2 * border && out_coherence.cols > 2 * border) {
			cv::Rect inner_rect(border, border, out_coherence.cols - 2 * border, out_coherence.rows - 2 * border);
			cv::Mat inner_coh = out_coherence(inner_rect);
			out_mean_coh = cv::mean(inner_coh)[0];
		} else {
			out_mean_coh = cv::mean(out_coherence)[0];
		}

		return true;
	}

	void NormalizeAndClamp2Sigma(
		const cv::Mat& src_amplitude,
		cv::Mat& dst_normalized
	) {
		if (src_amplitude.empty()) return;

		cv::Scalar mean_scalar, stddev_scalar;
		cv::meanStdDev(src_amplitude, mean_scalar, stddev_scalar);
		double mean = mean_scalar[0];
		double stddev = stddev_scalar[0];

		double min_val = mean - 2.0 * stddev;
		double max_val = mean + 2.0 * stddev;

		if (min_val < 0.0) min_val = 0.0;
		if (max_val <= min_val) max_val = min_val + 1.0;

		cv::Mat clamped;
		cv::threshold(src_amplitude, clamped, max_val, max_val, cv::THRESH_TRUNC);
		cv::max(clamped, min_val, clamped);

		// Normalize to [0, 255]
		double scale = 255.0 / (max_val - min_val);
		clamped.convertTo(dst_normalized, CV_8U, scale, -min_val * scale);
	}
}





Registration::Registration()
{
	memset(this->error_head, 0, 256);
	memset(this->parallel_error_head, 0, 256);
	strcpy(this->error_head, "REGISTRATION_DLL_ERROR: error happens when using ");
	strcpy(this->parallel_error_head, "REGISTRATION_DLL_ERROR: error happens when using parallel computing in function: ");
}

Registration::~Registration()
{
}
//int Registration::fft2(Mat& Src, Mat& Dst)
//{
//	if (Src.rows < 1 ||
//		Src.cols < 1 ||
//		Src.channels() != 1||
//		Src.type() != CV_64F)
//	{
//		fprintf(stderr, "fft2(): input check failed!\n\n");
//		return -1;
//	}
//	Mat planes[] = { Mat_<double>(Src), Mat::zeros(Src.size(), CV_64F) };
//	Mat complexImg;
//	merge(planes, 2, complexImg);
//	dft(complexImg, Dst);
//	return 0;
//}

int Registration::fft2(cv::Mat& Src, cv::Mat& Dst)
{
	if (Src.rows < 1 ||
		Src.cols < 1 ||
		Src.channels() != 1)
	{
		fprintf(stderr, "fft2(): input check failed!\n\n");
		return -1;
	}

	const int srcType = Src.type();

	// 支持 short / float / double
	if (srcType != CV_16S &&
		srcType != CV_32F &&
		srcType != CV_64F)
	{
		fprintf(stderr, "fft2(): unsupported input type! Only CV_16S, CV_32F, CV_64F are supported.\n\n");
		return -1;
	}

	cv::Mat srcConverted;
	cv::Mat planes[2];
	cv::Mat complexImg;

	if (srcType == CV_64F)
	{
		// double 输入 -> double 复数输出
		planes[0] = Src;
		planes[1] = cv::Mat::zeros(Src.size(), CV_64F);
		cv::merge(planes, 2, complexImg);   // complexImg: CV_64FC2
	}
	else
	{
		// short / float 输入 -> float 复数输出
		if (srcType == CV_16S)
			Src.convertTo(srcConverted, CV_32F);
		else
			srcConverted = Src;

		planes[0] = srcConverted;
		planes[1] = cv::Mat::zeros(Src.size(), CV_32F);
		cv::merge(planes, 2, complexImg);   // complexImg: CV_32FC2
	}

	cv::dft(complexImg, Dst);
	return 0;
}

int Registration::fftshift2(Mat& matrix)
{
	if (matrix.rows < 2 ||
		matrix.cols < 2 ||
		matrix.channels() != 1)
	{
		fprintf(stderr, "fftshift2(): input check failed!\n\n");
		return -1;
	}
	matrix = matrix(Rect(0, 0, matrix.cols & -2, matrix.rows & -2));
	int cx = matrix.cols / 2;
	int cy = matrix.rows / 2;
	Mat tmp;
	Mat q0(matrix, Rect(0, 0, cx, cy));
	Mat q1(matrix, Rect(cx, 0, cx, cy));
	Mat q2(matrix, Rect(0, cy, cx, cy));
	Mat q3(matrix, Rect(cx, cy, cx, cy));

	q0.copyTo(tmp);
	q3.copyTo(q0);
	tmp.copyTo(q3);

	q1.copyTo(tmp);
	q2.copyTo(q1);
	tmp.copyTo(q2);
	return 0;
}

int Registration::real_coherent(const ComplexMat& Master, const ComplexMat& Slave, int* offset_row, int* offset_col)
{
	//if (Master.GetRows() < 1 ||
	//	Master.GetCols() < 1 ||
	//	Master.GetRows() != Slave.GetRows() ||
	//	Master.GetCols() != Slave.GetCols())
	//{
	//	fprintf(stderr, "real_coherent(): input check failed!\n\n");
	//	return -1;
	//}
	//int ret;
	//Mat img1;
	//Mat img2;
	//img1 = Master.GetMod();
	//img2 = Slave.GetMod();
	//Mat im1fft;
	//Mat im2fft;
	//ret = fft2(img1, im1fft);
	//if (return_check(ret, "fft2(*, *)", error_head)) return -1;
	//ret = fft2(img2, im2fft);
	//if (return_check(ret, "fft2(*, *)", error_head)) return -1;
	//Mat spectrum;
	//mulSpectrums(im1fft, im2fft, spectrum, 0, true);

	//Mat result;
	//idft(spectrum, result, DFT_REAL_OUTPUT);//需要显示图像时可以用DFT_SCALE

	//ret = fftshift2(result);
	//if (return_check(ret, "fftshift2(*)", error_head)) return -1;
	//normalize(result, result, 0, 1, NORM_MINMAX);

	//int r = result.rows / 2;
	//int c = result.cols / 2;
	//Point peak_loc;
	//minMaxLoc(result, NULL, NULL, NULL, &peak_loc);

	//*offset_row = r - peak_loc.y;
	//*offset_col = c - peak_loc.x;
	//return 0;


	if (Master.GetRows() < 1 ||
		Master.GetCols() < 1 ||
		Master.GetRows() != Slave.GetRows() ||
		Master.GetCols() != Slave.GetCols() ||
		offset_row == nullptr ||
		offset_col == nullptr)
	{
		fprintf(stderr, "real_coherent(): input check failed!\n\n");
		return -1;
	}

	int ret;

	// 1) 幅度图
	Mat img1 = Master.GetMod();
	Mat img2 = Slave.GetMod();

	// 2) FFT
	Mat fft1, fft2_mat;
	ret = fft2(img1, fft1);
	if (return_check(ret, "fft2(*, *)", error_head)) return -1;
	img1.release();

	ret = fft2(img2, fft2_mat);
	if (return_check(ret, "fft2(*, *)", error_head)) return -1;
	img2.release();

	// 3) 直接复用 fft1 作为互功率谱结果，少开一个 spectrum
	mulSpectrums(fft1, fft2_mat, fft1, 0, true);
	fft2_mat.release();

	// 4) IFFT 得到相关图
	Mat corr;
	idft(fft1, corr, DFT_REAL_OUTPUT);
	fft1.release();

	// 不再做 normalize，也不做 fftshift2
	// 直接在未 shift 的相关图中找峰值
	Point peak_loc;
	minMaxLoc(corr, nullptr, nullptr, nullptr, &peak_loc);

	const int rows = corr.rows;
	const int cols = corr.cols;
	const int half_r = rows / 2;
	const int half_c = cols / 2;

	// 将未 shift 的峰值位置转换为有符号偏移
	// 对应原来 fftshift 后 center - peak 的效果
	*offset_row = (peak_loc.y <= half_r) ? (-peak_loc.y) : (rows - peak_loc.y);
	*offset_col = (peak_loc.x <= half_c) ? (-peak_loc.x) : (cols - peak_loc.x);

	return 0;
}

int Registration::registration_pixel(ComplexMat& Master, ComplexMat& Slave, int* move_r, int* move_c)
{
	if (Master.GetRows() < 1 ||
		Master.GetCols() < 1 ||
		Master.GetRows() != Slave.GetRows() ||
		Master.GetCols() != Slave.GetCols())
	{
		fprintf(stderr, "registration_pixel(): input check failed!\n\n");
		return -1;
	}
	int offset_rows, offset_cols, ret;
	offset_cols = 0;
	offset_rows = 0;
	ret = real_coherent(Master, Slave, &offset_rows, &offset_cols);//相关函数求取偏移量
	if (move_r)*move_r = offset_rows;
	if (move_c)*move_c = offset_cols;
	if (return_check(ret, "real_coherent(*, *, *, *)", error_head)) return -1;
	////搬移与裁剪

	//行偏移（竖直移动）
	ComplexMat image_master_mid;
	ComplexMat image_slave_mid;
	int nr = Slave.GetRows();
	int nc = Slave.GetCols();
	//////////////////////////////检查粗配准偏移量是否超过图像大小////////////////////////////////////
	if ((offset_rows > 0 ? offset_rows : -offset_rows) >= nr ||
		(offset_cols > 0 ? offset_cols : -offset_cols) >= nc)
	{
		fprintf(stderr, "registration_pixel(): registration offset exceed size of images!\n\n");
		return -1;
	}
	if (offset_rows >= 0)
	{
		//实部
		Slave.re(Range(offset_rows, nr), Range(0, nc)).copyTo(image_slave_mid.re);//辅图像向上搬移

		Master.re(Range(0, nr - offset_rows), Range(0, nc)).copyTo(image_master_mid.re);//裁剪主图像


		//虚部
		Slave.im(Range(offset_rows, nr), Range(0, nc)).copyTo(image_slave_mid.im);//辅图像向上搬移

		Master.im(Range(0, nr - offset_rows), Range(0, nc)).copyTo(image_master_mid.im);//裁剪

	}

	else
	{
		//实部
		Slave.re(Range(0, nr + offset_rows), Range(0, nc)).copyTo(image_slave_mid.re);//辅图像向下搬移

		Master.re(Range(-offset_rows, nr), Range(0, nc)).copyTo(image_master_mid.re);//裁剪


		//虚部
		Slave.im(Range(0, nr + offset_rows), Range(0, nc)).copyTo(image_slave_mid.im);//辅图像向下搬移

		Master.im(Range(-offset_rows, nr), Range(0, nc)).copyTo(image_master_mid.im);//裁剪

	}

	ComplexMat image_master_regis;
	ComplexMat image_slave_regis;
	int nr1 = image_master_mid.re.rows;
	int nc1 = image_master_mid.re.cols;
	//列偏移（水平移动）
	if (offset_cols >= 0)//辅图像向左搬移
	{
		//实部
		image_slave_mid.re(Range(0, nr1), Range(offset_cols, nc1)).copyTo(image_slave_regis.re);

		image_master_mid.re(Range(0, nr1), Range(0, nc1 - offset_cols)).copyTo(image_master_regis.re);

		//虚部
		image_slave_mid.im(Range(0, nr1), Range(offset_cols, nc1)).copyTo(image_slave_regis.im);

		image_master_mid.im(Range(0, nr1), Range(0, nc1 - offset_cols)).copyTo(image_master_regis.im);

	}

	else//辅图像向右搬移
	{
		//实部
		image_slave_mid.re(Range(0, nr1), Range(0, nc1 + offset_cols)).copyTo(image_slave_regis.re);

		image_master_mid.re(Range(0, nr1), Range(-offset_cols, nc1)).copyTo(image_master_regis.re);

		//虚部
		image_slave_mid.im(Range(0, nr1), Range(0, nc1 + offset_cols)).copyTo(image_slave_regis.im);

		image_master_mid.im(Range(0, nr1), Range(-offset_cols, nc1)).copyTo(image_master_regis.im);
	}

	Master.re = image_master_regis.re;

	Master.im = image_master_regis.im;

	Slave.re = image_slave_regis.re;

	Slave.im = image_slave_regis.im;
	return 0;
}

int Registration::interp_paddingzero(ComplexMat& InputMatrix, ComplexMat& OutputMatrix, int interp_times)
{
	if (InputMatrix.GetRows() < 2 ||
		InputMatrix.GetCols() < 2 ||
		interp_times < 2)
	{
		fprintf(stderr,"interp_paddingzero(): input check failed!\n\n");
		return -1;
	}

	int nr = InputMatrix.GetRows();
	int nc = InputMatrix.GetCols();

	OutputMatrix.re = Mat::zeros(interp_times * nr, interp_times * nc, CV_64F);

	OutputMatrix.im = Mat::zeros(interp_times * nr, interp_times * nc, CV_64F);
	Mat re, im;
	InputMatrix.re.copyTo(re);
	InputMatrix.im.copyTo(im);
	Mat planes[] = { Mat_<double>(re), Mat_<double>(im) };

	Mat complexImg;

	merge(planes, 2, complexImg);

	dft(complexImg, complexImg, DFT_COMPLEX_OUTPUT);

	split(complexImg, planes);

	planes[0](Range(0, nr / 2), Range(0, nc / 2)).copyTo(OutputMatrix.re(Range(0, nr / 2), Range(0, nc / 2)));
	planes[1](Range(0, nr / 2), Range(0, nc / 2)).copyTo(OutputMatrix.im(Range(0, nr / 2), Range(0, nc / 2)));

	planes[0](Range(nr / 2, nr), Range(0, nc / 2)).copyTo(OutputMatrix.re(Range(nr * interp_times - nr / 2, nr * interp_times), Range(0, nc / 2)));
	planes[1](Range(nr / 2, nr), Range(0, nc / 2)).copyTo(OutputMatrix.im(Range(nr * interp_times - nr / 2, nr * interp_times), Range(0, nc / 2)));

	planes[0](Range(0, nr / 2), Range(nc / 2, nc)).copyTo(OutputMatrix.re(Range(0, nr / 2), Range(nc * interp_times - nc / 2, nc * interp_times)));
	planes[1](Range(0, nr / 2), Range(nc / 2, nc)).copyTo(OutputMatrix.im(Range(0, nr / 2), Range(nc * interp_times - nc / 2, nc * interp_times)));

	planes[0](Range(nr / 2, nr), Range(nc / 2, nc)).copyTo(OutputMatrix.re(Range(nr * interp_times - nr / 2, nr * interp_times), Range(nc * interp_times - nc / 2, nc * interp_times)));
	planes[1](Range(nr / 2, nr), Range(nc / 2, nc)).copyTo(OutputMatrix.im(Range(nr * interp_times - nr / 2, nr * interp_times), Range(nc * interp_times - nc / 2, nc * interp_times)));

	Mat planes1[] = { Mat_<double>(OutputMatrix.re), Mat_<double>(OutputMatrix.im) };

	merge(planes1, 2, complexImg);

	idft(complexImg, complexImg);

	split(complexImg, planes1);

	OutputMatrix.re = planes1[0];
	OutputMatrix.im = planes1[1];
	return 0;
}

int Registration::interp_cubic(ComplexMat& InputMatrix, ComplexMat& OutputMatrix, double offset_row, double offset_col)
{
	int nr = InputMatrix.GetRows();
	int nc = InputMatrix.GetCols();//输入矩阵尺寸
	if (nr < 2 || nc < 2 || InputMatrix.re.type() != CV_64F)
	{
		fprintf(stderr, "interp_cubic(): input check failed!\n\n");
		return -1;
	}
	ComplexMat new_image_slave;
	new_image_slave.re = Mat::zeros(nr + 3, nc + 3, CV_64F);
	new_image_slave.im = Mat::zeros(nr + 3, nc + 3, CV_64F);

	//扩充矩阵(扩展三行三列)
	padBorder(InputMatrix.re, new_image_slave.re);
	padBorder(InputMatrix.im, new_image_slave.im);


	//行权
	double row_weight[4];
	row_weight[0] = WeightCalculation(1.0 + offset_row);
	row_weight[1] = WeightCalculation(offset_row);
	row_weight[2] = WeightCalculation(1.0 - offset_row);
	row_weight[3] = WeightCalculation(2.0 - offset_row);

	Mat Row_weight(4, 1, CV_64F, row_weight);

	//列权
	double col_weight[4];
	col_weight[0] = WeightCalculation(1.0 + offset_col);
	col_weight[1] = WeightCalculation(offset_col);
	col_weight[2] = WeightCalculation(1.0 - offset_col);
	col_weight[3] = WeightCalculation(2.0 - offset_col);

	Mat Col_weight(1, 4, CV_64F, col_weight);

	
	//权矩阵
	Mat Weight = Row_weight * Col_weight;

	//实部虚部分别插值
	Mat image_slave_regis_re = Mat::zeros(nr, nc, CV_64F);
	Mat image_slave_regis_im = Mat::zeros(nr, nc, CV_64F);

	double temp;
#pragma omp parallel for schedule(guided) \
	private(temp)
	for (int i = 0; i <= nr - 1; i++)
	{
		for (int j = 0; j <= nc - 1; j++)
		{
			temp = new_image_slave.re(Range(i, i + 4), Range(j, j + 4)).dot(Weight);

			image_slave_regis_re.at<double>(i, j) = temp;


			temp = new_image_slave.im(Range(i, i + 4), Range(j, j + 4)).dot(Weight);

			image_slave_regis_im.at<double>(i, j) = temp;
		}

	}
	image_slave_regis_re.copyTo(OutputMatrix.re);
	image_slave_regis_im.copyTo(OutputMatrix.im);
	return 0;
}

int Registration::interp_cubic(ComplexMat& InputMatrix, ComplexMat& OutputMatrix, Mat& Coefficient)
{
	int nr = InputMatrix.GetRows();
	int nc = InputMatrix.GetCols();//输入矩阵尺寸
	if (nr < 2 || nc < 2 || InputMatrix.re.type() != CV_64F)
	{
		fprintf(stderr, "interp_cubic(): input check failed!\n\n");
		return -1;
	}

	ComplexMat new_image_slave;
	new_image_slave.re = Mat::zeros(nr + 3, nc + 3, CV_64F);
	new_image_slave.im = Mat::zeros(nr + 3, nc + 3, CV_64F);

	//扩充矩阵(扩展三行三列)
	padBorder(InputMatrix.re, new_image_slave.re);
	padBorder(InputMatrix.im, new_image_slave.im);




	
	Mat image_slave_regis_re = Mat::zeros(nr, nc, CV_64F);
	Mat image_slave_regis_im = Mat::zeros(nr, nc, CV_64F);

	
	int ret;
	std::atomic<bool> parallel_flag(true);
#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i <= nr - 1; i++)
	{
		if (!parallel_flag) continue;
		
		for (int j = 0; j <= nc - 1; j++)
		{
			if (!parallel_flag) continue;
			double temp, offset_row, offset_col;
			Mat Row_weight = Mat::zeros(4, 1, CV_64F);
			Mat Col_weight = Mat::zeros(1, 4, CV_64F);
			offset_row = 0;
			offset_col = 0;
			ret = every_subpixel_move(i + 1, j + 1, Coefficient, &offset_row, &offset_col);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			Row_weight.at<double>(0, 0) = WeightCalculation(offset_row + 1.0);
			Row_weight.at<double>(1, 0) = WeightCalculation(offset_row);
			Row_weight.at<double>(2, 0) = WeightCalculation(1.0 - offset_row);
			Row_weight.at<double>(3, 0) = WeightCalculation(2.0 - offset_row);

			Col_weight.at<double>(0, 0) = WeightCalculation(1.0 + offset_col);
			Col_weight.at<double>(0, 1) = WeightCalculation(offset_col);
			Col_weight.at<double>(0, 2) = WeightCalculation(1.0 - offset_col);
			Col_weight.at<double>(0, 3) = WeightCalculation(2.0 - offset_col);
			temp = new_image_slave.re(Range(i, i + 4), Range(j, j + 4)).dot(Row_weight * Col_weight);

			image_slave_regis_re.at<double>(i, j) = temp;


			temp = new_image_slave.im(Range(i, i + 4), Range(j, j + 4)).dot(Row_weight * Col_weight);

			image_slave_regis_im.at<double>(i, j) = temp;
		}

	}
	if(parallel_check(parallel_flag, "interp_cubic()", parallel_error_head)) return -1;
	image_slave_regis_re.copyTo(OutputMatrix.re);
	image_slave_regis_im.copyTo(OutputMatrix.im);
	return 0;
}

double Registration::WeightCalculation(double offset)
{
	double weight = 0;
	offset = fabs(offset);

	if (offset < 1.0)
	{
		weight = 1.0 - 2.0 * offset * offset + offset * offset * offset;
	}
	else if (offset >= 1.0 && offset <= 2.0)
	{
		weight = 4.0 - 8.0 * offset + 5.0 * offset * offset - offset * offset * offset;
	}
	else
	{
		weight = 0.0;
	}

	return weight;
}

int Registration::registration_subpixel(ComplexMat& Master, ComplexMat& Slave, int blocksize, int interp_times)
{
	constexpr double COHERENCE_THRESH = 0.4;

	if (Master.GetRows() < 1 ||
		Master.GetCols() < 1 ||
		Master.GetRows() != Slave.GetRows() ||
		Master.GetCols() != Slave.GetCols() ||
		blocksize < 1 ||
		interp_times < 1)
	{
		fprintf(stderr, "%s\n\n", "registration_subpixel(): input check failed!\n\n");
		return -1;
	}

	int nsubr = Master.GetRows() / blocksize; //子块行数
	int nsubc = Master.GetCols() / blocksize; //子块列数
	int nsub = nsubr * nsubc;  //子块总数
	if (nsubc < 1 || nsubr < 1)
	{
		fprintf(stderr, "%s\n\n", "registration_subpixel: subblockszie, nsubc < 1 || nsubr < 1");
		return -1;
	}
	Mat sub_r_offset = Mat::zeros(nsub, 1, CV_64F); //行亚像素偏移量
	Mat sub_c_offset = Mat::zeros(nsub, 1, CV_64F); //列亚像素偏移量

	Mat m = Mat::zeros(nsub, 1, CV_32S); //子块中心行坐标
	Mat n = Mat::zeros(nsub, 1, CV_32S); //子块中心列坐标
	Mat indx = Mat::zeros(nsub, 1, CV_32S); //索引


	int count = 0;
	int ret;
	Mat coherence;
	Utils util;
	std::atomic<bool> parallel_flag(true);
#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = 0; i < nsubc; i++)
	{
		if (!parallel_flag) continue;
		
		for (int j = 0; j < nsubr; j++)
		{
			if (!parallel_flag) continue;
			ComplexMat temp_in_slave, temp_out_slave, temp_in_master, temp_out_master;
			int offset_row, offset_col;
			//辅图像子块插值
			Slave.re(Range(j * blocksize, (j + 1) * blocksize), Range(i * blocksize, (i + 1) * blocksize)).copyTo(temp_in_slave.re);
			Slave.im(Range(j * blocksize, (j + 1) * blocksize), Range(i * blocksize, (i + 1) * blocksize)).copyTo(temp_in_slave.im);
			//util.cvmat2bin("E:\\zgb1\\InSAR\\InSAR\\bin\\re.bin", temp_in_slave.re);
			ret = interp_paddingzero(temp_in_slave, temp_out_slave, interp_times);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}

			//主图像子块插值
			Master.re(Range(j * blocksize, (j + 1) * blocksize), Range(i * blocksize, (i + 1) * blocksize)).copyTo(temp_in_master.re);
			Master.im(Range(j * blocksize, (j + 1) * blocksize), Range(i * blocksize, (i + 1) * blocksize)).copyTo(temp_in_master.im);
			ret = interp_paddingzero(temp_in_master, temp_out_master, interp_times);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}

			//实相关函数求取亚像素偏移量
			ret = real_coherent(temp_out_master, temp_out_slave, &offset_row, &offset_col);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			// removed unused: mean_coh (commented-out coherence code below)
			double offset_row_sub, offset_col_sub;
			//ret = util.real_coherence(temp_out_master, temp_out_slave, coherence);
			//if (ret < 0)
			//{
			//	parallel_flag = false;
			//	continue;
			//}
			//mean_coh = mean(coherence)[0];
			offset_row_sub = double(offset_row) / double(interp_times);
			offset_col_sub = double(offset_col) / double(interp_times);
			sub_r_offset.at<double>(i * nsubr + j, 0) = offset_row_sub;
			sub_c_offset.at<double>(i * nsubr + j, 0) = offset_col_sub;

			//子块中心坐标
			n.at<int>(i * nsubr + j, 0) = blocksize / 2 + i * blocksize;
			m.at<int>(i * nsubr + j, 0) = blocksize / 2 + j * blocksize;

			ret = interp_cubic(temp_in_slave, temp_in_slave, sub_r_offset.at<double>(i * nsubr + j, 0), sub_c_offset.at<double>(i * nsubr + j, 0));//子辅图像插值
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			ret = util.real_coherence(temp_in_master, temp_in_slave, coherence);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			if (mean(coherence)[0] > COHERENCE_THRESH)
			{
				indx.at<int>(i * nsubr + j, 0) = 1;
			}

		}
	}
	if (parallel_check(parallel_flag, "registration_subpixel()", parallel_error_head)) return -1;
	int NoneZero = countNonZero(indx);//非零元素个数
	count = 0;
	if (NoneZero == 0)
	{
		fprintf(stderr, "registration_subpixel(): NoneZero == 0\n\n");
		return -1;
	}
	Mat sub_r_offset_sifted = Mat::zeros(NoneZero, 1, CV_64F); //筛选后行亚像素偏移量
	Mat sub_c_offset_sifted = Mat::zeros(NoneZero, 1, CV_64F); //筛选后列亚像素偏移量

	Mat m_sifted = Mat::zeros(NoneZero, 1, CV_32S); //筛选后子块中心行坐标
	Mat n_sifted = Mat::zeros(NoneZero, 1, CV_32S); //筛选后子块中心列坐标


	for (int k = 0; k < nsub; k++)
	{
		if (indx.at<int>(k, 0) > 0)
		{

			sub_r_offset_sifted.at<double>(count, 0) = sub_r_offset.at<double>(k, 0);
			sub_c_offset_sifted.at<double>(count, 0) = sub_c_offset.at<double>(k, 0);
			m_sifted.at<int>(count, 0) = m.at<int>(k, 0);
			n_sifted.at<int>(count, 0) = n.at<int>(k, 0);

			count++;
		}
	}


	Mat para;
	ret = all_subpixel_move(m_sifted, n_sifted, sub_r_offset_sifted, sub_c_offset_sifted, para);//拟合辅图像偏移量
	//测试
	//cout << para << "\n";
	//
	if (return_check(ret, "all_subpixel_move(*, *, *, *)", error_head)) return -1;
	ret = interp_cubic(Slave, Slave, para);
	if (return_check(ret, "interp_cubic(*, *, *)", error_head)) return -1;
	return 0;
}

int Registration::coregistration_subpixel(ComplexMat& master, ComplexMat& slave, int blocksize, int interp_times, int* offset_row,
	int* offset_col, RegistrationProgressCallback cb, void* userData)
{
	constexpr int MAX_CROP_SIZE = 10000;
	constexpr double COHERENCE_THRESH = 0.05;
	constexpr int COH_WIN_SIZE = 7;
	constexpr double ZERO_TOLERANCE = 1e-7;

	if (master.isEmpty() ||
		slave.isEmpty() ||
		//blocksize * 5 > (slave.GetCols() < slave.GetRows() ? slave.GetCols() : slave.GetRows()) ||
		blocksize < 8||interp_times < 1 ||
		master.type() != slave.type() ||
		(master.type() != CV_64F && master.type() != CV_32F && master.type() != CV_16S)
		)
	{
		fprintf(stderr, "coregistration_subpixel(): input check failed!\n");
		return -1;
	}

	//粗配准
	ComplexMat slave_r, master_small, slave_small;
	slave_r = master;
	slave_r.re = 0.0; slave_r.im = 0.0;
	int nr0 = master.GetRows() > MAX_CROP_SIZE ? MAX_CROP_SIZE : master.GetRows();
	int nc0 = master.GetCols() > MAX_CROP_SIZE ? MAX_CROP_SIZE : master.GetCols();
	nr0 = slave.GetRows() > nr0 ? nr0 : slave.GetRows();
	nc0 = slave.GetCols() > nc0 ? nc0 : slave.GetCols();

	master_small = master(cv::Range(0, nr0), cv::Range(0, nc0));
	slave_small = slave(cv::Range(0, nr0), cv::Range(0, nc0));
	master_small.convertTo(master_small, CV_64F);
	slave_small.convertTo(slave_small, CV_64F);
	int offset_rows_pre, offset_cols_pre;
	real_coherent(master_small, slave_small, &offset_rows_pre, &offset_cols_pre);

	int start_r, start_r2, end_r, end_r2, start_c, end_c, start_c2, end_c2;
	if (offset_rows_pre > 0)
	{
		start_r = offset_rows_pre;
		start_r2 = 0;
		end_r = (slave.GetRows() - offset_rows_pre) > master.GetRows() ? (offset_rows_pre + master.GetRows()) : slave.GetRows();
		end_r2 = start_r2 + (end_r - start_r);
	}
	else
	{
		//start_r = (slave.GetRows() + offset_rows_pre) > master.GetRows() ? (slave.GetRows() + offset_rows_pre - master.GetRows()) : 0;
		//start_r2 = (slave.GetRows() + offset_rows_pre) > master.GetRows() ? 0 : (master.GetRows() - slave.GetRows() - offset_rows_pre);
		//end_r = slave.GetRows() + offset_rows_pre;
		//end_r2 = master.GetRows();

		start_r = 0;
		start_r2 = -offset_rows_pre;
		end_r = (slave.GetRows() - offset_rows_pre) > master.GetRows() ? (master.GetRows() + offset_rows_pre) : slave.GetRows();
		end_r2 = (slave.GetRows() - offset_rows_pre) > master.GetRows() ? master.GetRows() : slave.GetRows() - offset_rows_pre;
	}
	if (offset_cols_pre > 0)
	{
		start_c = offset_cols_pre;
		start_c2 = 0;
		end_c = (slave.GetCols() - offset_cols_pre) > master.GetCols() ? (offset_cols_pre + master.GetCols()) : slave.GetCols();
		end_c2 = start_c2 + (end_c - start_c);
	}
	else
	{
		//start_c = (slave.GetCols() + offset_cols_pre) > master.GetCols() ? (slave.GetCols() + offset_cols_pre - master.GetCols()) : 0;
		//start_c2 = (slave.GetCols() + offset_cols_pre) > master.GetCols() ? 0 : (master.GetCols() - slave.GetCols() - offset_cols_pre);
		//end_c = slave.GetCols() + offset_cols_pre;
		//end_c2 = master.GetCols();

		start_c = 0;
		start_c2 = -offset_cols_pre;
		end_c = (slave.GetCols() - offset_cols_pre) > master.GetCols() ? (master.GetCols() + offset_cols_pre) : slave.GetCols();
		end_c2 = (slave.GetCols() - offset_cols_pre) > master.GetCols() ? master.GetCols() : slave.GetCols() - offset_cols_pre;
	}
	slave.re(cv::Range(start_r, end_r), cv::Range(start_c, end_c)).copyTo(slave_r.re(cv::Range(start_r2, end_r2), cv::Range(start_c2, end_c2)));
	slave.im(cv::Range(start_r, end_r), cv::Range(start_c, end_c)).copyTo(slave_r.im(cv::Range(start_r2, end_r2), cv::Range(start_c2, end_c2)));
	slave = slave_r;

	/*---------------------------------------*/
	/*              求取偏移量矩阵           */
	/*---------------------------------------*/
	interp_times = interp_times > 32 ? 32 : interp_times;//限定最多32倍插值
	Utils util;
	int m = (master.GetRows()) / blocksize;
	int n = (master.GetCols()) / blocksize;
	if (m * n < 10)
	{
		fprintf(stderr, "coregistration_subpixel(): try smaller blocksize!\n");
		return -1;
	}
	Mat offset_r = Mat::zeros(m, n, CV_64F); Mat offset_c = Mat::zeros(m, n, CV_64F);
	Mat offset_coord_row = Mat::zeros(m, n, CV_64F); 
	Mat offset_coord_col = Mat::zeros(m, n, CV_64F);
	Mat sentinel0 = Mat::zeros(m, n, CV_64F);
	//子块中心坐标
	for (int i = 0; i < m; i++)
	{
		for (int j = 0; j < n; j++)
		{
			offset_coord_row.at<double>(i, j) = ((double)blocksize) / 2 * (double)(2 * i + 1);
			offset_coord_col.at<double>(i, j) = ((double)blocksize) / 2 * (double)(2 * j + 1);
		}
	}
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_blocks(0);
	int block_step = std::max(1, m / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < m; i++)
	{
		if (cancel_flag)
		{
			continue;
		}
		ComplexMat master_sub, slave_sub, master_sub_interp, slave_sub_interp, master1, slave1;
		Mat amplitude_slave, sign, coh1;
		int offset_row, offset_col;
		double mean_coh;
		for (int j = 0; j < n; j++)
		{
			if (cancel_flag)
			{
				break;
			}
			//计算相关系数判断是否是有效数据
			master.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master1.re);
			master.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master1.im);
			if (master1.type() != CV_64F) master1.convertTo(master1, CV_64F);
			slave.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave1.re);
			slave.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave1.im);
			if (slave1.type() != CV_64F) slave1.convertTo(slave1, CV_64F);
			registration_pixel(master1, slave1);
			util.complex_coherence(master1, slave1, COH_WIN_SIZE, COH_WIN_SIZE, coh1);
			mean_coh = cv::mean(coh1)[0];
			if (mean_coh < COHERENCE_THRESH)
			{
				sentinel0.at<double>(i, j) = 1.0;
				continue;
			}
			//主图像子块插值
			master.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master_sub.re);
			master.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master_sub.im);
			if (master_sub.type() != CV_64F) master_sub.convertTo(master_sub, CV_64F);
			interp_paddingzero(master_sub, master_sub_interp, interp_times);
			//辅图像子块插值
			slave.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave_sub.re);
			slave.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave_sub.im);
			if (slave_sub.type() != CV_64F) slave_sub.convertTo(slave_sub, CV_64F);
			amplitude_slave = slave_sub.re;
			int count_zero = 0;
			for (int ii = 0; ii < amplitude_slave.rows; ii++)
			{
				for (int jj = 0; jj < amplitude_slave.cols; jj++)
				{
					if (fabs(amplitude_slave.at<double>(ii, jj)) < ZERO_TOLERANCE) count_zero++;
				}
			}
			//sign = amplitude_slave < ZERO_TOLERANCE;
			int thresh = blocksize * blocksize / 4;
			if (count_zero > thresh)
			{
				sentinel0.at<double>(i, j) = 1.0;
				continue;
			}
			interp_paddingzero(slave_sub, slave_sub_interp, interp_times);
			//求取偏移量
			real_coherent(master_sub_interp, slave_sub_interp, &offset_row, &offset_col);
			offset_r.at<double>(i, j) = (double)offset_row / (double)interp_times;
			offset_c.at<double>(i, j) = (double)offset_col / (double)interp_times;
			
		}
		int current_completed = ++completed_blocks;
		if (cb && current_completed % block_step == 0)
		{
			int progress = current_completed * 50 / m;
			if (!cb(progress, "Subpixel searching...", userData))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;

	/*---------------------------------------*/
	/*    拟合偏移量（将坐标做归一化处理）   */
	/*---------------------------------------*/

	/*
	* 拟合公式为 offser_row/offser_col = a0 + a1*x + a2*y
	*/
	
	////剔除outliers
	Mat sentinel = Mat::zeros(m, n, CV_64F);
	// removed unused: ix, iy, delta, thresh (commented-out outlier removal below)
	int count = 0, c = 0;
	//for (int i = 0; i < m; i++)
	//{
	//	for (int j = 0; j < n; j++)
	//	{
	//		count = 0;
	//		//上
	//		ix = j; 
	//		iy = i - 1; iy = iy < 0 ? 0 : iy;
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;
	//		//下
	//		ix = j;
	//		iy = i + 1; iy = iy > m - 1 ? m - 1 : iy;
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;
	//		//左
	//		ix = j - 1; ix = ix < 0 ? 0 : ix;
	//		iy = i; 
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;
	//		//右
	//		ix = j + 1; ix = ix > n - 1 ? n - 1 : ix;
	//		iy = i;
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;

	//		if (count > 2) { sentinel.at<double>(i, j) = 1.0; }
	//	}
	//}
	for (int i = 0; i < m; i++)
	{
		for (int j = 0; j < n; j++)
		{
			if (sentinel.at<double>(i, j) > 0.5 || sentinel0.at<double>(i, j) > 0.5) c++;
		}
	}
	Mat offset_c_0, offset_r_0, offset_coord_row_0 , offset_coord_col_0;
	offset_c_0 = Mat::zeros(m * n - c, 1, CV_64F);
	offset_r_0 = Mat::zeros(m * n - c, 1, CV_64F);
	offset_coord_row_0 = Mat::zeros(m * n - c, 1, CV_64F);
	offset_coord_col_0 = Mat::zeros(m * n - c, 1, CV_64F);
	count = 0;
	for (int i = 0; i < m; i++)
	{
		for (int j = 0; j < n; j++)
		{
			if (sentinel.at<double>(i, j) < 0.5 && sentinel0.at<double>(i, j) < 0.5)
			{
				offset_r_0.at<double>(count, 0) = offset_r.at<double>(i, j);
				offset_c_0.at<double>(count, 0) = offset_c.at<double>(i, j);
				offset_coord_row_0.at<double>(count, 0) = offset_coord_row.at<double>(i, j);
				offset_coord_col_0.at<double>(count, 0) = offset_coord_col.at<double>(i, j);
				count++;
			}
		}
	}


	offset_c = offset_c_0;
	offset_r = offset_r_0;
	offset_coord_row = offset_coord_row_0;
	offset_coord_col = offset_coord_col_0;
	m = 1; n = count;
	if (count < 11)
	{
		fprintf(stderr, "coregistration_subpixel(): insufficient valide sub blocks!\n");
		return -1;
	}
	double offset_x = (double)master.GetCols() / 2;
	double offset_y = (double)master.GetRows() / 2;
	double scale_x = (double)master.GetCols();
	double scale_y = (double)master.GetRows();
	offset_coord_row -= offset_y;
	offset_coord_col -= offset_x;
	offset_coord_row /= scale_y;
	offset_coord_col /= scale_x;
	Mat A = Mat::ones(m * n, 3, CV_64F);
	Mat temp, A_t;
	offset_coord_col.copyTo(A(Range(0, m * n), Range(1, 2)));

	offset_coord_row.copyTo(A(Range(0, m * n), Range(2, 3)));



	cv::transpose(A, A_t);

	Mat b_r, b_c, coef_r, coef_c, error_r, error_c, b_t, a, a_t;
	
	A.copyTo(a);
	cv::transpose(a, a_t);
	offset_r.copyTo(b_r);
	b_r = A_t * b_r;

	offset_c.copyTo(b_c);
	b_c = A_t * b_c;

	A = A_t * A;

	double rms1 = -1.0; double rms2 = -1.0;
	Mat eye = Mat::zeros(m * n, m * n, CV_64F);
	for (int i = 0; i < m * n; i++)
	{
		eye.at<double>(i, i) = 1.0;
	}
	if (cv::invert(A, error_r, cv::DECOMP_LU) > 0)
	{
		cv::transpose(offset_r, b_t);
		error_r = b_t * (eye - a * error_r * a_t) * offset_r;
		rms1 = sqrt(error_r.at<double>(0, 0) / double(m * n));
	}
	if (cv::invert(A, error_c, cv::DECOMP_LU) > 0)
	{
		cv::transpose(offset_c, b_t);
		error_c = b_t * (eye - a * error_c * a_t) * offset_c;
		rms2 = sqrt(error_c.at<double>(0, 0) / double(m * n));
	}
	if (!cv::solve(A, b_r, coef_r, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "coregistration_subpixel(): matrix deficiency!\n");
		return -1;
	}
	if (!cv::solve(A, b_c, coef_c, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "coregistration_subpixel(): matrix deficiency!\n");
		return -1;
	}

	/*---------------------------------------*/
	/*    双线性插值获取重采样后的辅图像     */
	/*---------------------------------------*/

	Mat tt(1, 3, CV_64F);
	tt.at<double>(0, 0) = 1.0;
	tt.at<double>(0, 1) = (0.0 - offset_x) / scale_x;
	tt.at<double>(0, 2) = (0.0 - offset_y) / scale_y;
	if (offset_row) *offset_row = static_cast<int>(sum(tt * coef_r)[0]);
	if (offset_col) *offset_col = static_cast<int>(sum(tt * coef_c)[0]);


	int rows = master.GetRows(); int cols = master.GetCols();
	ComplexMat slave_tmp;
	int master_type = master.type();
	slave_tmp = master;
	int rows_slave = slave.GetRows(); int cols_slave = slave.GetCols();

	const double cr0 = coef_r.at<double>(0, 0);
	const double cr1 = coef_r.at<double>(1, 0);
	const double cr2 = coef_r.at<double>(2, 0);
	const double cc0 = coef_c.at<double>(0, 0);
	const double cc1 = coef_c.at<double>(1, 0);
	const double cc2 = coef_c.at<double>(2, 0);

	std::atomic<int> completed_rows(0);
	int row_step = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		double x, y, ii, jj;
		double offset_rows, offset_cols;
		for (int j = 0; j < cols; j++)
		{
			jj = (double)j;
			ii = (double)i;
			x = (jj - offset_x) / scale_x;
			y = (ii - offset_y) / scale_y;

			offset_rows = cr0 + cr1 * x + cr2 * y;
			offset_cols = cc0 + cc1 * x + cc2 * y;

			ii += offset_rows;
			jj += offset_cols;
			
			double re_val = bilinear_interp2d(slave.re, ii, jj);
			double im_val = bilinear_interp2d(slave.im, ii, jj);

			mat_set_from_double(slave_tmp.re, i, j, re_val);
			mat_set_from_double(slave_tmp.im, i, j, im_val);
		}
		int current_completed = ++completed_rows;
		if (cb && current_completed % row_step == 0)
		{
			int progress = 50 + current_completed * 50 / rows;
			if (!cb(progress, "Bilinear resampling...", userData))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;
	slave = slave_tmp;
	return 0;
}

int Registration::every_subpixel_move(int i, int j, Mat& coefficient, double* offset_row, double* offset_col)
{
	if (i < 1 || j < 1 || coefficient.cols < 1 || coefficient.rows < 12 || coefficient.type() != CV_64F)
	{
		fprintf(stderr, "every_subpixel_move(): input check failed!\n\n");
		return -1;
	}
	double a1, a2, b1, b2, c1, c2, d1, d2, e1, e2, f1, f2;
	a1 = coefficient.at<double>(0, 0);
	b1 = coefficient.at<double>(1, 0);
	c1 = coefficient.at<double>(2, 0);
	d1 = coefficient.at<double>(3, 0);
	e1 = coefficient.at<double>(4, 0);
	f1 = coefficient.at<double>(5, 0);
	a2 = coefficient.at<double>(6, 0);
	b2 = coefficient.at<double>(7, 0);
	c2 = coefficient.at<double>(8, 0);
	d2 = coefficient.at<double>(9, 0);
	e2 = coefficient.at<double>(10, 0);
	f2 = coefficient.at<double>(11, 0);

	*offset_row = a1 + b1 * i + c1 * j + d1 * i * i + e1 * j * j + f1 * i * j;
	*offset_col = a2 + b2 * i + c2 * j + d2 * i * i + e2 * j * j + f2 * i * j;
	return 0;
}

int Registration::coregistration_subpixel_sinc(ComplexMat& master, ComplexMat& slave, int blocksize, int interp_times, int* offset_row,
	int* offset_col, double coh_thresh, RegistrationProgressCallback cb, void* userData)
{
	constexpr int MAX_CROP_SIZE = 10000;
	constexpr int COH_WIN_SIZE = 7;
	constexpr double ZERO_TOLERANCE = 1e-7;

	if (master.isEmpty() ||
		slave.isEmpty() ||
		//blocksize * 5 > (slave.GetCols() < slave.GetRows() ? slave.GetCols() : slave.GetRows()) ||
		blocksize < 8 || interp_times < 1 ||
		master.type() != slave.type() ||
		(master.type() != CV_64F && master.type() != CV_32F && master.type() != CV_16S)
		)
	{
		fprintf(stderr, "coregistration_subpixel_sinc(): input check failed!\n");
		return -1;
	}

	//粗配准
	ComplexMat slave_r, master_small, slave_small;
	slave_r = master;
	slave_r.re = 0.0; slave_r.im = 0.0;
	int nr0 = master.GetRows() > MAX_CROP_SIZE ? MAX_CROP_SIZE : master.GetRows();
	int nc0 = master.GetCols() > MAX_CROP_SIZE ? MAX_CROP_SIZE : master.GetCols();
	nr0 = slave.GetRows() > nr0 ? nr0 : slave.GetRows();
	nc0 = slave.GetCols() > nc0 ? nc0 : slave.GetCols();

	master_small = master(cv::Range(0, nr0), cv::Range(0, nc0));
	slave_small = slave(cv::Range(0, nr0), cv::Range(0, nc0));
	master_small.convertTo(master_small, CV_64F);
	slave_small.convertTo(slave_small, CV_64F);
	int offset_rows_pre, offset_cols_pre;
	real_coherent(master_small, slave_small, &offset_rows_pre, &offset_cols_pre);

	int start_r, start_r2, end_r, end_r2, start_c, end_c, start_c2, end_c2;
	if (offset_rows_pre > 0)
	{
		start_r = offset_rows_pre;
		start_r2 = 0;
		end_r = (slave.GetRows() - offset_rows_pre) > master.GetRows() ? (offset_rows_pre + master.GetRows()) : slave.GetRows();
		end_r2 = start_r2 + (end_r - start_r);
	}
	else
	{
		//start_r = (slave.GetRows() + offset_rows_pre) > master.GetRows() ? (slave.GetRows() + offset_rows_pre - master.GetRows()) : 0;
		//start_r2 = (slave.GetRows() + offset_rows_pre) > master.GetRows() ? 0 : (master.GetRows() - slave.GetRows() - offset_rows_pre);
		//end_r = slave.GetRows() + offset_rows_pre;
		//end_r2 = master.GetRows();

		start_r = 0;
		start_r2 = -offset_rows_pre;
		end_r = (slave.GetRows() - offset_rows_pre) > master.GetRows() ? (master.GetRows() + offset_rows_pre) : slave.GetRows();
		end_r2 = (slave.GetRows() - offset_rows_pre) > master.GetRows() ? master.GetRows() : slave.GetRows() - offset_rows_pre;
	}
	if (offset_cols_pre > 0)
	{
		start_c = offset_cols_pre;
		start_c2 = 0;
		end_c = (slave.GetCols() - offset_cols_pre) > master.GetCols() ? (offset_cols_pre + master.GetCols()) : slave.GetCols();
		end_c2 = start_c2 + (end_c - start_c);
	}
	else
	{
		//start_c = (slave.GetCols() + offset_cols_pre) > master.GetCols() ? (slave.GetCols() + offset_cols_pre - master.GetCols()) : 0;
		//start_c2 = (slave.GetCols() + offset_cols_pre) > master.GetCols() ? 0 : (master.GetCols() - slave.GetCols() - offset_cols_pre);
		//end_c = slave.GetCols() + offset_cols_pre;
		//end_c2 = master.GetCols();

		start_c = 0;
		start_c2 = -offset_cols_pre;
		end_c = (slave.GetCols() - offset_cols_pre) > master.GetCols() ? (master.GetCols() + offset_cols_pre) : slave.GetCols();
		end_c2 = (slave.GetCols() - offset_cols_pre) > master.GetCols() ? master.GetCols() : slave.GetCols() - offset_cols_pre;
	}
	slave.re(cv::Range(start_r, end_r), cv::Range(start_c, end_c)).copyTo(slave_r.re(cv::Range(start_r2, end_r2), cv::Range(start_c2, end_c2)));
	slave.im(cv::Range(start_r, end_r), cv::Range(start_c, end_c)).copyTo(slave_r.im(cv::Range(start_r2, end_r2), cv::Range(start_c2, end_c2)));
	slave = slave_r;

	/*---------------------------------------*/
	/*              求取偏移量矩阵           */
	/*---------------------------------------*/
	interp_times = interp_times > 32 ? 32 : interp_times;//限定最多32倍插值
	Utils util;
	int m = (master.GetRows()) / blocksize;
	int n = (master.GetCols()) / blocksize;
	if (m * n < 10)
	{
		fprintf(stderr, "coregistration_subpixel_sinc(): try smaller blocksize!\n");
		return -1;
	}
	Mat offset_r = Mat::zeros(m, n, CV_64F); Mat offset_c = Mat::zeros(m, n, CV_64F);
	Mat offset_coord_row = Mat::zeros(m, n, CV_64F);
	Mat offset_coord_col = Mat::zeros(m, n, CV_64F);
	Mat sentinel0 = Mat::zeros(m, n, CV_64F);
	//子块中心坐标
	for (int i = 0; i < m; i++)
	{
		for (int j = 0; j < n; j++)
		{
			offset_coord_row.at<double>(i, j) = ((double)blocksize) / 2 * (double)(2 * i + 1);
			offset_coord_col.at<double>(i, j) = ((double)blocksize) / 2 * (double)(2 * j + 1);
		}
	}
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_blocks(0);
	int block_step = std::max(1, m / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < m; i++)
	{
		if (cancel_flag)
		{
			continue;
		}
		ComplexMat master_sub, slave_sub, master_sub_interp, slave_sub_interp, master1, slave1;
		Mat amplitude_slave, sign, coh1;
		int offset_row, offset_col;
		double mean_coh;
		for (int j = 0; j < n; j++)
		{
			if (cancel_flag)
			{
				break;
			}
			//计算相关系数判断是否是有效数据
			master.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master1.re);
			master.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master1.im);
			if (master1.type() != CV_64F) master1.convertTo(master1, CV_64F);
			slave.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave1.re);
			slave.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave1.im);
			if (slave1.type() != CV_64F) slave1.convertTo(slave1, CV_64F);
			registration_pixel(master1, slave1);
			util.complex_coherence(master1, slave1, COH_WIN_SIZE, COH_WIN_SIZE, coh1);
			mean_coh = cv::mean(coh1)[0];
			if (mean_coh < coh_thresh)
			{
				sentinel0.at<double>(i, j) = 1.0;
				continue;
			}
			//主图像子块插值
			master.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master_sub.re);
			master.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(master_sub.im);
			if (master_sub.type() != CV_64F) master_sub.convertTo(master_sub, CV_64F);
			interp_paddingzero(master_sub, master_sub_interp, interp_times);
			//辅图像子块插值
			slave.re(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave_sub.re);
			slave.im(Range(i * blocksize, (i + 1) * blocksize), Range(j * blocksize, (j + 1) * blocksize)).copyTo(slave_sub.im);
			if (slave_sub.type() != CV_64F) slave_sub.convertTo(slave_sub, CV_64F);
			amplitude_slave = slave_sub.re;
			int count_zero = 0;
			for (int ii = 0; ii < amplitude_slave.rows; ii++)
			{
				for (int jj = 0; jj < amplitude_slave.cols; jj++)
				{
					if (fabs(amplitude_slave.at<double>(ii, jj)) < ZERO_TOLERANCE) count_zero++;
				}
			}
			//sign = amplitude_slave < ZERO_TOLERANCE;
			int thresh = blocksize * blocksize / 4;
			if (count_zero > thresh)
			{
				sentinel0.at<double>(i, j) = 1.0;
				continue;
			}
			interp_paddingzero(slave_sub, slave_sub_interp, interp_times);
			//求取偏移量
			real_coherent(master_sub_interp, slave_sub_interp, &offset_row, &offset_col);
			offset_r.at<double>(i, j) = (double)offset_row / (double)interp_times;
			offset_c.at<double>(i, j) = (double)offset_col / (double)interp_times;

		}
		int current_completed = ++completed_blocks;
		if (cb && current_completed % block_step == 0)
		{
			int progress = current_completed * 50 / m;
			if (!cb(progress, "Subpixel searching...", userData))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;

	/*---------------------------------------*/
	/*    拟合偏移量（将坐标做归一化处理）   */
	/*---------------------------------------*/

	/*
	* 拟合公式为 offser_row/offser_col = a0 + a1*x + a2*y
	*/

	////剔除outliers
	Mat sentinel = Mat::zeros(m, n, CV_64F);
	int /*ix, iy, */count = 0, c = 0; double /*delta, */thresh = 2.0;
	//for (int i = 0; i < m; i++)
	//{
	//	for (int j = 0; j < n; j++)
	//	{
	//		count = 0;
	//		//上
	//		ix = j; 
	//		iy = i - 1; iy = iy < 0 ? 0 : iy;
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;
	//		//下
	//		ix = j;
	//		iy = i + 1; iy = iy > m - 1 ? m - 1 : iy;
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;
	//		//左
	//		ix = j - 1; ix = ix < 0 ? 0 : ix;
	//		iy = i; 
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;
	//		//右
	//		ix = j + 1; ix = ix > n - 1 ? n - 1 : ix;
	//		iy = i;
	//		delta = fabs(offset_c.at<double>(i, j) - offset_c.at<double>(iy, ix));
	//		delta += fabs(offset_r.at<double>(i, j) - offset_r.at<double>(iy, ix));
	//		if (fabs(delta) >= thresh) count++;

	//		if (count > 2) { sentinel.at<double>(i, j) = 1.0; }
	//	}
	//}
	for (int i = 0; i < m; i++)
	{
		for (int j = 0; j < n; j++)
		{
			if (sentinel.at<double>(i, j) > 0.5 || sentinel0.at<double>(i, j) > 0.5) c++;
		}
	}
	Mat offset_c_0, offset_r_0, offset_coord_row_0, offset_coord_col_0;
	offset_c_0 = Mat::zeros(m * n - c, 1, CV_64F);
	offset_r_0 = Mat::zeros(m * n - c, 1, CV_64F);
	offset_coord_row_0 = Mat::zeros(m * n - c, 1, CV_64F);
	offset_coord_col_0 = Mat::zeros(m * n - c, 1, CV_64F);
	count = 0;
	for (int i = 0; i < m; i++)
	{
		for (int j = 0; j < n; j++)
		{
			if (sentinel.at<double>(i, j) < 0.5 && sentinel0.at<double>(i, j) < 0.5)
			{
				offset_r_0.at<double>(count, 0) = offset_r.at<double>(i, j);
				offset_c_0.at<double>(count, 0) = offset_c.at<double>(i, j);
				offset_coord_row_0.at<double>(count, 0) = offset_coord_row.at<double>(i, j);
				offset_coord_col_0.at<double>(count, 0) = offset_coord_col.at<double>(i, j);
				count++;
			}
		}
	}


	offset_c = offset_c_0;
	offset_r = offset_r_0;
	offset_coord_row = offset_coord_row_0;
	offset_coord_col = offset_coord_col_0;
	m = 1; n = count;
	if (count < 11)
	{
		fprintf(stderr, "coregistration_subpixel_sinc(): insufficient valide sub blocks!\n");
		return -1;
	}
	double offset_x = (double)master.GetCols() / 2;
	double offset_y = (double)master.GetRows() / 2;
	double scale_x = (double)master.GetCols();
	double scale_y = (double)master.GetRows();
	offset_coord_row -= offset_y;
	offset_coord_col -= offset_x;
	offset_coord_row /= scale_y;
	offset_coord_col /= scale_x;
	Mat A = Mat::ones(m * n, 3, CV_64F);
	Mat temp, A_t;
	offset_coord_col.copyTo(A(Range(0, m * n), Range(1, 2)));

	offset_coord_row.copyTo(A(Range(0, m * n), Range(2, 3)));



	cv::transpose(A, A_t);

	Mat b_r, b_c, coef_r, coef_c, error_r, error_c, b_t, a, a_t;

	A.copyTo(a);
	cv::transpose(a, a_t);
	offset_r.copyTo(b_r);
	b_r = A_t * b_r;

	offset_c.copyTo(b_c);
	b_c = A_t * b_c;

	A = A_t * A;

	double rms1 = -1.0; double rms2 = -1.0;
	Mat eye = Mat::zeros(m * n, m * n, CV_64F);
	for (int i = 0; i < m * n; i++)
	{
		eye.at<double>(i, i) = 1.0;
	}
	if (cv::invert(A, error_r, cv::DECOMP_LU) > 0)
	{
		cv::transpose(offset_r, b_t);
		error_r = b_t * (eye - a * error_r * a_t) * offset_r;
		rms1 = sqrt(error_r.at<double>(0, 0) / double(m * n));
	}
	if (cv::invert(A, error_c, cv::DECOMP_LU) > 0)
	{
		cv::transpose(offset_c, b_t);
		error_c = b_t * (eye - a * error_c * a_t) * offset_c;
		rms2 = sqrt(error_c.at<double>(0, 0) / double(m * n));
	}
	if (!cv::solve(A, b_r, coef_r, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "coregistration_subpixel_sinc(): matrix deficiency!\n");
		return -1;
	}
	if (!cv::solve(A, b_c, coef_c, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "coregistration_subpixel_sinc(): matrix deficiency!\n");
		return -1;
	}

	/*---------------------------------------*/
	/*    双线性插值获取重采样后的辅图像     */
	/*---------------------------------------*/

		/*---------------------------------------*/
	/*    Windowed sinc 插值获取重采样后的辅图像 */
	/*---------------------------------------*/

	Mat tt(1, 3, CV_64F);
	tt.at<double>(0, 0) = 1.0;
	tt.at<double>(0, 1) = (0.0 - offset_x) / scale_x;
	tt.at<double>(0, 2) = (0.0 - offset_y) / scale_y;

	if (offset_row) *offset_row = static_cast<int>(cvRound(sum(tt * coef_r)[0]));
	if (offset_col) *offset_col = static_cast<int>(cvRound(sum(tt * coef_c)[0]));

	int rows = master.GetRows();
	int cols = master.GetCols();

	ComplexMat slave_tmp;
	slave_tmp = master;

	int rows_slave = slave.GetRows();
	int cols_slave = slave.GetCols();

	// sinc 插值核半径
	// radius = 4 表示使用 9×9 的二维 sinc 核
	// 如果想更高精度，可以改成 6，但速度会明显变慢
	const int SINC_RADIUS = 4;

	// 提前取出系数，避免在每个像元里反复创建 Mat，速度会快很多
	const double cr0 = coef_r.at<double>(0, 0);
	const double cr1 = coef_r.at<double>(1, 0);
	const double cr2 = coef_r.at<double>(2, 0);

	const double cc0 = coef_c.at<double>(0, 0);
	const double cc1 = coef_c.at<double>(1, 0);
	const double cc2 = coef_c.at<double>(2, 0);

	std::atomic<int> completed_rows(0);
	int row_step = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		for (int j = 0; j < cols; j++)
		{
			double jj = static_cast<double>(j);
			double ii = static_cast<double>(i);

			double x = (jj - offset_x) / scale_x;
			double y = (ii - offset_y) / scale_y;

			double offset_rows = cr0 + cr1 * x + cr2 * y;
			double offset_cols = cc0 + cc1 * x + cc2 * y;

			double src_row = ii + offset_rows;
			double src_col = jj + offset_cols;

			// 对实部和虚部分别做 sinc 插值
			double re_val = sinc_interp2d(slave.re, src_row, src_col, SINC_RADIUS);
			double im_val = sinc_interp2d(slave.im, src_row, src_col, SINC_RADIUS);

			mat_set_from_double(slave_tmp.re, i, j, re_val);
			mat_set_from_double(slave_tmp.im, i, j, im_val);
		}
		int current_completed = ++completed_rows;
		if (cb && current_completed % row_step == 0)
		{
			int progress = 50 + current_completed * 50 / rows;
			if (!cb(progress, "Sinc resampling...", userData))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag) return -2;

	slave = slave_tmp;
	return 0;
}

int Registration::all_subpixel_move(const Mat& Coordinate_x, const Mat& Coordinate_y, const Mat& offset_row, const Mat& offset_col, Mat& para)
{
	if (Coordinate_x.rows < 1 ||
		Coordinate_x.cols < 1 ||
		Coordinate_x.rows != Coordinate_y.rows ||
		Coordinate_x.cols != Coordinate_y.cols ||
		offset_row.rows != offset_col.rows ||
		offset_row.cols != offset_col.cols)
	{
		fprintf(stderr, "all_subpixel_move(): input size/type check failed!\n\n");
		return -1;
	}
	int N = Coordinate_x.rows;
	Mat cx, cy, or_val, oc_val;
	Coordinate_x.convertTo(cx, CV_64F);
	Coordinate_y.convertTo(cy, CV_64F);
	offset_row.convertTo(or_val, CV_64F);
	offset_col.convertTo(oc_val, CV_64F);

	Mat connect_h_x[] = { Mat::ones(N, 1, CV_64F), cx, cy, cx.mul(cx), cy.mul(cy),
		cy.mul(cx) };
	Mat matrix, matrix_t;
	hconcat(connect_h_x, 6, matrix);
	Mat para1;
	transpose(matrix, matrix_t);
	if (!solve(matrix_t * matrix, matrix_t * or_val, para1, DECOMP_LU))
	{
		fprintf(stderr, "all_subpixel_move(): can't solve least square problem!\n");
		return -1;
	}

	Mat connect_h_y[] = { Mat::ones(N, 1, CV_64F), cx, cy, cx.mul(cx), cy.mul(cy),
		cy.mul(cx) };
	hconcat(connect_h_y, 6, matrix);
	transpose(matrix, matrix_t);
	Mat para2;
	if (!solve(matrix_t * matrix, matrix_t * oc_val, para2, DECOMP_LU))
	{
		fprintf(stderr, "all_subpixel_move(): can't solve least square problem!\n");
		return -1;
	}
	Mat connect[] = { para1, para2 };
	vconcat(connect, 2, para);
	return 0;
}

int Registration::gcps_sift(int rows, int cols, int move_rows, int move_cols, Mat& gcps)
{
	if (fabs(move_rows) > rows ||
		fabs(move_cols) > cols ||
		rows < 1 ||
		cols < 1 ||
		gcps.cols != 5 ||
		gcps.type() != CV_64F ||
		gcps.channels() != 1||
		gcps.rows < 3)
	{
		fprintf(stderr, "gcps_sift(): input check failed!\n\n");
		return -1;
	}
	Mat gcps_tmp, GCP;
	gcps(Range(0, 2), Range(0, 5)).copyTo(GCP);
	gcps.copyTo(gcps_tmp);
	Mat index = Mat::zeros(gcps.rows, 1, CV_64F);
	int count = 0;
	//////////////////////////Scenario 1///////////////////////////////
	if (move_rows > 0 && move_cols > 0)
	{
		for (int i = 0; i < gcps_tmp.rows; i++)
		{
			if (gcps_tmp.at<double>(i, 0) <= double(rows - move_rows) &&
				gcps_tmp.at<double>(i, 1) <= double(cols - move_cols))
			{
				index.at<double>(i, 0) = 1.0;
				count++;
			}
		}
		if (count == 0)
		{
			fprintf(stderr, "all ground control points have been sifted out!\n\n");
			return -1;
		}
		gcps_tmp = Mat::zeros(count, gcps.cols, CV_64F);
		count = 0;
		for (int i = 0; i < gcps.rows; i++)
		{
			if (index.at<double>(i, 0) > 0.5)
			{
				gcps(Range(i, i + 1), Range(0, gcps.cols)).copyTo(gcps_tmp(Range(count, count + 1), Range(0, gcps.cols)));
				count++;
			}
		}
	}
	//////////////////////////Scenario 2///////////////////////////////
	if (move_rows > 0 && move_cols <= 0)
	{
		for (int i = 0; i < gcps_tmp.rows; i++)
		{
			if (gcps_tmp.at<double>(i, 0) <= double(rows - move_rows) &&
				gcps_tmp.at<double>(i, 1) >= double(1 - move_cols))
			{
				index.at<double>(i, 0) = 1.0;
				gcps_tmp.at<double>(i, 1) = gcps_tmp.at<double>(i, 1) + move_cols;
				count++;
			}
		}
		if (count == 0)
		{
			fprintf(stderr, "all ground control points have been sifted out!\n\n");
			return -1;
		}
		Mat gcps_tmp1 = Mat::zeros(count, gcps.cols, CV_64F);
		count = 0;
		for (int i = 0; i < gcps.rows; i++)
		{
			if (index.at<double>(i, 0) > 0.5)
			{
				gcps_tmp(Range(i, i + 1), Range(0, gcps.cols)).copyTo(gcps_tmp1(Range(count, count + 1), Range(0, gcps.cols)));
				count++;
			}
		}
		gcps = gcps_tmp1;
	}
	//////////////////////////Scenario 3///////////////////////////////
	if (move_rows <= 0 && move_cols > 0)
	{
		for (int i = 0; i < gcps_tmp.rows; i++)
		{
			if (gcps_tmp.at<double>(i, 0) >= double(1 - move_rows) &&
				gcps_tmp.at<double>(i, 1) <= double(cols - move_cols))
			{
				index.at<double>(i, 0) = 1.0;
				gcps_tmp.at<double>(i, 0) = gcps_tmp.at<double>(i, 0) + move_rows;
				count++;
			}
		}
		if (count == 0)
		{
			fprintf(stderr, "all ground control points have been sifted out!\n\n");
			return -1;
		}
		Mat gcps_tmp1 = Mat::zeros(count, gcps.cols, CV_64F);
		count = 0;
		for (int i = 0; i < gcps.rows; i++)
		{
			if (index.at<double>(i, 0) > 0.5)
			{
				gcps_tmp(Range(i, i + 1), Range(0, gcps.cols)).copyTo(gcps_tmp1(Range(count, count + 1), Range(0, gcps.cols)));
				count++;
			}
		}
		gcps = gcps_tmp1;
	}
	//////////////////////////Scenario 4///////////////////////////////
	if (move_rows <= 0 && move_cols <= 0)
	{
		for (int i = 0; i < gcps_tmp.rows; i++)
		{
			if (gcps_tmp.at<double>(i, 0) >= double(1 - move_rows) &&
				gcps_tmp.at<double>(i, 1) >= double(1 - move_cols))
			{
				index.at<double>(i, 0) = 1.0;
				gcps_tmp.at<double>(i, 0) = gcps_tmp.at<double>(i, 0) + move_rows;
				gcps_tmp.at<double>(i, 1) = gcps_tmp.at<double>(i, 1) + move_cols;
				count++;
			}
		}
		if (count == 0)
		{
			fprintf(stderr, "all ground control points have been sifted out!\n\n");
			return -1;
		}
		Mat gcps_tmp1 = Mat::zeros(count, gcps.cols, CV_64F);
		count = 0;
		for (int i = 0; i < gcps.rows; i++)
		{
			if (index.at<double>(i, 0) > 0.5)
			{
				gcps_tmp(Range(i, i + 1), Range(0, gcps.cols)).copyTo(gcps_tmp1(Range(count, count + 1), Range(0, gcps.cols)));
				count++;
			}
		}
		gcps = gcps_tmp1;
	}
	Mat GCPS(2 + gcps.rows, 5, CV_64F);
	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 5; j++)
		{
			GCPS.at<double>(i, j) = GCP.at<double>(i, j);
		}
	}
	for (int i = 2; i < GCPS.rows; i++)
	{
		for (int j = 0; j < 5; j++)
		{
			GCPS.at<double>(i, j) = gcps.at<double>(i - 2, j);
		}
	}
	GCPS.copyTo(gcps);
	return 0;
}

int Registration::getDEMRgAzPos(
	Mat& DEM,
	Mat& stateVector, 
	Mat& rangePos,
	Mat& azimuthPos, 
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
	double lon_spacing,
	double lat_spacing,
	RegistrationProgressCallback cb,
	void* userData
)
{
	if (DEM.empty() ||
		DEM.type() != CV_16S ||
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
		fprintf(stderr, "getDEMRgAzPos(): input check failed!\n");
		return -1;
	}
	//初始化轨道类
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime);
	stateVectors.applyOrbit();
	// removed unused: ret (no fallible call in this function)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	rangePos.create(DEM_rows, DEM_cols, CV_64F);
	azimuthPos.create(DEM_rows, DEM_cols, CV_64F);
	double dopplerFrequency = 0.0;
	std::atomic<bool> cancel_flag(false);
	std::atomic<int> completed_rows(0);
	int step = std::max(1, DEM_rows / 100);

	//采用迭代计算每个DEM点在SAR图像中的坐标，以减小计算量
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
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
				rangePos.at<double>(i, j) = -1.0;
				azimuthPos.at<double>(i, j) = -1.0;
				continue;
			}
			int azimuthIndex = static_cast<int>((zeroDopplerTime - acquisitionStartTime) / time_interval);
			int rangeIndex = static_cast<int>((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 1 || rangeIndex < 0 || rangeIndex > sceneWidth - 1)
			{
				rangePos.at<double>(i, j) = -1.0;
				azimuthPos.at<double>(i, j) = -1.0;
			}
			else
			{
				rangePos.at<double>(i, j) = rangeIndex;
				azimuthPos.at<double>(i, j) = azimuthIndex;
			}
		}

		int current_completed = ++completed_rows;
		if (cb && current_completed % step == 0)
		{
			int progress = current_completed * 100 / DEM_rows;
			if (!cb(progress, "Solving radar geometry equations...", userData))
			{
				cancel_flag = true;
			}
		}
	}
	if (cancel_flag)
	{
		return -2;
	}
	return 0;
}

int Registration::fitSlaveOffset(Mat& slaveOffset, Mat& masterRange,
	Mat& masterAzimuth, double* a0, double* a1, double* a2)
{
	if (slaveOffset.empty() ||
		slaveOffset.type() != CV_64F || 
		masterRange.type() != CV_64F ||
		masterAzimuth.type() != CV_64F ||
		masterRange.rows != slaveOffset.rows ||
		masterRange.cols != slaveOffset.cols ||
		masterAzimuth.rows != slaveOffset.rows ||
		masterAzimuth.cols != slaveOffset.cols
		)
	{
		fprintf(stderr, "fitSlaveOffset(): input check failed!\n");
		return -1;
	}
	int count = 0, nr, nc;
	nr = slaveOffset.rows;
	nc = slaveOffset.cols;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (fabs(slaveOffset.at<double>(i, j) - invalidOffset) > 0.0001) count++;
		}
	}
	if (count < 4)
	{
		fprintf(stderr, "fitSlaveOffset(): not enough valid offsetpoints!\n");
		return -1;
	}
	Mat offset(count, 1, CV_64F);
	Mat range(count, 1, CV_64F);
	Mat azimuth(count, 1, CV_64F);
	Mat A = Mat::ones(count, 3, CV_64F);
	count = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (fabs(slaveOffset.at<double>(i, j) - invalidOffset) > 0.0001)
			{
				offset.at<double>(count, 0) = slaveOffset.at<double>(i, j);
				range.at<double>(count, 0) = masterRange.at<double>(i, j);
				azimuth.at<double>(count++, 0) = masterAzimuth.at<double>(i, j);
			}
		}
	}
	range.copyTo(A(cv::Range(0, count), cv::Range(1, 2)));
	azimuth.copyTo(A(cv::Range(0, count), cv::Range(2, 3)));
	Mat A_t, b, coef;
	cv::transpose(A, A_t);
	A = A_t * A;
	b = A_t * offset;
	if (!cv::solve(A, b, coef, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "fitSlaveOffset(): matrix deficiency!\n");
		return -1;
	}
	if (a0) *a0 = coef.at<double>(0, 0);
	if (a1) *a1 = coef.at<double>(1, 0);
	if (a2) *a2 = coef.at<double>(2, 0);
	return 0;
}

int Registration::computeSlaveOffset(
	Mat& masterRange,
	Mat& masterAzimuth, 
	Mat& slaveRange,
	Mat& slaveAzimuth,
	Mat& slaveAzimuthOffset, 
	Mat& slaveRangeOffset
)
{
	if (masterRange.rows != masterAzimuth.rows ||
		masterRange.cols != masterAzimuth.cols ||
		masterRange.rows != slaveRange.rows ||
		masterRange.cols != slaveRange.cols ||
		masterRange.rows != slaveAzimuth.rows ||
		masterRange.cols != slaveAzimuth.cols
		)
	{
		fprintf(stderr, "computeSlaveOffset(): input check failed!\n");
		return -1;
	}
	slaveAzimuthOffset.create(masterRange.size(), CV_64F);
	slaveRangeOffset.create(masterRange.size(), CV_64F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < slaveAzimuthOffset.rows; i++)
	{
		for (int j = 0; j < slaveAzimuthOffset.cols; j++)
		{
			//方位向偏移量计算
			if (masterAzimuth.at<double>(i, j) < -0.5 || slaveAzimuth.at<double>(i, j) < -0.5)
			{
				slaveAzimuthOffset.at<double>(i, j) = invalidOffset;
			}
			else
			{
				slaveAzimuthOffset.at<double>(i, j) = slaveAzimuth.at<double>(i, j) - masterAzimuth.at<double>(i, j);
			}
			//距离向偏移量计算
			if (masterRange.at<double>(i, j) < -0.5 || slaveRange.at<double>(i, j) < -0.5)
			{
				slaveRangeOffset.at<double>(i, j) = invalidOffset;
			}
			else
			{
				slaveRangeOffset.at<double>(i, j) = slaveRange.at<double>(i, j) - masterRange.at<double>(i, j);
			}
		}
	}
	return 0;
}

int Registration::performBilinearResampling(
	ComplexMat& slave, 
	int dstHeight,
	int dstWidth, 
	double a0Rg, double a1Rg, double a2Rg, 
	double a0Az, double a1Az, double a2Az,
	int* offset_row,
	int* offset_col,
	RegistrationProgressCallback cb,
	void* userData
)
{
	if (slave.isEmpty() || dstHeight < 2 || dstWidth < 2 ||
		(slave.type() != CV_16S && slave.type() != CV_64F && slave.type() != CV_32F)
		)
	{
		fprintf(stderr, "performBilinearResampling(): input check failed!\n");
		return -1;
	}
	ComplexMat slcResampled;
	int type = slave.type();
	if (type == CV_16S)
	{
		slcResampled.re.create(dstHeight, dstWidth, CV_16S);
		slcResampled.im.create(dstHeight, dstWidth, CV_16S);
	}
	else if (type == CV_32F)
	{
		slcResampled.re.create(dstHeight, dstWidth, CV_32F);
		slcResampled.im.create(dstHeight, dstWidth, CV_32F);
	}
	else {
		slcResampled.re.create(dstHeight, dstWidth, CV_64F);
		slcResampled.im.create(dstHeight, dstWidth, CV_64F);
	}
	
	Mat coef_r(3, 1, CV_64F), coef_c(3, 1, CV_64F);
	coef_r.at<double>(0, 0) = a0Az;
	coef_r.at<double>(1, 0) = a1Az;
	coef_r.at<double>(2, 0) = a2Az;
	coef_c.at<double>(0, 0) = a0Rg;
	coef_c.at<double>(1, 0) = a1Rg;
	coef_c.at<double>(2, 0) = a2Rg;
	if (offset_row && offset_col)
	{
		Mat tt(1, 3, CV_64F);
		tt.at<double>(0, 0) = 1.0;
		tt.at<double>(0, 1) = 0.0;
		tt.at<double>(0, 2) = 0.0;

		*offset_row = static_cast<int>(sum(tt * coef_r)[0]);
		*offset_col = static_cast<int>(sum(tt * coef_c)[0]);
	}
	int rows = dstHeight; int cols = dstWidth;
	int cols_slave = slave.GetCols(); int rows_slave = slave.GetRows();

	const double cr0 = coef_r.at<double>(0, 0);
	const double cr1 = coef_r.at<double>(1, 0);
	const double cr2 = coef_r.at<double>(2, 0);
	const double cc0 = coef_c.at<double>(0, 0);
	const double cc1 = coef_c.at<double>(1, 0);
	const double cc2 = coef_c.at<double>(2, 0);

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		// removed unused: x, y
		double ii, jj;
		double offset_rows, offset_cols;
		for (int j = 0; j < cols; j++)
		{
			jj = (double)j;
			ii = (double)i;

			offset_rows = cr0 + cr1 * jj + cr2 * ii;
			offset_cols = cc0 + cc1 * jj + cc2 * ii;

			ii += offset_rows;
			jj += offset_cols;

			double re_val = bilinear_interp2d(slave.re, ii, jj);
			double im_val = bilinear_interp2d(slave.im, ii, jj);

			mat_set_from_double(slcResampled.re, i, j, re_val);
			mat_set_from_double(slcResampled.im, i, j, im_val);
		}

		int current_completed = ++completed_rows;
		if (cb && current_completed % step == 0)
		{
			int progress = current_completed * 100 / rows;
			if (!cb(progress, "Bilinear resampling...", userData))
			{
				cancel_flag = true;
			}
		}
	}

	if (cancel_flag)
	{
		return -2;
	}
	slave = slcResampled;
	return 0;
}


int Registration::performSincResampling(
	ComplexMat& slave,
	int dstHeight,
	int dstWidth,
	double a0Rg, double a1Rg, double a2Rg,
	double a0Az, double a1Az, double a2Az,
	int* offset_row,
	int* offset_col,
	RegistrationProgressCallback cb,
	void* userData
)
{
	if (slave.isEmpty() || dstHeight < 2 || dstWidth < 2 ||
		(slave.type() != CV_16S && slave.type() != CV_64F && slave.type() != CV_32F)
		)
	{
		fprintf(stderr, "performBilinearResampling(): input check failed!\n");
		return -1;
	}

	ComplexMat slcResampled;
	int type = slave.type();

	if (type == CV_16S)
	{
		slcResampled.re.create(dstHeight, dstWidth, CV_16S);
		slcResampled.im.create(dstHeight, dstWidth, CV_16S);
	}
	else if (type == CV_32F)
	{
		slcResampled.re.create(dstHeight, dstWidth, CV_32F);
		slcResampled.im.create(dstHeight, dstWidth, CV_32F);
	}
	else
	{
		slcResampled.re.create(dstHeight, dstWidth, CV_64F);
		slcResampled.im.create(dstHeight, dstWidth, CV_64F);
	}

	// Azimuth / row direction offset
	Mat coef_r(3, 1, CV_64F);

	coef_r.at<double>(0, 0) = a0Az;
	coef_r.at<double>(1, 0) = a1Az;
	coef_r.at<double>(2, 0) = a2Az;

	// Range / column direction offset
	Mat coef_c(3, 1, CV_64F);

	coef_c.at<double>(0, 0) = a0Rg;
	coef_c.at<double>(1, 0) = a1Rg;
	coef_c.at<double>(2, 0) = a2Rg;

	if (offset_row && offset_col)
	{
		Mat tt(1, 3, CV_64F);
		tt.at<double>(0, 0) = 1.0;
		tt.at<double>(0, 1) = 0.0;
		tt.at<double>(0, 2) = 0.0;

		// 保持原接口 int*，这里仍然四舍五入
		*offset_row = static_cast<int>(cvRound(sum(tt * coef_r)[0]));
		*offset_col = static_cast<int>(cvRound(sum(tt * coef_c)[0]));
	}

	int rows = dstHeight;
	int cols = dstWidth;

	// sinc 插值半径
	// SINC_RADIUS = 4 表示使用 9 × 9 有限窗 sinc 核
	// 可改为 6，但速度会明显下降
	const int SINC_RADIUS = 4;

	// 提前取出系数，避免每个像元反复创建 Mat 和做矩阵乘法
	const double cr0 = a0Az;
	const double cr1 = a1Az;
	const double cr2 = a2Az;

	const double cc0 = a0Rg;
	const double cc1 = a1Rg;
	const double cc2 = a2Rg;

	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, rows / 100);

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag) {
			continue;
		}
		for (int j = 0; j < cols; j++)
		{
			double ii = static_cast<double>(i);
			double jj = static_cast<double>(j);

			// offset_rows = a0Az + a1Az * col + a2Az * row
			double offset_rows = cr0 + cr1 * jj + cr2 * ii;

			// offset_cols = a0Rg + a1Rg * col + a2Rg * row
			double offset_cols = cc0 + cc1 * jj + cc2 * ii;

			double src_row = ii + offset_rows;
			double src_col = jj + offset_cols;

			double re_value = sinc_interp2d(slave.re, src_row, src_col, SINC_RADIUS);
			double im_value = sinc_interp2d(slave.im, src_row, src_col, SINC_RADIUS);

			mat_set_from_double(slcResampled.re, i, j, re_value);
			mat_set_from_double(slcResampled.im, i, j, im_value);
		}

		int current_completed = ++completed_rows;
		if (cb && current_completed % step == 0)
		{
			int progress = current_completed * 100 / rows;
			if (!cb(progress, "Sinc resampling...", userData))
			{
				cancel_flag = true;
			}
		}
	}

	if (cancel_flag)
	{
		return -2;
	}
	slave = slcResampled;

	return 0;
}

// ==========================================
// 外部导出 API 接口具体实现
// ==========================================

extern "C" InSAR_API int DetectAdaptiveSamplingPoints(
	const char* master_h5_path,
	Point2D* out_points,
	int points_count
) {
	if (master_h5_path == nullptr || out_points == nullptr || points_count != 5)
	{
		return -1;
	}

	FormatConversion fc;
	int rows = 0;
	int cols = 0;
	if (fc.get_dataset_dims(master_h5_path, "s_re", &rows, &cols) != 0)
	{
		return -2;
	}

	int stepY = rows / 6;
	int stepX = cols / 6;

	// 定义 5 个区域中格点的搜索顺序，确保空间分布均匀性
	// 5个区域对应：左上、右上、中心、左下、右下
	std::vector<std::vector<std::pair<int, int>>> search_regions = {
		// 左上区域
		{ {1, 1}, {1, 2}, {2, 1}, {2, 2} },
		// 右上区域
		{ {1, 5}, {1, 4}, {2, 5}, {2, 4} },
		// 中心区域
		{ {3, 3}, {3, 2}, {3, 4}, {2, 3}, {4, 3}, {2, 2}, {2, 4}, {4, 2}, {4, 4} },
		// 左下区域
		{ {5, 1}, {5, 2}, {4, 1}, {4, 2} },
		// 右下区域
		{ {5, 5}, {5, 4}, {4, 5}, {4, 4} }
	};

	int found_count = 0;
	for (int k = 0; k < 5; ++k)
	{
		bool found_in_region = false;
		for (const auto& grid_pos : search_regions[k])
		{
			int i = grid_pos.first;
			int j = grid_pos.second;

			int center_y = i * stepY;
			int center_x = j * stepX;

			// 边界安全校验，并读取 3x3 的微型区域
			int y_start = center_y - 1;
			int x_start = center_x - 1;
			if (y_start < 0 || x_start < 0 || y_start + 3 > rows || x_start + 3 > cols)
			{
				continue;
			}

			cv::Mat re_mat, im_mat;
			if (fc.read_subarray_from_h5(master_h5_path, "s_re", y_start, x_start, 3, 3, re_mat) == 0 &&
				fc.read_subarray_from_h5(master_h5_path, "s_im", y_start, x_start, 3, 3, im_mat) == 0)
			{
				// 转换为 CV_32F 并进行安全校验以符合 GEMINI.md 守则一
				if (re_mat.type() != CV_32F) re_mat.convertTo(re_mat, CV_32F);
				if (im_mat.type() != CV_32F) im_mat.convertTo(im_mat, CV_32F);

				// 检查微型区域内是否有非零有效强度的反射值
				bool is_valid = false;
				for (int r = 0; r < re_mat.rows; ++r)
				{
					for (int c = 0; c < re_mat.cols; ++c)
					{
						float val_re = re_mat.at<float>(r, c);
						float val_im = im_mat.at<float>(r, c);
						if (std::abs(val_re) > 1e-4f || std::abs(val_im) > 1e-4f)
						{
							is_valid = true;
							break;
						}
					}
					if (is_valid) break;
				}

				if (is_valid)
				{
					out_points[k].y = center_y;
					out_points[k].x = center_x;
					found_in_region = true;
					found_count++;
					break;
				}
			}
		}

		// 容错降级：如果该区域内没有搜寻到非零样点，则强制设置中心默认格点，防止返回空点
		if (!found_in_region)
		{
			out_points[k].y = search_regions[k][0].first * stepY;
			out_points[k].x = search_regions[k][0].second * stepX;
			found_count++;
		}
	}

	return found_count;
}

extern "C" InSAR_API int CalculateOffsetAndCoherence(
	const char* master_h5_path,
	const char* slave_h5_path,
	const Point2D* sample_points,
	int points_count,
	int template_size,
	int search_size,
	AlignmentResult* out_results
) {
	if (master_h5_path == nullptr || slave_h5_path == nullptr || sample_points == nullptr ||
		out_results == nullptr || points_count != 5 || template_size <= 0 || search_size <= template_size)
	{
		return -1;
	}

	FormatConversion fc;
	int rows = 0;
	int cols = 0;
	if (fc.get_dataset_dims(master_h5_path, "s_re", &rows, &cols) != 0)
	{
		return -2;
	}

	for (int i = 0; i < points_count; ++i)
	{
		// 初始化指针为 nullptr，以防出错时调用 Free 发生异常
		out_results[i].heatmap_rgb = nullptr;
		out_results[i].overlay_rgb = nullptr;

		Point2D pt = sample_points[i];
		int tr = template_size / 2;
		int sr = search_size / 2;

		// 1. 计算 Master（模板）与 Slave（搜索）区域的起始偏移量并进行安全边界剪裁
		int m_row = pt.y - tr;
		int m_col = pt.x - tr;
		int s_row = pt.y - sr;
		int s_col = pt.x - sr;

		if (m_row < 0) m_row = 0;
		if (m_col < 0) m_col = 0;
		if (m_row + template_size > rows) m_row = rows - template_size;
		if (m_col + template_size > cols) m_col = cols - template_size;

		if (s_row < 0) s_row = 0;
		if (s_col < 0) s_col = 0;
		if (s_row + search_size > rows) s_row = rows - search_size;
		if (s_col + search_size > cols) s_col = cols - search_size;

		// 2. 从 H5 文件分块读取主副影像的实部/虚部数据
		cv::Mat M_re, M_im, S_re, S_im;
		if (fc.read_subarray_from_h5(master_h5_path, "s_re", m_row, m_col, template_size, template_size, M_re) != 0 ||
			fc.read_subarray_from_h5(master_h5_path, "s_im", m_row, m_col, template_size, template_size, M_im) != 0 ||
			fc.read_subarray_from_h5(slave_h5_path, "s_re", s_row, s_col, search_size, search_size, S_re) != 0 ||
			fc.read_subarray_from_h5(slave_h5_path, "s_im", s_row, s_col, search_size, search_size, S_im) != 0)
		{
			return -3;
		}

		// 强制转换为 CV_32F (单精度)，严防 direct pointer type mismatch 导致的 Bug
		if (M_re.type() != CV_32F) M_re.convertTo(M_re, CV_32F);
		if (M_im.type() != CV_32F) M_im.convertTo(M_im, CV_32F);
		if (S_re.type() != CV_32F) S_re.convertTo(S_re, CV_32F);
		if (S_im.type() != CV_32F) S_im.convertTo(S_im, CV_32F);

		// 3. 计算幅度矩阵
		cv::Mat Amp_M, Amp_S;
		cv::magnitude(M_re, M_im, Amp_M);
		cv::magnitude(S_re, S_im, Amp_S);

		/* 备份老代码 - 直接对原始振幅匹配相关系数过低且易产生斑噪随机偏置
		// 4. 执行模板匹配搜索偏差量
		cv::Mat match_res;
		cv::matchTemplate(Amp_S, Amp_M, match_res, cv::TM_CCOEFF_NORMED);
		*/

		// 新改动：做 5x5 均值滤波平滑（多视处理，压制相干斑噪声）后进行匹配以提升稳定度
		cv::Mat Amp_M_smooth, Amp_S_smooth;
		cv::blur(Amp_M, Amp_M_smooth, cv::Size(5, 5));
		cv::blur(Amp_S, Amp_S_smooth, cv::Size(5, 5));

		// 4. 执行模板匹配搜索偏差量
		cv::Mat match_res;
		cv::matchTemplate(Amp_S_smooth, Amp_M_smooth, match_res, cv::TM_CCOEFF_NORMED);

		double maxVal = 0.0;
		cv::Point maxLoc;
		cv::minMaxLoc(match_res, nullptr, &maxVal, nullptr, &maxLoc);

		// 结算匹配偏差偏移量 (dy, dx)
		int dy = maxLoc.y - (search_size - template_size) / 2;
		int dx = maxLoc.x - (search_size - template_size) / 2;

		out_results[i].maxCorrelation = maxVal;
		out_results[i].offsetY = dy;
		out_results[i].offsetX = dx;
		out_results[i].imageWidth = template_size;
		out_results[i].imageHeight = template_size;

		// 5. 零位移相干性结算 (读取未偏移的同尺寸 Slave 块)
		cv::Mat S_re_zero, S_im_zero;
		if (fc.read_subarray_from_h5(slave_h5_path, "s_re", m_row, m_col, template_size, template_size, S_re_zero) != 0 ||
			fc.read_subarray_from_h5(slave_h5_path, "s_im", m_row, m_col, template_size, template_size, S_im_zero) != 0)
		{
			return -4;
		}
		if (S_re_zero.type() != CV_32F) S_re_zero.convertTo(S_re_zero, CV_32F);
		if (S_im_zero.type() != CV_32F) S_im_zero.convertTo(S_im_zero, CV_32F);

		cv::Mat coh_zero_map;
		double coh_zero_mean = 0.0;
		ComputeSubBlockCoherence(M_re, M_im, S_re_zero, S_im_zero, coh_zero_map, coh_zero_mean);
		out_results[i].coherenceZeroShift = coh_zero_mean;

		// 6. 最佳位移相干性结算 (读取偏移后对齐的 Slave 块)
		int s_opt_row = m_row + dy;
		int s_opt_col = m_col + dx;
		if (s_opt_row < 0) s_opt_row = 0;
		if (s_opt_col < 0) s_opt_col = 0;
		if (s_opt_row + template_size > rows) s_opt_row = rows - template_size;
		if (s_opt_col + template_size > cols) s_opt_col = cols - template_size;

		cv::Mat S_re_opt, S_im_opt;
		if (fc.read_subarray_from_h5(slave_h5_path, "s_re", s_opt_row, s_opt_col, template_size, template_size, S_re_opt) != 0 ||
			fc.read_subarray_from_h5(slave_h5_path, "s_im", s_opt_row, s_opt_col, template_size, template_size, S_im_opt) != 0)
		{
			return -5;
		}
		if (S_re_opt.type() != CV_32F) S_re_opt.convertTo(S_re_opt, CV_32F);
		if (S_im_opt.type() != CV_32F) S_im_opt.convertTo(S_im_opt, CV_32F);

		cv::Mat coh_opt_map;
		double coh_opt_mean = 0.0;
		ComputeSubBlockCoherence(M_re, M_im, S_re_opt, S_im_opt, coh_opt_map, coh_opt_mean);
		out_results[i].coherenceOptimal = coh_opt_mean;

		// 7. 渲染相干性热力图 (Heatmap)
		cv::Mat coh_8u;
		coh_opt_map.convertTo(coh_8u, CV_8U, 255.0);
		cv::Mat heatmap_bgr, heatmap_rgb;
		cv::applyColorMap(coh_8u, heatmap_bgr, cv::COLORMAP_JET);
		cv::cvtColor(heatmap_bgr, heatmap_rgb, cv::COLOR_BGR2RGB);

		int img_bytes = template_size * template_size * 3;
		out_results[i].heatmap_rgb = new unsigned char[img_bytes];
		std::memcpy(out_results[i].heatmap_rgb, heatmap_rgb.data, img_bytes);

		// 8. 渲染红-青叠合对比图 (Red-Cyan Overlay Map)
		cv::Mat Amp_M_norm, Amp_S_opt, Amp_S_norm;
		NormalizeAndClamp2Sigma(Amp_M, Amp_M_norm);
		cv::magnitude(S_re_opt, S_im_opt, Amp_S_opt);
		NormalizeAndClamp2Sigma(Amp_S_opt, Amp_S_norm);

		std::vector<cv::Mat> channels = { Amp_S_norm, Amp_S_norm, Amp_M_norm }; // BGR order
		cv::Mat overlay_bgr, overlay_rgb;
		cv::merge(channels, overlay_bgr);
		cv::cvtColor(overlay_bgr, overlay_rgb, cv::COLOR_BGR2RGB);

		out_results[i].overlay_rgb = new unsigned char[img_bytes];
		std::memcpy(out_results[i].overlay_rgb, overlay_rgb.data, img_bytes);
	}

	return 0;
}

extern "C" InSAR_API void FreeAlignmentResults(
	AlignmentResult* results,
	int count
) {
	if (results == nullptr) return;
	for (int i = 0; i < count; ++i)
	{
		if (results[i].heatmap_rgb != nullptr)
		{
			delete[] results[i].heatmap_rgb;
			results[i].heatmap_rgb = nullptr;
		}
		if (results[i].overlay_rgb != nullptr)
		{
			delete[] results[i].overlay_rgb;
			results[i].overlay_rgb = nullptr;
		}
	}
}

extern "C" InSAR_API int AnalyzeCropRegistration(
	const char* master_h5_path,
	const char* slave_h5_path,
	const char* output_coherence_jpg,
	const char* output_phase_jpg,
	double thres_mean_pass,
	double thres_ratio_pass,
	double thres_mean_warn,
	double thres_ratio_warn,
	CropEvalResult* out_result
) {
	if (master_h5_path == nullptr || slave_h5_path == nullptr ||
		output_coherence_jpg == nullptr || output_phase_jpg == nullptr || out_result == nullptr)
	{
		return -1;
	}

	// 1. 设置阈值默认缺省值
	double t_mean_pass = (thres_mean_pass < 0.0) ? 0.5 : thres_mean_pass;
	double t_ratio_pass = (thres_ratio_pass < 0.0) ? 0.50 : thres_ratio_pass;
	double t_mean_warn = (thres_mean_warn < 0.0) ? 0.3 : thres_mean_warn;
	double t_ratio_warn = (thres_ratio_warn < 0.0) ? 0.30 : thres_ratio_warn;

	FormatConversion fc;
	int rows = 0;
	int cols = 0;
	if (fc.get_dataset_dims(master_h5_path, "s_re", &rows, &cols) != 0)
	{
		return -2;
	}
	if (rows <= 0 || cols <= 0)
	{
		return -2;
	}

	// 2. 从 H5 读取全图实部与虚部数据
	cv::Mat M_re, M_im;
	if (fc.read_array_from_h5(master_h5_path, "s_re", M_re) != 0 ||
		fc.read_array_from_h5(master_h5_path, "s_im", M_im) != 0)
	{
		return -3;
	}

	cv::Mat S_re, S_im;
	if (fc.read_array_from_h5(slave_h5_path, "s_re", S_re) != 0 ||
		fc.read_array_from_h5(slave_h5_path, "s_im", S_im) != 0)
	{
		return -4;
	}

	// 3. 精度强制转换为 CV_32F，保障指针操作安全，规避 mismatch
	if (M_re.type() != CV_32F) M_re.convertTo(M_re, CV_32F);
	if (M_im.type() != CV_32F) M_im.convertTo(M_im, CV_32F);
	if (S_re.type() != CV_32F) S_re.convertTo(S_re, CV_32F);
	if (S_im.type() != CV_32F) S_im.convertTo(S_im, CV_32F);

	// 4. 并行干涉相位矩阵计算 (OpenMP 行级并行加速)
	cv::Mat phase_mat(rows, cols, CV_32F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; ++i)
	{
		const float* m_re = M_re.ptr<float>(i);
		const float* m_im = M_im.ptr<float>(i);
		const float* s_re = S_re.ptr<float>(i);
		const float* s_im = S_im.ptr<float>(i);
		float* p_out = phase_mat.ptr<float>(i);
		for (int j = 0; j < cols; ++j)
		{
			// Re(M * S*) = M_re * S_re + M_im * S_im
			float re = m_re[j] * s_re[j] + m_im[j] * s_im[j];
			// Im(M * S*) = M_im * S_re - M_re * S_im
			float im = m_im[j] * s_re[j] - m_re[j] * s_im[j];
			p_out[j] = std::atan2(im, re);
		}
	}

	// 5. 并行相干性矩阵计算 (复用 ComputeSubBlockCoherence)
	cv::Mat coh_mat;
	double coh_mean = 0.0;
	if (!ComputeSubBlockCoherence(M_re, M_im, S_re, S_im, coh_mat, coh_mean))
	{
		return -5;
	}

	// 6. 直方图统计法估算中位数与高相干像素占比 (O(N) 复杂度)
	int hist[10000] = { 0 };
#pragma omp parallel
	{
		int local_hist[10000] = { 0 };
#pragma omp for nowait
		for (int r = 0; r < rows; ++r)
		{
			const float* ptr = coh_mat.ptr<float>(r);
			for (int c = 0; c < cols; ++c)
			{
				float val = ptr[c];
				int idx = static_cast<int>(val * 9999.0f);
				if (idx < 0) idx = 0;
				if (idx > 9999) idx = 9999;
				local_hist[idx]++;
			}
		}
#pragma omp critical
		{
			for (int i = 0; i < 10000; ++i)
			{
				hist[i] += local_hist[i];
			}
		}
	}

	long long total_pixels = static_cast<long long>(rows) * cols;
	long long target_half = total_pixels / 2;
	long long accum = 0;
	int median_bin = 0;
	for (int i = 0; i < 10000; ++i)
	{
		accum += hist[i];
		if (accum >= target_half)
		{
			median_bin = i;
			break;
		}
	}
	double coh_median = median_bin / 9999.0;

	// 高相干阈值 (>0.5，对应直方图索引 >= 5000)
	long long high_coh_pixels = 0;
	for (int i = 5000; i < 10000; ++i)
	{
		high_coh_pixels += hist[i];
	}
	double high_coh_pct = static_cast<double>(high_coh_pixels) / total_pixels;

	// 获取最大相干性
	double max_coh = 0.0;
	cv::minMaxLoc(coh_mat, nullptr, &max_coh);

	// 7. 评估状态计算
	int status = 2; // FAILED
	if (coh_mean >= t_mean_pass && high_coh_pct >= t_ratio_pass)
	{
		status = 0; // PASS
	}
	else if (coh_mean >= t_mean_warn && high_coh_pct >= t_ratio_warn)
	{
		status = 1; // WARNING
	}

	out_result->meanCoherence = coh_mean;
	out_result->medianCoherence = coh_median;
	out_result->maxCoherence = max_coh;
	out_result->highCoherencePct = high_coh_pct;
	out_result->assessmentStatus = status;

	// 8. 图像直接落盘保存 (JPG 质量 95，静默覆盖)
	// (1) 保存干涉相位图
	cv::Mat phase_8u;
	phase_mat.convertTo(phase_8u, CV_8U, 255.0 / (2.0 * INSAR_PI), 127.5);
	cv::Mat phase_color;
	cv::applyColorMap(phase_8u, phase_color, cv::COLORMAP_JET);
	
	std::vector<int> compression_params;
	compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
	compression_params.push_back(95);

	if (!cv::imwrite(output_phase_jpg, phase_color, compression_params))
	{
		return -6;
	}

	// (2) 保存相干系数图
	cv::Mat coh_8u;
	coh_mat.convertTo(coh_8u, CV_8U, 255.0);
	if (!cv::imwrite(output_coherence_jpg, coh_8u, compression_params))
	{
		return -7;
	}

	return 0;
}



