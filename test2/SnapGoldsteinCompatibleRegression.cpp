#include "SnapGoldsteinCompatibleRegression.h"

#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>
#include "opencv2\opencv.hpp"
#include "..\include\Filter.h"
#include "..\include\FormatConversion.h"

namespace {

const char* const kFixtureContract = "snap_goldstein_compatible_fixture_v1";
const char* const kCases[] = { "full_valid_64", "single_valid_64", "all_invalid_64", "edge_origin_65" };

bool readMat(FormatConversion& conversion, const char* fixturePath, const std::string& name, cv::Mat& value)
{
    return conversion.read_array_from_h5(fixturePath, name.c_str(), value) == 0;
}

bool compareMat(const cv::Mat& actual, const cv::Mat& expected, const char* name)
{
    constexpr double kAbsoluteTolerance = 2e-5;
    constexpr double kRelativeTolerance = 2e-5;
    if (actual.type() != expected.type() || actual.size() != expected.size()) {
        fprintf(stderr, "[FAIL] %s: type or grid mismatch\n", name);
        return false;
    }
    for (int row = 0; row < actual.rows; ++row) {
        for (int column = 0; column < actual.cols; ++column) {
            const double a = actual.type() == CV_32F ? actual.at<float>(row, column) : actual.at<double>(row, column);
            const double b = expected.type() == CV_32F ? expected.at<float>(row, column) : expected.at<double>(row, column);
            if (!std::isfinite(a) || !std::isfinite(b) ||
                std::fabs(a - b) > kAbsoluteTolerance + kRelativeTolerance * std::fabs(b)) {
                fprintf(stderr, "[FAIL] %s: sample (%d,%d), actual=%.9g expected=%.9g\n", name, row, column, a, b);
                return false;
            }
        }
    }
    fprintf(stdout, "[PASS] %s\n", name);
    return true;
}

bool compareMask(const cv::Mat& actual, const cv::Mat& expected, const char* name)
{
    const bool equal = actual.type() == CV_8U && expected.type() == CV_8U && actual.size() == expected.size() &&
        cv::countNonZero(actual != expected) == 0;
    fprintf(stdout, "[%s] %s\n", equal ? "PASS" : "FAIL", name);
    return equal;
}

bool runFixtureCase(FormatConversion& conversion, const char* fixturePath, const char* caseName)
{
    const std::string prefix = std::string("case_") + caseName + "_";
    cv::Mat inputI, inputQ, gamma, gammaMask, validMask, expectedStatus;
    if (!readMat(conversion, fixturePath, prefix + "interferogram_i", inputI) ||
        !readMat(conversion, fixturePath, prefix + "interferogram_q", inputQ) ||
        !readMat(conversion, fixturePath, prefix + "complex_gamma", gamma) ||
        !readMat(conversion, fixturePath, prefix + "complex_gamma_valid_mask", gammaMask) ||
        !readMat(conversion, fixturePath, prefix + "phase_valid_mask", validMask) ||
        !readMat(conversion, fixturePath, prefix + "expected_status", expectedStatus) ||
        expectedStatus.type() != CV_32S || expectedStatus.total() != 1) {
        fprintf(stderr, "[FAIL] %s: required frozen SNAP fixture datasets are missing or invalid\n", caseName);
        return false;
    }
    Filter filter;
    cv::Mat outputI, outputQ, support;
    const int actualStatus = filter.Goldstein_filter_snap_compatible(inputI, inputQ, gamma, gammaMask,
                                                                        validMask, outputI, outputQ, support);
    const int expected = expectedStatus.at<int>(0, 0);
    if ((expected == 0) != (actualStatus == 0)) {
        fprintf(stderr, "[FAIL] %s: status actual=%d expected=%d\n", caseName, actualStatus, expected);
        return false;
    }
    if (expected != 0) {
        fprintf(stdout, "[PASS] %s: expected fail-closed status=%d\n", caseName, actualStatus);
        return true;
    }
    cv::Mat expectedI, expectedQ, expectedSupport;
    if (!readMat(conversion, fixturePath, prefix + "expected_filtered_i", expectedI) ||
        !readMat(conversion, fixturePath, prefix + "expected_filtered_q", expectedQ) ||
        !readMat(conversion, fixturePath, prefix + "expected_filter_support_mask", expectedSupport)) {
        fprintf(stderr, "[FAIL] %s: successful fixture lacks frozen expected I/Q or support mask\n", caseName);
        return false;
    }
    return compareMat(outputI, expectedI, (prefix + "filtered_i").c_str()) &&
        compareMat(outputQ, expectedQ, (prefix + "filtered_q").c_str()) &&
        compareMask(support, expectedSupport, (prefix + "filter_support_mask").c_str());
}

} // namespace

int RunSnapGoldsteinCompatibleRegression(const char* fixturePath)
{
    std::ifstream fixtureFile(fixturePath ? fixturePath : "", std::ios::binary);
    if (!fixtureFile.good()) {
        fprintf(stderr, "[FAIL] SNAP fixture is required: %s\n", fixturePath ? fixturePath : "<missing>");
        return 2;
    }
    FormatConversion conversion;
    std::string contract;
    if (conversion.read_str_from_h5(fixturePath, "fixture_contract", contract) != 0 || contract != kFixtureContract) {
        fprintf(stderr, "[FAIL] fixture_contract must equal %s\n", kFixtureContract);
        return 2;
    }
    int failures = 0;
    for (const char* caseName : kCases) failures += runFixtureCase(conversion, fixturePath, caseName) ? 0 : 1;
    fprintf(stdout, "SnapGoldsteinCompatibleRegression: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
