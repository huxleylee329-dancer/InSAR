#include "stdafx.h"
#include "../include/AtmosphericCorrection.h"
#include "kiss_fft/kiss_fft.h"

// 辅助函数：一维距离向 FFT
static void rangeFFT(const cv::Mat& complexIn, cv::Mat& complexOut, bool forward) {
    int rows = complexIn.rows;
    int cols = complexIn.cols;
    int nfft = cols;

    kiss_fft_cfg cfg = kiss_fft_alloc(nfft, forward ? 0 : 1, nullptr, nullptr);
    if (!cfg) return;

    complexOut = cv::Mat::zeros(rows, cols, CV_32FC2);

    std::vector<kiss_fft_cpx> inBuf(nfft);
    std::vector<kiss_fft_cpx> outBuf(nfft);

    for (int r = 0; r < rows; r++) {
        // 填充输入缓冲区
        for (int c = 0; c < cols; c++) {
            cv::Vec2f val = complexIn.at<cv::Vec2f>(r, c);
            inBuf[c].r = val[0];
            inBuf[c].i = val[1];
        }

        // 执行 FFT / IFFT
        kiss_fft(cfg, inBuf.data(), outBuf.data());

        // 写回结果
        for (int c = 0; c < cols; c++) {
            complexOut.at<cv::Vec2f>(r, c) = cv::Vec2f(outBuf[c].r, outBuf[c].i);
        }
    }

    kiss_fft_free(cfg);
}

bool computeIonosphericCorrection(
    const cv::Mat& masterComplex,
    const cv::Mat& slaveComplex,
    const IonosphericParams& params,
    cv::Mat& correctedSlaveComplex,
    char* errBuf,
    int bufSize,
    AtmosProgressCallback cb
) {
    if (masterComplex.empty() || slaveComplex.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输入的主从影像复数矩阵不能为空。");
        }
        return false;
    }
    if (correctedSlaveComplex.empty()) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输出的 correctedSlaveComplex 矩阵必须由调用者预先分配。");
        }
        return false;
    }
    if (correctedSlaveComplex.size() != slaveComplex.size() || correctedSlaveComplex.type() != CV_32FC2) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "输出的 correctedSlaveComplex 尺寸或类型（必须是 CV_32FC2）不匹配。");
        }
        return false;
    }
    if (params.subbandRatio <= 0.0 || params.subbandRatio >= 1.0) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "子频带比例参数 subbandRatio 必须在 (0, 1) 区间内。");
        }
        return false;
    }

    int rows = masterComplex.rows;
    int cols = masterComplex.cols;

    // 辅助 Lambda：确保将输入矩阵转换为双通道复数 (CV_32FC2)
    auto toComplex2Ch = [](const cv::Mat& src, cv::Mat& dst, int r, int c) -> bool {
        if (src.channels() == 1) {
            std::vector<cv::Mat> channels = {src, cv::Mat::zeros(r, c, CV_32FC1)};
            cv::merge(channels, dst);
        } else if (src.channels() == 2) {
            if (src.type() == CV_32FC2) {
                dst = src;
            } else {
                src.convertTo(dst, CV_32FC2);
            }
        } else {
            return false;
        }
        return true;
    };

    cv::Mat master_2ch, slave_2ch;
    if (!toComplex2Ch(masterComplex, master_2ch, rows, cols)) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "转换主图像为 CV_32FC2 双通道复数失败。");
        }
        return false;
    }
    if (!toComplex2Ch(slaveComplex, slave_2ch, rows, cols)) {
        if (errBuf && bufSize > 0) {
            snprintf(errBuf, bufSize, "转换从图像为 CV_32FC2 双通道复数失败。");
        }
        return false;
    }

    // === 步骤 1: 沿距离向做 FFT ===
    if (cb) cb(10, "正在执行主图像距离向一维 FFT...");
    cv::Mat master_fft;
    rangeFFT(master_2ch, master_fft, true);

    if (cb) cb(35, "正在执行从图像距离向一维 FFT...");
    cv::Mat slave_fft;
    rangeFFT(slave_2ch, slave_fft, true);

    // === 步骤 2: 分割子频带 ===
    if (cb) cb(50, "正在进行子频带划分与 IFFT 重构...");
    int lowStart = 0;
    int lowEnd = static_cast<int>(cols * (0.5 - params.subbandRatio / 2.0));
    int highStart = static_cast<int>(cols * (0.5 + params.subbandRatio / 2.0));
    int highEnd = cols;

    auto splitSubband = [&](const cv::Mat& fft, cv::Mat& lowSLC, cv::Mat& highSLC) {
        cv::Mat lowBand = cv::Mat::zeros(rows, cols, CV_32FC2);
        cv::Mat highBand = cv::Mat::zeros(rows, cols, CV_32FC2);
        for (int r = 0; r < rows; r++) {
            for (int c = lowStart; c < lowEnd; c++) {
                lowBand.at<cv::Vec2f>(r, c) = fft.at<cv::Vec2f>(r, c);
            }
            for (int c = highStart; c < highEnd; c++) {
                highBand.at<cv::Vec2f>(r, c) = fft.at<cv::Vec2f>(r, c);
            }
        }
        rangeFFT(lowBand, lowSLC, false);
        rangeFFT(highBand, highSLC, false);
    };

    cv::Mat master_lowSLC, master_highSLC;
    splitSubband(master_fft, master_lowSLC, master_highSLC);

    cv::Mat slave_lowSLC, slave_highSLC;
    splitSubband(slave_fft, slave_lowSLC, slave_highSLC);

    // === 步骤 3: 生成子频带相干干涉图并计算相位差 ===
    if (cb) cb(70, "正在生成低频和高频干涉相位图...");
    cv::Mat lowPhase(rows, cols, CV_32FC1);
    cv::Mat highPhase(rows, cols, CV_32FC1);

    #pragma omp parallel for
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            cv::Vec2f lowM = master_lowSLC.at<cv::Vec2f>(r, c);
            cv::Vec2f lowS = slave_lowSLC.at<cv::Vec2f>(r, c);
            cv::Vec2f highM = master_highSLC.at<cv::Vec2f>(r, c);
            cv::Vec2f highS = slave_highSLC.at<cv::Vec2f>(r, c);

            // 共轭相乘 lowM * conj(lowS)
            float realL = lowM[0] * lowS[0] + lowM[1] * lowS[1];
            float imagL = lowM[1] * lowS[0] - lowM[0] * lowS[1];
            lowPhase.at<float>(r, c) = std::atan2(imagL, realL);

            // 共轭相乘 highM * conj(highS)
            float realH = highM[0] * highS[0] + highM[1] * highS[1];
            float imagH = highM[1] * highS[0] - highM[0] * highS[1];
            highPhase.at<float>(r, c) = std::atan2(imagH, realH);
        }
    }

    // === 步骤 4: 求解电离层相位 ===
    if (cb) cb(80, "正在利用频带差分求解电离层原始相位值...");
    double f0 = 1.0; // 归一化中心频率
    double fL = f0 * (1.0 - params.subbandRatio);
    double fH = f0 * (1.0 + params.subbandRatio);
    double det = (fL / f0) * (f0 / fH) - (fH / f0) * (f0 / fL);

    cv::Mat ionoPhase = cv::Mat::zeros(rows, cols, CV_32FC1);
    #pragma omp parallel for
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float phiL = lowPhase.at<float>(r, c);
            float phiH = highPhase.at<float>(r, c);
            ionoPhase.at<float>(r, c) = static_cast<float>(((fL / f0) * phiH - (fH / f0) * phiL) / det);
        }
    }

    // === 步骤 5: 低通高斯平滑 ===
    if (cb) cb(90, "正在对电离层相位进行空间高斯平滑滤波...");
    int kernelSize = static_cast<int>(params.filterStrength * 10);
    if (kernelSize < 3) kernelSize = 3;
    if (kernelSize % 2 == 0) kernelSize++;
    cv::Mat smoothedIono;
    cv::GaussianBlur(ionoPhase, smoothedIono, cv::Size(kernelSize, kernelSize), 0);

    // === 步骤 6: 校正 Slave SLC 的相位 ===
    if (cb) cb(95, "正在将电离层延迟相位反补偿到从影像中...");
    #pragma omp parallel for
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            cv::Vec2f slcVal = slave_2ch.at<cv::Vec2f>(r, c);
            float ionoCorr = smoothedIono.at<float>(r, c);

            float mag = std::sqrt(slcVal[0] * slcVal[0] + slcVal[1] * slcVal[1]);
            float origPhase = std::atan2(slcVal[1], slcVal[0]);
            float newPhase = origPhase + ionoCorr; // 补偿

            correctedSlaveComplex.at<cv::Vec2f>(r, c) = cv::Vec2f(mag * std::cos(newPhase), mag * std::sin(newPhase));
        }
    }

    if (cb) cb(100, "电离层分裂谱校正计算完成。");
    return true;
}
