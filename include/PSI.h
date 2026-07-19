#pragma once

#ifdef PSI_EXPORTS
#define PSI_API __declspec(dllexport)
#else
#define PSI_API __declspec(dllimport)
#endif

#include "..\include\Package.h"
#include "..\include\Cancellation.h"
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>

// ===== PS 点数据结构 =====
class PSI_API PS_Point {
public:
    PS_Point() : row(0), col(0), index(-1), amplitude_mean(0.0), amplitude_std(0.0),
                 dispersion(0.0), phase(0.0), is_unwrapped(false),
                 deformation_vel(0.0), temporal_coherence(0.0) {}
                 
    PS_Point(int r, int c, int idx) : row(r), col(c), index(idx), amplitude_mean(0.0), 
                                     amplitude_std(0.0), dispersion(0.0), phase(0.0), 
                                     is_unwrapped(false), deformation_vel(0.0), 
                                     temporal_coherence(0.0) {}

    int row;                      // 像素行坐标
    int col;                      // 像素列坐标
    int index;                    // 在稀疏 PS 列表中的索引
    double amplitude_mean;        // 时序振幅均值
    double amplitude_std;         // 时序振幅标准差
    double dispersion;            // 振幅离差指数 DA
    double phase;                 // 单景包裹相位值
    bool is_unwrapped;            // 是否已解缠
    double deformation_vel;       // 解算形变速率 (m/yr)
    double temporal_coherence;    // 时间相干性值
    std::vector<int> neigh_edges; // 邻接边的编号列表
};

// ===== PS 网络边数据结构 =====
struct PSI_API PS_Edge {
    int num;                      // 边编号
    int end1;                     // 端点1在 ps_points 里的索引
    int end2;                     // 端点2在 ps_points 里的索引
    double distance;              // 两点间空间距离（像素）
    double weight;                // 边权重
    bool is_boundary;             // 是否为 Delaunay 边界边
    std::vector<double> phase_diff; // 各景干涉图上的包裹相位差 [num_ifg]
};

// ===== PSI 进度回调函数指针类型 =====

// ===== PSI 主算法类 =====
class PSI_API PSI {
public:
    PSI();
    ~PSI();

    std::string error_head;

    /**
     * @brief 步骤1: PS 候选点振幅离差指数计算（纯内存计算）
     * @param sum_amplitude       输入：时序振幅累加和 [rows x cols] (float)
     * @param sum_amplitude_sq    输入：时序振幅平方累加和 [rows x cols] (float)
     * @param num_images          输入：影像数量
     * @param da_threshold        输入：离差阈值（如0.4）
     * @param ps_mask             输出：PS 候选点掩膜 [rows x cols] (uchar, 1=候选)
     * @param amplitude_dispersion 输出：振幅离差分布图 [rows x cols] (float)
     */
    int compute_ps_candidates(
        const cv::Mat& sum_amplitude,
        const cv::Mat& sum_amplitude_sq,
        int num_images,
        double da_threshold,
        cv::Mat& ps_mask,
        cv::Mat& amplitude_dispersion,
        IsCancelledCallback is_cancelled = nullptr,
        void* cancel_context = nullptr,
        InSARProgressCallback progress = nullptr,
        void* progress_context = nullptr
    );

    /**
     * @brief 步骤2: Delaunay 三角网构建（高效查表优化）
     * @param ps_mask             输入：PS 点掩膜
     * @param ps_points           输出：PS 点数组
     * @param edges               输出：PS 边数组
     * @param max_edge_length     输入：最大边长约束（像素）
     */
    int build_ps_network(
        const cv::Mat& ps_mask,
        std::vector<PS_Point>& ps_points,
        std::vector<PS_Edge>& edges,
        double max_edge_length = 1000.0,
        IsCancelledCallback is_cancelled = nullptr,
        void* cancel_context = nullptr,
        InSARProgressCallback progress = nullptr,
        void* progress_context = nullptr
    );

    /**
     * @brief 步骤3: 边相位差时序计算
     * @param ps_points           输入：PS 点数组
     * @param edges               输入：PS 边数组
     * @param ps_slc_data         输入：时序 SLC 值 [ps_count x num_images] (CV_32FC2)
     * @param formation_matrix    输入：干涉组合矩阵 [num_ifg x 2] (CV_32SC1)
     * @param edge_phase_diff     输出：边时序相位差矩阵 [num_edges x num_ifg] (float)
     */
    int compute_ps_phase_diff(
        const std::vector<PS_Point>& ps_points,
        const std::vector<PS_Edge>& edges,
        const cv::Mat& ps_slc_data,
        const cv::Mat& formation_matrix,
        cv::Mat& edge_phase_diff,
        IsCancelledCallback is_cancelled = nullptr,
        void* cancel_context = nullptr,
        InSARProgressCallback progress = nullptr,
        void* progress_context = nullptr
    );

    /**
     * @brief 步骤4: 周期图估计（参数反演）与空间网格积分
     * @param edges                    输入：边数组
     * @param ps_points                输入：PS 点数组
     * @param edge_phase_diff          输入：边时序相位差 [num_edges x num_ifg] (float)
     * @param formation_matrix         输入：干涉组合矩阵 [num_ifg x 2] (CV_32SC1)
     * @param temporal_baseline        输入：时间基线（天） [num_ifg] (float 或 double)
     * @param spatial_baseline         输入：垂直空间基线（米） [num_ifg] (float 或 double)
     * @param num_images               输入：影像总数
     * @param ref_index                输入：参考点索引
     * @param lambda                   输入：雷达波长 (m)
     * @param range                    输入：斜距 (m)
     * @param theta                    输入：入射角 (rad)
     * @param deformation_time_series  输出：累积形变序列 [ps_count x num_images] (double)
     * @param deformation_velocity     输出：形变速率 [ps_count x 1] (double)
     * @param temporal_coherence       输出：时间相干性 [ps_count x 1] (double)
     * @param topographic_residual     输出：地形残差 [ps_count x 1] (double)
     */
    int ps_time_series_inversion(
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
        IsCancelledCallback is_cancelled = nullptr,
        void* cancel_context = nullptr,
        InSARProgressCallback progress = nullptr,
        void* progress_context = nullptr
    );

    /**
     * @brief 步骤5: 基于相干性的稀疏点属性过滤提取
     */
    int filter_ps_results(
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
        cv::Mat& filtered_deformation_time_series,
        IsCancelledCallback is_cancelled = nullptr,
        void* cancel_context = nullptr,
        InSARProgressCallback progress = nullptr,
        void* progress_context = nullptr
    );
};
