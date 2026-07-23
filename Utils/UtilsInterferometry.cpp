#include "stdafx.h"
#include "..\include\Utils.h"
#include "Eigen/Dense"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <vector>

using namespace cv;
using namespace std;
int Utils::homogeneous_selection_and_phase_linking(
	vector<ComplexMat>& slc_stack,
	vector<ComplexMat>& slc_stack_filtered,
	int test_wndsize, int est_wndsize
)
{
	if (slc_stack.size() < 2 ||
		test_wndsize < 5 ||
		est_wndsize < 3 ||
		test_wndsize < est_wndsize ||
		test_wndsize % 2 != 1 ||
		est_wndsize % 2 != 1
		)
	{
		fprintf(stderr, "homogeneous_selection_and_phase_linking(): input check failed!\n");
		return -1;
	}
	if (slc_stack[0].type() != CV_32F)
	{
		fprintf(stderr, "homogeneous_selection_and_phase_linking(): input check failed!\n");
		return -1;
	}
	int n_images = static_cast<int>(slc_stack.size());
	//检查各图像数据尺寸是否一致
	int nr = slc_stack[0].GetRows();
	int nc = slc_stack[0].GetCols();
	for (int i = 0; i < n_images; i++)
	{
		if (slc_stack[i].GetCols() != nc || slc_stack[i].GetRows() != nr)
		{
			fprintf(stderr, "homogeneous_selection_and_phase_linking(): stack imagesize mismatch!\n");
			return -1;
		}
	}
	//将待处理数据拷贝至结果数据
	for (int i = 0; i < n_images; i++)
	{
		slc_stack_filtered.push_back(slc_stack[i]);
	}
	int radius_test = (test_wndsize - 1) / 2;
	int radius_estimation = (est_wndsize - 1) / 2;
	//扩展数据边缘
	 for (int i = 0; i < n_images; i++)
	{
		 cv::copyMakeBorder(slc_stack[i].re, slc_stack[i].re, radius_test + radius_estimation,
			 radius_test + radius_estimation, radius_test + radius_estimation, radius_test + radius_estimation,
			 BORDER_REFLECT);
		 cv::copyMakeBorder(slc_stack[i].im, slc_stack[i].im, radius_test + radius_estimation,
			 radius_test + radius_estimation, radius_test + radius_estimation, radius_test + radius_estimation,
			 BORDER_REFLECT);
	}
	

	double Look = 5.5;
	double thresh = 13.3;
	double rho = 0.8409;
	//预先计算第一块相关矩阵
	vector<ComplexMat> pre_covariance;
	ComplexMat temp((2 * radius_test + 1) * (nc + 2 * radius_test), 4);
	int x = n_images * (n_images - 1) / 2;
	for (int i = 0; i < x; i++)
	{
		pre_covariance.push_back(temp);
	}
#pragma omp parallel for schedule(guided)
	for (int mm = 0; mm < n_images; mm++)
	{
		for (int nn = mm + 1; nn < n_images; nn++)
		{
			int count = (n_images - 1 + n_images - mm) * mm / 2 + nn - mm - 1;
			ComplexMat Cov_t;
			for (int i = radius_estimation; i < 2 * radius_test + 1 + radius_estimation; i++)
			{
				for (int j = radius_estimation; j < nc + 2 * radius_test + radius_estimation; j++)
				{
					ComplexMat Cov(2, 2); ComplexMat vec(2, 1); ComplexMat vec_h, tmp_vec;
					for (int ii = i - radius_estimation; ii < i + 1 + radius_estimation; ii++)
					{
						for (int jj = j - radius_estimation; jj < j + 1 + radius_estimation; jj++)
						{
							vec.re.at<double>(0, 0) = slc_stack[mm].re.at<float>(ii, jj);
							vec.im.at<double>(0, 0) = slc_stack[mm].im.at<float>(ii, jj);
							vec.re.at<double>(1, 0) = slc_stack[nn].re.at<float>(ii, jj);
							vec.im.at<double>(1, 0) = slc_stack[nn].im.at<float>(ii, jj);
							vec_h = vec.transpose();
							vec.mul(vec_h, tmp_vec);
							Cov = Cov + tmp_vec;
						}
					}
					Cov.reshape(1, 4, Cov_t);
					int row = (i - radius_estimation) * (nc + 2 * radius_test) + j - radius_estimation;
					pre_covariance[count].SetValue(cv::Range(row, row + 1), cv::Range(0, 4), Cov_t);
				}
			}
		}
	}

	Mat coherence = Mat::zeros(nr, nc, CV_64F);
	Mat phase = Mat::zeros(nr, nc, CV_64F);
	Mat gamma = Mat::zeros(nr, nc, CV_64F);
	Mat homo_num = Mat::zeros(nr, nc, CV_32S);
	Mat homo_index = Mat::zeros(nr * nc, test_wndsize * test_wndsize, CV_16S);
	int total_count = 0;
	//预先对第一行元素的进行同质点选取和phase-linking
#pragma omp parallel for schedule(guided)
	for (int j = radius_test + radius_estimation; j < nc + radius_test + radius_estimation; j++)
	{
		ComplexMat Covariance, eigenvector;
		Mat Covariance_x1 = Mat::zeros(n_images, n_images, CV_64F);
		Mat Covariance_x2 = Mat::zeros(n_images, n_images, CV_64F);
		Mat eigenvalue;
		Covariance.re = Mat::eye(n_images, n_images, CV_64F);
		Covariance.im = Mat::eye(n_images, n_images, CV_64F);
		int count = 0;
		int i = radius_estimation + radius_test;
		for (int mm = 0; mm < n_images; mm++)
		{
			for (int nn = mm + 1; nn < n_images; nn++)
			{
				//选取中心点相关矩阵
				ComplexMat Cov_center, Cov_other, vec(2, 1), tmp;
				double a_re, a_im, b_re, b_im;
				int row = (i - radius_estimation) * (nc + 2 * radius_test) + j - radius_estimation;
				Cov_center = pre_covariance[count](cv::Range(row, row + 1), cv::Range(0, 4));
				Cov_center.reshape(2, 2, Cov_center);
				double det_center = Cov_center.determinant().real();
				a_re = slc_stack[mm].re.at<float>(i, j);
				a_im = slc_stack[mm].im.at<float>(i, j);
				b_re = slc_stack[nn].re.at<float>(i, j);
				b_im = slc_stack[nn].im.at<float>(i, j);
				Covariance.re.at<double>(mm, nn) += a_re * b_re + a_im * b_im;
				Covariance.im.at<double>(mm, nn) += a_im * b_re - a_re * b_im;
				Covariance_x1.at<double>(mm, nn) += a_re * a_re + a_im * a_im;
				Covariance_x2.at<double>(mm, nn) += b_re * b_re + b_im * b_im;
				Mat mask = Mat::zeros(test_wndsize, test_wndsize, CV_16S);
				mask.at<short>(radius_test, radius_test) = 1;
				//homo_num.at<int>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) += 1;
				//测试窗口内同质像元
				for (int ii = i - radius_test; ii < i + 1 + radius_test; ii++)
				{
					for (int jj = j - radius_test; jj < j + 1 + radius_test; jj++)
					{
						int row = (ii - radius_estimation) * (nc + 2 * radius_test) + jj - radius_estimation;
						Cov_other = pre_covariance[count](cv::Range(row, row + 1), cv::Range(0, 4));
						Cov_other.reshape(2, 2, Cov_other);
						double det_other = Cov_other.determinant().real();
						double det_sum = (Cov_other + Cov_center).determinant().real();
						double lnQ = Look * (2 * 2 * log(2) + log(det_center) + log(det_other) - 2.0 * log(det_sum));
						if (-2.0 * rho * lnQ <= thresh)
						{
							a_re = slc_stack[mm].re.at<float>(ii, jj);
							a_im = slc_stack[mm].im.at<float>(ii, jj);
							b_re = slc_stack[nn].re.at<float>(ii, jj);
							b_im = slc_stack[nn].im.at<float>(ii, jj);
							Covariance.re.at<double>(mm, nn) += a_re * b_re + a_im * b_im;
							Covariance.im.at<double>(mm, nn) += a_im * b_re - a_re * b_im;
							Covariance_x1.at<double>(mm, nn) += a_re * a_re + a_im * a_im;
							Covariance_x2.at<double>(mm, nn) += b_re * b_re + b_im * b_im;
							int xx = ii - i + radius_test;
							int yy = jj - j + radius_test;
							mask.at<short>(xx, yy) = 1;
							//homo_num.at<int>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) += 1;
						}
					}
				}
				int ix = i - radius_estimation - radius_test;
				int jx = j - radius_estimation - radius_test;
				cv::transpose(mask, mask);
				mask = mask.reshape(1, 1);
				mask.copyTo(homo_index(cv::Range(ix* nc + jx, ix* nc + jx + 1), cv::Range(0, test_wndsize* test_wndsize)));
				count++;
			}
		}
		for (int mm = 0; mm < n_images; mm++)
		{
			for (int nn = mm + 1; nn < n_images; nn++)
			{
				Covariance.re.at<double>(mm, nn) = Covariance.re.at<double>(mm, nn) / sqrt(Covariance_x1.at<double>(mm, nn) *
					Covariance_x2.at<double>(mm, nn));
				Covariance.im.at<double>(mm, nn) = Covariance.im.at<double>(mm, nn) / sqrt(Covariance_x1.at<double>(mm, nn) *
					Covariance_x2.at<double>(mm, nn));
				Covariance.re.at<double>(nn, mm) = Covariance.re.at<double>(mm, nn);
				Covariance.im.at<double>(nn, mm) = -Covariance.im.at<double>(mm, nn);
			}
		}
		//保存相位和相关系数（调试用）
		double re = Covariance.re.at<double>(0, 1);
		double im = Covariance.im.at<double>(0, 1);
		phase.at<double>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
			atan2(im, re);
		coherence.at<double>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
			sqrt(im * im + re * re);
		//phase-linking
		//int ret = HermitianEVD(Covariance, eigenvalue, eigenvector);
		//if (ret == 0)
		//{
		//	//计算goodness of fit
		//	/*for (int mm = 0; mm < n_images; mm++)
		//	{
		//		for (int nn = mm + 1; nn < n_images; nn++)
		//		{
		//			int rows = i - radius_estimation - radius_test;
		//			int cols = j - radius_estimation - radius_test;
		//			double re2, im2, re3, im3, phi_ik, phi_ik2;
		//			re = Covariance.re.at<double>(mm, nn);
		//			im = Covariance.im.at<double>(mm, nn);
		//			phi_ik = atan2(im, re);

		//			re = eigenvector.re.at<double>(mm, 0);;
		//			im = eigenvector.im.at<double>(mm, 0);
		//			re2 = eigenvector.re.at<double>(nn, 0);
		//			im2 = eigenvector.im.at<double>(nn, 0);

		//			re3 = re * re2 + im * im2;
		//			im3 = im * re2 - re * im2;
		//			phi_ik2 = atan2(im3, re3);
		//			gamma.at<double>(rows, cols) += cos(phi_ik - phi_ik2);
		//		}
		//	}*/
		//	for (int kk = 0; kk < n_images; kk++)
		//	{
		//		slc_stack_filtered[kk].re.at<float>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
		//			eigenvector.re.at<double>(kk, 0);
		//		slc_stack_filtered[kk].im.at<float>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
		//			eigenvector.im.at<double>(kk, 0);
		//	}
		//}
		
	}
	total_count++;
	printf("\r估计进度：%lf%%", double(total_count) / double(nr) * 100.0);
	fflush(stdout);
	//对剩余行进行循环处理


	for (int i = radius_estimation + radius_test + 1; i < nr + radius_estimation + radius_test; i++)
	{
		//更新相关矩阵块
#pragma omp parallel for schedule(guided)
		for (int mm = 0; mm < n_images; mm++)
		{
			for (int nn = mm + 1; nn < n_images; nn++)
			{
				int count = (n_images - 1 + n_images - mm) * mm / 2 + nn - mm - 1;
				//删除已经处理的第一行并将剩余部分上移一行
				pre_covariance[count].SetValue(cv::Range(0, (2 * radius_test)* (nc + 2 * radius_test)), cv::Range(0, 4),
					pre_covariance[count](cv::Range(nc + 2 * radius_test, (2 * radius_test + 1)* (nc + 2 * radius_test)),
						cv::Range(0, 4)));
				//重新计算新的一行并加入到相关矩阵块
				int i_last_row = i + radius_test;//新的一行行号
				for (int j = radius_estimation; j < nc + 2 * radius_test + radius_estimation; j++)
				{
					ComplexMat Cov(2, 2); ComplexMat Cov_t;
					for (int ii = i_last_row - radius_estimation; ii < i_last_row + 1 + radius_estimation; ii++)
					{
						for (int jj = j - radius_estimation; jj < j + 1 + radius_estimation; jj++)
						{
							ComplexMat vec(2, 1); ComplexMat vec_h, tmp_vec;
							vec.re.at<double>(0, 0) = slc_stack[mm].re.at<float>(ii, jj);
							vec.im.at<double>(0, 0) = slc_stack[mm].im.at<float>(ii, jj);
							vec.re.at<double>(1, 0) = slc_stack[nn].re.at<float>(ii, jj);
							vec.im.at<double>(1, 0) = slc_stack[nn].im.at<float>(ii, jj);
							vec_h = vec.transpose();
							vec.mul(vec_h, tmp_vec);
							Cov = Cov + tmp_vec;
						}
					}
					Cov.reshape(1, 4, Cov_t); 
					int row = (2 * radius_test) * (nc + 2 * radius_test) + j - radius_estimation;
					pre_covariance[count].SetValue(cv::Range(row, row + 1), cv::Range(0, 4), Cov_t);
				}
			}
		}


		//处理新的一行
#pragma omp parallel for schedule(guided)
		for (int j = radius_test + radius_estimation; j < nc + radius_test + radius_estimation; j++)
		{
			ComplexMat Covariance, eigenvector;
			Mat Covariance_x1 = Mat::zeros(n_images, n_images, CV_64F);
			Mat Covariance_x2 = Mat::zeros(n_images, n_images, CV_64F);
			Mat eigenvalue;
			Covariance.re = Mat::eye(n_images, n_images, CV_64F);
			Covariance.im = Mat::eye(n_images, n_images, CV_64F);
			int count = 0;
			//int i = radius_estimation + radius_test;
			for (int mm = 0; mm < n_images; mm++)
			{
				for (int nn = mm + 1; nn < n_images; nn++)
				{
					//选取中心点相关矩阵
					ComplexMat Cov_center, Cov_other, vec(2, 1), tmp;
					double a_re, a_im, b_re, b_im;
					/*注意此处总是处理中间行，因为相关矩阵块已经更新了*/
					int row = radius_test * (nc + 2 * radius_test) + j - radius_estimation;
					Cov_center = pre_covariance[count](cv::Range(row, row + 1), cv::Range(0, 4));
					Cov_center.reshape(2, 2, Cov_center);
					double det_center = Cov_center.determinant().real();
					a_re = slc_stack[mm].re.at<float>(i, j);
					a_im = slc_stack[mm].im.at<float>(i, j);
					b_re = slc_stack[nn].re.at<float>(i, j);
					b_im = slc_stack[nn].im.at<float>(i, j);
					Covariance.re.at<double>(mm, nn) += a_re * b_re + a_im * b_im;
					Covariance.im.at<double>(mm, nn) += a_im * b_re - a_re * b_im;
					Covariance_x1.at<double>(mm, nn) += a_re * a_re + a_im * a_im;
					Covariance_x2.at<double>(mm, nn) += b_re * b_re + b_im * b_im;
					Mat mask = Mat::zeros(test_wndsize, test_wndsize, CV_16S);
					mask.at<short>(radius_test, radius_test) = 1;
					//homo_num.at<int>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) += 1;
					//测试窗口内同质像元
					for (int ii = i - radius_test; ii < i + 1 + radius_test; ii++)
					{
						for (int jj = j - radius_test; jj < j + 1 + radius_test; jj++)
						{
							int row = (ii - i + radius_test) * (nc + 2 * radius_test) + jj - radius_estimation;
							Cov_other = pre_covariance[count](cv::Range(row, row + 1), cv::Range(0, 4));
							Cov_other.reshape(2, 2, Cov_other);
							double det_other = Cov_other.determinant().real();
							double det_sum = (Cov_other + Cov_center).determinant().real();
							double lnQ = Look * (2 * 2 * log(2) + log(det_center) + log(det_other) - 2.0 * log(det_sum));
							if (-2.0 * rho * lnQ <= thresh)
							{
								a_re = slc_stack[mm].re.at<float>(ii, jj);
								a_im = slc_stack[mm].im.at<float>(ii, jj);
								b_re = slc_stack[nn].re.at<float>(ii, jj);
								b_im = slc_stack[nn].im.at<float>(ii, jj);
								Covariance.re.at<double>(mm, nn) += a_re * b_re + a_im * b_im;
								Covariance.im.at<double>(mm, nn) += a_im * b_re - a_re * b_im;
								Covariance_x1.at<double>(mm, nn) += a_re * a_re + a_im * a_im;
								Covariance_x2.at<double>(mm, nn) += b_re * b_re + b_im * b_im;
								int xx = ii - i + radius_test;
								int yy = jj - j + radius_test;
								mask.at<short>(xx, yy) = 1;
								//homo_num.at<int>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) += 1;
							}
						}
					}
					int ix = i - radius_estimation - radius_test;
					int jx = j - radius_estimation - radius_test;
					cv::transpose(mask, mask);
					mask = mask.reshape(1, 1);
					mask.copyTo(homo_index(cv::Range(ix * nc + jx, ix * nc + jx + 1), cv::Range(0, test_wndsize * test_wndsize)));
					count++;
				}
			}
			for (int mm = 0; mm < n_images; mm++)
			{
				for (int nn = mm + 1; nn < n_images; nn++)
				{
					Covariance.re.at<double>(mm, nn) = Covariance.re.at<double>(mm, nn) / sqrt(Covariance_x1.at<double>(mm, nn) *
						Covariance_x2.at<double>(mm, nn));
					Covariance.im.at<double>(mm, nn) = Covariance.im.at<double>(mm, nn) / sqrt(Covariance_x1.at<double>(mm, nn) *
						Covariance_x2.at<double>(mm, nn));
					Covariance.re.at<double>(nn, mm) = Covariance.re.at<double>(mm, nn);
					Covariance.im.at<double>(nn, mm) = -Covariance.im.at<double>(mm, nn);
				}
			}

			//保存相位和相关系数（调试用）
			double re = Covariance.re.at<double>(0, 1);
			double im = Covariance.im.at<double>(0, 1);
			phase.at<double>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
				atan2(im, re);
			coherence.at<double>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
				sqrt(im * im + re * re);
			//phase-linking
			//int ret = HermitianEVD(Covariance, eigenvalue, eigenvector);
			//if (0)
			//{
			//	//计算goodness of fit
			//	/*for (int mm = 0; mm < n_images; mm++)
			//	{
			//		for (int nn = mm + 1; nn < n_images; nn++)
			//		{
			//			int rows = i - radius_estimation - radius_test;
			//			int cols = j - radius_estimation - radius_test;
			//			double re2, im2, re3, im3, phi_ik, phi_ik2;
			//			re = Covariance.re.at<double>(mm, nn);
			//			im = Covariance.im.at<double>(mm, nn);
			//			phi_ik = atan2(im, re);

			//			re = eigenvector.re.at<double>(mm, 0);;
			//			im = eigenvector.im.at<double>(mm, 0);
			//			re2 = eigenvector.re.at<double>(nn, 0);
			//			im2 = eigenvector.im.at<double>(nn, 0);

			//			re3 = re * re2 + im * im2;
			//			im3 = im * re2 - re * im2;
			//			phi_ik2 = atan2(im3, re3);
			//			gamma.at<double>(rows, cols) += cos(phi_ik - phi_ik2);
			//		}
			//	}*/
			//	for (int kk = 0; kk < n_images; kk++)
			//	{
			//		slc_stack_filtered[kk].re.at<float>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
			//			eigenvector.re.at<double>(kk, 0);
			//		slc_stack_filtered[kk].im.at<float>(i - radius_estimation - radius_test, j - radius_estimation - radius_test) =
			//			eigenvector.im.at<double>(kk, 0);
			//	}
			//}
		}
		total_count++;
		printf("\r估计进度：%lf%%", double(total_count) / double(nr) * 100.0);
		fflush(stdout);
	}
	//gamma = gamma / ((n_images - 1) * n_images / 2);
	//homo_index.convertTo(homo_index, CV_64F);
	//cvmat2bin("E:\\working_dir\\papers\\homogeneous_selection\\gamma.bin", gamma);
	// FormatConversion conversion;
	// conversion.creat_new_h5("E:\\working_dir\\papers\\homogeneous_selection\\Sentinel\\sceneA\\singlepol\\homo_index.h5");
	// conversion.write_array_to_h5("E:\\working_dir\\papers\\homogeneous_selection\\Sentinel\\sceneA\\singlepol\\homo_index.h5",
	// 	"homo_index", homo_index);
	// cvmat2bin("E:\\working_dir\\papers\\homogeneous_selection\\Sentinel\\sceneA\\singlepol\\phase.bin", phase);
	// cvmat2bin("E:\\working_dir\\papers\\homogeneous_selection\\Sentinel\\sceneA\\singlepol\\coherence.bin", coherence);
	return 0;
}

int Utils::SKP_decomposition(
	ComplexMat& inputMat,
	int nr1,
	int nc1,
	int nr2,
	int nc2,
	vector<ComplexMat>& outputMat1,
	vector<ComplexMat>& outputMat2
)
{
	assert(!inputMat.isEmpty() &&
		inputMat.type() == CV_64F &&
		(nr1 * nr2 == inputMat.GetRows()) &&
		(nc1 * nc2 == inputMat.GetCols()));
	
	//完成复矩阵reshape
	int nr_new = nr1 * nc1;
	int nc_new = nr2 * nc2;
	ComplexMat R(nr1 * nc1, nr2 * nc2);
	Mat tmp_re, tmp_im;
	int count = 0;
	for (int j = 0; j < nc1; j++)
	{
		for (int i = 0; i < nr1; i++)
		{
			inputMat.re(cv::Range(i * nr2, (i + 1) * nr2), cv::Range(j * nc2, (j + 1) * nc2)).copyTo(tmp_re);
			cv::transpose(tmp_re, tmp_re);
			tmp_re = tmp_re.reshape(1, 1);
			tmp_re.copyTo(R.re(cv::Range(count, count + 1), cv::Range(0, nr2 * nc2)));

			inputMat.im(cv::Range(i * nr2, (i + 1) * nr2), cv::Range(j * nc2, (j + 1) * nc2)).copyTo(tmp_im);
			cv::transpose(tmp_im, tmp_im);
			tmp_im = tmp_im.reshape(1, 1);
			tmp_im.copyTo(R.im(cv::Range(count, count + 1), cv::Range(0, nr2 * nc2)));

			count++;
		}
	}
	//cout << R.re << endl;
	//完成奇异值分解
	Eigen::MatrixXcd x(nr_new, nc_new);
	complex<double> d;
	for (int i = 0; i < nr_new; i++)
	{
		for (int j = 0; j < nc_new; j++)
		{
			d.real(R.re.at<double>(i, j));
			d.imag(R.im.at<double>(i, j));
			x(i, j) = d;
		}
	}
	Eigen::JacobiSVD<Eigen::MatrixXcd> svd(x, Eigen::ComputeThinU | Eigen::ComputeThinV);
	svd.compute(x);
	Eigen::MatrixXcd U = svd.matrixU();
	Eigen::MatrixXcd V = svd.matrixV();
	Eigen::VectorXd singular_value = svd.singularValues();
	int M = static_cast<int>(singular_value.size());
	
	for (int i = 0; i < M; i++)
	{
		ComplexMat U_matrix(nr_new, 1), V_matrix(nc_new, 1);
		for (int j = 0; j < nr_new; j++)
		{
			U_matrix.re.at<double>(j, 0) = U(j, i).real();
			U_matrix.im.at<double>(j, 0) = U(j, i).imag();
		}
		for (int j = 0; j < nc_new; j++)
		{
			V_matrix.re.at<double>(j, 0) = V(j, i).real();
			V_matrix.im.at<double>(j, 0) = V(j, i).imag();
		}
		U_matrix = U_matrix * sqrt(singular_value(i, 0));
		V_matrix = V_matrix * sqrt(singular_value(i, 0));
		U_matrix.reshape(nr1, nc1, U_matrix);
		V_matrix.reshape(nr2, nc2, V_matrix);
		U_matrix = U_matrix.transpose(false);
		V_matrix = V_matrix.transpose();
		outputMat1.push_back(U_matrix);
		outputMat2.push_back(V_matrix);
	}
	return 0;
}

int Utils::homogeneous_test(const Mat& pixel1, const Mat& pixel2, int* homo_flag, double alpha, const char* method)
{
	if (pixel1.cols != 1 ||
		pixel1.rows < 5 ||
		pixel1.type() != CV_64F ||
		pixel1.rows != pixel2.rows ||
		pixel1.cols != pixel2.cols ||
		pixel2.type() != CV_64F ||
		homo_flag == NULL ||
		method == NULL
		)
	{
		fprintf(stderr, "homogeneous_test(): input check failed!\n");
		return -1;
	}

	//Kolmogorov-Smirnov检验
	/*
	 alpha      0.20	0.15	0.10	0.05	0.025	0.01	0.005	0.001
     c(alpha)   1.073	1.138	1.224	1.358	1.48	1.628	1.731	1.949
	*/
	if (strcmp(method, "KS") == 0)
	{
		Mat p1, p2, cdf1, cdf2;
		double thresh;
		pixel1.copyTo(p1); pixel2.copyTo(p2);
		int N = p1.rows;
		cv::sort(p1, p1, cv::SORT_EVERY_COLUMN + cv::SORT_ASCENDING);
		cv::sort(p2, p2, cv::SORT_EVERY_COLUMN + cv::SORT_ASCENDING);
		if (p1.at<double>(N - 2, 0) <= p2.at<double>(0, 0) || p2.at<double>(N - 2, 0) <= p1.at<double>(0, 0))
		{
			*homo_flag = -1;
			return 0;
		}
		//确定threshold
		if (fabs(alpha - 0.2) < 0.01)
		{
			thresh = sqrt(2 / (double)N) * 1.073;
		}
		else if (fabs(alpha - 0.15) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.138;
		}
		else if (fabs(alpha - 0.1) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.224;
		}
		else if (fabs(alpha - 0.05) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.358;
		}
		else if (fabs(alpha - 0.025) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.48;
		}
		else if (fabs(alpha - 0.01) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.628;
		}
		else if (fabs(alpha - 0.005) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.731;
		}
		else if (fabs(alpha - 0.001) < 0.0001)
		{
			thresh = sqrt(2 / (double)N) * 1.949;
		}
		else
		{
			thresh = sqrt(2 / (double)N) * 1.358;
		}
		//计算C.D.F最大间距
		double Dmax = 0.0, tmp;
		int front_1 = 0, front_2 = 0;
		if (p1.at<double>(0, 0) > p2.at<double>(0, 0))
		{
			for (int i = 0; i < N - 1;i++)
			{
				if (p2.at<double>(i, 0) <= p1.at<double>(0, 0) && p2.at<double>(i + 1, 0) >= p1.at<double>(0, 0))
				{
					front_2 = i;
					break;
				}
			}
		}
		else
		{
			for (int i = 0; i < N - 1; i++)
			{
				if (p1.at<double>(i, 0) <= p2.at<double>(0, 0) && p1.at<double>(i + 1, 0) >= p2.at<double>(0, 0))
				{
					front_1 = i;
					break;
				}
			}
		}
		while (front_1 < N && front_2 < N)
		{
			tmp = fabs((double)front_1 / (double)N - (double)front_2 / (double)N);
			Dmax = Dmax > tmp ? Dmax : tmp;
			if (front_1 >= N - 1 || front_2 >= N - 1)
			{
				break;
			}
			if (p1.at<double>(front_1 + 1, 0) < p2.at<double>(front_2 + 1, 0)) front_1++;
			else if (p1.at<double>(front_1 + 1, 0) > p2.at<double>(front_2 + 1, 0)) front_2++;
			else
			{
				front_1++; front_2++;
			}
		}
		if (Dmax > thresh) *homo_flag = -1;
		else *homo_flag = 0;
	}

	//Anderson-Darling 检验
	else if (strcmp(method, "AD")== 0)
	{
		/*
		alpha = 0.01, AD_inf = 3.857;
		alpha = 0.05, AD_inf = 2.492;
		alpha = 0.1,  AD_inf = 1.933;
		*/
		Mat p1, p2, p;
		double thresh;
		pixel1.copyTo(p1); pixel2.copyTo(p2);
		cv::vconcat(p1, p2, p);
		int n = p1.rows; int N = 2 * n;
		cv::sort(p1, p1, cv::SORT_EVERY_COLUMN + cv::SORT_ASCENDING);
		cv::sort(p2, p2, cv::SORT_EVERY_COLUMN + cv::SORT_ASCENDING);
		cv::sort(p, p, cv::SORT_EVERY_COLUMN + cv::SORT_ASCENDING);
		if (fabs(alpha - 0.05) < 0.0001)
		{
			thresh = (2.492 - 1) * (1 - 1.55 / (double)N) + 1;
		}
		else if (fabs(alpha - 0.01) < 0.0001)
		{
			thresh = (3.857 - 1) * (1 - 1.55 / (double)N) + 1;
		}
		else
		{
			thresh = (1.933 - 1) * (1 - 1.55 / (double)N) + 1;
		}
		int c = 0; double sum = 0.0, sentinel = -1.0;
		for (int i = 1; i < N; i++)
		{
			while (c <= n - 1)
			{
				if (p.at<double>(i - 1, 0) <= p1.at<double>(c, 0)) break;
				c++;
			}
			sum += double((N * c - n * i) * (N * c - n * i)) / double(i * (N - i));
		}
		sum /= (double)(n * n);
		if (sum > thresh) *homo_flag = -1;
		else *homo_flag = 0;

	}

	return 0;
}

int Utils::homogeneous_test(
	const vector<ComplexMat>& slc_series,
	int windsize_az,
	int windsize_rg,
	Mat& homo_num,
	Mat& homo_index,
	double alpha,
	const char* method
)
{
	if (slc_series.size() < 5 ||
		windsize_az < 3 ||
		windsize_az % 2 == 0 ||
		windsize_rg < 3 ||
		windsize_rg % 2 == 0
		)
	{
		fprintf(stderr, "homogeneous_test(): input check failed!\n");
		return -1;
	}
	int nr = slc_series[0].GetRows();
	int nc = slc_series[0].GetCols();
	int n_images = static_cast<int>(slc_series.size());
	int type = slc_series[0].type();
	for (int i = 0; i < n_images; i++)
	{
		int nr_tmp = slc_series[i].GetRows();
		int nc_tmp = slc_series[i].GetCols();
		int type_tmp = slc_series[i].type();
		if (type != type_tmp || nr_tmp != nr || nc_tmp != nc)
		{
			fprintf(stderr, "homogeneous_test(): images stack type or size mismatch!\n");
			return -1;
		}
	}
	homo_num.create(nr, nc, CV_32S); homo_num = 0;
	homo_index.create(nr * nc, windsize_az * windsize_rg, CV_8U); homo_index = 0;
	int radius_rg = (windsize_rg - 1) / 2;
	int radius_az = (windsize_az - 1) / 2;
	//统计同质检验
	if (type == CV_64F)
	{
		for (int m = 0; m < nr; m++)
		{
#pragma omp parallel for schedule(guided)
			for (int n = 0; n < nc; n++)
			{
				ComplexMat pix1(n_images, 1); ComplexMat pix2(n_images, 1);
				Mat pix1_amp, pix2_amp;
				Mat mask = Mat::zeros(windsize_az, windsize_rg, CV_32S);
				int ref_row, ref_col;
				ref_row = m, ref_col = n;
				mask.at<int>(radius_az, radius_rg) = 1;
				int count = 1;

				for (int i = 0; i < n_images; i++)
				{
					pix1.re.at<double>(i, 0) = slc_series[i].re.at<double>(ref_row, ref_col);
					pix1.im.at<double>(i, 0) = slc_series[i].im.at<double>(ref_row, ref_col);
				}
				pix1_amp = pix1.GetMod();
				for (int i = -radius_az; i <= radius_az; i++)
				{

					for (int j = -radius_rg; j <= radius_rg; j++)
					{
						if ((ref_row + i) < 0 || (ref_row + i) >= nr || (ref_col + j) < 0 || (ref_col + j) >= nc || (i == 0 && j == 0)) continue;
						for (int k = 0; k < n_images; k++)
						{
							pix2.re.at<double>(k, 0) = slc_series[k].re.at<double>(ref_row + i, ref_col + j);
							pix2.im.at<double>(k, 0) = slc_series[k].im.at<double>(ref_row + i, ref_col + j);
						}
						pix2_amp = pix2.GetMod();
						int b_homo = -1;
						homogeneous_test(pix1_amp, pix2_amp, &b_homo, alpha);
						if (b_homo == 0) { mask.at<int>(i + radius_az, j + radius_rg) = 1; count++; }
					}
				}
				homo_num.at<int>(m, n) = count;
				for (int i = 0; i < windsize_az; i++)
				{
					for (int j = 0; j < windsize_rg; j++)
					{
						homo_index.at<uchar>(m * nc + n, i * windsize_rg + j) = mask.at<int>(i, j);
					}
				}
			}
			printf("\r估计进度：%lf%%", double(m + 1) / double(nr) * 100.0);
			fflush(stdout);
		}
	}
	else if (type == CV_32F)
	{
		for (int m = 0; m < nr; m++)
		{
#pragma omp parallel for schedule(guided)
			for (int n = 0; n < nc; n++)
			{
				ComplexMat pix1(n_images, 1); ComplexMat pix2(n_images, 1);
				Mat pix1_amp, pix2_amp;
				Mat mask = Mat::zeros(windsize_az, windsize_rg, CV_32S);
				int ref_row, ref_col;
				ref_row = m, ref_col = n;
				mask.at<int>(radius_az, radius_rg) = 1;
				int count = 1;

				for (int i = 0; i < n_images; i++)
				{
					pix1.re.at<double>(i, 0) = slc_series[i].re.at<float>(ref_row, ref_col);
					pix1.im.at<double>(i, 0) = slc_series[i].im.at<float>(ref_row, ref_col);
				}
				pix1_amp = pix1.GetMod();
				for (int i = -radius_az; i <= radius_az; i++)
				{

					for (int j = -radius_rg; j <= radius_rg; j++)
					{
						if ((ref_row + i) < 0 || (ref_row + i) >= nr || (ref_col + j) < 0 || (ref_col + j) >= nc || (i == 0 && j == 0)) continue;
						for (int k = 0; k < n_images; k++)
						{
							pix2.re.at<double>(k, 0) = slc_series[k].re.at<float>(ref_row + i, ref_col + j);
							pix2.im.at<double>(k, 0) = slc_series[k].im.at<float>(ref_row + i, ref_col + j);
						}
						pix2_amp = pix2.GetMod();
						int b_homo = -1;
						homogeneous_test(pix1_amp, pix2_amp, &b_homo, alpha);
						if (b_homo == 0) { mask.at<int>(i + radius_az, j + radius_rg) = 1; count++; }
					}
				}
				homo_num.at<int>(m, n) = count;
				for (int i = 0; i < windsize_az; i++)
				{
					for (int j = 0; j < windsize_rg; j++)
					{
						homo_index.at<uchar>(m * nc + n, i * windsize_rg + j) = mask.at<int>(i, j);
					}
				}
			}
			printf("\r估计进度：%lf%%", double(m + 1) / double(nr) * 100.0);
			fflush(stdout);
		}
	}
	else if (type == CV_16S)
	{
		for (int m = 0; m < nr; m++)
		{
#pragma omp parallel for schedule(guided)
			for (int n = 0; n < nc; n++)
			{
				ComplexMat pix1(n_images, 1); ComplexMat pix2(n_images, 1);
				Mat pix1_amp, pix2_amp;
				Mat mask = Mat::zeros(windsize_az, windsize_rg, CV_32S);
				int ref_row, ref_col;
				ref_row = m, ref_col = n;
				mask.at<int>(radius_az, radius_rg) = 1;
				int count = 1;

				for (int i = 0; i < n_images; i++)
				{
					pix1.re.at<double>(i, 0) = slc_series[i].re.at<short>(ref_row, ref_col);
					pix1.im.at<double>(i, 0) = slc_series[i].im.at<short>(ref_row, ref_col);
				}
				pix1_amp = pix1.GetMod();
				for (int i = -radius_az; i <= radius_az; i++)
				{

					for (int j = -radius_rg; j <= radius_rg; j++)
					{
						if ((ref_row + i) < 0 || (ref_row + i) >= nr || (ref_col + j) < 0 || (ref_col + j) >= nc || (i == 0 && j == 0)) continue;
						for (int k = 0; k < n_images; k++)
						{
							pix2.re.at<double>(k, 0) = slc_series[k].re.at<short>(ref_row + i, ref_col + j);
							pix2.im.at<double>(k, 0) = slc_series[k].im.at<short>(ref_row + i, ref_col + j);
						}
						pix2_amp = pix2.GetMod();
						int b_homo = -1;
						homogeneous_test(pix1_amp, pix2_amp, &b_homo, alpha);
						if (b_homo == 0) { mask.at<int>(i + radius_az, j + radius_rg) = 1; count++; }
					}
				}
				homo_num.at<int>(m, n) = count;
				for (int i = 0; i < windsize_az; i++)
				{
					for (int j = 0; j < windsize_rg; j++)
					{
						homo_index.at<uchar>(m * nc + n, i * windsize_rg + j) = mask.at<int>(i, j);
					}
				}
			}
			printf("\r估计进度：%lf%%", double(m + 1) / double(nr) * 100.0);
			fflush(stdout);
		}
	}
	else
	{
		fprintf(stderr, "homogeneous_test(): data type not supported!\n");
		return -1;
	}
	return 0;
}

int Utils::HermitianEVD(const ComplexMat& input, Mat& eigenvalue, ComplexMat& eigenvector)
{
	if (input.GetCols() != input.GetRows() ||
		input.GetCols() <= 1 || 
		input.type() != CV_64F
		)
	{
		fprintf(stderr, "HermitianEVD(): input check failed!\n");
		return -1;
	}
	int rows = input.GetRows(); int cols = input.GetCols();
	eigenvalue.create(cols, 1, CV_64F);
	eigenvector.re.create(rows, cols, CV_64F);
	eigenvector.im.create(rows, cols, CV_64F);
	Eigen::MatrixXcd x(rows, cols);
	complex<double> d;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			d.real(input.re.at<double>(i, j));
			d.imag(input.im.at<double>(i, j));
			x(i, j) = d;
		}
	}
	Eigen::ComplexEigenSolver<Eigen::MatrixXcd> solver;
	solver.compute(x, true);
	if (solver.info() != Eigen::Success)
	{
		fprintf(stderr, "ComplexEVD(): EVD failed!\n");
		return -1;
	}
	Eigen::MatrixXcd eigenvectors = solver.eigenvectors();
	Eigen::MatrixXcd eigenvalues = solver.eigenvalues();
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			eigenvector.re.at<double>(i, j) = eigenvectors(i, j).real();
			eigenvector.im.at<double>(i, j) = eigenvectors(i, j).imag();
			eigenvalue.at<double>(i, 0) = eigenvalues(i, 0).real();
		}
	}
	Mat idx, value; ComplexMat vec; int xx, xxx;
	eigenvalue.copyTo(value); vec = eigenvector;
	cv::sortIdx(eigenvalue, idx, cv::SORT_EVERY_COLUMN + cv::SORT_DESCENDING);
	for (int i = 0; i < rows; i++)
	{
		xx = idx.at<int>(i, 0);
		for (int j = 0; j < cols; j++)
		{
			xxx = idx.at<int>(j, 0);
			eigenvalue.at<double>(i, 0) = value.at<double>(xx, 0);
			eigenvector.re.at<double>(i, j) = vec.re.at<double>(i, xxx);
			eigenvector.im.at<double>(i, j) = vec.im.at<double>(i, xxx);
		}
	}
	return 0;
}

int Utils::coherence_matrix_estimation(const vector<ComplexMat>& slc_series, ComplexMat& coherence_matrix, int est_window_width, int est_window_height,  int ref_row, int ref_col, bool b_homogeneous_test, bool b_normalize)
{
	if (slc_series.size() < 3||
		est_window_width < 3||
		est_window_height < 3||
		est_window_height % 2 != 1||
		est_window_width % 2 != 1||
		ref_col < 0||
		ref_row < 0
		)
	{
		fprintf(stderr, "coherence_estimation(): input check failed!\n");
		return -1;
	}
	if (slc_series[0].type() != CV_64F || slc_series[0].isEmpty() || ref_row > slc_series[0].GetRows() - 1 || ref_col > slc_series[0].GetCols() - 1
		)
	{
		fprintf(stderr, "coherence_estimation(): input check failed!\n");
		return -1;
	}
	int n_images = static_cast<int>(slc_series.size()), ret;
	int rows = slc_series[0].GetRows(); int cols = slc_series[0].GetCols();
	int radius_width = (est_window_width - 1) / 2;
	int radius_height = (est_window_height - 1) / 2;
	int left, right, bottom, top;
	left = (ref_col - radius_width) < 0 ? 0 : (ref_col - radius_width);
	right = (ref_col + radius_width) > cols - 1 ? cols - 1 : (ref_col + radius_width);
	bottom = (ref_row + radius_height) > rows - 1 ? rows - 1 : (ref_row + radius_height);
	top = (ref_row - radius_height) < 0 ? 0 : (ref_row - radius_height);
	rows = bottom - top + 1;
	cols = right - left + 1;
	if (b_homogeneous_test)
	{
		//统计同质检验
		ComplexMat pix1(n_images, 1); ComplexMat pix2(n_images, 1);
		Mat pix1_amp, pix2_amp;
		Mat mask = Mat::zeros(rows, cols, CV_32S);
		mask.at<int>(ref_row - top, ref_col - left) = 1;
		int b_homo, count = 1;
		for (int i = 0; i < n_images; i++)
		{
			pix1.re.at<double>(i, 0) = slc_series[i].re.at<double>(ref_row, ref_col);
			pix1.im.at<double>(i, 0) = slc_series[i].im.at<double>(ref_row, ref_col);
		}
		pix1_amp = pix1.GetMod();
		for (int i = 0; i < rows; i++)
		{
			
			for (int j = 0; j < cols; j++)
			{
				if (i == (ref_row - top) && j == (ref_col - left)) continue;
				for (int k = 0; k < n_images; k++)
				{
					pix2.re.at<double>(k, 0) = slc_series[k].re.at<double>(i + top, j + left);
					pix2.im.at<double>(k, 0) = slc_series[k].im.at<double>(i + top, j + left);
				}
				pix2_amp = pix2.GetMod();
				ret = homogeneous_test(pix1_amp, pix2_amp, &b_homo, 0.1);
				//ret = homogeneous_test(pix1_amp, pix2_amp, &b_homo, 0.1, "AD");
				if (return_check(ret, "homogeneous_test()", error_head)) return -1;
				if (b_homo == 0) { mask.at<int>(i, j) = 1; count++; }
			}
		}
		//Mat c; mask.convertTo(c, CV_64F);
		//cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\mask.bin", c);
		if (count < 2) 
		{
			//fprintf(stderr, "coherence_matrix_estimation(): no homogenous pixels inside estimation window!\n");
			return -1;
		}
		//估计相关矩阵
		int count2 = count; count = 0;
		ComplexMat Covariance;
		Mat sum(n_images, 1, CV_64F), A(n_images, count2, CV_64F), B(n_images, count2, CV_64F), C, A_t, B_t;
		double s;
		for (int k = 0; k < n_images; k++)
		{
			s = 0.0;
			count = 0;
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					if (mask.at<int>(i, j) > 0)
					{
						A.at<double>(k, count) = slc_series[k].re.at<double>(i + top, j + left);
						B.at<double>(k, count) = slc_series[k].im.at<double>(i + top, j + left);
						count++;
						s += A.at<double>(k, count - 1) * A.at<double>(k, count - 1)
							+ B.at<double>(k, count - 1) * B.at<double>(k, count - 1);
					}
				}
			}
			sum.at<double>(k, 0) = s;
		}
		cv::transpose(A, A_t); cv::transpose(B, B_t);
		C = A * A_t + B * B_t;
		C.copyTo(Covariance.re);
		C = B * A_t - A * B_t;
		C.copyTo(Covariance.im);
		double denum;
		if (b_normalize)
		{

			for (int i = 0; i < n_images; i++)
			{
				for (int j = 0; j < n_images; j++)
				{
					denum = sqrt(sum.at<double>(i, 0) * sum.at<double>(j, 0));
					Covariance.re.at<double>(i, j) = Covariance.re.at<double>(i, j) / (denum + 1e-10);
					Covariance.im.at<double>(i, j) = Covariance.im.at<double>(i, j) / (denum + 1e-10);
				}
			}


		}
		else
		{
			Covariance = Covariance * (1 / (double)count2);
		}
		coherence_matrix = Covariance;
	}
	else
	{
		//估计相关矩阵
		int count2 = rows * cols; int count;
		ComplexMat Covariance;
		Mat sum(n_images, 1, CV_64F), A(n_images, count2, CV_64F), B(n_images, count2, CV_64F), C, A_t, B_t;
		double s;
		for (int k = 0; k < n_images; k++)
		{
			s = 0.0;
			count = 0;
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					A.at<double>(k, count) = slc_series[k].re.at<double>(i + top, j + left);
					B.at<double>(k, count) = slc_series[k].im.at<double>(i + top, j + left);
					count++;
					s += A.at<double>(k, count - 1) * A.at<double>(k, count - 1)
						+ B.at<double>(k, count - 1) * B.at<double>(k, count - 1);
				}
			}
			sum.at<double>(k, 0) = s;
		}
		cv::transpose(A, A_t); cv::transpose(B, B_t);
		C = A * A_t + B * B_t;
		C.copyTo(Covariance.re);
		C = B * A_t - A * B_t;
		C.copyTo(Covariance.im);
		double denum;
		if (b_normalize)
		{

			for (int i = 0; i < n_images; i++)
			{
				for (int j = 0; j < n_images; j++)
				{
					denum = sqrt(sum.at<double>(i, 0) * sum.at<double>(j, 0));
					Covariance.re.at<double>(i, j) = Covariance.re.at<double>(i, j) / (denum + 1e-10);
					Covariance.im.at<double>(i, j) = Covariance.im.at<double>(i, j) / (denum + 1e-10);
				}
			}


		}
		else
		{
			Covariance = Covariance * (1 / (double)count2);
		}
		coherence_matrix = Covariance;
	}
	return 0;
}

//int Utils::MB_phase_estimation(
//	vector<string> coregis_slc_files,
//	vector<string> phase_files, 
//	vector<string> coherence_files,
//	int master_indx, 
//	int blocksize_row, 
//	int blocksize_col, 
//	Mat& out_mask,
//	bool b_coh_est,
//	int homogeneous_test_wnd,
//	double thresh_c1_to_c2,
//	bool b_flat,
//	bool b_normalize
//)
//{
//	if (coregis_slc_files.size() < 2 ||
//		phase_files.size() != coregis_slc_files.size() ||
//		coherence_files.size() != phase_files.size() ||
//		master_indx < 1 ||
//		master_indx > phase_files.size() ||
//		blocksize_row < 100 ||
//		blocksize_col < 100 ||
//		thresh_c1_to_c2 < 0.0 ||
//		thresh_c1_to_c2 > 1.0 ||
//		homogeneous_test_wnd % 2 == 0
//		)
//	{
//		fprintf(stderr, "MB_phase_estimation(): input check failed!\n");
//		return -1;
//	}
//	int homotest_radius = (homogeneous_test_wnd - 1) / 2;
//	if (blocksize_row <= homotest_radius || blocksize_col <= homotest_radius)
//	{
//		fprintf(stderr, "MB_phase_estimation(): input check failed!\n");
//		return -1;
//	}
//	int nr, nc, ret, n_images; Mat tmp;
//	n_images = coregis_slc_files.size();
//	FormatConversion conversion; Deflat flat;
//	ret = conversion.read_array_from_h5(coregis_slc_files[0].c_str(), "azimuth_len", tmp);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	nr = tmp.at<int>(0, 0);
//	ret = conversion.read_array_from_h5(coregis_slc_files[0].c_str(), "range_len", tmp);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	nc = tmp.at<int>(0, 0);
//	Mat mask = Mat::zeros(nr, nc, CV_32S); mask.copyTo(out_mask); mask.release();
//	//检查输入SAR图像尺寸是否相同
//	for (int i = 1; i < n_images; i++)
//	{
//		ret = conversion.read_array_from_h5(coregis_slc_files[i].c_str(), "azimuth_len", tmp);
//		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//		if (tmp.at<int>(0, 0) != nr)
//		{
//			fprintf(stderr, "%s images size mismatch!\n", coregis_slc_files[i].c_str());
//			return -1;
//		}
//		ret = conversion.read_array_from_h5(coregis_slc_files[i].c_str(), "range_len", tmp);
//		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//		if (tmp.at<int>(0, 0) != nc)
//		{
//			fprintf(stderr, "%s images size mismatch!\n", coregis_slc_files[i].c_str());
//			return -1;
//		}
//	}
//
//	//去平地相位
//	Mat stateVec1, prf, lon_coef, lat_coef, 
//		carrier_frequency, stateVec2, prf2, phase,
//		phase_deflat, flat_phase_coef, azimuth_len, range_len;
//	phase = Mat::zeros(nr, nc, CV_64F);
//	azimuth_len = Mat::zeros(1, 1, CV_32S); range_len = Mat::zeros(1, 1, CV_32S);
//	int offset_row, offset_col;
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "offset_row", tmp);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	offset_row = tmp.at<int>(0, 0);
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "offset_col", tmp);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	offset_col = tmp.at<int>(0, 0);
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "state_vec", stateVec1);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "prf", prf);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "lon_coefficient", lon_coef);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "lat_coefficient", lat_coef);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	ret = conversion.read_array_from_h5(coregis_slc_files[master_indx - 1].c_str(), "carrier_frequency", carrier_frequency);
//	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//	for (int i = 0; i < n_images; i++)
//	{
//		//预先填充相关系数
//		if (b_coh_est)
//		{
//			ret = conversion.creat_new_h5(coherence_files[i].c_str());
//			if (return_check(ret, "creat_new_h5()", error_head)) return -1;
//			ret = conversion.write_array_to_h5(coherence_files[i].c_str(), "coherence", phase);
//			if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//			azimuth_len.at<int>(0, 0) = phase.rows; range_len.at<int>(0, 0) = phase.cols;
//			ret = conversion.write_array_to_h5(coherence_files[i].c_str(), "azimuth_len", azimuth_len);
//			if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//			ret = conversion.write_array_to_h5(coherence_files[i].c_str(), "range_len", range_len);
//			if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//		}
//		//预先填充干涉相位
//		ret = conversion.creat_new_h5(phase_files[i].c_str());
//		if (return_check(ret, "creat_new_h5()", error_head)) return -1;
//		ret = conversion.write_array_to_h5(phase_files[i].c_str(), "phase", phase);
//		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//		azimuth_len.at<int>(0, 0) = phase.rows; range_len.at<int>(0, 0) = phase.cols;
//		ret = conversion.write_array_to_h5(phase_files[i].c_str(), "azimuth_len", azimuth_len);
//		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//		ret = conversion.write_array_to_h5(phase_files[i].c_str(), "range_len", range_len);
//		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//		if (b_flat)
//		{
//			ret = conversion.read_array_from_h5(coregis_slc_files[i].c_str(), "state_vec", stateVec2);
//			if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//			ret = conversion.read_array_from_h5(coregis_slc_files[i].c_str(), "prf", prf2);
//			if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//			ret = flat.deflat(stateVec1, stateVec2, lon_coef, lat_coef, phase, offset_row, offset_col, 0, 1 / prf.at<double>(0, 0),
//				1 / prf2.at<double>(0, 0), 1, 3e8 / carrier_frequency.at<double>(0, 0), phase, flat_phase_coef);
//			ret = conversion.write_array_to_h5(phase_files[i].c_str(), "flat_phase_coefficient", flat_phase_coef);
//			if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
//		}
//		fprintf(stdout, "去平地进度：%d/%d\n", i, n_images - 1);
//	}
//
//	//分块读取、计算和储存
//
//	int left, right, top, bottom, block_num_row, block_num_col, left_pad, right_pad, top_pad, bottom_pad;
//	vector<ComplexMat> slc_series, slc_series_filter;
//	vector<Mat> coherence_series; coherence_series.resize(n_images);
//	ComplexMat slc, slc2, temp;
//	Mat flat_phase, ph, zeromat;
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
//
//			//读取数据
//			for (int k = 0; k < n_images; k++)
//			{
//				ret = conversion.read_subarray_from_h5(coregis_slc_files[k].c_str(), "s_re",
//					top_pad, left_pad, bottom_pad - top_pad, right_pad - left_pad, slc.re);
//				if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
//				ret = conversion.read_subarray_from_h5(coregis_slc_files[k].c_str(), "s_im",
//					top_pad, left_pad, bottom_pad - top_pad, right_pad - left_pad, slc.im);
//				if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
//				if (b_flat)
//				{
//					if (k != master_indx - 1)
//					{
//						flat_phase.create(bottom_pad - top_pad, right_pad - left_pad, CV_64F);
//						ret = conversion.read_array_from_h5(phase_files[k].c_str(), "flat_phase_coefficient", flat_phase_coef);
//						if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
//#pragma omp parallel for schedule(guided)
//						for (int ii = top_pad; ii < bottom_pad; ii++)
//						{
//							Mat tempp(1, 6, CV_64F);
//							for (int jj = left_pad; jj < right_pad; jj++)
//							{
//								tempp.at<double>(0, 0) = 1.0;
//								tempp.at<double>(0, 1) = ii;
//								tempp.at<double>(0, 2) = jj;
//								tempp.at<double>(0, 3) = ii * jj;
//								tempp.at<double>(0, 4) = ii * ii;
//								tempp.at<double>(0, 5) = jj * jj;
//								flat_phase.at<double>(ii - top_pad, jj - left_pad) = sum(tempp.mul(flat_phase_coef))[0];
//							}
//						}
//						ret = phase2cos(flat_phase, temp.re, temp.im);
//						if (return_check(ret, "phase2cos()", error_head)) return -1;
//						slc = slc * temp;
//					}
//					
//				}
//				slc_series.push_back(slc);
//				slc_series_filter.push_back(slc);
//			}
//
//			//填充相关系数
//			if (b_coh_est)
//			{
//				zeromat = Mat::zeros(flat_phase.rows, flat_phase.cols, CV_64F);
//				for (int mm = 0; mm < n_images; mm++)
//				{
//					zeromat.copyTo(coherence_series[mm]);
//				}
//			}
//			//计算
//#pragma omp parallel for schedule(guided)
//			for (int ii = (top - top_pad); ii < (bottom - top_pad); ii++)
//			{
//				ComplexMat coherence_matrix, eigenvector; Mat eigenvalue; int ret;
//				for (int jj = (left - left_pad); jj < (right - left_pad); jj++)
//				{
//					ret = coherence_matrix_estimation(slc_series, coherence_matrix, homogeneous_test_wnd, homogeneous_test_wnd, ii, jj);
//					if (ret == 0)
//					{
//						ret = HermitianEVD(coherence_matrix, eigenvalue, eigenvector);
//						if (!eigenvalue.empty() && ret == 0)
//						{
//							if (eigenvalue.at<double>(1, 0) / (eigenvalue.at<double>(0, 0) + 1e-10) < thresh_c1_to_c2)
//							{
//								out_mask.at<int>(ii + top_pad, jj + left_pad) = 1;
//								for (int kk = 0; kk < n_images; kk++)
//								{
//									slc_series_filter[kk].re.at<double>(ii, jj) = eigenvector.re.at<double>(kk, 0);
//									slc_series_filter[kk].im.at<double>(ii, jj) = eigenvector.im.at<double>(kk, 0);
//									if (b_coh_est)
//									{
//										coherence_series[kk].at<double>(ii, jj) = coherence_matrix(cv::Range(master_indx - 1, master_indx),
//											cv::Range(kk, kk + 1)).GetMod().at<double>(0, 0);
//									}
//									
//								}
//							}
//						}
//					}
//
//
//				}
//			}
//
//			//储存
//			slc = slc_series_filter[master_indx - 1];
//			for (int kk = 0; kk < n_images; kk++)
//			{
//				coherence_series[kk](cv::Range(top - top_pad, bottom - top_pad), cv::Range(left - left_pad, right - left_pad)).copyTo(ph);
//				ret = conversion.write_subarray_to_h5(coherence_files[kk].c_str(), "coherence", ph, top, left, bottom - top, right - left);
//				if (return_check(ret, "write_subarray_to_h5()", error_head)) return -1;
//				//if (kk == master_indx - 1) continue;
//				ret = multilook(slc, slc_series_filter[kk], 1, 1, phase);
//				if (return_check(ret, "multilook()", error_head)) return -1;
//				phase(cv::Range(top - top_pad, bottom - top_pad), cv::Range(left - left_pad, right - left_pad)).copyTo(ph);
//				ret = conversion.write_subarray_to_h5(phase_files[kk].c_str(), "phase", ph, top, left, bottom - top, right - left);
//				if (return_check(ret, "write_subarray_to_h5()", error_head)) return -1;
//				
//			}
//			slc_series.clear();
//			slc_series_filter.clear();
//			
//			fprintf(stdout, "估计相位进度：%lf\n", double((i + 1) * block_num_col + j + 1) / double((block_num_col) * (block_num_row)));
//		}
//	}
//
//	return 0;
//}

int Utils::unwrap_region_growing(
	vector<tri_node>& nodes, 
	const vector<tri_edge>& edges, 
	size_t start_edge, 
	double distance_thresh, 
	double quality_thresh
)
{
	if (nodes.size() < 3 ||
		edges.size() < 3 ||
		start_edge < 1 ||
		start_edge > edges.size()
		)
	{
		fprintf(stderr, "unwrap_region_growing(): input check failed!\n\n");
		return -1;
	}
	if (distance_thresh < 1.0) distance_thresh = 1.0;
	if (quality_thresh > 0.9) quality_thresh = 0.9;
	//找到增量积分起始点
	int ix = 0;
	double MC = -1.0;
	size_t num_edges = edges.size();
	//for (int i = 0; i < num_edges; i++)
	//{
	//	if (edges[i].MC > MC)
	//	{
	//		ix = i;
	//		MC = edges[i].MC;
	//	}
	//}
	size_t start = edges[start_edge - 1].end1;

	//采用类似质量图法解缠的算法进行增量积分集成
	edge_index tmp;
	priority_queue<edge_index> que;
	//nodes[start - 1].set_vel(0.0);//起始点形变速率和高程误差设置为0，后续可根据参考点进行校正
	//nodes[start - 1].set_height(0.0);
	nodes[start - 1].set_status(true);
	int end2, number, row1, col1, row2, col2;
	double distance, phase, MC_total, phase_total, delta_phase;

	for (long edge_val : nodes[start - 1].get_neigh_edges())
	{
		end2 = edges[edge_val - 1].end1 == start ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
		nodes[start - 1].get_distance(nodes[end2 - 1], &distance);
		if (!nodes[end2 - 1].get_status() &&
			distance <= distance_thresh &&
			edges[edge_val - 1].quality > quality_thresh
			)
		{
			tmp.num = edge_val;
			tmp.quality = -edges[edge_val - 1].quality;
			que.push(tmp);
		}
	}

	while (que.size() != 0)
	{
		tmp = que.top();
		que.pop();
		if (nodes[edges[tmp.num - 1].end1 - 1].get_status())
		{
			number = edges[tmp.num - 1].end1;
			end2 = edges[tmp.num - 1].end2;
		}
		else
		{
			number = edges[tmp.num - 1].end2;
			end2 = edges[tmp.num - 1].end1;
		}
		MC_total = 1e-10;
		phase_total = 0.0;
		if (!nodes[end2 - 1].get_status())
		{
			nodes[end2 - 1].get_pos(&row2, &col2);
			for (long edge_val : nodes[end2 - 1].get_neigh_edges())
			{
				number = edges[edge_val - 1].end1 == end2 ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
				if (nodes[number - 1].get_status())
				{
					nodes[number - 1].get_phase(&phase);
					nodes[number - 1].get_pos(&row1, &col1);
					if (row1 > row2)
					{
						delta_phase = -edges[edge_val - 1].phase_diff;
					}
					if (row1 < row2)
					{
						delta_phase = edges[edge_val - 1].phase_diff;
					}
					if (row1 == row2)
					{
						if (col1 > col2)
						{
							delta_phase = -edges[edge_val - 1].phase_diff;
						}
						else
						{
							delta_phase = edges[edge_val - 1].phase_diff;
						}
					}
					phase_total += (delta_phase + phase) * edges[edge_val - 1].quality;
					MC_total += edges[edge_val - 1].quality;
				}
			}
			nodes[end2 - 1].set_phase(phase_total / MC_total);
			nodes[end2 - 1].set_status(true);
			number = end2;
			for (long edge_val : nodes[number - 1].get_neigh_edges())
			{
				end2 = edges[edge_val - 1].end1 == number ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
				nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
				if (!nodes[end2 - 1].get_status() &&
					distance <= distance_thresh &&
					edges[edge_val - 1].quality > quality_thresh
					)
				{
					tmp.num = edge_val;
					tmp.quality = -edges[edge_val - 1].quality;
					que.push(tmp);
				}
			}
		}
	}
	return 0;
}

