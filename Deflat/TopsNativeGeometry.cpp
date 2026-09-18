#include "TopsNativeGeometry.h"
#include "..\include\Utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
const double kSemiMajorAxis = 6378137.0;
const double kSemiMinorAxis = 6356752.314245179;

// slave 零多普勒快速找根窗口。零多普勒解始终落在配准时间种子附近（毫秒量级），
// 而阶梯搜索窗口的初值是 8 s，按 32 分格从左边缘线性扫描要空走十余次求值才能变号。
// 这里先在种子附近的窄窗内细步长找变号，命中即用；未命中回退到原有全窗口扫描，
// 后者语义与结果保持不变。多普勒沿时间单调，窄窗内根唯一，故两条路径给出同一根。
const double kSlaveZeroDopplerFastProbeHalfWindowSeconds = 0.25;
const int kSlaveZeroDopplerFastProbePartitions = 16;

cv::Vec3d toVector(const Position& p)
{
    return cv::Vec3d(p.x, p.y, p.z);
}

cv::Vec3d toVector(const Velocity& v)
{
    return cv::Vec3d(v.vx, v.vy, v.vz);
}

Position toPosition(const cv::Vec3d& v)
{
    return Position(v[0], v[1], v[2]);
}

double ellipsoidEquation(const cv::Vec3d& p)
{
    return p[0] * p[0] / (kSemiMajorAxis * kSemiMajorAxis) +
           p[1] * p[1] / (kSemiMajorAxis * kSemiMajorAxis) +
           p[2] * p[2] / (kSemiMinorAxis * kSemiMinorAxis) - 1.0;
}

bool finiteVector(const cv::Vec3d& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

double vectorNorm(const cv::Vec3d& value)
{
    return std::sqrt(value.dot(value));
}

// The three residuals are range/rho, normalized zero Doppler, and
// ellipsoid-height/a.  Their 3x3 Jacobian is therefore dimensionally
// comparable (1/metre); its Frobenius condition estimate is conservative.
bool rdeConditionAndPointBound(const cv::Vec3d& point, const cv::Vec3d& satellite,
                               const cv::Vec3d& velocity, double rho,
                               double rangeResidual, double dopplerResidual,
                               double heightResidual, double& condition,
                               double& pointErrorBound)
{
    const cv::Vec3d d = point - satellite;
    const double speed = vectorNorm(velocity);
    if (rho <= 0.0 || speed <= 0.0) return false;
    const double a2 = kSemiMajorAxis * kSemiMajorAxis;
    const double b2 = kSemiMinorAxis * kSemiMinorAxis;
    const double j00 = d[0] / (rho * rho), j01 = d[1] / (rho * rho), j02 = d[2] / (rho * rho);
    const double j10 = velocity[0] / (rho * speed), j11 = velocity[1] / (rho * speed), j12 = velocity[2] / (rho * speed);
    // This row is the derivative of the dimensionless ellipsoid equation,
    // so every Jacobian row has unit 1/metre after residual normalization.
    const double j20 = 2.0 * point[0] / a2;
    const double j21 = 2.0 * point[1] / a2;
    const double j22 = 2.0 * point[2] / b2;
    const double c00 = j11 * j22 - j12 * j21;
    const double c01 = j02 * j21 - j01 * j22;
    const double c02 = j01 * j12 - j02 * j11;
    const double c10 = j12 * j20 - j10 * j22;
    const double c11 = j00 * j22 - j02 * j20;
    const double c12 = j02 * j10 - j00 * j12;
    const double c20 = j10 * j21 - j11 * j20;
    const double c21 = j01 * j20 - j00 * j21;
    const double c22 = j00 * j11 - j01 * j10;
    const double determinant = j00 * c00 + j01 * c10 + j02 * c20;
    const double normJ = std::sqrt(j00*j00+j01*j01+j02*j02+j10*j10+j11*j11+j12*j12+j20*j20+j21*j21+j22*j22);
    const double normAdj = std::sqrt(c00*c00+c01*c01+c02*c02+c10*c10+c11*c11+c12*c12+c20*c20+c21*c21+c22*c22);
    if (!std::isfinite(determinant) || std::fabs(determinant) <= 1e-300 || !std::isfinite(normJ) || !std::isfinite(normAdj)) return false;
    const double inverseNorm = normAdj / std::fabs(determinant);
    condition = normJ * inverseNorm;
    const double normalizedResidual = std::sqrt((rangeResidual / rho) * (rangeResidual / rho) +
        dopplerResidual * dopplerResidual + (heightResidual / kSemiMajorAxis) * (heightResidual / kSemiMajorAxis));
    pointErrorBound = inverseNorm * normalizedResidual;
    return std::isfinite(condition) && std::isfinite(pointErrorBound);
}

bool validOrbitContract(const TopsFepV5Orbit& orbit, std::string* error = nullptr)
{
    const bool rawStateVecCubic =
        orbit.interpolationStrategy == "raw_state_vec_nearest_contiguous_8_osv_cubic_least_squares_v1";
    const bool fineApplyOrbit =
        orbit.interpolationStrategy == "orbit_state_vectors_apply_orbit_1s_lagrange_v1";
    const bool fineV2CubicHermite =
        orbit.interpolationStrategy == "fine_state_vec_cubic_hermite_v2";
    if (orbit.stateVectors.type() != CV_64F || orbit.stateVectors.cols != 7) {
        if (error) *error = "stateVectors type/cols invalid (type=" + std::to_string(orbit.stateVectors.type()) + ", cols=" + std::to_string(orbit.stateVectors.cols) + ")";
        return false;
    }
    if (orbit.stateVectors.rows < (rawStateVecCubic ? 8 : 4)) {
        if (error) *error = "stateVectors rows too few (" + std::to_string(orbit.stateVectors.rows) + ")";
        return false;
    }
    if (orbit.timeScale != "GPS") {
        if (error) *error = "timeScale != GPS (" + orbit.timeScale + ")";
        return false;
    }
    if (!std::isfinite(orbit.acquisitionStartGps) || !std::isfinite(orbit.acquisitionStopGps) ||
        !(orbit.acquisitionStopGps > orbit.acquisitionStartGps)) {
        if (error) *error = "acquisition GPS range invalid";
        return false;
    }
    if (!std::isfinite(orbit.geometryStartGps) || !std::isfinite(orbit.geometryStopGps) ||
        !(orbit.geometryStopGps > orbit.geometryStartGps)) {
        if (error) *error = "geometry GPS range invalid";
        return false;
    }
    if (!std::isfinite(orbit.interpolationMarginSeconds) || orbit.interpolationMarginSeconds <= 0.0) {
        if (error) *error = "interpolationMarginSeconds invalid";
        return false;
    }
    if (!std::isfinite(orbit.osvStartGps) || !std::isfinite(orbit.osvStopGps) ||
        !(orbit.osvStopGps > orbit.osvStartGps)) {
        if (error) *error = "osv GPS range invalid";
        return false;
    }
    if (orbit.source.empty() || orbit.selectionReason.empty()) {
        if (error) *error = "source or selectionReason empty";
        return false;
    }
    if (!rawStateVecCubic && !fineApplyOrbit && !fineV2CubicHermite) {
        if (error) *error = "unsupported interpolationStrategy (" + orbit.interpolationStrategy + ")";
        return false;
    }
    if (rawStateVecCubic && orbit.source != "state_vec") {
        if (error) *error = "rawStateVecCubic but source != state_vec (" + orbit.source + ")";
        return false;
    }
    if (fineV2CubicHermite && orbit.source != "fine_state_vec") {
        if (error) *error = "fineV2CubicHermite but source != fine_state_vec (" + orbit.source + ")";
        return false;
    }
    for (int row = 0; row < orbit.stateVectors.rows; ++row)
    {
        for (int column = 0; column < orbit.stateVectors.cols; ++column)
        {
            if (!std::isfinite(orbit.stateVectors.at<double>(row, column))) {
                if (error) *error = "stateVectors contains non-finite at row " + std::to_string(row) + ", col " + std::to_string(column);
                return false;
            }
        }
        if (row > 0 && orbit.stateVectors.at<double>(row, 0) <= orbit.stateVectors.at<double>(row - 1, 0)) {
            if (error) *error = "stateVectors timestamps not strictly increasing at row " + std::to_string(row);
            return false;
        }
    }
    const double first = orbit.stateVectors.at<double>(0, 0);
    const double last = orbit.stateVectors.at<double>(orbit.stateVectors.rows - 1, 0);
    const bool geometryCovered = rawStateVecCubic
        ? first <= orbit.geometryStartGps && last >= orbit.geometryStopGps
        : first <= orbit.geometryStartGps - orbit.interpolationMarginSeconds &&
          last >= orbit.geometryStopGps + orbit.interpolationMarginSeconds;
    if (std::fabs(first - orbit.osvStartGps) >= 1e-6) {
        if (error) *error = "first timestamp differs from osvStartGps (" + std::to_string(first) + " vs " + std::to_string(orbit.osvStartGps) + ")";
        return false;
    }
    if (std::fabs(last - orbit.osvStopGps) >= 1e-6) {
        if (error) *error = "last timestamp differs from osvStopGps (" + std::to_string(last) + " vs " + std::to_string(orbit.osvStopGps) + ")";
        return false;
    }
    if (!geometryCovered) {
        if (error) *error = "geometry not covered (first=" + std::to_string(first) + ", last=" + std::to_string(last) +
            ", geoStart=" + std::to_string(orbit.geometryStartGps) + ", geoStop=" + std::to_string(orbit.geometryStopGps) +
            ", margin=" + std::to_string(orbit.interpolationMarginSeconds) + ")";
        return false;
    }
    return true;
}
}

struct TopsNativeGeometry::RawStateVecCubicInterpolator
{
    struct Segment
    {
        double originGps = 0.0;
        Mat positionCoefficients;
        Mat velocityCoefficients;
    };

    explicit RawStateVecCubicInterpolator(const Mat& vectors)
    {
        vectors.copyTo(stateVectors);
    }

    bool prepare()
    {
        if (stateVectors.type() != CV_64F || stateVectors.cols != 7 || stateVectors.rows < 8) return false;
        segments.clear();
        segments.reserve(static_cast<size_t>(stateVectors.rows - 7));
        for (int first = 0; first <= stateVectors.rows - 8; ++first) {
            Segment segment;
            segment.originGps = stateVectors.at<double>(first + 3, 0);
            Mat design(8, 4, CV_64F);
            Mat positions(8, 3, CV_64F);
            Mat velocities(8, 3, CV_64F);
            for (int row = 0; row < 8; ++row) {
                const double dt = stateVectors.at<double>(first + row, 0) - segment.originGps;
                design.at<double>(row, 0) = 1.0;
                design.at<double>(row, 1) = dt;
                design.at<double>(row, 2) = dt * dt;
                design.at<double>(row, 3) = dt * dt * dt;
                for (int component = 0; component < 3; ++component) {
                    positions.at<double>(row, component) = stateVectors.at<double>(first + row, component + 1);
                    velocities.at<double>(row, component) = stateVectors.at<double>(first + row, component + 4);
                }
            }
            if (!cv::solve(design, positions, segment.positionCoefficients, cv::DECOMP_SVD) ||
                !cv::solve(design, velocities, segment.velocityCoefficients, cv::DECOMP_SVD)) return false;
            segments.push_back(segment);
        }
        return !segments.empty();
    }

    bool state(double time, Position& position, Velocity& velocity) const
    {
        if (segments.empty() || !std::isfinite(time) || time < firstTime() || time > lastTime()) return false;
        int firstAtOrAfter = 0;
        while (firstAtOrAfter < stateVectors.rows && stateVectors.at<double>(firstAtOrAfter, 0) < time) ++firstAtOrAfter;
        const int first = std::max(0, std::min(stateVectors.rows - 8, firstAtOrAfter - 4));
        const Segment& segment = segments[static_cast<size_t>(first)];
        const double q = time - segment.originGps;
        const auto evaluate = [q](const Mat& coefficients, int component) {
            return ((coefficients.at<double>(3, component) * q + coefficients.at<double>(2, component)) * q +
                coefficients.at<double>(1, component)) * q + coefficients.at<double>(0, component);
        };
        position.x = evaluate(segment.positionCoefficients, 0);
        position.y = evaluate(segment.positionCoefficients, 1);
        position.z = evaluate(segment.positionCoefficients, 2);
        velocity.vx = evaluate(segment.velocityCoefficients, 0);
        velocity.vy = evaluate(segment.velocityCoefficients, 1);
        velocity.vz = evaluate(segment.velocityCoefficients, 2);
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
            std::isfinite(velocity.vx) && std::isfinite(velocity.vy) && std::isfinite(velocity.vz);
    }

    double firstTime() const { return stateVectors.at<double>(0, 0); }
    double lastTime() const { return stateVectors.at<double>(stateVectors.rows - 1, 0); }

    Mat stateVectors;
    std::vector<Segment> segments;
};

TopsNativeGeometry::TopsNativeGeometry(const TopsFepV5Orbit& masterOrbit,
                                       const TopsFepV5Orbit& slaveOrbit,
                                       const TopsFepV5Options& options)
    : m_masterOrbit(masterOrbit),
      m_slaveOrbit(slaveOrbit),
      m_options(options)
{
}

TopsNativeGeometry::~TopsNativeGeometry() = default;

bool TopsNativeGeometry::prepare(std::string* failureReason)
{
    std::string err;
    if (!validOrbitContract(m_masterOrbit, &err)) {
        if (failureReason) *failureReason = "masterOrbit: " + err;
        return false;
    }
    if (!validOrbitContract(m_slaveOrbit, &err)) {
        if (failureReason) *failureReason = "slaveOrbit: " + err;
        return false;
    }
    if (m_masterOrbit.interpolationStrategy != m_slaveOrbit.interpolationStrategy) {
        if (failureReason) *failureReason = "interpolationStrategy mismatch: master=" + m_masterOrbit.interpolationStrategy + ", slave=" + m_slaveOrbit.interpolationStrategy;
        return false;
    }
    if (m_options.masterLookSide == 0) {
        if (failureReason) *failureReason = "masterLookSide == 0";
        return false;
    }
    if (m_options.maxRdeIterations < 1) {
        if (failureReason) *failureReason = "maxRdeIterations < 1 (" + std::to_string(m_options.maxRdeIterations) + ")";
        return false;
    }
    if (m_options.maxZeroDopplerIterations < 1) {
        if (failureReason) *failureReason = "maxZeroDopplerIterations < 1 (" + std::to_string(m_options.maxZeroDopplerIterations) + ")";
        return false;
    }
    if (m_options.slaveSearchHalfWindowSeconds <= 0.0) {
        if (failureReason) *failureReason = "slaveSearchHalfWindowSeconds <= 0.0 (" + std::to_string(m_options.slaveSearchHalfWindowSeconds) + ")";
        return false;
    }
    if (m_options.slaveSearchMaximumHalfWindowSeconds < m_options.slaveSearchHalfWindowSeconds) {
        if (failureReason) *failureReason = "slaveSearchMaximumHalfWindowSeconds < slaveSearchHalfWindowSeconds";
        return false;
    }
    if (m_options.slaveSearchExpansionFactor <= 1.0) {
        if (failureReason) *failureReason = "slaveSearchExpansionFactor <= 1.0 (" + std::to_string(m_options.slaveSearchExpansionFactor) + ")";
        return false;
    }
    if (m_options.maxSlaveSearchExpansions < 0) {
        if (failureReason) *failureReason = "maxSlaveSearchExpansions < 0 (" + std::to_string(m_options.maxSlaveSearchExpansions) + ")";
        return false;
    }
    if (!std::isfinite(m_options.maxSlaveRangeError) || m_options.maxSlaveRangeError <= 0.0) {
        if (failureReason) *failureReason = "maxSlaveRangeError invalid or <= 0.0 (" + std::to_string(m_options.maxSlaveRangeError) + ")";
        return false;
    }
    if (m_masterOrbit.interpolationStrategy == "raw_state_vec_nearest_contiguous_8_osv_cubic_least_squares_v1") {
        m_masterRaw.reset(new RawStateVecCubicInterpolator(m_masterOrbit.stateVectors));
        m_slaveRaw.reset(new RawStateVecCubicInterpolator(m_slaveOrbit.stateVectors));
        if (!m_masterRaw->prepare()) {
            if (failureReason) *failureReason = "m_masterRaw->prepare() returned false";
            return false;
        }
        if (!m_slaveRaw->prepare()) {
            if (failureReason) *failureReason = "m_slaveRaw->prepare() returned false";
            return false;
        }
        return true;
    }
    const OrbitInterpolationMode mode = m_masterOrbit.interpolationStrategy == "fine_state_vec_cubic_hermite_v2"
        ? OrbitInterpolationMode::FineV2CubicHermite
        : OrbitInterpolationMode::ResampledLagrange;
    m_masterFine.reset(new orbitStateVectors(m_masterOrbit.stateVectors,
                                             m_masterOrbit.geometryStartGps,
                                             m_masterOrbit.geometryStopGps,
                                             mode));
    m_slaveFine.reset(new orbitStateVectors(m_slaveOrbit.stateVectors,
                                            m_slaveOrbit.geometryStartGps,
                                            m_slaveOrbit.geometryStopGps,
                                            mode));
    const int masterRc = m_masterFine->applyOrbit();
    if (masterRc != 0) {
        if (failureReason) *failureReason = "m_masterFine->applyOrbit() failed, rc=" + std::to_string(masterRc);
        return false;
    }
    const int slaveRc = m_slaveFine->applyOrbit();
    if (slaveRc != 0) {
        if (failureReason) *failureReason = "m_slaveFine->applyOrbit() failed, rc=" + std::to_string(slaveRc);
        return false;
    }
    return true;
}

bool TopsNativeGeometry::masterState(double time, Position& position, Velocity& velocity) const
{
    if (m_masterRaw) return m_masterRaw->state(time, position, velocity);
    return m_masterFine && std::isfinite(time) && m_masterFine->getPosition(time, position) == 0 &&
           m_masterFine->getVelocity(time, velocity) == 0;
}

bool TopsNativeGeometry::slaveState(double time, Position& position, Velocity& velocity) const
{
	if (m_slaveRaw) return m_slaveRaw->state(time, position, velocity);
	return m_slaveFine && std::isfinite(time) && m_slaveFine->getPosition(time, position) == 0 &&
		m_slaveFine->getVelocity(time, velocity) == 0;
}

bool TopsNativeGeometry::slaveCoverage(double& firstGps, double& lastGps) const
{
    if (m_slaveRaw) {
        firstGps = m_slaveRaw->firstTime();
        lastGps = m_slaveRaw->lastTime();
        return true;
    }
    if (!m_slaveFine || m_slaveFine->newStateVectors.rows < 2) return false;
    firstGps = m_slaveFine->newStateVectors.at<double>(0, 0);
    lastGps = m_slaveFine->newStateVectors.at<double>(m_slaveFine->newStateVectors.rows - 1, 0);
    return true;
}

bool TopsNativeGeometry::solveMasterH0Point(const Position& satellite,
                                            const Velocity& velocity,
                                            double rhoMaster,
                                            const Position* rangeSeed,
                                            Position& point,
                                            SampleClosure& closure,
                                            TopsFepV5BurstStatistics& statistics) const
{
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const cv::Vec3d s = toVector(satellite);
    const cv::Vec3d v = toVector(velocity);
    const double speed = std::sqrt(v.dot(v));
    if (!finiteVector(s) || !finiteVector(v) || !std::isfinite(rhoMaster) || rhoMaster <= 0.0 || speed <= 0.0)
    {
        fprintf(stderr, "solveMasterH0Point(): invalid satellite state or master slant range.\\n");
        return false;
    }

    const cv::Vec3d vUnit = v * (1.0 / speed);
    cv::Vec3d towardEarth = -s;
    towardEarth -= vUnit * towardEarth.dot(vUnit);
    const double towardEarthNorm = std::sqrt(towardEarth.dot(towardEarth));
    if (towardEarthNorm <= 0.0)
    {
        fprintf(stderr, "solveMasterH0Point(): cannot construct zero-Doppler earthward basis.\\n");
        return false;
    }
    const cv::Vec3d e1 = towardEarth * (1.0 / towardEarthNorm);
    const cv::Vec3d e2 = vUnit.cross(e1);
    const cv::Vec3d lookAxis = v.cross(s);
    const double lookAxisNorm = std::sqrt(lookAxis.dot(lookAxis));
    if (lookAxisNorm <= 0.0)
    {
        fprintf(stderr, "solveMasterH0Point(): cannot construct observation-side axis.\\n");
        return false;
    }

    const auto evaluate = [&](double theta) {
        return ellipsoidEquation(s + rhoMaster * (std::cos(theta) * e1 + std::sin(theta) * e2));
    };
    double selectedLeft = 0.0;
    double selectedRight = 0.0;
    bool found = false;
    // A previous valid range sample supplies the deterministic Newton seed.
    // Only the first usable sample of a row performs the bounded coarse scan.
    if (rangeSeed)
    {
        const cv::Vec3d seedLos = toVector(*rangeSeed) - s;
        const double seedNorm = vectorNorm(seedLos);
        if (seedNorm > 0.0)
        {
            const double thetaSeed = std::atan2(seedLos.dot(e2) / seedNorm, seedLos.dot(e1) / seedNorm);
            selectedLeft = thetaSeed - 0.05;
            selectedRight = thetaSeed + 0.05;
            const cv::Vec3d candidate = s + rhoMaster * (std::cos(thetaSeed) * e1 + std::sin(thetaSeed) * e2);
            found = (candidate - s).dot(lookAxis) * m_options.masterLookSide > 0.0;
        }
    }
    const int samples = 720;
    for (int index = 0; !found && index < samples; ++index)
    {
        const double left = -CV_PI + 2.0 * CV_PI * index / samples;
        const double right = -CV_PI + 2.0 * CV_PI * (index + 1) / samples;
        const double fLeft = evaluate(left);
        const double fRight = evaluate(right);
        if (!std::isfinite(fLeft) || !std::isfinite(fRight) || fLeft * fRight > 0.0) continue;
        double a = left;
        double b = right;
        double fa = fLeft;
        for (int iteration = 0; iteration < 48; ++iteration)
        {
            const double mid = 0.5 * (a + b);
            const double fMid = evaluate(mid);
            if (fa * fMid <= 0.0) b = mid; else { a = mid; fa = fMid; }
        }
        const double theta = 0.5 * (a + b);
        const cv::Vec3d candidate = s + rhoMaster * (std::cos(theta) * e1 + std::sin(theta) * e2);
        const double side = (candidate - s).dot(lookAxis) / lookAxisNorm;
        if (side * m_options.masterLookSide > 0.0)
        {
            selectedLeft = left;
            selectedRight = right;
            found = true;
            break;
        }
    }
    if (!found)
    {
        fprintf(stderr, "solveMasterH0Point(): no h=0 ellipsoid intersection on the declared observation side; rhoMaster=%.12g, lookSide=%d.\\n",
            rhoMaster, m_options.masterLookSide);
        return false;
    }

    double theta = 0.5 * (selectedLeft + selectedRight);
    double lastStep = std::numeric_limits<double>::infinity();
    int iterations = 0;
    double derivative = 0.0;
    for (; iterations < m_options.maxRdeIterations; ++iterations)
    {
        const double value = evaluate(theta);
        const double delta = 1e-7;
        derivative = (evaluate(theta + delta) - evaluate(theta - delta)) / (2.0 * delta);
        if (!std::isfinite(value) || !std::isfinite(derivative) || std::fabs(derivative) <= 1e-14)
        {
            fprintf(stderr, "solveMasterH0Point(): Newton derivative is invalid; value=%.12g, derivative=%.12g, iteration=%d.\\n",
                value, derivative, iterations);
            return false;
        }
        lastStep = -value / derivative;
        theta += lastStep;
        if (std::fabs(lastStep) <= 1e-13 && std::fabs(value) <= m_options.maxRdeResidual) break;
    }
    const cv::Vec3d solved = s + rhoMaster * (std::cos(theta) * e1 + std::sin(theta) * e2);
    const cv::Vec3d lineOfSight = solved - s;
    const double rangeResidual = std::fabs(std::sqrt(lineOfSight.dot(lineOfSight)) - rhoMaster);
    const double dopplerResidual = std::fabs(lineOfSight.dot(v) / (rhoMaster * speed));
    const double heightResidual = std::fabs(ellipsoidEquation(solved)) * kSemiMajorAxis;
    double condition = 0.0;
    double pointErrorBound = 0.0;
    if (!rdeConditionAndPointBound(solved, s, v, rhoMaster, rangeResidual, dopplerResidual,
                                   heightResidual, condition, pointErrorBound))
    {
        fprintf(stderr, "solveMasterH0Point(): RDE Jacobian/error-bound evaluation failed.\\n");
        return false;
    }
	// A Newton seed may cross to the other ellipsoid intersection.  The final
	// point, rather than the seed, must satisfy the declared observation side.
    if (!finiteVector(solved) || lineOfSight.dot(lookAxis) * m_options.masterLookSide <= 0.0 ||
		rangeResidual > m_options.maxRdeResidual ||
        dopplerResidual > m_options.maxZeroDopplerResidual || heightResidual > kSemiMajorAxis * m_options.maxRdeResidual ||
        condition > m_options.maxJacobianCondition || iterations == m_options.maxRdeIterations)
    {
        fprintf(stderr, "solveMasterH0Point(): closure rejected; finite=%d, observationSide=%.12g, rangeResidual=%.12g, dopplerResidual=%.12g, heightResidual=%.12g, condition=%.12g, iterations=%d, limits={rde=%.12g,doppler=%.12g,height=%.12g,condition=%.12g,iterations=%d}.\\n",
            finiteVector(solved) ? 1 : 0, lineOfSight.dot(lookAxis) * m_options.masterLookSide,
            rangeResidual, dopplerResidual, heightResidual, condition, iterations,
            m_options.maxRdeResidual, m_options.maxZeroDopplerResidual,
            kSemiMajorAxis * m_options.maxRdeResidual, m_options.maxJacobianCondition,
            m_options.maxRdeIterations);
        return false;
    }

    point = toPosition(solved);
    closure.masterRangeErrorBound = pointErrorBound;
    closure.masterPointLastStep = std::fabs(lastStep) * rhoMaster;
    statistics.maxRdeIterations = std::max(statistics.maxRdeIterations, iterations + 1);
	statistics.totalRdeIterations += iterations + 1;
    statistics.maxMasterRangeResidual = std::max(statistics.maxMasterRangeResidual, rangeResidual);
    statistics.maxMasterZeroDopplerResidual = std::max(statistics.maxMasterZeroDopplerResidual, dopplerResidual);
    statistics.maxEllipsoidResidual = std::max(statistics.maxEllipsoidResidual, heightResidual);
    statistics.maxJacobianCondition = std::max(statistics.maxJacobianCondition, condition);
	statistics.masterRdeElapsedMilliseconds +=
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

bool TopsNativeGeometry::solveMasterReferenceHeightPoint(const Position& satellite,
                                                         const Velocity& velocity,
                                                         double rhoMaster,
                                                         double referenceEllipsoidHeight,
                                                         const Position* rangeSeed,
                                                         Position& point,
                                                         SampleClosure& closure,
                                                         TopsFepV5BurstStatistics& statistics) const
{
    if (!std::isfinite(referenceEllipsoidHeight) ||
        referenceEllipsoidHeight <= -kSemiMinorAxis + 1.0) return false;
    if (std::fabs(referenceEllipsoidHeight) <= 1e-9) {
        return solveMasterH0Point(satellite, velocity, rhoMaster, rangeSeed, point, closure, statistics);
    }

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const cv::Vec3d s = toVector(satellite);
    const cv::Vec3d v = toVector(velocity);
    const double speed = vectorNorm(v);
    if (!finiteVector(s) || !finiteVector(v) || !std::isfinite(rhoMaster) || rhoMaster <= 0.0 || speed <= 0.0)
        return false;
    const cv::Vec3d vUnit = v * (1.0 / speed);
    cv::Vec3d towardEarth = -s;
    towardEarth -= vUnit * towardEarth.dot(vUnit);
    const double towardEarthNorm = vectorNorm(towardEarth);
    const cv::Vec3d lookAxis = v.cross(s);
    const double lookAxisNorm = vectorNorm(lookAxis);
    if (towardEarthNorm <= 0.0 || lookAxisNorm <= 0.0) return false;
    const cv::Vec3d e1 = towardEarth * (1.0 / towardEarthNorm);
    const cv::Vec3d e2 = vUnit.cross(e1);
	const double heightTolerance = kSemiMajorAxis * m_options.maxRdeResidual;
    // A constant geodetic/ellipsoidal height is not an ellipsoid obtained by
    // adding h to both semi-axes.  Evaluate height through the project's
    // canonical ECEF-to-ellipsoid conversion so h_ref has exactly the same
    // meaning as ell2xyz() consumers use.
    const auto evaluate = [&](double theta) {
        const cv::Vec3d p = s + rhoMaster * (std::cos(theta) * e1 + std::sin(theta) * e2);
        double latitude = 0.0, longitude = 0.0, height = 0.0;
        if (!finiteVector(p) || Utils::xyz2ell(p[0], p[1], p[2], latitude, longitude, height) != 0 ||
            !std::isfinite(height)) return std::numeric_limits<double>::quiet_NaN();
        return height - referenceEllipsoidHeight;
    };

    double left = 0.0;
    double right = 0.0;
    double fLeft = 0.0;
    double fRight = 0.0;
    bool bracketed = false;
    const auto acceptBracket = [&](double candidateLeft, double candidateRight,
                                   double candidateFLeft, double candidateFRight) {
        if (!std::isfinite(candidateFLeft) || !std::isfinite(candidateFRight) ||
            candidateFLeft * candidateFRight > 0.0) return false;
        const double mid = 0.5 * (candidateLeft + candidateRight);
        const cv::Vec3d candidate = s + rhoMaster * (std::cos(mid) * e1 + std::sin(mid) * e2);
        if ((candidate - s).dot(lookAxis) * m_options.masterLookSide <= 0.0) return false;
        left = candidateLeft;
        right = candidateRight;
        fLeft = candidateFLeft;
        fRight = candidateFRight;
        return true;
    };
    if (rangeSeed) {
        const cv::Vec3d seedLos = toVector(*rangeSeed) - s;
        const double seedNorm = vectorNorm(seedLos);
        if (seedNorm > 0.0) {
            const double seed = std::atan2(seedLos.dot(e2) / seedNorm, seedLos.dot(e1) / seedNorm);
            const cv::Vec3d candidate = s + rhoMaster * (std::cos(seed) * e1 + std::sin(seed) * e2);
            if ((candidate - s).dot(lookAxis) * m_options.masterLookSide > 0.0) {
                const double candidateLeft = seed - 0.05;
                const double candidateRight = seed + 0.05;
                bracketed = acceptBracket(candidateLeft, candidateRight,
                                          evaluate(candidateLeft), evaluate(candidateRight));
            }
        }
    }
    for (int index = 0; !bracketed && index < 720; ++index) {
        const double candidateLeft = -CV_PI + 2.0 * CV_PI * index / 720.0;
        const double candidateRight = -CV_PI + 2.0 * CV_PI * (index + 1) / 720.0;
        bracketed = acceptBracket(candidateLeft, candidateRight,
                                  evaluate(candidateLeft), evaluate(candidateRight));
    }
    if (!bracketed) return false;

    double theta = 0.5 * (left + right);
    double lastStep = std::numeric_limits<double>::infinity();
    int iteration = 0;
    bool converged = false;
    for (; iteration < m_options.maxRdeIterations; ++iteration) {
        const double value = evaluate(theta);
        if (!std::isfinite(value)) return false;
        if (std::fabs(value) <= heightTolerance) {
            converged = true;
            break;
        }
        if (fLeft * value <= 0.0) {
            right = theta;
            fRight = value;
        } else {
            left = theta;
            fLeft = value;
        }
        const double midpoint = 0.5 * (left + right);
        const double delta = 1e-7;
        const double derivative = (evaluate(theta + delta) - evaluate(theta - delta)) / (2.0 * delta);
        double next = midpoint;
        if (std::isfinite(derivative) && std::fabs(derivative) > 1e-14) {
            const double newton = theta - value / derivative;
            if (std::isfinite(newton) && newton > left && newton < right) next = newton;
        }
        lastStep = next - theta;
        theta = next;
    }
    const cv::Vec3d solved = s + rhoMaster * (std::cos(theta) * e1 + std::sin(theta) * e2);
    const cv::Vec3d los = solved - s;
    const double rangeResidual = std::fabs(vectorNorm(los) - rhoMaster);
    const double dopplerResidual = std::fabs(los.dot(v) / (rhoMaster * speed));
    const double heightResidual = std::fabs(evaluate(theta));
    double condition = 0.0;
    double pointErrorBound = 0.0;
    if (!rdeConditionAndPointBound(solved, s, v, rhoMaster, rangeResidual,
                                   dopplerResidual, heightResidual, condition, pointErrorBound)) return false;
    if (!finiteVector(solved) || los.dot(lookAxis) * m_options.masterLookSide <= 0.0 ||
        rangeResidual > m_options.maxRdeResidual || dopplerResidual > m_options.maxZeroDopplerResidual ||
        heightResidual > heightTolerance || condition > m_options.maxJacobianCondition ||
        !std::isfinite(pointErrorBound) || !converged)
        return false;
    point = toPosition(solved);
    closure.masterRangeErrorBound = pointErrorBound;
    closure.masterPointLastStep = std::fabs(lastStep) * rhoMaster;
    statistics.maxRdeIterations = std::max(statistics.maxRdeIterations, iteration + 1);
    statistics.totalRdeIterations += iteration + 1;
    statistics.maxMasterRangeResidual = std::max(statistics.maxMasterRangeResidual, rangeResidual);
    statistics.maxMasterZeroDopplerResidual = std::max(statistics.maxMasterZeroDopplerResidual, dopplerResidual);
    statistics.maxEllipsoidResidual = std::max(statistics.maxEllipsoidResidual, heightResidual);
	statistics.maxJacobianCondition = std::max(statistics.maxJacobianCondition, condition);
    statistics.masterRdeElapsedMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

bool TopsNativeGeometry::evaluateSlaveDoppler(const Position& point, double time,
                                              double& normalizedDoppler, double& range) const
{
    Position satellite;
    Velocity velocity;
    if (!slaveState(time, satellite, velocity)) return false;
    const cv::Vec3d difference = toVector(point) - toVector(satellite);
    const cv::Vec3d speed = toVector(velocity);
    range = std::sqrt(difference.dot(difference));
    const double speedNorm = std::sqrt(speed.dot(speed));
    if (!std::isfinite(range) || !std::isfinite(speedNorm) || range <= 0.0 || speedNorm <= 0.0) return false;
    normalizedDoppler = difference.dot(speed) / (range * speedNorm);
    return std::isfinite(normalizedDoppler);
}

bool TopsNativeGeometry::solveSlaveZeroDoppler(const Position& point,
                                               double registrationTimeSeed,
                                               double& zeroDopplerTime,
                                               double& rhoSlave,
                                               SampleClosure& closure,
                                               TopsFepV5BurstStatistics& statistics) const
{
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    if ((!m_slaveRaw && !m_slaveFine) || !std::isfinite(registrationTimeSeed)) return false;
    double first = 0.0;
    double last = 0.0;
    if (!slaveCoverage(first, last)) return false;
    const int partitions = 32;
    bool bracketed = false;
    double left = 0.0, right = 0.0, fLeft = 0.0, fRight = 0.0;
    double rangeLeft = 0.0, rangeRight = 0.0, range = 0.0;
    double usedHalfWindow = 0.0;
    int expansion = 0;
	int dopplerEvaluations = 0;
    for (; expansion <= m_options.maxSlaveSearchExpansions; ++expansion)
    {
        const double halfWindow = std::min(m_options.slaveSearchMaximumHalfWindowSeconds,
            m_options.slaveSearchHalfWindowSeconds * std::pow(m_options.slaveSearchExpansionFactor, expansion));
		const double begin = registrationTimeSeed - halfWindow;
		const double end = registrationTimeSeed + halfWindow;
		// Coverage was preflighted for the maximum window.  Clipping here would
		// turn an orbit-coverage defect into a different local ZD solution.
		if (begin < first || end > last || !(end > begin)) return false;
		// 快速路径：零多普勒解几乎总落在种子附近毫秒到百毫秒量级，而本阶梯窗口宽 8 s、按 32 分格
		// 从左边缘线性扫描要空走十余次求值才变号。这里先在种子两侧的窄窗内细步长找变号，命中即用。
		// 为与全窗口扫描“取窗口内第一个变号”的语义严格一致，额外校验窄窗左端与整个窗口左端同号——
		// 多普勒沿时间单调，同号即说明左段无变号，窄窗命中的就是同一个根；否则回退到下面的全扫描。
		// 窄窗只依赖种子，与阶梯无关，故仅在 expansion==0 尝试，避免后续阶梯重复同一探测。
		// 记录的最大搜索半窗仍取阶梯窗口值，保持 flat_earth_slave_search_* 契约不变。
		const double probeHalfWindow = std::min(halfWindow, kSlaveZeroDopplerFastProbeHalfWindowSeconds);
		if (expansion == 0 && probeHalfWindow > 0.0)
		{
			double anchorFLeft = 0.0, anchorRangeLeft = 0.0;
			++dopplerEvaluations;
			if (!evaluateSlaveDoppler(point, begin, anchorFLeft, anchorRangeLeft)) return false;
			const double probeBegin = registrationTimeSeed - probeHalfWindow;
			const double probeEnd = registrationTimeSeed + probeHalfWindow;
			double probeLeft = probeBegin;
			double probeFLeft = 0.0, probeRangeLeft = 0.0;
			++dopplerEvaluations;
			if (!evaluateSlaveDoppler(point, probeLeft, probeFLeft, probeRangeLeft)) return false;
			if (anchorFLeft == 0.0 || anchorFLeft * probeFLeft > 0.0)
			{
				for (int index = 1; index <= kSlaveZeroDopplerFastProbePartitions; ++index)
				{
					const double probeRight =
						probeBegin + (probeEnd - probeBegin) * index / kSlaveZeroDopplerFastProbePartitions;
					double probeFRight = 0.0, probeRangeRight = 0.0;
					++dopplerEvaluations;
					if (!evaluateSlaveDoppler(point, probeRight, probeFRight, probeRangeRight)) return false;
					if (probeFLeft == 0.0 || probeFLeft * probeFRight <= 0.0)
					{
						left = probeLeft;
						right = probeRight;
						fLeft = probeFLeft;
						fRight = probeFRight;
						rangeLeft = probeRangeLeft;
						rangeRight = probeRangeRight;
						bracketed = true;
						break;
					}
					probeLeft = probeRight;
					probeFLeft = probeFRight;
					probeRangeLeft = probeRangeRight;
				}
			}
			if (bracketed) { usedHalfWindow = halfWindow; break; }
		}
		++dopplerEvaluations;
		if (!evaluateSlaveDoppler(point, begin, fLeft, rangeLeft)) return false;
		left = begin;
        for (int index = 1; index <= partitions; ++index)
        {
            right = begin + (end - begin) * index / partitions;
			++dopplerEvaluations;
			if (!evaluateSlaveDoppler(point, right, fRight, rangeRight)) return false;
			if (fLeft == 0.0 || fLeft * fRight <= 0.0) { bracketed = true; break; }
			left = right;
			fLeft = fRight;
			rangeLeft = rangeRight;
        }
        if (bracketed) { usedHalfWindow = halfWindow; break; }
    }
    if (!bracketed) return false;
    int iterations = 0;
    double previousRange = std::numeric_limits<double>::infinity();
    for (; iterations < m_options.maxZeroDopplerIterations; ++iterations)
    {
        const double mid = 0.5 * (left + right);
        double fMid = 0.0;
		++dopplerEvaluations;
		if (!evaluateSlaveDoppler(point, mid, fMid, range)) return false;
        // At zero Doppler, d(rho)/dt is zero. Bound the remaining range error
        // directly by the bracket endpoint range changes instead of requiring
        // an unrepresentable sub-nanosecond width in absolute GPS time.
        const double bracketRangeErrorBound = std::max(std::fabs(range - rangeLeft),
                                                       std::fabs(range - rangeRight));
        if (std::fabs(fMid) <= m_options.maxZeroDopplerResidual &&
            std::isfinite(bracketRangeErrorBound) &&
            bracketRangeErrorBound <= m_options.maxSlaveRangeError)
        {
            zeroDopplerTime = mid;
            rhoSlave = range;
            statistics.maxZeroDopplerIterations = std::max(statistics.maxZeroDopplerIterations, iterations + 1);
			statistics.totalZeroDopplerIterations += iterations + 1;
			statistics.totalSlaveDopplerEvaluations += dopplerEvaluations;
			statistics.maxSlaveDopplerEvaluations = std::max(statistics.maxSlaveDopplerEvaluations, dopplerEvaluations);
			statistics.slaveZeroDopplerElapsedMilliseconds +=
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            statistics.maxSlaveZeroDopplerResidual = std::max(statistics.maxSlaveZeroDopplerResidual, std::fabs(fMid));
            closure.slaveRangeLastStep = std::isfinite(previousRange) ? std::fabs(range - previousRange) : 0.0;
            closure.slaveRangeErrorBound = std::max(closure.slaveRangeLastStep, bracketRangeErrorBound);
            statistics.maxSlaveSearchHalfWindowSeconds = std::max(statistics.maxSlaveSearchHalfWindowSeconds, usedHalfWindow);
            statistics.maxSlaveSearchExpansions = std::max(statistics.maxSlaveSearchExpansions, expansion);
            return true;
        }
        previousRange = range;
        if (fLeft * fMid <= 0.0) {
            right = mid;
            fRight = fMid;
            rangeRight = range;
        } else {
            left = mid;
            fLeft = fMid;
            rangeLeft = range;
        }
    }
    return false;
}
