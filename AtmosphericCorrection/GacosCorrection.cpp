#include "stdafx.h"
#include "../include/AtmosphericCorrection.h"
#include <sstream>

struct RscConfig {
    int width = 0;
    int file_length = 0;
    double x_first = 0.0;
    double y_first = 0.0;
    double x_step = 0.0;
    double y_step = 0.0;
};

// 辅助函数：解析 .ztd.rsc 文本文件
static bool parseRscFile(const std::string& rscPath, RscConfig& config) {
    std::ifstream ifs(rscPath);
    if (!ifs.is_open()) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        // 去除前后空白
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty()) continue;
        
        std::istringstream iss(line);
        std::string key;
        double val = 0.0;
        if (iss >> key >> val) {
            std::transform(key.begin(), key.end(), key.begin(), ::toupper);
            if (key == "WIDTH") config.width = static_cast<int>(val);
            else if (key == "FILE_LENGTH") config.file_length = static_cast<int>(val);
            else if (key == "X_FIRST") config.x_first = val;
            else if (key == "Y_FIRST") config.y_first = val;
            else if (key == "X_STEP") config.x_step = val;
            else if (key == "Y_STEP") config.y_step = val;
        }
    }
    return config.width > 0 && config.file_length > 0;
}

// 辅助函数：经纬度映射到格网像元坐标
static bool mapGeoToGrid(double lat, double lon, const double gt[6], int cols, int rows, double& px, double& py) {
    if (gt[1] == 0.0 || gt[5] == 0.0) return false;
    double target_lon = lon;
    
    // 处理经度跨 180 度界线
    if (gt[0] > 180.0 && lon < 0.0) {
        target_lon += 360.0;
    } else if (gt[0] < 0.0 && lon > 180.0) {
        target_lon -= 360.0;
    }
    
    px = (target_lon - gt[0]) / gt[1];
    py = (lat - gt[3]) / gt[5];
    
    if (px >= 0.0 && px < static_cast<double>(cols) && py >= 0.0 && py < static_cast<double>(rows)) {
        return true;
    }
    return false;
}

// 辅助函数：双线性插值
static float interpolateGrid(const cv::Mat& grid, double x, double y) {
    int cols = grid.cols;
    int rows = grid.rows;
    
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    
    x0 = std::max(0, std::min(x0, cols - 1));
    y0 = std::max(0, std::min(y0, rows - 1));
    int x1 = std::min(x0 + 1, cols - 1);
    int y1 = std::min(y0 + 1, rows - 1);
    
    double dx = x - x0;
    double dy = y - y0;
    
    float v00 = grid.at<float>(y0, x0);
    float v10 = grid.at<float>(y0, x1);
    float v01 = grid.at<float>(y1, x0);
    float v11 = grid.at<float>(y1, x1);
    
    if (std::isnan(v00)) v00 = 0.0f;
    if (std::isnan(v10)) v10 = v00;
    if (std::isnan(v01)) v01 = v00;
    if (std::isnan(v11)) v11 = v01;
    
    double val = (1.0 - dx) * (1.0 - dy) * v00 +
                 dx * (1.0 - dy) * v10 +
                 (1.0 - dx) * dy * v01 +
                 dx * dy * v11;
                 
    return static_cast<float>(val);
}

bool applyGacosCorrection(
    const cv::Mat& phase,
    const cv::Mat& latMat,
    const cv::Mat& lonMat,
    const char* localGacosFilePath,
    int dataFormat,
    double wavelength,
    cv::Mat& correctedPhase,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb
) {
    if (phase.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输入相位矩阵为空。");
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
            snprintf(errBuf, bufSize, "输出与输入矩阵的尺寸或类型不匹配。");
        }
        return false;
    }
    if (wavelength <= 0.0) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "雷达波长必须为正数。");
        }
        return false;
    }
    if (!localGacosFilePath) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "GACOS 延迟数据路径不能为空。");
        }
        return false;
    }

    int rows = phase.rows;
    int cols = phase.cols;

    if (cb) cb(15, "正在读取并解析 GACOS 延迟场文件...");

    cv::Mat ztd_grid;
    double gt[6] = {0};
    bool has_geotransform = false;
    int grid_rows = 0, grid_cols = 0;

    if (dataFormat == 0) {
        // GeoTIFF 格式
        GDALDataset* poDataset = (GDALDataset*)GDALOpen(localGacosFilePath, GA_ReadOnly);
        if (!poDataset) {
            if (errBuf && bufSize > 0) {
                snprintf(errBuf, bufSize, "无法使用 GDAL 打开 GeoTIFF 文件: %s", localGacosFilePath);
            }
            return false;
        }

        GDALRasterBand* poBand = poDataset->GetRasterBand(1);
        grid_rows = poBand->GetYSize();
        grid_cols = poBand->GetXSize();

        ztd_grid.create(grid_rows, grid_cols, CV_32FC1);
        poBand->RasterIO(GF_Read, 0, 0, grid_cols, grid_rows,
                         ztd_grid.data, grid_cols, grid_rows, GDT_Float32, 0, 0);

        if (poDataset->GetGeoTransform(gt) == CE_None) {
            has_geotransform = true;
        }
        GDALClose(poDataset);
    } 
    else {
        // 二进制 .ztd 格式
        std::ifstream ifs(localGacosFilePath, std::ios::binary);
        if (!ifs.is_open()) {
            if (errBuf && bufSize > 0) {
                snprintf(errBuf, bufSize, "无法打开二进制 .ztd 文件: %s", localGacosFilePath);
            }
            return false;
        }

        ifs.seekg(0, std::ios::end);
        std::streampos fileSize = ifs.tellg();
        ifs.seekg(0, std::ios::beg);

        // 尝试解析 .ztd.rsc 配置文件
        std::string rscPath = std::string(localGacosFilePath) + ".rsc";
        RscConfig rsc;
        bool has_rsc = parseRscFile(rscPath, rsc);

        if (has_rsc) {
            grid_cols = rsc.width;
            grid_rows = rsc.file_length;
            
            gt[0] = rsc.x_first;
            gt[1] = rsc.x_step;
            gt[2] = 0.0;
            gt[3] = rsc.y_first;
            gt[4] = 0.0;
            gt[5] = rsc.y_step;
            has_geotransform = true;
        }

        // 检查是否有 8 字节行列整型头
        if (grid_rows <= 0 || grid_cols <= 0) {
            int header_rows = 0, header_cols = 0;
            ifs.read(reinterpret_cast<char*>(&header_rows), sizeof(int));
            ifs.read(reinterpret_cast<char*>(&header_cols), sizeof(int));
            if (fileSize == static_cast<std::streamoff>(header_rows * header_cols * sizeof(float) + 8)) {
                grid_rows = header_rows;
                grid_cols = header_cols;
            } else {
                ifs.seekg(0, std::ios::beg);
            }
        } else {
            // 如果 rsc 正确并且文件包含行列头，跳过前 8 字节
            if (fileSize == static_cast<std::streamoff>(grid_rows * grid_cols * sizeof(float) + 8)) {
                ifs.seekg(8, std::ios::beg);
            }
        }

        if (grid_rows > 0 && grid_cols > 0 && grid_rows < 50000 && grid_cols < 50000) {
            ztd_grid.create(grid_rows, grid_cols, CV_32FC1);
            ifs.read(reinterpret_cast<char*>(ztd_grid.data), grid_rows * grid_cols * sizeof(float));
        } else {
            if (errBuf && bufSize > 0) {
                snprintf(errBuf, bufSize, "无法确定 .ztd 文件尺寸。文件大小: %lld 字节", (long long)fileSize);
            }
            ifs.close();
            return false;
        }
        ifs.close();
    }

    if (ztd_grid.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "读取的 ZTD 格网数据为空。");
        }
        return false;
    }

    if (cb) cb(50, "正在进行经纬度投影匹配与重采样插值...");

    cv::Mat ztd_delay(rows, cols, CV_32FC1, cv::Scalar(0));
    bool use_geo_mapping = has_geotransform && !latMat.empty() && !lonMat.empty();

    if (use_geo_mapping) {
        // 模式 A：使用实际经纬度格网逐像素双线性查找插值 (保证雷达/地理几何对齐)
        #pragma omp parallel for
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                float ph = phase.at<float>(r, c);
                if (std::isnan(ph) || std::isinf(ph)) continue;

                float lat = latMat.at<float>(r, c);
                float lon = lonMat.at<float>(r, c);
                if (std::isnan(lat) || std::isnan(lon)) continue;

                double px = 0.0, py = 0.0;
                if (mapGeoToGrid(lat, lon, gt, grid_cols, grid_rows, px, py)) {
                    ztd_delay.at<float>(r, c) = interpolateGrid(ztd_grid, px, py);
                }
            }
        }
    } 
    else {
        // 模式 B：退化模式，直接双线性缩放 (如经纬度缺失)
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "警告：无法获取经纬度或坐标变换参数，退化为直接 resize。");
        }
        cv::resize(ztd_grid, ztd_delay, cv::Size(cols, rows), 0, 0, cv::INTER_LINEAR);
    }

    if (cb) cb(85, "正在将延迟转换为相位改正屏并应用改正...");

    // Δφ = -4π/λ · ΔZTD
    double factor = -4.0 * M_PI / wavelength;

    #pragma omp parallel for
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float ph = phase.at<float>(r, c);
            if (std::isnan(ph) || std::isinf(ph)) {
                correctedPhase.at<float>(r, c) = ph;
            } else {
                float ztd = ztd_delay.at<float>(r, c);
                float phase_correction = static_cast<float>(ztd * factor);
                correctedPhase.at<float>(r, c) = ph - phase_correction; // 扣除延迟相位
            }
        }
    }

    if (cb) cb(100, "GACOS 延迟校正应用完成。");
    return true;
}
