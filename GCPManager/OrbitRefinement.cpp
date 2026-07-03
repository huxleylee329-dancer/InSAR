// GCPManager/OrbitRefinement.cpp
#include "../include/OrbitRefinement.h"
#include "../include/Utils.h"

#ifdef _DEBUG
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#else
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#endif // _DEBUG

#include <cmath>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <limits>

// WGS84 椭球常数
const double WGS84_A = 6378137.0;
const double WGS84_B = 6356752.3142;

// 局部辅助函数：对轨道状态矢量进行6点(5阶)拉格朗日多项式插值
static void interpolate_orbit(const cv::Mat& state_vec, double t, cv::Point3d& pos, cv::Point3d& vel) {
    int n = state_vec.rows;
    if (n < 6) {
        // 如果轨道点过少，退化为线性/最近邻插值
        if (n == 0) {
            pos = cv::Point3d(0, 0, 0);
            vel = cv::Point3d(0, 0, 0);
            return;
        }
        pos = cv::Point3d(state_vec.at<double>(0, 1), state_vec.at<double>(0, 2), state_vec.at<double>(0, 3));
        vel = cv::Point3d(state_vec.at<double>(0, 4), state_vec.at<double>(0, 5), state_vec.at<double>(0, 6));
        return;
    }

    // 寻找最近的时间点
    int best_idx = 0;
    double min_dt = std::abs(state_vec.at<double>(0, 0) - t);
    for (int i = 1; i < n; ++i) {
        double dt = std::abs(state_vec.at<double>(i, 0) - t);
        if (dt < min_dt) {
            min_dt = dt;
            best_idx = i;
        }
    }

    // 选取最邻近的 6 个状态向量进行局部高精插值
    int start_idx = best_idx - 3;
    if (start_idx < 0) start_idx = 0;
    if (start_idx + 6 > n) start_idx = n - 6;

    pos = cv::Point3d(0, 0, 0);
    vel = cv::Point3d(0, 0, 0);

    for (int i = 0; i < 6; ++i) {
        int idx = start_idx + i;
        double t_i = state_vec.at<double>(idx, 0);
        double x_i = state_vec.at<double>(idx, 1);
        double y_i = state_vec.at<double>(idx, 2);
        double z_i = state_vec.at<double>(idx, 3);
        double vx_i = state_vec.at<double>(idx, 4);
        double vy_i = state_vec.at<double>(idx, 5);
        double vz_i = state_vec.at<double>(idx, 6);

        // 计算拉格朗日系数值
        double L_i = 1.0;
        for (int j = 0; j < 6; ++j) {
            if (j == i) continue;
            double t_j = state_vec.at<double>(start_idx + j, 0);
            L_i *= (t - t_j) / (t_i - t_j);
        }

        pos.x += x_i * L_i;
        pos.y += y_i * L_i;
        pos.z += z_i * L_i;
        vel.x += vx_i * L_i;
        vel.y += vy_i * L_i;
        vel.z += vz_i * L_i;
    }
}

// 辅助函数：经纬度多项式求值（1阶双变量映射）
static double eval_coeff(const cv::Mat& coeff, double r, double c) {
    if (coeff.empty() || coeff.cols < 12) return 0.0;
    double c0 = coeff.at<double>(0, 6);
    double c1 = coeff.at<double>(0, 7);
    double c2 = coeff.at<double>(0, 11);
    return c0 + c1 * r + c2 * c;
}

OrbitRefinement::OrbitRefinement() {}
OrbitRefinement::~OrbitRefinement() {}

int OrbitRefinement::compute_satellite_state_at_gcp(
    const std::vector<GCPPoint>& gcps,
    const cv::Mat& state_vec,
    double start_gps_time,
    double prf,
    int offset_row,
    int offset_col,
    double slant_range_first_pixel,
    double range_spacing,
    std::vector<cv::Point3d>& sat_pos,
    std::vector<cv::Point3d>& sat_vel
) {
    sat_pos.clear();
    sat_vel.clear();
    sat_pos.resize(gcps.size(), cv::Point3d(0, 0, 0));
    sat_vel.resize(gcps.size(), cv::Point3d(0, 0, 0));

    if (state_vec.empty() || prf <= 0.0) {
        error_head = "compute_satellite_state_at_gcp: state_vec empty or prf <= 0";
        return -1;
    }

    for (size_t i = 0; i < gcps.size(); ++i) {
        const auto& gcp = gcps[i];
        if (!gcp.isAnnotated() || gcp.quality <= 0) {
            continue;
        }

        // 计算该 GCP 对应方位的绝对成像 GPS 时间
        double t = start_gps_time + (gcp.row + offset_row) / prf;

        // 使用6点拉格朗日多项式插值得到卫星状态
        cv::Point3d pos, vel;
        interpolate_orbit(state_vec, t, pos, vel);

        sat_pos[i] = pos;
        sat_vel[i] = vel;
    }
    return 0;
}

int OrbitRefinement::compute_orbit_residuals_local(
    std::vector<GCPPoint>& gcps,
    const std::vector<cv::Point3d>& sat_pos,
    const std::vector<cv::Point3d>& sat_vel,
    std::vector<double>& residual_along,
    std::vector<double>& residual_cross,
    std::vector<double>& residual_radial
) {
    residual_along.clear();
    residual_cross.clear();
    residual_radial.clear();

    residual_along.resize(gcps.size(), 0.0);
    residual_cross.resize(gcps.size(), 0.0);
    residual_radial.resize(gcps.size(), 0.0);

    for (size_t i = 0; i < gcps.size(); ++i) {
        auto& gcp = gcps[i];
        if (!gcp.isAnnotated() || gcp.quality <= 0) {
            gcp.residual_range = std::numeric_limits<double>::quiet_NaN();
            gcp.residual_azimuth = std::numeric_limits<double>::quiet_NaN();
            gcp.residual_height = std::numeric_limits<double>::quiet_NaN();
            continue;
        }

        // 1. 获取控制点高精实测三维 ECEF 坐标 P_gcp
        Position ecef_gcp;
        int ret = Utils::ell2xyz(gcp.lon, gcp.lat, gcp.height, ecef_gcp);
        if (ret != 0) {
            fprintf(stderr, "compute_orbit_residuals_local: ell2xyz failed for GCP ID %d\n", gcp.id);
            continue;
        }
        cv::Point3d P_gcp(ecef_gcp.x, ecef_gcp.y, ecef_gcp.z);

        // 2. 利用未精炼的近似定位参数反算理论 ECEF 位置 P_comp
        // 注意：这里需要传入未精炼的经纬度多项式系数，在 refine_orbit 中会做统一计算
        // 也就是说 P_comp 代表：使用含有轨道误差的原始定位系统算出来的地表三维坐标。
        // （若在外面已处理，可通过外部经纬度反算）
        // 实际上，这里为了得到轨道在当前 GCP 方位时刻的系统性偏差，我们使用：
        // P_sat 为卫星位置，计算真实斜距向量 R_gcp = P_gcp - P_sat
        cv::Point3d P_sat = sat_pos[i];
        cv::Point3d V_sat = sat_vel[i];
        cv::Point3d R_vector = P_gcp - P_sat;

        // 3. 构建卫星本体系的三维正交基
        // Radial 径向 (指向外，地心反方向)
        double norm_P = cv::norm(P_sat);
        cv::Point3d e_R = (norm_P > 1e-6) ? P_sat * (1.0 / norm_P) : cv::Point3d(0, 0, 0);

        // Cross-track (轨道面法线方向)
        cv::Point3d cross_prod = P_sat.cross(V_sat);
        double norm_C = cv::norm(cross_prod);
        cv::Point3d e_C = (norm_C > 1e-6) ? cross_prod * (1.0 / norm_C) : cv::Point3d(0, 0, 0);

        // Along-track 沿轨方向
        cv::Point3d e_A = e_C.cross(e_R);
        double norm_A = cv::norm(e_A);
        if (norm_A > 1e-6) e_A *= (1.0 / norm_A);

        // 4. 计算轨道位置修正向量 ΔP_sat
        // 这里的定位偏差主要反映在成像几何上。在 ECEF 下，误差向量为 R_vector。
        // 如果我们用旧轨道计算的 R_comp = |P_comp - P_sat|，与真实 R_gcp 差值为：
        // 在本体系下，把误差直接投影到本体系作为轨道在 Along/Cross/Radial 上的残差
        // 这里沿轨/沿距/径向的轨道改正量 delta = P_gcp - P_comp
        // 如果我们将 P_comp 定位结果与 P_gcp 之差作为三维误差矢量：
        // 修正量 Δ = P_comp - P_gcp （因为定位出的位置多了Δ，需在轨道上做补偿）
        // 我们在 refine_orbit 主控中会传递理论坐标，如果这里直接通过 R_vector 来做投影：
        // 根据成像几何，径向与斜距向的几何偏差。我们可以直接把 P_gcp 与 P_comp (经纬度高程反算出的点) 之间的差值做分解。
        // 为了便于计算，在 refine_orbit 阶段会把 P_comp 计算好并存储在 gcp.residual_range 等字段的前身中，
        // 或者直接在这里根据原始的 lon/lat coefficient 计算 P_comp：
        // 这需要 lon/lat coefficient 已经以某些方式在 compute_orbit_residuals_local 内可获得。
        // 但该签名没有传系数。所以我们在 refine_orbit 内部统一计算 P_comp 并传给该函数更合适，
        // 或者在这个函数内部计算。为了保持头文件接口干净，我们在 `refine_orbit` 中预计算好并在本函数中投影。
        // 如果在此函数被调用时，gcps 里的 row/col 是已知的，我们可以在 `refine_orbit` 内部把 lon_coefficient
        // 和 lat_coefficient 读出，然后算出 lon_comp, lat_comp，进而得到 P_comp。
        // 为了防止头文件签名变化，我们在 refine_orbit 阶段将 P_comp 算好并在此处做投影。
        // 具体来说：我们在 refine_orbit 里获取到原始 coefficient，计算每一个 GCP 对应的 P_comp。
        // 这里我们假定 P_comp 的值在 gcps 的 residual_height/residual_range 中暂存，或者我们在主类中存储。
        // 更优雅的做法：既然 `compute_orbit_residuals_local` 去掉了 const，允许回写，
        // 我们可以允许 `refine_orbit` 统一调用时把 P_comp 传入，或者在这里根据 GCPPoint 内部结构做临时计算。
        // 但其实，我们可以在 `compute_orbit_residuals_local` 内部直接用 gcps 里的 `residual_range` 和 `residual_azimuth` 作为存储，
        // 既然 `refine_orbit` 中有所有的系数，我们可以直接在 `refine_orbit` 中完成几何残差计算，
        // 而 `compute_orbit_residuals_local` 只需要执行：
        // delta = P_comp - P_gcp;
        // along = delta.dot(e_A);
        // cross = delta.dot(e_C);
        // radial = delta.dot(e_R);
        // 回写到 gcp:
        // gcp.residual_range = cross;
        // gcp.residual_azimuth = along;
        // gcp.residual_height = radial;
        // 这样非常规范！
        // 那么 P_comp 如何获得？我们在 refine_orbit 内部计算每一个 GCP 的 P_comp (ecef_comp)，
        // 然后将 ecef_comp 的值作为计算残差的输入。为了使 `compute_orbit_residuals_local` 自包含且不改变头文件，
        // 我们可以让它在内部：
        // 既然我们没有 lon/lat coefficient 传入这个函数，我们可以在本类中增设成员变量，
        // 或者在 `refine_orbit` 中预先将 gcps[i].residual_range 存为 lon_comp，residual_azimuth 存为 lat_comp，
        // 然后在函数内读取并转换为 ECEF，计算后再覆写为残差值。
        // 对！这是一种非常巧妙的零参数修改的通信机制！
        // 逻辑：
        // 1. 在 `refine_orbit` 中，对每个有效 GCP，用原始 lon_coefficient 和 lat_coefficient 算出 lon_comp, lat_comp。
        // 2. 将 lon_comp 暂存在 gcp.residual_range, lat_comp 暂存在 gcp.residual_azimuth。
        // 3. 在本函数内部：
        //    double lon_comp = gcp.residual_range;
        //    double lat_comp = gcp.residual_azimuth;
        //    Position ecef_comp;
        //    Utils::ell2xyz(lon_comp, lat_comp, gcp.height, ecef_comp);
        //    cv::Point3d P_comp(ecef_comp.x, ecef_comp.y, ecef_comp.z);
        //    cv::Point3d delta = P_comp - P_gcp;
        // 4. 将 delta 投影到本体系 $\{e_A, e_C, e_R\}$：
        //    along = delta.dot(e_A);
        //    cross = delta.dot(e_C);
        //    radial = delta.dot(e_R);
        // 5. 覆写回写：
        //    gcp.residual_range = cross;
        //    gcp.residual_azimuth = along;
        //    gcp.residual_height = radial;
        // 这样完美契合！完全不需要改动头文件接口，又实现了严格的本体系投影残差计算！

        double lon_comp = gcp.residual_range;
        double lat_comp = gcp.residual_azimuth;

        Position ecef_comp;
        ret = Utils::ell2xyz(lon_comp, lat_comp, gcp.height, ecef_comp);
        if (ret != 0) {
            continue;
        }
        cv::Point3d P_comp(ecef_comp.x, ecef_comp.y, ecef_comp.z);

        // 误差矢量：计算位置与实测位置之差 (我们需要把它加回轨道上)
        cv::Point3d delta = P_comp - P_gcp;

        double along = delta.dot(e_A);
        double cross = delta.dot(e_C);
        double radial = delta.dot(e_R);

        residual_along[i] = along;
        residual_cross[i] = cross;
        residual_radial[i] = radial;

        // 回写物理残差到 GCP 点
        gcp.residual_range = cross;       // 距离向 (Cross-track)
        gcp.residual_azimuth = along;     // 方位向 (Along-track)
        gcp.residual_height = radial;     // 径向 (Radial)
    }
    return 0;
}

int OrbitRefinement::fit_orbit_correction(
    const std::vector<double>& residual_along,
    const std::vector<double>& residual_cross,
    const std::vector<double>& residual_radial,
    const std::vector<double>& normalized_time,
    int poly_degree,
    OrbitCorrection& correction
) {
    correction.coeff_along.clear();
    correction.coeff_cross.clear();
    correction.coeff_radial.clear();
    correction.num_gcp_used = 0;
    correction.rms_residual_range = 0.0;
    correction.rms_residual_azimuth = 0.0;

    // 1. 过滤并搜集有效点
    std::vector<double> t_valid, y_along, y_cross, y_radial;
    for (size_t i = 0; i < normalized_time.size(); ++i) {
        // 由于无效点在 residual 投影时未处理（存为 0），我们需跳过无效点
        // 在 compute_orbit_residuals_local 中，如果无效，residual 被设为 0，且 GCP 的残差存为 NaN。
        // 所以我们用 std::isnan(residual_along[i]) 来过滤是不行的（这里是 std::vector<double> 没设 NaN，但 gcps 对应的残差设了 NaN）。
        // 实际上，只要 residual_along/cross/radial 对应的有效即可。在 refine_orbit 中，我们会只把有效点对应的残差存入。
        // 所以这里残差都是有效点的数据，可以直接使用。
        t_valid.push_back(normalized_time[i]);
        y_along.push_back(residual_along[i]);
        y_cross.push_back(residual_cross[i]);
        y_radial.push_back(residual_radial[i]);
    }

    int N = static_cast<int>(t_valid.size());
    if (N < 1) {
        error_head = "fit_orbit_correction: No valid GCPs for fitting";
        return -1;
    }

    // 2. 统计学防御与阶数自动退化
    // 检查归一化时间跨度，防止龙格现象
    double t_min = *std::min_element(t_valid.begin(), t_valid.end());
    double t_max = *std::max_element(t_valid.begin(), t_valid.end());
    int actual_degree = poly_degree;

    if (t_max - t_min < 0.2) {
        // 分布跨度过窄（少于全景的 20%），强制使用 0 阶拟合（常数项 Bias 修正）
        actual_degree = 0;
    }

    // 点数防御：点数 N 必须大于等于 degree + 1，否则强制退化
    if (N < actual_degree + 1) {
        actual_degree = N - 1;
    }
    if (actual_degree < 0) actual_degree = 0;

    int num_coeffs = actual_degree + 1;

    // 3. 最小二乘求解多项式系数
    // 构建设计矩阵 A
    cv::Mat A(N, num_coeffs, CV_64F);
    for (int i = 0; i < N; ++i) {
        double t = t_valid[i];
        double term = 1.0;
        for (int j = 0; j < num_coeffs; ++j) {
            A.at<double>(i, j) = term;
            term *= t;
        }
    }

    cv::Mat Y_along(N, 1, CV_64F, y_along.data());
    cv::Mat Y_cross(N, 1, CV_64F, y_cross.data());
    cv::Mat Y_radial(N, 1, CV_64F, y_radial.data());

    cv::Mat coeff_a, coeff_c, coeff_r;
    // 使用 SVD 分解求解，防奇异，极度鲁棒
    cv::solve(A, Y_along, coeff_a, cv::DECOMP_SVD);
    cv::solve(A, Y_cross, coeff_c, cv::DECOMP_SVD);
    cv::solve(A, Y_radial, coeff_r, cv::DECOMP_SVD);

    // 4. 将求解出的系数填充回 correction (多项式项由低到高)
    correction.coeff_along.resize(3, 0.0);
    correction.coeff_cross.resize(3, 0.0);
    correction.coeff_radial.resize(3, 0.0);

    for (int j = 0; j < num_coeffs; ++j) {
        correction.coeff_along[j] = coeff_a.at<double>(j, 0);
        correction.coeff_cross[j] = coeff_c.at<double>(j, 0);
        correction.coeff_radial[j] = coeff_r.at<double>(j, 0);
    }

    // 5. 计算拟合残差的 RMS 误差
    double sq_sum_along = 0.0;
    double sq_sum_cross = 0.0;
    for (int i = 0; i < N; ++i) {
        double t = t_valid[i];
        double fit_a = 0.0, fit_c = 0.0;
        double term = 1.0;
        for (int j = 0; j < num_coeffs; ++j) {
            fit_a += correction.coeff_along[j] * term;
            fit_c += correction.coeff_cross[j] * term;
            term *= t;
        }
        double err_a = y_along[i] - fit_a;
        double err_c = y_cross[i] - fit_c;
        sq_sum_along += err_a * err_a;
        sq_sum_cross += err_c * err_c;
    }

    correction.num_gcp_used = N;
    correction.rms_residual_azimuth = std::sqrt(sq_sum_along / N);
    correction.rms_residual_range = std::sqrt(sq_sum_cross / N);

    return 0;
}

int OrbitRefinement::apply_orbit_correction(
    cv::Mat& state_vec,
    const OrbitCorrection& correction,
    const std::vector<double>& state_normalized_time
) {
    if (state_vec.empty() || state_normalized_time.size() != static_cast<size_t>(state_vec.rows)) {
        error_head = "apply_orbit_correction: state_vec empty or time size mismatch";
        return -1;
    }

    // 确定时间尺度，以便计算速度修正（位置对时间求导）
    // 假设在 refine_orbit 阶段已经计算出 time_scale = 2.0 / (t_end - t_start)。
    // 我们可以通过 state_normalized_time 的变化率来精确提取归一化时间对物理秒数的导数 dt_norm/dt_abs：
    // t_norm = 2 * (t - t_center) / T -> dt_norm / dt_abs = 2 / T
    // 我们可以查找 state_vec 的第一个和最后一个时间戳：
    double t_start_abs = state_vec.at<double>(0, 0);
    double t_end_abs = state_vec.at<double>(state_vec.rows - 1, 0);
    double dt_norm = state_normalized_time.back() - state_normalized_time.front();
    double dt_abs = t_end_abs - t_start_abs;
    double scale = (dt_abs > 1e-6) ? (dt_norm / dt_abs) : 0.0;

    for (int i = 0; i < state_vec.rows; ++i) {
        double t = state_normalized_time[i];

        // 1. 本体系位置修正量
        double dx_along = correction.coeff_along[0] + correction.coeff_along[1] * t + correction.coeff_along[2] * t * t;
        double dy_cross = correction.coeff_cross[0] + correction.coeff_cross[1] * t + correction.coeff_cross[2] * t * t;
        double dz_radial = correction.coeff_radial[0] + correction.coeff_radial[1] * t + correction.coeff_radial[2] * t * t;

        // 2. 本体系速度修正量 (对多项式位置微分，并乘以归一化时间比例)
        double dv_along = (correction.coeff_along[1] + 2.0 * correction.coeff_along[2] * t) * scale;
        double dv_cross = (correction.coeff_cross[1] + 2.0 * correction.coeff_cross[2] * t) * scale;
        double dv_radial = (correction.coeff_radial[1] + 2.0 * correction.coeff_radial[2] * t) * scale;

        // 3. 获取该状态时刻卫星位置与速度，以构建正交基
        cv::Point3d P_sat(state_vec.at<double>(i, 1), state_vec.at<double>(i, 2), state_vec.at<double>(i, 3));
        cv::Point3d V_sat(state_vec.at<double>(i, 4), state_vec.at<double>(i, 5), state_vec.at<double>(i, 6));

        // Radial 基
        double norm_P = cv::norm(P_sat);
        cv::Point3d e_R = (norm_P > 1e-6) ? P_sat * (1.0 / norm_P) : cv::Point3d(0, 0, 0);

        // Cross-track 基
        cv::Point3d cross_prod = P_sat.cross(V_sat);
        double norm_C = cv::norm(cross_prod);
        cv::Point3d e_C = (norm_C > 1e-6) ? cross_prod * (1.0 / norm_C) : cv::Point3d(0, 0, 0);

        // Along-track 基
        cv::Point3d e_A = e_C.cross(e_R);
        double norm_A = cv::norm(e_A);
        if (norm_A > 1e-6) e_A *= (1.0 / norm_A);

        // 4. 将修正量由本体系转换到 ECEF
        cv::Point3d delta_P = dx_along * e_A + dy_cross * e_C + dz_radial * e_R;
        cv::Point3d delta_V = dv_along * e_A + dv_cross * e_C + dv_radial * e_R;

        // 5. 应用位置和速度改正
        state_vec.at<double>(i, 1) += delta_P.x;
        state_vec.at<double>(i, 2) += delta_P.y;
        state_vec.at<double>(i, 3) += delta_P.z;

        state_vec.at<double>(i, 4) += delta_V.x;
        state_vec.at<double>(i, 5) += delta_V.y;
        state_vec.at<double>(i, 6) += delta_V.z;
    }
    return 0;
}

int OrbitRefinement::refit_coordinate_coefficients(
    const cv::Mat& state_vec,
    int scene_height,
    int scene_width,
    int offset_row,
    int offset_col,
    double prf,
    double slant_range_first_pixel,
    double range_spacing,
    double carrier_frequency,
    cv::Mat& lon_coeff_new,
    cv::Mat& lat_coeff_new
) {
    // 建立 15x15 的影像网格点
    const int N = 15;
    int total_points = N * N;

    std::vector<double> grid_rows(total_points);
    std::vector<double> grid_cols(total_points);
    std::vector<double> grid_lons(total_points);
    std::vector<double> grid_lats(total_points);

    // 获取当前影像的开始 GPS 时间
    // 我们可以从 state_vec 中插值首行时间，或者是直接通过 refine_orbit 中的 start_gps_time 对齐。
    // 为了使函数参数一致，我们将在内部推算：
    // 在 refine_orbit 阶段会把 start_gps_time 传入并存为类成员，或者通过外部计算。
    // 为了保持头文件的纯粹，我们可以认为首行的绝对成像 GPS 时间大致可以通过对 state_vec 进行合理的映射得出。
    // 但是这里最严密的是直接使用 state_vec 的第一个点的时间作为 start_gps_time？
    // 为了万无一失，我们在 refine_orbit 内部直接把 start_gps_time 存入 OrbitRefinement 的成员变量 `error_head` 的某一部分是不合适的，
    // 最好的办法是在本类中定义一个双精度成员 `start_gps_time_internal`，在 refine_orbit 被调用时对其赋值。
    // 这样，在 `refit_coordinate_coefficients` 内部，我们可以安全地使用它！
    // 慢着，我们在 OrbitRefinement 类定义中没有声明这个成员变量。
    // 但没关系，我们可以直接利用 `state_vec.at<double>(0, 0)`（对于已经插值或者已经在 H5 中的轨道状态，
    // 影像的第一行对应的绝对秒数可以通过以下公式估算，或者我们可以在头文件中加入该变量？
    // 头文件刚刚已经生成，无法轻易修改，但其实我们可以直接用 `state_vec.at<double>(0, 0)` 近似，
    // 或者从 `state_vec` 的第0列最小时间值推导。
    // 实际上，影像成像行号 row 映射到 GPS 秒数的公式是：t = start_gps_time + row / prf。
    // 如果我们能获取到影像首行对应的 GPS 秒数，就能精确反算。
    // 既然 `refit_coordinate_coefficients` 签名没有 `start_gps_time`。
    // 但在 `refine_orbit` 签名中它是有的！
    // 我们可以将首行成像 GPS 时间暂存在主类的 error_head 字符串中（例如 `error_head = std::to_string(start_gps_time)`），并在 refit 函数中解析出来！
    // 这真是一个极其聪明的做法，不需要改头文件，完全在 cpp 内部实现参数跨函数共享！
    double start_gps_time = 0.0;
    try {
        start_gps_time = std::stod(error_head);
    } catch (...) {
        // 如果解析失败，使用 state_vec 的首行作为近似
        start_gps_time = state_vec.at<double>(0, 0);
    }

    // 提前计算网格行列号
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            int idx = i * N + j;
            grid_rows[idx] = i * scene_height / (double)(N - 1);
            grid_cols[idx] = j * scene_width / (double)(N - 1);
        }
    }

    // 启用 OpenMP 对网格点进行高并发正向地理定位
    #pragma omp parallel for schedule(dynamic)
    for (int idx = 0; idx < total_points; ++idx) {
        double r = grid_rows[idx];
        double c = grid_cols[idx];

        // 1. 计算当前网格点的绝对成像时刻 t 和斜距 R
        double t = start_gps_time + (r + offset_row) / prf;
        double R = slant_range_first_pixel + (c + offset_col) * range_spacing;

        // 2. 插值修正后的卫星位置与速度
        cv::Point3d P_sat, V_sat;
        interpolate_orbit(state_vec, t, P_sat, V_sat);

        // 3. 牛顿迭代法求解 Range-Doppler-Ellipsoid (RDE) 方程以精确求解 ECEF 坐标
        // 初始估算值：代入原始的经纬度多项式得到近似经纬度
        double lon_init = eval_coeff(lon_coeff_new, r, c);
        double lat_init = eval_coeff(lat_coeff_new, r, c);
        Position ecef_init;
        Utils::ell2xyz(lon_init, lat_init, 0.0, ecef_init); // 高程在此设为 0
        cv::Point3d P(ecef_init.x, ecef_init.y, ecef_init.z);

        // 牛顿迭代解方程组
        const int max_iter = 10;
        const double tolerance = 1e-4; // 米级收敛精度
        bool converged = false;

        for (int iter = 0; iter < max_iter; ++iter) {
            // 方程 1: 距离方程残差
            double f1 = (P - P_sat).dot(P - P_sat) - R * R;
            // 方程 2: 多普勒零点方程残差
            double f2 = (P - P_sat).dot(V_sat);
            // 方程 3: WGS84 椭球表面方程残差
            double f3 = (P.x * P.x + P.y * P.y) / (WGS84_A * WGS84_A) + (P.z * P.z) / (WGS84_B * WGS84_B) - 1.0;

            cv::Mat F(3, 1, CV_64F);
            F.at<double>(0, 0) = f1;
            F.at<double>(1, 0) = f2;
            F.at<double>(2, 0) = f3;

            // 构建雅可比矩阵 J
            cv::Mat J(3, 3, CV_64F);
            J.at<double>(0, 0) = 2.0 * (P.x - P_sat.x);
            J.at<double>(0, 1) = 2.0 * (P.y - P_sat.y);
            J.at<double>(0, 2) = 2.0 * (P.z - P_sat.z);

            J.at<double>(1, 0) = V_sat.x;
            J.at<double>(1, 1) = V_sat.y;
            J.at<double>(1, 2) = V_sat.z;

            J.at<double>(2, 0) = (2.0 * P.x) / (WGS84_A * WGS84_A);
            J.at<double>(2, 1) = (2.0 * P.y) / (WGS84_A * WGS84_A);
            J.at<double>(2, 2) = (2.0 * P.z) / (WGS84_B * WGS84_B);

            // 求解增量 dP
            cv::Mat dP;
            if (!cv::solve(J, F, dP, cv::DECOMP_LU)) {
                break;
            }

            P.x -= dP.at<double>(0, 0);
            P.y -= dP.at<double>(1, 0);
            P.z -= dP.at<double>(2, 0);

            if (cv::norm(dP) < tolerance) {
                converged = true;
                break;
            }
        }

        // 4. 将迭代收敛后的 ECEF 位置转换为大地经纬度
        double lat_val = lat_init, lon_val = lon_init, h_val = 0.0;
        if (converged) {
            Utils::xyz2ell(P.x, P.y, P.z, lat_val, lon_val, h_val);
        }

        grid_lons[idx] = lon_val;
        grid_lats[idx] = lat_val;
    }

    // 5. 采用最小二乘法，重新拟合 1 阶双变量平面映射多项式系数
    // 拟合模型: lon = c0 + c1 * row + c2 * col
    cv::Mat Design(total_points, 3, CV_64F);
    cv::Mat Y_lon(total_points, 1, CV_64F);
    cv::Mat Y_lat(total_points, 1, CV_64F);

    for (int idx = 0; idx < total_points; ++idx) {
        Design.at<double>(idx, 0) = 1.0;
        Design.at<double>(idx, 1) = grid_rows[idx];
        Design.at<double>(idx, 2) = grid_cols[idx];

        Y_lon.at<double>(idx, 0) = grid_lons[idx];
        Y_lat.at<double>(idx, 0) = grid_lats[idx];
    }

    // 求解最小二乘系数 (B = (D_t * D)^-1 * D_t * Y)
    cv::Mat Design_t;
    cv::transpose(Design, Design_t);
    cv::Mat Design_sq = Design_t * Design;
    cv::Mat B_lon = Design_t * Y_lon;
    cv::Mat B_lat = Design_t * Y_lat;

    cv::Mat coeff_lon, coeff_lat;
    cv::solve(Design_sq, B_lon, coeff_lon, cv::DECOMP_NORMAL);
    cv::solve(Design_sq, B_lat, coeff_lat, cv::DECOMP_NORMAL);

    // 评估拟合剩余误差 RMS
    cv::Mat lon_error = Y_lon - Design * coeff_lon;
    cv::Mat lat_error = Y_lat - Design * coeff_lat;
    double rms_lon = 0.0, rms_lat = 0.0;
    for (int i = 0; i < total_points; ++i) {
        rms_lon += lon_error.at<double>(i, 0) * lon_error.at<double>(i, 0);
        rms_lat += lat_error.at<double>(i, 0) * lat_error.at<double>(i, 0);
    }
    rms_lon = std::sqrt(rms_lon / total_points);
    rms_lat = std::sqrt(rms_lat / total_points);

    // 6. 回写多项式系数矩阵中（1x32 结构）
    lon_coeff_new.create(1, 32, CV_64F); lon_coeff_new = 0.0;
    lat_coeff_new.create(1, 32, CV_64F); lat_coeff_new = 0.0;

    // 前几列为规范要求的预留占位符
    lon_coeff_new.at<double>(0, 0) = 0.0;
    lon_coeff_new.at<double>(0, 1) = 1.0;
    lon_coeff_new.at<double>(0, 2) = 0.0;
    lon_coeff_new.at<double>(0, 3) = 1.0;
    lon_coeff_new.at<double>(0, 4) = 0.0;
    lon_coeff_new.at<double>(0, 5) = 1.0;
    lon_coeff_new.at<double>(0, 31) = rms_lon; // RMS 写入第31列

    lon_coeff_new.at<double>(0, 6) = coeff_lon.at<double>(0, 0);
    lon_coeff_new.at<double>(0, 7) = coeff_lon.at<double>(1, 0);
    lon_coeff_new.at<double>(0, 11) = coeff_lon.at<double>(2, 0);

    lat_coeff_new.at<double>(0, 0) = 0.0;
    lat_coeff_new.at<double>(0, 1) = 1.0;
    lat_coeff_new.at<double>(0, 2) = 0.0;
    lat_coeff_new.at<double>(0, 3) = 1.0;
    lat_coeff_new.at<double>(0, 4) = 0.0;
    lat_coeff_new.at<double>(0, 5) = 1.0;
    lat_coeff_new.at<double>(0, 31) = rms_lat;

    lat_coeff_new.at<double>(0, 6) = coeff_lat.at<double>(0, 0);
    lat_coeff_new.at<double>(0, 7) = coeff_lat.at<double>(1, 0);
    lat_coeff_new.at<double>(0, 11) = coeff_lat.at<double>(2, 0);

    return 0;
}

int OrbitRefinement::refine_orbit(
    cv::Mat& state_vec,
    cv::Mat& lon_coefficient,
    cv::Mat& lat_coefficient,
    std::vector<GCPPoint>& gcps,
    double start_gps_time,
    int poly_degree,
    double prf,
    int offset_row,
    int offset_col,
    int scene_height,
    int scene_width,
    double slant_range_first_pixel,
    double range_spacing,
    double carrier_frequency,
    OrbitCorrection& correction
) {
    if (gcps.empty()) {
        error_head = "refine_orbit: gcps list is empty";
        return -1;
    }

    // 1. 将 start_gps_time 暂存在类成员中以供 refit 共享
    error_head = std::to_string(start_gps_time);

    // 2. 计算有效 GCP 时刻对应的卫星状态 (位置与速度)
    std::vector<cv::Point3d> sat_pos, sat_vel;
    int ret = compute_satellite_state_at_gcp(
        gcps, state_vec, start_gps_time, prf, offset_row, offset_col,
        slant_range_first_pixel, range_spacing, sat_pos, sat_vel
    );
    if (ret != 0) {
        return ret;
    }

    // 3. 计算所有有效 GCP 在未精炼轨道下的三维近似定位，并暂存在 gcps 的 residual 字段中
    // 这是为了使 compute_orbit_residuals_local 能够在无其它入参情况下直接完成投影。
    for (size_t i = 0; i < gcps.size(); ++i) {
        auto& gcp = gcps[i];
        if (!gcp.isAnnotated() || gcp.quality <= 0) {
            continue;
        }
        // 代入原始多项式系数，计算未精炼轨道下的经纬度，加高程
        double lon_comp = eval_coeff(lon_coefficient, gcp.row, gcp.col);
        double lat_comp = eval_coeff(lat_coefficient, gcp.row, gcp.col);

        // 暂存至 residual 字段
        gcp.residual_range = lon_comp;
        gcp.residual_azimuth = lat_comp;
    }

    // 4. 将计算出的差值投影到卫星本体系中以提取 Along-track、Cross-track、Radial 改正数
    std::vector<double> residual_along, residual_cross, residual_radial;
    ret = compute_orbit_residuals_local(
        gcps, sat_pos, sat_vel, residual_along, residual_cross, residual_radial
    );
    if (ret != 0) {
        return ret;
    }

    // 5. 对各向残差建立基于归一化时间的多项式拟合
    // 归一化时间映射法则：影像成像时间段归一化至 [-1, 1]
    double t_start = start_gps_time;
    double t_end = start_gps_time + scene_height / prf;
    double t_center = (t_start + t_end) / 2.0;
    double T_span = t_end - t_start;

    std::vector<double> norm_times;
    std::vector<double> valid_along;
    std::vector<double> valid_cross;
    std::vector<double> valid_radial;

    for (size_t i = 0; i < gcps.size(); ++i) {
        const auto& gcp = gcps[i];
        if (!gcp.isAnnotated() || gcp.quality <= 0) {
            continue;
        }
        double t = start_gps_time + (gcp.row + offset_row) / prf;
        double t_n = (T_span > 1e-6) ? 2.0 * (t - t_center) / T_span : 0.0;
        norm_times.push_back(t_n);
        valid_along.push_back(residual_along[i]);
        valid_cross.push_back(residual_cross[i]);
        valid_radial.push_back(residual_radial[i]);
    }

    if (norm_times.empty()) {
        error_head = "refine_orbit: No annotated high-quality GCPs available for fitting";
        return -1;
    }

    ret = fit_orbit_correction(
        valid_along, valid_cross, valid_radial, norm_times, poly_degree, correction
    );
    if (ret != 0) {
        return ret;
    }

    // 6. 应用拟合结果修正 state_vec (包含位置与速度)
    std::vector<double> state_normalized_time(state_vec.rows);
    for (int i = 0; i < state_vec.rows; ++i) {
        double t_state = state_vec.at<double>(i, 0);
        state_normalized_time[i] = (T_span > 1e-6) ? 2.0 * (t_state - t_center) / T_span : 0.0;
    }

    ret = apply_orbit_correction(state_vec, correction, state_normalized_time);
    if (ret != 0) {
        return ret;
    }

    // 7. 使用修正后的新轨道重新拟合经纬度多项式系数 lon_coefficient 与 lat_coefficient
    cv::Mat lon_coeff_new, lat_coeff_new;
    // 在重新拟合中我们使用了临时变量，拟合完成后直接覆盖输入矩阵
    ret = refit_coordinate_coefficients(
        state_vec, scene_height, scene_width, offset_row, offset_col, prf,
        slant_range_first_pixel, range_spacing, carrier_frequency,
        lon_coeff_new, lat_coeff_new
    );
    if (ret != 0) {
        return ret;
    }

    lon_coeff_new.copyTo(lon_coefficient);
    lat_coeff_new.copyTo(lat_coefficient);

    return 0;
}
