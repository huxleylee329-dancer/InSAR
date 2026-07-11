// Filter.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include"..\include\Filter.h"
#include<tchar.h>
#include <atlconv.h>
#include <atomic>
#ifdef _DEBUG
#pragma comment(lib,"ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#else
#pragma comment(lib,"ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#endif // _DEBUG
using namespace cv;

namespace {
	std::wstring toWString(const std::string& str)
	{
		if (str.empty()) return std::wstring();
		int size_needed = MultiByteToWideChar(CP_ACP, 0, &str[0], (int)str.size(), NULL, 0);
		std::wstring wstrTo(size_needed, 0);
		MultiByteToWideChar(CP_ACP, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
		return wstrTo;
	}
}

Filter::Filter()
{
	this->error_head = "FILTER_DLL_ERROR: error happens when using ";
	this->parallel_error_head = "FILTER_DLL_ERROR: error happens when using parallel computing in function: ";
}

Filter::~Filter()
{
}

int Filter::czt2(Mat& src, Mat& dst, int M, int N, double theta0, double phi0,
	const Mat& h_dft, const Mat& result_const, Mat& g, Mat& g_trans, Mat& y, Mat& W, Mat& tmp)
{
	if (src.cols < 1 ||
		src.rows < 1 ||
		M < 1 ||
		N < 1 ||
		(src.type() != CV_64FC2 && src.type() != CV_32FC2))
	{
		fprintf(stderr, "czt2(): input check failed!\n\n");
		return -1;
	}
	theta0 = -theta0;
	int nc = src.cols;
	int L = h_dft.cols;
	int i, j;

	if (src.type() == CV_32FC2)
	{
		// Float precision implementation
		for (i = 0; i < N; i++)
		{
			float val = (float)(theta0 * i - phi0 * 0.5 * i * i);
			float c = cosf(val);
			float s = sinf(val);
			for (j = 0; j < nc; j++)
			{
				W.at<Vec2f>(i, j)[0] = c;/*实部*/
				W.at<Vec2f>(i, j)[1] = s;/*虚部*/
			}
		}
		mulSpectrums(W, src, tmp, 0, false);
		
		g.setTo(Scalar::all(0));
		tmp.copyTo(g(Range(0, N), Range(0, nc)));

		transpose(g, g_trans);
		dft(g_trans, g_trans, DFT_ROWS);
		
		mulSpectrums(g_trans, h_dft, g_trans, 0, false);
		idft(g_trans, g_trans, DFT_ROWS);
		
		transpose(g_trans, y);

		mulSpectrums(result_const, y(Range(0, M), Range(0, nc)), dst, 0, false);
		dst /= (float)(L);
	}
	else
	{
		// Double precision implementation
		for (i = 0; i < N; i++)
		{
			double val = theta0 * i - phi0 * 0.5 * i * i;
			double c = cos(val);
			double s = sin(val);
			for (j = 0; j < nc; j++)
			{
				W.at<Vec2d>(i, j)[0] = c;/*实部*/
				W.at<Vec2d>(i, j)[1] = s;/*虚部*/
			}
		}
		mulSpectrums(W, src, tmp, 0, false);
		
		g.setTo(Scalar::all(0));
		tmp.copyTo(g(Range(0, N), Range(0, nc)));

		transpose(g, g_trans);
		dft(g_trans, g_trans, DFT_ROWS);
		
		mulSpectrums(g_trans, h_dft, g_trans, 0, false);
		idft(g_trans, g_trans, DFT_ROWS);
		
		transpose(g_trans, y);

		mulSpectrums(result_const, y(Range(0, M), Range(0, nc)), dst, 0, false);
		dst /= double(L);
	}
	return 0;
}

int Filter::meanfilter(Mat& Src, int WndSize)
{
	if (WndSize % 2 == 0 ||
		WndSize < 1 ||
		Src.rows < 1 ||
		Src.cols < 1 ||
		(Src.type() != CV_64FC2 && Src.type() != CV_32FC2))
	{
		fprintf(stderr, "meanfilter(): input check failed!\n\n");
		return -1;
	}
	int depth = Src.depth();
	Mat planes[] = { Mat::zeros(Src.rows, Src.cols, depth), Mat::zeros(Src.rows, Src.cols, depth) };
	split(Src, planes);
	blur(planes[0], planes[0], Size(WndSize, WndSize));
	blur(planes[1], planes[1], Size(WndSize, WndSize));
	merge(planes, 2, Src);
	return 0;
}

int Filter::fftshift2(Mat& matrix)
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

int Filter::slope_adaptive_filter(Mat& phase, Mat& phase_filter, int wndsize_filter, int wndsize_prefilter, FilterProgressCallback cb)
{
	if (wndsize_filter % 2 == 0 ||
		wndsize_prefilter % 2 == 0 ||
		wndsize_filter < 3 ||
		wndsize_prefilter < 3 ||
		wndsize_filter > int(phase.rows / 2) ||
		wndsize_filter > int(phase.cols / 2) ||
		wndsize_prefilter > int(phase.rows / 2) ||
		wndsize_prefilter > int(phase.cols / 2) ||
		(phase.type() != CV_64F && phase.type() != CV_32F) ||
		phase.channels() > 1)
	{
		fprintf(stderr, "slope_adaptive_filter(): input check failed!\n\n");
		return -1;
	}

	Mat phase_f;
	phase.convertTo(phase_f, CV_32F);

	// 限制 OpenCV 内部的多线程，避免与外层 OpenMP 产生嵌套并发冲突
	int prev_threads = cv::getNumThreads();
	cv::setNumThreads(1);

	int nn, mm;
	nn = getOptimalDFTSize(2 * wndsize_filter);
	mm = nn;
	float pi = (float)PI;
	int Radius = (wndsize_filter - 1) / 2; /*窗半径*/
	int nr_orig = phase_f.rows;/*原始尺寸rows*/
	int nc_orig = phase_f.cols;/*原始尺寸cols*/
	Mat phase_enlarged;
	copyMakeBorder(phase_f, phase_enlarged, Radius, Radius, Radius, Radius, BORDER_REFLECT);
	int nr_new = phase_enlarged.rows;
	int nc_new = phase_enlarged.cols;

	Mat phase_update(nr_new, nc_new, CV_32FC2, Scalar::all(0));/*相位矩阵转换为复数 (Scheme A)*/

	int i, j;
	for (i = 0; i < nr_new; i++)
	{
		for (j = 0; j < nc_new; j++)
		{
			phase_update.at<Vec2f>(i, j)[0] = cos(phase_enlarged.at<float>(i, j));/*实部*/
			phase_update.at<Vec2f>(i, j)[1] = sin(phase_enlarged.at<float>(i, j));/*虚部*/
		}
	}

	int mn = mm * nn;/*频谱总分辨率*/

	// 优化 3：均值预处理。在进入多线程循环前，先对整张图进行一次均值滤波
	Mat phase_update_blurred = phase_update.clone();
	meanfilter(phase_update_blurred, wndsize_prefilter);

	Mat phase_filtered = Mat::zeros(nr_new, nc_new, CV_32F);

	Mat tempi(wndsize_filter, 1, CV_32F, Scalar::all(0));/*行线性相位网格*/
	Mat tempj(1, wndsize_filter, CV_32F, Scalar::all(0));/*列线性相位网格*/

	for (i = 0; i < wndsize_filter; i++)
	{
		tempi.at<float>(i, 0) = (float)i;
		tempj.at<float>(0, i) = (float)i;
	}

	float phi0 = (float)(2.0 * pi / ((double)mn)); /*CZT变换参数*/

	// 优化 2：预计算 czt2 的 h_dft 矩阵
	int L_val = 1;
	do {
		L_val *= 2;
	} while (L_val < 3 * mm + wndsize_filter);

	Mat h_1d = Mat::zeros(L_val, 1, CV_32FC2);
	for (i = 0; i < 3 * mm; i++) {
		h_1d.at<Vec2f>(i, 0)[0] = cos(phi0 * 0.5f * i * i);
		h_1d.at<Vec2f>(i, 0)[1] = sin(phi0 * 0.5f * i * i);
	}
	for (i = 3 * mm; i < L_val - wndsize_filter + 1; i++) {
		h_1d.at<Vec2f>(i, 0)[0] = 0;
		h_1d.at<Vec2f>(i, 0)[1] = 0;
	}
	for (i = L_val - wndsize_filter + 1; i < L_val; i++) {
		h_1d.at<Vec2f>(i, 0)[0] = cos(phi0 * 0.5f * (L_val - i) * (L_val - i));
		h_1d.at<Vec2f>(i, 0)[1] = sin(phi0 * 0.5f * (L_val - i) * (L_val - i));
	}
	Mat h_1d_trans;
	transpose(h_1d, h_1d_trans); // 1 x L
	dft(h_1d_trans, h_1d_trans, DFT_ROWS); // 1 x L
	
	Mat h_dft_1;
	repeat(h_1d_trans, wndsize_filter, 1, h_dft_1); // wndsize_filter x L

	Mat h_dft_2;
	repeat(h_1d_trans, 3 * mm, 1, h_dft_2); // 3 * mm x L

	// 优化 1：预计算 czt2 的 result 常量矩阵
	Mat result_1d = Mat::zeros(3 * mm, 1, CV_32FC2);
	for (i = 0; i < 3 * mm; i++) {
		result_1d.at<Vec2f>(i, 0)[0] = cos(phi0 * 0.5f * i * i);
		result_1d.at<Vec2f>(i, 0)[1] = -sin(phi0 * 0.5f * i * i);
	}
	Mat result_const_1, result_const_2;
	repeat(result_1d, 1, wndsize_filter, result_const_1); // 3 * mm x wndsize_filter
	repeat(result_1d, 1, 3 * mm, result_const_2);       // 3 * mm x 3 * mm

	int ret;
	std::atomic<bool> parallel_flag(true);
	std::atomic<int> completed_rows(0);
	int total_rows = nr_new - 2 * Radius;
#pragma omp parallel for schedule(guided) private(ret)
	for (int i = Radius; i < nr_new - Radius; i++)
	{
		if (!parallel_flag) continue;

		// 优化 1：重用线程缓冲区，消除高频堆内存申请。
		Mat phase_estimation(wndsize_filter, wndsize_filter, CV_32FC2);
		Mat window_mean_dft = Mat::zeros(nn, nn, CV_32FC2);
		Mat planes_dft[] = { Mat::zeros(nn, nn, CV_32F), Mat::zeros(nn, nn, CV_32F) };
		Mat planes_czt[] = { Mat::zeros(3 * mm, 3 * mm, CV_32F), Mat::zeros(3 * mm, 3 * mm, CV_32F) };
		Mat AA(wndsize_filter, wndsize_filter, CV_32F);
		Mat phase_0_matrix(wndsize_filter, wndsize_filter, CV_32FC2);
		Mat aa(wndsize_filter, wndsize_filter, CV_32FC2);
		Mat phase_0(1, 1, CV_32FC2);
		Mat one(tempi.rows, tempi.cols, CV_32F, Scalar::all(1));
		Mat one_t(tempj.rows, tempj.cols, CV_32F, Scalar::all(1));

		// czt2 第 1 次调用的辅助缓冲区
		Mat g_1 = Mat::zeros(L_val, wndsize_filter, CV_32FC2);
		Mat g_trans_1 = Mat::zeros(wndsize_filter, L_val, CV_32FC2);
		Mat y_1 = Mat::zeros(L_val, wndsize_filter, CV_32FC2);
		Mat W_1 = Mat::zeros(wndsize_filter, wndsize_filter, CV_32FC2);
		Mat tmp_1 = Mat::zeros(wndsize_filter, wndsize_filter, CV_32FC2);
		Mat phase_czt1(3 * mm, wndsize_filter, CV_32FC2);
		Mat phase_czt1_trans = Mat::zeros(wndsize_filter, 3 * mm, CV_32FC2);

		// czt2 第 2 次调用的辅助缓冲区
		Mat g_2 = Mat::zeros(L_val, 3 * mm, CV_32FC2);
		Mat g_trans_2 = Mat::zeros(3 * mm, L_val, CV_32FC2);
		Mat y_2 = Mat::zeros(L_val, 3 * mm, CV_32FC2);
		Mat W_2 = Mat::zeros(wndsize_filter, 3 * mm, CV_32FC2);
		Mat tmp_2 = Mat::zeros(wndsize_filter, 3 * mm, CV_32FC2);
		Mat phase_czt2(3 * mm, 3 * mm, CV_32FC2);
		Mat phase_czt2_trans = Mat::zeros(3 * mm, 3 * mm, CV_32FC2);

		for (int j = Radius; j < nc_new - Radius; j++)
		{
			if (!parallel_flag) continue;
			int k, kk;
			Point peak_loc;
			float fi, fj, fii, fjj;
			float theta0_i, theta0_j;
			
			phase_update(Range(i - Radius, i + Radius + 1), Range(j - Radius, j + Radius + 1)).copyTo(phase_estimation);
			
			// 优化：从整图预均值滤波的结果中直接复制子窗口到 dft 缓冲区的左上角，并将其余部分清零
			window_mean_dft.setTo(Scalar::all(0));
			phase_update_blurred(Range(i - Radius, i + Radius + 1), Range(j - Radius, j + Radius + 1)).copyTo(window_mean_dft(Range(0, wndsize_filter), Range(0, wndsize_filter)));

			/*nn点离散傅里叶变换*/
			dft(window_mean_dft, window_mean_dft);
			split(window_mean_dft, planes_dft);
			magnitude(planes_dft[0], planes_dft[1], planes_dft[0]);
			ret = fftshift2(planes_dft[0]);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			minMaxLoc(planes_dft[0], NULL, NULL, NULL, &peak_loc);
			fi = ((float)(peak_loc.y - nn / 2 - 1)) / ((float)nn);/*此处减2是为了扩大CZT变换的搜索范围*/
			fj = ((float)(peak_loc.x - nn / 2 - 1)) / ((float)nn);

			theta0_i = (float)(2.0 * pi * fi);
			theta0_j = (float)(2.0 * pi * fj);

			ret = czt2(phase_estimation, phase_czt1, 3 * mm, phase_estimation.rows, (double)theta0_i, (double)phi0,
				h_dft_1, result_const_1, g_1, g_trans_1, y_1, W_1, tmp_1);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			
			transpose(phase_czt1, phase_czt1_trans);
			
			ret = czt2(phase_czt1_trans, phase_czt2, 3 * mm, phase_czt1_trans.rows, (double)theta0_j, (double)phi0,
				h_dft_2, result_const_2, g_2, g_trans_2, y_2, W_2, tmp_2);
			if (ret < 0)
			{
				parallel_flag = false;
				continue;
			}
			
			transpose(phase_czt2, phase_czt2_trans);
			split(phase_czt2_trans, planes_czt);
			magnitude(planes_czt[0], planes_czt[1], planes_czt[0]);
			minMaxLoc(planes_czt[0], NULL, NULL, NULL, &peak_loc);

			fii = fi + ((float)(peak_loc.y) / (float)mn);
			fjj = fj + ((float)(peak_loc.x) / (float)mn);/*频谱细化后峰值位置*/

			theta0_i = (float)(2.0 * pi * fii);
			theta0_j = (float)(2.0 * pi * fjj);/*更新细化值*/

			AA = ((tempi * theta0_i) * one_t) + (one * (tempj * theta0_j));
			for (k = 0; k < wndsize_filter; k++)
			{
				for (kk = 0; kk < wndsize_filter; kk++)
				{
					aa.at<Vec2f>(k, kk)[0] = cos(AA.at<float>(k, kk));/*实部*/
					aa.at<Vec2f>(k, kk)[1] = sin(AA.at<float>(k, kk));/*虚部*/
				}
			}
			
			mulSpectrums(phase_update(Range(i - Radius, i + Radius + 1), Range(j - Radius, j + Radius + 1)), aa, phase_0_matrix, 0, true);

			Scalar mean_val = mean(phase_0_matrix);
			phase_0.at<Vec2f>(0, 0)[0] = (float)mean_val[0];
			phase_0.at<Vec2f>(0, 0)[1] = (float)mean_val[1];

			mulSpectrums(phase_0, aa(Range((wndsize_filter - 3) / 2, (wndsize_filter - 1) / 2),
				Range((wndsize_filter - 3) / 2, (wndsize_filter - 1) / 2)), phase_0, 0, false);

			/*转换为相位*/
			phase_filtered.at<float>(i, j) = atan2(phase_0.at<Vec2f>(0, 0)[1], phase_0.at<Vec2f>(0, 0)[0]);
		}

		int current_completed = ++completed_rows;
		if (cb && current_completed % 10 == 0)
		{
			int prog = current_completed * 100 / total_rows;
			if (!cb(prog, "Filtering rows..."))
			{
				parallel_flag = false;
			}
		}

		if (!cb)
		{
			int last_pct = (current_completed - 1) * 100 / total_rows;
			int current_pct = current_completed * 100 / total_rows;
			if ((current_pct / 10 > last_pct / 10) || current_completed == total_rows)
			{
				int print_pct = (current_completed == total_rows) ? 100 : (current_pct / 10 * 10);
#pragma omp critical(stdout_print)
				{
					fprintf(stdout, "process: %d %%\n", print_pct);
				}
			}
		}
	}
	// 恢复 OpenCV 线程设置
	cv::setNumThreads(prev_threads);

	if (!parallel_flag)
	{
		return -2; // 提前返回 -2 表示用户中止
	}
	Mat tmp_out = phase_filtered(Range(Radius, nr_new - Radius), Range(Radius, nc_new - Radius));
	tmp_out.convertTo(phase_filter, phase.type());
	return 0;
}

int Filter::filter_dl(const char* filter_dl_path, const char* tmp_path, const char* dl_model_file, Mat& phase, Mat& phase_filtered, FilterProgressCallback cb)
{
	if (filter_dl_path == NULL ||
		dl_model_file == NULL ||
		tmp_path == NULL||
		phase.rows < 1 ||
		phase.cols < 1 ||
		phase.channels() != 1 ||
		phase.type() != CV_64F)
	{
		fprintf(stderr, "filter_dl(): input check failed!\n\n");
		return -1;
	}
	int nr = phase.rows;
	int nc = phase.cols;
	int ret;
	Utils util;
	Mat cos = Mat::zeros(nr, nc, CV_64F);
	Mat sin = Mat::zeros(nr, nc, CV_64F);
	phase_filtered = Mat::zeros(nr, nc, CV_64F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			cos.at<double>(i, j) = std::cos(phase.at<double>(i, j));
			sin.at<double>(i, j) = std::sin(phase.at<double>(i, j));
		}
	}
	std::string cos_file(tmp_path);
	std::replace(cos_file.begin(), cos_file.end(), '/', '\\');
	std::string sin_file(tmp_path);
	std::replace(sin_file.begin(), sin_file.end(), '/', '\\');
	cos_file.append("\\cos.dat");
	sin_file.append("\\sin.dat");
	ret = util.cvmat2bin(cos_file.c_str(), cos);
	if (return_check(ret, "util.cvmat2bin(*, *)", error_head)) return -1;
	ret = util.cvmat2bin(sin_file.c_str(), sin);
	if (return_check(ret, "util.cvmat2bin(*, *)", error_head)) return -1;
	///////////////////////////创建并调用深度学习滤波进程//////////////////////
	string Filter_dl_path(filter_dl_path);
	std::replace(Filter_dl_path.begin(), Filter_dl_path.end(), '/', '\\');
	std::wstring cmdLine = toWString(Filter_dl_path) + L"\\filter_dl.exe " + 
	                       toWString(dl_model_file) + L" " + 
	                       toWString(cos_file) + L" " + 
	                       toWString(sin_file);

	std::vector<wchar_t> cmdLineCopy(cmdLine.begin(), cmdLine.end());
	cmdLineCopy.push_back(L'\0');

	STARTUPINFO si;
	PROCESS_INFORMATION p_i;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&p_i, sizeof(p_i));
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = FALSE;
	BOOL bRet = ::CreateProcess(
		NULL,           // 不在此指定可执行文件的文件名
		cmdLineCopy.data(), // 命令行参数
		NULL,           // 默认进程安全性
		NULL,           // 默认线程安全性
		FALSE,          // 指定当前进程内的句柄不可以被子进程继承
		CREATE_NEW_CONSOLE, // 为新进程创建一个新的控制台窗口
		NULL,           // 使用本进程的环境变量
		NULL,           // 使用本进程的驱动器和目录
		&si,
		&p_i);
	if (bRet)
	{
		char filter_job_name[512]; filter_job_name[0] = 0;
		time_t tt = std::time(0);
		sprintf(filter_job_name, "FILTER_%lld", tt);
		string filter_job_name_string(filter_job_name);
		HANDLE hd = CreateJobObjectA(NULL, filter_job_name_string.c_str());
		if (hd)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION extLimitInfo;
			extLimitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			BOOL retval = SetInformationJobObject(hd, JobObjectExtendedLimitInformation, &extLimitInfo, sizeof(extLimitInfo));
			if (retval)
			{
				if (p_i.hProcess)
				{
					retval = AssignProcessToJobObject(hd, p_i.hProcess);
				}
			}
		}
		
		bool cancel_requested = false;
		int wait_tick = 0;
		while (true)
		{
			DWORD wait_res = WaitForSingleObject(p_i.hProcess, 100);
			if (wait_res == WAIT_OBJECT_0)
			{
				break;
			}
			wait_tick++;
			if (wait_tick >= 5) // 500ms
			{
				wait_tick = 0;
				if (cb && !cb(50, "深度学习网络模型前向推理中..."))
				{
					cancel_requested = true;
					break;
				}
			}
		}

		if (cancel_requested)
		{
			::TerminateProcess(p_i.hProcess, -1);
			if (hd)
			{
				::CloseHandle(hd);
			}
			::CloseHandle(p_i.hThread);
			::CloseHandle(p_i.hProcess);
			std::remove(cos_file.c_str());
			std::remove(sin_file.c_str());
			return -2;
		}

		if (hd)
		{
			::CloseHandle(hd);
		}
		::CloseHandle(p_i.hThread);
		::CloseHandle(p_i.hProcess);
	}
	else
	{
		fprintf(stderr, "filter_dl(): create filter_dl.exe process failed!\n\n");
		return -1;
	}
	///////////////////////////创建并调用深度学习滤波进程//////////////////////
	cos_file.append(".out");
	sin_file.append(".out");
	Mat cos1, sin1;
	ret = util.bin2cvmat(cos_file.c_str(), cos1);
	if (return_check(ret, "util.bin2cvmat(*, *)", error_head)) return -1;
	ret = util.bin2cvmat(sin_file.c_str(), sin1);
	if (return_check(ret, "util.bin2cvmat(*, *)", error_head)) return -1;
	cos = cos - cos1;
	sin = sin - sin1;
	ComplexMat tmp(cos, sin);
	tmp.GetPhase().copyTo(phase_filtered);
	return 0;
}

int Filter::goldstein_filter_impl(
	Mat& phase,
	Mat& phase_filter,
	double alpha,
	int n_win,
	int n_pad,
	bool parallel,
	FilterProgressCallback cb
) {
	if (phase.cols < 3 ||
		phase.rows < 3 ||
		phase.channels() != 1 ||
		phase.type() != CV_64F ||
		alpha <= 0 ||
		n_win < 5 ||
		n_pad < 0
		)
	{
		fprintf(stderr, "%s(): input check failed!\n\n", parallel ? "Goldstein_filter_parallel" : "Goldstein_filter");
		return -1;
	}
	int n_i = phase.rows;
	int n_j = phase.cols;
	ComplexMat ph;
	
	Utils util;
	util.phase2cos(phase, ph.re, ph.im);

	ComplexMat ph_out(n_i, n_j);
	int n_inc = static_cast<int>(floor(n_win / 4));
	int n_win_i = static_cast<int>(ceil(n_i / n_inc)) - 1;
	int n_win_j = static_cast<int>(ceil(n_j / n_inc)) - 1;
	int x = static_cast<int>(floor(n_win / 2 - 1));
	Mat qua_wnd = Mat::zeros(x + 1, x + 1, CV_64F);
	for (int i = 0; i <= x; i++)
	{
		for (int j = 0; j <= x; j++)
		{
			qua_wnd.at<double>(i, j) = double(i + j);
		}
	}
	qua_wnd.at<double>(0, 0) = 1e-6;
	Mat fliped_wnd;
	flip(qua_wnd, fliped_wnd, 1);
	hconcat(qua_wnd, fliped_wnd, qua_wnd);
	flip(qua_wnd, fliped_wnd, 0);
	vconcat(qua_wnd, fliped_wnd, qua_wnd);

	Mat guasswin;
	/*
	// 历史遗留的 sigma = 1.2 手工高斯核计算（在原单线程版本中被下方的 GenerateGaussMask(..., 1.0) 覆盖而未生效，在此注释保留以备参考）
	Mat guasswin_legacy = Mat::zeros(7, 1, CV_64F);
	double val[] = { 0.0439369336234074, 0.249352208777296, 0.706648277857716, 1, 0.706648277857716, 0.249352208777296, 0.0439369336234074 };
	memcpy(guasswin_legacy.data, val, sizeof(double) * 7);
	Mat guasswin_t;
	transpose(guasswin_legacy, guasswin_t);
	guasswin_legacy = guasswin_legacy * guasswin_t;
	*/
	GenerateGaussMask(guasswin, 7, 7, 1.0);

	int n_win_ex = n_win + n_pad;
	std::atomic<bool> parallel_flag(true);
	std::atomic<int> completed_wins(0);
	int total_wins = n_win_i;
	int step = std::max(1, total_wins / 100);

	for (int ix1 = 1; ix1 <= n_win_i; ix1++)
	{
		if (!parallel_flag) break;

		Mat wf, wind_func, tmp1, tmp2;
		qua_wnd.copyTo(wind_func);
		int i1, i2, i_shift;
		wind_func.copyTo(wf);
		i1 = (ix1 - 1) * n_inc + 1;
		i2 = i1 + n_win - 1;
		if (i2 > n_i)
		{
			i_shift = i2 - n_i;
			i2 = n_i;
			i1 = n_i - n_win + 1;
			tmp1 = Mat::zeros(i_shift, n_win, CV_64F);
			wf(Range(0, n_win - i_shift), Range(0, wf.cols)).copyTo(tmp2);
			vconcat(tmp1, tmp2, wf);
		}

#pragma omp parallel for schedule(guided) if(parallel)
		for (int ix2 = 1; ix2 <= n_win_j; ix2++)
		{
			if (!parallel_flag) continue;

			ComplexMat ph_bit(n_win_ex, n_win_ex);
			ComplexMat temp, temp1, temp2, fft_out, ph_filt;
			Mat wf2, tmp, tmp11, tmp22, H;
			int j_shift, j1, j2; 
			int ret;
			double median = 1.0;

			wf.copyTo(wf2);
			j1 = (ix2 - 1) * n_inc + 1;
			j2 = j1 + n_win - 1;
			if (j2 > n_j)
			{
				j_shift = j2 - n_j;
				j2 = n_j;
				j1 = n_j - n_win + 1;
				tmp11 = Mat::zeros(n_win, j_shift, CV_64F);
				wf2(Range(0, wf2.rows), Range(0, n_win - j_shift)).copyTo(tmp22);
				hconcat(tmp11, tmp22, wf2);
			}

			if (wf2.cols != n_win || wf2.rows != n_win)
			{
				fprintf(stderr, "%s(): wf2.size and n_win mismatch, please check to make sure n_win is even!\n\n", 
					parallel ? "Goldstein_filter_parallel" : "Goldstein_filter");
				parallel_flag = false;
				continue;
			}

			temp = ph(cv::Range(i1 - 1, i2), cv::Range(j1 - 1, j2));
			ret = ph_bit.SetValue(cv::Range(0, n_win), cv::Range(0, n_win), temp);
			if (ret < 0) { parallel_flag = false; continue; }

			ret = util.fft2(ph_bit, fft_out);
			if (ret < 0) { parallel_flag = false; continue; }

			H = fft_out.GetMod();
			ret = fftshift2(H);
			if (ret < 0) { parallel_flag = false; continue; }

			GaussianFilter(H, H, guasswin);

			ret = util.ifftshift(H);
			if (ret < 0) { parallel_flag = false; continue; }

			H.copyTo(tmp);
			tmp = tmp.reshape(0, 1);
			cv::sort(tmp, tmp, cv::SORT_EVERY_ROW | cv::SORT_ASCENDING);
			if (tmp.cols % 2 == 1)
			{
				median = tmp.at<double>(0, int(tmp.cols / 2));
			}
			else
			{
				median = tmp.at<double>(0, int(tmp.cols / 2) - 1) + tmp.at<double>(0, int(tmp.cols / 2));
				median = median / 2.0;
			}
			if (fabs(median) > 1e-8)
			{
				H = H / median;
			}
			cv::pow(H, alpha, H);
			fft_out = fft_out * H;

			ret = util.ifft2(fft_out, temp);
			if (ret < 0) { parallel_flag = false; continue; }
			Mat re, im;
			re = temp.GetRe();
			im = temp.GetIm();
			temp1 = temp(Range(0, n_win), Range(0, n_win));
			ph_filt = temp1 * wf2;
			temp = ph_out(Range(i1 - 1, i2), Range(j1 - 1, j2)) + ph_filt;
			ret = ph_out.SetValue(Range(i1 - 1, i2), Range(j1 - 1, j2), temp);
		}

		int current = ++completed_wins;
		if (cb && current % step == 0)
		{
			int progress = current * 100 / total_wins;
			if (!cb(progress, "Goldstein filtering..."))
			{
				parallel_flag = false;
			}
		}
	}
	if (!parallel_flag) return -2;
	ph_out.GetPhase().copyTo(phase_filter);
	return 0;
}

int Filter::Goldstein_filter(Mat& phase, Mat& phase_filter, double alpha, int n_win, int n_pad, FilterProgressCallback cb)
{
	return goldstein_filter_impl(phase, phase_filter, alpha, n_win, n_pad, false, cb);
}

int Filter::Goldstein_filter_parallel(Mat& phase, Mat& phase_filter, double alpha, int n_win, int n_pad, FilterProgressCallback cb)
{
	return goldstein_filter_impl(phase, phase_filter, alpha, n_win, n_pad, true, cb);
}

// 按二维高斯函数实现高斯滤波
int Filter::GaussianFilter(cv::Mat& src, cv::Mat& dst, const cv::Mat& window) 
{
	int hh = (window.rows - 1) / 2;
	int hw = (window.cols - 1) / 2;
	Mat Dst = cv::Mat::zeros(src.size(), src.type());
	//边界填充
	cv::Mat Newsrc;
	cv::copyMakeBorder(src, Newsrc, hh, hh, hw, hw, cv::BORDER_REPLICATE);//边界复制
	Dst.setTo(0);
	//高斯滤波
	for (int i = hh; i < src.rows + hh; ++i) {
		for (int j = hw; j < src.cols + hw; ++j) {
			double sum = 0.0;

			for (int r = -hh; r <= hh; ++r) {
				for (int c = -hw; c <= hw; ++c) {
					sum = sum + Newsrc.ptr<double>(i + r)[j + c] * window.ptr<double>(r + hh)[c + hw];
				}
			}
			Dst.ptr<double>(i - hh)[j - hw] = sum;
		}
	}
	Dst.copyTo(dst);
	return 0;
}

int Filter::GenerateGaussMask(Mat& Mask, int window_height, int window_width, double sigma)
{
	Mask.create(window_height, window_width, CV_64F);
	int h = window_height;
	int w = window_width;
	int center_h = (h - 1) / 2;
	int center_w = (w - 1) / 2;
	double sum = 0.0;
	double x, y;
	for (int i = 0; i < h; ++i) {
		y = pow(i - center_h, 2);
		for (int j = 0; j < w; ++j) {
			x = pow(j - center_w, 2);
			//因为最后都要归一化的，常数部分可以不计算，也减少了运算量
			double g = exp(-(x + y) / (2 * sigma * sigma));
			Mask.ptr<double>(i)[j] = g;
			sum += g;
		}
	}
	Mask = Mask / sum;
	return 0;
}
