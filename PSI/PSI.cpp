#include "pch.h"
#include "..\include\PSI.h"
#include <cmath>
#include <complex>
#include <queue>
#include <algorithm>
#include <vector>

PSI::PSI() {
    error_head = "[PSI-DLL]";
}

PSI::~PSI() {}

// ==================== 步骤1: 离差指数计算 ====================
int PSI::compute_ps_candidates(
    const cv::Mat& sum_amplitude,
    const cv::Mat& sum_amplitude_sq,
    int num_images,
    double da_threshold,
    cv::Mat& ps_mask,
    cv::Mat& amplitude_dispersion,
    PSIProgressCallback cb
) {
    if (sum_amplitude.empty() || sum_amplitude_sq.empty()) return -1;
    if (sum_amplitude.type() != CV_32FC1 || sum_amplitude_sq.type() != CV_32FC1) return -1;
    if (sum_amplitude.size() != sum_amplitude_sq.size()) return -1;

    int rows = sum_amplitude.rows;
    int cols = sum_amplitude.cols;
    
    ps_mask = cv::Mat::zeros(rows, cols, CV_8UC1);
    amplitude_dispersion = cv::Mat::zeros(rows, cols, CV_32FC1);

    std::atomic<int> completed_rows(0);
    std::atomic<bool> cancel_flag(false);
    int step = std::max(1, rows / 100);

    #pragma omp parallel for schedule(dynamic)
    for (int r = 0; r < rows; ++r) {
        if (cancel_flag) continue;
        for (int c = 0; c < cols; ++c) {
            float sum_val = sum_amplitude.at<float>(r, c);
            float sum_sq_val = sum_amplitude_sq.at<float>(r, c);
            
            float mean_A = sum_val / num_images;
            float mean_A2 = sum_sq_val / num_images;
            float amplitude_std = std::sqrt(std::max(0.0f, mean_A2 - mean_A * mean_A));
            
            if (mean_A > 1e-6) {
                float DA = amplitude_std / mean_A;
                amplitude_dispersion.at<float>(r, c) = DA;
                if (DA < da_threshold) {
                    ps_mask.at<uchar>(r, c) = 1;
                }
            } else {
                amplitude_dispersion.at<float>(r, c) = 999.0f;
            }
        }

        int current = ++completed_rows;
        if (cb && current % step == 0) {
            if (!cb(current * 100 / rows, "Computing PS candidates...")) {
                cancel_flag = true;
            }
        }
    }
    if (cancel_flag) return -2;
    return 0;
}

// ==================== 步骤2: 三角网构建（高效查表优化） ====================
int PSI::build_ps_network(
    const cv::Mat& ps_mask,
    std::vector<PS_Point>& ps_points,
    std::vector<PS_Edge>& edges,
    double max_edge_length
) {
    if (ps_mask.empty() || ps_mask.type() != CV_8UC1) return -1;

    int rows = ps_mask.rows;
    int cols = ps_mask.cols;
    int index = 0;

    // 1. 建立位置到索引的快速查找表，并将查找表初始化为 -1
    cv::Mat ps_indices = cv::Mat::ones(rows, cols, CV_32SC1) * -1;

    // 2. 收集 PS 点行列号
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (ps_mask.at<uchar>(r, c) == 1) {
                ps_indices.at<int>(r, c) = index;
                ps_points.push_back(PS_Point(r, c, index++));
            }
        }
    }

    if (ps_points.empty()) return -1;

    // 3. 利用 OpenCV Subdiv2D 执行 Delaunay 三角剖分
    cv::Rect rect(0, 0, cols, rows);
    cv::Subdiv2D subdiv(rect);
    for (const auto& pt : ps_points) {
        subdiv.insert(cv::Point2f(static_cast<float>(pt.col), static_cast<float>(pt.row)));
    }

    std::vector<cv::Vec4f> edgeList;
    subdiv.getEdgeList(edgeList);

    // 4. 构建稀疏网络边集
    int edge_num = 0;
    for (const auto& e : edgeList) {
        cv::Point2f p1(e[0], e[1]);
        cv::Point2f p2(e[2], e[3]);

        if (!rect.contains(p1) || !rect.contains(p2)) continue;

        double dist = cv::norm(p1 - p2);
        if (dist > max_edge_length) continue;

        // 利用快速查找表实现 O(1) 端点检索
        int col1 = cvRound(p1.x);
        int row1 = cvRound(p1.y);
        int col2 = cvRound(p2.x);
        int row2 = cvRound(p2.y);

        if (row1 < 0 || row1 >= rows || col1 < 0 || col1 >= cols) continue;
        if (row2 < 0 || row2 >= rows || col2 < 0 || col2 >= cols) continue;

        int idx1 = ps_indices.at<int>(row1, col1);
        int idx2 = ps_indices.at<int>(row2, col2);

        if (idx1 == -1 || idx2 == -1 || idx1 == idx2) continue;

        // 过滤重复边与无向图反向边，仅保留 idx1 < idx2 的有向代表
        if (idx1 >= idx2) continue;

        PS_Edge edge;
        edge.num = edge_num++;
        edge.end1 = idx1;
        edge.end2 = idx2;
        edge.distance = dist;
        edge.weight = 1.0 / (dist + 1.0);
        edge.is_boundary = false;
        
        edges.push_back(edge);

        // 更新点-边邻接关系
        ps_points[idx1].neigh_edges.push_back(edge.num);
        ps_points[idx2].neigh_edges.push_back(edge.num);
    }
    return 0;
}

// ==================== 步骤3: 时序相位差计算 ====================
int PSI::compute_ps_phase_diff(
    const std::vector<PS_Point>& ps_points,
    const std::vector<PS_Edge>& edges,
    const cv::Mat& ps_slc_data,
    const cv::Mat& formation_matrix,
    cv::Mat& edge_phase_diff,
    PSIProgressCallback cb
) {
    if (ps_slc_data.empty() || formation_matrix.empty()) return -1;
    if (ps_slc_data.type() != CV_32FC2 || formation_matrix.type() != CV_32SC1) return -1;

    int num_edges = static_cast<int>(edges.size());
    int num_ifg = formation_matrix.rows;
    int num_images = ps_slc_data.cols;

    edge_phase_diff = cv::Mat::zeros(num_edges, num_ifg, CV_32FC1);

    // 1. 内存中将所有复数值预先转为相位
    int ps_count = static_cast<int>(ps_points.size());
    cv::Mat ps_phases = cv::Mat::zeros(ps_count, num_images, CV_64FC1);

    std::atomic<bool> cancel_flag(false);

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < ps_count; ++i) {
        if (cancel_flag) continue;
        for (int k = 0; k < num_images; ++k) {
            cv::Vec2f complex_val = ps_slc_data.at<cv::Vec2f>(i, k);
            ps_phases.at<double>(i, k) = std::atan2(complex_val[1], complex_val[0]);
        }
    }

    std::atomic<int> completed_edges(0);
    int step = std::max(1, num_edges / 100);

    // 2. 边相位差计算
    #pragma omp parallel for schedule(dynamic)
    for (int e = 0; e < num_edges; ++e) {
        if (cancel_flag) continue;
        int idx1 = edges[e].end1;
        int idx2 = edges[e].end2;

        for (int m = 0; m < num_ifg; ++m) {
            int im1 = formation_matrix.at<int>(m, 0); // 主影像索引
            int im2 = formation_matrix.at<int>(m, 1); // 辅影像索引

            double p1 = ps_phases.at<double>(idx1, im1) - ps_phases.at<double>(idx1, im2);
            double p2 = ps_phases.at<double>(idx2, im1) - ps_phases.at<double>(idx2, im2);
            
            // 包裹两端相位差
            double diff = p1 - p2;
            edge_phase_diff.at<float>(e, m) = static_cast<float>(std::atan2(std::sin(diff), std::cos(diff)));
        }

        int current = ++completed_edges;
        if (cb && current % step == 0) {
            if (!cb(current * 100 / num_edges, "Computing PS edge phase differences...")) {
                cancel_flag = true;
            }
        }
    }
    if (cancel_flag) return -2;
    return 0;
}

// ==================== 步骤4: 周期图估计与积分反演 ====================
int PSI::ps_time_series_inversion(
    const std::vector<PS_Edge>& edges,
    const std::vector<PS_Point>& ps_points,
    const cv::Mat& edge_phase_diff,
    const cv::Mat& formation_matrix,
    const cv::Mat& temporal_baseline,
    const cv::Mat& spatial_baseline,
    int num_images,
    int ref_index,
    double lambda,
    double range,
    double theta,
    cv::Mat& deformation_time_series,
    cv::Mat& deformation_velocity,
    cv::Mat& temporal_coherence,
    cv::Mat& topographic_residual,
    PSIProgressCallback cb
) {
    int num_edges = static_cast<int>(edges.size());
    int ps_count = static_cast<int>(ps_points.size());
    int num_ifg = formation_matrix.rows;

    if (ref_index < 0 || ref_index >= ps_count) return -1;

    // 分配输出存储
    deformation_time_series = cv::Mat::zeros(ps_count, num_images, CV_64FC1);
    deformation_velocity = cv::Mat::zeros(ps_count, 1, CV_64FC1);
    temporal_coherence = cv::Mat::zeros(ps_count, 1, CV_64FC1);
    topographic_residual = cv::Mat::zeros(ps_count, 1, CV_64FC1);

    // 辅助 lambda 函数：安全读取基线值，支持 float 和 double 类型及行列向量形状
    auto get_baseline_val = [](const cv::Mat& m_val, int idx) -> double {
        int type = m_val.type();
        if (type == CV_32FC1) {
            return m_val.rows > m_val.cols ? m_val.at<float>(idx, 0) : m_val.at<float>(0, idx);
        } else if (type == CV_64FC1) {
            return m_val.rows > m_val.cols ? m_val.at<double>(idx, 0) : m_val.at<double>(0, idx);
        }
        return 0.0;
    };

    // 1. 构建搜索网格候选参数
    // 速率差 v_diff [-50, 50] mm/yr -> 步长 1 mm/yr
    // 地形残差差值 h_diff [-100, 100] m -> 步长 2 m
    std::vector<double> grid_v;
    std::vector<double> grid_h;
    for (int vi = -50; vi <= 50; vi += 1) {
        grid_v.push_back(vi * 0.001); // m/yr
    }
    for (int hi = -100; hi <= 100; hi += 2) {
        grid_h.push_back(static_cast<double>(hi));
    }
    int num_v = static_cast<int>(grid_v.size());
    int num_h = static_cast<int>(grid_h.size());
    int num_grid = num_v * num_h;

    // 2. 预计算模型相位项 W(g, m)
    std::vector<std::vector<std::complex<double>>> W(num_grid, std::vector<std::complex<double>>(num_ifg));
    std::atomic<bool> cancel_flag(false);

    #pragma omp parallel for schedule(static)
    for (int g = 0; g < num_grid; ++g) {
        if (cancel_flag) continue;
        int vi = g / num_h;
        int hi = g % num_h;
        double v = grid_v[vi];
        double h = grid_h[hi];

        for (int m = 0; m < num_ifg; ++m) {
            double b_temp = get_baseline_val(temporal_baseline, m) / 365.25; // 年
            double b_perp = get_baseline_val(spatial_baseline, m);

            // 理论相位值
            double model_phase = (4.0 * PI / lambda) * b_temp * v + 
                                 (4.0 * PI * b_perp / (lambda * range * std::sin(theta))) * h;
            
            // 取共轭以便进行复数点乘：exp(j * obs) * exp(-j * model)
            W[g][m] = std::complex<double>(std::cos(-model_phase), std::sin(-model_phase));
        }
    }

    // 3. 预计算边观测相位项 O(e, m)
    std::vector<std::vector<std::complex<double>>> O(num_edges, std::vector<std::complex<double>>(num_ifg));
    #pragma omp parallel for schedule(static)
    for (int e = 0; e < num_edges; ++e) {
        if (cancel_flag) continue;
        for (int m = 0; m < num_ifg; ++m) {
            double obs_phase = edge_phase_diff.at<float>(e, m);
            O[e][m] = std::complex<double>(std::cos(obs_phase), std::sin(obs_phase));
        }
    }

    // 4. 针对每条边做周期图网格搜索算法（复数点乘快速优化）
    cv::Mat edge_v_diff = cv::Mat::zeros(num_edges, 1, CV_64FC1);
    cv::Mat edge_h_diff = cv::Mat::zeros(num_edges, 1, CV_64FC1);
    cv::Mat edge_coh = cv::Mat::zeros(num_edges, 1, CV_64FC1);

    std::atomic<int> completed_edges(0);
    int step = std::max(1, num_edges / 100);

    #pragma omp parallel for schedule(dynamic)
    for (int e = 0; e < num_edges; ++e) {
        if (cancel_flag) continue;
        double best_v = 0.0;
        double best_h = 0.0;
        double max_gamma = -1.0;

        for (int g = 0; g < num_grid; ++g) {
            std::complex<double> sum_phase(0.0, 0.0);
            for (int m = 0; m < num_ifg; ++m) {
                sum_phase += O[e][m] * W[g][m];
            }
            
            double gamma = std::abs(sum_phase) / num_ifg;
            if (gamma > max_gamma) {
                max_gamma = gamma;
                best_v = grid_v[g / num_h];
                best_h = grid_h[g % num_h];
            }
        }
        edge_v_diff.at<double>(e) = best_v;
        edge_h_diff.at<double>(e) = best_h;
        edge_coh.at<double>(e) = max_gamma;

        int current = ++completed_edges;
        if (cb && current % step == 0) {
            if (!cb(current * 100 / num_edges, "Running periodogram grid search on edges...")) {
                cancel_flag = true;
            }
        }
    }
    if (cancel_flag) return -2;

    // 5. 空间积分（BFS 积分方法）
    // 以传入的参考点 ref_index 为积分起点，并修正正负号 Bug
    std::vector<double> v_results(ps_count, 0.0);
    std::vector<double> h_results(ps_count, 0.0);
    std::vector<bool> visited(ps_count, false);
    std::queue<int> q;

    q.push(ref_index);
    visited[ref_index] = true;

    while (!q.empty()) {
        int u = q.front();
        q.pop();

        for (int edge_idx : ps_points[u].neigh_edges) {
            const auto& edge = edges[edge_idx];
            if (edge_coh.at<double>(edge.num) < 0.65) continue; // 过滤低质量相干边

            int v_node = (edge.end1 == u) ? edge.end2 : edge.end1;
            if (!visited[v_node]) {
                // 修正积分方向的正负号符号
                // 观测值 = end1 - end2 => end2 = end1 - 观测值,  end1 = end2 + 观测值
                double sign = (edge.end1 == u) ? -1.0 : 1.0;
                
                v_results[v_node] = v_results[u] + sign * edge_v_diff.at<double>(edge.num);
                h_results[v_node] = h_results[u] + sign * edge_h_diff.at<double>(edge.num);
                
                visited[v_node] = true;
                q.push(v_node);
            }
        }
    }

    // 6. 输出形变速率与时序赋值
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < ps_count; ++i) {
        if (cancel_flag) continue;
        deformation_velocity.at<double>(i) = v_results[i];
        topographic_residual.at<double>(i) = h_results[i];
        
        // 基于 BFS 连通性设定模拟相干性。
        // （真实 PSI 会在时序相位解缠后基于整体拟合残差重新计算时间相干性）
        temporal_coherence.at<double>(i) = visited[i] ? 0.85 : 0.2; 

        for (int k = 0; k < num_images; ++k) {
            // 计算自首景以来的时间差（年）并得到线性形变序列
            double t = get_baseline_val(temporal_baseline, k) / 365.25; 
            deformation_time_series.at<double>(i, k) = v_results[i] * t;
        }
    }

    return 0;
}

// ==================== 步骤5: 筛选稀疏点并压缩输出 ====================
int PSI::filter_ps_results(
    const std::vector<PS_Point>& ps_points,
    const cv::Mat& temporal_coherence,
    const cv::Mat& deformation_velocity,
    const cv::Mat& deformation_time_series,
    const cv::Mat& topographic_residual,
    double coherence_threshold,
    int rows, int cols,
    cv::Mat& final_ps_mask,
    cv::Mat& mask_count_map,
    cv::Mat& filtered_velocity,
    cv::Mat& filtered_coherence,
    cv::Mat& filtered_topographic_residual,
    cv::Mat& filtered_deformation_time_series
) {
    final_ps_mask = cv::Mat::zeros(rows, cols, CV_8UC1);
    mask_count_map = cv::Mat::ones(rows, cols, CV_32SC1) * -1; // -1 代表非 PS 点

    int old_ps_count = static_cast<int>(ps_points.size());
    int num_images = deformation_time_series.cols;
    
    std::vector<int> keep_indices;
    keep_indices.reserve(old_ps_count);
    
    int new_index = 0;
    for (int i = 0; i < old_ps_count; ++i) {
        double coh = temporal_coherence.at<double>(i);
        if (coh >= coherence_threshold) {
            int r = ps_points[i].row;
            int c = ps_points[i].col;
            
            final_ps_mask.at<uchar>(r, c) = 1;
            mask_count_map.at<int>(r, c) = new_index++;
            keep_indices.push_back(i);
        }
    }

    int new_ps_count = static_cast<int>(keep_indices.size());
    filtered_velocity = cv::Mat(new_ps_count, 1, CV_64FC1);
    filtered_coherence = cv::Mat(new_ps_count, 1, CV_64FC1);
    filtered_topographic_residual = cv::Mat(new_ps_count, 1, CV_64FC1);
    filtered_deformation_time_series = cv::Mat(new_ps_count, num_images, CV_64FC1);

    for (int i = 0; i < new_ps_count; ++i) {
        int orig_idx = keep_indices[i];
        filtered_velocity.at<double>(i) = deformation_velocity.at<double>(orig_idx);
        filtered_coherence.at<double>(i) = temporal_coherence.at<double>(orig_idx);
        filtered_topographic_residual.at<double>(i) = topographic_residual.at<double>(orig_idx);
        
        for (int k = 0; k < num_images; ++k) {
            filtered_deformation_time_series.at<double>(i, k) = deformation_time_series.at<double>(orig_idx, k);
        }
    }

    return 0;
}
