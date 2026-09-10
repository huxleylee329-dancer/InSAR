#include "DimacsValidationRegression.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <limits>

#define NOMINMAX
#include <windows.h>

#include "opencv2\core\core.hpp"
#include "..\include\Utils.h"

namespace
{
struct TemporaryNet
{
	std::string path;

	~TemporaryNet()
	{
		if (!path.empty()) DeleteFileA(path.c_str());
	}
};

bool makeTemporaryNet(TemporaryNet& file)
{
	char directory[MAX_PATH] = {};
	char placeholder[MAX_PATH] = {};
	if (GetTempPathA(MAX_PATH, directory) == 0 ||
		GetTempFileNameA(directory, "dvr", 0, placeholder) == 0)
		return false;
	DeleteFileA(placeholder);
	file.path = std::string(placeholder) + ".net";
	return true;
}

bool hasIntegerNodeSupplies(const std::string& path, bool& foundPositive, bool& foundNegative)
{
	std::ifstream input(path.c_str());
	if (!input) return false;

	foundPositive = false;
	foundNegative = false;
	std::string line;
	while (std::getline(input, line))
	{
		if (line.size() < 2 || line[0] != 'n' || line[1] != ' ') continue;
		std::istringstream tokens(line.substr(2));
		long node = 0;
		std::string supply;
		if (!(tokens >> node >> supply) || supply.empty()) return false;
		if (supply.find_first_of(".eE") != std::string::npos) return false;
		if (supply == "1") foundPositive = true;
		if (supply == "-1") foundNegative = true;
	}
	return foundPositive && foundNegative;
}

int writeCase(Utils& util, cv::Mat& residue, cv::Mat& coherence, const char* name, bool expectedSuccess,
	const std::string& path)
{
	const int result = util.write_DIMACS(path.c_str(), residue, coherence, 0.5);
	const bool success = (result == 0);
	const bool passed = success == expectedSuccess;
	std::fprintf(stdout, "%s: %s (return=%d)\n", name, passed ? "PASS" : "FAIL", result);
	return passed ? 0 : 1;
}
}

int RunDimacsValidationRegression()
{
	Utils util;
	int failures = 0;

	TemporaryNet output;
	if (!makeTemporaryNet(output))
	{
		std::fprintf(stderr, "DIMACS validation regression: unable to create temporary path\n");
		return 1;
	}

	cv::Mat residue(2, 2, CV_64F, cv::Scalar::all(0.0));
	cv::Mat coherence(3, 3, CV_64F, cv::Scalar::all(0.5));
	residue.at<double>(0, 0) = std::nextafter(1.0, 2.0);
	residue.at<double>(0, 1) = std::nextafter(-1.0, -2.0);
	if (writeCase(util, residue, coherence, "near-integer residue accepted", true, output.path) == 0)
	{
		bool foundPositive = false;
		bool foundNegative = false;
		const bool suppliesAreIntegers = hasIntegerNodeSupplies(output.path, foundPositive, foundNegative);
		std::fprintf(stdout, "integer node supply text: %s\n", suppliesAreIntegers ? "PASS" : "FAIL");
		if (!suppliesAreIntegers) ++failures;
	}
	else
		++failures;

	residue.setTo(cv::Scalar::all(0.0));
	residue.at<double>(0, 0) = 0.75;
	if (writeCase(util, residue, coherence, "non-integer residue rejected", false, output.path) != 0) ++failures;

	residue.setTo(cv::Scalar::all(0.0));
	residue.at<double>(0, 0) = std::numeric_limits<double>::quiet_NaN();
	if (writeCase(util, residue, coherence, "NaN residue rejected", false, output.path) != 0) ++failures;

	residue.at<double>(0, 0) = std::numeric_limits<double>::infinity();
	if (writeCase(util, residue, coherence, "infinite residue rejected", false, output.path) != 0) ++failures;

	residue.setTo(cv::Scalar::all(0.0));
	residue.at<double>(0, 0) = 1.0;
	residue.at<double>(0, 1) = -1.0;
	coherence.at<double>(0, 0) = std::numeric_limits<double>::quiet_NaN();
	if (writeCase(util, residue, coherence, "NaN cost rejected", false, output.path) != 0) ++failures;

	coherence.at<double>(0, 0) = std::numeric_limits<double>::infinity();
	if (writeCase(util, residue, coherence, "infinite cost rejected", false, output.path) != 0) ++failures;

	coherence.at<double>(0, 0) = -0.1;
	if (writeCase(util, residue, coherence, "negative cost rejected", false, output.path) != 0) ++failures;

	std::fprintf(stdout, "DIMACS validation regression: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
