#include "pch.h"
#include "..\include\DeformationRateField.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <iostream>

#ifdef _DEBUG
#pragma comment(lib, "Utils_d.lib")
#else
#pragma comment(lib, "Utils.lib")
#endif

// Helper function to read values from cv::Mat regardless of its type
static double getMatVal(const cv::Mat& m, int r, int c) {
    if (m.empty()) return 0.0;
    int type = m.type();
    if (type == CV_32FC1) {
        return (double)m.at<float>(r, c);
    } else if (type == CV_64FC1) {
        return m.at<double>(r, c);
    } else if (type == CV_8UC1) {
        return (double)m.at<uchar>(r, c);
    } else if (type == CV_32SC1) {
        return (double)m.at<int>(r, c);
    }
    return 0.0;
}

// Helper to determine Z critical value for confidence intervals
static double getZCriticalValue(double confidence_level) {
    if (confidence_level >= 0.98) return 2.576; // 99%
    if (confidence_level >= 0.93) return 1.96;  // 95%
    if (confidence_level >= 0.85) return 1.645; // 90%
    return 1.96; // default 95%
}

DeformationRateField::DeformationRateField() {
    error_head = "DeformationRateField::";
}

DeformationRateField::~DeformationRateField() {}

int DeformationRateField::estimate_nonlinear_velocity(
    const cv::Mat& time_series,
    const cv::Mat& temporal,
    const cv::Mat& spatial,
    const cv::Mat& mask,
    const cv::Mat& coherence,
    const RateFieldParams& params,
    RateFieldResult& result,
    DeformationProgressCallback cb
) {
    if (time_series.empty() || temporal.empty() || mask.empty()) {
        std::cerr << error_head << "estimate_nonlinear_velocity: empty inputs." << std::endl;
        return -1;
    }

    int N_times = time_series.cols;
    int N_valid = time_series.rows;
    int rows = mask.rows;
    int cols = mask.cols;

    // Initialize result matrices
    result.velocity_nonlinear = cv::Mat::zeros(rows, cols, CV_64FC1);
    result.acceleration = cv::Mat::zeros(rows, cols, CV_64FC1);
    result.mask = mask.clone();

    // Map 2D valid pixels to 1D time_series index
    std::vector<std::pair<int, int>> valid_coords;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (getMatVal(mask, r, c) != 0) {
                valid_coords.push_back({r, c});
            }
        }
    }

    if (valid_coords.size() != (size_t)N_valid) {
        std::cerr << error_head << "estimate_nonlinear_velocity: Mask valid pixel count (" 
                  << valid_coords.size() << ") does not match time_series rows (" << N_valid << ")." << std::endl;
        return -1;
    }

    bool use_weights = (coherence.rows == N_valid && coherence.cols == N_times);
    bool use_spatial = (!spatial.empty() && spatial.cols == N_times);

    // Build the design matrix A (global for all pixels)
    // Parameters: [v1 (velocity), v2 (acceleration), kappa (elevation baseline coefficient)]
    int num_params = 3;
    cv::Mat A(N_times, num_params, CV_64FC1);
    for (int i = 0; i < N_times; i++) {
        double t = getMatVal(temporal, 0, i) / 365.25; // Convert days to years
        double B = use_spatial ? getMatVal(spatial, 0, i) : 0.0;
        A.at<double>(i, 0) = t;
        A.at<double>(i, 1) = t * t;
        A.at<double>(i, 2) = B;
    }

    std::atomic<int> completed_rows(0);
    std::atomic<bool> cancel_flag(false);
    int step = std::max(1, N_valid / 100);

    // Solve for each pixel using OpenMP parallelization
    #pragma omp parallel for
    for (int idx = 0; idx < N_valid; idx++) {
        if (cancel_flag) continue;
        int r = valid_coords[idx].first;
        int c = valid_coords[idx].second;

        cv::Mat y(N_times, 1, CV_64FC1);
        for (int i = 0; i < N_times; i++) {
            y.at<double>(i, 0) = getMatVal(time_series, idx, i);
        }

        // Apply weights if coherence matrix is provided
        cv::Mat A_W = A.clone();
        cv::Mat y_W = y.clone();

        if (use_weights) {
            for (int i = 0; i < N_times; i++) {
                double w = getMatVal(coherence, idx, i);
                if (w < 0.001) w = 0.001; // Avoid divide by zero
                double sqrt_w = sqrt(w);
                A_W.at<double>(i, 0) *= sqrt_w;
                A_W.at<double>(i, 1) *= sqrt_w;
                A_W.at<double>(i, 2) *= sqrt_w;
                y_W.at<double>(i, 0) *= sqrt_w;
            }
        }

        cv::Mat x_vec;
        bool solved = cv::solve(A_W, y_W, x_vec, cv::DECOMP_SVD);
        if (solved) {
            result.velocity_nonlinear.at<double>(r, c) = x_vec.at<double>(0, 0); // v1
            result.acceleration.at<double>(r, c) = x_vec.at<double>(1, 0);       // v2
        } else {
            result.velocity_nonlinear.at<double>(r, c) = 0.0;
            result.acceleration.at<double>(r, c) = 0.0;
        }

        int current = ++completed_rows;
        if (cb && current % step == 0) {
            if (!cb(current * 100 / N_valid, "Estimating nonlinear velocity...")) {
                cancel_flag = true;
            }
        }
    }

    if (cancel_flag) return -2;

    return 0;
}

int DeformationRateField::estimate_uncertainty(
    const cv::Mat& time_series,
    const cv::Mat& temporal,
    const cv::Mat& coherence,
    const cv::Mat& velocity,
    double         confidence_level,
    cv::Mat&       velocity_std,
    cv::Mat&       velocity_lower,
    cv::Mat&       velocity_upper,
    DeformationProgressCallback cb
) {
    if (time_series.empty() || temporal.empty() || velocity.empty()) {
        std::cerr << error_head << "estimate_uncertainty: empty inputs." << std::endl;
        return -1;
    }

    int N_times = time_series.cols;
    int N_valid = time_series.rows;
    int rows = velocity.rows;
    int cols = velocity.cols;

    velocity_std = cv::Mat::zeros(rows, cols, CV_64FC1);
    velocity_lower = cv::Mat::zeros(rows, cols, CV_64FC1);
    velocity_upper = cv::Mat::zeros(rows, cols, CV_64FC1);

    std::vector<std::pair<int, int>> valid_coords;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            // A pixel is valid if velocity is non-zero (or wait, velocity could be exactly 0, 
            // so we check if the time series row exists by tracking coordinates of non-zero mask pixels)
            // Let's use the valid coordinate mapping. We will assume the velocity map corresponds to the same grid layout.
            if (velocity.at<double>(r, c) != 0.0 || getMatVal(velocity, r, c) != 0.0) {
                valid_coords.push_back({r, c});
            }
        }
    }

    // If valid coords list is empty, map all pixels
    if (valid_coords.empty()) {
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                if (getMatVal(velocity, r, c) != 0.0) {
                    valid_coords.push_back({r, c});
                }
            }
        }
    }

    bool use_weights = (coherence.rows == N_valid && coherence.cols == N_times);
    double z = getZCriticalValue(confidence_level);

    // Design matrix A (for linear propagation A = [t])
    cv::Mat A(N_times, 1, CV_64FC1);
    for (int i = 0; i < N_times; i++) {
        A.at<double>(i, 0) = getMatVal(temporal, 0, i) / 365.25;
    }

    std::atomic<int> completed_rows(0);
    std::atomic<bool> cancel_flag(false);
    int step = std::max(1, (int)valid_coords.size() / 100);

    #pragma omp parallel for
    for (int idx = 0; idx < (int)valid_coords.size(); idx++) {
        if (cancel_flag) continue;
        int r = valid_coords[idx].first;
        int c = valid_coords[idx].second;
        double v = velocity.at<double>(r, c);

        // Compute residuals r_i = d_i - v * t_i
        double sum_r = 0.0;
        double sum_sq_r = 0.0;
        std::vector<double> residuals(N_times);

        for (int i = 0; i < N_times; i++) {
            double t = A.at<double>(i, 0);
            double d = getMatVal(time_series, idx, i);
            double res = d - v * t;
            residuals[i] = res;
            sum_r += res;
            sum_sq_r += res * res;
        }

        double mean_r = sum_r / N_times;
        double var_phi = (sum_sq_r - N_times * mean_r * mean_r) / (N_times - 1 > 0 ? N_times - 1 : 1);
        if (var_phi < 1e-6) var_phi = 1e-6; // Safety floor

        // Compute sum of weights * t^2
        double sum_wt2 = 0.0;
        for (int i = 0; i < N_times; i++) {
            double t = A.at<double>(i, 0);
            double w = use_weights ? getMatVal(coherence, idx, i) : 1.0;
            if (w < 0.001) w = 0.001;
            sum_wt2 += w * t * t;
        }

        if (sum_wt2 < 1e-6) sum_wt2 = 1e-6;

        double sigma2_v = var_phi / sum_wt2;
        double sigma_v = sqrt(sigma2_v);

        velocity_std.at<double>(r, c) = sigma_v;
        velocity_lower.at<double>(r, c) = v - z * sigma_v;
        velocity_upper.at<double>(r, c) = v + z * sigma_v;

        int current = ++completed_rows;
        if (cb && current % step == 0) {
            if (!cb(current * 100 / (int)valid_coords.size(), "Estimating velocity uncertainty...")) {
                cancel_flag = true;
            }
        }
    }

    if (cancel_flag) return -2;

    return 0;
}

int DeformationRateField::assess_quality(
    const cv::Mat& velocity,
    const cv::Mat& velocity_std,
    const cv::Mat& temporal_coherence,
    double coherence_threshold_high,
    double coherence_threshold_mid,
    double uncertainty_threshold_high,
    double uncertainty_threshold_mid,
    cv::Mat& quality_mask,
    double& mean_velocity,
    double& std_velocity,
    int&    num_valid_pixels
) {
    if (velocity.empty() || velocity_std.empty() || temporal_coherence.empty()) {
        std::cerr << error_head << "assess_quality: empty inputs." << std::endl;
        return -1;
    }

    int rows = velocity.rows;
    int cols = velocity.cols;
    quality_mask = cv::Mat::zeros(rows, cols, CV_32SC1);

    double sum_vel = 0.0;
    double sum_sq_vel = 0.0;
    int valid_count = 0;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double vel = velocity.at<double>(r, c);
            double std_val = velocity_std.at<double>(r, c);
            double coh = temporal_coherence.at<double>(r, c);

            // Check if this pixel is valid (has standard deviation)
            if (std_val > 0.0) {
                int q = 0;
                if (coh >= coherence_threshold_high && std_val <= uncertainty_threshold_high) {
                    q = 2; // High
                } else if (coh >= coherence_threshold_mid && std_val <= uncertainty_threshold_mid) {
                    q = 1; // Medium
                } else {
                    q = 0; // Low
                }

                quality_mask.at<int>(r, c) = q;
                sum_vel += vel;
                sum_sq_vel += vel * vel;
                valid_count++;
            }
        }
    }

    num_valid_pixels = valid_count;
    if (valid_count > 0) {
        mean_velocity = sum_vel / valid_count;
        double var_vel = (sum_sq_vel / valid_count) - (mean_velocity * mean_velocity);
        std_velocity = sqrt(std::max(0.0, var_vel));
    } else {
        mean_velocity = 0.0;
        std_velocity = 0.0;
    }

    return 0;
}

int DeformationRateField::generate_statistics_report(
    const RateFieldResult& result,
    std::string& report_text
) {
    std::stringstream ss;
    ss << "==================================================\n";
    ss << "          Deformation Rate Field Analysis Report   \n";
    ss << "==================================================\n\n";
    ss << "Number of Valid Pixels: " << result.num_valid_pixels << "\n";
    ss << "Mean Velocity: " << result.mean_velocity << " mm/year\n";
    ss << "Global Velocity Std Dev: " << result.std_velocity << " mm/year\n\n";

    int q_high = 0, q_mid = 0, q_low = 0;
    if (!result.quality_mask.empty()) {
        for (int r = 0; r < result.quality_mask.rows; r++) {
            for (int c = 0; c < result.quality_mask.cols; c++) {
                // Check if this pixel was assessed (non-zero mask)
                if (result.velocity_std.at<double>(r, c) > 0.0) {
                    int q = result.quality_mask.at<int>(r, c);
                    if (q == 2) q_high++;
                    else if (q == 1) q_mid++;
                    else q_low++;
                }
            }
        }
    }

    int total_valid = q_high + q_mid + q_low;
    if (total_valid > 0) {
        ss << "Quality Assessment Summary:\n";
        ss << "  - High Quality (Level 2): " << q_high << " pixels (" 
           << (q_high * 100.0 / total_valid) << "%)\n";
        ss << "  - Medium Quality (Level 1): " << q_mid << " pixels (" 
           << (q_mid * 100.0 / total_valid) << "%)\n";
        ss << "  - Low Quality (Level 0): " << q_low << " pixels (" 
           << (q_low * 100.0 / total_valid) << "%)\n\n";
    }

    ss << "==================================================\n";
    report_text = ss.str();
    return 0;
}

int DeformationRateField::analyze_rate_field(
    const cv::Mat& time_series,
    const cv::Mat& temporal,
    const cv::Mat& spatial,
    const cv::Mat& mask,
    const cv::Mat& coherence,
    const cv::Mat& velocity_linear,
    const RateFieldParams& params,
    RateFieldResult& result,
    DeformationProgressCallback cb
) {
    int ret = 0;
    
    // Model Type 2: Quadratic (Nonlinear) Fit
    if (params.model_type == 2) {
        ret = estimate_nonlinear_velocity(time_series, temporal, spatial, mask, coherence, params, result, cb);
        if (ret != 0) return ret;
    } 
    // Model Type 1: Linear Mode (reuses velocity_linear)
    else {
        result.velocity_nonlinear = velocity_linear.clone();
        result.acceleration = cv::Mat::zeros(velocity_linear.size(), CV_64FC1);
        result.mask = mask.clone();
    }

    // Estimate Uncertainty
    cv::Mat vel_to_estimate = (params.model_type == 2) ? result.velocity_nonlinear : velocity_linear;
    ret = estimate_uncertainty(time_series, temporal, coherence, vel_to_estimate, 
                               params.confidence_level, result.velocity_std, 
                               result.velocity_lower, result.velocity_upper, cb);
    if (ret != 0) return ret;

    // Estimate Fallback Temporal Coherence for Quality Assessment 
    // (If not provided in quality assessment stage, we compute a fallback using C-band wavelength 55.6mm)
    int rows = mask.rows;
    int cols = mask.cols;
    cv::Mat temporal_coherence = cv::Mat::zeros(rows, cols, CV_64FC1);
    
    std::vector<std::pair<int, int>> valid_coords;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (getMatVal(mask, r, c) != 0) {
                valid_coords.push_back({r, c});
            }
        }
    }

    int N_times = time_series.cols;
    int N_valid = time_series.rows;
    
    std::atomic<int> completed_rows(0);
    std::atomic<bool> cancel_flag(false);
    int step = std::max(1, N_valid / 100);

    #pragma omp parallel for
    for (int idx = 0; idx < N_valid; idx++) {
        if (cancel_flag) continue;
        int r = valid_coords[idx].first;
        int c = valid_coords[idx].second;
        double v = vel_to_estimate.at<double>(r, c);

        double real_sum = 0.0;
        double imag_sum = 0.0;

        for (int i = 0; i < N_times; i++) {
            double t = getMatVal(temporal, 0, i) / 365.25;
            double d = getMatVal(time_series, idx, i);
            double residual = d - v * t;
            
            // Convert displacement residual (mm) to phase (rad) using C-band S1 wavelength 55.6mm
            double phase_res = residual * (-4.0 * PI / 55.6);
            real_sum += cos(phase_res);
            imag_sum += sin(phase_res);
        }

        temporal_coherence.at<double>(r, c) = sqrt(real_sum * real_sum + imag_sum * imag_sum) / N_times;

        int current = ++completed_rows;
        if (cb && current % step == 0) {
            if (!cb(current * 100 / N_valid, "Estimating temporal coherence...")) {
                cancel_flag = true;
            }
        }
    }
    if (cancel_flag) return -2;

    // Default thresholds for the all-in-one analysis
    double coherence_threshold_high = 0.5;
    double coherence_threshold_mid = 0.3;
    double uncertainty_threshold_high = 2.0;
    double uncertainty_threshold_mid = 5.0;

    ret = assess_quality(vel_to_estimate, result.velocity_std, temporal_coherence,
                         coherence_threshold_high, coherence_threshold_mid,
                         uncertainty_threshold_high, uncertainty_threshold_mid,
                         result.quality_mask, result.mean_velocity, result.std_velocity,
                         result.num_valid_pixels);

    return ret;
}
