#include "stdafx.h"
#include "../include/AtmosphericCorrection.h"

// 大气折射率常数 (Bevis et al., 1994)
static const double K1 = 77.6;   // K/hPa
static const double K2 = 71.6;   // K/hPa
static const double K3 = 3.747e5; // K^2/hPa
static const double Rd = 287.05;  // 干空气比气体常数 J/(kg·K)
static const double g0 = 9.80665; // 标准重力加速度 m/s^2

// 辅助函数：不区分大小写地检查子串
static bool containsIgnoreCase(const std::string& str, const std::string& sub) {
    auto it = std::search(
        str.begin(), str.end(),
        sub.begin(), sub.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return it != str.end();
}

// 辅助函数：读取 NetCDF 子数据集变量
static cv::Mat readNcVar(const std::string& subName, double adfGeoTransform[6], int& era5_rows, int& era5_cols) {
    GDALDataset* ds = (GDALDataset*)GDALOpen(subName.c_str(), GA_ReadOnly);
    if (!ds) return cv::Mat();
    ds->GetGeoTransform(adfGeoTransform);
    GDALRasterBand* band = ds->GetRasterBand(1);
    era5_cols = band->GetXSize();
    era5_rows = band->GetYSize();
    cv::Mat data(era5_rows, era5_cols, CV_32FC1);
    band->RasterIO(GF_Read, 0, 0, era5_cols, era5_rows, data.data, era5_cols, era5_rows, GDT_Float32, 0, 0);
    GDALClose(ds);
    return data;
}

// 辅助函数：加载 ERA5 的气压、比湿、温度数据集
static bool loadEra5Variables(const std::string& ncPath, cv::Mat& temp_field, cv::Mat& q_field, cv::Mat& sp_field,
                              double adfGeoTransform[6], int& era5_rows, int& era5_cols)
{
    GDALDataset* ncDataset = (GDALDataset*)GDALOpen(ncPath.c_str(), GA_ReadOnly);
    if (!ncDataset) return false;

    char** subdatasets = ncDataset->GetMetadata("SUBDATASETS");
    int nSubdatasets = 0;
    if (subdatasets) {
        while (subdatasets[nSubdatasets * 2] != nullptr) nSubdatasets++;
    }

    std::string tempSub, qSub, spSub;
    for (int i = 0; i < nSubdatasets; i++) {
        std::string key = subdatasets[i * 2];
        std::string val = subdatasets[i * 2 + 1];
        
        if (containsIgnoreCase(key, "_NAME")) {
            if (containsIgnoreCase(val, ":t") || containsIgnoreCase(val, ":temperature")) {
                if (!containsIgnoreCase(val, "time")) {
                    tempSub = val;
                }
            }
            if (containsIgnoreCase(val, ":q") || containsIgnoreCase(val, ":specific_humidity")) {
                if (!containsIgnoreCase(val, "time")) {
                    qSub = val;
                }
            }
            if (containsIgnoreCase(val, ":sp") || containsIgnoreCase(val, ":surface_pressure")) {
                spSub = val;
            }
        }
    }

    GDALClose(ncDataset);

    if (tempSub.empty() || qSub.empty() || spSub.empty()) {
        return false;
    }

    temp_field = readNcVar(tempSub, adfGeoTransform, era5_rows, era5_cols);
    q_field = readNcVar(qSub, adfGeoTransform, era5_rows, era5_cols);
    sp_field = readNcVar(spSub, adfGeoTransform, era5_rows, era5_cols);

    return !temp_field.empty() && !q_field.empty() && !sp_field.empty();
}

// 辅助函数：经纬度坐标与像元坐标转换
static bool mapGeoToPixel(double lat, double lon, const double gt[6], int cols, int rows, int& px, int& py)
{
    if (gt[1] == 0 || gt[5] == 0) return false;
    double target_lon = lon;
    if (gt[0] > 180 && lon < 0) {
        target_lon += 360.0;
    } else if (gt[0] < 0 && lon > 180) {
        target_lon -= 360.0;
    }
    px = static_cast<int>(std::round((target_lon - gt[0]) / gt[1]));
    py = static_cast<int>(std::round((lat - gt[3]) / gt[5]));
    if (px >= 0 && px < cols && py >= 0 && py < rows) {
        return true;
    }
    return false;
}

// 辅助函数：计算天顶延迟 ZTD (ZHD + ZWD)
static float computeZtd(float T, float q, float sp, float h, float lat)
{
    if (std::isnan(T) || std::isnan(q) || std::isnan(sp) || T < 100.0f) return 0.0f;
    float sp_hpa = sp / 100.0f;
    float e = q * sp_hpa / (0.622f + 0.378f * q);
    float lat_rad = static_cast<float>(lat * M_PI / 180.0f);
    float zhd = 0.0022768f * sp_hpa / (1.0f - 0.00266f * std::cos(2.0f * lat_rad) - 0.00028f * h / 1000.0f);
    float zwd = 0.0022768f * (1255.0f / T + 0.05f) * e;
    return (zhd + zwd) / 1000.0f; // 从毫米转换为米
}

bool computeTroposphericCorrection(
    const cv::Mat& phase,
    const cv::Mat& latMat,
    const cv::Mat& lonMat,
    const cv::Mat& demMat,
    bool hasDem,
    const char* masterNcPath,
    const char* slaveNcPath,
    double wavelength,
    cv::Mat& correctedPhase,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb
) {
    if (phase.empty() || latMat.empty() || lonMat.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输入矩阵 (phase, lat, lon) 不能为空。");
        }
        return false;
    }
    if (correctedPhase.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输出矩阵 correctedPhase 必须由调用者预先分配。");
        }
        return false;
    }
    if (correctedPhase.size() != phase.size() || correctedPhase.type() != phase.type()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输出矩阵的大小或类型与输入不匹配。");
        }
        return false;
    }
    if (wavelength <= 0.0) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "雷达波长必须为正数。");
        }
        return false;
    }
    if (!masterNcPath || !slaveNcPath) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "主从图像日期 ERA5 NC 文件路径不能为空。");
        }
        return false;
    }

    if (cb) cb(10, "正在加载主影像 ERA5 NetCDF 参数...");
    cv::Mat master_T, master_q, master_sp;
    double master_gt[6] = {0};
    int master_rows = 0, master_cols = 0;
    if (!loadEra5Variables(masterNcPath, master_T, master_q, master_sp, master_gt, master_rows, master_cols)) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "加载主图像 ERA5 气象数据失败: %s", masterNcPath);
        }
        return false;
    }

    if (cb) cb(30, "正在加载从影像 ERA5 NetCDF 参数...");
    cv::Mat slave_T, slave_q, slave_sp;
    double slave_gt[6] = {0};
    int slave_rows = 0, slave_cols = 0;
    if (!loadEra5Variables(slaveNcPath, slave_T, slave_q, slave_sp, slave_gt, slave_rows, slave_cols)) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "加载从图像 ERA5 气象数据失败: %s", slaveNcPath);
        }
        return false;
    }

    if (cb) cb(50, "正在计算对流层绝对天顶延迟差分值...");
    int rows = phase.rows;
    int cols = phase.cols;

    cv::Mat tropo_delay(rows, cols, CV_32FC1, cv::Scalar(0));

    #pragma omp parallel for
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float ph = phase.at<float>(r, c);
            if (std::isnan(ph) || std::isinf(ph)) continue;

            float lat = latMat.at<float>(r, c);
            float lon = lonMat.at<float>(r, c);

            int px_m = 0, py_m = 0;
            int px_s = 0, py_s = 0;

            bool map_m = mapGeoToPixel(lat, lon, master_gt, master_cols, master_rows, px_m, py_m);
            bool map_s = mapGeoToPixel(lat, lon, slave_gt, slave_cols, slave_rows, px_s, py_s);

            if (!map_m || !map_s) continue;

            float T_m = master_T.at<float>(py_m, px_m);
            float q_m = master_q.at<float>(py_m, px_m);
            float sp_m = master_sp.at<float>(py_m, px_m);

            float T_s = slave_T.at<float>(py_s, px_s);
            float q_s = slave_q.at<float>(py_s, px_s);
            float sp_s = slave_sp.at<float>(py_s, px_s);

            float h = 0.0f;
            if (hasDem && !demMat.empty() && !std::isnan(demMat.at<float>(r, c))) {
                h = demMat.at<float>(r, c);
            }

            float ztd_m = computeZtd(T_m, q_m, sp_m, h, lat);
            float ztd_s = computeZtd(T_s, q_s, sp_s, h, lat);

            tropo_delay.at<float>(r, c) = ztd_m - ztd_s;
        }
    }

    if (cb) cb(80, "正在生成对流层相位改正屏并改写相位值...");
    double factor = -4.0 * M_PI / wavelength;

    #pragma omp parallel for
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float ph = phase.at<float>(r, c);
            float delay = tropo_delay.at<float>(r, c);
            if (std::isnan(ph) || std::isinf(ph)) {
                correctedPhase.at<float>(r, c) = ph;
            } else {
                float phase_correction = static_cast<float>(delay * factor);
                correctedPhase.at<float>(r, c) = ph - phase_correction;
            }
        }
    }

    if (cb) cb(100, "对流层校正计算完成。");
    return true;
}
