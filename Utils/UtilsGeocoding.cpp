#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\Hdf5IO.h"
#include "..\include\tinyxml.h"
#include "gdal_priv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace cv;
using namespace std;

extern void InitializeGDALAndProjOnce();

namespace
{
	constexpr uint64_t kSar2UtmOutputMemoryBudgetBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
	// b_filled(CV_8U) + mapped_phase(CV_64F)。预算校验与输出网格预检必须共用这一个口径。
	constexpr uint64_t kSar2UtmOutputBytesPerPixel = sizeof(uchar) + sizeof(double);
	constexpr int kSar2UtmGradientSamplesPerAxis = 129;
	constexpr double kSar2UtmCoordinateGradientEpsilon = 1.0e-12;
	constexpr double kMetersPerLatitudeDegree = 110574.0;
	constexpr double kMetersPerLongitudeDegreeAtEquator = 111320.0;

	struct GeographicGridStats
	{
		uint64_t validCount = 0;
		uint64_t missingCount = 0;
		uint64_t nonFiniteCount = 0;
		uint64_t outOfRangeCount = 0;
		double minLon = 0.0;
		double maxLon = 0.0;
		double minLat = 0.0;
		double maxLat = 0.0;
	};

	struct GeographicGridSpacing
	{
		double lonInterval = 0.0;
		double latInterval = 0.0;
		double rowSpacingMeters = 0.0;
		double columnSpacingMeters = 0.0;
		uint64_t rowGradientCount = 0;
		uint64_t columnGradientCount = 0;
	};

	double geographicCoordinateAt(const Mat& coordinate, int row, int column)
	{
		return coordinate.type() == CV_32F ? coordinate.at<float>(row, column) : coordinate.at<double>(row, column);
	}

	double wrappedLongitudeDifference(double difference)
	{
		difference = std::fmod(difference + 180.0, 360.0);
		if (difference < 0.0) difference += 360.0;
		return difference - 180.0;
	}

	double medianPositiveValue(vector<double>& values)
	{
		if (values.empty()) return 0.0;
		std::sort(values.begin(), values.end());
		const size_t middle = values.size() / 2;
		return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
	}

	void appendGeographicGradient(double lon1, double lat1, double lon2, double lat2,
		vector<double>& lonComponents, vector<double>& latComponents, vector<double>& metricSpacings)
	{
		if (!std::isfinite(lon1) || !std::isfinite(lat1) || !std::isfinite(lon2) || !std::isfinite(lat2)) return;
		const double lonDifference = wrappedLongitudeDifference(lon2 - lon1);
		const double latDifference = lat2 - lat1;
		const double absoluteLonDifference = std::fabs(lonDifference);
		const double absoluteLatDifference = std::fabs(latDifference);
		if (absoluteLonDifference > kSar2UtmCoordinateGradientEpsilon) lonComponents.push_back(absoluteLonDifference);
		if (absoluteLatDifference > kSar2UtmCoordinateGradientEpsilon) latComponents.push_back(absoluteLatDifference);
		const double meanLatitudeRadians = (lat1 + lat2) * PI / 360.0;
		const double metersPerLongitudeDegree = kMetersPerLongitudeDegreeAtEquator * std::cos(meanLatitudeRadians);
		const double spacingMeters = std::hypot(lonDifference * metersPerLongitudeDegree,
			latDifference * kMetersPerLatitudeDegree);
		if (std::isfinite(spacingMeters) && spacingMeters > kSar2UtmCoordinateGradientEpsilon) metricSpacings.push_back(spacingMeters);
	}

	bool estimateGeographicGridSpacing(const char* caller, const Mat& mappedLon, const Mat& mappedLat,
		GeographicGridSpacing& spacing)
	{
		spacing = GeographicGridSpacing{};
		if (mappedLon.rows < 2 || mappedLon.cols < 2) return false;
		const int rowSampleCount = (std::min)(kSar2UtmGradientSamplesPerAxis, mappedLon.rows - 1);
		const int columnSampleCount = (std::min)(kSar2UtmGradientSamplesPerAxis, mappedLon.cols - 1);
		// 输出网格是经纬度轴对齐的：输出行间距必须由行方向的纬度增量决定，输出列间距必须由列方向的
		// 经度增量决定。SAR 几何下方位向与距离向的地面尺度相差数倍，两个方向混在一起取中位数会得到
		// 既非行向亦非列向的中间值，导致输出网格在两个方向上都被过采样。
		vector<double> rowLonComponents;
		vector<double> rowLatComponents;
		vector<double> columnLonComponents;
		vector<double> columnLatComponents;
		vector<double> rowSpacings;
		vector<double> columnSpacings;
		rowLonComponents.reserve(static_cast<size_t>(rowSampleCount) * columnSampleCount);
		rowLatComponents.reserve(static_cast<size_t>(rowSampleCount) * columnSampleCount);
		columnLonComponents.reserve(static_cast<size_t>(rowSampleCount) * columnSampleCount);
		columnLatComponents.reserve(static_cast<size_t>(rowSampleCount) * columnSampleCount);
		rowSpacings.reserve(static_cast<size_t>(rowSampleCount) * columnSampleCount);
		columnSpacings.reserve(static_cast<size_t>(rowSampleCount) * columnSampleCount);

		for (int rowIndex = 0; rowIndex < rowSampleCount; ++rowIndex)
		{
			const int row = rowSampleCount == 1 ? 0 : ((mappedLon.rows - 2) * rowIndex) / (rowSampleCount - 1);
			for (int columnIndex = 0; columnIndex < columnSampleCount; ++columnIndex)
			{
				const int column = columnSampleCount == 1 ? 0 : ((mappedLon.cols - 2) * columnIndex) / (columnSampleCount - 1);
				const double lon = geographicCoordinateAt(mappedLon, row, column);
				const double lat = geographicCoordinateAt(mappedLat, row, column);
				appendGeographicGradient(lon, lat,
					geographicCoordinateAt(mappedLon, row + 1, column), geographicCoordinateAt(mappedLat, row + 1, column),
					rowLonComponents, rowLatComponents, rowSpacings);
				appendGeographicGradient(lon, lat,
					geographicCoordinateAt(mappedLon, row, column + 1), geographicCoordinateAt(mappedLat, row, column + 1),
					columnLonComponents, columnLatComponents, columnSpacings);
			}
		}

		spacing.lonInterval = medianPositiveValue(columnLonComponents);
		spacing.latInterval = medianPositiveValue(rowLatComponents);
		spacing.rowSpacingMeters = medianPositiveValue(rowSpacings);
		spacing.columnSpacingMeters = medianPositiveValue(columnSpacings);
		spacing.rowGradientCount = rowSpacings.size();
		spacing.columnGradientCount = columnSpacings.size();
		const bool valid = std::isfinite(spacing.lonInterval) && std::isfinite(spacing.latInterval) &&
			std::isfinite(spacing.rowSpacingMeters) && std::isfinite(spacing.columnSpacingMeters) &&
			spacing.lonInterval > 0.0 && spacing.latInterval > 0.0 &&
			spacing.rowSpacingMeters > 0.0 && spacing.columnSpacingMeters > 0.0;
		fprintf(stderr,
			"%s: geographic gradient samples row=%llu column=%llu rowSpacingMeters=%.17g columnSpacingMeters=%.17g lonInterval=%.17g latInterval=%.17g.\n",
			caller, static_cast<unsigned long long>(spacing.rowGradientCount), static_cast<unsigned long long>(spacing.columnGradientCount),
			spacing.rowSpacingMeters, spacing.columnSpacingMeters, spacing.lonInterval, spacing.latInterval);
		return valid;
	}

	bool scanGeographicGrid(const Mat& mappedLon, const Mat& mappedLat, GeographicGridStats& stats)
	{
		stats = GeographicGridStats{};
		for (int row = 0; row < mappedLat.rows; ++row)
		{
			for (int column = 0; column < mappedLat.cols; ++column)
			{
				const double lon = mappedLon.type() == CV_32F ? mappedLon.at<float>(row, column) : mappedLon.at<double>(row, column);
				const double lat = mappedLat.type() == CV_32F ? mappedLat.at<float>(row, column) : mappedLat.at<double>(row, column);
				if (!std::isfinite(lon) || !std::isfinite(lat))
				{
					++stats.nonFiniteCount;
					continue;
				}
				// Legacy products use values above 350 as a paired no-data sentinel.
				if (lon > 350.0 && lat > 350.0)
				{
					++stats.missingCount;
					continue;
				}
				if (lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0)
				{
					++stats.outOfRangeCount;
					continue;
				}
				if (stats.validCount == 0)
				{
					stats.minLon = stats.maxLon = lon;
					stats.minLat = stats.maxLat = lat;
				}
				else
				{
					stats.minLon = (std::min)(stats.minLon, lon);
					stats.maxLon = (std::max)(stats.maxLon, lon);
					stats.minLat = (std::min)(stats.minLat, lat);
					stats.maxLat = (std::max)(stats.maxLat, lat);
				}
				++stats.validCount;
			}
		}
		return stats.validCount != 0 && stats.nonFiniteCount == 0 && stats.outOfRangeCount == 0;
	}

	void logGeographicGridStats(const char* caller, const Mat& mappedLon, const Mat& mappedLat,
		const GeographicGridStats& stats)
	{
		fprintf(stderr,
			"%s: mappedLon=%dx%d mappedLat=%dx%d valid=%llu missing=%llu nonFinite=%llu outOfRange=%llu lon=[%.17g,%.17g] lat=[%.17g,%.17g]\n",
			caller, mappedLon.rows, mappedLon.cols, mappedLat.rows, mappedLat.cols,
			static_cast<unsigned long long>(stats.validCount), static_cast<unsigned long long>(stats.missingCount),
			static_cast<unsigned long long>(stats.nonFiniteCount), static_cast<unsigned long long>(stats.outOfRangeCount),
			stats.minLon, stats.maxLon, stats.minLat, stats.maxLat);
	}

	void logSar2UtmInput(const char* caller, const Mat& mappedLon, const Mat& mappedLat, const Mat& source)
	{
		fprintf(stderr, "%s: mappedLon=%dx%d type=%d mappedLat=%dx%d type=%d source=%dx%d type=%d\n",
			caller, mappedLon.rows, mappedLon.cols, mappedLon.type(), mappedLat.rows, mappedLat.cols, mappedLat.type(),
			source.rows, source.cols, source.type());
	}

	bool checkedSar2UtmDimensions(double rowCount, double columnCount, int& rows, int& columns)
	{
		if (!std::isfinite(rowCount) || !std::isfinite(columnCount) || rowCount < 1.0 || columnCount < 1.0 ||
			rowCount > static_cast<double>((std::numeric_limits<int>::max)()) ||
			columnCount > static_cast<double>((std::numeric_limits<int>::max)())) return false;
		const int64_t rowValue = static_cast<int64_t>(std::ceil(rowCount));
		const int64_t columnValue = static_cast<int64_t>(std::ceil(columnCount));
		if (rowValue < 1 || columnValue < 1 ||
			rowValue > (std::numeric_limits<int>::max)() || columnValue > (std::numeric_limits<int>::max)()) return false;
		const uint64_t pixelCount = static_cast<uint64_t>(rowValue) * static_cast<uint64_t>(columnValue);
		if (pixelCount > kSar2UtmOutputMemoryBudgetBytes / kSar2UtmOutputBytesPerPixel) return false;
		rows = static_cast<int>(rowValue);
		columns = static_cast<int>(columnValue);
		return true;
	}

	bool toSar2UtmGridIndex(double lon, double lat, double west, double minLat,
		double lonInterval, double latInterval, int rows, int columns, int& row, int& column)
	{
		if (!std::isfinite(lon) || !std::isfinite(lat)) return false;
		const double rowValue = std::round((lat - minLat) / latInterval);
		double lonDistance = std::fabs(lon - west);
		lonDistance = lonDistance > 180.0 ? 360.0 - lonDistance : lonDistance;
		const double columnValue = std::round(lonDistance / lonInterval);
		if (!std::isfinite(rowValue) || !std::isfinite(columnValue) ||
			rowValue < 0.0 || rowValue >= rows || columnValue < 0.0 || columnValue >= columns) return false;
		row = static_cast<int>(rowValue);
		column = static_cast<int>(columnValue);
		return true;
	}
}

namespace
{
	// 顺序填充：按行、再按列各走一遍，遇到一段连续空洞（filled==0）就一次性填完。
	//
	// 原实现是让每个空洞各自向上下左右四个方向搜索最近锚点，代价为 Σ L(L+1)
	// （L 为连续空洞的游程长度，一个长度 8893 的游程就要 1.98e7 次扫描步），
	// 在 9858×32100 的网格上实测耗掉 7,552 s。本实现与游程长度无关，代价 O(行×列)。
	//
	// 语义：filled==1 是散射时写入且值有限的锚点；filled==2 是输入自身携带的 NaN，
	// 一律保留、既不作为锚点也不被覆盖；两侧都取不到有限锚点的游程写 nodata(NaN)。
	void fillRunsSequential(cv::Mat& value, const cv::Mat& filled, bool interpolate)
	{
		const int rows = value.rows;
		const int cols = value.cols;
		if (value.type() != CV_64F || filled.type() != CV_8U ||
			filled.rows != rows || filled.cols != cols)
		{
			// 类型不符时必须出声：静默不填会让空洞保持初值 0.0，与真实观测无法区分
			fprintf(stderr, "fillRunsSequential(): unsupported input (value type=%d, filled type=%d, %dx%d vs %dx%d).\n",
				value.type(), filled.type(), value.rows, value.cols, filled.rows, filled.cols);
			return;
		}

		// 第一遍：按行，填掉两侧都有锚点的水平游程
		for (int i = 0; i < rows; ++i)
		{
			int j = 0;
			while (j < cols)
			{
				if (filled.at<uchar>(i, j) != 0) { ++j; continue; }
				const int first = j;
				while (j < cols && filled.at<uchar>(i, j) == 0) ++j;
				const int last = j - 1;
				const int left = first - 1;
				const int right = j;
				if (left >= 0 && right < cols &&
					std::isfinite(value.at<double>(i, left)) && std::isfinite(value.at<double>(i, right)))
				{
					const double leftValue = value.at<double>(i, left);
					const double rightValue = value.at<double>(i, right);
					for (int k = first; k <= last; ++k)
					{
						if (interpolate)
						{
							const double ratio = static_cast<double>(k - left) / static_cast<double>(right - left);
							value.at<double>(i, k) = leftValue + (rightValue - leftValue) * ratio;
						}
						else
						{
							value.at<double>(i, k) = ((k - left) <= (right - k)) ? leftValue : rightValue;
						}
					}
				}
			}
		}

		// 第二遍：按列，处理第一遍留下的、整行都取不到锚点的游程；仍填不了的写 nodata
		for (int j = 0; j < cols; ++j)
		{
			int i = 0;
			while (i < rows)
			{
				if (filled.at<uchar>(i, j) != 0) { ++i; continue; }
				const int first = i;
				while (i < rows && filled.at<uchar>(i, j) == 0) ++i;
				const int last = i - 1;
				const int up = first - 1;
				const int down = i;
				if (up >= 0 && down < rows &&
					std::isfinite(value.at<double>(up, j)) && std::isfinite(value.at<double>(down, j)))
				{
					const double upValue = value.at<double>(up, j);
					const double downValue = value.at<double>(down, j);
					for (int k = first; k <= last; ++k)
					{
						if (interpolate)
						{
							const double ratio = static_cast<double>(k - up) / static_cast<double>(down - up);
							value.at<double>(k, j) = upValue + (downValue - upValue) * ratio;
						}
						else
						{
							value.at<double>(k, j) = ((k - up) <= (down - k)) ? upValue : downValue;
						}
					}
				}
				else
				{
					for (int k = first; k <= last; ++k)
					{
						value.at<double>(k, j) = std::numeric_limits<double>::quiet_NaN();
					}
				}
			}
		}
	}
} // namespace

namespace
{
	// 椭球残差在「零多普勒平面圆」上的展开。
	//
	// 圆上的点 X(φ) = P + R·(cosφ·e1 + sinφ·e2)，椭球残差
	//     Q(φ) = (Xx² + Xy²)/aH² + Xz²/bH² − 1
	// 展开后是 (u, v) = (cosφ, sinφ) 的严格二次型：
	//     Q = A0 + A1·u + B1·v + A2·(u²−v²) + 2·B2·u·v
	// 于是 Q 本身、切向导数 Q'、二阶导 Q'' 全是闭式代数式，求根不需要任何 sin/cos/atan2。
	// 旧实现用「180 点扫描 + 40 次二分」，是 sarToGeo 的主要开销来源。
	struct PhiQuadratic
	{
		double A0, A1, B1, A2, B2;
	};

	// 行级常量：只依赖 P、V、e1、e2，与 R 和 aH/bH 无关，整个高度迭代复用。
	struct PhiRowConst
	{
		double sx1, sz1, sx2, sz2, sxy, sz12;   // e1·e1、e2·e2、e1·e2 的 (x,y) 分量与 z 分量
		double px1, pz1, px2, pz2, pp, pz;      // P·e1、P·e2、P·P 的 (x,y) 分量与 z 分量
	};

	inline void makePhiRowConst(const Position& P, double e1x, double e1y, double e1z,
		double e2x, double e2y, double e2z, PhiRowConst& k)
	{
		k.sx1 = e1x * e1x + e1y * e1y;
		k.sz1 = e1z * e1z;
		k.sx2 = e2x * e2x + e2y * e2y;
		k.sz2 = e2z * e2z;
		k.sxy = e1x * e2x + e1y * e2y;
		k.sz12 = e1z * e2z;
		k.px1 = P.x * e1x + P.y * e1y;
		k.pz1 = P.z * e1z;
		k.px2 = P.x * e2x + P.y * e2y;
		k.pz2 = P.z * e2z;
		k.pp = P.x * P.x + P.y * P.y;
		k.pz = P.z * P.z;
	}

	inline PhiQuadratic makePhiQuadratic(const PhiRowConst& k, double R, double aH, double bH)
	{
		const double ia = 1.0 / (aH * aH);
		const double ib = 1.0 / (bH * bH);
		const double alpha = k.sx1 * ia + k.sz1 * ib;      // e1ᵀMe1
		const double beta = k.sxy * ia + k.sz12 * ib;      // e1ᵀMe2
		const double gamma = k.sx2 * ia + k.sz2 * ib;      // e2ᵀMe2
		const double p1 = k.px1 * ia + k.pz1 * ib;         // PᵀMe1
		const double p2 = k.px2 * ia + k.pz2 * ib;         // PᵀMe2
		const double cc = k.pp * ia + k.pz * ib - 1.0;     // PᵀMP − 1
		const double R2 = R * R;
		PhiQuadratic o;
		o.A0 = cc + 0.5 * R2 * (alpha + gamma);
		o.A2 = 0.5 * R2 * (alpha - gamma);
		o.B2 = R2 * beta;
		o.A1 = 2.0 * R * p1;
		o.B1 = 2.0 * R * p2;
		return o;
	}

	// 圆上方向的固定采样表：kCount 个等分单位向量，连同 cos2φ、sin2φ 一起预计算。
	// 用查表代替「逐点算 sin/cos」——那正是旧实现「180 点扫描」的真实开销来源。
	// 表只在首次调用时构造一次（C++11 起函数内 static 的初始化是线程安全的）。
	struct PhiDirectionTable
	{
		enum { kCount = 64 };
		double u[kCount], v[kCount], cos2[kCount], sin2[kCount];
		PhiDirectionTable()
		{
			for (int k = 0; k < kCount; ++k)
			{
				const double phi = 2.0 * PI * k / kCount;
				u[k] = std::cos(phi);
				v[k] = std::sin(phi);
				cos2[k] = u[k] * u[k] - v[k] * v[k];   // cos2φ
				sin2[k] = 2.0 * u[k] * v[k];           // sin2φ
			}
		}
	};

	inline const PhiDirectionTable& phiDirectionTable()
	{
		static const PhiDirectionTable table;
		return table;
	}

	// Q 在单位向量 (u,v) 上的取值。注意：(u,v) 必须是单位向量。
	// Q 含常数项 A0 与线性项 A1·u + B1·v，不是齐次式（Q(λu,λv) ≠ λ²·Q(u,v)），
	// 所以用非单位向量代入得到的符号与它所在方向的符号无关，判符号会失效。
	inline double phiResidual(const PhiQuadratic& o, double u, double v)
	{
		return o.A0 + o.A1 * u + o.B1 * v + o.A2 * (u * u - v * v) + 2.0 * o.B2 * u * v;
	}

	// 64 个方向把圆周分成 5.625°。比旧实现的 180 点（2°）略粗，但实测中两个根相距 52°，
	// 框得住；24 步二分把括号收敛到 ~1e-9 rad，远超位置精度需求。
	const int kEllipsoidBisectSteps = 24;

	// 圆上的椭球交点。
	//
	// 旧实现是「180 点 sin/cos 扫描 + 40 次二分」，开销几乎全在三角函数上。
	//
	// 曾试过用「8 个种子各走 4 步牛顿」替代（残差对 (cosφ,sinφ) 是严格二次型、导数闭式），
	// 实测在 P·V≈0 的近圆轨道几何下 100% 失败：那种几何里 e1 是正东方向而卫星位置的正东
	// 分量近似为 0、e1ᵀMe2 ≡ 0，于是 Q 的一阶幅值只略大于 A0，两个根被挤到相隔约 52°，
	// 而 φ=±90° 恰是 Q 的驻点夹在两根之间——牛顿一步就跨过根、随后被驻点吸住，
	// 落在驻点上的种子更是永不移动。牛顿法对这种「两根既近、中间又有驻点」的形状不稳健，
	// 所以改回「扫描定括号 + 二分」，只把三角函数换成查表。
	inline int solveEllipsoidOnCircle(const PhiQuadratic& o, const PhiDirectionTable& dir,
		double* rootU, double* rootV, int maxRoots)
	{
		const int n = PhiDirectionTable::kCount;
		double q[PhiDirectionTable::kCount];
		for (int i = 0; i < n; ++i)
		{
			q[i] = o.A0 + o.A1 * dir.u[i] + o.B1 * dir.v[i] + o.A2 * dir.cos2[i] + o.B2 * dir.sin2[i];
		}

		int found = 0;
		for (int i = 0; i < n && found < maxRoots; ++i)
		{
			const int j = (i + 1 == n) ? 0 : (i + 1);
			if ((q[i] <= 0.0) == (q[j] <= 0.0)) continue;

			double uLo = dir.u[i], vLo = dir.v[i], qLo = q[i];
			double uHi = dir.u[j], vHi = dir.v[j];
			for (int step = 0; step < kEllipsoidBisectSteps; ++step)
			{
				// 中点必须归一化回圆上。Q 不是齐次式（有常数项与线性项），弦中点
				// 与其所在方向上的 Q 没有符号关系；不归一化会让结果偏约 0.07°，
				// 再经「天底分量」判据放大后选到镜像的那一侧。弦中点方向 = 精确角平分方向。
				double uMid = 0.5 * (uLo + uHi);
				double vMid = 0.5 * (vLo + vHi);
				const double nMid = std::sqrt(uMid * uMid + vMid * vMid);
				if (!(nMid > 0.0)) break;
				uMid /= nMid; vMid /= nMid;
				const double qMid = phiResidual(o, uMid, vMid);
				if ((qMid <= 0.0) == (qLo <= 0.0)) { uLo = uMid; vLo = vMid; qLo = qMid; }
				else { uHi = uMid; vHi = vMid; }
			}
			const double uR = 0.5 * (uLo + uHi);
			const double vR = 0.5 * (vLo + vHi);
			const double nR = std::sqrt(uR * uR + vR * vR);
			if (!(nR > 0.0)) continue;
			rootU[found] = uR / nR;
			rootV[found] = vR / nR;
			++found;
		}
		return found;
	}

} // namespace

int Utils::sarToGeo(
	Mat& stateVec,
	double prf,
	double rangeSpacing,
	double nearRangeTime,
	double startTime,
	double endTime,
	int sceneHeight,
	int sceneWidth,
	int offsetRow,
	int offsetCol,
	int caliAz,
	int caliRg,
	Mat& sourceRowMap,
	Mat& dem,
	double demLonUpperLeft,
	double demLatUpperLeft,
	double demSpacing,
	Mat& outLat,
	Mat& outLon,
	int sampleStride)
{
	if (stateVec.type() != CV_64F || stateVec.cols != 7 || stateVec.rows < 7 ||
		prf <= 0.0 || rangeSpacing <= 0.0 || nearRangeTime <= 0.0 ||
		sceneHeight < 1 || sceneWidth < 1)
	{
		fprintf(stderr, "sarToGeo(): input check failed!\n");
		return -1;
	}

	// 独立墙钟：只量 sarToGeo 自己，闭环自检不在其中，可以直接用来推算全分辨率的代价。
	const auto sar2geoClockStart = std::chrono::steady_clock::now();

	const double timeInterval = 1.0 / prf;
	const double deltaT = stateVec.at<double>(1, 0) - stateVec.at<double>(0, 0);
	orbitStateVectors orbit(stateVec, startTime, endTime, deltaT);
	orbit.applyOrbit();

	const double aAxis = 6378137.0;
	const double fInv = 298.257223563;
	const double bAxis = aAxis - aAxis / fInv;
	const double slant0 = nearRangeTime * VEL_C * 0.5;

	const int demRows = dem.rows;
	const int demCols = dem.cols;
	const bool hasDem = (dem.type() == CV_16S && demRows > 1 && demCols > 1 && demSpacing > 0.0);

	outLat.create(sceneHeight, sceneWidth, CV_64F);
	outLon.create(sceneHeight, sceneWidth, CV_64F);
	// 抽样模式下未计算的格子必须显式置 NaN：create() 不做初始化，
	// 否则调用方会把未初始化的内存当成结果拿去比对。
	outLat.setTo(std::numeric_limits<double>::quiet_NaN());
	outLon.setTo(std::numeric_limits<double>::quiet_NaN());

	const int sampleStep = (sampleStride > 0) ? sampleStride : 1;

	// 高度不动点迭代的收敛性统计。迭代 h ← DEM(pos(h)) 的导数是 tanα·cotθ（α=地形坡度，
	// θ=视角），地形坡度超过视角的地方——即 layover——必然发散；阻尼迭代跑满轮数仍未收敛时，
	// 写出的点与它自身位置上的 DEM 高度相差 residual，这个不自洽量直接换算成距离向误差。
	// 这样的点按 R4 约定写 NaN。闭环自检里那条距离向长尾就来自这里。
	// 用 double 累加是为了避开 MSVC OpenMP 对 long long reduction 的支持问题。
	double nConverged = 0.0, nDiverged = 0.0, nNoSample = 0.0;
	double nNoRoot = 0.0;                     // 圆上找不到椭球交点（几何无解或牛顿未收敛）
	double maxResidual = 0.0, sumResidual = 0.0;
	// 像素循环单独计时：输出表的分配与 NaN 填充是固定成本（与采样步长无关），
	// 混在一起会把全分辨率代价外推错。
	const auto sar2geoClockLoop = std::chrono::steady_clock::now();

#pragma omp parallel for schedule(guided) reduction(+:nConverged,nDiverged,nNoSample,nNoRoot,sumResidual)
	for (int r = 0; r < sceneHeight; r += sampleStep)
	{
		// 方位时刻必须用【去斜后的输出行 r】，不能用 sourceRowMap(r)。
		//
		// 去斜后的产品网格是时间均匀的：输出行 r 对应的方位时刻就是 startTime + r/PRI，
		// 这一点由上游几何多项式独立确认（它在 burst 接缝处完全连续，Δlat 恒为 +1.2334e-4 度/行）。
		// 而 sourceRowMap 在 5 道 burst 接缝处各跳过约 174 行（那是 burst 间的方位重叠），
		// 拿它当时间用会每道接缝多算 174 × 13.8849 m ≈ 2.40 km 的方位时间，
		// 地面位置随之向北跳 2.40 km 并逐道累积——实测相对上游多项式：
		// 接缝前 57 m、过 3 道 7193 m、过 5 道 11927 m，正是每道 2.40 km。
		//
		// sourceRowMap 对【读取源数据】仍然是必需的，错的只是把它当时间用；
		// 参数保留在签名里，本函数不再使用。
		const double t = startTime + static_cast<double>(r + offsetRow - caliAz) * timeInterval;

		Position P;
		Velocity V;
		if (orbit.getPosition(t, P) != 0 || orbit.getVelocity(t, V) != 0)
		{
			continue;   // 该行无解，留 NaN
		}

		// 零多普勒平面内的正交基：e1 ⊥ V，e2 = V × e1 / |V|
		const double vNorm = std::sqrt(V.vx * V.vx + V.vy * V.vy + V.vz * V.vz);
		if (!(vNorm > 0.0)) continue;
		double e1x = -V.vy, e1y = V.vx, e1z = 0.0;
		const double e1n = std::sqrt(e1x * e1x + e1y * e1y);
		if (!(e1n > 0.0)) continue;
		e1x /= e1n; e1y /= e1n; e1z /= e1n;
		const double e2x = (V.vy * e1z - V.vz * e1y) / vNorm;
		const double e2y = (V.vz * e1x - V.vx * e1z) / vNorm;
		const double e2z = (V.vx * e1y - V.vy * e1x) / vNorm;

		// 行级常量：椭球残差二次型的系数只通过 aH/bH 与 R 依赖高度和距离，
		// 其余点积在整行内不变。预计算后每轮求根只需 ~30 flop 组装 5 个系数。
		PhiRowConst rowConst;
		makePhiRowConst(P, e1x, e1y, e1z, e2x, e2y, e2z, rowConst);

		for (int c = 0; c < sceneWidth; c += sampleStep)
		{
			const double R = slant0 + static_cast<double>(c + offsetCol - caliRg) * rangeSpacing;

			// 高度迭代：先用 0 米椭球求交，再用 DEM 在该点的高度重求，直到收敛。
			// 注意不能用固定次数——陡地形上每次迭代的高度改变可达数十米，必须检查收敛，
			// 否则报出的点用的是上一轮的高度、与它所在位置的真实高度不符，会在距离向产生偏置。
			const double hTolerance = 0.5;      // 米
			const int maxHeightIterations = 12;
			double hTarget = 0.0;
			double hResidual = -1.0;   // 最后一轮的 |DEM(h) - h|；<0 表示始终没采到有效 DEM
			double omega = 1.0;            // R4 阻尼系数：坡度超过视角时 ω=1 会发散，见下方
			double prevAbsResidual = 0.0;  // 上一轮步长，用于发散判定
			double hitLat = std::numeric_limits<double>::quiet_NaN();
			double hitLon = std::numeric_limits<double>::quiet_NaN();

			for (int iter = 0; iter < maxHeightIterations; ++iter)
			{
				const double aH = aAxis + hTarget;
				const double bH = bAxis + hTarget;

				// 圆上的椭球残差 Q(φ) = (Xx²+Xy²)/aH² + Xz²/bH² − 1，X = P + R·(cosφ·e1 + sinφ·e2)
				// 它是 (cosφ, sinφ) 的严格二次型，查表扫描 + 二分求根，全程无三角函数。
				const PhiQuadratic quad = makePhiQuadratic(rowConst, R, aH, bH);
				double rootU[4], rootV[4];
				const int rootCount = solveEllipsoidOnCircle(quad, phiDirectionTable(), rootU, rootV, 4);
				if (rootCount == 0)
				{
					++nNoRoot;
					break;      // 该像素几何无解，留 NaN
				}

				// 两个交点取「天底方向分量最大」的那个（= 较小的视角），与旧实现完全一致。
				//
				// 注意：这里是刻意逐字照抄旧代码的算式（含 R 与 |P| 的除法），而不是用等价化简式
				// 「最小化 u·P·e1 + v·P·e2」。因为圆轨道下 P·V = 0 ⇒ 天底方向整条线都落在零多普勒
				// 平面内 ⇒ 半径 R 的圆关于天底线镜像对称 ⇒ 两个交点的天底分量在 double 里恰好相等，
				// 选解完全由浮点末位和「严格大于」的先后决定（实际取的是扫描序里的第一个）。
				// 换算式就会换末位，进而可能选中镜像的那一侧，产物会整体翻到航迹另一边。
				const double pNorm = std::sqrt(P.x * P.x + P.y * P.y + P.z * P.z);
				double bestU = rootU[0], bestV = rootV[0];
				double bestT = -std::numeric_limits<double>::max();
				for (int ri = 0; ri < rootCount; ++ri)
				{
					const double Xx = P.x + R * (rootU[ri] * e1x + rootV[ri] * e2x);
					const double Xy = P.y + R * (rootU[ri] * e1y + rootV[ri] * e2y);
					const double Xz = P.z + R * (rootU[ri] * e1z + rootV[ri] * e2z);
					const double down = -((Xx - P.x) * P.x + (Xy - P.y) * P.y + (Xz - P.z) * P.z) / pNorm;
					if (down > bestT) { bestT = down; bestU = rootU[ri]; bestV = rootV[ri]; }
				}

				const double Xx = P.x + R * (bestU * e1x + bestV * e2x);
				const double Xy = P.y + R * (bestU * e1y + bestV * e2y);
				const double Xz = P.z + R * (bestU * e1z + bestV * e2z);
				double hh = 0.0;
				if (xyz2ell(Xx, Xy, Xz, hitLat, hitLon, hh) != 0) break;

				// 用 DEM 在该点的高度作为下一轮的目标
				double hDem = 0.0;
				if (hasDem)
				{
					// demMapping 内部把 DEM 原点平移到像元中心（± 半像元），此处必须保持一致，
					// 否则高程采样偏半个 DEM 像元（~44 m 纬向 / ~32 m 经向），进而影响地面点位置。
					const double latOrigin = demLatUpperLeft + demSpacing * 0.5;
					const double lonOrigin = demLonUpperLeft - demSpacing * 0.5;
					const int di = static_cast<int>(std::floor((latOrigin - hitLat) / demSpacing));
					const int dj = static_cast<int>(std::floor((hitLon - lonOrigin) / demSpacing));
					if (di >= 0 && di < demRows - 1 && dj >= 0 && dj < demCols - 1)
					{
						const double fi = (latOrigin - hitLat) / demSpacing - di;
						const double fj = (hitLon - lonOrigin) / demSpacing - dj;
						const double v00 = dem.at<short>(di, dj);
						const double v01 = dem.at<short>(di, dj + 1);
						const double v10 = dem.at<short>(di + 1, dj);
						const double v11 = dem.at<short>(di + 1, dj + 1);
						if (v00 > -9000.0 && v01 > -9000.0 && v10 > -9000.0 && v11 > -9000.0)
						{
							hDem = (v00 * (1 - fi) + v10 * fi) * (1 - fj) + (v01 * (1 - fi) + v11 * fi) * fj;
							const double step = hDem - hTarget;
							const double hDelta = std::fabs(step);

							// R4 阻尼。不动点迭代 h ← DEM(pos(h)) 的导数是 tanα·cotθ（α=地形坡度，
							// θ=视角），坡度小于视角时是收缩的、ω=1 即可；坡度超过视角（layover）时
							// 导数大于 1，ω=1 必然发散——这正是距离向长尾的来源（实测占 8.075%）。
							// 判据用「步长比上一轮还大」，此时把 ω 减半并回退本步，让迭代重新落回收缩域。
							if (prevAbsResidual > 0.0 && hDelta > prevAbsResidual) omega *= 0.5;
							prevAbsResidual = hDelta;

							hTarget += omega * step;
							hResidual = hDelta;
							if (hDelta < hTolerance) break;   // 收敛：本轮解与 DEM 高度自洽
						}
					}
				}
				else
				{
					break;
				}
			}

			// R4 决定：阻尼后仍未收敛的点（layover 发散）与它自身位置上的 DEM 高度不自洽，
			// 距真实位置最多差 hResidual，写出去就是错的——写 NaN，不做假。
			const bool hDiverged = (hResidual >= hTolerance);
			if (hDiverged)
			{
				outLat.at<double>(r, c) = std::numeric_limits<double>::quiet_NaN();
				outLon.at<double>(r, c) = std::numeric_limits<double>::quiet_NaN();
			}
			else
			{
				outLat.at<double>(r, c) = hitLat;
				outLon.at<double>(r, c) = hitLon;
			}

			if (hResidual < 0.0)
			{
				++nNoSample;   // DEM 越界 / 四邻含 nodata / 几何无解
			}
			else if (hDiverged)
			{
				++nDiverged;
				sumResidual += hResidual;
				// reduction(max:) 是 OpenMP 3.1 才有的，MSVC 的 /openmp 只到 2.0，
				// 这里用 critical 保护；只有发散像素才走到这，开销可忽略。
#pragma omp critical
				{
					if (hResidual > maxResidual) maxResidual = hResidual;
				}
			}
			else
			{
				++nConverged;
			}
		}
	}

	const double nStatPx = nConverged + nDiverged + nNoSample;
	const double statDen = (nStatPx > 0.0) ? nStatPx : 1.0;
	const auto sar2geoClockEnd = std::chrono::steady_clock::now();
	const long long sar2geoSetupMs = static_cast<long long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(sar2geoClockLoop - sar2geoClockStart).count());
	const long long sar2geoLoopMs = static_cast<long long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(sar2geoClockEnd - sar2geoClockLoop).count());
	// loop_ms 只含像素循环，乘 (全网格像素数 / 本次采样像素数) 才是全分辨率的求解代价；
	// setup_ms 是输出表的分配与 NaN 填充，与采样步长无关，不能一起外推。
	fprintf(stderr, "sarToGeo(): height iteration over %.0f px: converged=%.3f%%, diverged=%.3f%% "
	                "(residual max=%.1f m mean=%.1f m), no-sample=%.3f%% (of which no-root %.3f%%), setup=%lld ms loop=%lld ms\n",
	        nStatPx,
	        100.0 * nConverged / statDen,
	        100.0 * nDiverged / statDen,
	        maxResidual,
	        (nDiverged > 0.0) ? (sumResidual / nDiverged) : 0.0,
	        100.0 * nNoSample / statDen,
	        100.0 * nNoRoot / statDen,
	        sar2geoSetupMs,
	        sar2geoLoopMs);
	fprintf(stderr, "sarToGeo(): done %dx%d\n", sceneHeight, sceneWidth);
	return 0;
}

namespace
{
	// ---- 逆向重采样：粗网格索引 + 局部单元求逆 ----

	// 表的双线性单元：2×2 四角的经纬度
	struct Sar2GeoCell
	{
		double lat00, lat10, lat01, lat11;
		double lon00, lon10, lon01, lon11;
	};

	// 读单元四角。任一非有限则返回 false —— 含 NaN 的单元不能用来求逆，结果无意义。
	inline bool loadSar2GeoCell(const Mat& lat, const Mat& lon, int r0, int c0, Sar2GeoCell& cell)
	{
		cell.lat00 = lat.at<double>(r0, c0);
		cell.lat10 = lat.at<double>(r0 + 1, c0);
		cell.lat01 = lat.at<double>(r0, c0 + 1);
		cell.lat11 = lat.at<double>(r0 + 1, c0 + 1);
		cell.lon00 = lon.at<double>(r0, c0);
		cell.lon10 = lon.at<double>(r0 + 1, c0);
		cell.lon01 = lon.at<double>(r0, c0 + 1);
		cell.lon11 = lon.at<double>(r0 + 1, c0 + 1);
		return std::isfinite(cell.lat00) && std::isfinite(cell.lat10) &&
		       std::isfinite(cell.lat01) && std::isfinite(cell.lat11) &&
		       std::isfinite(cell.lon00) && std::isfinite(cell.lon10) &&
		       std::isfinite(cell.lon01) && std::isfinite(cell.lon11);
	}

	// 在单元内解 (s,t)（以 (r0,c0) 为原点的分数坐标），使双线性插值等于 (latT, lonT)。
	// 先用线性项给初值，再用含交叉项的完整双线性式做牛顿修正。
	// 注意 s/t 允许落到 [0,1] 之外——调用方拿这个外推量直接跳格，比一格一格挪快得多。
	inline bool invertSar2GeoCell(const Sar2GeoCell& cell, double latT, double lonT, double& s, double& t)
	{
		const double a = cell.lat10 - cell.lat00;   // ∂lat/∂s 的线性部分
		const double b = cell.lat01 - cell.lat00;   // ∂lat/∂t
		const double c = cell.lon10 - cell.lon00;
		const double d = cell.lon01 - cell.lon00;
		const double det = a * d - b * c;
		if (!(std::fabs(det) > 1e-16)) return false;   // 单元退化

		const double u = latT - cell.lat00;
		const double v = lonT - cell.lon00;
		s = (d * u - b * v) / det;
		t = (-c * u + a * v) / det;

		const double kla = cell.lat11 - cell.lat10 - cell.lat01 + cell.lat00;   // 交叉项系数
		const double klo = cell.lon11 - cell.lon10 - cell.lon01 + cell.lon00;

		for (int it = 0; it < 4; ++it)
		{
			const double glat = cell.lat00 + s * a + t * b + s * t * kla - latT;
			const double glon = cell.lon00 + s * c + t * d + s * t * klo - lonT;
			const double j11 = a + t * kla, j12 = b + s * kla;
			const double j21 = c + t * klo, j22 = d + s * klo;
			const double jdet = j11 * j22 - j12 * j21;
			if (!(std::fabs(jdet) > 1e-20)) return false;
			const double ds = (j22 * glat - j12 * glon) / jdet;
			const double dt = (-j21 * glat + j11 * glon) / jdet;
			s -= ds; t -= dt;
			if (std::fabs(ds) < 1e-10 && std::fabs(dt) < 1e-10) break;
		}
		return std::isfinite(s) && std::isfinite(t);
	}

	// 源数据一次取值（统一转 double）并做 nodata 判定。
	// sourceNoData 传 NaN 时该判定恒不成立，等价于关闭。
	inline bool sar2GeoSourceValue(const Mat& src, int r, int c, double sourceNoData, double& out)
	{
		if (src.type() == CV_64F) out = src.at<double>(r, c);
		else if (src.type() == CV_32F) out = double(src.at<float>(r, c));
		else out = double(src.at<short>(r, c));
		return std::isfinite(out) && !(out == sourceNoData);
	}

	// 每个桶覆盖的 SAR 行列范围；rowMin > rowMax 表示空桶
	struct Sar2GeoBucket
	{
		int rowMin, rowMax, colMin, colMax;
	};
}

int Utils::sar2GeoInverseSample(
	const Mat& sar2GeoLat,
	const Mat& sar2GeoLon,
	const Mat& mappedLon,
	const Mat& mappedLat,
	const Mat& source,
	Mat& outMapped,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south,
	double sourceNoData,
	int bucketSarPixels)
{
	if (sar2GeoLat.type() != CV_64F || sar2GeoLon.type() != CV_64F) return -1;
	if (sar2GeoLat.rows < 2 || sar2GeoLat.cols < 2) return -1;
	if (sar2GeoLat.rows != sar2GeoLon.rows || sar2GeoLat.cols != sar2GeoLon.cols) return -1;
	if (mappedLon.rows != sar2GeoLat.rows || mappedLon.cols != sar2GeoLat.cols) return -1;
	if (mappedLat.rows != sar2GeoLat.rows || mappedLat.cols != sar2GeoLat.cols) return -1;
	if ((mappedLon.type() != CV_32F && mappedLon.type() != CV_64F) ||
	    (mappedLat.type() != CV_32F && mappedLat.type() != CV_64F)) return -1;
	if (source.rows != sar2GeoLat.rows || source.cols != sar2GeoLat.cols) return -1;
	if (source.channels() != 1) return -1;
	if (source.type() != CV_64F && source.type() != CV_32F && source.type() != CV_16S) return -1;
	if (bucketSarPixels < 1) bucketSarPixels = 16;

	const int H = sar2GeoLat.rows;
	const int W = sar2GeoLat.cols;

	// ---- 输出网格：口径与 SAR2UTM 第一重载完全一致（同一个估计器与同一个拒绝条件）----
	// mapped_lon/mapped_lat 是 demMapping 的产物，已经过补洞与平滑、本身没有 NaN，
	// 所以这里可以直接把它的返回值当健全性检查。
	GeographicGridStats gridStats;
	if (!scanGeographicGrid(mappedLon, mappedLat, gridStats)) return -1;
	if (gridStats.validCount == 0) return -1;

	GeographicGridSpacing gridSpacing;
	if (!estimateGeographicGridSpacing("sar2GeoInverseSample", mappedLon, mappedLat, gridSpacing)) return -1;
	const double lon_interval = gridSpacing.lonInterval;
	const double lat_interval = gridSpacing.latInterval;

	const double rawLonSpan = gridStats.maxLon - gridStats.minLon;
	const double lonSpan = rawLonSpan > 180.0 ? 360.0 - rawLonSpan : rawLonSpan;
	const double latSpan = gridStats.maxLat - gridStats.minLat;
	if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan < 0.0 || latSpan < 0.0 ||
	    lonSpan > 30.0 || latSpan > 30.0) return -1;

	const double west = rawLonSpan > 180.0 ? gridStats.maxLon : gridStats.minLon;
	int outRows = 0, outCols = 0;
	if (!checkedSar2UtmDimensions(latSpan / lat_interval + 2.0, lonSpan / lon_interval + 2.0, outRows, outCols))
	{
		fprintf(stderr,
			"sar2GeoInverseSample(): output grid rejected: latSpan=%.17g lonSpan=%.17g intervals=[%.17g,%.17g] budget=%llu bytes.\n",
			latSpan, lonSpan, lon_interval, lat_interval,
			static_cast<unsigned long long>(kSar2UtmOutputMemoryBudgetBytes));
		return -1;
	}
	const double north = gridStats.maxLat;
	const double southLat = gridStats.maxLat - static_cast<double>(outRows - 1) * lat_interval;
	double east = west + static_cast<double>(outCols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	if (lon_west) *lon_west = west;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = southLat;

	// ---- 粗网格索引：按 (lat,lon) 分桶，桶内记录落入的 SAR 行列范围 ----
	// 索引的是 sar2geo 表本身，所以桶网格覆盖它的包围盒（不是 mapped 表的）。
	// sar2geo 表自带约 3% 的 NaN（layover 发散点按 R4 约定置 NaN），scanGeographicGrid
	// 会因此返回 false（它的契约是「干净网格」），但统计量对 NaN 是逐像元正确跳过的，
	// 所以这里忽略返回值取包围盒。
	GeographicGridStats tableStats;
	scanGeographicGrid(sar2GeoLon, sar2GeoLat, tableStats);
	if (tableStats.validCount == 0) return -1;
	if (tableStats.maxLon - tableStats.minLon > 180.0) return -1;   // 桶网格不支持跨 ±180
	const double tableLatSpan = tableStats.maxLat - tableStats.minLat;
	const double tableLonSpan = tableStats.maxLon - tableStats.minLon;

	// 桶边长取 bucketSarPixels 个 SAR 像素的地面采样间距，使桶大致是方的。
	const double bucketDegLat = double(bucketSarPixels) * lat_interval;
	const double bucketDegLon = double(bucketSarPixels) * lon_interval;
	if (!(bucketDegLat > 0.0) || !(bucketDegLon > 0.0)) return -1;

	int nbLat = static_cast<int>(std::ceil(tableLatSpan / bucketDegLat)) + 1;
	int nbLon = static_cast<int>(std::ceil(tableLonSpan / bucketDegLon)) + 1;
	if (nbLat < 1) nbLat = 1;
	if (nbLon < 1) nbLon = 1;
	// 索引本身也占内存，超过上限就等比缩小（桶变大，只影响种子质量，不影响正确性）
	static const long long kMaxBuckets = 4000000;
	if (static_cast<long long>(nbLat) * static_cast<long long>(nbLon) > kMaxBuckets)
	{
		const double shrink = std::sqrt(double(kMaxBuckets) /
			(double(nbLat) * double(nbLon)));
		nbLat = (std::max)(1, static_cast<int>(double(nbLat) * shrink));
		nbLon = (std::max)(1, static_cast<int>(double(nbLon) * shrink));
	}

	std::vector<Sar2GeoBucket> buckets(static_cast<size_t>(nbLat) * static_cast<size_t>(nbLon));
	for (size_t k = 0; k < buckets.size(); ++k)
	{
		buckets[k].rowMin = (std::numeric_limits<int>::max)();
		buckets[k].rowMax = -1;
		buckets[k].colMin = (std::numeric_limits<int>::max)();
		buckets[k].colMax = -1;
	}

	// 建桶用抽样（步长 2）：桶的包围盒不需要每一个像元，抽样后种子最多偏 1~2 个像元，
	// 而反算的跳格机制本来就能吸收这个量级。全采样要多付一倍的 203M 次遍历。
	const int kBuildStride = 2;
	for (int r = 0; r < H; r += kBuildStride)
	{
		for (int c = 0; c < W; c += kBuildStride)
		{
			const double la = sar2GeoLat.at<double>(r, c);
			const double lo = sar2GeoLon.at<double>(r, c);
			if (!std::isfinite(la) || !std::isfinite(lo)) continue;   // NaN 必须跳过，否则 min/max 被毒化
			int bi = static_cast<int>(std::floor((tableStats.maxLat - la) / bucketDegLat));
			int bj = static_cast<int>(std::floor((lo - tableStats.minLon) / bucketDegLon));
			if (bi < 0) bi = 0; else if (bi >= nbLat) bi = nbLat - 1;
			if (bj < 0) bj = 0; else if (bj >= nbLon) bj = nbLon - 1;
			Sar2GeoBucket& b = buckets[static_cast<size_t>(bi) * static_cast<size_t>(nbLon) + static_cast<size_t>(bj)];
			if (r < b.rowMin) b.rowMin = r;
			if (r > b.rowMax) b.rowMax = r;
			if (c < b.colMin) b.colMin = c;
			if (c > b.colMax) b.colMax = c;
		}
	}

	// ---- 逐输出像素反算 + 双线性采样 ----
	outMapped.create(outRows, outCols, CV_64F);
	outMapped.setTo(std::numeric_limits<double>::quiet_NaN());

	double nNoBucket = 0.0, nNoInverse = 0.0, nNoSource = 0.0, nWritten = 0.0;

#pragma omp parallel for schedule(guided) reduction(+:nNoBucket,nNoInverse,nNoSource,nWritten)
	for (int i = 0; i < outRows; ++i)
	{
		const double latT = north - static_cast<double>(i) * lat_interval;
		int bi = static_cast<int>(std::floor((tableStats.maxLat - latT) / bucketDegLat));
		if (bi < 0) bi = 0; else if (bi >= nbLat) bi = nbLat - 1;
		const Sar2GeoBucket* bucketRow = &buckets[static_cast<size_t>(bi) * static_cast<size_t>(nbLon)];

		for (int j = 0; j < outCols; ++j)
		{
			const double lonT = west + static_cast<double>(j) * lon_interval;
			int bj = static_cast<int>(std::floor((lonT - tableStats.minLon) / bucketDegLon));
			if (bj < 0) bj = 0; else if (bj >= nbLon) bj = nbLon - 1;
			const Sar2GeoBucket& bucket = bucketRow[bj];
			if (bucket.rowMax < 0 || bucket.colMax < 0) { ++nNoBucket; continue; }   // 空桶

			// 种子取桶包围盒中心；映射是光滑的，后续用线性外推量直接跳格收敛很快。
			double r0 = 0.5 * double(bucket.rowMin + bucket.rowMax);
			double c0 = 0.5 * double(bucket.colMin + bucket.colMax);
			const int seedR = int(r0), seedC = int(c0);

			bool found = false;
			double hitR = 0.0, hitC = 0.0;
			for (int attempt = 0; attempt < 8 && !found; ++attempt)
			{
				int cr = int(std::floor(r0));
				int cc = int(std::floor(c0));
				if (cr < 0) cr = 0; else if (cr > H - 2) cr = H - 2;
				if (cc < 0) cc = 0; else if (cc > W - 2) cc = W - 2;

				Sar2GeoCell cell;
				if (loadSar2GeoCell(sar2GeoLat, sar2GeoLon, cr, cc, cell))
				{
					double s = 0.0, t = 0.0;
					if (invertSar2GeoCell(cell, latT, lonT, s, t))
					{
						if (s >= -0.001 && s <= 1.001 && t >= -0.001 && t <= 1.001)
						{
							hitR = double(cr) + s;
							hitC = double(cc) + t;
							found = true;
							break;
						}
						// 解落在格外：用线性外推量直接跳格（限制单步幅度防跑飞）
						double js = std::floor(s), jt = std::floor(t);
						if (js > 64.0) js = 64.0; else if (js < -64.0) js = -64.0;
						if (jt > 64.0) jt = 64.0; else if (jt < -64.0) jt = -64.0;
						if (js == 0.0 && jt == 0.0) break;   // 无进展，防死循环
						r0 = double(cr) + js;
						c0 = double(cc) + jt;
						continue;
					}
				}
				// 单元含 NaN 或求逆退化：依次试种子格的四个邻格
				static const int kOffR[4] = { 1, -1, 0, 0 };
				static const int kOffC[4] = { 0, 0, 1, -1 };
				r0 = double(seedR + kOffR[attempt & 3]);
				c0 = double(seedC + kOffC[attempt & 3]);
			}

			if (!found) { ++nNoInverse; continue; }
			if (!(hitR >= 0.0 && hitR <= double(H - 1) && hitC >= 0.0 && hitC <= double(W - 1)))
			{
				++nNoInverse;
				continue;
			}

			const int sr = int(std::floor(hitR));
			const int sc = int(std::floor(hitC));
			if (sr < 0 || sr + 1 >= H || sc < 0 || sc + 1 >= W) { ++nNoInverse; continue; }

			double v00 = 0.0, v01 = 0.0, v10 = 0.0, v11 = 0.0;
			if (!sar2GeoSourceValue(source, sr, sc, sourceNoData, v00) ||
			    !sar2GeoSourceValue(source, sr, sc + 1, sourceNoData, v01) ||
			    !sar2GeoSourceValue(source, sr + 1, sc, sourceNoData, v10) ||
			    !sar2GeoSourceValue(source, sr + 1, sc + 1, sourceNoData, v11))
			{
				++nNoSource;   // 窗口触及 NaN/nodata → 整点 nodata，不做合成
				continue;
			}

			const double fr = hitR - double(sr);
			const double fc = hitC - double(sc);
			outMapped.at<double>(i, j) =
				(v00 * (1.0 - fr) + v10 * fr) * (1.0 - fc) + (v01 * (1.0 - fr) + v11 * fr) * fc;
			++nWritten;
		}
	}

	const double total = double(outRows) * double(outCols);
	const double den = (total > 0.0) ? total : 1.0;
	fprintf(stderr,
		"sar2GeoInverseSample(): out %dx%d, written=%.3f%% (empty-bucket=%.3f%% inverse-failed=%.3f%% source-nodata=%.3f%%), buckets=%dx%d\n",
		outRows, outCols,
		100.0 * nWritten / den, 100.0 * nNoBucket / den, 100.0 * nNoInverse / den, 100.0 * nNoSource / den,
		nbLat, nbLon);
	return 0;
}

int Utils::estimateSar2UtmGridFromGeometry(
	Mat& lat_coefficient,
	Mat& lon_coefficient,
	int sceneHeight,
	int sceneWidth,
	int offset_row,
	int offset_col,
	int* requestedRows,
	int* requestedCols,
	unsigned long long* requiredBytes,
	unsigned long long* budgetBytes)
{
	if (lon_coefficient.rows != 1 || lon_coefficient.cols != 32 || lon_coefficient.type() != CV_64F ||
		lat_coefficient.rows != 1 || lat_coefficient.cols != 32 || lat_coefficient.type() != CV_64F ||
		sceneHeight < 2 || sceneWidth < 2 ||
		!requestedRows || !requestedCols || !requiredBytes || !budgetBytes)
	{
		fprintf(stderr, "estimateSar2UtmGridFromGeometry(): input check failed!\n");
		return -1;
	}

	// 与 SAR2UTM 内部相同的采样密度：129x129 个锚点均匀铺在场景上。
	// 关键点：锚点可以稀疏铺开，但梯度的步长必须是 1 个场景像元，
	// 否则量到的是"跨越几十行"的位移，间距会被放大上百倍、网格被算小上百倍。
	const int samples = kSar2UtmGradientSamplesPerAxis;
	const int latticeCount = samples * samples;
	const int totalCount = latticeCount * 3 + 4;   // 锚点 + 下一行 + 下一列，末尾追加四角

	// 步长取整，保证 anchor+1 始终落在场景内
	const int rowStep = (std::max)(1, (sceneHeight - 1) / (samples - 1));
	const int colStep = (std::max)(1, (sceneWidth - 1) / (samples - 1));

	Mat rows(totalCount, 1, CV_64F);
	Mat cols(totalCount, 1, CV_64F);
	for (int i = 0; i < samples; ++i)
	{
		const double row = static_cast<double>(offset_row) + static_cast<double>(i * rowStep);
		for (int j = 0; j < samples; ++j)
		{
			const double column = static_cast<double>(offset_col) + static_cast<double>(j * colStep);
			const int k = i * samples + j;
			rows.at<double>(k, 0) = row;                            // 锚点
			cols.at<double>(k, 0) = column;
			rows.at<double>(latticeCount + k, 0) = row + 1.0;        // 下一行（步长 1 像元）
			cols.at<double>(latticeCount + k, 0) = column;
			rows.at<double>(2 * latticeCount + k, 0) = row;          // 下一列（步长 1 像元）
			cols.at<double>(2 * latticeCount + k, 0) = column + 1.0;
		}
	}
	// 四角：与 computeImageGeoBoundry 取跨度的口径一致（不含它额外的 DEM 取数外扩）
	const int cornerBase = 3 * latticeCount;
	rows.at<double>(cornerBase + 0, 0) = offset_row;
	cols.at<double>(cornerBase + 0, 0) = offset_col;
	rows.at<double>(cornerBase + 1, 0) = offset_row;
	cols.at<double>(cornerBase + 1, 0) = offset_col + sceneWidth;
	rows.at<double>(cornerBase + 2, 0) = offset_row + sceneHeight;
	cols.at<double>(cornerBase + 2, 0) = offset_col;
	rows.at<double>(cornerBase + 3, 0) = offset_row + sceneHeight;
	cols.at<double>(cornerBase + 3, 0) = offset_col + sceneWidth;

	Mat lon, lat;
	Utils util;
	if (util.coord_conversion(lon_coefficient, rows, cols, lon) != 0) return -1;
	if (util.coord_conversion(lat_coefficient, rows, cols, lat) != 0) return -1;
	if (lon.total() != static_cast<size_t>(totalCount) || lat.total() != static_cast<size_t>(totalCount)) return -1;
	lon = lon.reshape(1, 1);
	lat = lat.reshape(1, 1);

	// 跨度：对全部采样点（含四角）取极值
	GeographicGridStats stats;
	if (!scanGeographicGrid(lon, lat, stats)) return -1;
	const double rawLonSpan = stats.maxLon - stats.minLon;
	const double lonSpan = rawLonSpan > 180.0 ? 360.0 - rawLonSpan : rawLonSpan;
	const double latSpan = stats.maxLat - stats.minLat;
	if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan < 0.0 || latSpan < 0.0 ||
		lonSpan > 30.0 || latSpan > 30.0)
	{
		return -1;
	}

	// 间距：锚点 -> 下一行 / 下一列，复用与 SAR2UTM 内部同一套梯度累积与中位数口径
	vector<double> rowLonComponents, rowLatComponents, columnLonComponents, columnLatComponents;
	vector<double> rowSpacings, columnSpacings;
	for (int k = 0; k < latticeCount; ++k)
	{
		const double baseLon = lon.at<double>(0, k);
		const double baseLat = lat.at<double>(0, k);
		appendGeographicGradient(baseLon, baseLat,
			lon.at<double>(0, latticeCount + k), lat.at<double>(0, latticeCount + k),
			rowLonComponents, rowLatComponents, rowSpacings);
		appendGeographicGradient(baseLon, baseLat,
			lon.at<double>(0, 2 * latticeCount + k), lat.at<double>(0, 2 * latticeCount + k),
			columnLonComponents, columnLatComponents, columnSpacings);
	}

	GeographicGridSpacing spacing;
	spacing.lonInterval = medianPositiveValue(columnLonComponents);   // 列间距 ← 列方向经度增量
	spacing.latInterval = medianPositiveValue(rowLatComponents);      // 行间距 ← 行方向纬度增量
	spacing.rowSpacingMeters = medianPositiveValue(rowSpacings);
	spacing.columnSpacingMeters = medianPositiveValue(columnSpacings);
	if (!(std::isfinite(spacing.lonInterval) && std::isfinite(spacing.latInterval) &&
		std::isfinite(spacing.rowSpacingMeters) && std::isfinite(spacing.columnSpacingMeters) &&
		spacing.lonInterval > 0.0 && spacing.latInterval > 0.0))
	{
		return -1;
	}
	fprintf(stderr,
		"SAR2UTM(preflight): samples=%d step=[%d,%d] rowSpacingMeters=%.17g columnSpacingMeters=%.17g lonInterval=%.17g latInterval=%.17g\n",
		latticeCount, rowStep, colStep, spacing.rowSpacingMeters, spacing.columnSpacingMeters,
		spacing.lonInterval, spacing.latInterval);

	const double rowsRequested = latSpan / spacing.latInterval + 2.0;
	const double colsRequested = lonSpan / spacing.lonInterval + 2.0;
	if (!std::isfinite(rowsRequested) || !std::isfinite(colsRequested) ||
		rowsRequested < 1.0 || colsRequested < 1.0 ||
		rowsRequested > static_cast<double>((std::numeric_limits<int>::max)()) ||
		colsRequested > static_cast<double>((std::numeric_limits<int>::max)()))
	{
		return -1;
	}

	*requestedRows = static_cast<int>(std::ceil(rowsRequested));
	*requestedCols = static_cast<int>(std::ceil(colsRequested));
	*requiredBytes = static_cast<unsigned long long>(*requestedRows) *
		static_cast<unsigned long long>(*requestedCols) * kSar2UtmOutputBytesPerPixel;
	*budgetBytes = static_cast<unsigned long long>(kSar2UtmOutputMemoryBudgetBytes);
	return 0;
}

int Utils::SAR2UTM(
	Mat& mapped_lon,
	Mat& mapped_lat,
	Mat& phase,
	Mat& mapped_phase,
	int interpolation_method,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south
)
{
	logSar2UtmInput("SAR2UTM", mapped_lon, mapped_lat, phase);
	// 阶段埋点：SAR2UTM 内部耗时此前完全不可见（调用方只报总耗时），
	// 而它包含一次全图坐标扫描、一次全图散射和一次四方向搜索填充，代价结构完全不同。
	const auto sar2utmPhaseStart = std::chrono::steady_clock::now();
	const auto sar2utmPhaseMs = [](const std::chrono::steady_clock::time_point& from,
		const std::chrono::steady_clock::time_point& to) {
		return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
	};
	if (mapped_lat.size() != mapped_lon.size() ||
		mapped_lat.size() != phase.size() ||
		phase.rows < 4 ||
		phase.cols < 2 ||
		phase.type() != CV_64F ||
		mapped_lat.type() != mapped_lon.type() ||
		(mapped_lon.type() != CV_32F && mapped_lon.type() != CV_64F)
		)
	{
		fprintf(stderr, "SAR2UTM(): input check failed!\n");
		return -1;
	}
	GeographicGridStats coordinateStats;
	const bool validCoordinates = scanGeographicGrid(mapped_lon, mapped_lat, coordinateStats);
	logGeographicGridStats("SAR2UTM", mapped_lon, mapped_lat, coordinateStats);
	if (!validCoordinates || coordinateStats.missingCount != 0)
	{
		fprintf(stderr, "SAR2UTM(): mapped coordinates contain missing, non-finite, or out-of-range values.\n");
		return -1;
	}
	const double min_lon = coordinateStats.minLon;
	const double max_lon = coordinateStats.maxLon;
	const double min_lat = coordinateStats.minLat;
	const double max_lat = coordinateStats.maxLat;
	const double rawLonSpan = max_lon - min_lon;
	const double lonSpan = rawLonSpan > 180.0 ? 360.0 - rawLonSpan : rawLonSpan;
	const double latSpan = max_lat - min_lat;
	if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan < 0.0 || latSpan < 0.0 ||
		lonSpan > 30.0 || latSpan > 30.0)
	{
		fprintf(stderr, "SAR2UTM(): mapped coordinate span is unsupported: lonSpan=%.17g latSpan=%.17g.\n", lonSpan, latSpan);
		return -1;
	}
	double west = max_lon - min_lon > 180.0 ? max_lon : min_lon;
	if (lon_west) *lon_west = west;
	
	GeographicGridSpacing geographicSpacing;
	if (!estimateGeographicGridSpacing("SAR2UTM", mapped_lon, mapped_lat, geographicSpacing))
	{
		fprintf(stderr, "SAR2UTM(): source coordinate grid is degenerate.\n");
		return -1;
	}
	const double lon_interval = geographicSpacing.lonInterval;
	const double lat_interval = geographicSpacing.latInterval;
	const auto sar2utmPhaseScan = std::chrono::steady_clock::now();
	fprintf(stderr, "SAR2UTM(): phase scan+estimate=%lld ms (in %dx%d, type=%d)\n",
		sar2utmPhaseMs(sar2utmPhaseStart, sar2utmPhaseScan), mapped_lon.rows, mapped_lon.cols, mapped_lon.type());

	int rows = phase.rows; int cols = phase.cols;
	

	//计算UTM坐标系相位尺寸
	int UTM_rows = 0;
	int UTM_cols = 0;
	const double requestedRows = latSpan / lat_interval + 2.0;
	const double requestedCols = lonSpan / lon_interval + 2.0;
	if (!checkedSar2UtmDimensions(requestedRows, requestedCols, UTM_rows, UTM_cols))
	{
		fprintf(stderr,
			"SAR2UTM(): output grid rejected: requestedRows=%.17g requestedCols=%.17g intervals=[%.17g,%.17g] spans=[%.17g,%.17g] budget=%llu bytes.\n",
			requestedRows, requestedCols, lon_interval, lat_interval, lonSpan, latSpan,
			static_cast<unsigned long long>(kSar2UtmOutputMemoryBudgetBytes));
		return -1;
	}
	double south = max_lat - (double)(UTM_rows - 1) * lat_interval;
	double north = max_lat;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = south;
	double east = west + (double)(UTM_cols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	try
	{
		Mat b_filled(UTM_rows, UTM_cols, CV_8U); b_filled = 0;
		mapped_phase.create(UTM_rows, UTM_cols, CV_64F); mapped_phase = 0.0;
	//开始地理编码
	if (mapped_lat.type() == CV_32F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				double lon, lat;
				lon = mapped_lon.at<float>(i, j);
				lat = mapped_lat.at<float>(i, j);
				const double rowValue = round((lat - min_lat) / lat_interval);
				lon = fabs(lon - west);
				lon = lon > 180.0 ? 360.0 - lon : lon;
				const double columnValue = round(lon / lon_interval);
				if (rowValue < 0.0 || rowValue >= UTM_rows || columnValue < 0.0 || columnValue >= UTM_cols) continue;
				const int row = static_cast<int>(rowValue);
				const int col = static_cast<int>(columnValue);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				// 1 = 有效锚点；2 = NaN（保留 nodata，但绝不作为填充的锚点，
				// 否则 NaN 会随插值向周围空洞扩散，形成成带的斑块边界）
				b_filled.at<uchar>(row, col) = std::isfinite(phase.at<double>(i, j)) ? 1 : 2;
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
				double lon, lat;
				lon = mapped_lon.at<double>(i, j);
				lat = mapped_lat.at<double>(i, j);
				const double rowValue = round((lat - min_lat) / lat_interval);
				lon = fabs(lon - west);
				lon = lon > 180.0 ? 360.0 - lon : lon;
				const double columnValue = round(lon / lon_interval);
				if (rowValue < 0.0 || rowValue >= UTM_rows || columnValue < 0.0 || columnValue >= UTM_cols) continue;
				const int row = static_cast<int>(rowValue);
				const int col = static_cast<int>(columnValue);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				// 1 = 有效锚点；2 = NaN（保留 nodata，但绝不作为填充的锚点，
				// 否则 NaN 会随插值向周围空洞扩散，形成成带的斑块边界）
				b_filled.at<uchar>(row, col) = std::isfinite(phase.at<double>(i, j)) ? 1 : 2;
			}
		}
	}
	const auto sar2utmPhaseScatter = std::chrono::steady_clock::now();
	fprintf(stderr, "SAR2UTM(): phase alloc+scatter=%lld ms (out %dx%d)\n",
		sar2utmPhaseMs(sar2utmPhaseScan, sar2utmPhaseScatter), UTM_rows, UTM_cols);

	// 顺序填充：O(行×列)，与空洞游程长度无关。
	// 取代下面原有的「每个洞向四个方向搜索」实现（代价 Σ L(L+1)，实测 7552 s，见 §2.1）。
	fillRunsSequential(mapped_phase, b_filled, interpolation_method != 0);
	

	const auto sar2utmPhaseFill = std::chrono::steady_clock::now();
	fprintf(stderr, "SAR2UTM(): phase fill=%lld ms\n", sar2utmPhaseMs(sar2utmPhaseScatter, sar2utmPhaseFill));
	cv::flip(mapped_phase, mapped_phase, 0);
	return 0;
	}
	catch (const cv::Exception& exception)
	{
		mapped_phase.release();
		fprintf(stderr, "SAR2UTM(): OpenCV exception: %s\n", exception.what());
		return -1;
	}
	catch (const std::exception& exception)
	{
		mapped_phase.release();
		fprintf(stderr, "SAR2UTM(): exception: %s\n", exception.what());
		return -1;
	}
}

int Utils::SAR2UTM(Mat& mapped_lon, Mat& mapped_lat, Mat& phase, Mat& mapped_phase, double grid_size, int interpolation_method, double* lon_east, double* lon_west, double* lat_north, double* lat_south)
{
	logSar2UtmInput("SAR2UTM(grid)", mapped_lon, mapped_lat, phase);
	if (mapped_lat.size() != mapped_lon.size() ||
		mapped_lat.size() != phase.size() ||
		phase.rows < 2 ||
		phase.cols < 2 ||
		phase.type() != CV_64F ||
		mapped_lat.type() != mapped_lon.type() ||
		(mapped_lon.type() != CV_32F && mapped_lon.type() != CV_64F)
		)
	{
		fprintf(stderr, "SAR2UTM(): input check failed!\n");
		return -1;
	}
	if (!std::isfinite(grid_size) || grid_size <= 0.0)
	{
		fprintf(stderr, "SAR2UTM(): grid size is invalid: %.17g.\n", grid_size);
		return -1;
	}
	GeographicGridStats coordinateStats;
	const bool validCoordinates = scanGeographicGrid(mapped_lon, mapped_lat, coordinateStats);
	logGeographicGridStats("SAR2UTM(grid)", mapped_lon, mapped_lat, coordinateStats);
	if (!validCoordinates || coordinateStats.missingCount != 0)
	{
		fprintf(stderr, "SAR2UTM(): mapped coordinates contain missing, non-finite, or out-of-range values.\n");
		return -1;
	}
	const double min_lon = coordinateStats.minLon;
	const double max_lon = coordinateStats.maxLon;
	const double min_lat = coordinateStats.minLat;
	const double max_lat = coordinateStats.maxLat;
	const double rawLonSpan = max_lon - min_lon;
	const double lonSpan = rawLonSpan > 180.0 ? 360.0 - rawLonSpan : rawLonSpan;
	const double latSpan = max_lat - min_lat;
	if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan < 0.0 || latSpan < 0.0 ||
		lonSpan > 30.0 || latSpan > 30.0)
	{
		fprintf(stderr, "SAR2UTM(): mapped coordinate span is unsupported: lonSpan=%.17g latSpan=%.17g.\n", lonSpan, latSpan);
		return -1;
	}
	double west = max_lon - min_lon > 180.0 ? max_lon : min_lon;
	if (lon_west) *lon_west = west;

	GeographicGridSpacing geographicSpacing;
	if (!estimateGeographicGridSpacing("SAR2UTM(grid)", mapped_lon, mapped_lat, geographicSpacing))
	{
		fprintf(stderr, "SAR2UTM(): source coordinate grid is degenerate.\n");
		return -1;
	}

	// The output remains geographic; grid_size controls its geographic sampling.
	double lon_interval, lat_interval;
	lon_interval = 5.0 / 6000.0 / (90.0 / grid_size);
	lat_interval = lon_interval;

	int rows = phase.rows; int cols = phase.cols;


	//计算UTM坐标系相位尺寸
	const double max_lon_temp = lonSpan;
	int UTM_rows = 0;
	int UTM_cols = 0;
	const double requestedRows = latSpan / lat_interval + 2.0;
	const double requestedCols = max_lon_temp / lon_interval + 2.0;
	if (!checkedSar2UtmDimensions(requestedRows, requestedCols, UTM_rows, UTM_cols))
	{
		fprintf(stderr,
			"SAR2UTM(): output grid rejected: requestedRows=%.17g requestedCols=%.17g gridSize=%.17g intervals=[%.17g,%.17g] spans=[%.17g,%.17g] budget=%llu bytes.\n",
			requestedRows, requestedCols, grid_size, lon_interval, lat_interval, lonSpan, latSpan,
			static_cast<unsigned long long>(kSar2UtmOutputMemoryBudgetBytes));
		return -1;
	}
	double south = max_lat - (double)(UTM_rows - 1) * lat_interval;
	double north = max_lat;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = south;
	double east = west + (double)(UTM_cols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	Mat b_filled;
	try
	{
		b_filled = Mat::zeros(UTM_rows, UTM_cols, CV_8U);
		mapped_phase = Mat::zeros(UTM_rows, UTM_cols, CV_64F);
	}
	catch (const cv::Exception& exception)
	{
		mapped_phase.release();
		fprintf(stderr, "SAR2UTM(): output allocation failed: %s\n", exception.what());
		return -1;
	}
	catch (const std::exception& exception)
	{
		mapped_phase.release();
		fprintf(stderr, "SAR2UTM(): output allocation failed: %s\n", exception.what());
		return -1;
	}
	//开始地理编码
	if (mapped_lat.type() == CV_32F)
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < rows; i++)
		{
			for (int j = 0; j < cols; j++)
			{
				const double lon = mapped_lon.at<float>(i, j);
				const double lat = mapped_lat.at<float>(i, j);
				const double rowValue = std::round((max_lat - lat) / lat_interval);
				double lonDistance = std::fabs(lon - west);
				lonDistance = lonDistance > 180.0 ? 360.0 - lonDistance : lonDistance;
				const double columnValue = std::round(lonDistance / lon_interval);
				if (!std::isfinite(rowValue) || !std::isfinite(columnValue) ||
					rowValue < 0.0 || rowValue >= UTM_rows || columnValue < 0.0 || columnValue >= UTM_cols) continue;
				const int row = static_cast<int>(rowValue);
				const int col = static_cast<int>(columnValue);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				// 1 = 有效锚点；2 = NaN（保留 nodata，但绝不作为填充的锚点）
				// 原实现在此累加计数，但该计数从未被消费，统一为标志位语义
				b_filled.at<uchar>(row, col) = std::isfinite(phase.at<double>(i, j)) ? 1 : 2;
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
				const double lon = mapped_lon.at<double>(i, j);
				const double lat = mapped_lat.at<double>(i, j);
				const double rowValue = std::round((max_lat - lat) / lat_interval);
				double lonDistance = std::fabs(lon - west);
				lonDistance = lonDistance > 180.0 ? 360.0 - lonDistance : lonDistance;
				const double columnValue = std::round(lonDistance / lon_interval);
				if (!std::isfinite(rowValue) || !std::isfinite(columnValue) ||
					rowValue < 0.0 || rowValue >= UTM_rows || columnValue < 0.0 || columnValue >= UTM_cols) continue;
				const int row = static_cast<int>(rowValue);
				const int col = static_cast<int>(columnValue);
				mapped_phase.at<double>(row, col) = phase.at<double>(i, j);
				// 1 = 有效锚点；2 = NaN（保留 nodata，但绝不作为填充的锚点）
				// 原实现在此累加计数，但该计数从未被消费，统一为标志位语义
				b_filled.at<uchar>(row, col) = std::isfinite(phase.at<double>(i, j)) ? 1 : 2;
			}
		}
	}

	// 顺序填充：O(行×列)，与空洞游程长度无关（取代原四方向搜索，见 geocoding_inverse_plan.md §2.1）
	fillRunsSequential(mapped_phase, b_filled, interpolation_method != 0);


	//cv::flip(mapped_phase, mapped_phase, 0);
	return 0;
}

int Utils::SAR2UTM(
	Mat& mapped_lon,
	Mat& mapped_lat,
	ComplexMat& slc, 
	ComplexMat& mapped_slc, 
	int interpolation_method,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south
)
{
	logSar2UtmInput("SAR2UTM(complex)", mapped_lon, mapped_lat, slc.re);
	if (mapped_lat.size() != mapped_lon.size() ||
		mapped_lat.size() != slc.re.size() ||
		mapped_lat.size() != slc.im.size() ||
		slc.GetRows() < 2 ||
		slc.GetCols() < 2 ||
		(slc.type() != CV_16S && slc.type() != CV_32F) ||
		mapped_lat.type() != mapped_lon.type() ||
		(mapped_lon.type() != CV_32F && mapped_lon.type() != CV_64F)
		)
	{
		fprintf(stderr, "SAR2UTM(): input check failed!\n");
		return -1;
	}
	GeographicGridStats coordinateStats;
	const bool validCoordinates = scanGeographicGrid(mapped_lon, mapped_lat, coordinateStats);
	logGeographicGridStats("SAR2UTM(complex)", mapped_lon, mapped_lat, coordinateStats);
	if (!validCoordinates || coordinateStats.missingCount != 0)
	{
		fprintf(stderr, "SAR2UTM(): mapped coordinates contain missing, non-finite, or out-of-range values.\n");
		return -1;
	}
	const double min_lon = coordinateStats.minLon;
	const double max_lon = coordinateStats.maxLon;
	const double min_lat = coordinateStats.minLat;
	const double max_lat = coordinateStats.maxLat;
	const double rawLonSpan = max_lon - min_lon;
	const double lonSpan = rawLonSpan > 180.0 ? 360.0 - rawLonSpan : rawLonSpan;
	const double latSpan = max_lat - min_lat;
	if (!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan < 0.0 || latSpan < 0.0 ||
		lonSpan > 30.0 || latSpan > 30.0)
	{
		fprintf(stderr, "SAR2UTM(): mapped coordinate span is unsupported: lonSpan=%.17g latSpan=%.17g.\n", lonSpan, latSpan);
		return -1;
	}
	double west = max_lon - min_lon > 180.0 ? max_lon : min_lon;
	if (lon_west) *lon_west = west;
	GeographicGridSpacing geographicSpacing;
	if (!estimateGeographicGridSpacing("SAR2UTM(complex)", mapped_lon, mapped_lat, geographicSpacing))
	{
		fprintf(stderr, "SAR2UTM(): source coordinate grid is degenerate.\n");
		return -1;
	}
	const double lon_interval = geographicSpacing.lonInterval;
	const double lat_interval = geographicSpacing.latInterval;

	int rows = slc.GetRows(); int cols = slc.GetCols();

	
	
	//计算UTM坐标系相位尺寸
	int UTM_rows = 0;
	int UTM_cols = 0;
	const double requestedRows = latSpan / lat_interval + 2.0;
	const double requestedCols = lonSpan / lon_interval + 2.0;
	if (!checkedSar2UtmDimensions(requestedRows, requestedCols, UTM_rows, UTM_cols))
	{
		fprintf(stderr,
			"SAR2UTM(): output grid rejected: requestedRows=%.17g requestedCols=%.17g intervals=[%.17g,%.17g] spans=[%.17g,%.17g] budget=%llu bytes.\n",
			requestedRows, requestedCols, lon_interval, lat_interval, lonSpan, latSpan,
			static_cast<unsigned long long>(kSar2UtmOutputMemoryBudgetBytes));
		return -1;
	}
	double south = max_lat - (double)(UTM_rows - 1) * lat_interval;
	double north = max_lat;
	if (lat_north) *lat_north = north;
	if (lat_south) *lat_south = south;
	double east = west + (double)(UTM_cols - 1) * lon_interval;
	east = east > 180.0 ? east - 360.0 : east;
	if (lon_east) *lon_east = east;
	Mat b_filled;
	try
	{
		b_filled = Mat::zeros(UTM_rows, UTM_cols, CV_8U);
		mapped_slc.re = Mat::zeros(UTM_rows, UTM_cols, slc.type());
		mapped_slc.im = Mat::zeros(UTM_rows, UTM_cols, slc.type());
	}
	catch (const cv::Exception& exception)
	{
		mapped_slc.re.release();
		mapped_slc.im.release();
		fprintf(stderr, "SAR2UTM(): output allocation failed: %s\n", exception.what());
		return -1;
	}
	catch (const std::exception& exception)
	{
		mapped_slc.re.release();
		mapped_slc.im.release();
		fprintf(stderr, "SAR2UTM(): output allocation failed: %s\n", exception.what());
		return -1;
	}
	//开始地理编码
	if (slc.type() == CV_16S)
	{
		if (mapped_lon.type() == CV_32F)
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					const double lon = mapped_lon.at<float>(i, j);
					const double lat = mapped_lat.at<float>(i, j);
					int row = 0, col = 0;
					if (!toSar2UtmGridIndex(lon, lat, west, min_lat, lon_interval, lat_interval, UTM_rows, UTM_cols, row, col)) continue;
					mapped_slc.re.at<short>(row, col) = slc.re.at<short>(i, j);
					mapped_slc.im.at<short>(row, col) = slc.im.at<short>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
		else
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					const double lon = mapped_lon.at<double>(i, j);
					const double lat = mapped_lat.at<double>(i, j);
					int row = 0, col = 0;
					if (!toSar2UtmGridIndex(lon, lat, west, min_lat, lon_interval, lat_interval, UTM_rows, UTM_cols, row, col)) continue;
					mapped_slc.re.at<short>(row, col) = slc.re.at<short>(i, j);
					mapped_slc.im.at<short>(row, col) = slc.im.at<short>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
	}
	else
	{
		if (mapped_lon.type() == CV_32F)
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					const double lon = mapped_lon.at<float>(i, j);
					const double lat = mapped_lat.at<float>(i, j);
					int row = 0, col = 0;
					if (!toSar2UtmGridIndex(lon, lat, west, min_lat, lon_interval, lat_interval, UTM_rows, UTM_cols, row, col)) continue;
					mapped_slc.re.at<float>(row, col) = slc.re.at<float>(i, j);
					mapped_slc.im.at<float>(row, col) = slc.im.at<float>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
		else
		{
			for (int i = 0; i < rows; i++)
			{
				for (int j = 0; j < cols; j++)
				{
					const double lon = mapped_lon.at<double>(i, j);
					const double lat = mapped_lat.at<double>(i, j);
					int row = 0, col = 0;
					if (!toSar2UtmGridIndex(lon, lat, west, min_lat, lon_interval, lat_interval, UTM_rows, UTM_cols, row, col)) continue;
					mapped_slc.re.at<float>(row, col) = slc.re.at<float>(i, j);
					mapped_slc.im.at<float>(row, col) = slc.im.at<float>(i, j);
					b_filled.at<uchar>(row, col) = 1;
				}
			}
		}
	}
	

	//插值
	if (interpolation_method == 0)
	{
		if (slc.type() == CV_16S)
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count, value1, value2, ratio1, ratio2 (nearest-neighbor method)
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) == 1) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) == 1) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) == 1) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) == 1) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						int left_distance = j - left;
						int right_distance = right - j;
						if (down_distance < up_distance && down_distance < left_distance && down_distance < right_distance)
						{
							x_i = down; x_j = j;
						}
						else if (up_distance < down_distance && up_distance < left_distance && up_distance < right_distance)
						{
							x_i = up; x_j = j;
						}
						else if (left_distance < down_distance && left_distance < up_distance && left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(x_i, x_j);
						mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(x_i, x_j);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						if (down_distance < up_distance)
						{
							x_i = down; x_j = j;
						}
						else
						{
							x_i = up; x_j = j;
						}
						mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(x_i, x_j);
						mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(x_i, x_j);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						int x_i = i, x_j = j;
						int left_distance = j - left;
						int right_distance = right - j;
						if (left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(x_i, x_j);
						mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(x_i, x_j);
						continue;
					}
					//四角兜底：网格四角只有一个方向的锚点（例如 up<0 且 left<0），上面三种情形都不成立。
					//不兜底会留下未填充的初值 0，对相位产品而言与真实观测无法区分。
					if (up >= 0) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(up, j); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(up, j); continue; }
					if (down <= UTM_rows - 1) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(down, j); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(down, j); continue; }
					if (left >= 0) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(i, left); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(i, left); continue; }
					if (right <= UTM_cols - 1) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(i, right); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(i, right); continue; }
				}
			}
		}
		else
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
					// removed unused: double value1, value2, ratio1, ratio2;
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) == 1) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) == 1) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) == 1) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) == 1) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						int left_distance = j - left;
						int right_distance = right - j;
						if (down_distance < up_distance && down_distance < left_distance && down_distance < right_distance)
						{
							x_i = down; x_j = j;
						}
						else if (up_distance < down_distance && up_distance < left_distance && up_distance < right_distance)
						{
							x_i = up; x_j = j;
						}
						else if (left_distance < down_distance && left_distance < up_distance && left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(x_i, x_j);
						mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(x_i, x_j);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						int x_i = i, x_j = j;
						int down_distance = down - i;
						int up_distance = i - up;
						if (down_distance < up_distance)
						{
							x_i = down; x_j = j;
						}
						else
						{
							x_i = up; x_j = j;
						}
						mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(x_i, x_j);
						mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(x_i, x_j);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						int x_i = i, x_j = j;
						int left_distance = j - left;
						int right_distance = right - j;
						if (left_distance < right_distance)
						{
							x_i = i; x_j = left;
						}
						else
						{
							x_i = i; x_j = right;
						}
						mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(x_i, x_j);
						mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(x_i, x_j);
						continue;
					}
					//四角兜底：网格四角只有一个方向的锚点（例如 up<0 且 left<0），上面三种情形都不成立。
					//不兜底会留下未填充的初值 0，对相位产品而言与真实观测无法区分。
					if (up >= 0) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(up, j); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(up, j); continue; }
					if (down <= UTM_rows - 1) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(down, j); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(down, j); continue; }
					if (left >= 0) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(i, left); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(i, left); continue; }
					if (right <= UTM_cols - 1) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(i, right); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(i, right); continue; }
				}
			}
		}
		
	}
	else
	{
		if (slc.type() == CV_16S)
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
					double value1, value2, ratio1, ratio2;
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) == 1) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) == 1) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) == 1) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) == 1) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{

						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<short>(i, left)) +
							double(mapped_slc.re.at<short>(i, right) - mapped_slc.re.at<short>(i, left)) * ratio1;
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<short>(up, j)) +
							double(mapped_slc.re.at<short>(down, j) - mapped_slc.re.at<short>(up, j)) * ratio2;
						mapped_slc.re.at<short>(i, j) = static_cast<short>((value1 + value2) / 2.0);

						value1 = double(mapped_slc.im.at<short>(i, left)) +
							double(mapped_slc.im.at<short>(i, right) - mapped_slc.im.at<short>(i, left)) * ratio1;
						value2 = double(mapped_slc.im.at<short>(up, j)) +
							double(mapped_slc.im.at<short>(down, j) - mapped_slc.im.at<short>(up, j)) * ratio2;
						mapped_slc.im.at<short>(i, j) = static_cast<short>((value1 + value2) / 2.0);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<short>(up, j)) +
							double(mapped_slc.re.at<short>(down, j) - mapped_slc.re.at<short>(up, j)) * ratio2;
						mapped_slc.re.at<short>(i, j) = static_cast<short>(value2);

						value2 = double(mapped_slc.im.at<short>(up, j)) +
							double(mapped_slc.im.at<short>(down, j) - mapped_slc.im.at<short>(up, j)) * ratio2;
						mapped_slc.im.at<short>(i, j) = static_cast<short>(value2);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<short>(i, left)) +
							double(mapped_slc.re.at<short>(i, right) - mapped_slc.re.at<short>(i, left)) * ratio1;
						mapped_slc.re.at<short>(i, j) = static_cast<short>(value1);

						value1 = double(mapped_slc.im.at<short>(i, left)) +
							double(mapped_slc.im.at<short>(i, right) - mapped_slc.im.at<short>(i, left)) * ratio1;
						mapped_slc.im.at<short>(i, j) = static_cast<short>(value1);
						continue;
					}
					//四角兜底：网格四角只有一个方向的锚点（例如 up<0 且 left<0），上面三种情形都不成立。
					//不兜底会留下未填充的初值 0，对相位产品而言与真实观测无法区分。
					if (up >= 0) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(up, j); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(up, j); continue; }
					if (down <= UTM_rows - 1) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(down, j); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(down, j); continue; }
					if (left >= 0) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(i, left); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(i, left); continue; }
					if (right <= UTM_cols - 1) { mapped_slc.re.at<short>(i, j) = mapped_slc.re.at<short>(i, right); mapped_slc.im.at<short>(i, j) = mapped_slc.im.at<short>(i, right); continue; }
				}
			}
		}
		else
		{
			for (int i = 0; i < UTM_rows; i++)
			{
				for (int j = 0; j < UTM_cols; j++)
				{
					if (b_filled.at<uchar>(i, j) != 0) continue;
					int up, down, left, right;
					// removed unused: up_count, down_count, left_count, right_count (bilinear uses value/ratio, not counts)
					double value1, value2, ratio1, ratio2;
					//寻找上面有值的点
					up = i;
					while (true)
					{
						up--;
						if (up < 0) break;
						if (b_filled.at<uchar>(up, j) == 1) break;
					}
					//寻找下面有值的点
					down = i;
					while (true)
					{
						down++;
						if (down > UTM_rows - 1) break;
						if (b_filled.at<uchar>(down, j) == 1) break;
					}
					//寻找左边有值的点
					left = j;
					while (true)
					{
						left--;
						if (left < 0) break;
						if (b_filled.at<uchar>(i, left) == 1) break;
					}
					//寻找右边有值的点
					right = j;
					while (true)
					{
						right++;
						if (right > UTM_cols - 1) break;
						if (b_filled.at<uchar>(i, right) == 1) break;
					}

					//上下左右都有值
					if (left >= 0 && right <= UTM_cols - 1 && up >= 0 && down <= UTM_rows - 1)
					{

						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<float>(i, left)) +
							double(mapped_slc.re.at<float>(i, right) - mapped_slc.re.at<float>(i, left)) * ratio1;
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<float>(up, j)) +
							double(mapped_slc.re.at<float>(down, j) - mapped_slc.re.at<float>(up, j)) * ratio2;
						mapped_slc.re.at<float>(i, j) = static_cast<float>((value1 + value2) / 2.0);

						value1 = double(mapped_slc.im.at<float>(i, left)) +
							double(mapped_slc.im.at<float>(i, right) - mapped_slc.im.at<float>(i, left)) * ratio1;
						value2 = double(mapped_slc.im.at<float>(up, j)) +
							double(mapped_slc.im.at<float>(down, j) - mapped_slc.im.at<float>(up, j)) * ratio2;
						mapped_slc.im.at<float>(i, j) = static_cast<float>((value1 + value2) / 2.0);
						continue;
					}
					//上下有值
					if (up >= 0 && down <= UTM_rows - 1)
					{
						ratio2 = double(i - up) / double(down - up);
						value2 = double(mapped_slc.re.at<float>(up, j)) +
							double(mapped_slc.re.at<float>(down, j) - mapped_slc.re.at<float>(up, j)) * ratio2;
						mapped_slc.re.at<float>(i, j) = static_cast<float>(value2);

						value2 = double(mapped_slc.im.at<float>(up, j)) +
							double(mapped_slc.im.at<float>(down, j) - mapped_slc.im.at<float>(up, j)) * ratio2;
						mapped_slc.im.at<float>(i, j) = static_cast<float>(value2);
						continue;
					}
					//左右有值
					if (left >= 0 && right <= UTM_cols - 1)
					{
						ratio1 = double(j - left) / double(right - left);
						value1 = double(mapped_slc.re.at<float>(i, left)) +
							double(mapped_slc.re.at<float>(i, right) - mapped_slc.re.at<float>(i, left)) * ratio1;
						mapped_slc.re.at<float>(i, j) = static_cast<float>(value1);

						value1 = double(mapped_slc.im.at<float>(i, left)) +
							double(mapped_slc.im.at<float>(i, right) - mapped_slc.im.at<float>(i, left)) * ratio1;
						mapped_slc.im.at<float>(i, j) = static_cast<float>(value1);
						continue;
					}
					//四角兜底：网格四角只有一个方向的锚点（例如 up<0 且 left<0），上面三种情形都不成立。
					//不兜底会留下未填充的初值 0，对相位产品而言与真实观测无法区分。
					if (up >= 0) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(up, j); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(up, j); continue; }
					if (down <= UTM_rows - 1) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(down, j); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(down, j); continue; }
					if (left >= 0) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(i, left); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(i, left); continue; }
					if (right <= UTM_cols - 1) { mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(i, right); mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(i, right); continue; }
				}
			}
		}
	}


	cv::flip(mapped_slc.re, mapped_slc.re, 0);
	cv::flip(mapped_slc.im, mapped_slc.im, 0);
	return 0;
}

int Utils::geocode(
	Mat& DEM84,
	Mat& input,
	double mapped_resolution_x,
	double mapped_resolution_y,
	Mat& mapped_result,
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
	Mat& stateVector, 
	double lon_spacing,
	double lat_spacing,
	double* lon_east,
	double* lon_west,
	double* lat_north, 
	double* lat_south
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_16S ||
		(input.type() != CV_32F && input.type() != CV_64F)||
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
		fprintf(stderr, "geocode(): input check failed!\n");
		return -1;
	}
	//计算DEM插值倍数
	int interp_times_x, interp_times_y;
	double a = 6378137, b = 6356752;
	double C_short = (a + b) * PI;//经线一圈长度
	double C_long = a * 2 * PI;
	double lon_per_meter = 360.0 / C_short;//经线上每米多少度
	double lat_per_meter = 360.0 / (C_long * cos(lat_upperleft / 180.0 * PI));//纬线上每米多少度
	interp_times_x = static_cast<int>(lon_spacing / lon_per_meter / mapped_resolution_x);
	interp_times_y = static_cast<int>(lat_spacing / lat_per_meter / mapped_resolution_y);
	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times_x = interp_times_x < 1 ? 1 : interp_times_x;
	interp_times_y = interp_times_y < 1 ? 1 : interp_times_y;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times_x, DEM84.rows * interp_times_y));
	lon_spacing = lon_spacing / (double)interp_times_x;
	lat_spacing = lat_spacing / (double)interp_times_y;
	//初始化轨道类
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime);
	stateVectors.applyOrbit();
	// removed unused: ret (geocode loop uses inline computation, no H5/return calls)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	mapped_result.create(DEM.rows, DEM.cols, input.type()); mapped_result = 0.0;
	//地理编码
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
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
				continue;
			}
			int azimuthIndex = static_cast<int>(floor((zeroDopplerTime - acquisitionStartTime) / time_interval));
			int rangeIndex = static_cast<int>(floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing));
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 2 || rangeIndex < 0 || rangeIndex > sceneWidth - 2)
			{

			}
			else
			{
				//双线性插值计算
				double ratio_x = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing - floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
				double ratio_y = (zeroDopplerTime - acquisitionStartTime) / time_interval - floor((zeroDopplerTime - acquisitionStartTime) / time_interval);
				
				if (input.type() == CV_32F)
				{
					double upper = (double)input.at<float>(azimuthIndex, rangeIndex) + double(input.at<float>(azimuthIndex, rangeIndex + 1)
						- input.at<float>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)input.at<float>(azimuthIndex + 1, rangeIndex) + double(input.at<float>(azimuthIndex + 1, rangeIndex + 1)
						- input.at<float>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_result.at<float>(i, j) = static_cast<float>(upper + (lower - upper) * ratio_y);

					/*mapped_result.at<float>(i, j) = input.at<float>(azimuthIndex + 1, rangeIndex + 1) + input.at<float>(azimuthIndex + 1, rangeIndex) +
						input.at<float>(azimuthIndex, rangeIndex + 1) + input.at<float>(azimuthIndex, rangeIndex);
					mapped_result.at<float>(i, j) = mapped_result.at<float>(i, j) / 4.0;*/
				}
				else
				{
					double upper = (double)input.at<double>(azimuthIndex, rangeIndex) + double(input.at<double>(azimuthIndex, rangeIndex + 1)
						- input.at<double>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)input.at<double>(azimuthIndex + 1, rangeIndex) + double(input.at<double>(azimuthIndex + 1, rangeIndex + 1)
						- input.at<double>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_result.at<double>(i, j) = upper + (lower - upper) * ratio_y;

					/*mapped_result.at<double>(i, j) = input.at<double>(azimuthIndex + 1, rangeIndex + 1) + input.at<double>(azimuthIndex + 1, rangeIndex) +
						input.at<double>(azimuthIndex, rangeIndex + 1) + input.at<double>(azimuthIndex, rangeIndex);
					mapped_result.at<double>(i, j) = mapped_result.at<double>(i, j) / 4.0;*/
				}
			}
		}
	}
	if (lat_north && lat_south && lon_east && lon_west)
	{
		*lat_north = lat_upperleft;
		*lat_south = lat_upperleft - (double)(DEM.rows - 1) * lat_spacing;
		*lon_east = lon_upperleft;
		*lon_west = lon_upperleft + (double)(DEM.cols - 1) * lon_spacing;
		*lon_west = *lon_west > 180.0 ? (*lon_west - 360.0) : *lon_west;
	}
	return 0;
}

int Utils::geocode(
	Mat& DEM84,
	ComplexMat& slc,
	double mapped_resolution_x,
	double mapped_resolution_y,
	ComplexMat& mapped_slc, 
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
	Mat& stateVector,
	double lon_spacing,
	double lat_spacing,
	double* lon_east,
	double* lon_west,
	double* lat_north,
	double* lat_south
)
{
	if (DEM84.empty() ||
		DEM84.type() != CV_16S ||
		(slc.type() != CV_32F && slc.type() != CV_16S) ||
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
		fprintf(stderr, "geocode(): input check failed!\n");
		return -1;
	}
	//计算DEM插值倍数
	int interp_times_x, interp_times_y;
	double a = 6378137, b = 6356752;
	double C_short = (a + b) * PI;//经线一圈长度
	double C_long = a * 2 * PI;
	double lon_per_meter = 360.0 / C_short;//经线上每米多少度
	double lat_per_meter = 360.0 / (C_long * cos(lat_upperleft / 180.0 * PI));//纬线上每米多少度

	interp_times_x = static_cast<int>(lon_spacing / lon_per_meter / mapped_resolution_x); 
	interp_times_y = static_cast<int>(lat_spacing / lat_per_meter / mapped_resolution_y); 
	//考虑DEM像素中心与边缘差异
	lat_upperleft = lat_upperleft + lat_spacing / 2.0 - lat_spacing / (double)interp_times_y * 0.5;                                                           
	lon_upperleft = lon_upperleft - lon_spacing / 2.0 + lon_spacing / (double)interp_times_x * 0.5;   

	//84坐标系DEM插值
	Mat DEM, stateVector_interp;
	interp_times_x = interp_times_x < 1 ? 1 : interp_times_x;
	interp_times_y = interp_times_y < 1 ? 1 : interp_times_y;
	cv::resize(DEM84, DEM, cv::Size(DEM84.cols * interp_times_x, DEM84.rows * interp_times_y));
	lon_spacing = lon_spacing / (double)interp_times_x;
	lat_spacing = lat_spacing / (double)interp_times_y;
	//初始化轨道类
	orbitStateVectors stateVectors(stateVector, acquisitionStartTime, acquisitionStopTime);
	stateVectors.applyOrbit();
	// removed unused: ret (geocode loop uses inline computation, no H5/return calls)
	double time_interval = 1.0 / prf;

	int DEM_rows = DEM.rows; int DEM_cols = DEM.cols;
	double dopplerFrequency = 0.0;
	mapped_slc.re.create(DEM.rows, DEM.cols, slc.type()); mapped_slc.re = 0.0;
	mapped_slc.im.create(DEM.rows, DEM.cols, slc.type()); mapped_slc.im = 0.0;
	//地理编码
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < DEM_rows; i++)
	{
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
				continue;
			}
			int azimuthIndex = static_cast<int>(floor((zeroDopplerTime - acquisitionStartTime) / time_interval));
			int rangeIndex = static_cast<int>(floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing));
			azimuthIndex = azimuthIndex - offset_row;
			rangeIndex = rangeIndex - offset_col;
			if (azimuthIndex < 0 || azimuthIndex > sceneHeight - 2 || rangeIndex < 0 || rangeIndex > sceneWidth - 2)
			{

			}
			else
			{
				//双线性插值计算
				double ratio_x = (distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing - floor((distance - nearRangeTime * VEL_C * 0.5) / rangeSpacing);
				double ratio_y = (zeroDopplerTime - acquisitionStartTime) / time_interval - floor((zeroDopplerTime - acquisitionStartTime) / time_interval);
				if (slc.type() == CV_32F)
				{
					double upper = (double)slc.re.at<float>(azimuthIndex, rangeIndex) + double(slc.re.at<float>(azimuthIndex, rangeIndex + 1)
						- slc.re.at<float>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)slc.re.at<float>(azimuthIndex + 1, rangeIndex) + double(slc.re.at<float>(azimuthIndex + 1, rangeIndex + 1)
						- slc.re.at<float>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.re.at<float>(i, j) = static_cast<float>(upper + (lower - upper) * ratio_y);

					upper = (double)slc.im.at<float>(azimuthIndex, rangeIndex) + double(slc.im.at<float>(azimuthIndex, rangeIndex + 1)
						- slc.im.at<float>(azimuthIndex, rangeIndex)) * ratio_x;
					lower = (double)slc.im.at<float>(azimuthIndex + 1, rangeIndex) + double(slc.im.at<float>(azimuthIndex + 1, rangeIndex + 1)
						- slc.im.at<float>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.im.at<float>(i, j) = static_cast<float>(upper + (lower - upper) * ratio_y);

					/*mapped_slc.im.at<float>(i, j) = slc.im.at<float>(azimuthIndex + 1, rangeIndex + 1) + slc.im.at<float>(azimuthIndex + 1, rangeIndex) +
						slc.im.at<float>(azimuthIndex, rangeIndex + 1) + slc.im.at<float>(azimuthIndex, rangeIndex);
					mapped_slc.im.at<float>(i, j) = mapped_slc.im.at<float>(i, j) / 4.0;

					mapped_slc.re.at<float>(i, j) = slc.re.at<float>(azimuthIndex + 1, rangeIndex + 1) + slc.re.at<float>(azimuthIndex + 1, rangeIndex) +
						slc.re.at<float>(azimuthIndex, rangeIndex + 1) + slc.re.at<float>(azimuthIndex, rangeIndex);
					mapped_slc.re.at<float>(i, j) = mapped_slc.re.at<float>(i, j) / 4.0;*/
				}
				else
				{
					double upper = (double)slc.re.at<short>(azimuthIndex, rangeIndex) + double(slc.re.at<short>(azimuthIndex, rangeIndex + 1)
						- slc.re.at<short>(azimuthIndex, rangeIndex)) * ratio_x;
					double lower = (double)slc.re.at<short>(azimuthIndex + 1, rangeIndex) + double(slc.re.at<short>(azimuthIndex + 1, rangeIndex + 1)
						- slc.re.at<short>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.re.at<short>(i, j) = static_cast<short>(upper + (lower - upper) * ratio_y);

					upper = (double)slc.im.at<short>(azimuthIndex, rangeIndex) + double(slc.im.at<short>(azimuthIndex, rangeIndex + 1)
						- slc.im.at<short>(azimuthIndex, rangeIndex)) * ratio_x;
					lower = (double)slc.im.at<short>(azimuthIndex + 1, rangeIndex) + double(slc.im.at<short>(azimuthIndex + 1, rangeIndex + 1)
						- slc.im.at<short>(azimuthIndex + 1, rangeIndex)) * ratio_x;
					mapped_slc.im.at<short>(i, j) = static_cast<short>(upper + (lower - upper) * ratio_y);

					/*mapped_slc.im.at<short>(i, j) = slc.im.at<short>(azimuthIndex + 1, rangeIndex + 1) + slc.im.at<short>(azimuthIndex + 1, rangeIndex) +
						slc.im.at<short>(azimuthIndex, rangeIndex + 1) + slc.im.at<short>(azimuthIndex, rangeIndex);
					mapped_slc.im.at<short>(i, j) = double(mapped_slc.im.at<short>(i, j)) / 4.0;

					mapped_slc.re.at<short>(i, j) = slc.re.at<short>(azimuthIndex + 1, rangeIndex + 1) + slc.re.at<short>(azimuthIndex + 1, rangeIndex) +
						slc.re.at<short>(azimuthIndex, rangeIndex + 1) + slc.re.at<short>(azimuthIndex, rangeIndex);
					mapped_slc.re.at<short>(i, j) = double(mapped_slc.re.at<short>(i, j)) / 4.0;*/
				}
			}
		}
	}
	if (lat_north && lat_south && lon_east && lon_west)
	{
		*lat_north = lat_upperleft;
		*lat_south = lat_upperleft - (double)(DEM.rows - 1) * lat_spacing;
		*lon_west = lon_upperleft;
		*lon_east = lon_upperleft + (double)(DEM.cols - 1) * lon_spacing;
		*lon_east = *lon_east > 180.0 ? (*lon_east - 360.0) : *lon_east;
	}
	return 0;
}

int Utils::geo_transformation(
	const char* grille_file,
	Mat DTM, double xllcorner,
	double yllcorner,
	Mat& prior_DTM,
	Mat& mapped_DTM,
	int SAR_extent_x,
	int SAR_extent_y,
	NewtonProgressCallback cb
)
{
	if (cb && !cb(0, "Preparing geographic transformation...")) return -2;
	if (!grille_file || DTM.empty() || xllcorner < 0 || yllcorner < 0)
	{
		fprintf(stderr, "geo_transformation(): input check failed!\n");
		return -1;
	}
	int ret;
	if (DTM.type() != CV_64F)
	{
		DTM.convertTo(DTM, CV_64F);
	}
	Mat row_matrix, col_matrix, utm_x, utm_y;
	vector<Mat> lon_matrix;
	vector<Mat> lat_matrix;
	vector<double> height_vector;
	mapped_DTM.create(SAR_extent_y, SAR_extent_x, CV_64F);
	mapped_DTM = -1.0;
	ret = read_grille(grille_file, row_matrix, col_matrix, lon_matrix, lat_matrix, height_vector);
	if (return_check(ret, "read_grille()", error_head)) return -1;
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\row_matrix.bin", row_matrix);
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\col_matrix.bin", col_matrix);
	//经纬度转UTM
	for (int i = 0; i < height_vector.size(); i++)
	{
		ret = lonlat2utm(lon_matrix[i], lat_matrix[i], utm_x, utm_y);
		if (return_check(ret, "lonlat2utm()", error_head)) return -1;
		//conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5");
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "X", utm_x);
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "Y", utm_y);
		utm_x.copyTo(lon_matrix[i]);
		utm_y.copyTo(lat_matrix[i]);
	}
	int DTM_rows = DTM.rows;
	int DTM_cols = DTM.cols;
	//DTM逐点转换
	Mat DTM_mapped_X, DTM_mapped_Y;
	DTM_mapped_X = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_Y = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_X = -1;
	DTM_mapped_Y = -1;
	std::atomic<int> count(0);
	std::atomic<bool> cancel_flag(false);
	Mat mask = Mat::zeros(DTM_rows, DTM_cols, CV_8UC1);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < SAR_extent_y; i++)
	{
		if (cancel_flag.load(std::memory_order_relaxed)) continue;
		if (cb && i % 16 == 0 && !cb(i * 100 / std::max(1, SAR_extent_y), "Transforming geographic grid...")) {
			cancel_flag.store(true, std::memory_order_relaxed);
			continue;
		}
		for (int j = 0; j < SAR_extent_x; j++)
		{
			//首先确定DTM值是否有效
			double h = prior_DTM.at<double>(i, j);
			//double h = 20.6;
			if (h < -9000)
			{
				continue;
			}
			mask.at<uchar>(i, j) = 1;
			//定位相邻的网格层
			int low_ix, high_ix;
			for (int k = 0; k < height_vector.size() - 1; k++)
			{
				if (h >= height_vector[k] && h <= height_vector[k + 1])
				{
					low_ix = k;
					high_ix = k + 1;
					//对下层网格寻找定位点
					bool located1 = false;
					for (int ii = 0; ii < row_matrix.rows - 1; ii++)
					{
						for (int jj = 0; jj < row_matrix.cols - 1; jj++)
						{
							//located1 = false; located2 = false;
							//判断该点是否在四个点中间
							double Ax, Ay, Bx, Dy;
							// removed unused: Mx, My, By, Cx, Cy, Dx (only Ax/Ay/Bx/Dy used for bounding box check)
							Ax = row_matrix.at<double>(ii, jj); Ay = col_matrix.at<double>(ii, jj);
							Bx = row_matrix.at<double>(ii + 1, jj); Dy = col_matrix.at<double>(ii, jj + 1);

							if (i >= Ax && 
								i <= Bx &&
								j >= Ay &&
								j <= Dy &&
								!located1
								)
							{
								located1 = true;
								//线性插值得到在下层网格上的UTM坐标
								//UTM_x插值
								double UTM_x_upleft = lon_matrix[low_ix].at<double>(ii, jj);
								double UTM_x_upright = lon_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_x_lowleft = lon_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_x_lowright = lon_matrix[low_ix].at<double>(ii + 1, jj + 1);
								double upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) * 
									(j - col_matrix.at<double>(ii, jj));
								double lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								double UTM_y_upleft = lat_matrix[low_ix].at<double>(ii, jj);
								double UTM_y_upright = lat_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_y_lowleft = lat_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_y_lowright = lat_matrix[low_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//线性插值得到在上层网格上的UTM坐标
								//UTM_x插值
								UTM_x_upleft = lon_matrix[high_ix].at<double>(ii, jj);
								UTM_x_upright = lon_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_x_lowleft = lon_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_x_lowright = lon_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								UTM_y_upleft = lat_matrix[high_ix].at<double>(ii, jj);
								UTM_y_upright = lat_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_y_lowleft = lat_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_y_lowright = lat_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));



								//上下两层之间插值得到UTM_x和UTM_y
								double UTM_x_final = UTM_x_final_lower + (UTM_x_final_higher - UTM_x_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);
								double UTM_y_final = UTM_y_final_lower + (UTM_y_final_higher - UTM_y_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);



								//通过插值得到的UTM_x和UTM_y再次插值得到DTM
								if (UTM_x_final >= xllcorner &&
									UTM_x_final < (xllcorner + DTM_cols - 1) &&
									UTM_y_final >= yllcorner &&
									UTM_y_final < (yllcorner + DTM_rows - 1)
									)
								{
									int row = static_cast<int>(floor((yllcorner + DTM_rows - 1) - UTM_y_final));
									int col = static_cast<int>(floor(UTM_x_final - xllcorner));
									if (DTM.at<double>(row, col) > -10 &&
										DTM.at<double>(row + 1, col) > -10 &&
										DTM.at<double>(row + 1, col + 1) > -10 &&
										DTM.at<double>(row, col + 1) > -10
										)
									{
										upper = DTM.at<double>(row, col) + (DTM.at<double>(row, col + 1) - DTM.at<double>(row, col)) / 1 * ((yllcorner + DTM_rows - 1) - UTM_y_final - row);
										lower = DTM.at<double>(row + 1, col) + (DTM.at<double>(row + 1, col + 1) - DTM.at<double>(row + 1, col)) / 1 *
											((yllcorner + DTM_rows - 1) - UTM_y_final - row);
										mapped_DTM.at<double>(i, j) = lower + (upper - lower) / 1 * (UTM_x_final - xllcorner - col);
									}
									
								}
								break;
							}
						}
						if (located1)break;
					}
				}
			}
		}
		count++;
		if (count % 10 == 0)
		{
			printf("\r估计进度1：%lf%%", double(count) / double(SAR_extent_y) * 100.0);
			fflush(stdout);
		}
	}
	
//	conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5");
//	conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "X", DTM_mapped_X);
//	conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "Y", DTM_mapped_Y);
//
//	conversion.read_array_from_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "X", DTM_mapped_X);
//	conversion.read_array_from_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\DTM_mapped.h5", "Y", DTM_mapped_Y);
//
//	
//	//最邻近插值法得到SAR坐标系DTM
////#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < DTM_rows; i++)
//	{
//		for (int j = 0; j < DTM_cols; j++)
//		{
//			if (DTM_mapped_X.at<double>(i, j) > 0 && DTM_mapped_Y.at<double>(i, j) > 0 && DTM.at<double>(i, j) > -1)
//			{
//				int col = round(DTM_mapped_Y.at<double>(i, j));
//				int row = round(DTM_mapped_X.at<double>(i, j));
//				mapped_DTM.at<double>(row, col) = DTM.at<double>(i, j);
//			}
//		}
//	}
	
//	//插值得到SAR坐标系的DTM
//	
//	double minX1, maxX1, minY1, maxY1;
//	cv::minMaxIdx(DTM_mapped_X, &minX1, &maxX1, NULL, NULL, mask);
//	cv::minMaxIdx(DTM_mapped_Y, &minY1, &maxY1, NULL, NULL, mask);
//	int start_y = floor(minY1) < 0 ? 0 : floor(minY1);
//	int end_y = ceil(maxY1);
//	int start_x = floor(minX1) < 0 ? 0 : floor(minX1);
//	int end_x = ceil(maxX1);
//	count = 0;
//#pragma omp parallel for schedule(guided)
//	for (int i = start_y; i < end_y; i++)
//	{
//		for (int j = start_x; j < end_x; j++)
//		{
//			bool located = false;
//			for (int ii = 0; ii < DTM_rows - 1; ii++)
//			{
//				for (int jj = 0; jj < DTM_cols - 1; jj++)
//				{
//					Mat X_tmp = Mat::zeros(1, 4, CV_64F), Y_tmp = Mat::zeros(1, 4, CV_64F);
//					X_tmp.at<double>(0, 0) = DTM_mapped_X.at<double>(ii, jj); Y_tmp.at<double>(0, 0) = DTM_mapped_Y.at<double>(ii, jj);
//					X_tmp.at<double>(0, 1) = DTM_mapped_X.at<double>(ii, jj + 1); Y_tmp.at<double>(0, 1) = DTM_mapped_Y.at<double>(ii, jj + 1);
//					X_tmp.at<double>(0, 2) = DTM_mapped_X.at<double>(ii + 1, jj + 1); Y_tmp.at<double>(0, 2) = DTM_mapped_Y.at<double>(ii + 1, jj + 1);
//					X_tmp.at<double>(0, 3) = DTM_mapped_X.at<double>(ii + 1, jj); Y_tmp.at<double>(0, 3) = DTM_mapped_Y.at<double>(ii + 1, jj);
//					double minX, minY, maxX, maxY;
//					cv::minMaxIdx(X_tmp, &minX, &maxX);
//					cv::minMaxIdx(Y_tmp, &minY, &maxY);
//					if (j >= minX &&
//						j <= maxX &&
//						i >= minY &&
//						i <= maxY
//						)
//					{
//						located = true;
//						//(反距离权重法插值)
//						double d1 = sqrt((j - DTM_mapped_X.at<double>(ii, jj)) * (j - DTM_mapped_X.at<double>(ii, jj)) +
//							(i - DTM_mapped_Y.at<double>(ii, jj)) * (i - DTM_mapped_Y.at<double>(ii, jj)));
//						double d2 = sqrt((j - DTM_mapped_X.at<double>(ii + 1, jj)) * (j - DTM_mapped_X.at<double>(ii + 1, jj)) +
//							(i - DTM_mapped_Y.at<double>(ii + 1, jj)) * (i - DTM_mapped_Y.at<double>(ii + 1, jj)));
//						double d3 = sqrt((j - DTM_mapped_X.at<double>(ii, jj + 1)) * (j - DTM_mapped_X.at<double>(ii, jj + 1)) +
//							(i - DTM_mapped_Y.at<double>(ii, jj + 1)) * (i - DTM_mapped_Y.at<double>(ii, jj + 1)));
//						double d4 = sqrt((j - DTM_mapped_X.at<double>(ii + 1, jj + 1)) * (j - DTM_mapped_X.at<double>(ii + 1, jj + 1)) +
//							(i - DTM_mapped_Y.at<double>(ii + 1, jj + 1)) * (i - DTM_mapped_Y.at<double>(ii + 1, jj + 1)));
//
//						mapped_DTM.at<double>(j, i) = (DTM.at<double>(ii, jj) / d1 + DTM.at<double>(ii + 1, jj) / d2 + DTM.at<double>(ii, jj + 1) / d3 + DTM.at<double>(ii + 1, jj + 1) / d4) /
//							(1 / d1 + 1 / d2 + 1 / d3 + 1 / d4);
//						break;
//					}
//				}
//				if (located)break;
//			}
//			count++;
//			if (count % 1 == 0)
//			{
//				printf("\r估计进度2：%lf%%", double(count) / double(end_y - start_y + 1) / double(end_x - start_x + 1) * 100.0);
//				fflush(stdout);
//			}
//		}
//		/*count++;
//		if (count % 10 == 0)
//		{
//			printf("\r估计进度2：%lf%%", double(count) / double(end_y - start_y + 1) * 100.0);
//			fflush(stdout);
//		}*/
//	}
	

	if (cancel_flag.load(std::memory_order_relaxed)) return -2;
	if (cb && !cb(100, "Geographic transformation complete.")) return -2;
	return 0;
}

int Utils::geo_transformation(
	const char* grille_file,
	Mat DTM,
	double lon_upleft,
	double lat_upleft,
	double lon_interval,
	double lat_interval,
	Mat& prior_DTM,
	Mat& mapped_DTM,
	int SAR_extent_x,
	int SAR_extent_y,
	NewtonProgressCallback cb
)
{
	if (cb && !cb(0, "Preparing geographic transformation...")) return -2;
	if (!grille_file || DTM.empty() || fabs(lon_upleft) > 180.0 || fabs(lat_upleft) > 90.0)
	{
		fprintf(stderr, "geo_transformation(): input check failed!\n");
		return -1;
	}
	int ret;
	if (DTM.type() != CV_64F)
	{
		DTM.convertTo(DTM, CV_64F);
	}
	Mat row_matrix, col_matrix, utm_x, utm_y;
	vector<Mat> lon_matrix;
	vector<Mat> lat_matrix;
	vector<double> height_vector;
	mapped_DTM.create(SAR_extent_y, SAR_extent_x, CV_64F);
	mapped_DTM = -1.0;
	ret = read_grille(grille_file, row_matrix, col_matrix, lon_matrix, lat_matrix, height_vector);
	if (return_check(ret, "read_grille()", error_head)) return -1;
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\row_matrix.bin", row_matrix);
	//cvmat2bin("D:\\working_dir\\projects\\software\\InSAR\\bin\\col_matrix.bin", col_matrix);
	//经纬度转UTM
	for (int i = 0; i < height_vector.size(); i++)
	{
		ret = lonlat2utm(lon_matrix[i], lat_matrix[i], utm_x, utm_y);
		if (return_check(ret, "lonlat2utm()", error_head)) return -1;
		//conversion.creat_new_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5");
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "X", utm_x);
		//conversion.write_array_to_h5("D:\\working_dir\\projects\\software\\InSAR\\bin\\utm_x.h5", "Y", utm_y);
		utm_x.copyTo(lon_matrix[i]);
		utm_y.copyTo(lat_matrix[i]);
	}

	int DTM_rows = DTM.rows;
	int DTM_cols = DTM.cols;
	//DTM逐点转换
	Mat DTM_mapped_X, DTM_mapped_Y;
	DTM_mapped_X = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_Y = Mat::zeros(DTM_rows, DTM_cols, CV_64F);
	DTM_mapped_X = -1;
	DTM_mapped_Y = -1;
	std::atomic<int> count(0);
	std::atomic<bool> cancel_flag(false);
	//Mat mask = Mat::zeros(DTM_rows, DTM_cols, CV_8UC1);
	InitializeGDALAndProjOnce();
	OGRSpatialReference monUtm;
	monUtm.SetWellKnownGeogCS("WGS84");
	monUtm.SetUTM(22, 1);
	OGRSpatialReference monGeo;
	monGeo.SetWellKnownGeogCS("WGS84");
	int max_threads = omp_get_max_threads();
	std::vector<OGRCoordinateTransformation*> coordTransList(max_threads, nullptr);
	for (int t = 0; t < max_threads; ++t)
	{
		coordTransList[t] = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
		if (coordTransList[t] == nullptr)
		{
			for (int k = 0; k < t; ++k) delete coordTransList[k];
			fprintf(stderr, "Error: OGRCreateCoordinateTransformation failed!\n");
			return -1;
		}
	}
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < SAR_extent_y; i++)
	{
		if (cancel_flag.load(std::memory_order_relaxed)) continue;
		if (cb && i % 16 == 0 && !cb(i * 100 / std::max(1, SAR_extent_y), "Transforming geographic grid...")) {
			cancel_flag.store(true, std::memory_order_relaxed);
			continue;
		}
		for (int j = 0; j < SAR_extent_x; j++)
		{
			//首先确定DTM值是否有效
			double h = prior_DTM.at<double>(i, j);
			//double h = 20.6;
			if (h < -9000)
			{
				continue;
			}
			//mask.at<uchar>(i, j) = 1;
			//定位相邻的网格层
			int low_ix, high_ix;
			for (int k = 0; k < height_vector.size() - 1; k++)
			{
				if (h >= height_vector[k] && h <= height_vector[k + 1])
				{
					low_ix = k;
					high_ix = k + 1;
					//对下层网格寻找定位点
					bool located1 = false;
					for (int ii = 0; ii < row_matrix.rows - 1; ii++)
					{
						for (int jj = 0; jj < row_matrix.cols - 1; jj++)
						{
							//located1 = false; located2 = false;
							//判断该点是否在四个点中间
							double Ax, Ay, Bx, Dy;
							Ax = row_matrix.at<double>(ii, jj); Ay = col_matrix.at<double>(ii, jj);
							Bx = row_matrix.at<double>(ii + 1, jj); Dy = col_matrix.at<double>(ii, jj + 1);

							if (i >= Ax &&
								i <= Bx &&
								j >= Ay &&
								j <= Dy &&
								!located1
								)
							{
								located1 = true;
								//对分层网格寻找下层定位点的UTM值
								//UTM_x插值
								double UTM_x_upleft = lon_matrix[low_ix].at<double>(ii, jj);
								double UTM_x_upright = lon_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_x_lowleft = lon_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_x_lowright = lon_matrix[low_ix].at<double>(ii + 1, jj + 1);
								double upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								double lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								double UTM_y_upleft = lat_matrix[low_ix].at<double>(ii, jj);
								double UTM_y_upright = lat_matrix[low_ix].at<double>(ii, jj + 1);
								double UTM_y_lowleft = lat_matrix[low_ix].at<double>(ii + 1, jj);
								double UTM_y_lowright = lat_matrix[low_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_lower = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//对分层网格寻找上层定位点的UTM值
								//UTM_x插值
								UTM_x_upleft = lon_matrix[high_ix].at<double>(ii, jj);
								UTM_x_upright = lon_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_x_lowleft = lon_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_x_lowright = lon_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_x_upleft + (UTM_x_upright - UTM_x_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_x_lowleft + (UTM_x_lowright - UTM_x_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_x_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//UTM_y插值
								UTM_y_upleft = lat_matrix[high_ix].at<double>(ii, jj);
								UTM_y_upright = lat_matrix[high_ix].at<double>(ii, jj + 1);
								UTM_y_lowleft = lat_matrix[high_ix].at<double>(ii + 1, jj);
								UTM_y_lowright = lat_matrix[high_ix].at<double>(ii + 1, jj + 1);
								upper = UTM_y_upleft + (UTM_y_upright - UTM_y_upleft) / (col_matrix.at<double>(ii, jj + 1) - col_matrix.at<double>(ii, jj)) *
									(j - col_matrix.at<double>(ii, jj));
								lower = UTM_y_lowleft + (UTM_y_lowright - UTM_y_lowleft) / (col_matrix.at<double>(ii + 1, jj + 1) - col_matrix.at<double>(ii + 1, jj)) *
									(j - col_matrix.at<double>(ii + 1, jj));
								double UTM_y_final_higher = lower + (upper - lower) / (row_matrix.at<double>(ii, jj) - row_matrix.at<double>(ii + 1, jj)) *
									(i - row_matrix.at<double>(ii + 1, jj));

								//高程插值得到UTM_x和UTM_y
								double UTM_x_final = UTM_x_final_lower + (UTM_x_final_higher - UTM_x_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);
								double UTM_y_final = UTM_y_final_lower + (UTM_y_final_higher - UTM_y_final_lower) / (height_vector[high_ix] - height_vector[low_ix]) *
									(h - height_vector[low_ix]);

								double lat_x, lon_y;
								lat_x = UTM_x_final;
								lon_y = UTM_y_final;
								int thread_num = omp_get_thread_num();
								int reprojected = coordTransList[thread_num]->Transform(1, &lat_x, &lon_y);

								//通过插值得到lat_x和lon_y后再插值得到DTM
								if (lat_x <= lat_upleft &&
									lat_x >= (lat_upleft - (DTM_rows - 1)*lat_interval) &&
									lon_y <= (lon_upleft + (DTM_cols - 1)*lon_interval) &&
									lon_y >= lon_upleft
									)
								{
									int row = static_cast<int>(floor((lat_upleft - lat_x) / lat_interval));
									double delta_row = ((lat_upleft - lat_x) / lat_interval - row);
									int col = static_cast<int>(floor((lon_y - lon_upleft) / lon_interval));
									double delta_col = ((lon_y - lon_upleft) / lon_interval - col);
									if (row < 0 || row >= DTM_rows - 1 || col < 0 || col >= DTM_cols - 1) continue;
									if (DTM.at<double>(row, col) > -1000 &&
										DTM.at<double>(row + 1, col) > -1000 &&
										DTM.at<double>(row + 1, col + 1) > -1000 &&
										DTM.at<double>(row, col + 1) > -1000
										)
									{
										upper = DTM.at<double>(row, col) + (DTM.at<double>(row, col + 1) - DTM.at<double>(row, col)) * delta_row;
										lower = DTM.at<double>(row + 1, col) + (DTM.at<double>(row + 1, col + 1) - DTM.at<double>(row + 1, col)) * delta_row;
										mapped_DTM.at<double>(i, j) = lower + (upper - lower) * delta_col;
									}

								}
								break;
							}
						}
						if (located1)break;
					}
				}
			}
		}
		count++;
		if (count % 10 == 0)
		{
			printf("\r估计进度1：%lf%%", double(count) / double(SAR_extent_y) * 100.0);
			fflush(stdout);
		}
	}
	for (int t = 0; t < max_threads; ++t)
	{
		if (coordTransList[t] != nullptr)
		{
			delete coordTransList[t];
		}
	}
	if (cancel_flag.load(std::memory_order_relaxed)) return -2;
	if (cb && !cb(100, "Geographic transformation complete.")) return -2;
	return 0;
}

int Utils::read_grille(
	const char* grille_file,
	Mat& row_matrix, 
	Mat& col_matrix,
	vector<Mat>& lon_matrix,
	vector<Mat>& lat_matrix, 
	vector<double>& height_vector
)
{
	if (!grille_file)
	{
		fprintf(stderr, "read_grille(): input check failed!\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(grille_file, "rt");
	if (!fp)
	{
		fprintf(stderr, "read_grille(): cannot open grille file!\n");
		return -1;
	}
	char data[1024];
	char* ptr;
	// removed unused: val (grille parsing uses strtol directly)
	fgets(data, 1024, fp);
	fgets(data, 1024, fp);
	fgets(data, 1024, fp);
	memset(data, 0, 1024);
	fgets(data, 1024, fp);
	int rows = strtol(data, &ptr, 10);
	fgets(data, 1024, fp);
	int cols = strtol(data, &ptr, 10);
	fgets(data, 1024, fp);
	int height_num = strtol(data, &ptr, 10);
	size_t total = rows * cols * height_num;
	Mat zeroMat = Mat::zeros(rows, cols, CV_64F);
	zeroMat.copyTo(row_matrix);
	zeroMat.copyTo(col_matrix);
	lon_matrix.resize(height_num);
	lat_matrix.resize(height_num);
	height_vector.resize(height_num);
	for (int i = 0; i < height_num; i++)
	{
		zeroMat.copyTo(lon_matrix[i]);
		zeroMat.copyTo(lat_matrix[i]);
	}
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			for (int k = 0; k < height_num; k++)
			{
				fgets(data, 1024, fp);
				row_matrix.at<double>(i, j) = strtod(data, &ptr);
				col_matrix.at<double>(i, j) = strtod(ptr, &ptr);
				height_vector[k] = strtod(ptr, &ptr);
				lon_matrix[k].at<double>(i, j) = strtod(ptr, &ptr);
				lat_matrix[k].at<double>(i, j) = strtod(ptr, &ptr);
			}
		}
	}
	fclose(fp);
	return 0;
}

int Utils::lonlat2utm(Mat lon, Mat lat, Mat& UTM_X, Mat& UTM_Y)
{
	if (lon.empty() || lat.empty() || lon.size() != lat.size() || lon.type() != lat.type() || lon.type() != CV_64F)
	{
		fprintf(stderr, "lonlat2utm(): input check failed!\n");
		return -1;
	}
	InitializeGDALAndProjOnce();
	OGRSpatialReference monUtm;
	monUtm.SetWellKnownGeogCS("WGS84");
	monUtm.SetUTM(22, true);

	OGRSpatialReference monGeo;
	monGeo.SetWellKnownGeogCS("WGS84");

	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monGeo, &monUtm);

	int nr = lon.rows;
	int nc = lon.cols;
	lon.copyTo(UTM_X);
	lon.copyTo(UTM_Y);
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			double x, y;
			x = lat.at<double>(i, j);
			y = lon.at<double>(i, j);
			int reprojected = coordTrans->Transform(1, &x, &y);
			UTM_X.at<double>(i, j) = x;
			UTM_Y.at<double>(i, j) = y;
		}
	}
	delete coordTrans;
	return 0;
}

int Utils::read_LVIS(
	vector<string>& LVIS2_filelist,
	Mat& DTM,
	Mat& DSM,
	Mat& RH100,
	Mat& RH95,
	Mat& lat,
	Mat& lon
)
{
	if (LVIS2_filelist.empty())
	{
		fprintf(stderr, "read_LVIS(): input check failed!\n");
		return -1;
	}
	int n_files = static_cast<int>(LVIS2_filelist.size());
	int LVIS_cols = 42;
	vector<double> dtm, dsm, rh100, rh95, lo, la;
	FILE* fp = NULL;
	char str[1024];
	char* ret = NULL;
	char* ptr = NULL;
	double val;
	for (int i = 0; i < n_files; i++)
	{
		fp = fopen(LVIS2_filelist[i].c_str(), "rt");
		if (!fp)
		{
			fprintf(stderr, "read_LVIS(): cannot open %s!\n", LVIS2_filelist[i].c_str());
			return -1;
		}
		for (int j = 0; j < 18; j++)
		{
			ret = fgets(str, 1024, fp);
		}
		while (ret)
		{
			ret=fgets(str, 1024, fp);
			vector<double> tmp;
			ptr = str;
			for (int k = 0; k < LVIS_cols; k++)
			{
				val = strtod(ptr, &ptr);
				tmp.push_back(val);
			}
			lo.push_back(tmp[3]);
			la.push_back(tmp[4]);
			dtm.push_back(tmp[5]);
			dsm.push_back(tmp[11]);
			rh100.push_back(tmp[33]);
			rh95.push_back(tmp[28]);
			tmp.clear();
		}
		fclose(fp);
		fp = NULL;
	}
	DTM.create(static_cast<int>(dtm.size()), 1, CV_64F);
	DTM = 0;
	DTM.copyTo(DSM);
	DTM.copyTo(lon);
	DTM.copyTo(lat);
	DTM.copyTo(RH100);
	DTM.copyTo(RH95);
	for (int i = 0; i < dtm.size(); i++)
	{
		DTM.at<double>(i, 0) = dtm[i];
		DSM.at<double>(i, 0) = dsm[i];
		lon.at<double>(i, 0) = lo[i];
		lat.at<double>(i, 0) = la[i];
		RH100.at<double>(i, 0) = rh100[i];
		RH95.at<double>(i, 0) = rh95[i];
	}
	return 0;
}

int Utils::geo2sar_DLR(
	double east_min,
	double north_min,
	double east_max,
	double north_max,
	int projection_zone,
	double pixel_spacing,
	Mat& sr2geo_az,
	Mat& sr2geo_rg,
	Mat& sr2geo_h_ref,
	Mat& sr2geo3d_rg_o1,
	Mat& sr2geo3d_rg_o2,
	Mat& sr2geo3d_az_o1,
	Mat& sr2geo3d_az_o2,
	Mat& DTM,
	Mat& DTM_lon,
	Mat& DTM_lat,
	Mat& mapped_slc_rg,
	Mat& mapped_slc_az,
	Mat& mapped_h0
)
{
	if (pixel_spacing < 0 ||
		sr2geo_az.empty() || sr2geo_az.type() != CV_64F ||
		sr2geo_az.size() != sr2geo_rg.size() || sr2geo_az.size() != sr2geo_h_ref.size() || sr2geo_az.size() != sr2geo3d_rg_o1.size() || 
		sr2geo_az.size() != sr2geo3d_rg_o2.size() || sr2geo_az.size() != sr2geo3d_az_o1.size() || sr2geo_az.size() != sr2geo3d_az_o2.size() ||
		DTM.empty() || DTM.type() != CV_64F ||
		DTM.size() != DTM_lon.size() || DTM.size() != DTM_lat.size()
		)
	{
		fprintf(stderr, "geo2sar_DLR(): input check failed!\n");
		return -1;
	}
	//确定场景的经纬度范围
	Mat lon, lat;
	lon.create(4, 1, CV_64F); lon = 0.0; lon.copyTo(lat);
	InitializeGDALAndProjOnce();
	OGRSpatialReference monUtm;
	monUtm.SetWellKnownGeogCS("WGS84");
	monUtm.SetUTM(abs(projection_zone), projection_zone>0);
	OGRSpatialReference monGeo;
	monGeo.SetWellKnownGeogCS("WGS84");
	OGRCoordinateTransformation* coordTrans = OGRCreateCoordinateTransformation(&monUtm, &monGeo);
	double x, y;
	x = east_min; y = north_min;
	int reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(0, 0) = y; lat.at<double>(0, 0) = x;

	x = east_min; y = north_max;
	reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(1, 0) = y; lat.at<double>(1, 0) = x;

	x = east_max; y = north_min;
	reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(2, 0) = y; lat.at<double>(2, 0) = x;

	x = east_max; y = north_max;
	reprojected = coordTrans->Transform(1, &x, &y);
	lon.at<double>(3, 0) = y; lat.at<double>(3, 0) = x;

	double lon_min, lon_max, lat_min, lat_max;
	cv::minMaxLoc(lon, &lon_min, &lon_max);
	cv::minMaxLoc(lat, &lat_min, &lat_max);
	delete coordTrans;

	
	mapped_slc_rg.create(DTM.rows, DTM.cols, CV_64F); mapped_slc_rg = -1;
	mapped_slc_az.create(DTM.rows, DTM.cols, CV_64F); mapped_slc_az = -1;
	mapped_h0.create(DTM.rows, DTM.cols, CV_64F); mapped_h0 = -1;
	Mat UTM_x, UTM_y;
	DTM_lat.copyTo(UTM_x);
	DTM_lon.copyTo(UTM_y);
	OGRCoordinateTransformation* coordTrans2 = OGRCreateCoordinateTransformation(&monGeo , &monUtm);
	//将DTM的经纬度转换为UTM
	for (int i = 0; i < DTM.rows; i++)
	{
		for (int j = 0; j < DTM.cols; j++)
		{
			if (DTM.at<double>(i, j) < -9000.0) continue;
			double xx, yy;
			xx = DTM_lat.at<double>(i, j);
			yy = DTM_lon.at<double>(i, j);
			int reprojected = coordTrans2->Transform(1, &xx, &yy);
			UTM_x.at<double>(i, j) = xx;
			UTM_y.at<double>(i, j) = yy;
		}
	}
	delete coordTrans2;
	//开始投影
	for (int i = 0; i < DTM.rows; i++)
	{
		for (int j = 0; j < DTM.cols; j++)
		{
			if (DTM.at<double>(i, j) < -9000.0) continue;
			double utm_x, utm_y, h;
			utm_x = UTM_x.at<double>(i, j);
			utm_y = UTM_y.at<double>(i, j);
			h = DTM.at<double>(i, j);
			if (DTM_lon.at<double>(i, j) <= lon_min || DTM_lon.at<double>(i, j) >= lon_max ||
				DTM_lat.at<double>(i, j) <= lat_min || DTM_lat.at<double>(i, j) >= lat_max
				)
			{
				continue;
			}
			//插值得到2D地理编码下的距离方位坐标，参考DEM和3D地理编码系数
			double easting, northing, rg0, az0, h0, rg_o1, rg_o2, az_o1, az_o2, upper, lower;
			int row, col;
			easting = (utm_x - east_min) / pixel_spacing;
			northing = (utm_y - north_min) / pixel_spacing;

			row = sr2geo_h_ref.rows - 1 - int(floor(northing));
			col = int(floor(easting));
			if (row < 1 || row > sr2geo_h_ref.rows - 1 || col < 0 || col >= sr2geo_h_ref.cols - 1)
			{
				continue;
			}
			if (sr2geo_h_ref.at<double>(row, col) < -9000 ||
				sr2geo_h_ref.at<double>(row, col + 1) < -9000 ||
				sr2geo_h_ref.at<double>(row - 1, col) < -9000 ||
				sr2geo_h_ref.at<double>(row - 1, col + 1) < -9000
				)
			{
				continue;
			}
				


			lower = sr2geo_rg.at<double>(row, col) + (easting - double(col)) * (sr2geo_rg.at<double>(row, col + 1) - sr2geo_rg.at<double>(row, col));
			upper = sr2geo_rg.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo_rg.at<double>(row - 1, col + 1) - sr2geo_rg.at<double>(row - 1, col));
			rg0 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo_az.at<double>(row, col) + (easting - double(col)) * (sr2geo_az.at<double>(row, col + 1) - sr2geo_az.at<double>(row, col));
			upper = sr2geo_az.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo_az.at<double>(row - 1, col + 1) - sr2geo_az.at<double>(row - 1, col));
			az0 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo_h_ref.at<double>(row, col) + (easting - double(col)) * (sr2geo_h_ref.at<double>(row, col + 1) - sr2geo_h_ref.at<double>(row, col));
			upper = sr2geo_h_ref.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo_h_ref.at<double>(row - 1, col + 1) - sr2geo_h_ref.at<double>(row - 1, col));
			h0 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_rg_o1.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_rg_o1.at<double>(row, col + 1) - sr2geo3d_rg_o1.at<double>(row, col));
			upper = sr2geo3d_rg_o1.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_rg_o1.at<double>(row - 1, col + 1) - sr2geo3d_rg_o1.at<double>(row - 1, col));
			rg_o1 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_rg_o2.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_rg_o2.at<double>(row, col + 1) - sr2geo3d_rg_o2.at<double>(row, col));
			upper = sr2geo3d_rg_o2.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_rg_o2.at<double>(row - 1, col + 1) - sr2geo3d_rg_o2.at<double>(row - 1, col));
			rg_o2 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_az_o1.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_az_o1.at<double>(row, col + 1) - sr2geo3d_az_o1.at<double>(row, col));
			upper = sr2geo3d_az_o1.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_az_o1.at<double>(row - 1, col + 1) - sr2geo3d_az_o1.at<double>(row - 1, col));
			az_o1 = lower + (upper - lower) * (northing - floor(northing));

			lower = sr2geo3d_az_o2.at<double>(row, col) + (easting - double(col)) * (sr2geo3d_az_o2.at<double>(row, col + 1) - sr2geo3d_az_o2.at<double>(row, col));
			upper = sr2geo3d_az_o2.at<double>(row - 1, col) + (easting - double(col)) * (sr2geo3d_az_o2.at<double>(row - 1, col + 1) - sr2geo3d_az_o2.at<double>(row - 1, col));
			az_o2 = lower + (upper - lower) * (northing - floor(northing));

			double delta_h, rg_new, az_new;
			delta_h = h - h0;
			rg_new = rg0 + rg_o1 * delta_h + rg_o2 * delta_h * delta_h;
			az_new = az0 + az_o1 * delta_h + az_o2 * delta_h * delta_h;
	
			mapped_slc_rg.at<double>(i, j) = rg0;
			mapped_slc_az.at<double>(i, j) = az0;
			mapped_h0.at<double>(i, j) = h0;
		}
	}
	return 0;
}

// 根据经纬度获取大地水准面高差
double Utils::getGeoidHeight(const std::string& geoidFilePath, double lon, double lat) {

	// 注册 GDAL/PROJ 驱动与路径

	InitializeGDALAndProjOnce();



	// 打开 Geoid 文件

	GDALDataset* poDataset = (GDALDataset*)GDALOpen(geoidFilePath.c_str(), GA_ReadOnly);

	if (poDataset == nullptr) {

		std::cerr << "无法打开 Geoid 文件: " << geoidFilePath << std::endl;

		return 0.0;

	}



	// 获取第一个波段（Geoid 数据）

	GDALRasterBand* poBand = poDataset->GetRasterBand(1);

	if (poBand == nullptr) {

		std::cerr << "无法获取波段数据" << std::endl;

		GDALClose(poDataset);

		return 0.0;

	}



	// 获取 Geoid 文件的地理变换信息

	double adfGeoTransform[6];

	if (poDataset->GetGeoTransform(adfGeoTransform) != CE_None) {

		std::cerr << "无法获取地理变换信息" << std::endl;

		GDALClose(poDataset);

		return 0.0;

	}



	// 将经纬度转换为像素坐标

	double x = (lon - adfGeoTransform[0]) / adfGeoTransform[1];

	double y = (lat - adfGeoTransform[3]) / adfGeoTransform[5];



	// 插值获取 Geoid Height

	float geoidHeight = 0.0;

	if (poBand->RasterIO(GF_Read, static_cast<int>(x), static_cast<int>(y), 1, 1,

		&geoidHeight, 1, 1, GDT_Float32, 0, 0) != CE_None) {

		std::cerr << "无法读取 Geoid 数据" << std::endl;

		GDALClose(poDataset);

		return 0.0;

	}



	// 关闭数据集

	GDALClose(poDataset);



	return static_cast<double>(geoidHeight);

}


















tri_node::tri_node(int row, int col, int num_neigh_edge, double phi)
{
	this->rows = row;
	this->cols = col;
	this->phase = phi;
	this->b_unwrapped = false;
	this->b_residue = false;
	this->b_balanced = true;
	this->epsilon_height = 0.0;
	this->vel = 0.0;
	if (num_neigh_edge > 0)
	{
		this->neigh_edges.assign(num_neigh_edge, -1);
	}
}

int tri_node::get_phase(double* phi) const
{
	if (phi == NULL)
	{
		fprintf(stderr, "get_phase(): input check failed!\n\n");
		return -1;
	}
	*phi = this->phase;
	return 0;
}

int tri_node::get_pos(int* rows, int* cols) const
{
	if (rows == NULL ||
		cols == NULL)
	{
		fprintf(stderr, "tri_node::get_pos(): input check failed!\n\n");
		return -1;
	}
	*rows = this->rows;
	*cols = this->cols;
	return 0;
}

int tri_node::set_phase(double phi)
{
	this->phase = phi;
	return 0;
}

const std::vector<long>& tri_node::get_neigh_edges() const
{
	return this->neigh_edges;
}

int tri_node::add_neigh_edge(long edge_idx)
{
	for (auto& edge : this->neigh_edges)
	{
		if (edge == -1)
		{
			edge = edge_idx;
			return 0;
		}
	}
	return -1;
}

int tri_node::set_status(bool b_unwrapped)
{
	this->b_unwrapped = b_unwrapped;
	return 0;
}

int tri_node::set_balance(bool b_balanced)
{
	this->b_balanced = b_balanced;
	return 0;
}

int tri_node::print_neighbour() const
{
	if (this->neigh_edges.empty())
	{
		fprintf(stdout, "no neighbour edges!\n");
		return 0;
	}
	for (long edge : this->neigh_edges)
	{
		fprintf(stdout, "%ld ", edge);
	}
	fprintf(stdout, "\n");
	return 0;
}

int tri_node::get_num_neigh(int* num_neigh) const
{
	if (num_neigh == NULL)
	{
		return -1;
	}
	*num_neigh = static_cast<int>(this->neigh_edges.size());
	return 0;
}

int tri_node::get_distance(const tri_node& node, double* distance) const
{
	*distance = sqrt(((double)node.rows - (double)this->rows) * ((double)node.rows - (double)this->rows) +
		((double)node.cols - (double)this->cols) * ((double)node.cols - (double)this->cols));
	return 0;
}

bool tri_node::get_status() const
{
	return this->b_unwrapped;
}

bool tri_node::get_balance() const
{
	return this->b_balanced;
}

bool tri_node::is_residue_node() const
{
	return this->b_residue;
}

int tri_node::set_residue(bool b_res)
{
	this->b_residue = b_res;
	return 0;
}

double tri_node::get_vel() const
{
	return this->vel;
}

double tri_node::get_height() const
{
	return this->epsilon_height;
}

int tri_node::set_vel(double vel)
{
	this->vel = vel;
	return 0;
}

int tri_node::set_height(double height)
{
	this->epsilon_height = height;
	return 0;
}






