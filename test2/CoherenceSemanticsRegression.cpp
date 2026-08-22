#include "CoherenceSemanticsRegression.h"

#include <cstdio>
#include <cmath>
#include <limits>
#include <vector>
#include "opencv2\opencv.hpp"
#include "..\include\ComplexMat.h"
#include "..\include\Utils.h"

using cv::Mat;
using cv::Range;
using cv::Scalar;

namespace {

// 判定用的容差
const double kTolLoose = 1e-3;

int g_failed = 0;

void report(const char* name, bool ok, const char* detail)
{
    if (ok) {
        fprintf(stdout, "[PASS] %s\n", name);
    }
    else {
        fprintf(stdout, "[FAIL] %s : %s\n", name, detail);
        ++g_failed;
    }
}

bool nearly(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

// ------------------------------------------------------------------
// 用例 1：判别用例（R1 与 R2 的语义分离）
//
// 3x3 窗口内 4 个 +pi/2、5 个 -pi/2：
//   R1 = |mean(exp(i*phi))| = |4i - 5i| / 9 = 1/9
//   R2 = |mean(exp(i*2*phi))| = |9 * (-1)| / 9 = 1
// 该用例可唯一区分“常规圆统计集中度”与“二倍角轴向集中度”。
//
// 说明：两个被测函数内部窗口estimator对边界的处理不同，故构造
// 一个较大的常量分块矩阵，只检查内部远离边界的像元。
// ------------------------------------------------------------------
int caseDiscriminator()
{
    Utils util;

    // 构造 9x9：按行主序前 4 个元素为 +pi/2，其余为 -pi/2 的 3x3 图案平铺，
    // 保证任意内部 3x3 窗口内恰好 4 个 +pi/2、5 个 -pi/2。
    // 为使窗口统计严格可控，这里直接用一个 3x3 图案在整幅图上重复。
    const int nr = 9, nc = 9;
    Mat phase(nr, nc, CV_64F);
    // 图案：行内 [+,+,-] / [+,+,-] / [-,-,-] —— 3x3 内 4 个 +pi/2、5 个 -pi/2
    const int pat[3][3] = {
        {  1,  1, -1 },
        {  1,  1, -1 },
        { -1, -1, -1 }
    };
    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc; j++) {
            phase.at<double>(i, j) = pat[i % 3][j % 3] * (CV_PI / 2.0);
        }
    }

    Mat r1, r2, r2Named;
    int ret1 = util.phase_circular_concentration(phase, 3, 3, r1);
    int ret2 = util.phase_coherence(phase, 3, 3, r2);
    int ret3 = util.phase_axial_concentration(phase, 3, 3, r2Named);

    if (ret1 != 0) { report("discriminator/phase_circular_concentration returns 0", false, "non-zero return"); return -1; }
    if (ret2 != 0) { report("discriminator/phase_coherence returns 0", false, "non-zero return"); return -1; }
    if (ret3 != 0) { report("discriminator/phase_axial_concentration returns 0", false, "non-zero return"); return -1; }

    char buf[512];

    // 尺寸应与输入一致（两个 estimator 都做了边界补齐）
    sprintf_s(buf, "r1 size=%dx%d r2 size=%dx%d input=%dx%d",
        r1.rows, r1.cols, r2.rows, r2.cols, nr, nc);
    report("discriminator/output size equals input", r1.rows == nr && r1.cols == nc && r2.rows == nr && r2.cols == nc, buf);

    // 打印内部区域，便于人工核对
    fprintf(stdout, "  --- R1 (circular, expect 1/9=0.1111 on aligned windows) ---\n");
    for (int i = 2; i <= 6; i++) {
        fprintf(stdout, "   ");
        for (int j = 2; j <= 6; j++) fprintf(stdout, " %8.5f", r1.at<double>(i, j));
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "  --- R2 (axial, expect 1.0 everywhere) ---\n");
    for (int i = 2; i <= 6; i++) {
        fprintf(stdout, "   ");
        for (int j = 2; j <= 6; j++) fprintf(stdout, " %8.5f", r2.at<double>(i, j));
        fprintf(stdout, "\n");
    }

    // R2 在全图应恒为 1（所有相位均为 +-pi/2，二倍角后同相）
    bool r2AllOne = true;
    double r2Min = 1.0;
    for (int i = 1; i < nr - 1; i++) {
        for (int j = 1; j < nc - 1; j++) {
            const double v = r2.at<double>(i, j);
            if (v < r2Min) r2Min = v;
            if (!nearly(v, 1.0, kTolLoose)) r2AllOne = false;
        }
    }
    sprintf_s(buf, "min R2 on interior = %.6f (expect 1.0)", r2Min);
    report("discriminator/R2 == 1 (axial: phi and phi+pi identical)", r2AllOne, buf);

    // R1 在内部应显著小于 1；对齐窗口上应为 1/9
    double r1Max = 0.0, r1Min = 1.0;
    for (int i = 1; i < nr - 1; i++) {
        for (int j = 1; j < nc - 1; j++) {
            const double v = r1.at<double>(i, j);
            if (v > r1Max) r1Max = v;
            if (v < r1Min) r1Min = v;
        }
    }
    sprintf_s(buf, "R1 interior range = [%.6f, %.6f] (expect max << 1)", r1Min, r1Max);
    report("discriminator/R1 << 1 (circular: cancellation occurs)", r1Max < 0.9, buf);

    // 关键断言：R1 与 R2 必须显著不同，否则说明 R1 实现仍在算二倍角
    sprintf_s(buf, "R1max=%.6f R2min=%.6f — if equal, R1 is still computing 2*phi", r1Max, r2Min);
    report("discriminator/R1 differs from R2", r1Max < r2Min - 0.05, buf);

    // 精确断言：存在像元 R1 == 1/9
    bool foundOneNinth = false;
    for (int i = 1; i < nr - 1 && !foundOneNinth; i++) {
        for (int j = 1; j < nc - 1 && !foundOneNinth; j++) {
            if (nearly(r1.at<double>(i, j), 1.0 / 9.0, kTolLoose)) foundOneNinth = true;
        }
    }
    sprintf_s(buf, "no interior pixel equals 1/9=%.6f; see printed R1 grid above", 1.0 / 9.0);
    report("discriminator/R1 == 1/9 on aligned window", foundOneNinth, buf);

    double maxAliasDifference = 0.0;
    for (int i = 0; i < nr; ++i) {
        for (int j = 0; j < nc; ++j) {
            maxAliasDifference = std::max(maxAliasDifference,
                std::fabs(r2.at<double>(i, j) - r2Named.at<double>(i, j)));
        }
    }
    sprintf_s(buf, "max |phase_coherence - phase_axial_concentration| = %.3g", maxAliasDifference);
    report("discriminator/legacy R2 alias is numerically unchanged", maxAliasDifference <= 1e-12, buf);

    return 0;
}

// ------------------------------------------------------------------
// 用例 2：常数相位下的一致性
// 全图相位为常数时，R1 与 R2 均应为 1。
// 该用例用于排除“R1 实现把 slave 置错导致恒为常数”的情形。
// ------------------------------------------------------------------
int caseConstantPhase()
{
    Utils util;
    const int nr = 16, nc = 16;
    Mat phase(nr, nc, CV_64F, Scalar::all(0.7));

    Mat r1, r2;
    if (util.phase_circular_concentration(phase, 5, 5, r1) != 0) {
        report("constant/phase_circular_concentration returns 0", false, "non-zero return");
        return -1;
    }
    if (util.phase_coherence(phase, 5, 5, r2) != 0) {
        report("constant/phase_coherence returns 0", false, "non-zero return");
        return -1;
    }

    double minR1 = 2.0, minR2 = 2.0;
    for (int i = 2; i < nr - 2; i++) {
        for (int j = 2; j < nc - 2; j++) {
            minR1 = std::min(minR1, r1.at<double>(i, j));
            minR2 = std::min(minR2, r2.at<double>(i, j));
        }
    }
    char buf[256];
    sprintf_s(buf, "min R1 = %.6f (expect 1.0)", minR1);
    report("constant/R1 == 1 for constant phase", nearly(minR1, 1.0, kTolLoose), buf);
    sprintf_s(buf, "min R2 = %.6f (expect 1.0)", minR2);
    report("constant/R2 == 1 for constant phase", nearly(minR2, 1.0, kTolLoose), buf);
    return 0;
}

// ------------------------------------------------------------------
// 用例 3：complex_coherence_demodulated 的符号约定与去调制
//
// 构造一对复 SLC，使 phi = arg(M*conj(S)) 恰为已知空间斜坡：
//   M = exp(i*phi_ramp), S = 1+0i  =>  M*conj(S) = exp(i*phi_ramp)
// 此时：
//   - 传入 reference_phase = phi_ramp        => gamma 应趋近 1（斜坡被抵消）
//   - 调用显式无参考接口                     => gamma 明显小于 1（斜坡造成人为去相干）
//   - 传入 reference_phase = -phi_ramp       => gamma 更低（符号反向，斜坡被加倍）
//
// 第三项是符号约定的判别：若“取负”反而使 gamma 趋近 1，说明
// 实现内部的补偿因子符号与文档约定相反。
// ------------------------------------------------------------------
int caseDemodulation()
{
    Utils util;
    const int nr = 64, nc = 64;

    // 每像元 0.6 rad 的距离向斜坡，窗口内足以造成明显去相干
    Mat ramp(nr, nc, CV_64F);
    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc; j++) {
            ramp.at<double>(i, j) = 0.6 * j + 0.25 * i;
        }
    }

    ComplexMat master, slave;
    Mat mre(nr, nc, CV_64F), mim(nr, nc, CV_64F);
    Mat sre(nr, nc, CV_64F, Scalar::all(1.0)), sim(nr, nc, CV_64F, Scalar::all(0.0));
    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc; j++) {
            mre.at<double>(i, j) = std::cos(ramp.at<double>(i, j));
            mim.at<double>(i, j) = std::sin(ramp.at<double>(i, j));
        }
    }
    master.SetRe(mre); master.SetIm(mim);
    slave.SetRe(sre);  slave.SetIm(sim);

    // 先自检：Utils::Multilook 在 1x1 下应给出 phi == ramp（wrap 后），
    // 以确认本用例构造的符号与 Core 写入 H5 的 phase 定义一致。
    Mat phaseCheck;
    if (util.Multilook(master, slave, 1, 1, phaseCheck) != 0) {
        report("demod/Multilook self-check returns 0", false, "non-zero return");
        return -1;
    }
    Mat wrappedRamp;
    util.wrap(ramp, wrappedRamp);
    double maxDiff = 0.0;
    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc; j++) {
            double d = std::fabs(phaseCheck.at<double>(i, j) - wrappedRamp.at<double>(i, j));
            // 处理 +-pi 边界的等价性
            if (d > CV_PI) d = std::fabs(d - 2.0 * CV_PI);
            maxDiff = std::max(maxDiff, d);
        }
    }
    char buf[512];
    sprintf_s(buf, "max|arg(M*conj(S)) - wrap(ramp)| = %.6e ; if ~2*|ramp| the sign convention is inverted", maxDiff);
    report("demod/sign convention: arg(M*conj(S)) == ramp", maxDiff < 1e-6, buf);

    const int mlRg = 2, mlAz = 2;
    const int winRg = 5, winAz = 5;

    Mat gMatched, gNone, gNegated, maskMatched, maskNone, maskNegated;
    Mat supportMatched, supportNone, supportNegated;
    Mat negRamp = -ramp;
    Mat inputValid(nr, nc, CV_8U, Scalar::all(255));

    int retA = util.complex_coherence_demodulated(master, slave, ramp, inputValid,
        mlRg, mlAz, winRg, winAz, gMatched, maskMatched, supportMatched);
    int retB = util.complex_coherence_multilooked(master, slave, inputValid,
        mlRg, mlAz, winRg, winAz, gNone, maskNone, supportNone);
    int retC = util.complex_coherence_demodulated(master, slave, negRamp, inputValid,
        mlRg, mlAz, winRg, winAz, gNegated, maskNegated, supportNegated);

    if (retA != 0 || retB != 0 || retC != 0) {
        sprintf_s(buf, "returns: matched=%d none=%d negated=%d", retA, retB, retC);
        report("demod/all three calls return 0", false, buf);
        return -1;
    }

    // 输出网格断言
    const int expRows = nr / mlAz, expCols = nc / mlRg;
    sprintf_s(buf, "gamma size=%dx%d expect=%dx%d", gMatched.rows, gMatched.cols, expRows, expCols);
    report("demod/output grid == multilooked grid", gMatched.rows == expRows && gMatched.cols == expCols, buf);
    sprintf_s(buf, "mask size=%dx%d type=%d (expect CV_8U=%d)", maskMatched.rows, maskMatched.cols, maskMatched.type(), CV_8U);
    report("demod/valid_mask is CV_8U and same size",
        maskMatched.rows == expRows && maskMatched.cols == expCols && maskMatched.type() == CV_8U, buf);
    sprintf_s(buf, "support size=%dx%d type=%d (expect CV_32S=%d)",
        supportMatched.rows, supportMatched.cols, supportMatched.type(), CV_32S);
    report("demod/support count is CV_32S and same size",
        supportMatched.rows == expRows && supportMatched.cols == expCols && supportMatched.type() == CV_32S, buf);

    // 统计内部区域均值，避开部分窗口边界
    const int m = 4;
    double sumA = 0.0, sumB = 0.0, sumC = 0.0; int cnt = 0;
    for (int i = m; i < expRows - m; i++) {
        for (int j = m; j < expCols - m; j++) {
            sumA += gMatched.at<double>(i, j);
            sumB += gNone.at<double>(i, j);
            sumC += gNegated.at<double>(i, j);
            ++cnt;
        }
    }
    const double meanA = cnt ? sumA / cnt : 0.0;
    const double meanB = cnt ? sumB / cnt : 0.0;
    const double meanC = cnt ? sumC / cnt : 0.0;

    fprintf(stdout, "  mean gamma: matched_ref=%.6f  no_ref=%.6f  negated_ref=%.6f\n", meanA, meanB, meanC);

    sprintf_s(buf, "mean gamma with matched reference = %.6f (expect ~1.0)", meanA);
    report("demod/gamma -> 1 with matched reference phase", meanA > 0.99, buf);

    sprintf_s(buf, "mean gamma without reference = %.6f (expect << 1 due to ramp)", meanB);
    report("demod/gamma << 1 without reference (artificial decorrelation)", meanB < 0.7, buf);

    sprintf_s(buf, "matched=%.6f negated=%.6f — if negated is the one near 1.0, the compensation sign is inverted",
        meanA, meanC);
    report("demod/compensation sign matches documented convention", meanA > meanC, buf);

    // 全掩膜有效性：本用例所有像元幅度非零，掩膜应全为 1
    int invalidCount = 0;
    for (int i = 0; i < expRows; i++)
        for (int j = 0; j < expCols; j++)
            if (maskMatched.at<uchar>(i, j) == 0) ++invalidCount;
    sprintf_s(buf, "invalid pixels = %d (expect 0 for all-nonzero amplitude)", invalidCount);
    report("demod/valid_mask all valid for nonzero amplitude", invalidCount == 0, buf);

    const int expectedInteriorSupport = mlRg * mlAz * winRg * winAz;
    const int centerSupport = supportMatched.at<int>(expRows / 2, expCols / 2);
    sprintf_s(buf, "center support=%d expect=%d raw sample pairs", centerSupport, expectedInteriorSupport);
    report("demod/support count reports raw contributing pairs",
        centerSupport == expectedInteriorSupport, buf);

    Mat emptyReference, rejectedGamma, rejectedMask, rejectedSupport;
    const int emptyRet = util.complex_coherence_demodulated(master, slave, emptyReference, inputValid,
        mlRg, mlAz, winRg, winAz, rejectedGamma, rejectedMask, rejectedSupport);
    report("demod/empty reference fails closed", emptyRet == -1,
        "empty reference was accepted; use complex_coherence_multilooked for a no-reference baseline");

    return 0;
}

// ------------------------------------------------------------------
// 用例 4：零能量窗口必须置无效
// 构造一半幅度为零的 SLC，零区对应的 valid_mask 必须为 0，
// 以验证“gamma 的数值 0 不兼任无效语义”。
// ------------------------------------------------------------------
int caseZeroEnergyMask()
{
    Utils util;
    const int nr = 32, nc = 32;

    ComplexMat master, slave;
    Mat mre(nr, nc, CV_64F, Scalar::all(0.0)), mim(nr, nc, CV_64F, Scalar::all(0.0));
    Mat sre(nr, nc, CV_64F, Scalar::all(0.0)), sim(nr, nc, CV_64F, Scalar::all(0.0));
    // 仅左半幅有能量，右半幅为严格零（模拟配准后的零填充区）
    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc / 2; j++) {
            mre.at<double>(i, j) = 1.0; mim.at<double>(i, j) = 0.0;
            sre.at<double>(i, j) = 1.0; sim.at<double>(i, j) = 0.0;
        }
    }
    master.SetRe(mre); master.SetIm(mim);
    slave.SetRe(sre);  slave.SetIm(sim);

    Mat inputValid(nr, nc, CV_8U, Scalar::all(0));
    inputValid.colRange(0, nc / 2).setTo(Scalar::all(255));
    Mat g, mask, support;
    if (util.complex_coherence_multilooked(master, slave, inputValid,
        1, 1, 3, 3, g, mask, support) != 0) {
        report("zeromask/call returns 0", false, "non-zero return");
        return -1;
    }

    // 远离交界处的右半幅应全部无效
    int rightInvalid = 0, rightTotal = 0;
    for (int i = 0; i < nr; i++) {
        for (int j = nc / 2 + 2; j < nc; j++) {
            ++rightTotal;
            if (mask.at<uchar>(i, j) == 0) ++rightInvalid;
        }
    }
    // 远离交界处的左半幅应全部有效
    int leftValid = 0, leftTotal = 0;
    for (int i = 0; i < nr; i++) {
        for (int j = 0; j < nc / 2 - 2; j++) {
            ++leftTotal;
            if (mask.at<uchar>(i, j) != 0) ++leftValid;
        }
    }

    char buf[256];
    sprintf_s(buf, "zero-energy region: %d/%d marked invalid (expect all)", rightInvalid, rightTotal);
    report("zeromask/zero-energy windows marked invalid", rightInvalid == rightTotal, buf);
    sprintf_s(buf, "signal region: %d/%d marked valid (expect all)", leftValid, leftTotal);
    report("zeromask/signal windows marked valid", leftValid == leftTotal, buf);
    const int signalSupport = support.at<int>(nr / 2, nc / 4);
    const int boundarySupport = support.at<int>(nr / 2, nc / 2);
    const int zeroSupport = support.at<int>(nr / 2, 3 * nc / 4);
    sprintf_s(buf, "support signal=%d boundary=%d zero=%d", signalSupport, boundarySupport, zeroSupport);
    report("zeromask/support count distinguishes full, partial and zero support",
        signalSupport == 9 && boundarySupport > 0 && boundarySupport < 9 && zeroSupport == 0, buf);
    return 0;
}

// ------------------------------------------------------------------
// 用例 5：参考相位非有限值不得泄漏到有效输出
// ------------------------------------------------------------------
int caseNonFiniteReference()
{
    Utils util;
    const int nr = 16, nc = 16;
    ComplexMat master, slave;
    Mat realPart(nr, nc, CV_64F, Scalar::all(1.0));
    Mat imagPart(nr, nc, CV_64F, Scalar::all(0.0));
    Mat slaveReal = realPart.clone();
    Mat slaveImag = imagPart.clone();
    master.SetRe(realPart); master.SetIm(imagPart);
    slave.SetRe(slaveReal); slave.SetIm(slaveImag);

    Mat inputValid(nr, nc, CV_8U, Scalar::all(255));
    Mat reference(nr, nc, CV_64F, Scalar::all(0.0));
    reference.at<double>(nr / 2, nc / 2) = std::numeric_limits<double>::quiet_NaN();

    Mat g, mask, support;
    if (util.complex_coherence_demodulated(master, slave, reference, inputValid,
        1, 1, 3, 3, g, mask, support) != 0) {
        report("nonfinite/single-NaN call returns 0", false, "non-zero return");
        return -1;
    }

    const int row = nr / 2, col = nc / 2;
    char buf[256];
    sprintf_s(buf, "gamma=%g mask=%d support=%d (expect finite, 1, 8)",
        g.at<double>(row, col), int(mask.at<uchar>(row, col)), support.at<int>(row, col));
    report("nonfinite/single NaN is excluded and reported by support count",
        std::isfinite(g.at<double>(row, col)) && mask.at<uchar>(row, col) != 0 &&
        support.at<int>(row, col) == 8, buf);

    reference.setTo(Scalar::all(std::numeric_limits<double>::quiet_NaN()));
    if (util.complex_coherence_demodulated(master, slave, reference, inputValid,
        1, 1, 3, 3, g, mask, support) != 0) {
        report("nonfinite/all-NaN call returns 0", false, "non-zero return");
        return -1;
    }
    sprintf_s(buf, "gamma=%g mask=%d support=%d (expect 0, 0, 0)",
        g.at<double>(row, col), int(mask.at<uchar>(row, col)), support.at<int>(row, col));
    report("nonfinite/all NaN window is invalid",
        g.at<double>(row, col) == 0.0 && mask.at<uchar>(row, col) == 0 &&
        support.at<int>(row, col) == 0, buf);
    return 0;
}

// ------------------------------------------------------------------
// 用例 6：非均匀幅度、热噪声与参考相位误差下的数值回归
//
// 构造已知总体复相关 rho 的主辅复高斯场，并叠加共同的非均匀幅度和
// 空间相位斜坡。精确参考相位解调后，内部窗口平均 gamma 应接近 rho；
// 加入独立参考相位误差或完全不解调时，gamma 应显著下降。
// ------------------------------------------------------------------
int caseNoisyAmplitudeValidation()
{
    Utils util;
    const int nr = 128, nc = 128;
    const double rho = 0.65;
    const double independentScale = std::sqrt(1.0 - rho * rho);
    const double componentStd = 1.0 / std::sqrt(2.0);
    cv::RNG rng(761923);

    Mat reference(nr, nc, CV_64F);
    Mat perturbedReference(nr, nc, CV_64F);
    Mat mre(nr, nc, CV_64F), mim(nr, nc, CV_64F);
    Mat sre(nr, nc, CV_64F), sim(nr, nc, CV_64F);
    for (int i = 0; i < nr; ++i) {
        for (int j = 0; j < nc; ++j) {
            const double ref = 0.35 * j + 0.18 * i;
            const double amplitude = 1.3 + 0.7 * std::sin(0.07 * i) * std::cos(0.05 * j);
            const double z1r = rng.gaussian(componentStd);
            const double z1i = rng.gaussian(componentStd);
            const double z2r = rng.gaussian(componentStd);
            const double z2i = rng.gaussian(componentStd);
            const double correlatedReal = rho * z1r + independentScale * z2r;
            const double correlatedImag = rho * z1i + independentScale * z2i;
            const double cosRef = std::cos(ref);
            const double sinRef = std::sin(ref);

            mre.at<double>(i, j) = amplitude * (z1r * cosRef - z1i * sinRef);
            mim.at<double>(i, j) = amplitude * (z1r * sinRef + z1i * cosRef);
            sre.at<double>(i, j) = amplitude * correlatedReal;
            sim.at<double>(i, j) = amplitude * correlatedImag;
            reference.at<double>(i, j) = ref;
            perturbedReference.at<double>(i, j) = ref + rng.gaussian(0.8);
        }
    }

    ComplexMat master, slave;
    master.SetRe(mre); master.SetIm(mim);
    slave.SetRe(sre); slave.SetIm(sim);
    Mat inputValid(nr, nc, CV_8U, Scalar::all(255));
    Mat matched, perturbed, noReference;
    Mat matchedMask, perturbedMask, noReferenceMask;
    Mat matchedSupport, perturbedSupport, noReferenceSupport;
    const int window = 15;
    const int retMatched = util.complex_coherence_demodulated(
        master, slave, reference, inputValid, 1, 1, window, window,
        matched, matchedMask, matchedSupport);
    const int retPerturbed = util.complex_coherence_demodulated(
        master, slave, perturbedReference, inputValid, 1, 1, window, window,
        perturbed, perturbedMask, perturbedSupport);
    const int retNone = util.complex_coherence_multilooked(
        master, slave, inputValid, 1, 1, window, window,
        noReference, noReferenceMask, noReferenceSupport);
    if (retMatched != 0 || retPerturbed != 0 || retNone != 0) {
        report("noisy/all estimator calls return 0", false, "one or more calls failed");
        return -1;
    }

    const int margin = window;
    double sumMatched = 0.0, sumPerturbed = 0.0, sumNone = 0.0;
    int count = 0;
    bool allFinite = true;
    for (int i = margin; i < nr - margin; ++i) {
        for (int j = margin; j < nc - margin; ++j) {
            const double gm = matched.at<double>(i, j);
            const double gp = perturbed.at<double>(i, j);
            const double gn = noReference.at<double>(i, j);
            allFinite = allFinite && std::isfinite(gm) && std::isfinite(gp) && std::isfinite(gn);
            sumMatched += gm;
            sumPerturbed += gp;
            sumNone += gn;
            ++count;
        }
    }
    const double meanMatched = sumMatched / count;
    const double meanPerturbed = sumPerturbed / count;
    const double meanNone = sumNone / count;
    fprintf(stdout, "  noisy gamma: target=%.3f matched=%.5f perturbed_ref=%.5f no_ref=%.5f\n",
        rho, meanMatched, meanPerturbed, meanNone);

    char buf[256];
    sprintf_s(buf, "target rho=%.3f measured=%.5f tolerance=0.08", rho, meanMatched);
    report("noisy/matched-reference gamma tracks known population correlation",
        allFinite && std::fabs(meanMatched - rho) < 0.08, buf);
    sprintf_s(buf, "matched=%.5f perturbed=%.5f", meanMatched, meanPerturbed);
    report("noisy/reference error lowers estimated coherence",
        meanMatched > meanPerturbed + 0.08, buf);
    sprintf_s(buf, "matched=%.5f no_ref=%.5f", meanMatched, meanNone);
    report("noisy/uncompensated phase ramp lowers estimated coherence",
        meanMatched > meanNone + 0.20, buf);
    return 0;
}

// ------------------------------------------------------------------
// 用例 7：R2 = R1^4 的适用边界（信息性，不作断言）
//
// 在包裹高斯残余相位下应有 R2 ~= R1^4；而用例 1 的构造（+-pi/2）
// 是该关系的反例。此处打印一组高斯噪声相位下的实测值，供参考。
// ------------------------------------------------------------------
int caseGaussianRelation()
{
    Utils util;
    const int nr = 256, nc = 256;
    cv::RNG rng(12345);

    fprintf(stdout, "  --- R2 vs R1^4 under wrapped-Gaussian residual phase (informational) ---\n");
    const double sigmas[] = { 0.2, 0.5, 1.0 };
    for (int s = 0; s < 3; s++) {
        Mat phase(nr, nc, CV_64F);
        rng.fill(phase, cv::RNG::NORMAL, Scalar::all(0.0), Scalar::all(sigmas[s]));
        Mat wrapped;
        util.wrap(phase, wrapped);

        Mat r1, r2;
        if (util.phase_circular_concentration(wrapped, 5, 5, r1) != 0) continue;
        if (util.phase_coherence(wrapped, 5, 5, r2) != 0) continue;

        double m1 = 0.0, m2 = 0.0; int cnt = 0;
        for (int i = 4; i < nr - 4; i++) {
            for (int j = 4; j < nc - 4; j++) {
                m1 += r1.at<double>(i, j);
                m2 += r2.at<double>(i, j);
                ++cnt;
            }
        }
        m1 /= cnt; m2 /= cnt;
        fprintf(stdout, "   sigma=%.2f rad : mean R1=%.5f  mean R2=%.5f  R1^4=%.5f  (theory R2=exp(-2*s^2)=%.5f)\n",
            sigmas[s], m1, m2, m1 * m1 * m1 * m1, std::exp(-2.0 * sigmas[s] * sigmas[s]));
    }
    return 0;
}

// ------------------------------------------------------------------
// 用例 8：R1/R2 单门限不可保持原掩膜（信息性）
//
// 现有调用点的门限是按 R2 行为形成的（SBAS 夹取 [0.3, 0.95]、
// FormatConversion/S1 TOPS ESD 硬编码 0.4）。本用例只验证改用 R1 后
// 不存在保持原掩膜的通用单门限，不提供生产迁移阈值。
//
// 用例 5 已证明解析换算 R2 = R1^4 在低相干区失效（sigma=1.0 时偏差
// 达 43%），且偏置随窗口样本数 N 变化，故不存在通用换算常数。
// 对每个窗口尺寸扫描 R1 门限，找出与原 R2 门限
// 选出**相同像元数**的那个 R1 值，并报告两个掩膜的实际重合度
// (Jaccard = |交集| / |并集|)。
//
// 输入相位为分段包裹高斯场（sigma 在空间上变化），以覆盖从高相干到
// 低相干的全量程。样本在空间上独立，不代表真实残余相位的相关结构，
// 因此打印出的计数匹配门限不可用于生产，也不能据此断言真实门限漂移方向。
// ------------------------------------------------------------------
int caseThresholdCalibration()
{
    Utils util;
    const int nr = 512, nc = 512;
    cv::RNG rng(20260822);

    // 构造相干性空间渐变的残余相位场：sigma 从 0.1 线性增至 1.6 rad。
    // 这样单幅影像内即覆盖高/中/低相干，标定不偏向某一水平。
    Mat phase(nr, nc, CV_64F);
    for (int i = 0; i < nr; i++) {
        const double sigma = 0.1 + 1.5 * double(i) / double(nr - 1);
        for (int j = 0; j < nc; j++) {
            phase.at<double>(i, j) = rng.gaussian(sigma);
        }
    }
    Mat wrapped;
    if (util.wrap(phase, wrapped) < 0) {
        report("calibration/wrap", false, "util.wrap() failed");
        return -1;
    }

    // 待标定的窗口尺寸：3x3 为 Unwrap/ESD 与无窗口重载的默认；
    // 5x5 / 7x7 / 9x9 覆盖 SBAS 与干涉形成节点常用配置。
    const int wnds[] = { 3, 5, 7, 9 };
    // 待迁移的原 R2 门限：0.3 与 0.95 为 SBAS 夹取边界，0.4 为 ESD 硬编码。
    const double r2Thresholds[] = { 0.3, 0.4, 0.95 };

    fprintf(stdout, "  --- Count-matched R1 thresholds (non-production diagnostic only) ---\n");
    fprintf(stdout, "  (naive analytic conversion R1 = R2^(1/4) shown for contrast)\n");
    fprintf(stdout, "  %-6s %-8s %-10s %-10s %-9s %-8s\n",
        "window", "R2_thr", "R1_count", "R1=R2^.25", "Jaccard", "R2_keep%");

    bool anyComputed = false;
    for (int w = 0; w < 4; w++) {
        const int wnd = wnds[w];
        Mat r1, r2;
        if (util.phase_circular_concentration(wrapped, wnd, wnd, r1) != 0) continue;
        if (util.phase_coherence(wrapped, wnd, wnd, r2) != 0) continue;

        // 只统计远离边界的内部区域，避免两个 estimator 的边界处理差异干扰
        const int margin = wnd + 1;
        std::vector<double> r1Vals;
        std::vector<unsigned char> r2Keep[3];
        for (int t = 0; t < 3; t++) r2Keep[t].reserve(size_t(nr) * nc);
        r1Vals.reserve(size_t(nr) * nc);

        for (int i = margin; i < nr - margin; i++) {
            for (int j = margin; j < nc - margin; j++) {
                const double v1 = r1.at<double>(i, j);
                const double v2 = r2.at<double>(i, j);
                r1Vals.push_back(v1);
                for (int t = 0; t < 3; t++) {
                    r2Keep[t].push_back(v2 >= r2Thresholds[t] ? 1 : 0);
                }
            }
        }
        const size_t total = r1Vals.size();
        if (total == 0) continue;
        anyComputed = true;

        for (int t = 0; t < 3; t++) {
            // 原 R2 门限保留的像元数
            size_t keepCount = 0;
            for (size_t k = 0; k < total; k++) keepCount += r2Keep[t][k];

            // 找出保留相同像元数的 R1 门限：即 R1 的第 (total-keepCount) 个分位值。
            // 用副本排序，避免破坏 r1Vals 供后续门限复用。
            double r1Equiv = 1.0;
            if (keepCount == 0) {
                r1Equiv = 1.0;  // 原门限一个都不保留
            }
            else if (keepCount >= total) {
                r1Equiv = 0.0;  // 原门限全部保留
            }
            else {
                std::vector<double> sorted(r1Vals);
                const size_t idx = total - keepCount;  // 升序下第 idx 个即为门限
                std::nth_element(sorted.begin(), sorted.begin() + idx, sorted.end());
                r1Equiv = sorted[idx];
            }

            // 用实测门限重建掩膜，计算与原掩膜的重合度
            size_t inter = 0, uni = 0;
            for (size_t k = 0; k < total; k++) {
                const bool a = (r2Keep[t][k] != 0);
                const bool b = (r1Vals[k] >= r1Equiv);
                if (a && b) ++inter;
                if (a || b) ++uni;
            }
            const double jaccard = uni > 0 ? double(inter) / double(uni) : 1.0;
            const double naive = std::pow(r2Thresholds[t], 0.25);

            fprintf(stdout, "  %-6d %-8.2f %-10.4f %-10.4f %-9.4f %-8.2f\n",
                wnd, r2Thresholds[t], r1Equiv, naive, jaccard,
                100.0 * double(keepCount) / double(total));
        }
    }

    report("calibration/thresholds computed", anyComputed,
        "no window size produced valid R1/R2 pair");

    fprintf(stdout, "  NOTE: R1_count is diagnostic and must not be used as a production threshold.\n");
    fprintf(stdout, "        Jaccard < 1 proves that no single R1 threshold preserves the R2 mask;\n");
    fprintf(stdout, "        real spatial correlation can change both the value and direction of drift.\n");
    return 0;
}

} // namespace

int RunCoherenceSemanticsRegression()
{
    fprintf(stdout, "==== Coherence semantics regression ====\n");
    fprintf(stdout, "\n[1] Discriminator: R1 vs R2 semantics\n");
    caseDiscriminator();
    fprintf(stdout, "\n[2] Constant phase sanity\n");
    caseConstantPhase();
    fprintf(stdout, "\n[3] Demodulated complex coherence: sign convention and grid\n");
    caseDemodulation();
    fprintf(stdout, "\n[4] Zero-energy window must be marked invalid\n");
    caseZeroEnergyMask();
    fprintf(stdout, "\n[5] Non-finite reference handling and support count\n");
    caseNonFiniteReference();
    fprintf(stdout, "\n[6] Noisy nonuniform-amplitude complex coherence\n");
    caseNoisyAmplitudeValidation();
    fprintf(stdout, "\n[7] R2 vs R1^4 relation (informational)\n");
    caseGaussianRelation();
    fprintf(stdout, "\n[8] Threshold non-equivalence: R2 vs count-matched R1 (informational)\n");
    caseThresholdCalibration();

    fprintf(stdout, "\n==== %s : %d failure(s) ====\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT", g_failed);
    return g_failed == 0 ? 0 : 1;
}
