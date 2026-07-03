#include "stdafx.h"
#include "../include/AtmosphericCorrection.h"

bool computePhaseElevationRegression(
    const cv::Mat& phase,
    const cv::Mat& coherence,
    bool hasCoherence,
    const cv::Mat& dem,
    bool hasDem,
    const cv::Mat& latMat,
    const cv::Mat& lonMat,
    bool hasLatLon,
    const RegressionParams& params,
    cv::Mat& correctedPhase,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb
) {
    if (phase.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输入相位矩阵不能为空。");
        }
        return false;
    }
    if (correctedPhase.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输出 correctedPhase 矩阵必须由调用者预先分配。");
        }
        return false;
    }
    if (correctedPhase.size() != phase.size() || correctedPhase.type() != phase.type()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输出矩阵尺寸或类型与输入不匹配。");
        }
        return false;
    }
    if (hasCoherence && coherence.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "指定了使用相干性，但相干系数矩阵为空。");
        }
        return false;
    }
    if (hasDem && dem.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "指定了使用 DEM，但 DEM 矩阵为空。");
        }
        return false;
    }
    if (hasLatLon && (latMat.empty() || lonMat.empty())) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "指定了使用经纬度，但经度或纬度矩阵为空。");
        }
        return false;
    }

    int rows = phase.rows;
    int cols = phase.cols;

    if (cb) cb(10, "正在筛选用于回归拟合的有效像元...");

    std::vector<int> valid_rows_idx;
    std::vector<int> valid_cols_idx;
    std::vector<double> valid_phase_vals;
    std::vector<double> valid_elev_vals;
    std::vector<double> valid_lat_vals;
    std::vector<double> valid_lon_vals;

    // 预留容量以优化性能
    valid_rows_idx.reserve(rows * cols / 2);
    valid_cols_idx.reserve(rows * cols / 2);
    valid_phase_vals.reserve(rows * cols / 2);
    if (hasDem) valid_elev_vals.reserve(rows * cols / 2);
    if (hasLatLon) {
        valid_lat_vals.reserve(rows * cols / 2);
        valid_lon_vals.reserve(rows * cols / 2);
    }

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float ph = phase.at<float>(r, c);
            if (std::isnan(ph) || std::isinf(ph)) continue;

            // 相干性掩膜过滤
            if (hasCoherence) {
                float coh = coherence.at<float>(r, c);
                if (coh < params.coherenceThresh || std::isnan(coh)) continue;
            }

            double elev_val = 0.0;
            if (hasDem) {
                elev_val = static_cast<double>(dem.at<float>(r, c));
                if (std::isnan(elev_val) || std::isinf(elev_val)) continue;
            }

            double lat_val = 0.0, lon_val = 0.0;
            if (hasLatLon) {
                lat_val = static_cast<double>(latMat.at<float>(r, c));
                lon_val = static_cast<double>(lonMat.at<float>(r, c));
                if (std::isnan(lat_val) || std::isnan(lon_val)) continue;
            }

            valid_rows_idx.push_back(r);
            valid_cols_idx.push_back(c);
            valid_phase_vals.push_back(static_cast<double>(ph));
            if (hasDem) valid_elev_vals.push_back(elev_val);
            if (hasLatLon) {
                valid_lat_vals.push_back(lat_val);
                valid_lon_vals.push_back(lon_val);
            }
        }
    }

    int n_valid = static_cast<int>(valid_phase_vals.size());
    if (n_valid < 10) {
        // 有效点数太少，直接将输入相位复制到输出，跳过回归计算
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "警告：有效像元数少于 10 个，跳过回归计算。");
        }
        phase.copyTo(correctedPhase);
        if (cb) cb(100, "有效像元数不足，回归跳过。");
        return true;
    }

    if (cb) cb(40, "正在构建回归方程设计矩阵...");

    // 计算设计矩阵列数
    int n_cols = 1; // 常数项
    if (hasDem) n_cols += 1; // 高程一次项
    if (hasLatLon) n_cols += 2; // 纬度、经度一次项
    if (params.polyOrder >= 2) {
        if (hasDem) n_cols += 1; // 高程二次项
        if (hasLatLon) n_cols += 2; // 纬度、经度二次项
    }

    // 构建设计矩阵 X 和观测向量 y
    Eigen::MatrixXd X(n_valid, n_cols);
    Eigen::VectorXd y(n_valid);

    for (int i = 0; i < n_valid; i++) {
        int col_idx = 0;
        X(i, col_idx++) = 1.0; // 常数项
        if (hasDem) {
            X(i, col_idx++) = valid_elev_vals[i];
        }
        if (hasLatLon) {
            X(i, col_idx++) = valid_lat_vals[i];
            X(i, col_idx++) = valid_lon_vals[i];
        }
        if (params.polyOrder >= 2) {
            if (hasDem) {
                X(i, col_idx++) = valid_elev_vals[i] * valid_elev_vals[i];
            }
            if (hasLatLon) {
                X(i, col_idx++) = valid_lat_vals[i] * valid_lat_vals[i];
                X(i, col_idx++) = valid_lon_vals[i] * valid_lon_vals[i];
            }
        }
        y(i) = valid_phase_vals[i];
    }

    if (cb) cb(70, "正在利用最小二乘法 (Eigen LDLT) 求解回归系数...");
    Eigen::VectorXd beta;
    try {
        beta = (X.transpose() * X).ldlt().solve(X.transpose() * y);
    } catch (...) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "Eigen 最小二乘求解过程中发生未知异常。");
        }
        phase.copyTo(correctedPhase);
        return false;
    }

    // 检查求解是否成功
    if ((X.transpose() * X).ldlt().info() != Eigen::Success) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "回归方程求解失败 (设计矩阵奇异或病态)。");
        }
        phase.copyTo(correctedPhase);
        return false;
    }

    if (cb) cb(90, "正在从原始相位中扣除趋势面成分...");
    // 默认输出为输入 phase 的克隆（确保无效点的相位保留原样）
    phase.copyTo(correctedPhase);

    // 扣除趋势面 (APS)
    #pragma omp parallel for
    for (int i = 0; i < n_valid; i++) {
        double aps = 0.0;
        for (int j = 0; j < n_cols; j++) {
            aps += X(i, j) * beta[j];
        }
        int r = valid_rows_idx[i];
        int c = valid_cols_idx[i];
        correctedPhase.at<float>(r, c) = static_cast<float>(valid_phase_vals[i] - aps);
    }

    if (cb) cb(100, "相位-高程多项式回归与去轨道趋势计算完成。");
    return true;
}
