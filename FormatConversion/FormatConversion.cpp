#include"pch.h"
#include <direct.h>     // ��� _mkdir �Ҳ�����ʶ������
#include <mutex>        // ��� std::once_flag/call_once ����
#include <io.h>         // ��� _findfirst ����
#include <map>
#include <memory>
#include <limits>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <vector>
#include"gdal_priv.h"   // ��� GDALDataset �� GDAL C++ API ��ʶ��δ��������
#include"..\include\FormatConversion.h"
#include"..\include\Hdf5IO.h"
#include"..\include\Registration.h"
#include"..\include\Utils.h"
#include"..\include\tinyxml.h"
//#include<atlconv.h>
//#include<tchar.h>
#include<urlmon.h>
#pragma comment(lib,"URlmon")
#include <windows.h>


class Hdf5BatchGuard
{
public:
	Hdf5BatchGuard() : lock_(Hdf5IO::acquireBatchLock())
	{
	}
	~Hdf5BatchGuard()
	{
		Hdf5IO::releaseBatchLock(lock_);
	}
	int status() const { return Hdf5IO::getBatchLockStatus(lock_); }

private:
	Hdf5IO::BatchLock* lock_;
};
#define H5_LOCK Hdf5BatchGuard h5_batch_guard; if (h5_batch_guard.status() != 0) return h5_batch_guard.status();

static std::string refinement_manifest_directory(const std::string& path)
{
	const std::string::size_type separator = path.find_last_of("\\/");
	return separator == std::string::npos ? std::string(".") : path.substr(0, separator);
}

static bool refinement_manifest_value(const std::string& json, const char* key, std::string& value)
{
	const std::string prefix = std::string("\"") + key + "\": \"";
	const std::string::size_type start = json.find(prefix);
	if (start == std::string::npos) return false;
	const std::string::size_type valueStart = start + prefix.size();
	const std::string::size_type valueEnd = json.find('"', valueStart);
	if (valueEnd == std::string::npos) return false;
	value.assign(json, valueStart, valueEnd - valueStart);
	return true;
}

static std::string refinement_manifest_json_escape(const std::string& value)
{
	std::string escaped;
	for (char character : value)
	{
		if (character == '\\' || character == '"') escaped.push_back('\\');
		escaped.push_back(character);
	}
	return escaped;
}

static int inspect_refinement_manifest(const vector<string>& outputFiles, SentinelRefinementTransactionStatus& status)
{
	status.state = SENTINEL_REFINEMENT_TRANSACTION_INVALID;
	status.outputCount = static_cast<int>(outputFiles.size());
	status.verifiedOutputCount = 0;
	if (outputFiles.empty()) return -1;
	const std::string directory = refinement_manifest_directory(outputFiles.front());
	const std::string manifestPath = directory + "\\refinement_transaction.json";
	const DWORD manifestAttributes = GetFileAttributesA(manifestPath.c_str());
	if (manifestAttributes == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD manifestError = GetLastError();
		if (manifestError != ERROR_FILE_NOT_FOUND && manifestError != ERROR_PATH_NOT_FOUND) return -1;
		for (const std::string& outputFile : outputFiles)
		{
			const DWORD outputAttributes = GetFileAttributesA(outputFile.c_str());
			// First execution: prepareOutFiles() creates the formal result H5 only
			// after loadOutFiles() has completed.
			if (outputAttributes == INVALID_FILE_ATTRIBUTES)
			{
				const DWORD outputError = GetLastError();
				if (outputError == ERROR_FILE_NOT_FOUND || outputError == ERROR_PATH_NOT_FOUND) continue;
				return -1;
			}
			if ((outputAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return -1;

			int reRows = 0, reColumns = 0, imRows = 0, imColumns = 0;
			if (Hdf5IO::getDatasetDims(outputFile.c_str(), "s_re", &reRows, &reColumns) != 0 ||
				Hdf5IO::getDatasetDims(outputFile.c_str(), "s_im", &imRows, &imColumns) != 0 ||
				reRows < 1 || reColumns < 1 || reRows != imRows || reColumns != imColumns)
				return -1;

			std::string fileState;
			std::string fileTransactionId;
			const int stateResult = Hdf5IO::readString(outputFile.c_str(), "refinement_state", fileState);
			const int idResult = Hdf5IO::readString(outputFile.c_str(), "refinement_transaction_id", fileTransactionId);
			if (stateResult == 0 || idResult == 0) return -1;
		}
		status.state = SENTINEL_REFINEMENT_TRANSACTION_NONE;
		return 0;
	}
	std::ifstream manifest(manifestPath, std::ios::binary);
	if (!manifest) return -1;
	const std::string json((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
	std::string state;
	std::string transactionId;
	if (!refinement_manifest_value(json, "state", state) || !refinement_manifest_value(json, "transactionId", transactionId)) return -1;
	if (state == "complete") status.state = SENTINEL_REFINEMENT_TRANSACTION_COMPLETE;
	else if (state == "in_progress") status.state = SENTINEL_REFINEMENT_TRANSACTION_IN_PROGRESS;
	else if (state == "failed") status.state = SENTINEL_REFINEMENT_TRANSACTION_FAILED;
	else return -1;
	size_t entryCount = 0;
	for (std::string::size_type position = json.find("\"output\""); position != std::string::npos;
		position = json.find("\"output\"", position + 1)) ++entryCount;
	if (entryCount != outputFiles.size()) return -1;
	const bool hasFullBurstTemporary = json.find("\"fullBurstTemporary\"") != std::string::npos;
	const bool hasFullBurstSource = json.find("\"fullBurst\"") != std::string::npos;
	for (const std::string& outputFile : outputFiles)
	{
		if (_stricmp(refinement_manifest_directory(outputFile).c_str(), directory.c_str()) != 0) return -1;
		const std::string temporaryPath = outputFile + ".refinement-" + transactionId + ".tmp";
		const std::string fullBurstPath = outputFile + ".fullburst";
		const std::string fullBurstTemporaryPath = outputFile + ".refinement-" + transactionId + ".fullburst.tmp";
		const std::string backupPath = outputFile + ".refinement-" + transactionId + ".bak";
		if (json.find(std::string("\"output\": \"") + refinement_manifest_json_escape(outputFile) + "\"") == std::string::npos ||
			json.find(std::string("\"temporary\": \"") + refinement_manifest_json_escape(temporaryPath) + "\"") == std::string::npos ||
			json.find(std::string("\"backup\": \"") + refinement_manifest_json_escape(backupPath) + "\"") == std::string::npos ||
			(hasFullBurstSource && json.find(std::string("\"fullBurst\": \"") +
				refinement_manifest_json_escape(fullBurstPath) + "\"") == std::string::npos) ||
			(hasFullBurstTemporary && json.find(std::string("\"fullBurstTemporary\": \"") +
				refinement_manifest_json_escape(fullBurstTemporaryPath) + "\"") == std::string::npos)) return -1;
		if (status.state != SENTINEL_REFINEMENT_TRANSACTION_COMPLETE) continue;
		std::string fileState;
		std::string fileTransactionId;
		if (Hdf5IO::readString(outputFile.c_str(), "refinement_state", fileState) != 0 ||
			Hdf5IO::readString(outputFile.c_str(), "refinement_transaction_id", fileTransactionId) != 0 ||
			fileState != "complete" || fileTransactionId != transactionId) return -1;
		++status.verifiedOutputCount;
	}
	return 0;
}

static int validate_refinement_manifest(const vector<string>& outputFiles)
{
	SentinelRefinementTransactionStatus status = {};
	status.version = SENTINEL_REFINEMENT_TRANSACTION_STATUS_VERSION;
	status.structSize = sizeof(status);
	if (inspect_refinement_manifest(outputFiles, status) != 0) return -1;
	return status.state == SENTINEL_REFINEMENT_TRANSACTION_NONE ||
		status.state == SENTINEL_REFINEMENT_TRANSACTION_COMPLETE ? 0 : -1;
}

static std::mutex g_sentinel_diagnostic_mutex;
struct SentinelBackGeocodingDiagnosticState
{
	std::vector<SentinelBurstQualityStatus> burstStatus;
	std::vector<SentinelZeroDopplerFailureStatistic> zeroDopplerFailureStatistics;
	SentinelZeroDopplerDiagnostic lastZeroDopplerDiagnostic;
	bool hasZeroDopplerDiagnostic;
	bool zeroOffsetFallback;

	SentinelBackGeocodingDiagnosticState() : hasZeroDopplerDiagnostic(false), zeroOffsetFallback(false) {}
};
static std::map<const Sentinel1BackGeocoding*, SentinelBackGeocodingDiagnosticState> g_sentinel_diagnostic_state;
static thread_local int g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_NONE;
static thread_local SentinelZeroDopplerDiagnostic g_thread_zero_doppler_diagnostic;
static thread_local bool g_has_thread_zero_doppler_diagnostic = false;

enum RgAzProjectionFailureReason
{
	RGAZ_PROJECTION_NONE = 0,
	RGAZ_PROJECTION_INVALID_INPUT = 1,
	RGAZ_PROJECTION_SLANT_RANGE = 2,
	RGAZ_PROJECTION_RANGE_OUT_OF_BOUNDS = 3,
	RGAZ_PROJECTION_BURST_OUT_OF_BOUNDS = 4
};

static thread_local int g_rgaz_projection_failure_reason = RGAZ_PROJECTION_NONE;

struct ActiveDiagnosticContext
{
	InSARDiagnosticCallback callback;
	void* userData;
	int imageIndex;
	int burstIndex;

	ActiveDiagnosticContext() : callback(nullptr), userData(nullptr), imageIndex(-1), burstIndex(-1) {}
};

static thread_local ActiveDiagnosticContext g_active_diagnostic_context;

class ScopedDiagnosticContext
{
public:
	ScopedDiagnosticContext(InSARDiagnosticCallback callback, void* userData, int imageIndex = -1, int burstIndex = -1)
		: previous_(g_active_diagnostic_context)
	{
		g_active_diagnostic_context.callback = callback ? callback : previous_.callback;
		g_active_diagnostic_context.userData = callback ? userData : previous_.userData;
		g_active_diagnostic_context.imageIndex = imageIndex > 0 ? imageIndex : previous_.imageIndex;
		g_active_diagnostic_context.burstIndex = burstIndex > 0 ? burstIndex : previous_.burstIndex;
	}

	~ScopedDiagnosticContext()
	{
		g_active_diagnostic_context = previous_;
	}

private:
	ActiveDiagnosticContext previous_;
};

static void emit_diagnostic(
	InSARDiagnosticSeverity severity,
	const char* category,
	const char* phase,
	const char* message,
	const char* detail = nullptr,
	const char* h5File = nullptr,
	const char* dataset = nullptr,
	int statusCode = 0,
	int rows = -1,
	int columns = -1,
	int cvType = -1,
	long long elapsedMs = -1)
{
	const ActiveDiagnosticContext context = g_active_diagnostic_context;
	if (!context.callback)
		return;

	InSARDiagnosticEvent event = {};
	event.version = 1;
	event.severity = severity;
	event.category = category;
	event.phase = phase;
	event.message = message;
	event.detail = detail;
	event.h5File = h5File;
	event.dataset = dataset;
	event.imageIndex = context.imageIndex;
	event.burstIndex = context.burstIndex;
	event.statusCode = statusCode;
	event.rows = rows;
	event.columns = columns;
	event.cvType = cvType;
	event.elapsedMs = elapsedMs;
	try
	{
		context.callback(&event, context.userData);
	}
	catch (...)
	{
		// Native diagnostics must never alter processing control flow.
	}
}


static void emit_progress_for_context(const ActiveDiagnosticContext& context, int percent, const char* message)
{
    if (!context.callback || percent < 0 || percent > 100) return;
    InSARDiagnosticEvent event = {};
    event.version = 1;
    event.severity = INSAR_DIAGNOSTIC_INFO;
    event.category = "progress";
    event.phase = "progress.update";
    event.message = message;
    event.imageIndex = context.imageIndex;
    event.burstIndex = context.burstIndex;
    event.statusCode = percent;
    event.rows = -1;
    event.columns = -1;
    event.cvType = -1;
    event.elapsedMs = -1;
    try
    {
        context.callback(&event, context.userData);
    }
    catch (...)
    {
        // Progress reporting must never alter processing control flow.
    }
}

static void emit_progress(int percent, const char* message)
{
    emit_progress_for_context(g_active_diagnostic_context, percent, message);
}

struct ZeroDopplerFailureAccumulator
{
	int counts[SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT + 1];
	SentinelZeroDopplerDiagnostic diagnostics[SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT + 1];
	bool hasDiagnostic[SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT + 1];

	ZeroDopplerFailureAccumulator()
	{
		memset(counts, 0, sizeof(counts));
		memset(hasDiagnostic, 0, sizeof(hasDiagnostic));
	}

	void add(const SentinelZeroDopplerDiagnostic& diagnostic)
	{
		const int reason = diagnostic.reason;
		if (reason <= SENTINEL_ZERO_DOPPLER_NONE || reason > SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT)
			return;
		++counts[reason];
		if (!hasDiagnostic[reason])
		{
			diagnostics[reason] = diagnostic;
			hasDiagnostic[reason] = true;
		}
	}

	void merge(const ZeroDopplerFailureAccumulator& other)
	{
		for (int reason = SENTINEL_ZERO_DOPPLER_INVALID_INPUT;
			reason <= SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT; ++reason)
		{
			counts[reason] += other.counts[reason];
			if (!hasDiagnostic[reason] && other.hasDiagnostic[reason])
			{
				diagnostics[reason] = other.diagnostics[reason];
				hasDiagnostic[reason] = true;
			}
		}
	}
};

static void record_zero_doppler_diagnostic(const Sentinel1Utils* utils, const Position& groundPosition,
	double targetDoppler, int reason)
{
	SentinelZeroDopplerDiagnostic diagnostic;
	diagnostic.reason = reason;
	diagnostic.returnCode = -1;
	diagnostic.groundPosition = groundPosition;
	diagnostic.targetDoppler = targetDoppler;
	if (utils)
	{
		diagnostic.stateVectorCount = utils->stateVectors ? utils->stateVectors->newStateVectors.rows : 0;
		if (utils->stateVectors && !utils->stateVectors->newStateVectors.empty())
		{
			diagnostic.orbitStartTime = utils->stateVectors->newStateVectors.at<double>(0, 0);
			diagnostic.orbitStopTime = utils->stateVectors->newStateVectors.at<double>(diagnostic.stateVectorCount - 1, 0);
			diagnostic.nearestOrbitTime = (diagnostic.orbitStartTime + diagnostic.orbitStopTime) * 0.5;
		}
		strncpy_s(diagnostic.scene, utils->h5File.c_str(), _TRUNCATE);
		strncpy_s(diagnostic.swath, utils->swath.c_str(), _TRUNCATE);
		strncpy_s(diagnostic.polarization, utils->polarization.c_str(), _TRUNCATE);
	}
	g_thread_zero_doppler_diagnostic = diagnostic;
	g_has_thread_zero_doppler_diagnostic = true;
}

static int validate_sentinel_metadata(const Sentinel1Utils* utils)
{
	if (!utils || utils->h5File.empty() || utils->burstCount <= 0 || utils->linesPerBurst <= 0 ||
		utils->samplesPerBurst <= 0 || !std::isfinite(utils->azimuthTimeInterval) ||
		utils->azimuthTimeInterval <= 0.0 || !utils->stateVectors ||
		utils->burstAzimuthTime.rows < utils->burstCount || utils->burstAzimuthTime.type() != CV_64F ||
		utils->firstValidLine.rows < utils->burstCount || utils->lastValidLine.rows < utils->burstCount ||
		utils->firstValidLine.type() != CV_32S || utils->lastValidLine.type() != CV_32S ||
		utils->stateVectors->newStateVectors.rows < 2 || utils->stateVectors->newStateVectors.cols < 7)
	{
		return -1;
	}
	for (int i = 0; i < utils->burstCount; i++)
	{
		double burstTime = utils->burstAzimuthTime.at<double>(i, 0);
		int firstValidLine = utils->firstValidLine.at<int>(i, 0);
		int lastValidLine = utils->lastValidLine.at<int>(i, 0);
		if (!std::isfinite(burstTime) || firstValidLine < 1 || lastValidLine < firstValidLine ||
			lastValidLine > utils->linesPerBurst ||
			(i > 0 && burstTime <= utils->burstAzimuthTime.at<double>(i - 1, 0)))
		{
			return -1;
		}
	}
	for (int i = 0; i < utils->stateVectors->newStateVectors.rows; i++)
	{
		if (!std::isfinite(utils->stateVectors->newStateVectors.at<double>(i, 0)) ||
			(i > 0 && utils->stateVectors->newStateVectors.at<double>(i, 0) <=
			utils->stateVectors->newStateVectors.at<double>(i - 1, 0)))
		{
			return -1;
		}
	}
	return 0;
}

static void record_burst_quality(const Sentinel1BackGeocoding* backGeocoding,
	const SentinelBurstQualityStatus& status)
{
	std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
	std::vector<SentinelBurstQualityStatus>& statuses = g_sentinel_diagnostic_state[backGeocoding].burstStatus;
	for (size_t i = 0; i < statuses.size(); i++)
	{
		if (statuses[i].imageIndex == status.imageIndex && statuses[i].burstIndex == status.burstIndex)
		{
			statuses[i] = status;
			return;
		}
	}
	statuses.push_back(status);
}

static void clear_zero_doppler_failure_statistics(const Sentinel1BackGeocoding* backGeocoding,
	int imageIndex, int burstIndex)
{
	std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
	std::vector<SentinelZeroDopplerFailureStatistic>& statistics =
		g_sentinel_diagnostic_state[backGeocoding].zeroDopplerFailureStatistics;
	statistics.erase(std::remove_if(statistics.begin(), statistics.end(),
		[imageIndex, burstIndex](const SentinelZeroDopplerFailureStatistic& statistic)
		{
			return statistic.imageIndex == imageIndex && statistic.burstIndex == burstIndex;
		}), statistics.end());
}

static void record_zero_doppler_failure(const Sentinel1BackGeocoding* backGeocoding,
	const SentinelZeroDopplerDiagnostic& diagnostic, int count = 1)
{
	std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
	SentinelBackGeocodingDiagnosticState& state = g_sentinel_diagnostic_state[backGeocoding];
	state.lastZeroDopplerDiagnostic = diagnostic;
	state.hasZeroDopplerDiagnostic = true;
	for (size_t i = 0; i < state.zeroDopplerFailureStatistics.size(); ++i)
	{
		SentinelZeroDopplerFailureStatistic& statistic = state.zeroDopplerFailureStatistics[i];
		if (statistic.imageIndex == diagnostic.imageIndex && statistic.burstIndex == diagnostic.burstIndex &&
			statistic.reason == diagnostic.reason && statistic.callPath == diagnostic.callPath &&
			statistic.returnCode == diagnostic.returnCode)
		{
			statistic.count += count;
			return;
		}
	}
	SentinelZeroDopplerFailureStatistic statistic;
	statistic.imageIndex = diagnostic.imageIndex;
	statistic.burstIndex = diagnostic.burstIndex;
	statistic.reason = diagnostic.reason;
	statistic.callPath = diagnostic.callPath;
	statistic.returnCode = diagnostic.returnCode;
	statistic.count = count;
	state.zeroDopplerFailureStatistics.push_back(statistic);
}

static bool collect_zero_doppler_failure(ZeroDopplerFailureAccumulator& accumulator,
	int imageIndex, int burstIndex, int line, int sample, int callPath,
	const Position& groundPosition)
{
	if (g_zero_doppler_failure_reason == SENTINEL_ZERO_DOPPLER_NONE)
		return false;
	SentinelZeroDopplerDiagnostic diagnostic;
	if (g_has_thread_zero_doppler_diagnostic)
		diagnostic = g_thread_zero_doppler_diagnostic;
	diagnostic.reason = g_zero_doppler_failure_reason;
	diagnostic.returnCode = -1;
	diagnostic.callPath = callPath;
	diagnostic.imageIndex = imageIndex;
	diagnostic.burstIndex = burstIndex;
	diagnostic.line = line;
	diagnostic.sample = sample;
	diagnostic.groundPosition = groundPosition;
	accumulator.add(diagnostic);
	return true;
}

#ifdef _WIN32
  #define timegm _mkgmtime
#endif

static std::string gps2utc(double gps_time) {
	double total_unix_time = gps_time + 315964809.0;
	time_t unix_time = (time_t)floor(total_unix_time);
	double subsec = total_unix_time - (double)unix_time;

	long long subsec_us = (long long)floor(subsec * 1000000.0 + 0.5);
	if (subsec_us >= 1000000LL) {
		unix_time += 1;
		subsec_us -= 1000000LL;
	}
	if (subsec_us < 0) {
		subsec_us = 0;
	}

	tm TM;
#ifdef _WIN32
	gmtime_s(&TM, &unix_time);
#else
	gmtime_r(&unix_time, &TM);
#endif

	char buf[64];
	sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d.%06lld",
			TM.tm_year + 1900, TM.tm_mon + 1, TM.tm_mday,
			TM.tm_hour, TM.tm_min, TM.tm_sec, subsec_us);
	return std::string(buf);
}

static std::once_flag g_gdal_init_flag;
static void InitializeGDALOnce()
{
	std::call_once(g_gdal_init_flag, [](){
		GDALAllRegister();
	});
}

/**
 * @brief ����5�׷����ɾ�������TerraSAR-X��Sentinel-1����ת����
 * ����ṹ��[1, x, x2, x3, x?, y, xy, x2y, x3y, x?y, y2, xy2, x2y2, x3y2, y3, xy3, x2y3, y?, xy?, y?]
 * @param row               ���������У��ѹ�һ����
 * @param col               ���������У��ѹ�һ����
 * @param vandermondeMatrix ����ķ����ɾ���N��25��
 * @return �ɹ�����0��ʧ�ܷ���-1
 */
static int createVandermondeMatrix(const Mat& row, const Mat& col, Mat& vandermondeMatrix)
{
	if (row.empty() || col.empty() || row.rows != col.rows)
	{
		fprintf(stderr, "createVandermondeMatrix(): input check failed!\n");
		return -1;
	}

	int n = row.rows;
	vandermondeMatrix = Mat::ones(n, 25, CV_64F);
	Mat temp;

	// ��������ԭʼ˫�Ĵζ���ʽ���Ӧ���£�
	// 0=1, 1=x, 2=x2, 3=x3, 4=x?
	// 5=y, 6=yx, 7=yx2, 8=yx3, 9=yx?
	// 10=y2, 11=y2x, 12=y2x2, 13=y2x3, 14=y2x?
	// 15=y3, 16=y3x, 17=y3x2, 18=y3x3, 19=y3x?
	// 20=y?, 21=y?x, 22=y?x2, 23=y?x3, 24=y?x?

	// x ���ݴΣ�x, x2, x3, x?(�� 1 �� 4)
	row.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(1, 2)));
	temp = row.mul(row);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(2, 3)));
	temp = temp.mul(row);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(3, 4)));
	temp = temp.mul(row);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(4, 5)));

	// y * x^j (j=0..4) (�� 5 �� 9)
	col.copyTo(temp);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(5, 6)));
	for (int j = 1; j <= 4; j++) {
		temp = temp.mul(row);
		temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(5 + j, 6 + j)));
	}

	// y2 * x^j (j=0..4) (�� 10 �� 14)
	col.copyTo(temp);
	temp = temp.mul(col);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(10, 11)));
	for (int j = 1; j <= 4; j++) {
		temp = temp.mul(row);
		temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(10 + j, 11 + j)));
	}

	// y3 * x^j (j=0..4) (�� 15 �� 19)
	col.copyTo(temp);
	temp = temp.mul(col);
	temp = temp.mul(col);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(15, 16)));
	for (int j = 1; j <= 4; j++) {
		temp = temp.mul(row);
		temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(15 + j, 16 + j)));
	}

	// y? * x^j (j=0..4) (�� 20 �� 24)
	col.copyTo(temp);
	temp = temp.mul(col);
	temp = temp.mul(col);
	temp = temp.mul(col);
	temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(20, 21)));
	for (int j = 1; j <= 4; j++) {
		temp = temp.mul(row);
		temp.copyTo(vandermondeMatrix(cv::Range(0, n), cv::Range(20 + j, 21 + j)));
	}

	return 0;
}

/**
 * @brief ����ʽ��ϣ���С���˷���
 * @param A           �����ɾ���
 * @param b           Ŀ��ֵ����
 * @param coefficient �����ϵ������
 * @param rms         �����RMS����ѡ����nullptr�򲻼��㣩
 * @return �ɹ�����0��ʧ�ܷ���-1
 */
static int polyFit(const Mat& A, const Mat& b, Mat& coefficient, double* rms = nullptr)
{
	if (A.empty() || b.empty() || A.rows != b.rows)
	{
		fprintf(stderr, "polyFit(): input check failed!\n");
		return -1;
	}

	Mat At, AtA, Atb;
	cv::transpose(A, At);
	AtA = At * A;
	Atb = At * b;

	if (!cv::solve(AtA, Atb, coefficient, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "polyFit(): solve failed!\n");
		return -1;
	}

	// ���� RMS
	if (rms != nullptr)
	{
		Mat error, a, a_t, b_t;
		A.copyTo(a);
		cv::transpose(a, a_t);

		*rms = -1.0;
		if (cv::invert(AtA, error, cv::DECOMP_LU) > 0)
		{
			cv::transpose(b, b_t);
			error = b_t * b - (b_t * a) * error * (a_t * b);
			*rms = sqrt(error.at<double>(0, 0) / double(b.rows));
		}
	}

	return 0;
}

#ifdef _DEBUG
#pragma comment(lib,"ComplexMat_d.lib")
#else
#pragma comment(lib,"ComplexMat.lib")
#endif // _DEBUG

namespace
{
	constexpr double INSAR_PI = 3.141592653589793238462643383279502884;

	inline double sinc_func(double x)
	{
		if (std::abs(x) < 1.0e-12) return 1.0;
		double pix = INSAR_PI * x;
		return std::sin(pix) / pix;
	}

	inline double hamming_window(double x, int radius)
	{
		double ax = std::abs(x);
		if (ax > static_cast<double>(radius)) return 0.0;

		// Hamming window: center = 1, edge �� 0.08
		return 0.54 + 0.46 * std::cos(INSAR_PI * ax / static_cast<double>(radius));
	}

	inline double windowed_sinc_weight(double x, int radius)
	{
		if (std::abs(x) > static_cast<double>(radius)) return 0.0;
		return sinc_func(x) * hamming_window(x, radius);
	}

	inline double mat_get_as_double(const cv::Mat& img, int r, int c)
	{
		switch (img.depth())
		{
		case CV_64F:
			return img.at<double>(r, c);
		case CV_32F:
			return static_cast<double>(img.at<float>(r, c));
		case CV_16S:
			return static_cast<double>(img.at<short>(r, c));
		default:
			return 0.0;
		}
	}

	inline void mat_set_from_double(cv::Mat& img, int r, int c, double v)
	{
		switch (img.depth())
		{
		case CV_64F:
			img.at<double>(r, c) = v;
			break;
		case CV_32F:
			img.at<float>(r, c) = static_cast<float>(v);
			break;
		case CV_16S:
			img.at<short>(r, c) = cv::saturate_cast<short>(v);
			break;
		default:
			break;
		}
	}

	inline double sinc_interp2d(const cv::Mat& img, double row, double col, int radius)
	{
		const int rows = img.rows;
		const int cols = img.cols;

		// ������ȫԽ�磬ֱ������
		if (row < 0.0 || col < 0.0 || row > static_cast<double>(rows - 1) || col > static_cast<double>(cols - 1))
		{
			return 0.0;
		}

		// Ϊ����߽紦 sinc �˲���������αӰ��������Ե����Ԫֱ������
		if (row < radius || col < radius ||
			row > static_cast<double>(rows - 1 - radius) ||
			col > static_cast<double>(cols - 1 - radius))
		{
			return 0.0;
		}

		int r0 = static_cast<int>(std::floor(row));
		int c0 = static_cast<int>(std::floor(col));

		double sum_val = 0.0;
		double sum_w = 0.0;

		for (int rr = r0 - radius; rr <= r0 + radius; rr++)
		{
			double wr = windowed_sinc_weight(row - static_cast<double>(rr), radius);
			if (std::abs(wr) < 1.0e-15) continue;

			for (int cc = c0 - radius; cc <= c0 + radius; cc++)
			{
				double wc = windowed_sinc_weight(col - static_cast<double>(cc), radius);
				if (std::abs(wc) < 1.0e-15) continue;

				double w = wr * wc;
				sum_val += mat_get_as_double(img, rr, cc) * w;
				sum_w += w;
			}
		}

		if (std::abs(sum_w) < 1.0e-14) return 0.0;

		// ��һ�����������޴��ضϵ��·���ƫ��
		return sum_val / sum_w;
	}

	inline int readDoubleNode(XMLFile& xmldoc, TiXmlElement* pParent, const char* name, double& val, const char* err_filename)
	{
		TiXmlElement* pchild = NULL;
		int ret = xmldoc._find_node(pParent, name, pchild);
		if (ret < 0)
		{
			fprintf(stderr, "read_POD(): node %s not found!\n", name);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &val);
		if (ret != 1)
		{
			fprintf(stderr, "read_POD(): %s: unknown data format!\n", err_filename);
			return -1;
		}
		return 0;
	}

	inline std::string formatSRTMName(int col, int row)
	{
		char tmp[64];
		sprintf_s(tmp, "srtm_%02d_%02d.zip", col, row);
		return std::string(tmp);
	}

}



inline bool report_progress(ProgressCallback progressCallback, void* userData, int percent, const char* message)
{
	if (progressCallback)
	{
		return progressCallback(percent, message, userData);
	}
	return true;
}

inline float ReverseFloat(const float inFloat)
{
	float retVal;
	unsigned char* floatToConvert = (unsigned char*)&inFloat;
	unsigned char* returnFloat = (unsigned char*)&retVal;

	// swap the bytes into a temporary buffer
	returnFloat[0] = floatToConvert[3];
	returnFloat[1] = floatToConvert[2];
	returnFloat[2] = floatToConvert[1];
	returnFloat[3] = floatToConvert[0];

	return retVal;
}

int UTC2GPS(const char* utc_time, double* gps_time)
{
	if (utc_time == NULL || gps_time == NULL)
	{
		fprintf(stderr, "UTC2GPS(): input check failed!\n");
		return -1;
	}
	int ret, year, month, day, hour, minute, second;
	// removed unused: s (copy-paste remnant, not parsed from UTC format)
	double sec;
	ret = sscanf(utc_time, "%d-%d-%dT%d:%d:%lf\n", &year, &month, &day, &hour, &minute, &sec);
	if (ret != 6)
	{
		fprintf(stderr, "UTC2GPS(): %s: unknown format!\n", utc_time);
		return -1;
	}
	second = int(floor(sec));
	sec = sec - (double)second;
	tm TM;
	TM.tm_year = year - 1900;
	TM.tm_mon = month - 1;
	TM.tm_mday = day;
	TM.tm_hour = hour;
	TM.tm_min = minute;
	TM.tm_sec = second;
	TM.tm_isdst = 0;
	*gps_time = double(timegm(&TM) - 315964809) + sec;
	return 0;
}

FormatConversion::FormatConversion()
{
	memset(this->error_head, 0, 256);
	memset(this->parallel_error_head, 0, 256);
	strcpy(this->error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using ");
	strcpy(this->parallel_error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using parallel computing in function: ");
}

FormatConversion::~FormatConversion()
{

}

int FormatConversion::utc2gps(const char* utc_time, double* gps_time)
{
	if (utc_time == NULL || gps_time == NULL)
	{
		fprintf(stderr, "utc2gps(): input check failed!\n");
		return -1;
	}
	int ret, year, month, day, hour, minute, second;
	// removed unused: s (copy-paste remnant, not parsed from UTC format)
	double sec;
	ret = sscanf(utc_time, "%d-%d-%dT%d:%d:%lf\n", &year, &month, &day, &hour, &minute, &sec);
	if (ret != 6)
	{
		fprintf(stderr, "utc2gps(): %s: unknown format!\n", utc_time);
		return -1;
	}
	second = int(floor(sec));
	sec = sec - (double)second;
	tm TM;
	TM.tm_year = year - 1900;
	TM.tm_mon = month - 1;
	TM.tm_mday = day;
	TM.tm_hour = hour;
	TM.tm_min = minute;
	TM.tm_sec = second;
	TM.tm_isdst = 0;
	*gps_time = double(timegm(&TM) - 315964809) + sec;
	return 0;
}

int FormatConversion::creat_new_h5(const char* filename)
{
	return Hdf5IO::createFile(filename);
}
int FormatConversion::validate_distinct_h5_output(const char* sourceFilename, const char* outputFilename)
{
	return Hdf5IO::validateDistinctFilePaths(sourceFilename, outputFilename);
}
int FormatConversion::get_dataset_dims(const char* filename, const char* dataset_name, int* rows, int* cols)
{
	return Hdf5IO::getDatasetDims(filename, dataset_name, rows, cols);
}
int FormatConversion::write_zero_array_to_h5(const char* filename, const char* dataset_name, int type, int rows, int cols)
{
	return Hdf5IO::createZeroDataset(filename, dataset_name, rows, cols, type);
}
int FormatConversion::create_empty_dataset(const char* filePath, const char* datasetName, int rows, int cols, int dataType, int chunkRows, int chunkCols)
{
	return Hdf5IO::createEmptyDataset(filePath, datasetName, rows, cols, dataType, chunkRows, chunkCols);
}
int FormatConversion::write_array_to_h5(const char* filename, const char* dataset_name, const Mat& input_array)
{
	return Hdf5IO::writeArrayReplace(filename, dataset_name, input_array);
}
int FormatConversion::write_double_to_h5(const char* h5File, const char* datasetName, double data)
{
	return Hdf5IO::writeDouble(h5File, datasetName, data);
}
int FormatConversion::write_int_to_h5(const char* h5File, const char* datasetName, int data)
{
	return Hdf5IO::writeInt(h5File, datasetName, data);
}
int FormatConversion::read_array_from_h5(const char* filename, const char* dataset_name, Mat& out_array)
{
	return Hdf5IO::readArray(filename, dataset_name, out_array);
}
int FormatConversion::read_double_from_h5(const char* h5File, const char* datasetName, double* data)
{
	return Hdf5IO::readDouble(h5File, datasetName, data);
}
int FormatConversion::read_int_from_h5(const char* h5File, const char* datasetName, int* data)
{
	return Hdf5IO::readInt(h5File, datasetName, data);
}
int FormatConversion::read_subarray_from_h5(const char* filename, const char* dataset_name, int offset_row, int offset_col, int rows_subarray, int cols_subarray, Mat& out_array)
{
	return Hdf5IO::readSubarray(filename, dataset_name, offset_row, offset_col, rows_subarray, cols_subarray, out_array);
}
int FormatConversion::write_subarray_to_h5(const char* h5_filename, const char* dataset_name, Mat& subarray, int offset_row, int offset_col, int rows_subarray, int cols_subarray)
{
	if (rows_subarray != subarray.rows || cols_subarray != subarray.cols) return -1;
	return Hdf5IO::writeSubarray(h5_filename, dataset_name, subarray, offset_row, offset_col);
}
int FormatConversion::write_str_to_h5(const char* filename, const char* dataset_name, const char* Str)
{
	if (!filename || !dataset_name || !Str) return -1;
	if (strcmp(dataset_name, "source_1") == 0 || strcmp(dataset_name, "source_2") == 0)
	{
		std::wstring sourcePath;
		PathResolver::Error pathError = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(Str, sourcePath, &pathError)) return -1;
	}
	const int result = Hdf5IO::createString(filename, dataset_name, Str);
	if (result != 0) return result;
	if (strcmp(dataset_name, "source_1") != 0 && strcmp(dataset_name, "source_2") != 0) return 0;
	if (Hdf5IO::writeString(filename, "source_path_encoding", "UTF-8") != 0) return -1;
	return Hdf5IO::writeString(filename, "source_path_format_version", "2");
}
int FormatConversion::read_str_from_h5(const char* filename, const char* dataset_name, string& Str)
{
	return Hdf5IO::readString(filename, dataset_name, Str);
}
int FormatConversion::write_slc_to_h5(const char* filename, const ComplexMat& slc)
{
	if (!filename || slc.isEmpty()) return -1;
	std::unique_ptr<Hdf5IO::WriteSession, void(*)(Hdf5IO::WriteSession*)> session(
		Hdf5IO::openWriteSession(filename), Hdf5IO::closeWriteSession);
	if (!session) return -1;
	if (Hdf5IO::writeArrayReplace(session.get(), "s_re", slc.re) != 0) return -1;
	return Hdf5IO::writeArrayReplace(session.get(), "s_im", slc.im);
}
int FormatConversion::read_slc_from_h5(const char* filename, ComplexMat& slc)
{
	if (!filename) return -1;
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(filename), Hdf5IO::closeReadSession);
	if (!session) return -1;
	if (Hdf5IO::readArray(session.get(), "s_re", slc.re) != 0) return -1;
	return Hdf5IO::readArray(session.get(), "s_im", slc.im);
}
int FormatConversion::read_slc_from_TSXcos(const char* filename, ComplexMat& slc)
{
	if (filename == NULL)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): input check failed!\n");
		return -1;
	}

	InitializeGDALOnce();   /* ע������ */

	GDALDatasetH hDataset = GDALOpen(filename, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): failed to open %s!\n", filename);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand != 1)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): number of Bands != 1\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ��ȡ���� 1 */
	GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): failed to get band 1\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand);
	int ysize = GDALGetRasterBandYSize(hBand);

	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	GDALDataType dataType = GDALGetRasterDataType(hBand);
	if (dataType != GDT_CInt16)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): unexpected data type\n");
		GDALClose(hDataset);
		return -1;
	}

	cv::Mat temp;
	try
	{
		/* �����������Ԥ�����ڴ棨��֤ cv::split ʱ���������ڴ棩 */
		slc.re.create(ysize, xsize, CV_16S);
		slc.im.create(ysize, xsize, CV_16S);

		/* ������ʱ2ͨ������������ȡ������ */
		temp.create(ysize, xsize, CV_16SC2);
	}
	catch (const cv::Exception& e)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): out of memory! (OpenCV exception: %s)\n", e.what());
		GDALClose(hDataset);
		return -1;
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): out of memory!\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ��ȡ���� */
	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		temp.data,
		xsize, ysize,
		GDT_CInt16,
		static_cast<int>(temp.elemSize()),
		static_cast<int>(temp.step[0])) != CE_None)
	{
		fprintf(stderr, "read_slc_from_TSXcos(): RasterIO failed\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ����ͨ���� slc.re �� slc.im */
	cv::Mat channels[2] = { slc.re, slc.im };
	cv::split(temp, channels);

	GDALClose(hDataset);

	return 0;
}

int FormatConversion::TSX2h5(const char* cosar_filename, const char* xml_filename, const char* GEOREF_filename, const char* dst_h5_filename, ProgressCallback progressCallback, void* userData)
{
	H5_LOCK;
	if (cosar_filename == nullptr ||
		xml_filename == nullptr ||
		GEOREF_filename == nullptr ||
		dst_h5_filename == nullptr
		)
	{
		fprintf(stderr, "TSX2h5(): input check failed!\n");
		return -1;
	}
	if (!report_progress(progressCallback, userData, 0, "��ʼ����TerraSAR-X����")) return -2;
	/*
	* ���h5�ļ��Ƿ��Ѿ�����
	*/

	int ret;
	ret = creat_new_h5(dst_h5_filename);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 5, "����H5�ļ����")) return -2;

	/*
	* д��slc����
	*/

	int rows, cols;
	if (!report_progress(progressCallback, userData, 10, "��ȡTerraSAR-X SLC����")) return -2;

	InitializeGDALOnce();   /* ע������ */

	GDALDatasetH hDataset = GDALOpen(cosar_filename, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr, "TSX2h5(): failed to open %s!\n", cosar_filename);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand != 1)
	{
		fprintf(stderr, "TSX2h5(): number of Bands != 1\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ��ȡ���� 1 */
	GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
	if (hBand == NULL)
	{
		fprintf(stderr, "TSX2h5(): failed to get band 1\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand);
	int ysize = GDALGetRasterBandYSize(hBand);

	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "TSX2h5(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	GDALDataType dataType = GDALGetRasterDataType(hBand);
	if (dataType != GDT_CInt16)
	{
		fprintf(stderr, "TSX2h5(): unexpected data type\n");
		GDALClose(hDataset);
		return -1;
	}

	rows = ysize; cols = xsize;

	ret = write_zero_array_to_h5(dst_h5_filename, "s_re", CV_16S, ysize, xsize);
	if (return_check(ret, "write_zero_array_to_h5(s_re)", error_head)) { GDALClose(hDataset); return -1; }
	ret = write_zero_array_to_h5(dst_h5_filename, "s_im", CV_16S, ysize, xsize);
	if (return_check(ret, "write_zero_array_to_h5(s_im)", error_head)) { GDALClose(hDataset); return -1; }

	int block_height = 1024;
	int num_blocks = (ysize + block_height - 1) / block_height;

	for (int i = 0; i < num_blocks; ++i) {
		int current_offset = i * block_height;
		int current_rows = std::min(block_height, ysize - current_offset);

		cv::Mat block_temp;
		cv::Mat block_re;
		cv::Mat block_im;
		try {
			block_temp.create(current_rows, xsize, CV_16SC2);
			block_re.create(current_rows, xsize, CV_16S);
			block_im.create(current_rows, xsize, CV_16S);
		}
		catch (const cv::Exception& e) {
			fprintf(stderr, "TSX2h5(): out of memory in block allocation! (OpenCV exception: %s)\n", e.what());
			GDALClose(hDataset);
			return -1;
		}
		catch (const std::bad_alloc&) {
			fprintf(stderr, "TSX2h5(): out of memory in block allocation!\n");
			GDALClose(hDataset);
			return -1;
		}

		/* ��ȡ��һ������ */
		if (GDALRasterIO(
			hBand,
			GF_Read,
			0, current_offset,
			xsize, current_rows,
			block_temp.data,
			xsize, current_rows,
			GDT_CInt16,
			static_cast<int>(block_temp.elemSize()),
			static_cast<int>(block_temp.step[0])) != CE_None)
		{
			fprintf(stderr, "TSX2h5(): RasterIO failed at offset %d\n", current_offset);
			GDALClose(hDataset);
			return -1;
		}

		/* ����ͨ�� */
		cv::Mat channels[2] = { block_re, block_im };
		cv::split(block_temp, channels);

		/* д����һ�����ݵ� HDF5 */
		ret = write_subarray_to_h5(dst_h5_filename, "s_re", block_re, current_offset, 0, current_rows, xsize);
		if (return_check(ret, "write_subarray_to_h5(s_re)", error_head)) { GDALClose(hDataset); return -1; }
		ret = write_subarray_to_h5(dst_h5_filename, "s_im", block_im, current_offset, 0, current_rows, xsize);
		if (return_check(ret, "write_subarray_to_h5(s_im)", error_head)) { GDALClose(hDataset); return -1; }

		// �ͷŵ�ǰ���ڴ�
		block_temp.release();
		block_re.release();
		block_im.release();

		// �������½��� (10% -> 75% ֮��)
		int progress = 10 + (i + 1) * 65 / num_blocks;
		if (!report_progress(progressCallback, userData, progress, "���ڵ��� TerraSAR-X SLC ���ݷֿ�...")) {
			GDALClose(hDataset);
			return -2; // ֧���û���ֹ
		}
	}
	GDALClose(hDataset);
	if (!report_progress(progressCallback, userData, 75, "д��SLC�������")) return -2;

	/*
	* �� H5 �ļ�����Ԫ����ͳһд��
	*/
	/*
	* д����Ƶ�����
	*/

	std::unique_ptr<Hdf5IO::WriteSession, void(*)(Hdf5IO::WriteSession*)> writeSession(
		Hdf5IO::openWriteSession(dst_h5_filename), Hdf5IO::closeWriteSession);
	if (!writeSession)
	{
		fprintf(stderr, "TSX2h5(): failed to open %s for metadata writing!\n", dst_h5_filename);
		return -1;
	}
	Mat gcps;
	XMLFile xmldoc;
	if (!report_progress(progressCallback, userData, 77, "��ȡTerraSAR-X���Ƶ�����")) return -2;
	ret = xmldoc.XMLFile_load(GEOREF_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	ret = xmldoc.get_gcps_from_TSX(gcps);
	if (return_check(ret, "get_gcps_from_TSX", error_head)) return -1;
	ret = Hdf5IO::writeArray(writeSession.get(), "gcps", gcps);
	if (return_check(ret, "write_array_to_h5", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 80, "д����Ƶ��������")) return -2;

	/*
	* ���ݿ��Ƶ�������Ͼ�γ�ȡ����ӽ����������꣨�С��У�֮��Ķ���ʽ��ϵ
	*/

	double mean_lon, mean_lat, mean_inc, max_lon, max_lat, max_inc, min_lon, min_lat, min_inc;
	Mat lon, lat, inc, row, col;
	gcps(cv::Range(0, gcps.rows), cv::Range(0, 1)).copyTo(lon);
	gcps(cv::Range(0, gcps.rows), cv::Range(1, 2)).copyTo(lat);
	gcps(cv::Range(0, gcps.rows), cv::Range(3, 4)).copyTo(row);
	gcps(cv::Range(0, gcps.rows), cv::Range(4, 5)).copyTo(col);
	gcps(cv::Range(0, gcps.rows), cv::Range(5, 6)).copyTo(inc);
	mean_lon = cv::mean(lon)[0];
	mean_lat = cv::mean(lat)[0];
	mean_inc = cv::mean(inc)[0];
	cv::minMaxLoc(lon, &min_lon, &max_lon);
	cv::minMaxLoc(lat, &min_lat, &max_lat);
	cv::minMaxLoc(inc, &min_inc, &max_inc);
	lon = (lon - mean_lon) / (max_lon - min_lon + 1e-10);
	lat = (lat - mean_lat) / (max_lat - min_lat + 1e-10);
	inc = (inc - mean_inc) / (max_inc - min_inc + 1e-10);
	row = (row - double(rows) * 0.5) / (double(rows) + 1e-10);
	col = (col - double(cols) * 0.5) / (double(cols) + 1e-10);

	if (!report_progress(progressCallback, userData, 82, "���TerraSAR-X����ת��ϵ��")) return -2;
	// ����5�׷����ɾ���
	Mat A, temp, coefficient;
	double rms;
	if (::createVandermondeMatrix(row, col, A) != 0) return -1;

	// ��Ͼ���
	if (::polyFit(A, lon, coefficient, &rms) != 0) return -1;
	temp.create(1, 32, CV_64F);
	temp.at<double>(0, 0) = mean_lon;
	temp.at<double>(0, 1) = max_lon - min_lon + 1e-10;
	temp.at<double>(0, 2) = double(rows) * 0.5;
	temp.at<double>(0, 3) = double(rows) + 1e-10;
	temp.at<double>(0, 4) = double(cols) * 0.5;
	temp.at<double>(0, 5) = double(cols) + 1e-10;
	temp.at<double>(0, 31) = rms;
	cv::transpose(coefficient, coefficient);
	coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
	ret = Hdf5IO::writeArray(writeSession.get(), "lon_coefficient", temp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	// ���γ��
	if (::polyFit(A, lat, coefficient, &rms) != 0) return -1;
	temp.create(1, 32, CV_64F);
	temp.at<double>(0, 0) = mean_lat;
	temp.at<double>(0, 1) = max_lat - min_lat + 1e-10;
	temp.at<double>(0, 2) = double(rows) * 0.5;
	temp.at<double>(0, 3) = double(rows) + 1e-10;
	temp.at<double>(0, 4) = double(cols) * 0.5;
	temp.at<double>(0, 5) = double(cols) + 1e-10;
	temp.at<double>(0, 31) = rms;
	cv::transpose(coefficient, coefficient);
	coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
	ret = Hdf5IO::writeArray(writeSession.get(), "lat_coefficient", temp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	//������ӽ�

	Mat b, B, a, a_t, b_t, error;
	inc.copyTo(b);
	A = Mat::ones(inc.rows, 6, CV_64F);
	col.copyTo(A(cv::Range(0, inc.rows), cv::Range(1, 2)));
	temp = col.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(2, 3)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(3, 4)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(4, 5)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(5, 6)));
	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 11, CV_64F);
		temp.at<double>(0, 0) = mean_inc;
		temp.at<double>(0, 1) = max_inc - min_inc + 1e-10;
		temp.at<double>(0, 2) = double(cols) * 0.5;
		temp.at<double>(0, 3) = double(cols) + 1e-10;
		temp.at<double>(0, 10) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(4, 10)));
		ret = Hdf5IO::writeArray(writeSession.get(), "inc_coefficient", temp);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	}

	//���������

	row.copyTo(b);
	A = Mat::ones(lon.rows, 25, CV_64F);
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	temp = lon.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

	lat.copyTo(temp);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 32, CV_64F);
		temp.at<double>(0, 0) = double(rows) * 0.5;
		temp.at<double>(0, 1) = double(rows) + 1e-10;
		temp.at<double>(0, 2) = mean_lon;
		temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
		temp.at<double>(0, 4) = mean_lat;
		temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
		temp.at<double>(0, 31) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
		ret = Hdf5IO::writeArray(writeSession.get(), "row_coefficient", temp);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	}

	//���������

	col.copyTo(b);
	A = Mat::ones(lon.rows, 25, CV_64F);
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	temp = lon.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

	lat.copyTo(temp);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 32, CV_64F);
		temp.at<double>(0, 0) = double(cols) * 0.5;
		temp.at<double>(0, 1) = double(cols) + 1e-10;
		temp.at<double>(0, 2) = mean_lon;
		temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
		temp.at<double>(0, 4) = mean_lat;
		temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
		temp.at<double>(0, 31) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
		ret = Hdf5IO::writeArray(writeSession.get(), "col_coefficient", temp);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	}

	if (!report_progress(progressCallback, userData, 90, "����ת��ϵ��д�����")) return -2;
	/*
	* д��������
	*/

	Mat stateVec;
	ret = xmldoc.XMLFile_load(xml_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	ret = xmldoc.get_stateVec_from_TSX(stateVec);
	if (return_check(ret, "get_stateVec_from_TSX()", error_head)) return -1;
	ret = Hdf5IO::writeArray(writeSession.get(), "state_vec", stateVec);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	/*
	* д�����������Ƶ�ʲ���
	*/

	Mat Dc;
	ret = xmldoc.get_dopplerCentroid_from_TSX(Dc);
	if (return_check(ret, "get_dopplerCentroid_from_TSX()", error_head)) return -1;
	ret = Hdf5IO::writeArray(writeSession.get(), "doppler_centroid", Dc);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	if (!report_progress(progressCallback, userData, 95, "д��TerraSAR-X����Ͷ����ղ������")) return -2;
	/*
	* д��������������
	*/
	string file_type, sensor, polarization, imaging_mode,
		lookside, orbit_dir, acquisition_start_time, acquisition_stop_time, process_state;

	//��������
	ret = Hdf5IO::createString(writeSession.get(), "file_type", "SLC");
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//��������
	ret = xmldoc.get_str_para("mission", sensor);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "sensor", sensor.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//����
	ret = xmldoc.get_str_para("polLayer", polarization);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "polarization", polarization.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//����ģʽ
	ret = xmldoc.get_str_para("imagingMode", imaging_mode);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "imaging_mode", imaging_mode.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//����
	ret = xmldoc.get_str_para("lookDirection", lookside);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "lookside", lookside.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//�������
	ret = xmldoc.get_str_para("orbitDirection", orbit_dir);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "orbit_dir", orbit_dir.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;

	//����ȼ�
	ret = Hdf5IO::createString(writeSession.get(), "process_state", "InSAR_0");
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//��������
	ret = Hdf5IO::createString(writeSession.get(), "comment", "import from TerraSAR-X Single Look Complex, unprocessed.");
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;

	double carrier_frequency, incidence_center, slant_range_first_pixel,
		slant_range_last_pixel, heading, prf, azimuth_resolution,
		range_resolution, azimuth_spacing, range_spacing;
	// removed unused: orbit_altitude, scene_center_lon, scene_center_lat, scene_topleft_lon,
	//   scene_topleft_lat, scene_bottomleft_lon, scene_bottomleft_lat, scene_topright_lon,
	//   scene_topright_lat, scene_bottomright_lon, scene_bottomright_lat
	//   (scene coordinates not provided by TSX metadata, orbit_altitude hardcoded to -1)
	Mat tmp = Mat::zeros(1, 1, CV_64F);
	//����߶�,TerraSARû�ṩ������Ϊ-1
	tmp.at<double>(0, 0) = -1;
	ret = Hdf5IO::writeArray(writeSession.get(), "orbit_altitude", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	TiXmlElement* pnode, * pchild;

	//������ʼʱ��
	ret = xmldoc.find_node("start", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = xmldoc._find_node(pnode, "timeUTC", pchild);
	if (return_check(ret, "_find_node()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "acquisition_start_time", pchild->GetText());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//�������ʱ��
	ret = xmldoc.find_node("stop", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = xmldoc._find_node(pnode, "timeUTC", pchild);
	if (return_check(ret, "_find_node()", error_head)) return -1;
	ret = Hdf5IO::createString(writeSession.get(), "acquisition_stop_time", pchild->GetText());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//��Ƶ
	ret = xmldoc.find_node("instrument", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = xmldoc._find_node(pnode, "centerFrequency", pchild);
	if (return_check(ret, "_find_node()", error_head)) return -1;
	ret = sscanf(pchild->GetText(), "%lf", &carrier_frequency);
	if (ret != 1)
	{
		fprintf(stderr, "TSX2h5(): carrier frequency not found in %s!\n", xml_filename);
		return -1;
	}
	tmp.at<double>(0, 0) = carrier_frequency;
	ret = Hdf5IO::writeArray(writeSession.get(), "carrier_frequency", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//�������ӽ�
	ret = xmldoc.get_double_para("incidenceAngle", &incidence_center);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = incidence_center;
	ret = Hdf5IO::writeArray(writeSession.get(), "inc_center", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//���б��
	ret = xmldoc.get_double_para("firstPixel", &slant_range_first_pixel);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = slant_range_first_pixel * 299792458.0 / 2;
	ret = Hdf5IO::writeArray(writeSession.get(), "slant_range_first_pixel", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��Զб��
	ret = xmldoc.get_double_para("lastPixel", &slant_range_last_pixel);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = slant_range_last_pixel * 299792458.0 / 2;
	ret = Hdf5IO::writeArray(writeSession.get(), "slant_range_last_pixel", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//headingAngle
	ret = xmldoc.get_double_para("headingAngle", &heading);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = heading;
	ret = Hdf5IO::writeArray(writeSession.get(), "heading", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//�����ظ�Ƶ��
	ret = xmldoc.get_double_para("commonPRF", &prf);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = prf;
	ret = Hdf5IO::writeArray(writeSession.get(), "prf", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��λ��ֱ���
	ret = xmldoc.get_double_para("azimuthResolution", &azimuth_resolution);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = azimuth_resolution;
	ret = Hdf5IO::writeArray(writeSession.get(), "azimuth_resolution", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//������ֱ���
	ret = xmldoc.get_double_para("slantRangeResolution", &range_resolution);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = range_resolution;
	ret = Hdf5IO::writeArray(writeSession.get(), "range_resolution", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��λ��������
	ret = xmldoc.find_node("productSpecific", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = xmldoc._find_node(pnode, "projectedSpacingAzimuth", pchild);
	if (return_check(ret, "_find_node()", error_head)) return -1;
	ret = sscanf(pchild->GetText(), "%lf", &azimuth_spacing);
	if (ret != 1)
	{
		fprintf(stderr, "TSX2h5(): projectedSpacingAzimuth not found in %s!\n", xml_filename);
		return -1;
	}
	tmp.at<double>(0, 0) = azimuth_spacing;
	ret = Hdf5IO::writeArray(writeSession.get(), "azimuth_spacing", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//������������
	ret = xmldoc._find_node(pnode, "commonRSF", pchild);
	if (return_check(ret, "_find_node()", error_head)) return -1;
	ret = sscanf(pchild->GetText(), "%lf", &range_spacing);
	range_spacing = VEL_C / range_spacing / 2.0;
	if (ret != 1)
	{
		fprintf(stderr, "TSX2h5(): groundNear not found in %s!\n", xml_filename);
		return -1;
	}
	tmp.at<double>(0, 0) = range_spacing;
	ret = Hdf5IO::writeArray(writeSession.get(), "range_spacing", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;


	int azimuth_len, range_len;
	Mat tmp_int = Mat::zeros(1, 1, CV_32S);
	//��λ�����ص���
	ret = xmldoc.get_int_para("numberOfRows", &azimuth_len);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	tmp_int.at<int>(0, 0) = azimuth_len;
	ret = Hdf5IO::writeArray(writeSession.get(), "azimuth_len", tmp_int);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//���������ص���
	ret = xmldoc.get_int_para("numberOfColumns", &range_len);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	tmp_int.at<int>(0, 0) = range_len;
	ret = Hdf5IO::writeArray(writeSession.get(), "range_len", tmp_int);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 100, "TerraSAR-X���ݵ������")) return -2;
	return 0;
}

int FormatConversion::TSX2h5(const char* xml_filename, const char* dst_h5_filename, const char* polarization, ProgressCallback progressCallback, void* userData)
{
	H5_LOCK;
	if (xml_filename == nullptr || dst_h5_filename == nullptr)
	{
		fprintf(stderr, "TSX2h5(): input check failed!\n");
		return -1;
	}
	string main_xml(xml_filename);
	std::replace(main_xml.begin(), main_xml.end(), '/', '\\');
	string folder;
	if (main_xml.length() > main_xml.rfind("\\") && main_xml.rfind("\\") >= 0)
	{
		folder = main_xml.substr(0, main_xml.rfind("\\"));
	}
	else if (main_xml.length() > main_xml.rfind("/") && main_xml.rfind("/") >= 0)
	{
		folder = main_xml.substr(0, main_xml.rfind("/"));
	}
	else
	{
		fprintf(stderr, "TSX2h5(): invalid file %s!\n", main_xml.c_str());
		return -1;
	}
	string GEOREF = folder + "\\ANNOTATION\\GEOREF.xml";
	string COSAR = folder + "\\IMAGEDATA\\";
	XMLFile xmldoc;
	int ret = xmldoc.XMLFile_load(xml_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;

	TiXmlElement* pRoot = nullptr, * pnode = nullptr;
	ret = xmldoc.find_node("imageData", pRoot);
	if (return_check(ret, "find_node()", error_head)) return -1;

	bool match_found = false;
	if (polarization != nullptr)
	{
		ret = xmldoc._find_node(pRoot, "polLayer", pnode);
		if (return_check(ret, "_find_node()", error_head)) return -1;
		if (strcmp(pnode->GetText(), polarization) == 0)
		{
			ret = xmldoc._find_node(pRoot, "filename", pnode);
			if (return_check(ret, "_find_node()", error_head)) return -1;
			COSAR = COSAR + pnode->GetText();
			match_found = true;
		}
		else
		{
			pRoot = pRoot->NextSiblingElement();
			if (pRoot)
			{
				if (strcmp("imageData", pRoot->Value()) == 0)
				{
					ret = xmldoc._find_node(pRoot, "filename", pnode);
					if (return_check(ret, "_find_node()", error_head)) return -1;
					COSAR = COSAR + pnode->GetText();
					match_found = true;
				}
			}
		}
	}

	if (!match_found)
	{
		ret = xmldoc.find_node("imageData", pRoot);
		if (return_check(ret, "find_node()", error_head)) return -1;
		ret = xmldoc._find_node(pRoot, "filename", pnode);
		if (return_check(ret, "_find_node()", error_head)) return -1;
		COSAR = COSAR + pnode->GetText();
	}

	ret = TSX2h5(COSAR.c_str(), xml_filename, GEOREF.c_str(), dst_h5_filename, progressCallback, userData);
	if (return_check(ret, "TSX2h5()", error_head)) return -1;
	return 0;
}

int FormatConversion::TSX2h5(const char* xml_filename, const char* dst_h5_filename, ProgressCallback progressCallback, void* userData)
{
	return TSX2h5(xml_filename, dst_h5_filename, nullptr, progressCallback, userData);
}

int FormatConversion::read_POD(const char* POD_filename, double start_time, double stop_time, const char* dst_h5_filename)
{
	if (POD_filename == NULL ||
		dst_h5_filename == NULL ||
		start_time >= stop_time ||
		start_time < 0.0
		)
	{
		fprintf(stderr, "read_POD():input check failed!\n");
		return -1;
	}

	double actual_start = start_time;
	double actual_stop = stop_time;
	if (start_time <= 0.0 || stop_time >= 1e11)
	{
		string start_str, stop_str;
		if (read_str_from_h5(dst_h5_filename, "acquisition_start_time", start_str) == 0 &&
			read_str_from_h5(dst_h5_filename, "acquisition_stop_time", stop_str) == 0)
		{
			utc2gps(start_str.c_str(), &actual_start);
			utc2gps(stop_str.c_str(), &actual_stop);
		}
	}

	/*
	* ��ȡ���ܹ������
	*/

	int ret, numOfstateVec;
	TiXmlElement* pnode, * pchild;
	XMLFile xmldoc;
	ret = xmldoc.XMLFile_load(POD_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	ret = xmldoc.find_node("List_of_OSVs", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfstateVec);
	if (ret != 1)
	{
		fprintf(stderr, "read_POD(): %s: unknown data format!\n", POD_filename);
		return -1;
	}

	Mat tmp = Mat::zeros(numOfstateVec, 7, CV_64F);
	ret = xmldoc.find_node("OSV", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	string str;
	double gps_time, x, y, z, vx, vy, vz;
	bool start = false; bool stop = false;
	int count = 0;
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode || stop) break;
		ret = xmldoc._find_node(pnode, "UTC", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "read_POD(): node UTC not found!\n");
			return -1;
		}
		str = pchild->GetText();
		str = str.substr(4);
		ret = utc2gps(str.c_str(), &gps_time);
		if (return_check(ret, "utc2gps()", error_head)) return -1;
		if (gps_time <= actual_start && fabs(gps_time - actual_start) <= 100.0) start = true;
		if (gps_time >= actual_stop && fabs(gps_time - actual_stop) >= 100.0) stop = true;

		if (start && !stop)//��ʼ��¼
		{
			if (readDoubleNode(xmldoc, pnode, "X", x, POD_filename) < 0) return -1;
			if (readDoubleNode(xmldoc, pnode, "Y", y, POD_filename) < 0) return -1;
			if (readDoubleNode(xmldoc, pnode, "Z", z, POD_filename) < 0) return -1;
			if (readDoubleNode(xmldoc, pnode, "VX", vx, POD_filename) < 0) return -1;
			if (readDoubleNode(xmldoc, pnode, "VY", vy, POD_filename) < 0) return -1;
			if (readDoubleNode(xmldoc, pnode, "VZ", vz, POD_filename) < 0) return -1;

			tmp.at<double>(count, 0) = gps_time;
			tmp.at<double>(count, 1) = x;
			tmp.at<double>(count, 2) = y;
			tmp.at<double>(count, 3) = z;
			tmp.at<double>(count, 4) = vx;
			tmp.at<double>(count, 5) = vy;
			tmp.at<double>(count, 6) = vz;
			count++;
		}

		pnode = pnode->NextSiblingElement();
	}
	Mat stateVec;
	if (count < 1)
	{
		fprintf(stderr, "read_POD(): orbit mismatch! please check if POD file!\n");
		return -1;
	}
	tmp(cv::Range(0, count), cv::Range(0, 7)).copyTo(stateVec);

	/*
	* д�뾫�ܹ������
	*/

	ret = write_array_to_h5(dst_h5_filename, "fine_state_vec", stateVec);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	return 0;
}

int FormatConversion::read_slc_from_Sentinel(
	const char* filename,
	const char* xml_filename,
	ComplexMat& slc,
	Mat& gcps_line,
	ProgressCallback progressCallback,
	void* userData
)
{
	if (filename == NULL ||
		xml_filename == NULL
		)
	{
		fprintf(stderr, "read_slc_from_Sentinel(): input check failed!\n");
		return -1;
	}
	XMLFile xmldoc;
	int ret;
	ret = xmldoc.XMLFile_load(xml_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	TiXmlElement* pnode = NULL;
	int linesPerBurst, samplesPerBurst, burst_count;
	/*
	* ��ȡxml�ļ����burst����
	*/
	ret = xmldoc.get_int_para("linesPerBurst", &linesPerBurst);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	ret = xmldoc.get_int_para("samplesPerBurst", &samplesPerBurst);
	if (return_check(ret, "get_int_para()", error_head)) return -1;

	ret = xmldoc.find_node("burstList", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &burst_count);
	if (ret != 1)
	{
		fprintf(stderr, "read_slc_from_Sentinel(): %s: unknown data format!\n", xml_filename);
		return -1;
	}



	//���burst��ȡ����
	TiXmlElement* pchild = NULL;
	ret = xmldoc._find_node(pnode, "burst", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "read_slc_from_Sentinel(): node 'burst' not found!\n");
		return -1;
	}
	// removed unused: bytesoffset (burst offset handled inside get_a_burst)
	FILE* fp = NULL;
	fopen_s(&fp, filename, "rb");
	if (!fp)
	{
		fprintf(stderr, "read_slc_from_Sentinel(): failed to open %s!\n", filename);
		return -1;
	}
	struct FileGuard {
		FILE*& fp_ref;
		~FileGuard() {
			if (fp_ref) {
				fclose(fp_ref);
				fp_ref = NULL;
			}
		}
	} guard{ fp };
	Mat gcps_merged_line_num = Mat::zeros(1, burst_count + 1, CV_32S);
	ComplexMat last_burst, this_burst;
	ret = get_a_burst(pchild, xmldoc, fp, linesPerBurst, samplesPerBurst, slc);
	if (return_check(ret, "get_a_burst()", error_head)) return -1;
	gcps_merged_line_num.at<int>(0, 1) = slc.GetRows();
	pchild = pchild->NextSiblingElement();
	int overlapSize;
	for (int i = 1; i < burst_count; i++)
	{
		if (!pchild) break;
		ret = get_a_burst(pchild, xmldoc, fp, linesPerBurst, samplesPerBurst, this_burst);
		if (return_check(ret, "get_a_burst()", error_head)) return -1;
		ret = deburst_overlapSize(slc, this_burst, &overlapSize);
		if (return_check(ret, "deburst_overlapSize()", error_head)) return -1;
		ret = burst_stitch(this_burst, slc, overlapSize);
		if (return_check(ret, "burst_stitch()", error_head)) return -1;
		gcps_merged_line_num.at<int>(0, i + 1) = slc.GetRows();
		pchild = pchild->NextSiblingElement();
		if (progressCallback) {
			int progress = 10 + i * 80 / burst_count;
			if (!progressCallback(progress, "���ڶ�ȡ��ƴ�� Sentinel-1 Ӱ���Ƭ...", userData)) {
				return -2;
			}
		}
	}
	gcps_merged_line_num.copyTo(gcps_line);
	return 0;
}

int FormatConversion::sentinel_deburst(const char* xml_filename, ComplexMat& slc, Mat& Sentinel)
{
	if (xml_filename == NULL ||
		slc.isEmpty() ||
		slc.type() != CV_16S
		)
	{
		fprintf(stderr, "sentinel_deburst(): input check failed!\n");
		return -1;
	}
	XMLFile xmldoc;
	int ret;
	ret = xmldoc.XMLFile_load(xml_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	TiXmlElement* pnode = NULL;
	int linesPerBurst, burst_count, invalidLines;
	ret = xmldoc.get_int_para("linesPerBurst", &linesPerBurst);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	ret = xmldoc.find_node("burstList", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &burst_count);
	if (ret != 1)
	{
		fprintf(stderr, "sentinel_deburst(): %s: unknown data format!\n", xml_filename);
		return -1;
	}
	if (slc.GetRows() != (burst_count * linesPerBurst))
	{
		fprintf(stderr, "sentinel_deburst(): %s: input slc size mismatch!\n", xml_filename);
		return -1;
	}
	TiXmlElement* pchild = NULL;
	ret = xmldoc._find_node(pnode, "burst", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "sentinel_deburst(): node 'burst' not found!\n");
		return -1;
	}

	/*
	* �ҵ�������Ч��
	*/
	long firstValidSample;
	char* ptr;
	const char* p;
	invalidLines = 0;
	int count = 0;
	Mat sentinel = Mat::zeros(1, slc.GetRows(), CV_64F);
	for (int i = 0; i < burst_count; i++)
	{
		if (!pchild) break;
		ret = xmldoc._find_node(pchild, "firstValidSample", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "sentinel_deburst(): node 'firstValidSample' not found!\n");
			return -1;
		}
		p = pnode->GetText();
		firstValidSample = strtol(p, &ptr, 0);
		if (firstValidSample < 0)
		{
			invalidLines++;
			sentinel.at<double>(0, count) = -1;
		}
		count++;
		for (int j = 0; j < linesPerBurst - 1; j++)
		{
			firstValidSample = strtol(ptr, &ptr, 0);
			if (firstValidSample < 0)
			{
				invalidLines++;
				sentinel.at<double>(0, count) = -1;
			}
			count++;
		}
		pchild = pchild->NextSiblingElement();
	}
	count = 0;
	ComplexMat tmp; Mat sentinel_accu = Mat::zeros(slc.GetRows() - invalidLines, 1, CV_32S);
	tmp.re.create(slc.GetRows() - invalidLines, slc.GetCols(), CV_16S);
	tmp.im.create(slc.GetRows() - invalidLines, slc.GetCols(), CV_16S);
	int samplesPerLine = slc.GetCols();
	ComplexMat c;
	int invalidLine_accu = 0;
	for (int i = 0; i < slc.GetRows(); i++)
	{
		if (sentinel.at<double>(0, i) > -0.5)
		{
			sentinel_accu.at<int>(count, 0) = invalidLine_accu;
			c = slc(cv::Range(i, i + 1), cv::Range(0, samplesPerLine));
			tmp.SetValue(cv::Range(count, count + 1), cv::Range(0, samplesPerLine), c);
			count++;
		}
		else
		{
			invalidLine_accu++;
		}
	}
	slc = tmp;
	Mat t = Mat::zeros(burst_count, 1, CV_32S);
	count = 0;
	for (int i = 0; i < sentinel_accu.rows - 1; i++)
	{
		if (sentinel_accu.at<int>(i, 0) != sentinel_accu.at<int>(i + 1, 0))
		{
			t.at<int>(count, 0) = sentinel_accu.at<int>(i + 1, 0);
			count++;
		}
	}
	if (invalidLine_accu != sentinel_accu.at<int>(sentinel_accu.rows - 1, 0))
	{
		t.at<int>(count, 0) = invalidLine_accu;
	}
	else
	{
		t.at<int>(count, 0) = t.at<int>(count - 1, 0);
	}
	t.copyTo(Sentinel);
	return 0;
}

int FormatConversion::sentinel2h5(const char* tiff_filename, const char* xml_filename, const char* dst_h5_filename, const char* POD_file)
{
	H5_LOCK;
	return sentinel2h5(tiff_filename, xml_filename, dst_h5_filename, POD_file, static_cast<ProgressCallback>(NULL), static_cast<void*>(NULL));
}

int FormatConversion::sentinel2h5(const char* tiff_filename, const char* xml_filename, const char* dst_h5_filename, const char* POD_file, ProgressCallback progressCallback, void* userData)
{
	H5_LOCK;
	if (tiff_filename == NULL ||
		xml_filename == NULL ||
		dst_h5_filename == NULL
		)
	{
		fprintf(stderr, "sentinel2h5(): input check failed!\n");
		return -1;
	}

	if (!report_progress(progressCallback, userData, 0, "��ʼ����Sentinel-1����")) return -2;
	/*
	* ���h5�ļ��Ƿ��Ѿ�����
	*/

	int ret;
	ret = creat_new_h5(dst_h5_filename);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;

	/*
	* д��slc����
	*/

	ComplexMat slc;Mat gcps_line_index;
	int rows, cols;
	if (!report_progress(progressCallback, userData, 10, "��ȡSentinel-1 SLC����")) return -2;
	ret = read_slc_from_Sentinel(tiff_filename, xml_filename, slc, gcps_line_index, progressCallback, userData);//��Ҫdeburst
	if (ret == -2) return -2;
	if (return_check(ret, "read_slc_from_Sentinel()", error_head)) return -1;
	//ret = sentinel_deburst(xml_filename, slc, sentinel);
	//if (return_check(ret, "sentinel_deburst()", error_head)) return -1;
	rows = slc.GetRows(); cols = slc.GetCols();
	ret = write_array_to_h5(dst_h5_filename, "s_re", slc.re);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5_filename, "s_im", slc.im);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	slc.re.release();
	slc.im.release();

	/*
	* д����Ƶ�����
	*/

	Mat gcps;
	XMLFile xmldoc;
	int linesPerburst;
	ret = xmldoc.XMLFile_load(xml_filename);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	ret = xmldoc.get_gcps_from_sentinel(gcps);
	if (return_check(ret, "get_gcps_from_sentinel()", error_head)) return -1;
	ret = xmldoc.get_int_para("linesPerBurst", &linesPerburst);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	for (int i = 0; i < gcps.rows; i++)
	{
		int temp_r, xx;
		temp_r = (int)gcps.at<double>(i, 3);
		xx = temp_r / linesPerburst;
		if (xx > 0 && xx < gcps_line_index.cols)
		{
			gcps.at<double>(i, 3) = (double)gcps_line_index.at<int>(0, xx);
		}
	}
	ret = write_array_to_h5(dst_h5_filename, "gcps", gcps);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 50, "д����Ƶ��������")) return -2;

	/*
	* ���ݿ��Ƶ�������Ͼ�γ�ȡ����ӽ����������꣨�С��У�֮��Ķ���ʽ��ϵ
	*/

	double mean_lon, mean_lat, mean_inc, max_lon, max_lat, max_inc, min_lon, min_lat, min_inc;
	Mat lon, lat, inc, row, col;
	gcps(cv::Range(0, gcps.rows), cv::Range(0, 1)).copyTo(lon);
	gcps(cv::Range(0, gcps.rows), cv::Range(1, 2)).copyTo(lat);
	gcps(cv::Range(0, gcps.rows), cv::Range(3, 4)).copyTo(row);
	gcps(cv::Range(0, gcps.rows), cv::Range(4, 5)).copyTo(col);
	gcps(cv::Range(0, gcps.rows), cv::Range(5, 6)).copyTo(inc);
	mean_lon = cv::mean(lon)[0];
	mean_lat = cv::mean(lat)[0];
	mean_inc = cv::mean(inc)[0];
	cv::minMaxLoc(lon, &min_lon, &max_lon);
	cv::minMaxLoc(lat, &min_lat, &max_lat);
	cv::minMaxLoc(inc, &min_inc, &max_inc);
	lon = (lon - mean_lon) / (max_lon - min_lon + 1e-10);
	lat = (lat - mean_lat) / (max_lat - min_lat + 1e-10);
	inc = (inc - mean_inc) / (max_inc - min_inc + 1e-10);
	row = (row + 1 - double(rows) * 0.5) / (double(rows) + 1e-10);//sentinel�������Ϊ0��+1ͳһΪ1.
	col = (col + 1 - double(cols) * 0.5) / (double(cols) + 1e-10);

	// ����5�׷����ɾ���
	Mat A, temp, coefficient;
	double rms;
	if (::createVandermondeMatrix(row, col, A) != 0) return -1;

	// ��Ͼ���
	if (::polyFit(A, lon, coefficient, &rms) != 0) return -1;
	temp.create(1, 32, CV_64F);
	temp.at<double>(0, 0) = mean_lon;
	temp.at<double>(0, 1) = max_lon - min_lon + 1e-10;
	temp.at<double>(0, 2) = double(rows) * 0.5;
	temp.at<double>(0, 3) = double(rows) + 1e-10;
	temp.at<double>(0, 4) = double(cols) * 0.5;
	temp.at<double>(0, 5) = double(cols) + 1e-10;
	temp.at<double>(0, 31) = rms;
	cv::transpose(coefficient, coefficient);
	coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
	ret = write_array_to_h5(dst_h5_filename, "lon_coefficient", temp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	// ���γ��
	if (::polyFit(A, lat, coefficient, &rms) != 0) return -1;
	temp.create(1, 32, CV_64F);
	temp.at<double>(0, 0) = mean_lat;
	temp.at<double>(0, 1) = max_lat - min_lat + 1e-10;
	temp.at<double>(0, 2) = double(rows) * 0.5;
	temp.at<double>(0, 3) = double(rows) + 1e-10;
	temp.at<double>(0, 4) = double(cols) * 0.5;
	temp.at<double>(0, 5) = double(cols) + 1e-10;
	temp.at<double>(0, 31) = rms;
	cv::transpose(coefficient, coefficient);
	coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
	ret = write_array_to_h5(dst_h5_filename, "lat_coefficient", temp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	//������ӽ�

	Mat b, B, a, a_t, b_t, error;
	inc.copyTo(b);
	A = Mat::ones(inc.rows, 6, CV_64F);
	col.copyTo(A(cv::Range(0, inc.rows), cv::Range(1, 2)));
	temp = col.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(2, 3)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(3, 4)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(4, 5)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(5, 6)));
	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 11, CV_64F);
		temp.at<double>(0, 0) = mean_inc;
		temp.at<double>(0, 1) = max_inc - min_inc + 1e-10;
		temp.at<double>(0, 2) = double(cols) * 0.5;
		temp.at<double>(0, 3) = double(cols) + 1e-10;
		temp.at<double>(0, 10) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(4, 10)));
		ret = write_array_to_h5(dst_h5_filename, "inc_coefficient", temp);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	}

	//���������

	row.copyTo(b);
	A = Mat::ones(lon.rows, 25, CV_64F);
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	temp = lon.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

	lat.copyTo(temp);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 32, CV_64F);
		temp.at<double>(0, 0) = double(rows) * 0.5;
		temp.at<double>(0, 1) = double(rows) + 1e-10;
		temp.at<double>(0, 2) = mean_lon;
		temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
		temp.at<double>(0, 4) = mean_lat;
		temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
		temp.at<double>(0, 31) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
		ret = write_array_to_h5(dst_h5_filename, "row_coefficient", temp);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	}

	//���������

	col.copyTo(b);
	A = Mat::ones(lon.rows, 25, CV_64F);
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	temp = lon.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

	lat.copyTo(temp);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 32, CV_64F);
		temp.at<double>(0, 0) = double(cols) * 0.5;
		temp.at<double>(0, 1) = double(cols) + 1e-10;
		temp.at<double>(0, 2) = mean_lon;
		temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
		temp.at<double>(0, 4) = mean_lat;
		temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
		temp.at<double>(0, 31) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
		ret = write_array_to_h5(dst_h5_filename, "col_coefficient", temp);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	}

	/*
	* д��������
	*/

	Mat stateVec;
	ret = xmldoc.get_stateVec_from_sentinel(stateVec);
	if (return_check(ret, "get_stateVec_from_sentinel()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5_filename, "state_vec", stateVec);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;



	/*
	* д�����������Ƶ������
	*/

	Mat Dc;
	ret = xmldoc.get_dopplerCentroid_from_sentinel(Dc);
	if (return_check(ret, "get_dopplerCentroid_from_sentinel()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5_filename, "doppler_centroid", Dc);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	if (!report_progress(progressCallback, userData, 90, "д��Sentinel-1����Ͷ����ղ������")) return -2;
	/*
	* ������������
	*/

	string file_type, sensor, polarization, imaging_mode,
		lookside, orbit_dir, acquisition_start_time, acquisition_stop_time, process_state;

	//��������
	ret = xmldoc.get_str_para("productType", file_type);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "file_type", file_type.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//��������
	ret = xmldoc.get_str_para("missionId", sensor);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "sensor", sensor.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//����
	ret = xmldoc.get_str_para("polarisation", polarization);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "polarization", polarization.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//����ģʽ
	ret = xmldoc.get_str_para("mode", imaging_mode);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "imaging_mode", imaging_mode.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//����
	ret = write_str_to_h5(dst_h5_filename, "lookside", "RIGHT");
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//�������
	ret = xmldoc.get_str_para("pass", orbit_dir);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "orbit_dir", orbit_dir.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//������ʼʱ��
	ret = xmldoc.get_str_para("startTime", acquisition_start_time);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "acquisition_start_time", acquisition_start_time.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//�������ʱ��
	ret = xmldoc.get_str_para("stopTime", acquisition_stop_time);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = write_str_to_h5(dst_h5_filename, "acquisition_stop_time", acquisition_stop_time.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;

	//���ܹ������
	if (POD_file)
	{
		double start_t, end_t;
		utc2gps(acquisition_start_time.c_str(), &start_t);
		utc2gps(acquisition_stop_time.c_str(), &end_t);
		read_POD(POD_file, start_t, end_t, dst_h5_filename);
	}

	//����ȼ�
	ret = write_str_to_h5(dst_h5_filename, "process_state", "InSAR_0");
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	//��������
	ret = write_str_to_h5(dst_h5_filename, "comment", "import from sentinel1 Single Look Complex, unprocessed.");
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;

	double carrier_frequency, incidence_center, slant_range_first_pixel,
		heading, prf, azimuth_spacing, range_spacing;
	// removed unused: orbit_altitude (hardcoded to -1), slant_range_last_pixel (hardcoded to -1),
	//   azimuth_resolution (hardcoded to 20), range_resolution (hardcoded to 5),
	//   scene_center_lon, scene_center_lat, scene_topleft_lon, scene_topleft_lat,
	//   scene_bottomleft_lon, scene_bottomleft_lat, scene_topright_lon, scene_topright_lat,
	//   scene_bottomright_lon, scene_bottomright_lat
	//   (scene coordinates not provided by sentinel1 metadata)
	Mat tmp = Mat::zeros(1, 1, CV_64F);

	//����߶�,sentinelû�ṩ������Ϊ-1
	tmp.at<double>(0, 0) = -1;
	ret = write_array_to_h5(dst_h5_filename, "orbit_altitude", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��Ƶ
	ret = xmldoc.get_double_para("radarFrequency", &carrier_frequency);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = carrier_frequency;
	ret = write_array_to_h5(dst_h5_filename, "carrier_frequency", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//�������ӽ�
	ret = xmldoc.get_double_para("incidenceAngleMidSwath", &incidence_center);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = incidence_center;
	ret = write_array_to_h5(dst_h5_filename, "inc_center", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//���б��
	ret = xmldoc.get_double_para("slantRangeTime", &slant_range_first_pixel);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = slant_range_first_pixel * VEL_C / 2.0;
	ret = write_array_to_h5(dst_h5_filename, "slant_range_first_pixel", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��Զб�࣬sentinelδ�ṩ������Ϊ-1
	tmp.at<double>(0, 0) = -1;
	ret = write_array_to_h5(dst_h5_filename, "slant_range_last_pixel", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//heading
	ret = xmldoc.get_double_para("platformHeading", &heading);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = heading;
	ret = write_array_to_h5(dst_h5_filename, "heading", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//�����ظ�Ƶ��
	ret = xmldoc.get_double_para("prf", &prf);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = prf;
	ret = write_array_to_h5(dst_h5_filename, "prf", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��λ��ֱ���
	tmp.at<double>(0, 0) = 20;
	ret = write_array_to_h5(dst_h5_filename, "azimuth_resolution", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//������ֱ���
	tmp.at<double>(0, 0) = 5;
	ret = write_array_to_h5(dst_h5_filename, "range_resolution", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//��λ��������
	ret = xmldoc.get_double_para("azimuthPixelSpacing", &azimuth_spacing);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = azimuth_spacing;
	ret = write_array_to_h5(dst_h5_filename, "azimuth_spacing", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//������������
	ret = xmldoc.get_double_para("rangePixelSpacing", &range_spacing);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	tmp.at<double>(0, 0) = range_spacing;
	ret = write_array_to_h5(dst_h5_filename, "range_spacing", tmp);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;


	// removed unused: azimuth_len, range_len (rows/cols written directly to h5)
	Mat tmp_int = Mat::zeros(1, 1, CV_32S);
	//��λ�����ص���
	tmp_int.at<int>(0, 0) = rows;
	ret = write_array_to_h5(dst_h5_filename, "azimuth_len", tmp_int);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	//���������ص���
	tmp_int.at<int>(0, 0) = cols;
	ret = write_array_to_h5(dst_h5_filename, "range_len", tmp_int);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 100, "Sentinel-1���ݵ������")) return -2;

	return 0;
}

int FormatConversion::import_sentinel(
	const char* manifest,
	const char* subswath_name,
	const char* polarization,
	const char* dest_h5_file,
	const char* PODFile,
	int start_burst,
	int end_burst
)
{
	H5_LOCK;
	return import_sentinel(manifest, subswath_name, polarization, dest_h5_file, PODFile, static_cast<ProgressCallback>(NULL), static_cast<void*>(NULL), start_burst, end_burst);
}

int FormatConversion::import_sentinel(
	const char* manifest,
	const char* subswath_name,
	const char* polarization,
	const char* dest_h5_file,
	const char* PODFile,
	ProgressCallback progressCallback,
	void* userData,
	int start_burst,
	int end_burst
)
{
	H5_LOCK;
	if (manifest == NULL ||
		subswath_name == NULL ||
		polarization == NULL ||
		dest_h5_file == NULL)
	{
		fprintf(stderr, "import_sentinel(): input check failed!\n");
		return -1;
	}
	if (!report_progress(progressCallback, userData, 0, "��ʼ����Sentinel-1��Ʒ")) return -2;
	int ret;
	string xmlhead, tiffhead, subswath, polar;
	if (0 == strcmp("iw2", subswath_name))subswath = "iw2";
	else if (0 == strcmp("iw3", subswath_name))subswath = "iw3";
	else subswath = "iw1";

	if (0 == strcmp("vh", polarization)) polar = "vh";
	else polar = "vv";

	XMLFile xmldoc;
	if (!report_progress(progressCallback, userData, 10, "��ȡSentinel-1 manifest�ļ�")) return -2;
	ret = xmldoc.XMLFile_load(manifest);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	TiXmlElement* root = NULL, * pnode = NULL;
	ret = xmldoc.find_node("dataObjectSection", root);
	if (return_check(ret, "find_node()", error_head)) return -1;

	ret = xmldoc._find_node(root, "dataObject", pnode);
	if (return_check(ret, "_find_node()", error_head)) return -1;
	string tmp(pnode->FirstAttribute()->Value());
	if (0 == strcmp(tmp.substr(0, 10).c_str(), "products1a"))//sentinel-1A
	{
		xmlhead = "products1a" + subswath + "slc" + polar;
		tiffhead = "s1a" + subswath + "slc" + polar;
	}
	else if (0 == strcmp(tmp.substr(0, 10).c_str(), "products1b"))//sentinel-1B
	{
		xmlhead = "products1b" + subswath + "slc" + polar;
		tiffhead = "s1b" + subswath + "slc" + polar;
	}
	else if (0 == strcmp(tmp.substr(0, 10).c_str(), "products1c"))//sentinel-1C
	{
		xmlhead = "products1c" + subswath + "slc" + polar;
		tiffhead = "s1c" + subswath + "slc" + polar;
	}
	else//sentinel-1D
	{
		xmlhead = "products1d" + subswath + "slc" + polar;
		tiffhead = "s1d" + subswath + "slc" + polar;
	}

	string tiff_filename, xml_filename;
	string main_xml(manifest);
	string folder;
	if (main_xml.length() > main_xml.rfind("\\") && main_xml.rfind("\\") >= 0)
	{
		folder = main_xml.substr(0, main_xml.rfind("\\"));
	}
	else if (main_xml.length() > main_xml.rfind("/") && main_xml.rfind("/") >= 0)
	{
		folder = main_xml.substr(0, main_xml.rfind("/"));
	}
	else
	{
		fprintf(stderr, "import_sentinel(): invalid manifest file %s !\n", manifest);
		return -1;
	}
	TiXmlElement* pchild = NULL;
	if (!report_progress(progressCallback, userData, 30, "����Sentinel-1 XML��TIFF�ļ�")) return -2;

	while (pnode)
	{
		tmp = pnode->FirstAttribute()->Value();
		if (tmp.length() > 18)
		{
			if (0 == strcmp(tmp.substr(0, 18).c_str(), xmlhead.c_str()))
			{
				ret = xmldoc._find_node(pnode, "fileLocation", pchild);
				if (return_check(ret, "_find_node()", error_head)) return -1;
				xml_filename = pchild->Attribute("href");
				xml_filename = folder + xml_filename.substr(1, xml_filename.length() - 1);
			}
		}
		if (tmp.length() > 11)
		{
			if (0 == strcmp(tmp.substr(0, 11).c_str(), tiffhead.c_str()))
			{
				ret = xmldoc._find_node(pnode, "fileLocation", pchild);
				if (return_check(ret, "_find_node()", error_head)) return -1;
				tiff_filename = pchild->Attribute("href");
				tiff_filename = folder + tiff_filename.substr(1, tiff_filename.length() - 1);
			}
		}
		pnode = pnode->NextSiblingElement();
	}
	if (!report_progress(progressCallback, userData, 50, "д��Sentinel-1 H5�ļ�")) return -2;
	Sentinel1Reader reader(xml_filename.c_str(), tiff_filename.c_str(), PODFile);
	ret = reader.writeToh5(dest_h5_file, start_burst, end_burst, progressCallback, userData);
	if (return_check(ret, "writeToh5()", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 100, "Sentinel-1��Ʒ�������")) return -2;
	return 0;
}

int FormatConversion::get_a_burst(
	TiXmlElement* pnode,
	XMLFile& xmldoc,
	FILE*& fp,
	int linesPerBurst,
	int samplesPerBurst,
	ComplexMat& burst
)
{
	if (pnode == NULL ||
		linesPerBurst < 1 ||
		samplesPerBurst < 1 ||
		fp == NULL
		)
	{
		fprintf(stderr, "get_a_burst(): input check failed!\n");
		if (fp) {
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	int ret;
	TiXmlElement* pchild = NULL;
	size_t bytesoffset;
	INT16* buf = NULL;
	buf = (INT16*)malloc(linesPerBurst * samplesPerBurst * 2 * sizeof(INT16));
	ret = xmldoc._find_node(pnode, "byteOffset", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "get_a_burst(): node 'byteOffset' not found!\n");
		if (buf) free(buf);
		if (fp)
		{
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	ret = sscanf(pchild->GetText(), "%lld", &bytesoffset);
	if (ret != 1)
	{
		fprintf(stderr, "get_a_burst(): unknown data format!\n");
		if (buf) free(buf);
		if (fp)
		{
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	fseek(fp, static_cast<long>(bytesoffset), SEEK_SET);
	fread(buf, sizeof(INT16), linesPerBurst * samplesPerBurst * 2, fp);
	size_t offset = 0;
	burst.re.create(linesPerBurst, samplesPerBurst, CV_16S);
	burst.im.create(linesPerBurst, samplesPerBurst, CV_16S);
	for (int j = 0; j < linesPerBurst; j++)
	{
		for (int k = 0; k < samplesPerBurst; k++)
		{
			burst.re.at<short>(j, k) = buf[offset];
			offset++;
			burst.im.at<short>(j, k) = buf[offset];
			offset++;
		}
	}
	if (buf) free(buf);

	//�޳���Ч����
	ret = xmldoc._find_node(pnode, "firstValidSample", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "get_a_burst(): node 'firstValidSample' not found!\n");
		if (fp)
		{
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	long firstValidSample;
	char* ptr;
	const char* p;
	int invalidLines = 0;
	int count = 0;
	Mat sentinel = Mat::ones(1, linesPerBurst, CV_64F);
	p = pchild->GetText();
	firstValidSample = strtol(p, &ptr, 0);
	if (firstValidSample < 0)
	{
		invalidLines++;
		sentinel.at<double>(0, count) = -1;
	}
	count++;
	for (int j = 0; j < linesPerBurst - 1; j++)
	{
		firstValidSample = strtol(ptr, &ptr, 0);
		if (firstValidSample < 0)
		{
			invalidLines++;
			sentinel.at<double>(0, count) = -1;
		}
		count++;
	}
	int start, end;
	if (sentinel.at<double>(0, 0) > 0.0)
	{
		start = 0;
	}
	else
	{
		for (int i = 1; i < linesPerBurst; i++)
		{
			if (sentinel.at<double>(0, i - 1) * sentinel.at<double>(0, i) < 0.0)
			{
				start = i; break;
			}
		}
	}
	end = linesPerBurst - (invalidLines - start);
	burst = burst(cv::Range(start, end), cv::Range(0, samplesPerBurst));
	return 0;
}

int FormatConversion::get_burst_sentinel(
	int burst_num,
	const char* xml_file,
	const char* tiff_file,
	ComplexMat& burst,
	int* overlapSize
)
{
	if (burst_num < 1 ||
		xml_file == NULL ||
		tiff_file == NULL ||
		overlapSize == NULL)
	{
		fprintf(stderr, "get_burst_sentinel():  input check failed!\n");
		return -1;
	}


	XMLFile xmldoc;
	int ret;
	ret = xmldoc.XMLFile_load(xml_file);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	TiXmlElement* pnode = NULL;
	int linesPerBurst, samplesPerBurst, burst_count;
	double azimuthTimeInterval;
	/*
	* ��ȡxml�ļ����burst����
	*/
	ret = xmldoc.get_int_para("linesPerBurst", &linesPerBurst);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	ret = xmldoc.get_int_para("samplesPerBurst", &samplesPerBurst);
	if (return_check(ret, "get_int_para()", error_head)) return -1;
	ret = xmldoc.get_double_para("azimuthTimeInterval", &azimuthTimeInterval);
	if (return_check(ret, "get_double_para()", error_head)) return -1;

	ret = xmldoc.find_node("burstList", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &burst_count);
	if (ret != 1)
	{
		fprintf(stderr, "get_burst_sentinel(): %s: unknown data format!\n", xml_file);
		return -1;
	}
	if (burst_num > burst_count)
	{
		fprintf(stderr, "get_burst_sentinel(): burst_num out of range!\n");
		return -1;
	}


	//��ȡburst����
	TiXmlElement* pchild = NULL, * pchild2 = NULL;
	int count = 1;
	ret = xmldoc._find_node(pnode, "burst", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "get_burst_sentinel(): node 'burst' not found!\n");
		return -1;
	}
	while (pchild && count != burst_num)
	{
		pchild = pchild->NextSiblingElement();
		count++;
	}


	size_t bytesoffset;
	double azimuthAnxTime, azimuthAnxTime2;
	ret = xmldoc._find_node(pchild, "azimuthAnxTime", pchild2);
	if (ret < 0)
	{
		fprintf(stderr, "get_burst_sentinel(): node 'azimuthAnxTime' not found!\n");
		return -1;
	}
	ret = sscanf(pchild2->GetText(), "%lf", &azimuthAnxTime);
	if (ret != 1)
	{
		fprintf(stderr, "get_burst_sentinel(): unknown data format!\n");
		return -1;
	}
	FILE* fp = NULL;
	fopen_s(&fp, tiff_file, "rb");
	if (!fp)
	{
		fprintf(stderr, "get_burst_sentinel(): failed to open %s!\n", tiff_file);
		return -1;
	}

	short* buf = NULL;
	buf = (short*)malloc(linesPerBurst * samplesPerBurst * 2 * sizeof(short));
	if (!buf)
	{
		fprintf(stderr, "get_burst_sentinel(): out of memory!\n");
		if (fp)
		{
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	ret = xmldoc._find_node(pchild, "byteOffset", pchild2);
	if (ret < 0)
	{
		fprintf(stderr, "get_burst_sentinel(): node 'byteOffset' not found!\n");
		if (buf) free(buf);
		if (fp)
		{
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	ret = sscanf(pchild2->GetText(), "%lld", &bytesoffset);
	if (ret != 1)
	{
		fprintf(stderr, "get_burst_sentinel(): unknown data format!\n");
		if (buf) free(buf);
		if (fp)
		{
			fclose(fp); fp = NULL;
		}
		return -1;
	}
	fseek(fp, static_cast<long>(bytesoffset), SEEK_SET);
	fread(buf, sizeof(short), linesPerBurst * samplesPerBurst * 2, fp);
	if (fp)
	{
		fclose(fp); fp = NULL;
	}
	size_t offset = 0;
	burst.re.create(linesPerBurst, samplesPerBurst, CV_16S);
	burst.im.create(linesPerBurst, samplesPerBurst, CV_16S);
	for (int j = 0; j < linesPerBurst; j++)
	{
		for (int k = 0; k < samplesPerBurst; k++)
		{
			burst.re.at<short>(j, k) = buf[offset];
			offset++;
			burst.im.at<short>(j, k) = buf[offset];
			offset++;
		}
	}
	if (buf)
	{
		free(buf); buf = NULL;
	}

	//�޳���Ч����
	ret = xmldoc._find_node(pchild, "firstValidSample", pchild2);
	if (ret < 0)
	{
		fprintf(stderr, "get_burst_sentinel(): node 'firstValidSample' not found!\n");
		return -1;
	}
	long firstValidSample;
	char* ptr;
	const char* p;
	int invalidLines = 0;
	count = 0;
	Mat sentinel = Mat::ones(1, linesPerBurst, CV_64F);
	p = pchild2->GetText();
	firstValidSample = strtol(p, &ptr, 0);
	if (firstValidSample < 0)
	{
		invalidLines++;
		sentinel.at<double>(0, count) = -1;
	}
	count++;
	for (int j = 0; j < linesPerBurst - 1; j++)
	{
		firstValidSample = strtol(ptr, &ptr, 0);
		if (firstValidSample < 0)
		{
			invalidLines++;
			sentinel.at<double>(0, count) = -1;
		}
		count++;
	}
	int start, end;
	if (sentinel.at<double>(0, 0) > 0.0)
	{
		start = 0;
	}
	else
	{
		for (int i = 1; i < linesPerBurst; i++)
		{
			if (sentinel.at<double>(0, i - 1) * sentinel.at<double>(0, i) < 0.0)
			{
				start = i; break;
			}
		}
	}
	end = linesPerBurst - (invalidLines - start);
	burst = burst(cv::Range(start, end), cv::Range(0, samplesPerBurst));

	//��ȡoverlapSize
	if (!pchild->NextSiblingElement()) *overlapSize = -1;//-1��ʾ���һ��burst
	else
	{
		pchild = pchild->NextSiblingElement();
		ret = xmldoc._find_node(pchild, "azimuthAnxTime", pchild2);
		if (ret < 0)
		{
			fprintf(stderr, "get_burst_sentinel(): node 'azimuthAnxTime' not found!\n");
			return -1;
		}
		ret = sscanf(pchild2->GetText(), "%lf", &azimuthAnxTime2);
		if (ret != 1)
		{
			fprintf(stderr, "get_burst_sentinel(): unknown data format!\n");
			return -1;
		}
		int temp = static_cast<int>(std::round((azimuthAnxTime2 - azimuthAnxTime) / azimuthTimeInterval));
		*overlapSize = linesPerBurst - invalidLines - temp;
	}
	return 0;
}

int FormatConversion::deburst_overlapSize(ComplexMat& last_burst, ComplexMat& this_burst, int* overlapSize)
{
	if (last_burst.isEmpty() ||
		this_burst.isEmpty() ||
		last_burst.GetCols() != this_burst.GetCols() ||
		overlapSize == NULL
		)
	{
		fprintf(stderr, "deburst_overlapSize(): input check failed!\n");
		return -1;
	}

	int nr, nc, nr2, nc2, match_wnd_rows, match_wnd_cols, col_start, col_end, offset_row, offset_col, ret;
	ComplexMat match_wnd, match_wnd2;
	nr = last_burst.GetRows(); nc = last_burst.GetCols(); nr2 = this_burst.GetRows(); nc2 = this_burst.GetCols();
	match_wnd_cols = 1000;
	match_wnd_rows = (nr < nr2 ? nr : nr2) / 3;
	col_start = nc / 2 - match_wnd_cols;
	col_start = col_start < 0 ? 0 : col_start;
	col_end = col_start + match_wnd_cols;
	col_end = col_end > nc ? nc : col_end;
	match_wnd_cols = col_end - col_start;
	//match_wnd_rows = 600;
	match_wnd = last_burst(cv::Range(nr - match_wnd_rows, nr), cv::Range(0, nc));
	match_wnd2 = this_burst(cv::Range(0, match_wnd_rows), cv::Range(0, nc));
	match_wnd.convertTo(match_wnd, CV_64F);
	match_wnd2.convertTo(match_wnd2, CV_64F);


	//��ȡƫ����
	//ComplexMat t1, t2;
	//regis.interp_paddingzero(match_wnd, t1, 8);
	//regis.interp_paddingzero(match_wnd2, t2, 8);
	//Utils util;
	//util.saveSLC("E:/working_dir/projects/software/InSAR/bin/match_wnd.jpg", 65, last_burst);
	//util.saveSLC("E:/working_dir/projects/software/InSAR/bin/match_wnd2.jpg", 65, this_burst);
	ret = real_coherent(match_wnd, match_wnd2, &offset_row, &offset_col);
	if (return_check(ret, "real_coherent()", error_head)) return -1;

	//ˮƽ����this_burst

	//if (offset_col > 0)
	//{
	//	ComplexMat tmp_burst, tmp_burst2;
	//	tmp_burst.re = Mat::zeros(this_burst.GetRows(), this_burst.GetCols(), CV_16S);
	//	tmp_burst.im = Mat::zeros(this_burst.GetRows(), this_burst.GetCols(), CV_16S);
	//	tmp_burst2 = this_burst(cv::Range(0, nr2), cv::Range(0, nc - offset_col));
	//	tmp_burst.SetValue(cv::Range(0, nr2), cv::Range(offset_col, nc), tmp_burst2);
	//	this_burst = tmp_burst;
	//}
	//if (offset_col < 0)
	//{
	//	ComplexMat tmp_burst, tmp_burst2;
	//	tmp_burst.re = Mat::zeros(this_burst.GetRows(), this_burst.GetCols(), CV_16S);
	//	tmp_burst.im = Mat::zeros(this_burst.GetRows(), this_burst.GetCols(), CV_16S);
	//	tmp_burst2 = this_burst(cv::Range(0, nr2), cv::Range(-offset_col, nc));
	//	tmp_burst.SetValue(cv::Range(0, nr2), cv::Range(0, nc + offset_col), tmp_burst2);
	//	this_burst = tmp_burst;
	//}

	*overlapSize = (offset_row > 0 ? offset_row : -offset_row);

	return 0;
}

int FormatConversion::burst_stitch(
	ComplexMat& src_burst,
	ComplexMat& dst_burst,
	int overlapSize,
	const char* stitch_type
)
{
	if (src_burst.isEmpty() ||
		dst_burst.isEmpty() ||
		src_burst.GetCols() != dst_burst.GetCols() ||
		overlapSize < 0||
		overlapSize > dst_burst.GetRows()||
		overlapSize > src_burst.GetRows() ||
		src_burst.type() != CV_16S||
		dst_burst.type() != CV_16S
		)
	{
		fprintf(stderr, "burst_stitch(): input check failed!\n");
		return -1;
	}

	//stitch

	int nr, nc, nr2, nc2;
	nr = dst_burst.GetRows(); nc = dst_burst.GetCols(); nr2 = src_burst.GetRows(); nc2 = nc;
	Mat src_lowerpart_real, src_lowerpart_imag;
	if (strcmp(stitch_type, "low") == 0)
	{
		if (overlapSize < nr2)
		{
			src_burst.re(cv::Range(overlapSize, nr2), cv::Range(0, nc)).copyTo(src_lowerpart_real);
			src_burst.im(cv::Range(overlapSize, nr2), cv::Range(0, nc)).copyTo(src_lowerpart_imag);
			cv::vconcat(dst_burst.re, src_lowerpart_real, dst_burst.re);
			cv::vconcat(dst_burst.im, src_lowerpart_imag, dst_burst.im);
		}
	}
	else if (strcmp(stitch_type, "mid") == 0)
	{
		int mid = overlapSize / 2;
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < mid; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				dst_burst.re.at<short>(nr - mid + i, j) = src_burst.re.at<short>(i + overlapSize - mid, j);
				dst_burst.im.at<short>(nr - mid + i, j) = src_burst.im.at<short>(i + overlapSize - mid, j);
			}
		}
		if (mid < nr2)
		{
			src_burst.re(cv::Range(overlapSize, nr2), cv::Range(0, nc)).copyTo(src_lowerpart_real);
			src_burst.im(cv::Range(overlapSize, nr2), cv::Range(0, nc)).copyTo(src_lowerpart_imag);
			cv::vconcat(dst_burst.re, src_lowerpart_real, dst_burst.re);
			cv::vconcat(dst_burst.im, src_lowerpart_imag, dst_burst.im);
		}

	}
	else
	{
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < overlapSize; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				dst_burst.re.at<short>(nr - overlapSize + i, j) = src_burst.re.at<short>(i, j);
				dst_burst.im.at<short>(nr - overlapSize + i, j) = src_burst.im.at<short>(i, j);
			}
		}
		if (overlapSize < nr2)
		{
			src_burst.re(cv::Range(overlapSize, nr2), cv::Range(0, nc)).copyTo(src_lowerpart_real);
			src_burst.im(cv::Range(overlapSize, nr2), cv::Range(0, nc)).copyTo(src_lowerpart_imag);
			cv::vconcat(dst_burst.re, src_lowerpart_real, dst_burst.re);
			cv::vconcat(dst_burst.im, src_lowerpart_imag, dst_burst.im);
		}

	}
	return 0;
}

int FormatConversion::read_slc_from_ALOS(const char* img_file, ComplexMat& slc)
{
	if (img_file == NULL)
	{
		fprintf(stderr, "read_slc_from_ALOS(): input check failed!\n");
		return -1;
	}
	FILE* fp = NULL;
	fopen_s(&fp, img_file, "rb");
	char buf[2048];
	memset(buf, 0, 2048);
	if (!fp)
	{
		fprintf(stderr, "read_slc_from_ALOS(): failed to open %s!\n", img_file);
		return -1;
	}
	int rows, cols, sarfd_record_length, record_length, sardata_offset;
	//������ݸ�ʽ
	fseek(fp, 9 - 1, SEEK_SET);
	if (fread(&sarfd_record_length, 4, 1, fp) != 1)
	{
		fprintf(stderr, "read_slc_from_ALOS(): %s: unknown format!\n", img_file);
		if (fp) fclose(fp);
		return -1;
	}
	sarfd_record_length = Big2Little32(sarfd_record_length);
	if (sarfd_record_length != 720)
	{
		fprintf(stderr, "read_slc_from_ALOS(): %s: unknown format!\n", img_file);
		if (fp) fclose(fp);
		return -1;
	}
	char* ptr = NULL;
	//ȷ��slc���ݳߴ�
	fseek(fp, 237 - 1, SEEK_SET);
	fread(buf, 1, 8, fp);
	rows = strtol(buf, &ptr, 0);
	fseek(fp, sarfd_record_length + 9 - 1, SEEK_SET);
	fread(&record_length, 4, 1, fp);
	record_length = Big2Little32(record_length);
	fseek(fp, sarfd_record_length + 25 - 1, SEEK_SET);
	fread(&cols, 4, 1, fp);
	cols = Big2Little32(cols);
	//��ȡ����ƫ����
	memset(buf, 0, 2048);
	fseek(fp, 277 - 1, SEEK_SET);
	fread(buf, 1, 4, fp);
	sardata_offset = strtol(buf, &ptr, 0);
	//��ȡ����
	slc.re.create(rows, cols, CV_32F);
	slc.im.create(rows, cols, CV_32F);
	float* p = (float*)malloc(record_length);
	if (!p)
	{
		fprintf(stderr, "read_slc_from_ALOS(): out of memory!\n");
		if (fp) fclose(fp);
		return -1;
	}
	fseek(fp, sarfd_record_length + sardata_offset, SEEK_SET);
	for (int i = 0; i < rows; i++)
	{
		fread(p, 1, record_length, fp);
		for (int j = 0; j < cols; j++)
		{
			slc.re.at<float>(i, j) = ReverseFloat(p[2 * j]);
			slc.im.at<float>(i, j) = ReverseFloat(p[2 * j + 1]);
		}
	}
	if (p) free(p);
	if (fp) fclose(fp);
	return 0;
}

int FormatConversion::read_stateVec_from_ALOS(const char* LED_file, Mat& stateVec)
{
	if (LED_file == NULL)
	{
		fprintf(stderr, "read_stateVec_from_ALOS(): input check failed!\n");
		return -1;
	}

	//����ļ���ʽ
	int File_descriptor_len = 720;
	int Data_set_summary_len = 4096;
	int num_stateVec = 28;
	int tmp;
	FILE* fp = NULL;
	fopen_s(&fp, LED_file, "rb");
	if (!fp)
	{
		fprintf(stderr, "read_stateVec_from_ALOS(): failed to open %s!\n", LED_file);
		return -1;
	}
	fseek(fp, File_descriptor_len + Data_set_summary_len , SEEK_SET);
	fread(&tmp, 4, 1, fp);
	if (Big2Little32(tmp) != 3)
	{
		fprintf(stderr, "read_stateVec_from_ALOS():  %s : unknown format!\n", LED_file);
		if (fp) fclose(fp);
		return -1;
	}

	stateVec.create(num_stateVec, 7, CV_64F);

	//����GPSʱ��

	char str[10240];
	memset(str, 0, 10240);
	string year, month, day, hour, minute, second;
	hour = "0"; minute = "0"; second = "0.0";
	fseek(fp, File_descriptor_len + Data_set_summary_len + 145 - 1, SEEK_SET);
	fread(str, 1, 4, fp);
	year = str;
	memset(str, 0, 10240);
	fread(str, 1, 4, fp);
	month = str + 2;
	memset(str, 0, 10240);
	fread(str, 1, 4, fp);
	day = str + 2;
	string time = year + "-" + month + "-" + day + "T" + hour + ":" + minute + ":" + second;
	double gps_time_start0, gps_time_start, time_interval; char* ptr;
	if (return_check(utc2gps(time.c_str(), &gps_time_start0), "utc2gps()", error_head))
	{
		if (fp) fclose(fp);
		return -1;
	}
	fseek(fp, File_descriptor_len + Data_set_summary_len + 161 - 1, SEEK_SET);
	memset(str, 0, 10240);
	fread(str, 1, 22, fp);
	gps_time_start = strtod(str, &ptr);
	gps_time_start = gps_time_start + gps_time_start0;
	memset(str, 0, 10240);
	fread(str, 1, 22, fp);
	time_interval = strtod(str, &ptr);
	fseek(fp, File_descriptor_len + Data_set_summary_len + 387 - 1, SEEK_SET);
	memset(str, 0, 10240);
	fread(str, 1, 22 * 6 * num_stateVec, fp);
	if (fp)fclose(fp);
	ptr = str;
	for (int i = 0; i < num_stateVec; i++)
	{
		stateVec.at<double>(i, 0) = gps_time_start + i * time_interval;
		stateVec.at<double>(i, 1) = strtod(ptr, &ptr);
		stateVec.at<double>(i, 2) = strtod(ptr, &ptr);
		stateVec.at<double>(i, 3) = strtod(ptr, &ptr);
		stateVec.at<double>(i, 4) = strtod(ptr, &ptr);
		stateVec.at<double>(i, 5) = strtod(ptr, &ptr);
		stateVec.at<double>(i, 6) = strtod(ptr, &ptr);
	}
	return 0;
}

int FormatConversion::read_conversion_coefficient_from_ALOS(const char* LED_file, Mat& lon_coefficient, Mat& lat_coefficient, Mat& row_coefficient, Mat& col_coefficient)
{
	if (LED_file == NULL)
	{
		fprintf(stderr, "read_conversion_coefficient_from_ALOS(): input check failed!\n");
		return -1;
	}
	//����ļ�

	// removed unused: ret (no return-value checks in this function)
	int record_len, data_set_summary_len, platform_pos_len,
		attitue_data_len, radiometric_data_len, facility_related_record_len, offset;
	Mat tmp = Mat::zeros(1, 32, CV_64F);
	tmp.copyTo(lon_coefficient);
	tmp.copyTo(lat_coefficient);
	tmp.copyTo(row_coefficient);
	tmp.copyTo(col_coefficient);
	char* ptr; char str[20480];
	memset(str, 0, 20480);
	FILE* fp = NULL;
	fopen_s(&fp, LED_file, "rb");
	if (!fp)
	{
		fprintf(stderr, "read_conversion_coefficient_from_ALOS(): failed to open %s!\n", LED_file);
		return -1;
	}

	fseek(fp, 9 - 1, SEEK_SET);
	fread(&record_len, 4, 1, fp);
	record_len = Big2Little32(record_len);
	if (record_len != 720)
	{
		fprintf(stderr, "read_conversion_coefficient_from_ALOS(): %s: unknown format!\n", LED_file);
		if (fp) fclose(fp);
		return -1;
	}

	//����ƫ����

	fseek(fp, 187 - 1, SEEK_SET);
	fread(str, 1, 6, fp);
	data_set_summary_len = strtol(str, &ptr, 0);
	memset(str, 0, 20480);
	fseek(fp, 211 - 1, SEEK_SET);
	fread(str, 1, 6, fp);
	platform_pos_len = strtol(str, &ptr, 0);
	memset(str, 0, 20480);
	fseek(fp, 223 - 1, SEEK_SET);
	fread(str, 1, 6, fp);
	attitue_data_len = strtol(str, &ptr, 0);
	memset(str, 0, 20480);
	fseek(fp, 235 - 1, SEEK_SET);
	fread(str, 1, 6, fp);
	radiometric_data_len = strtol(str, &ptr, 0);
	facility_related_record_len = 0;
	offset = record_len + data_set_summary_len + platform_pos_len + attitue_data_len + radiometric_data_len;

	//��ȡ����
	int count0 = 0;
	while (facility_related_record_len != 5000/*ALOS2ֹͣ����*/ && count0 < 11/*ALOS1ֹͣ����*/)
	{
		offset += facility_related_record_len;
		fseek(fp, offset + 9 - 1, SEEK_SET);
		fread(&facility_related_record_len, 4, 1, fp);
		facility_related_record_len = Big2Little32(facility_related_record_len);
		count0++;
	}
	if (count0 == 11)offset += facility_related_record_len;
	fseek(fp, offset + 1025 - 1, SEEK_SET);
	memset(str, 0, 20480);
	fread(str, 1, 1040 * 2, fp);
	if (fp)fclose(fp);
	ptr = str;
	for (int i = 0; i < 25; i++)
	{
		lat_coefficient.at<double>(0, 30 - i) = strtod(ptr, &ptr);
	}
	for (int i = 0; i < 25; i++)
	{
		lon_coefficient.at<double>(0, 30 - i) = strtod(ptr, &ptr);
	}
	lat_coefficient.at<double>(0, 0) = 0;
	lat_coefficient.at<double>(0, 1) = 1.0;
	lat_coefficient.at<double>(0, 4) = strtod(ptr, &ptr);
	lat_coefficient.at<double>(0, 3) = 1.0;
	lat_coefficient.at<double>(0, 2) = strtod(ptr, &ptr);
	lat_coefficient.at<double>(0, 5) = 1.0;

	lon_coefficient.at<double>(0, 0) = 0;
	lon_coefficient.at<double>(0, 1) = 1.0;
	lon_coefficient.at<double>(0, 2) = lat_coefficient.at<double>(0, 2);
	lon_coefficient.at<double>(0, 3) = 1.0;
	lon_coefficient.at<double>(0, 4) = lat_coefficient.at<double>(0, 4);
	lon_coefficient.at<double>(0, 5) = 1.0;

	for (int i = 0; i < 25; i++)
	{
		col_coefficient.at<double>(0, 30 - i) = strtod(ptr, &ptr);
	}
	for (int i = 0; i < 25; i++)
	{
		row_coefficient.at<double>(0, 30 - i) = strtod(ptr, &ptr);
	}
	col_coefficient.at<double>(0, 0) = 0;
	col_coefficient.at<double>(0, 1) = 1.0;
	col_coefficient.at<double>(0, 4) = strtod(ptr, &ptr);
	col_coefficient.at<double>(0, 3) = 1.0;
	col_coefficient.at<double>(0, 2) = strtod(ptr, &ptr);
	col_coefficient.at<double>(0, 5) = 1.0;

	row_coefficient.at<double>(0, 0) = 0;
	row_coefficient.at<double>(0, 1) = 1.0;
	row_coefficient.at<double>(0, 2) = col_coefficient.at<double>(0, 2);
	row_coefficient.at<double>(0, 3) = 1.0;
	row_coefficient.at<double>(0, 4) = col_coefficient.at<double>(0, 4);
	row_coefficient.at<double>(0, 5) = 1.0;

	return 0;
}

int FormatConversion::ALOS2h5(const char* IMG_file, const char* LED_file, const char* dst_h5)
{
	H5_LOCK;
	return ALOS2h5(IMG_file, LED_file, dst_h5, static_cast<ProgressCallback>(NULL), static_cast<void*>(NULL));
}

int FormatConversion::ALOS2h5(const char* IMG_file, const char* LED_file, const char* dst_h5, ProgressCallback progressCallback, void* userData)
{
	H5_LOCK;
	if (IMG_file == NULL ||
		LED_file == NULL ||
		dst_h5 == NULL)
	{
		fprintf(stderr, "ALOS2h5(): input check failed!\n");
		return -1;
	}

	if (!report_progress(progressCallback, userData, 0, "��ʼ����ALOS����")) return -2;
	///////////////////////����h5�ļ�////////////

	int ret;
	if (return_check(creat_new_h5(dst_h5), "creat_new_h5()", error_head)) return -1;
	if (!report_progress(progressCallback, userData, 5, "����H5�ļ����")) return -2;

	//////////////��ȡslc���ݲ�д�뵽Ŀ���ļ���/////////////

	int rows = 0, cols = 0;
	if (!report_progress(progressCallback, userData, 10, "��ȡALOS SLC����")) return -2;

	FILE* fp_img = NULL;
	fopen_s(&fp_img, IMG_file, "rb");
	if (!fp_img)
	{
		fprintf(stderr, "ALOS2h5(): failed to open %s!\n", IMG_file);
		return -1;
	}

	char buf[2048];
	memset(buf, 0, 2048);
	int sarfd_record_length = 0, record_length = 0, sardata_offset = 0;

	fseek(fp_img, 9 - 1, SEEK_SET);
	if (fread(&sarfd_record_length, 4, 1, fp_img) != 1)
	{
		fprintf(stderr, "ALOS2h5(): %s: unknown format!\n", IMG_file);
		fclose(fp_img);
		return -1;
	}
	sarfd_record_length = Big2Little32(sarfd_record_length);
	if (sarfd_record_length != 720)
	{
		fprintf(stderr, "ALOS2h5(): %s: unknown format!\n", IMG_file);
		fclose(fp_img);
		return -1;
	}

	char* ptr = NULL;
	fseek(fp_img, 237 - 1, SEEK_SET);
	fread(buf, 1, 8, fp_img);
	rows = strtol(buf, &ptr, 0);

	fseek(fp_img, sarfd_record_length + 9 - 1, SEEK_SET);
	fread(&record_length, 4, 1, fp_img);
	record_length = Big2Little32(record_length);

	fseek(fp_img, sarfd_record_length + 25 - 1, SEEK_SET);
	fread(&cols, 4, 1, fp_img);
	cols = Big2Little32(cols);

	memset(buf, 0, 2048);
	fseek(fp_img, 277 - 1, SEEK_SET);
	fread(buf, 1, 4, fp_img);
	sardata_offset = strtol(buf, &ptr, 0);

	ret = write_zero_array_to_h5(dst_h5, "s_re", CV_32F, rows, cols);
	if (return_check(ret, "write_zero_array_to_h5(s_re)", error_head)) { fclose(fp_img); return -1; }
	ret = write_zero_array_to_h5(dst_h5, "s_im", CV_32F, rows, cols);
	if (return_check(ret, "write_zero_array_to_h5(s_im)", error_head)) { fclose(fp_img); return -1; }

	fseek(fp_img, sarfd_record_length + sardata_offset, SEEK_SET);

	int block_height = 1024;
	int num_blocks = (rows + block_height - 1) / block_height;
	size_t block_bytes_limit = (size_t)block_height * record_length;
	unsigned char* block_buf = (unsigned char*)malloc(block_bytes_limit);
	if (!block_buf)
	{
		fprintf(stderr, "ALOS2h5(): out of memory for block buffer!\n");
		fclose(fp_img);
		return -1;
	}

	for (int i = 0; i < num_blocks; ++i) {
		int current_offset = i * block_height;
		int current_rows = std::min(block_height, rows - current_offset);

		size_t read_bytes = fread(block_buf, 1, (size_t)current_rows * record_length, fp_img);
		if (read_bytes != (size_t)current_rows * record_length) {
			fprintf(stderr, "ALOS2h5(): warning: fread bytes mismatch!\n");
		}

		cv::Mat block_re;
		cv::Mat block_im;
		try {
			block_re.create(current_rows, cols, CV_32F);
			block_im.create(current_rows, cols, CV_32F);
		}
		catch (const cv::Exception& e) {
			fprintf(stderr, "ALOS2h5(): out of memory for sub-matrices! (OpenCV exception: %s)\n", e.what());
			free(block_buf);
			fclose(fp_img);
			return -1;
		}

		#pragma omp parallel for schedule(guided)
		for (int r = 0; r < current_rows; ++r) {
			float* ptr_re = block_re.ptr<float>(r);
			float* ptr_im = block_im.ptr<float>(r);
			const float* p_src = (const float*)&block_buf[r * record_length];
			for (int c = 0; c < cols; ++c) {
				ptr_re[c] = ReverseFloat(p_src[2 * c]);
				ptr_im[c] = ReverseFloat(p_src[2 * c + 1]);
			}
		}

		ret = write_subarray_to_h5(dst_h5, "s_re", block_re, current_offset, 0, current_rows, cols);
		if (return_check(ret, "write_subarray_to_h5(s_re)", error_head)) { free(block_buf); fclose(fp_img); return -1; }
		ret = write_subarray_to_h5(dst_h5, "s_im", block_im, current_offset, 0, current_rows, cols);
		if (return_check(ret, "write_subarray_to_h5(s_im)", error_head)) { free(block_buf); fclose(fp_img); return -1; }

		int progress = 10 + (i + 1) * 30 / num_blocks; // 10% -> 40% (since next step is 45%)
		if (!report_progress(progressCallback, userData, progress, "����д��ALOS��ͼ��ֿ�����...")) {
			free(block_buf);
			fclose(fp_img);
			return -2;
		}
	}
	free(block_buf);
	fclose(fp_img);

	//////////////��ȡ��д��������//////////////////////

	if (!report_progress(progressCallback, userData, 45, "��ȡALOS�������")) return -2;
	Mat stateVec;
	ret = read_stateVec_from_ALOS(LED_file, stateVec);
	if (return_check(ret, "read_stateVec_from_ALOS()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5, "state_vec", stateVec);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;

	////////////��ȡ��д��ͼ�������뾭γ����ת����ϵ////////////////////

	if (!report_progress(progressCallback, userData, 60, "��ȡALOS����ת��ϵ��")) return -2;
	Mat lon_coef, lat_coef, row_coef, col_coef;
	ret = read_conversion_coefficient_from_ALOS(LED_file, lon_coef, lat_coef, row_coef, col_coef);
	if (return_check(ret, "read_conversion_coefficient_from_ALOS()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5, "lon_coefficient", lon_coef);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5, "lat_coefficient", lat_coef);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5, "row_coefficient", row_coef);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
	ret = write_array_to_h5(dst_h5, "col_coefficient", col_coef);
	if (return_check(ret, "write_array_to_h5()", error_head)) return -1;


	//////////��LED�ļ�/////////////

	FILE* fp = NULL;
	fopen_s(&fp, LED_file, "rb");
	if (!fp)
	{
		fprintf(stderr, "ALOS2h5(): failed to open %s!\n", LED_file);
		return -1;
	}

	///////////��ȡ��д�����������Ƶ�ʣ�ALOSֻ���˶���������Ƶ���ؾ��������ϵ����/////////

	// Reuse previously declared ptr variable; removed redeclaration to avoid conflict
	Mat tmp = Mat::zeros(1, 1, CV_64F);
	char str[2048]; memset(str, 0, 2048);
	fseek(fp, 720 + 1479 - 1, SEEK_SET);
	fread(str, 1, 32, fp);
	tmp.at<double>(0, 0) = strtod(str, &ptr);
	write_array_to_h5(dst_h5, "doppler_coefficient_a", tmp);
	tmp.at<double>(0, 0) = strtod(ptr, &ptr);
	write_array_to_h5(dst_h5, "doppler_coefficient_b", tmp);

	///////////���ӽ����ϵ��//////////////

	Mat inc_coefficient_r = Mat::zeros(1, 11, CV_64F);
	fseek(fp, 720 + 1887 - 1, SEEK_SET); memset(str, 0, 2048);
	fread(str, 1, 120, fp);
	double factor = 180.0 / 3.1415926535;
	inc_coefficient_r.at<double>(0, 0) = 0.0;
	inc_coefficient_r.at<double>(0, 1) = 1.0;
	inc_coefficient_r.at<double>(0, 2) = 0.0;
	inc_coefficient_r.at<double>(0, 3) = 1.0;
	inc_coefficient_r.at<double>(0, 4) = strtod(str, &ptr) * factor;
	inc_coefficient_r.at<double>(0, 5) = strtod(ptr, &ptr) * factor;
	inc_coefficient_r.at<double>(0, 6) = strtod(ptr, &ptr) * factor;
	inc_coefficient_r.at<double>(0, 7) = strtod(ptr, &ptr) * factor;
	inc_coefficient_r.at<double>(0, 8) = strtod(ptr, &ptr) * factor;
	inc_coefficient_r.at<double>(0, 9) = strtod(ptr, &ptr) * factor;
	write_array_to_h5(dst_h5, "inc_coefficient_r", inc_coefficient_r);

	//�������ӽ�
	fseek(fp, 720 + 485 - 1, SEEK_SET); memset(str, 0, 2048);
	fread(str, 1, 8, fp);
	double inc_center;
	string temp_str = str;
	sscanf(temp_str.c_str(), "%lf", &inc_center);
	write_double_to_h5(dst_h5, "inc_center", inc_center);
	if (!report_progress(progressCallback, userData, 85, "д��ALOS��������"))
	{
		if (fp) fclose(fp);
		return -2;
	}
	///////////д��������������//////////////

	string file_type, sensor, polarization, imaging_mode,
		lookside, orbit_dir, acquisition_start_time, acquisition_stop_time, process_state;
	int record_len, data_set_summary_len, platform_pos_len,
		attitue_data_len, radiometric_data_len;
	record_len = 720; data_set_summary_len = 4096; platform_pos_len = 4680; radiometric_data_len = 9860;
	//��������
	ret = write_str_to_h5(dst_h5, "file_type", "SLC");
	if (return_check(ret, "write_array_to_h5()", error_head))
	{
		if (fp) fclose(fp);
		return -1;
	}
	//��������
	fseek(fp, 49 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 3, fp);
	sensor = str;
	write_str_to_h5(dst_h5, "sensor", sensor.c_str());
	//������ʼʱ��
	string year, month, day, hour, minute, second, temp_string;
	// removed unused: h, m, s (date components parsed as strings, not integers)
	fseek(fp, record_len + 69 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 32, fp);
	temp_string = str;
	year = temp_string.substr(0, 4);
	month = temp_string.substr(4, 2);
	day = temp_string.substr(6, 2);
	hour = "00";
	minute = "00";
	//PRF
	fseek(fp, 720 + 935 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 16, fp);
	tmp.at<double>(0, 0) = strtod(str, &ptr) / 1000;
	write_array_to_h5(dst_h5, "prf", tmp);
	double prf = tmp.at<double>(0, 0);
	//������ʽ
	FILE* fp1 = NULL;
	fopen_s(&fp1, IMG_file, "rb");
	if (fp1)
	{
		short trans, recv;
		fseek(fp1, 720 + 53 - 1, SEEK_SET);
		fread(&trans, 2, 1, fp1);
		trans = Big2Little16(trans);
		fread(&recv, 2, 1, fp1);
		recv = Big2Little16(recv);
		string t, r;
		t = (trans == 0 ? "H" : "V");
		r = (recv == 0 ? "H" : "V");
		polarization = r + t;
		write_str_to_h5(dst_h5, "polarization", polarization.c_str());

		//������ʼʱ��
		uint64_t micro;
		fseek(fp1, 720 + 85 - 1, SEEK_SET);
		fread(&micro, 8, 1, fp1);
		micro = Big2Little64(micro);
		double secds = (double)micro * 0.000001;

		uint64_t micro2;
		fseek(fp1, 720 + 45 - 1, SEEK_SET);
		fread(&micro2, 4, 1, fp1);
		micro2 = Big2Little32(micro2);
		double secds2 = (double)micro2 * 0.001;

		if (sensor == "AL1")secds = secds2;
		memset(str, 0, 2048);
		sprintf(str, "%lf", secds);
		second = str;
		string time = year + "-" + month + "-" + day + "T" + hour + ":" + minute + ":" + second;
		write_str_to_h5(dst_h5, "acquisition_start_time", time.c_str());

		secds += (double)(rows - 1) / prf;

		//�������ʱ��
		memset(str, 0, 2048);
		sprintf(str, "%lf", secds);
		second = str;
		string end_time = year + "-" + month + "-" + day + "T" + hour + ":" + minute + ":" + second;
		write_str_to_h5(dst_h5, "acquisition_stop_time", end_time.c_str());

		//���ϽǾ�γ�ȣ���һ�����أ�
		int lon_topleft, lat_topleft, lon_bottomright, lat_bottomright, slant_range_first_pixel;
		// removed unused: center_lat, center_lon (center coordinates computed from topleft/bottomright)
		Mat temp = Mat::zeros(1, 1, CV_64F);
		fseek(fp1, 720 + 193 - 1, SEEK_SET);
		fread(&lat_topleft, 4, 1, fp1);
		lat_topleft = Big2Little32(lat_topleft);
		temp.at<double>(0, 0) = (double)lat_topleft / 1e6;
		write_array_to_h5(dst_h5, "scene_topleft_lat", temp);

		fseek(fp1, 720 + 205 - 1, SEEK_SET);
		fread(&lon_topleft, 4, 1, fp1);
		lon_topleft = Big2Little32(lon_topleft);
		temp.at<double>(0, 0) = (double)lon_topleft / 1e6;
		write_array_to_h5(dst_h5, "scene_topleft_lon", temp);

		//���ϽǾ�γ��
		fseek(fp1, 720 + 201 - 1, SEEK_SET);
		fread(&lat_bottomright, 4, 1, fp1);
		lat_bottomright = Big2Little32(lat_bottomright);
		temp.at<double>(0, 0) = (double)lat_bottomright / 1e6;
		write_array_to_h5(dst_h5, "scene_topright_lat", temp);

		fseek(fp1, 720 + 213 - 1, SEEK_SET);
		fread(&lon_bottomright, 4, 1, fp1);
		lon_bottomright = Big2Little32(lon_bottomright);
		temp.at<double>(0, 0) = (double)lon_bottomright / 1e6;
		write_array_to_h5(dst_h5, "scene_topright_lon", temp);

		//���б��
		fseek(fp1, 720 + 117 - 1, SEEK_SET);
		fread(&slant_range_first_pixel, 4, 1, fp1);
		slant_range_first_pixel = Big2Little32(slant_range_first_pixel);
		write_int_to_h5(dst_h5, "slant_range_first_pixel", slant_range_first_pixel);

		if (fp1) fclose(fp1);
	}
	//����ģʽ
	fseek(fp, 720 + 413 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 32, fp);
	if (str[4] == '2')//ALOS2
	{
		attitue_data_len = 16384;
		//����
		string filename = IMG_file;
		filename = filename.substr(filename.find_last_of('\\') + 1);
		//����ģʽ
		imaging_mode = filename.substr(29, 3);
		write_str_to_h5(dst_h5, "imaging_mode", imaging_mode.c_str());
		if (filename[32] == 'L')//����
		{
			lookside = "LEFT";
		}
		else
		{
			lookside = "RIGHT";
		}

		//�������
		if (filename[38] == 'D')
		{
			orbit_dir = "Descending";
		}
		else
		{
			orbit_dir = "Ascending";
		}
	}
	else//ALOS1
	{
		attitue_data_len = 8192;
		lookside = "RIGHT";
		imaging_mode = str[10];
		write_str_to_h5(dst_h5, "imaging_mode", imaging_mode.c_str());
		string filename = IMG_file;
		if (filename[filename.length() - 1] == 'A')
		{
			orbit_dir = "Ascending";
		}
		else
		{
			orbit_dir = "Descending";
		}
	}
	write_str_to_h5(dst_h5, "lookside", lookside.c_str());
	write_str_to_h5(dst_h5, "orbit_dir", orbit_dir.c_str());
	//��Ƶ
	fseek(fp, 720 + 501 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 16, fp);
	tmp.at<double>(0, 0) = 3e8 / strtod(str, &ptr);
	write_array_to_h5(dst_h5, "carrier_frequency", tmp);
	//�������ӽ�
	fseek(fp, 720 + 485 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 8, fp);
	tmp.at<double>(0, 0) = strtod(str, &ptr);
	write_array_to_h5(dst_h5, "inc_center", tmp);
	////PRF
	//fseek(fp, 720 + 935 - 1, SEEK_SET);
	//memset(str, 0, 2048);
	//fread(str, 1, 16, fp);
	//tmp.at<double>(0, 0) = strtod(str, &ptr) / 1000;
	//write_array_to_h5(dst_h5, "prf", tmp);
	//��λ�����ص���
	Mat tmp_int = Mat::zeros(1, 1, CV_32S);
	tmp_int.at<int>(0, 0) = rows;
	write_array_to_h5(dst_h5, "azimuth_len", tmp_int);
	//���������ص���
	tmp_int.at<int>(0, 0) = cols;
	write_array_to_h5(dst_h5, "range_len", tmp_int);
	//������ֱ���
	int offset = record_len + data_set_summary_len + platform_pos_len + attitue_data_len + radiometric_data_len;
	fseek(fp, offset + 127 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 16, fp);
	tmp.at<double>(0, 0) = strtod(str, &ptr);
	write_array_to_h5(dst_h5, "range_resolution", tmp);
	//��λ��ֱ���
	memset(str, 0, 2048);
	fread(str, 1, 16, fp);
	tmp.at<double>(0, 0) = strtod(str, &ptr);
	write_array_to_h5(dst_h5, "azimuth_resolution", tmp);
	//��λ��������
	fseek(fp, 720 + 1687 - 1, SEEK_SET);
	memset(str, 0, 2048);
	fread(str, 1, 32, fp);
	tmp.at<double>(0, 0) = strtod(str, &ptr);
	write_array_to_h5(dst_h5, "azimuth_spacing", tmp);
	//������������
	tmp.at<double>(0, 0) = strtod(ptr, &ptr);
	write_array_to_h5(dst_h5, "range_spacing", tmp);

	//����ȼ�
	ret = write_str_to_h5(dst_h5, "process_state", "InSAR_0");
	//��������
	int t = attitue_data_len > 10000 ? 2 : 1;
	char strings[1024]; memset(strings, 0, 1024);
	sprintf(strings, "import from ALOS%d Single Look Complex, unprocessed.", t);
	string sss(strings);
	write_str_to_h5(dst_h5, "comment", sss.c_str());

	if (fp)fclose(fp);
	if (!report_progress(progressCallback, userData, 100, "ALOS���ݵ������")) return -2;
	return 0;
}


















struct XMLFile::Impl
{
	char m_xmlFileName[2048];
	TiXmlDocument doc;
	int data_node_count;
	char error_head[256];

	Impl()
	{
		memset(m_xmlFileName, 0, 2048);
		memset(error_head, 0, 256);
		data_node_count = 0;
		strcpy(error_head, "XMLFILE_DLL_ERROR: error happens when using ");
	}
};

XMLFile::XMLFile()
	: impl_(new Impl())
{
}

XMLFile::XMLFile(const XMLFile& other)
	: impl_(new Impl(*other.impl_))
{
}

XMLFile& XMLFile::operator=(const XMLFile& other)
{
	if (this != &other)
	{
		*impl_ = *other.impl_;
	}
	return *this;
}

XMLFile::~XMLFile()
{
}

int XMLFile::XMLFile_creat_new_project(const char* project_path, const char* project_name, const char* project_version)
{
	if (project_path == NULL ||
		project_name == NULL ||
		project_version == NULL
		)
	{
		fprintf(stderr, "XMLFile_creat_new_project(): input check failed!\n");
		return -1;
	}
	TiXmlDeclaration* declaration = new TiXmlDeclaration("1.0", "UTF-8", "yes");
	impl_->doc.LinkEndChild(declaration);
	TiXmlElement* Root = new TiXmlElement("Root");
	impl_->doc.LinkEndChild(Root);
	TiXmlElement* prj_info_node = new TiXmlElement("project_info");
	Root->LinkEndChild(prj_info_node);
	prj_info_node->SetAttribute("version", "1.0");
	TiXmlElement* prj_name_node = new TiXmlElement("project_name");
	TiXmlText* content = new TiXmlText(project_name);
	prj_name_node->LinkEndChild(content);
	prj_info_node->LinkEndChild(prj_name_node);
	content = new TiXmlText(project_path);
	TiXmlElement* prj_path_node = new TiXmlElement("project_path");
	prj_path_node->LinkEndChild(content);
	prj_info_node->LinkEndChild(prj_path_node);

	string path(project_path); string name(project_name);
	string filename = path + "\\" + name;
	std::replace(filename.begin(), filename.end(), '/', '\\');
	impl_->doc.SaveFile(filename.c_str());
	return 0;
}

int XMLFile::XMLFile_add_origin(
	const char* datanode_node,
	const char* node_name,
	const char* node_path,
	const char* sensor
)
{
	if (node_name == NULL ||
		node_path == NULL ||
		sensor == NULL ||
		datanode_node == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_origin(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	TiXmlElement* p = impl_->doc.RootElement();
	int ret = find_node_with_attribute(p, "DataNode", "name", datanode_node, DataNode);
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		impl_->doc.RootElement()->LinkEndChild(DataNode);
		DataNode->SetAttribute("name", datanode_node);
		DataNode->SetAttribute("index", "1");
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "import");
		DataNode->SetAttribute("rank", "complex-0.0");
		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-0.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		Row_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		Col_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Sensor = new TiXmlElement("Sensor");
		Sensor->LinkEndChild(new TiXmlText(sensor));
		Data_Processing_Parameters->LinkEndChild(Sensor);
	}
	else
	{
		impl_->data_node_count = atoi(DataNode->Attribute("data_count")) + 1;
		string tmp;
		tmp = int2str(impl_->data_node_count);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");


		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-0.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		Row_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		Col_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_origin_14(const char* datanode_node, const char* node_name, const char* node_path, int mode, const char* sensor)
{
	if (node_name == NULL ||
		node_path == NULL ||
		sensor == NULL ||
		datanode_node == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_origin_14(): input check failed!\n");
		return -1;
	}
	char rank[256];
	sprintf(rank, "%d-complex-0.0", mode);
	TiXmlElement* DataNode = NULL;
	TiXmlElement* p = impl_->doc.RootElement();
	int ret = find_node_with_attribute(p, "DataNode", "name", datanode_node, DataNode);
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		impl_->doc.RootElement()->LinkEndChild(DataNode);
		DataNode->SetAttribute("name", datanode_node);
		DataNode->SetAttribute("index", "1");
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "import");

		DataNode->SetAttribute("rank", rank);
		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		Row_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		Col_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Sensor = new TiXmlElement("Sensor");
		Sensor->LinkEndChild(new TiXmlText(sensor));
		Data_Processing_Parameters->LinkEndChild(Sensor);
	}
	else
	{
		impl_->data_node_count = atoi(DataNode->Attribute("data_count")) + 1;
		string tmp;
		tmp = int2str(impl_->data_node_count);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");


		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		Row_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		Col_Offset->LinkEndChild(new TiXmlText("0"));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}











static const char* safe_rank(TiXmlElement* el) {
	if (!el) return "";
	const char* r = el->Attribute("rank");
	return r ? r : "";
}

int XMLFile::XMLFile_add_cut(
	const char* datanode_name,
	int master_index,
	const char* node_name,
	const char* node_path,
	int Row_offset,
	int Col_offset,
	double lon,
	double lat,
	double width,
	double height,
	const char* data_rank
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_cut(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			int i = strcmp(safe_rank(root), "import") == 0;
			int j = strcmp(safe_rank(root), "cut") == 0;
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "cut");
		DataNode->SetAttribute("rank", data_rank);


		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(data_rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Center_lon = new TiXmlElement("Center_lon");
		Center_lon->SetAttribute("unit", "degree");
		char tmp2[100];
		sprintf_s(tmp2, "%.2f", lon);
		Center_lon->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Center_lon);
		TiXmlElement* Center_lat = new TiXmlElement("Center_lat");
		Center_lat->SetAttribute("unit", "degree");
		sprintf_s(tmp2, "%.2f", lat);
		Center_lat->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Center_lat);
		TiXmlElement* Width = new TiXmlElement("Width");
		Width->SetAttribute("unit", "m");
		sprintf_s(tmp2, "%.2f", width);
		Width->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Width);
		TiXmlElement* Height = new TiXmlElement("Height");
		Height->SetAttribute("unit", "m");
		sprintf_s(tmp2, "%.2f", height);
		Height->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Height);

		if (master_index > 0)
		{
			TiXmlElement* master_image = new TiXmlElement("master_image");
			sprintf_s(tmp2, "%d", master_index);
			master_image->LinkEndChild(new TiXmlText(tmp2));
			Data_Processing_Parameters->LinkEndChild(master_image);
		}

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(data_rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_cut_14(
	const char* datanode_name,
	int master_index,
	const char* node_name,
	const char* node_path,
	int Row_offset,
	int Col_offset,
	double lon,
	double lat,
	double width,
	double height,
	const char* data_rank
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_cut(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			int ret, mode;
			double level;
			ret = sscanf(safe_rank(root), "%d-complex-%lf", &mode, &level);
			if (ret == 0 && level <= 1.0) continue;
			else break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "cut");
		DataNode->SetAttribute("rank", data_rank);


		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(data_rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Center_lon = new TiXmlElement("Center_lon");
		Center_lon->SetAttribute("unit", "degree");
		char tmp2[100];
		sprintf_s(tmp2, "%.2f", lon);
		Center_lon->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Center_lon);
		TiXmlElement* Center_lat = new TiXmlElement("Center_lat");
		Center_lat->SetAttribute("unit", "degree");
		sprintf_s(tmp2, "%.2f", lat);
		Center_lat->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Center_lat);
		TiXmlElement* Width = new TiXmlElement("Width");
		Width->SetAttribute("unit", "m");
		sprintf_s(tmp2, "%.2f", width);
		Width->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Width);
		TiXmlElement* Height = new TiXmlElement("Height");
		Height->SetAttribute("unit", "m");
		sprintf_s(tmp2, "%.2f", height);
		Height->LinkEndChild(new TiXmlText(tmp2));
		Data_Processing_Parameters->LinkEndChild(Height);

		if (master_index > 0)
		{
			TiXmlElement* master_image = new TiXmlElement("master_image");
			sprintf_s(tmp2, "%d", master_index);
			master_image->LinkEndChild(new TiXmlText(tmp2));
			Data_Processing_Parameters->LinkEndChild(master_image);
		}


		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(data_rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_regis(const char* datanode_name, const char* node_name, const char* node_path, int Row_offset, int Col_offset, int master_index, int interp_times, int block_size, const char* temporal_baseline, const char* B_effect, const char* B_parallel)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		B_effect == NULL ||
		B_parallel == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_regis(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "coregistration");
		DataNode->SetAttribute("rank", "complex-2.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(master_index);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);
		TiXmlElement* Blocksize = new TiXmlElement("blocksize");
		tmp = int2str(block_size);
		Blocksize->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Blocksize);
		TiXmlElement* Interp_times = new TiXmlElement("interp_times");
		tmp = int2str(interp_times);
		Interp_times->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Interp_times);
		TiXmlElement* Temporal_baseline = new TiXmlElement("temporal_baseline_distribution");
		Temporal_baseline->SetAttribute("unit", "day");
		Temporal_baseline->LinkEndChild(new TiXmlText(temporal_baseline));
		Data_Processing_Parameters->LinkEndChild(Temporal_baseline);
		TiXmlElement* V_baseline = new TiXmlElement("effect_baseline_distribution");
		V_baseline->SetAttribute("unit", "m");
		V_baseline->LinkEndChild(new TiXmlText(B_effect));
		Data_Processing_Parameters->LinkEndChild(V_baseline);
		TiXmlElement* H_baseline = new TiXmlElement("parallel_baseline_distribution");
		H_baseline->SetAttribute("unit", "m");
		H_baseline->LinkEndChild(new TiXmlText(B_parallel));
		Data_Processing_Parameters->LinkEndChild(H_baseline);
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}


int XMLFile::XMLFile_add_regis(const char* datanode_name, const char* node_name, const char* node_path, double Row_offset, double Col_offset, int master_index, int interp_times, int block_size, const char* temporal_baseline, const char* B_effect, const char* B_parallel)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		B_effect == NULL ||
		B_parallel == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_regis(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "coregistration");
		DataNode->SetAttribute("rank", "complex-2.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);

		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = double2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = double2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(master_index);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);
		TiXmlElement* Blocksize = new TiXmlElement("blocksize");
		tmp = int2str(block_size);
		Blocksize->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Blocksize);
		TiXmlElement* Interp_times = new TiXmlElement("interp_times");
		tmp = int2str(interp_times);
		Interp_times->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Interp_times);
		TiXmlElement* Temporal_baseline = new TiXmlElement("temporal_baseline_distribution");
		Temporal_baseline->SetAttribute("unit", "day");
		Temporal_baseline->LinkEndChild(new TiXmlText(temporal_baseline));
		Data_Processing_Parameters->LinkEndChild(Temporal_baseline);
		TiXmlElement* V_baseline = new TiXmlElement("effect_baseline_distribution");
		V_baseline->SetAttribute("unit", "m");
		V_baseline->LinkEndChild(new TiXmlText(B_effect));
		Data_Processing_Parameters->LinkEndChild(V_baseline);
		TiXmlElement* H_baseline = new TiXmlElement("parallel_baseline_distribution");
		H_baseline->SetAttribute("unit", "m");
		H_baseline->LinkEndChild(new TiXmlText(B_parallel));
		Data_Processing_Parameters->LinkEndChild(H_baseline);
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);

		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = double2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = double2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_regis(const char* datanode_name, const std::vector<std::string>& node_names, const std::vector<std::string>& node_paths, const std::vector<double>& row_offsets, const std::vector<double>& col_offsets, int master_index, int interp_times, int block_size, const std::vector<std::string>& temporal_baselines, const std::vector<std::string>& B_effects, const std::vector<std::string>& B_parallels)
{
	size_t count = node_names.size();
	if (node_paths.size() != count || row_offsets.size() != count || col_offsets.size() != count) return -1;
	for (size_t i = 0; i < count; ++i) {
		const char* tb = (i < temporal_baselines.size()) ? temporal_baselines[i].c_str() : "";
		const char* be = (i < B_effects.size()) ? B_effects[i].c_str() : "";
		const char* bp = (i < B_parallels.size()) ? B_parallels[i].c_str() : "";
		int ret = XMLFile_add_regis(datanode_name, node_names[i].c_str(), node_paths[i].c_str(), row_offsets[i], col_offsets[i], master_index, interp_times, block_size, tb, be, bp);
		if (ret != 0) return ret;
	}
	return 0;
}

int XMLFile::XMLFile_add_regis14(
	int mode,
	const char* datanode_name,
	const char* node_name,
	const char* node_path,
	int Row_offset,
	int Col_offset,
	int master_index,
	int interp_times,
	int block_size,
	const char* temporal_baseline,
	const char* B_effect,
	const char* B_parallel
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		B_effect == NULL ||
		B_parallel == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_regis14(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp; int mode2; double level; char rank[256];
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			ret = sscanf(safe_rank(root), "%d-complex-%lf", &mode2, &level);
			if (ret == 2 && level <= 2.0) continue;
			else break;
		}

		sprintf(rank, "%d-complex-2.0", mode2);
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "coregistration");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(master_index);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);
		TiXmlElement* Blocksize = new TiXmlElement("blocksize");
		tmp = int2str(block_size);
		Blocksize->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Blocksize);
		TiXmlElement* Interp_times = new TiXmlElement("interp_times");
		tmp = int2str(interp_times);
		Interp_times->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Interp_times);
		TiXmlElement* Temporal_baseline = new TiXmlElement("temporal_baseline_distribution");
		Temporal_baseline->SetAttribute("unit", "day");
		Temporal_baseline->LinkEndChild(new TiXmlText(temporal_baseline));
		Data_Processing_Parameters->LinkEndChild(Temporal_baseline);
		TiXmlElement* V_baseline = new TiXmlElement("effect_baseline_distribution");
		V_baseline->SetAttribute("unit", "m");
		V_baseline->LinkEndChild(new TiXmlText(B_effect));
		Data_Processing_Parameters->LinkEndChild(V_baseline);
		TiXmlElement* H_baseline = new TiXmlElement("parallel_baseline_distribution");
		H_baseline->SetAttribute("unit", "m");
		H_baseline->LinkEndChild(new TiXmlText(B_parallel));
		Data_Processing_Parameters->LinkEndChild(H_baseline);
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		sprintf(rank, "%d-complex-2.0", mode);
		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}


int XMLFile::XMLFile_add_regis14(int mode, const char* datanode_name, const char* node_name, const char* node_path, double Row_offset, double Col_offset, int master_index, int interp_times, int block_size, const char* temporal_baseline, const char* B_effect, const char* B_parallel)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		B_effect == NULL ||
		B_parallel == NULL
		)
	{
		fprintf(stderr, "XMLFile_add_regis14(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	char rank[64] = {};
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		int mode2 = mode;
		double level = 0.0;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			ret = sscanf(safe_rank(root), "%d-complex-%lf", &mode2, &level);
			if (ret == 2 && level <= 2.0) continue;
			else break;
		}

		sprintf(rank, "%d-complex-2.0", mode2);
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "coregistration");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);

		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = double2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = double2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(master_index);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);
		TiXmlElement* Blocksize = new TiXmlElement("blocksize");
		tmp = int2str(block_size);
		Blocksize->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Blocksize);
		TiXmlElement* Interp_times = new TiXmlElement("interp_times");
		tmp = int2str(interp_times);
		Interp_times->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Interp_times);
		TiXmlElement* Temporal_baseline = new TiXmlElement("temporal_baseline_distribution");
		Temporal_baseline->SetAttribute("unit", "day");
		Temporal_baseline->LinkEndChild(new TiXmlText(temporal_baseline));
		Data_Processing_Parameters->LinkEndChild(Temporal_baseline);
		TiXmlElement* V_baseline = new TiXmlElement("effect_baseline_distribution");
		V_baseline->SetAttribute("unit", "m");
		V_baseline->LinkEndChild(new TiXmlText(B_effect));
		Data_Processing_Parameters->LinkEndChild(V_baseline);
		TiXmlElement* H_baseline = new TiXmlElement("parallel_baseline_distribution");
		H_baseline->SetAttribute("unit", "m");
		H_baseline->LinkEndChild(new TiXmlText(B_parallel));
		Data_Processing_Parameters->LinkEndChild(H_baseline);
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		sprintf(rank, "%d-complex-2.0", mode);
		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);

		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = double2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = double2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_regis14(int mode, const char* datanode_name, const std::vector<std::string>& node_names, const std::vector<std::string>& node_paths, const std::vector<double>& row_offsets, const std::vector<double>& col_offsets, int master_index, int interp_times, int block_size, const std::vector<std::string>& temporal_baselines, const std::vector<std::string>& B_effects, const std::vector<std::string>& B_parallels)
{
	size_t count = node_names.size();
	if (node_paths.size() != count || row_offsets.size() != count || col_offsets.size() != count) return -1;
	for (size_t i = 0; i < count; ++i) {
		const char* tb = (i < temporal_baselines.size()) ? temporal_baselines[i].c_str() : "";
		const char* be = (i < B_effects.size()) ? B_effects[i].c_str() : "";
		const char* bp = (i < B_parallels.size()) ? B_parallels[i].c_str() : "";
		int ret = XMLFile_add_regis14(mode, datanode_name, node_names[i].c_str(), node_paths[i].c_str(), row_offsets[i], col_offsets[i], master_index, interp_times, block_size, tb, be, bp);
		if (ret != 0) return ret;
	}
	return 0;
}

int XMLFile::XMLFile_add_backgeocoding(const char* dataNode, const char* dataName, const char* dataPath, int masterIndex)
{
	if (!dataNode ||
		!dataName ||
		!dataPath
		)
	{
		fprintf(stderr, "XMLFile_add_backgeocoding(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", dataNode, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", dataNode);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "coregistration");
		DataNode->SetAttribute("rank", "complex-2.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(masterIndex);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_SLC_deramp(const char* dataNode, const char* dataName, const char* dataPath, int masterIndex)
{
	if (!dataNode ||
		!dataName ||
		!dataPath
		)
	{
		fprintf(stderr, "XMLFile_add_SLC_deramp(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", dataNode, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", dataNode);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0 ||
				strcmp(safe_rank(root), "complex-3.0") == 0
				)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "SLC_deramp");
		DataNode->SetAttribute("rank", "complex-3.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-3.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(masterIndex);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-3.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_SLC_deramp_14(
	int mode,
	const char* dataNode,
	const char* dataName,
	const char* dataPath,
	int masterIndex
)
{
	if (!dataNode ||
		!dataName ||
		!dataPath
		)
	{
		fprintf(stderr, "XMLFile_add_SLC_deramp_14(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", dataNode, DataNode);
	string tmp;
	char rank[256];
	sprintf(rank, "%d-complex-3.0", mode);
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", dataNode);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			string rank2 = safe_rank(root);
			int mode2; double level2;
			ret = sscanf(rank2.c_str(), "%d-complex-%lf", &mode2, &level2);
			if (ret == 2 && level2 <= 3.0) continue;
			else break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "SLC_deramp");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_index = new TiXmlElement("master_image");
		tmp = int2str(masterIndex);
		Master_index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Master_index);

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_SBAS(const char* dataNode, const char* dataName, const char* dataPath)
{
	if (!dataNode ||
		!dataName ||
		!dataPath
		)
	{
		fprintf(stderr, "XMLFile_add_SBAS(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	string tmp;
	DataNode = new TiXmlElement("DataNode");
	DataNode->SetAttribute("name", dataNode);
	int index = 1;
	TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
	for (; root != NULL; root = root->NextSiblingElement(), index++)
	{
		if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
			strcmp(safe_rank(root), "complex-1.0") == 0 ||
			strcmp(safe_rank(root), "complex-3.0") == 0 ||
			strcmp(safe_rank(root), "phase-1.0") == 0 ||
			strcmp(safe_rank(root), "phase-2.0") == 0 ||
			strcmp(safe_rank(root), "phase-3.0") == 0 ||
			strcmp(safe_rank(root), "dem-1.0") == 0 ||
			strcmp(safe_rank(root), "SBAS-1.0") == 0
			)
			continue;
		else
			break;
	}
	string index_str = int2str(index);
	DataNode->SetAttribute("index", index_str.c_str());
	DataNode->SetAttribute("data_count", "1");
	DataNode->SetAttribute("data_processing", "SBAS");
	DataNode->SetAttribute("rank", "SBAS-1.0");

	TiXmlElement* Data = new TiXmlElement("Data");
	DataNode->LinkEndChild(Data);
	TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
	Data_Name->LinkEndChild(new TiXmlText(dataName));
	Data->LinkEndChild(Data_Name);
	TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
	Data_Rank->LinkEndChild(new TiXmlText("SBAS-1.0"));
	Data->LinkEndChild(Data_Rank);
	TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
	Data_Index->LinkEndChild(new TiXmlText("1"));
	Data->LinkEndChild(Data_Index);
	TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
	Data_Path->LinkEndChild(new TiXmlText(dataPath));
	Data->LinkEndChild(Data_Path);
	TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
	tmp = int2str(0);
	Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
	Data->LinkEndChild(Row_Offset);
	TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
	tmp = int2str(0);
	Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
	Data->LinkEndChild(Col_Offset);

	TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
	DataNode->LinkEndChild(Data_Processing_Parameters);
	TiXmlElement* nill = new TiXmlElement("nill");
	tmp = int2str(0);
	nill->LinkEndChild(new TiXmlText(tmp.c_str()));
	Data_Processing_Parameters->LinkEndChild(nill);

	if (!root)
	{
		impl_->doc.RootElement()->LinkEndChild(DataNode);
	}
	else
	{
		impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
		while (root)
		{
			index_str = int2str(++index);
			root->SetAttribute("index", index_str.c_str());
			root = root->NextSiblingElement();
		}

	}
	return 0;
}

int XMLFile::XMLFile_add_S1_Deburst(const char* dataNode, const char* dataName, const char* dataPath)
{
	if (!dataNode ||
		!dataName ||
		!dataPath
		)
	{
		fprintf(stderr, "XMLFile_add_S1_Deburst(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", dataNode, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", dataNode);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "deburst");
		DataNode->SetAttribute("rank", "complex-1.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-1.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* nill = new TiXmlElement("nill");
		tmp = int2str(0);
		nill->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(nill);

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("complex-1.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_geocoding(const char* dataNode, const char* dataName, const char* dataPath, const char* level)
{
	if (!dataNode ||
		!dataName ||
		!dataPath ||
		!level
		)
	{
		fprintf(stderr, "XMLFile_add_geocoding(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", dataNode, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", dataNode);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{

		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "geocoding");
		DataNode->SetAttribute("rank", level);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(level));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* nill = new TiXmlElement("nill");
		tmp = int2str(0);
		nill->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(nill);


		impl_->doc.RootElement()->LinkEndChild(DataNode);
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(dataName));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(level));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(dataPath));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(0);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(0);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_interferometric_phase_14(
	const char* datanode_name,
	const char* node_name,
	const char* node_path,
	const char* master_name,
	const char* rank,
	int offset_row,
	int offset_col,
	int multilook_rg,
	int multilook_az
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		master_name == NULL ||
		rank == NULL)
	{
		fprintf(stderr, "XMLFile_add_interferometric_phase_14(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			int ret2, ret3;
			int mode2, mode3; double level2, level3;
			ret2 = sscanf(safe_rank(root), "%d-complex-%lf", &mode2, &level2);
			ret3 = sscanf(safe_rank(root), "%d-phase-%lf", &mode3, &level3);
			if ((ret3 == 2 && level3 <= 1.0) || ret2 == 2) continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "interferometric_formation");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(offset_row);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(offset_col);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_image = new TiXmlElement("master_image");
		Master_image->LinkEndChild(new TiXmlText(master_name));
		Data_Processing_Parameters->LinkEndChild(Master_image);

		TiXmlElement* Multilook_rg = new TiXmlElement("multilook_rg");
		tmp = int2str(multilook_rg);
		Multilook_rg->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Multilook_rg);
		TiXmlElement* Multilook_az = new TiXmlElement("multilook_az");
		tmp = int2str(multilook_az);
		Multilook_az->LinkEndChild(new TiXmlText(tmp.c_str()));

		Data_Processing_Parameters->LinkEndChild(Multilook_az);

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(offset_row);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(offset_col);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_interferometric_phase(const char* datanode_name, const char* node_name, const char* node_path,
	const char* master_name, const char* rank, int offset_row, int offset_col, int isdeflat, int istopo_removal, int iscoherence,
	int win_w, int win_h, int multilook_rg, int multilook_az)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		master_name == NULL ||
		rank == NULL)
	{
		fprintf(stderr, "XMLFile_add_interferometric_phase(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0 ||
				strcmp(safe_rank(root), "phase-1.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "interferometric_formation");
		DataNode->SetAttribute("rank", "phase-1.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(offset_row);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(offset_col);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Master_image = new TiXmlElement("master_image");
		Master_image->LinkEndChild(new TiXmlText(master_name));
		Data_Processing_Parameters->LinkEndChild(Master_image);
		TiXmlElement* Isdeflat = new TiXmlElement("IsDeflat");
		tmp = int2str(isdeflat);
		Isdeflat->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Isdeflat);
		TiXmlElement* Istopo_removal = new TiXmlElement("IsTopo_Removal");
		tmp = int2str(istopo_removal);
		Istopo_removal->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Istopo_removal);
		TiXmlElement* Iscoherence = new TiXmlElement("IsCoherence");
		tmp = int2str(iscoherence);
		Iscoherence->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Iscoherence);
		TiXmlElement* Win_w = new TiXmlElement("coh_est_width");
		tmp = int2str(win_w);
		Win_w->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Win_w);
		TiXmlElement* Win_h = new TiXmlElement("coh_est_height");
		tmp = int2str(win_h);
		Win_h->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Win_h);
		TiXmlElement* Multilook_rg = new TiXmlElement("multilook_rg");
		tmp = int2str(multilook_rg);
		Multilook_rg->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data_Processing_Parameters->LinkEndChild(Multilook_rg);
		TiXmlElement* Multilook_az = new TiXmlElement("multilook_az");
		tmp = int2str(multilook_az);
		Multilook_az->LinkEndChild(new TiXmlText(tmp.c_str()));

		Data_Processing_Parameters->LinkEndChild(Multilook_az);

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(offset_row);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(offset_col);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_denoise_14(
	int mode,
	const char* datanode_name,
	const char* node_name,
	const char* node_path,
	int Row_offset,
	int Col_offset,
	const char* method,
	int Slop_win,
	int Pre_win,
	int Goldstein_win,
	int Goldstein_filled_win,
	double alpha,
	const char* filter_dl_path,
	const char* dl_model_file,
	const char* tmp_path
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		method == NULL)
	{
		fprintf(stderr, "XMLFile_add_denoise(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	char rank[256];
	sprintf(rank, "%d-phase-2.0", mode);
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			int ret2, mode2, ret3, mode3; double level2, level3;
			ret2 = sscanf(safe_rank(root), "%d-complex-%lf", &mode2, &level2);
			ret3 = sscanf(safe_rank(root), "%d-phase-%lf", &mode3, &level3);
			if ((ret2 == 2) || (ret3 == 2 && level3 <= 2.0))
			{
				continue;
			}
			else
			{
				break;
			}
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "denoise");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Method = new TiXmlElement("method");
		Method->LinkEndChild(new TiXmlText(method));
		Data_Processing_Parameters->LinkEndChild(Method);

		if (strcmp(method, "Slope") == 0)
		{
			TiXmlElement* Slop_wnd_size = new TiXmlElement("filter_wnd_size");
			tmp = int2str(Slop_win);
			Slop_wnd_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Slop_wnd_size);

			TiXmlElement* Pre_wnd_size = new TiXmlElement("filter_wnd_size");
			tmp = int2str(Pre_win);
			Pre_wnd_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Pre_wnd_size);
		}
		else if (strcmp(method, "Goldstein") == 0)
		{
			char tmp2[100];
			TiXmlElement* Goldstein_wnd_size = new TiXmlElement("FFT_size");
			tmp = int2str(Goldstein_win);
			Goldstein_wnd_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Goldstein_wnd_size);

			TiXmlElement* Goldstein_filled_size = new TiXmlElement("n_pad");
			tmp = int2str(Goldstein_filled_win);
			Goldstein_filled_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Goldstein_filled_size);

			TiXmlElement* Alpha = new TiXmlElement("alpha");
			sprintf_s(tmp2, "%.2f", alpha);
			Alpha->LinkEndChild(new TiXmlText(tmp2));
			Data_Processing_Parameters->LinkEndChild(Alpha);
		}
		else if (strcmp(method, "DL") == 0)
		{

			TiXmlElement* DL_path = new TiXmlElement("filter_dl_path");
			DL_path->LinkEndChild(new TiXmlText(filter_dl_path));
			Data_Processing_Parameters->LinkEndChild(DL_path);

			TiXmlElement* Model_path = new TiXmlElement("dl_model_file");
			Model_path->LinkEndChild(new TiXmlText(dl_model_file));
			Data_Processing_Parameters->LinkEndChild(Model_path);

			TiXmlElement* Tmp_path = new TiXmlElement("tmp_path");
			Tmp_path->LinkEndChild(new TiXmlText(tmp_path));
			Data_Processing_Parameters->LinkEndChild(Tmp_path);
		}

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}


int XMLFile::XMLFile_add_denoise(const char* datanode_name, const char* node_name, const char* node_path, int Row_offset, int Col_offset, const char* method, int Slop_win, int Pre_win, int Goldstein_win, int Goldstein_filled_win, double alpha, const char* filter_dl_path, const char* dl_model_file, const char* tmp_path)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		method == NULL)
	{
		fprintf(stderr, "XMLFile_add_denoise(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0 ||
				strcmp(safe_rank(root), "phase-1.0") == 0 ||
				strcmp(safe_rank(root), "phase-2.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "denoise");
		DataNode->SetAttribute("rank", "phase-2.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("phase-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Method = new TiXmlElement("method");
		Method->LinkEndChild(new TiXmlText(method));
		Data_Processing_Parameters->LinkEndChild(Method);

		if (strcmp(method, "Slope") == 0)
		{
			TiXmlElement* Slop_wnd_size = new TiXmlElement("filter_wnd_size");
			tmp = int2str(Slop_win);
			Slop_wnd_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Slop_wnd_size);

			TiXmlElement* Pre_wnd_size = new TiXmlElement("filter_wnd_size");
			tmp = int2str(Pre_win);
			Pre_wnd_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Pre_wnd_size);
		}
		else if (strcmp(method, "Goldstein") == 0)
		{
			char tmp2[100];
			TiXmlElement* Goldstein_wnd_size = new TiXmlElement("FFT_size");
			tmp = int2str(Goldstein_win);
			Goldstein_wnd_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Goldstein_wnd_size);

			TiXmlElement* Goldstein_filled_size = new TiXmlElement("n_pad");
			tmp = int2str(Goldstein_filled_win);
			Goldstein_filled_size->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Goldstein_filled_size);

			TiXmlElement* Alpha = new TiXmlElement("alpha");
			sprintf_s(tmp2, "%.2f", alpha);
			Alpha->LinkEndChild(new TiXmlText(tmp2));
			Data_Processing_Parameters->LinkEndChild(Alpha);
		}
		else if (strcmp(method, "DL") == 0)
		{

			TiXmlElement* DL_path = new TiXmlElement("filter_dl_path");
			DL_path->LinkEndChild(new TiXmlText(filter_dl_path));
			Data_Processing_Parameters->LinkEndChild(DL_path);

			TiXmlElement* Model_path = new TiXmlElement("dl_model_file");
			Model_path->LinkEndChild(new TiXmlText(dl_model_file));
			Data_Processing_Parameters->LinkEndChild(Model_path);

			TiXmlElement* Tmp_path = new TiXmlElement("tmp_path");
			Tmp_path->LinkEndChild(new TiXmlText(tmp_path));
			Data_Processing_Parameters->LinkEndChild(Tmp_path);
		}

		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("phase-2.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_unwrap(const char* datanode_name, const char* node_name, const char* node_path, int Row_offset, int Col_offset, const char* method, double threshold)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		method == NULL)
	{
		fprintf(stderr, "XMLFile_add_unwrap(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0 ||
				strcmp(safe_rank(root), "phase-1.0") == 0 ||
				strcmp(safe_rank(root), "phase-2.0") == 0 ||
				strcmp(safe_rank(root), "phase-3.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "unwrap");
		DataNode->SetAttribute("rank", "phase-3.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("phase-3.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Method = new TiXmlElement("method");
		Method->LinkEndChild(new TiXmlText(method));
		Data_Processing_Parameters->LinkEndChild(Method);

		if (strcmp(method, "Combined") == 0)
		{
			char tmp2[200];
			TiXmlElement* Coherence_threshold = new TiXmlElement("coherence_thresh");
			sprintf_s(tmp2, "%.2f", threshold);
			Coherence_threshold->LinkEndChild(new TiXmlText(tmp2));
			Data_Processing_Parameters->LinkEndChild(Coherence_threshold);
		}
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("phase-3.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_unwrap_14(
	int mode,
	const char* datanode_name,
	const char* node_name,
	const char* node_path,
	int Row_offset,
	int Col_offset,
	const char* method,
	double threshold
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		method == NULL)
	{
		fprintf(stderr, "XMLFile_add_unwrap_14(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	char rank[256];
	sprintf(rank, "%d-phase-3.0", mode);
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			int ret2, ret3, mode2, mode3; double level2, level3;
			ret2 = sscanf(safe_rank(root), "%d-complex-%lf", &mode2, &level2);
			ret3 = sscanf(safe_rank(root), "%d-phase-%lf", &mode3, &level3);
			if (ret2 == 2 || ret3 == 2) continue;
			else break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "unwrap");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Method = new TiXmlElement("method");
		Method->LinkEndChild(new TiXmlText(method));
		Data_Processing_Parameters->LinkEndChild(Method);

		if (strcmp(method, "Combined") == 0)
		{
			char tmp2[200];
			TiXmlElement* Coherence_threshold = new TiXmlElement("coherence_thresh");
			sprintf_s(tmp2, "%.2f", threshold);
			Coherence_threshold->LinkEndChild(new TiXmlText(tmp2));
			Data_Processing_Parameters->LinkEndChild(Coherence_threshold);
		}
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_dem(const char* datanode_name, const char* node_name, const char* node_path, int Row_offset, int Col_offset, const char* method, int times)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		method == NULL)
	{
		fprintf(stderr, "XMLFile_add_dem(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			if (strcmp(safe_rank(root), "complex-0.0") == 0 ||
				strcmp(safe_rank(root), "complex-1.0") == 0 ||
				strcmp(safe_rank(root), "complex-2.0") == 0 ||
				strcmp(safe_rank(root), "phase-1.0") == 0 ||
				strcmp(safe_rank(root), "phase-2.0") == 0 ||
				strcmp(safe_rank(root), "phase-3.0") == 0 ||
				strcmp(safe_rank(root), "dem-1.0") == 0)
				continue;
			else
				break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "dem");
		DataNode->SetAttribute("rank", "dem-1.0");

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("dem-1.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Method = new TiXmlElement("method");
		Method->LinkEndChild(new TiXmlText(method));
		Data_Processing_Parameters->LinkEndChild(Method);

		if (strcmp(method, "Iteration") == 0)
		{

			TiXmlElement* Iter_times = new TiXmlElement("iter_times");
			tmp = int2str(times);
			Iter_times->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Iter_times);
		}
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText("dem-1.0"));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_add_dem_14(
	int mode,
	const char* datanode_name,
	const char* node_name,
	const char* node_path,
	int Row_offset,
	int Col_offset,
	const char* method,
	int times
)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL ||
		method == NULL)
	{
		fprintf(stderr, "XMLFile_add_dem_14(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	char rank[256];
	sprintf(rank, "%d-dem-1.0", mode);
	if (!DataNode)
	{
		DataNode = new TiXmlElement("DataNode");
		DataNode->SetAttribute("name", datanode_name);
		int index = 1;
		TiXmlElement* root = impl_->doc.RootElement()->FirstChildElement()->NextSiblingElement();
		tmp = root->Value();
		for (; root != NULL; root = root->NextSiblingElement(), index++)
		{
			int ret1, ret2, ret3, mode1, mode2, mode3; double level;
			ret1 = sscanf(safe_rank(root), "%d-complex-%lf", &mode1, &level);
			ret2 = sscanf(safe_rank(root), "%d-phase-%lf", &mode2, &level);
			ret3 = sscanf(safe_rank(root), "%d-dem-%lf", &mode3, &level);
			if (ret1 == 2 || ret2 == 2 || (ret3 == 2 && level <= 1.0)) continue;
			else break;
		}
		string index_str = int2str(index);
		DataNode->SetAttribute("index", index_str.c_str());
		DataNode->SetAttribute("data_count", "1");
		DataNode->SetAttribute("data_processing", "dem");
		DataNode->SetAttribute("rank", rank);

		TiXmlElement* Data = new TiXmlElement("Data");
		DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText("1"));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);

		TiXmlElement* Data_Processing_Parameters = new TiXmlElement("Data_Processing_Parameters");
		DataNode->LinkEndChild(Data_Processing_Parameters);
		TiXmlElement* Method = new TiXmlElement("method");
		Method->LinkEndChild(new TiXmlText(method));
		Data_Processing_Parameters->LinkEndChild(Method);

		if (strcmp(method, "Iteration") == 0)
		{

			TiXmlElement* Iter_times = new TiXmlElement("iter_times");
			tmp = int2str(times);
			Iter_times->LinkEndChild(new TiXmlText(tmp.c_str()));
			Data_Processing_Parameters->LinkEndChild(Iter_times);
		}
		if (!root)
		{
			impl_->doc.RootElement()->LinkEndChild(DataNode);
		}
		else
		{
			impl_->doc.RootElement()->InsertBeforeChild(root, *DataNode);
			while (root)
			{
				index_str = int2str(++index);
				root->SetAttribute("index", index_str.c_str());
				root = root->NextSiblingElement();
			}

		}
	}
	else
	{
		string str = DataNode->Attribute("data_count");
		int index = str2int(str) + 1;
		TiXmlElement* p = NULL;
		tmp = int2str(index);
		DataNode->SetAttribute("data_count", tmp.c_str());
		TiXmlElement* LastNode = DataNode->LastChild()->ToElement();

		TiXmlElement* Data = new TiXmlElement("Data");

		int data_count = 0;
		ret = get_children_count(DataNode, &data_count);
		//DataNode->LinkEndChild(Data);
		TiXmlElement* Data_Name = new TiXmlElement("Data_Name");
		Data_Name->LinkEndChild(new TiXmlText(node_name));
		Data->LinkEndChild(Data_Name);
		TiXmlElement* Data_Rank = new TiXmlElement("Data_Rank");
		Data_Rank->LinkEndChild(new TiXmlText(rank));
		Data->LinkEndChild(Data_Rank);
		TiXmlElement* Data_Index = new TiXmlElement("Data_Index");
		Data_Index->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Data_Index);
		TiXmlElement* Data_Path = new TiXmlElement("Data_Path");
		Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Data_Path);
		/*TiXmlElement* Last_Data_Path = new TiXmlElement("Last_Data_Path");
		Last_Data_Path->LinkEndChild(new TiXmlText(node_path));
		Data->LinkEndChild(Last_Data_Path);*/
		TiXmlElement* Row_Offset = new TiXmlElement("Row_Offset");
		tmp = int2str(Row_offset);
		Row_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Row_Offset);
		TiXmlElement* Col_Offset = new TiXmlElement("Col_Offset");
		tmp = int2str(Col_offset);
		Col_Offset->LinkEndChild(new TiXmlText(tmp.c_str()));
		Data->LinkEndChild(Col_Offset);
		DataNode->InsertBeforeChild(LastNode, *Data);
	}
	return 0;
}

int XMLFile::XMLFile_remove_node(const char* datanode_name, const char* node_name, const char* node_path)
{
	if (datanode_name == NULL ||
		node_name == NULL ||
		node_path == NULL)
	{
		fprintf(stderr, "XMLFile_remove_node(): input check failed!\n");
		return -1;
	}
	TiXmlElement* DataNode = NULL;
	int ret = find_node_with_attribute(impl_->doc.RootElement(), "DataNode", "name", datanode_name, DataNode);
	string tmp;
	// removed unused: rank (planned rank attribute read, never implemented)
	if (DataNode)
	{
		tmp = DataNode->Attribute("data_count");
		int data_count = str2int(tmp);
		int delete_num = 0;
		TiXmlElement* Node = DataNode->FirstChildElement();
		TiXmlElement* Node_name = NULL;
		TiXmlElement* Node_index = NULL;
		while (Node)
		{
			if (!strcmp(Node->Value(), "Data"))
			{
				ret = _find_node(Node, "Data_Name", Node_name);
				if (!ret)
				{
					if (!strcmp(Node_name->FirstChild()->Value(), node_name))
					{
						data_count -= 1;
						delete_num += 1;
						TiXmlNode* Delete_Node = Node;
						Node = Node->NextSiblingElement();
						DataNode->RemoveChild(Delete_Node);
						Delete_Node = NULL;
					}
					else
					{
						ret = _find_node(Node, "Data_Index", Node_index);
						tmp = Node_index->FirstChild()->Value();
						int index = str2int(tmp);
						Node_index->FirstChild()->SetValue(int2str(index - delete_num).c_str());
						Node = Node->NextSiblingElement();
					}
				}
			}
			else
				break;
		}
		DataNode->SetAttribute("data_count", data_count);
	}

	return 0;
}

string XMLFile::double2str(double d)
{
	string sResult;
	stringstream ssTmp;
	ssTmp.imbue(std::locale::classic());
	ssTmp << std::setprecision(17) << d;
	sResult = ssTmp.str();
	return sResult;
}

string XMLFile::int2str(int n)
{
	string sResult;
	stringstream ssTmp;
	ssTmp << n;
	sResult = ssTmp.str();
	return sResult;
}

int XMLFile::str2int(const string& s)
{
	int n;
	stringstream ssTmp(s);
	ssTmp >> n;
	return n;
}

int XMLFile::XMLFile_save(const char* save_path)
{
	if (save_path == NULL)
	{
		fprintf(stderr, "XMLFile_save(): input check failed!\n");
		return -1;
	}
	impl_->doc.SaveFile(save_path);
	return 0;
}

int XMLFile::XMLFile_load(const char* xmlFileName)
{
	if (xmlFileName == NULL)
	{
		fprintf(stderr, "XMLFile_load(): input check failed!\n");
		return -1;
	}
	strcpy_s(impl_->m_xmlFileName, 2047, xmlFileName);
	if (!impl_->doc.LoadFile(xmlFileName))
	{
		fprintf(stderr, "XMLFile_load(): can't load XML file %s!\n", xmlFileName);
		return -1;
	}
	return 0;
}

int XMLFile::get_root(TiXmlElement*& root)
{
	root = impl_->doc.RootElement();
	return 0;
}

int XMLFile::get_children_count(TiXmlElement* pRoot, int* count)
{
	if (!pRoot)
	{
		fprintf(stderr, "get_children_count(): Node doesn't exist\n");
		return -1;
	}
	TiXmlElement* p;
	int tmp = 0;
	for (p = pRoot->FirstChildElement(); p != NULL; p = p->NextSiblingElement())
	{
		tmp++;
	}
	*count = tmp;
	return 0;
}

int XMLFile::_find_node(TiXmlElement* pRoot, const char* node_name, TiXmlElement*& pnode)
{
	if (node_name == NULL || pRoot == NULL)
	{
		fprintf(stderr, "find_node(): input check failed!\n");
		return -1;
	}
	const char* value = pRoot->Value();
	if (strcmp(pRoot->Value(), node_name) == 0)
	{
		pnode = pRoot;
		return 0;
	}

	TiXmlElement* p = pRoot;
	for (p = p->FirstChildElement(); p != NULL; p = p->NextSiblingElement())
	{
		if (0 == _find_node(p, node_name, pnode)) return 0;
	}

	return -1;
}

int XMLFile::find_node(const char* node_name, TiXmlElement*& pnode)
{
	if (node_name == NULL)
	{
		fprintf(stderr, "find_node(): input check failed!\n");
		return -1;
	}
	TiXmlElement* root = impl_->doc.RootElement();
	int ret = _find_node(root, node_name, pnode);
	if (ret < 0)
	{
		fprintf(stderr, "find_node(): node %s not found!\n", node_name);
		return -1;
	}
	return 0;
}

int XMLFile::find_node_with_attribute(TiXmlElement* pRoot, const char* node_name, const char* attribute_name, const char* attribute_value, TiXmlElement*& pnode)
{
	if (node_name == NULL ||
		attribute_name == NULL ||
		attribute_value == NULL)
	{
		fprintf(stderr, "find_node_with_attribute(): input check failed!\n");
		return -1;
	}
	const char* value = pRoot->Value();
	const char* attribute = pRoot->Attribute(attribute_name);
	if (strcmp(value, node_name) == 0 && strcmp(attribute, attribute_value) == 0)
	{
		pnode = pRoot;
		return 0;
	}

	TiXmlElement* p = pRoot;
	for (p = p->FirstChildElement(); p != NULL; p = p->NextSiblingElement())
	{
		if (0 == find_node_with_attribute(p, node_name, attribute_name, attribute_value, pnode)) return 0;
	}

	return -1;
}

int XMLFile::find_node_with_attribute(
	const char* node_name,
	const char* attribute_name,
	const char* attribute_value,
	TiXmlElement*& pnode
)
{
	if (node_name == NULL ||
		attribute_name == NULL ||
		impl_->doc.RootElement() == NULL ||
		attribute_value == NULL)
	{
		fprintf(stderr, "find_node_with_attribute(): input check failed!\n");
		return -1;
	}
	TiXmlElement* pRoot = impl_->doc.RootElement();
	const char* value = pRoot->Value();
	const char* attribute = pRoot->Attribute(attribute_name);
	if (strcmp(value, node_name) == 0 && strcmp(attribute, attribute_value) == 0)
	{
		pnode = pRoot;
		return 0;
	}

	TiXmlElement* p = pRoot;
	for (p = p->FirstChildElement(); p != NULL; p = p->NextSiblingElement())
	{
		if (0 == find_node_with_attribute(p, node_name, attribute_name, attribute_value, pnode)) return 0;
	}

	return -1;
}

int XMLFile::get_str_para(const char* node_name, string& value)
{
	if (node_name == NULL)
	{
		fprintf(stderr, "get_str_para(): input check failed!\n");
		return -1;
	}
	int ret;
	TiXmlElement* pnode = NULL;
	ret = find_node(node_name, pnode);
	if (return_check(ret, "get_str_para()", impl_->error_head)) return -1;
	if (pnode)
	{
		string x(pnode->GetText());
		value = x;
	}
	return 0;
}

int XMLFile::get_double_para(const char* node_name, double* value)
{
	if (node_name == NULL || value == NULL)
	{
		fprintf(stderr, "get_double_para(): input check failed!\n");
		return -1;
	}
	int ret;
	string tmp;
	ret = get_str_para(node_name, tmp);
	if (return_check(ret, "get_double_para()", impl_->error_head)) return -1;
	ret = sscanf(tmp.c_str(), "%lf", value);
	if (ret != 1)
	{
		fprintf(stderr, "get_double_para(): %s is unknown paramter format!\n", tmp.c_str());
		return -1;
	}
	return 0;
}

int XMLFile::getDoubleArray(const char* node_name, Mat& Array, TiXmlElement* rootNode)
{
	if (!node_name)
	{
		fprintf(stderr, "getDoubleArray(): input check failed!\n");
		return -1;
	}
	int ret, count;
	char* ptr = NULL;
	count = -1;
	TiXmlElement* pnode = NULL;
	if (rootNode)
	{
		ret = _find_node(rootNode, node_name, pnode);
	}
	else
	{
		ret = find_node(node_name, pnode);
	}
	if (return_check(ret, "getDoubleArray()", impl_->error_head)) return -1;
	if (pnode)
	{
		if (!pnode->FirstAttribute())
		{
			fprintf(stderr, "getDoubleArray(): no count attribute!\n");
			return -1;
		}
		ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &count);
		if (ret != 1 || count <= 0)
		{
			fprintf(stderr, "getDoubleArray(): no count attribute!\n");
			return -1;
		}
		Array.create(1, count, CV_64F);
		Array.at<double>(0, 0) = strtod(pnode->GetText(), &ptr);
		for (int i = 1; i < count; i++)
		{
			Array.at<double>(0, i) = strtod(ptr, &ptr);
		}

	}
	return 0;
}

int XMLFile::get_int_para(const char* node_name, int* value)
{
	if (node_name == NULL || value == NULL)
	{
		fprintf(stderr, "get_int_para(): input check failed!\n");
		return -1;
	}
	int ret;
	string tmp;
	ret = get_str_para(node_name, tmp);
	if (return_check(ret, "get_int_para()", impl_->error_head)) return -1;
	ret = sscanf(tmp.c_str(), "%d", value);
	if (ret != 1)
	{
		fprintf(stderr, "get_int_para(): %s is unknown paramter format!\n", tmp.c_str());
		return -1;
	}
	return 0;
}

int XMLFile::getIntArray(const char* node_name, Mat& Array, TiXmlElement* rootNode)
{
	if (!node_name)
	{
		fprintf(stderr, "getIntArray(): input check failed!\n");
		return -1;
	}
	int ret, count;
	char* ptr = NULL;
	count = -1;
	TiXmlElement* pnode = NULL;
	if (rootNode)
	{
		ret = _find_node(rootNode, node_name, pnode);
	}
	else
	{
		ret = find_node(node_name, pnode);
	}
	if (return_check(ret, "getIntArray()", impl_->error_head)) return -1;
	if (pnode)
	{
		if (!pnode->FirstAttribute())
		{
			fprintf(stderr, "getIntArray(): no count attribute!\n");
			return -1;
		}
		ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &count);
		if (ret != 1 || count <= 0)
		{
			fprintf(stderr, "getIntArray(): no count attribute!\n");
			return -1;
		}
		Array.create(1, count, CV_32S);
		Array.at<int>(0, 0) = strtol(pnode->GetText(), &ptr, 0);
		for (int i = 1; i < count; i++)
		{
			Array.at<int>(0, i) = strtol(ptr, &ptr, 0);
		}

	}
	return 0;
}

int XMLFile::get_gcps_from_TSX(Mat& gcps)
{
	/*
	* ��֤���ڵ�
	*/
	TiXmlElement* pRoot = NULL;
	pRoot = impl_->doc.RootElement();
	if (pRoot)
	{
		if (0 != strcmp(pRoot->Value(), "geoReference"))
		{
			fprintf(stderr, "get_gcps_from_TSX():  %s: unknown format!\n", impl_->m_xmlFileName);
			return -1;
		}
	}
	else
	{
		fprintf(stderr, "get_gcps_from_TSX():  %s: root element error!\n", impl_->m_xmlFileName);
		return -1;
	}
	/*
	* ȷ�����Ƶ����
	*/
	int n_gcps = 1;
	TiXmlElement* pnode = NULL;
	int ret = find_node("numberOfGridPoints", pnode);
	if (return_check(ret, "get_gcps_from_TSX()", impl_->error_head)) return -1;
	pnode = pnode->FirstChildElement();
	string tmp(pnode->GetText());
	ret = sscanf(tmp.c_str(), "%d", &n_gcps);
	if (ret != 1)
	{
		fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
		return -1;
	}
	Mat x(n_gcps, 6, CV_64F);
	/*
	* ��ȡ���Ƶ�
	*/
	ret = find_node("gridPoint", pnode);
	if (return_check(ret, "get_gcps_from_TSX()", impl_->error_head)) return -1;
	TiXmlElement* pchild = NULL;
	double lon, lat, height, row, col, inc;
	for (int i = 1; i <= n_gcps; i++)
	{
		if (!pnode) break;
		//��ȡ����
		ret = _find_node(pnode, "lon", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		tmp = pchild->GetText();
		ret = sscanf(tmp.c_str(), "%lf", &lon);
		if (ret != 1)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//��ȡγ��
		ret = _find_node(pnode, "lat", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		tmp = pchild->GetText();
		ret = sscanf(tmp.c_str(), "%lf", &lat);
		if (ret != 1)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//��ȡ�߶�
		ret = _find_node(pnode, "height", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		tmp = pchild->GetText();
		ret = sscanf(tmp.c_str(), "%lf", &height);
		if (ret != 1)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//��ȡ����
		ret = _find_node(pnode, "row", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		tmp = pchild->GetText();
		ret = sscanf(tmp.c_str(), "%lf", &row);
		if (ret != 1)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//��ȡ����
		ret = _find_node(pnode, "col", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		tmp = pchild->GetText();
		ret = sscanf(tmp.c_str(), "%lf", &col);
		if (ret != 1)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//��ȡ���ӽ�
		ret = _find_node(pnode, "inc", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		tmp = pchild->GetText();
		ret = sscanf(tmp.c_str(), "%lf", &inc);
		if (ret != 1)
		{
			fprintf(stderr, "get_gcps_from_TSX(): %s: unkown format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//��ֵ
		x.at<double>(i - 1, 0) = lon;
		x.at<double>(i - 1, 1) = lat;
		x.at<double>(i - 1, 2) = height;
		x.at<double>(i - 1, 3) = row;
		x.at<double>(i - 1, 4) = col;
		x.at<double>(i - 1, 5) = inc;
		//��һ���ڵ�
		pnode = pnode->NextSiblingElement();
	}
	x.copyTo(gcps);
	return 0;
}

int XMLFile::get_stateVec_from_TSX(Mat& stateVec)
{
	TiXmlElement* pRoot = NULL;
	pRoot = impl_->doc.RootElement();
	if (!pRoot)
	{
		fprintf(stderr, "get_stateVec_from_TSX(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}

	/*
	* ȷ��stateVec����
	*/

	TiXmlElement* pnode = NULL;
	int ret = _find_node(pRoot, "numStateVectors", pnode);
	if (ret < 0)
	{
		fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	int numstateVec;
	ret = sscanf(pnode->GetText(), "%d", &numstateVec);
	if (ret != 1)
	{
		fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	Mat x(numstateVec, 7, CV_64F);

	/*
	* �ҵ���һ��stateVec
	*/
	ret = _find_node(pRoot, "stateVec", pnode);
	if (ret < 0)
	{
		fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	TiXmlElement* pchild = NULL;
	double GPS_time, posX, posY, posZ, velX, velY, velZ;
	for (int i = 0; i < numstateVec; i++)
	{
		if (!pnode) break;
		//GPSʱ��
		ret = _find_node(pnode, "timeUTC", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		UTC2GPS(pchild->GetText(), &GPS_time);
		//ret = sscanf(pchild->GetText(), "%lf", &GPS_time);
		//if (ret != 1)
		//{
		//	fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
		//	return -1;
		//}
		//posX
		ret = _find_node(pnode, "posX", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &posX);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//posY
		ret = _find_node(pnode, "posY", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &posY);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//posZ
		ret = _find_node(pnode, "posZ", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &posZ);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//velX
		ret = _find_node(pnode, "velX", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &velX);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//velY
		ret = _find_node(pnode, "velY", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &velY);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//velZ
		ret = _find_node(pnode, "velZ", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		ret = sscanf(pchild->GetText(), "%lf", &velZ);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_TSX(): %s : unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}

		//��ֵ
		x.at<double>(i, 0) = GPS_time;
		x.at<double>(i, 1) = posX;
		x.at<double>(i, 2) = posY;
		x.at<double>(i, 3) = posZ;
		x.at<double>(i, 4) = velX;
		x.at<double>(i, 5) = velY;
		x.at<double>(i, 6) = velZ;
		//��һ���ڵ�
		pnode = pnode->NextSiblingElement();
	}
	x.copyTo(stateVec);
	return 0;
}

int XMLFile::get_dopplerCentroid_from_TSX(Mat& doppler)
{
	TiXmlElement* pnode = NULL;
	TiXmlElement* pchild = NULL;
	TiXmlElement* pchild2 = NULL;
	int ret, numberOfDopplerRecords, polynomialDegree;
	ret = find_node("numberOfDopplerRecords", pnode);
	if (return_check(ret, "find_node", impl_->error_head)) return -1;
	ret = sscanf(pnode->GetText(), "%d", &numberOfDopplerRecords);
	if (ret != 1)
	{
		fprintf(stderr, "get_dopplerCentroid_from_TSX(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	ret = find_node("dopplerEstimate", pnode);
	if (return_check(ret, "find_node", impl_->error_head)) return -1;
	ret = _find_node(pnode, "combinedDoppler", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "get_dopplerCentroid_from_TSX(): node combinedDoppler not found!\n");
		return -1;
	}
	ret = _find_node(pchild, "polynomialDegree", pnode);
	if (ret < 0)
	{
		fprintf(stderr, "get_dopplerCentroid_from_TSX(): node polynomialDegree not found!\n");
		return -1;
	}
	ret = sscanf(pnode->GetText(), "%d", &polynomialDegree);
	if (ret != 1)
	{
		fprintf(stderr, "get_dopplerCentroid_from_TSX(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}

	doppler.create(numberOfDopplerRecords, polynomialDegree + 2, CV_64F);
	ret = find_node("dopplerEstimate", pnode);
	if (return_check(ret, "find_node", impl_->error_head)) return -1;
	double ref_time, c;
	for (int i = 0; i < numberOfDopplerRecords; i++)
	{
		if (!pnode) break;
		ret = _find_node(pnode, "combinedDoppler", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_dopplerCentroid_from_TSX(): node combinedDoppler not found!\n");
			return -1;
		}
		ret = _find_node(pchild, "referencePoint", pchild2);
		if (ret < 0)
		{
			fprintf(stderr, "get_dopplerCentroid_from_TSX(): node referencePoint not found!\n");
			return -1;
		}
		ret = sscanf(pchild2->GetText(), "%lf", &ref_time);
		if (ret != 1)
		{
			fprintf(stderr, "get_dopplerCentroid_from_TSX(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		doppler.at<double>(i, 0) = ref_time;
		pchild = pchild2->NextSiblingElement()->NextSiblingElement();
		for (int j = 0; j < polynomialDegree + 1; j++)
		{
			if (!pchild) break;
			ret = sscanf(pchild->GetText(), "%lf", &c);
			if (ret != 1)
			{
				fprintf(stderr, "get_dopplerCentroid_from_TSX(): %s: unknown data format!\n", impl_->m_xmlFileName);
				return -1;
			}
			doppler.at<double>(i, j + 1) = c;
			pchild = pchild->NextSiblingElement();
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int XMLFile::get_gcps_from_sentinel(Mat& gcps)
{
	/*
	* ȷ�����Ƶ����
	*/
	int n_gcps = 1;
	TiXmlElement* pRoot = impl_->doc.RootElement();
	int ret;
	TiXmlElement* pnode = NULL;
	ret = _find_node(pRoot, "geolocationGridPointList", pnode);
	if (ret < 0)
	{
		fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node geolocationGridPointList not found!\n");
		return -1;
	}
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &n_gcps);
	if (ret != 1)
	{
		fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	gcps.create(n_gcps, 6, CV_64F);
	TiXmlElement* pchild = NULL;
	ret = _find_node(pnode, "geolocationGridPoint", pchild);
	if (ret < 0)
	{
		fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node geolocationGridPoint not found!\n");
		return -1;
	}
	double lon, lat, height, row, col, inc;
	for (int i = 0; i < n_gcps; i++)
	{
		if (!pchild) break;

		//longtitude
		ret = _find_node(pchild, "longitude", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node longitude not found!\n");
			return -1;
		}
		ret = sscanf(pnode->GetText(), "%lf", &lon);
		if (ret != 1)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}

		//latitude
		ret = _find_node(pchild, "latitude", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node latitude not found!\n");
			return -1;
		}
		ret = sscanf(pnode->GetText(), "%lf", &lat);
		if (ret != 1)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}

		//height
		ret = _find_node(pchild, "height", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node height not found!\n");
			return -1;
		}
		ret = sscanf(pnode->GetText(), "%lf", &height);
		if (ret != 1)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}

		//row
		ret = _find_node(pchild, "line", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node line not found!\n");
			return -1;
		}
		ret = sscanf(pnode->GetText(), "%lf", &row);
		if (ret != 1)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}

		//col
		ret = _find_node(pchild, "pixel", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node pixel not found!\n");
			return -1;
		}
		ret = sscanf(pnode->GetText(), "%lf", &col);
		if (ret != 1)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//incidence angle
		ret = _find_node(pchild, "incidenceAngle", pnode);
		if (ret < 0)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): node pixel not found!\n");
			return -1;
		}
		ret = sscanf(pnode->GetText(), "%lf", &inc);
		if (ret != 1)
		{
			fprintf(stderr, "XMLFile::get_gcps_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//assignment
		gcps.at<double>(i, 0) = lon;
		gcps.at<double>(i, 1) = lat;
		gcps.at<double>(i, 2) = height;
		gcps.at<double>(i, 3) = row;
		gcps.at<double>(i, 4) = col;
		gcps.at<double>(i, 5) = inc;
		pchild = pchild->NextSiblingElement();
	}
	return 0;
}

int XMLFile::get_dopplerCentroid_from_sentinel(Mat& doppler)
{
	TiXmlElement* pnode, * pchild1;
	// removed unused: pchild2 (copy-paste remnant from similar XML parsing)
	int ret, numOfDcEstimates, polynomialDegree;
	ret = find_node("dopplerCentroid", pnode);
	if (return_check(ret, "find_node", impl_->error_head)) return -1;
	ret = sscanf(pnode->FirstChildElement()->FirstAttribute()->Value(), "%d", &numOfDcEstimates);
	if (ret != 1)
	{
		fprintf(stderr, "get_dopplerCentroid_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	ret = _find_node(pnode, "dataDcPolynomial", pchild1);
	if (ret < 0)
	{
		fprintf(stderr, "get_dopplerCentroid_from_sentinel(): node dataDcPolynomial not found!\n");
		return -1;
	}
	ret = sscanf(pchild1->FirstAttribute()->Value(), "%d", &polynomialDegree);
	if (ret != 1)
	{
		fprintf(stderr, "get_dopplerCentroid_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}
	doppler.create(numOfDcEstimates, polynomialDegree + 1, CV_64F);
	double t0, c;
	char* ptr;
	ret = find_node("dcEstimate", pnode);
	if (return_check(ret, "find_node", impl_->error_head)) return -1;
	for (int i = 0; i < numOfDcEstimates; i++)
	{
		if (!pnode) break;
		ret = _find_node(pnode, "t0", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_dopplerCentroid_from_sentinel(): node t0 not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &t0);
		if (ret != 1)
		{
			fprintf(stderr, "get_dopplerCentroid_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		doppler.at<double>(i, 0) = t0;
		ret = _find_node(pnode, "dataDcPolynomial", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_dopplerCentroid_from_sentinel(): node dataDcPolynomial not found!\n");
			return -1;
		}
		c = strtod(pchild1->GetText(), &ptr);
		doppler.at<double>(i, 1) = c;
		for (int j = 1; j < polynomialDegree; j++)
		{
			c = strtod(ptr, &ptr);
			doppler.at<double>(i, j + 1) = c;
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int XMLFile::get_stateVec_from_sentinel(Mat& stateVec)
{
	TiXmlElement* pnode, * pchild, * pchild1;
	FormatConversion conversion;
	int ret, numOfstateVec;
	ret = find_node("orbitList", pnode);
	if (return_check(ret, "find_node()", impl_->error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfstateVec);
	if (ret != 1)
	{
		fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
		return -1;
	}

	ret = find_node("orbit", pnode);
	if (return_check(ret, "find_node()", impl_->error_head)) return -1;
	double time, x, y, z, vx, vy, vz;
	stateVec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPSʱ��
		ret = _find_node(pnode, "time", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node time not found!\n");
			return -1;
		}
		ret = conversion.utc2gps(pchild->GetText(), &time);
		if (return_check(ret, "utc2gps()", impl_->error_head)) return -1;
		//λ��x
		ret = _find_node(pnode, "position", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node position not found!\n");
			return -1;
		}
		ret = _find_node(pchild, "x", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node x not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &x);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//λ��y
		ret = _find_node(pchild, "y", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node y not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &y);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//λ��z
		ret = _find_node(pchild, "z", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node z not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &z);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//�ٶ�x
		ret = _find_node(pnode, "velocity", pchild);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node velocity not found!\n");
			return -1;
		}
		ret = _find_node(pchild, "x", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node x not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &vx);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//�ٶ�y
		ret = _find_node(pchild, "y", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node y not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &vy);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}
		//�ٶ�z
		ret = _find_node(pchild, "z", pchild1);
		if (ret < 0)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): node z not found!\n");
			return -1;
		}
		ret = sscanf(pchild1->GetText(), "%lf", &vz);
		if (ret != 1)
		{
			fprintf(stderr, "get_stateVec_from_sentinel(): %s: unknown data format!\n", impl_->m_xmlFileName);
			return -1;
		}

		//��ֵ
		stateVec.at<double>(i, 0) = time;
		stateVec.at<double>(i, 1) = x;
		stateVec.at<double>(i, 2) = y;
		stateVec.at<double>(i, 3) = z;
		stateVec.at<double>(i, 4) = vx;
		stateVec.at<double>(i, 5) = vy;
		stateVec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int FormatConversion::Copy_para_from_h5_2_h5(const char* Input_file, const char* Output_file)
{
	if (!Input_file || !*Input_file || !Output_file || !*Output_file) return Hdf5IO::kInvalidArgument;

	// Keep the conservative global HDF5 lock across identity validation and both
	// copy batches. A lock only serializes time; it never makes self-copy valid.
	Hdf5BatchGuard hdf5Batch;
	if (hdf5Batch.status() != 0) return hdf5Batch.status();
	int sameFile = 0;
	if (Hdf5IO::areSameExistingFile(Input_file, Output_file, &sameFile) != 0) return -1;
	if (sameFile != 0) return Hdf5IO::kInvalidArgument;

	static const char* const stringDatasets[] = {
		"file_type", "sensor", "polarization", "imaging_mode", "lookside", "orbit_dir", "swath",
		"acquisition_start_time", "acquisition_stop_time", "source_1", "source_2",
		"source_path_encoding", "source_path_format_version" };
	static const char* const arrayDatasets[] = {
		"orbit_altitude", "carrier_frequency", "heading", "prf", "inc_center", "gcps",
		"azimuth_resolution", "range_resolution", "azimuth_spacing", "range_spacing", "state_vec",
		"fine_state_vec", "doppler_centroid", "doppler_coefficient_a", "doppler_coefficient_b",
		"lon_coefficient", "lat_coefficient", "row_coefficient", "col_coefficient", "inc_coefficient",
		"inc_coefficient_r", "inc_center", "row_coefficient", "slant_range_first_pixel", "topLeftLon",
		"topLeftLat", "topRightLon", "topRightLat", "bottomLeftLon", "bottomLeftLat", "bottomRightLon",
		"bottomRightLat", "TR_mode" };

	int result = Hdf5IO::copyDatasetsIfPresent(Input_file, Output_file, stringDatasets,
		static_cast<int>(sizeof(stringDatasets) / sizeof(stringDatasets[0])), false);
	if (result != 0) return result;
	result = Hdf5IO::copyDatasetsIfPresent(Input_file, Output_file, arrayDatasets,
		static_cast<int>(sizeof(arrayDatasets) / sizeof(arrayDatasets[0])), true);
	if (result != 0) return result;

	std::string source1;
	std::string source2;
	const int source1Status = Hdf5IO::readString(Output_file, "source_1", source1);
	const int source2Status = Hdf5IO::readString(Output_file, "source_2", source2);
	if ((source1Status == 0) != (source2Status == 0)) return -1;
	if (source1Status != 0) return 0;
	std::wstring source1Wide;
	std::wstring source2Wide;
	PathResolver::Error pathError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(source1, source1Wide, &pathError) ||
		!PathResolver::utf8ToWide(source2, source2Wide, &pathError)) return -1;
	if (Hdf5IO::writeString(Output_file, "source_path_encoding", "UTF-8") != 0) return -1;
	return Hdf5IO::writeString(Output_file, "source_path_format_version", "2");
}
int FormatConversion::read_height_metric_from_GEDI_L2B(
	const char* gedi_h5_file,
	Mat& rh100,
	Mat& elev_lowestmode,
	Mat& elev_highestreturn,
	Mat& lon,
	Mat& lat,
	Mat& dem,
	Mat& quality_index
)
{
	if (!gedi_h5_file) return -1;
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(gedi_h5_file), Hdf5IO::closeReadSession);
	if (!session) return -1;

	static const char* const beams[] = {
		"/BEAM0000/", "/BEAM0001/", "/BEAM0010/", "/BEAM0011/",
		"/BEAM0101/", "/BEAM0110/", "/BEAM1000/", "/BEAM1011/" };
	int count = 0;
	for (const char* beam : beams)
	{
		const std::string prefix(beam);
		Mat rh100Tmp, lowestModeTmp, highestReturnTmp, lonTmp, latTmp, demTmp, qualityTmp;
		if (Hdf5IO::readArray(session.get(), (prefix + "rh100").c_str(), CV_16S, rh100Tmp) != 0) continue;
		if (Hdf5IO::readArray(session.get(), (prefix + "geolocation/elev_lowestmode").c_str(), CV_32F, lowestModeTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "geolocation/elev_highestreturn").c_str(), CV_32F, highestReturnTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "geolocation/lat_highestreturn").c_str(), CV_64F, latTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "geolocation/lon_highestreturn").c_str(), CV_64F, lonTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "geolocation/digital_elevation_model").c_str(), CV_32F, demTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "l2b_quality_flag").c_str(), CV_8U, qualityTmp) != 0) return -1;

		if (++count == 1)
		{
			rh100Tmp.copyTo(rh100);
			lowestModeTmp.copyTo(elev_lowestmode);
			highestReturnTmp.copyTo(elev_highestreturn);
			lonTmp.copyTo(lon);
			latTmp.copyTo(lat);
			demTmp.copyTo(dem);
			qualityTmp.copyTo(quality_index);
		}
		else
		{
			cv::vconcat(rh100, rh100Tmp, rh100);
			cv::vconcat(elev_lowestmode, lowestModeTmp, elev_lowestmode);
			cv::vconcat(elev_highestreturn, highestReturnTmp, elev_highestreturn);
			cv::vconcat(lon, lonTmp, lon);
			cv::vconcat(lat, latTmp, lat);
			cv::vconcat(dem, demTmp, dem);
			cv::vconcat(quality_index, qualityTmp, quality_index);
		}
	}
	quality_index.convertTo(quality_index, CV_16S);
	return 0;
}
int FormatConversion::read_height_metric_from_GEDI_L2A(const char* gedi_h5_file, Mat& rh, Mat& lon, Mat& lat, Mat& dem, Mat& quality_index, int rh_percentile)
{
	if (!gedi_h5_file || rh_percentile < 1 || rh_percentile > 100) return -1;
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(gedi_h5_file), Hdf5IO::closeReadSession);
	if (!session) return -1;

	static const char* const beams[] = {
		"/BEAM0000/", "/BEAM0001/", "/BEAM0010/", "/BEAM0011/",
		"/BEAM0101/", "/BEAM0110/", "/BEAM1000/", "/BEAM1011/" };
	int count = 0;
	for (const char* beam : beams)
	{
		const std::string prefix(beam);
		Mat rhTmp, lonTmp, latTmp, demTmp, qualityTmp;
		if (Hdf5IO::readArray(session.get(), (prefix + "rh").c_str(), CV_32F, rhTmp) != 0) continue;
		if (rhTmp.cols <= rh_percentile ||
			Hdf5IO::readArray(session.get(), (prefix + "lat_highestreturn").c_str(), CV_64F, latTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "lon_highestreturn").c_str(), CV_64F, lonTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "digital_elevation_model").c_str(), CV_32F, demTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "quality_flag").c_str(), CV_8U, qualityTmp) != 0) return -1;
		rhTmp.col(rh_percentile).copyTo(rhTmp);

		if (++count == 1)
		{
			rhTmp.copyTo(rh); lonTmp.copyTo(lon); latTmp.copyTo(lat); demTmp.copyTo(dem); qualityTmp.copyTo(quality_index);
		}
		else
		{
			cv::vconcat(rh, rhTmp, rh); cv::vconcat(lon, lonTmp, lon); cv::vconcat(lat, latTmp, lat);
			cv::vconcat(dem, demTmp, dem); cv::vconcat(quality_index, qualityTmp, quality_index);
		}
	}
	quality_index.convertTo(quality_index, CV_16S);
	return 0;
}
int FormatConversion::read_height_metric_from_ICESat_2_L3A(const char* ICESat_2_h5_file, Mat& rh, Mat& lon, Mat& lat, Mat& dem, Mat& quality_index, int rh_percentile)
{
	if (!ICESat_2_h5_file || rh_percentile < 1 || rh_percentile > 18) return -1;
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(ICESat_2_h5_file), Hdf5IO::closeReadSession);
	if (!session) return -1;

	static const char* const beams[] = { "/gt1l/", "/gt1r/", "/gt2l/", "/gt2r/", "/gt3l/", "/gt3r/" };
	int count = 0;
	for (const char* beam : beams)
	{
		const std::string prefix(beam);
		Mat rhTmp, lonTmp, latTmp, demTmp, qualityTmp;
		if (Hdf5IO::readArray(session.get(), (prefix + "land_segments/canopy/canopy_h_metrics").c_str(), CV_32F, rhTmp) != 0) continue;
		if (rhTmp.cols < rh_percentile ||
			Hdf5IO::readArray(session.get(), (prefix + "land_segments/latitude").c_str(), CV_32F, latTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "land_segments/longitude").c_str(), CV_32F, lonTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "land_segments/dem_h").c_str(), CV_32F, demTmp) != 0 ||
			Hdf5IO::readArray(session.get(), (prefix + "land_segments/canopy/can_quality_score").c_str(), CV_8U, qualityTmp) != 0) return -1;
		rhTmp.col(rh_percentile - 1).copyTo(rhTmp);

		if (++count == 1)
		{
			rhTmp.copyTo(rh); lonTmp.copyTo(lon); latTmp.copyTo(lat); demTmp.copyTo(dem); qualityTmp.copyTo(quality_index);
		}
		else
		{
			cv::vconcat(rh, rhTmp, rh); cv::vconcat(lon, lonTmp, lon); cv::vconcat(lat, latTmp, lat);
			cv::vconcat(dem, demTmp, dem); cv::vconcat(quality_index, qualityTmp, quality_index);
		}
	}
	quality_index.convertTo(quality_index, CV_16S);
	return 0;
}
int FormatConversion::ell2xyz(double lon, double lat, double elevation, Position& xyz)
{
	if (fabs(lon) > 180.0 || fabs(lat) > 90.0)
	{
		fprintf(stderr, "ell2xyz(): input check failed!\n");
		return -1;
	}
	const double epsilon = 0.000000000000001;
	const double pi = 3.14159265358979323846;
	const double d2r = pi / 180;
	const double r2d = 180 / pi;
	const double a = 6378137.0;		//���򳤰���
	const double f_inverse = 298.257223563;			//���ʵ���
	const double b = a - a / f_inverse;
	const double e = sqrt(a * a - b * b) / a;
	double y = lat;
	double x = lon;
	double z = elevation;
	double L = x * d2r;
	double B = y * d2r;
	double H = z;
	double N = a / sqrt(1 - e * e * sin(B) * sin(B));
	x = (N + H) * cos(B) * cos(L);
	y = (N + H) * cos(B) * sin(L);
	z = (N * (1 - e * e) + H) * sin(B);
	xyz.x = x;
	xyz.y = y;
	xyz.z = z;
	return 0;
}

int FormatConversion::phase2cos(const Mat& phase, Mat& cos, Mat& sin)
{
	if (phase.rows < 1 ||
		phase.cols < 1 ||
		(phase.type() != CV_64F && phase.type() != CV_32F) ||
		phase.channels() != 1)
	{
		fprintf(stderr, "phase2cos(): input check failed!\n\n");
		return -1;
	}
	int nr = phase.rows;
	int nc = phase.cols;
	if (phase.type() == CV_32F)
	{
		cos.create(nr, nc, CV_32F);
		sin.create(nr, nc, CV_32F);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				cos.at<float>(i, j) = std::cos(phase.at<float>(i, j));
				sin.at<float>(i, j) = std::sin(phase.at<float>(i, j));
			}
		}
	}
	else
	{
		cos.create(nr, nc, CV_64F);
		sin.create(nr, nc, CV_64F);
#pragma omp parallel for schedule(guided)
		for (int i = 0; i < nr; i++)
		{
			for (int j = 0; j < nc; j++)
			{
				cos.at<double>(i, j) = std::cos(phase.at<double>(i, j));
				sin.at<double>(i, j) = std::sin(phase.at<double>(i, j));
			}
		}
	}
	return 0;
}

int FormatConversion::createVandermondeMatrix(Mat& inArray, Mat& vandermondeMatrix, int degree)
{
	if (inArray.cols != 1 || inArray.rows < 1 || degree < 1)
	{
		fprintf(stderr, "createVandermondeMatrix(): input check failed!\n");
		return -1;
	}
	vandermondeMatrix.create(inArray.rows, degree + 1, CV_64F);
	if (inArray.type() != CV_64F) inArray.convertTo(inArray, CV_64F);
	for (int i = 0; i < inArray.rows; i++)
	{
		for (int j = 0; j < degree + 1; j++)
		{
			vandermondeMatrix.at<double>(i, j) = pow(inArray.at<double>(i, 0), (double)j);
		}
	}
	return 0;
}

int FormatConversion::polyFit(Mat& a, Mat& B, Mat& x)
{
	Mat A, b;
	a.copyTo(A);
	B.copyTo(b);
	if (A.rows != b.rows || A.cols > A.rows || A.empty())
	{
		fprintf(stderr, "polyFit(): input check failed!\n");
		return -1;
	}
	if (A.type() != CV_64F) A.convertTo(A, CV_64F);
	if (b.type() != CV_64F) b.convertTo(b, CV_64F);
	Mat A_t;
	cv::transpose(A, A_t);
	A = A_t * A;
	b = A_t * b;
	if (!cv::solve(A, b, x, cv::DECOMP_LU))
	{
		fprintf(stderr, "polyFit(): matrix deficiency!\n");
		return -1;
	}
	return 0;
}

int FormatConversion::polyVal(Mat& coefficient, double x, double* val)
{
	if (!val || coefficient.rows < 1 || coefficient.cols != 1) return -1;
	double sum = 0.0;
	if (coefficient.type() != CV_64F) coefficient.convertTo(coefficient, CV_64F);
	for (int i = 0; i < coefficient.rows; i++)
	{
		sum += coefficient.at<double>(i, 0) * pow(x, (double)i);
	}
	*val = sum;
	return 0;
}

int FormatConversion::real_coherent(const ComplexMat& Master, const ComplexMat& Slave, int* offset_row, int* offset_col)
{
	double d_row = 0.0, d_col = 0.0;
	int ret = real_coherent(Master, Slave, &d_row, &d_col, nullptr);
	if (ret == 0 && offset_row && offset_col) {
		*offset_row = static_cast<int>(std::round(d_row));
		*offset_col = static_cast<int>(std::round(d_col));
	}
	return ret;
}

int FormatConversion::real_coherent(const ComplexMat& Master, const ComplexMat& Slave, double* offset_row, double* offset_col, double* snr)
{
	if (Master.GetRows() < 1 ||
		Master.GetCols() < 1 ||
		Master.GetRows() != Slave.GetRows() ||
		Master.GetCols() != Slave.GetCols() ||
		offset_row == nullptr ||
		offset_col == nullptr)
	{
		fprintf(stderr, "real_coherent(): input check failed!\n\n");
		return -1;
	}

	int ret;
	Mat img1 = Master.GetMod();
	Mat img2 = Slave.GetMod();

	Mat fft1, fft2_mat;
	ret = fft2(img1, fft1);
	if (return_check(ret, "fft2(*, *)", error_head)) return -1;
	img1.release();

	ret = fft2(img2, fft2_mat);
	if (return_check(ret, "fft2(*, *)", error_head)) return -1;
	img2.release();

	mulSpectrums(fft1, fft2_mat, fft1, 0, true);
	fft2_mat.release();

	Mat corr;
	idft(fft1, corr, cv::DFT_REAL_OUTPUT);
	fft1.release();

	corr.convertTo(corr, CV_64F);

	double maxVal;
	cv::Point peak_loc;
	minMaxLoc(corr, nullptr, &maxVal, nullptr, &peak_loc);

	const int rows = corr.rows;
	const int cols = corr.cols;
	const int half_r = rows / 2;
	const int half_c = cols / 2;

	double sub_dx = 0.0;
	double sub_dy = 0.0;
	int x = peak_loc.x;
	int y = peak_loc.y;

	int left_x = (x - 1 + cols) % cols;
	int right_x = (x + 1) % cols;
	int up_y = (y - 1 + rows) % rows;
	int down_y = (y + 1) % rows;

	double c1_x = corr.at<double>(y, left_x);
	double c2_x = corr.at<double>(y, x);
	double c3_x = corr.at<double>(y, right_x);
	double denom_x = 2.0 * (c1_x - 2.0 * c2_x + c3_x);
	if (std::abs(denom_x) > 1e-6) sub_dx = (c1_x - c3_x) / denom_x;

	double c1_y = corr.at<double>(up_y, x);
	double c2_y = corr.at<double>(y, x);
	double c3_y = corr.at<double>(down_y, x);
	double denom_y = 2.0 * (c1_y - 2.0 * c2_y + c3_y);
	if (std::abs(denom_y) > 1e-6) sub_dy = (c1_y - c3_y) / denom_y;

	double y_cont = y + sub_dy;
	double x_cont = x + sub_dx;

	*offset_row = (y_cont <= half_r) ? (-y_cont) : (rows - y_cont);
	*offset_col = (x_cont <= half_c) ? (-x_cont) : (cols - x_cont);

	if (snr != nullptr)
	{
		cv::Scalar mean, stddev;
		meanStdDev(corr, mean, stddev);
		if (stddev[0] > 1e-6)
			*snr = (maxVal - mean[0]) / stddev[0];
		else
			*snr = 0.0;
	}

	return 0;
}

int FormatConversion::fftshift2(Mat& matrix)
{
	if (matrix.rows < 2 ||
		matrix.cols < 2 ||
		matrix.channels() != 1)
	{
		fprintf(stderr, "fftshift2(): input check failed!\n\n");
		return -1;
	}
	matrix = matrix(cv::Rect(0, 0, matrix.cols & -2, matrix.rows & -2));
	int cx = matrix.cols / 2;
	int cy = matrix.rows / 2;
	Mat tmp;
	Mat q0(matrix, cv::Rect(0, 0, cx, cy));
	Mat q1(matrix, cv::Rect(cx, 0, cx, cy));
	Mat q2(matrix, cv::Rect(0, cy, cx, cy));
	Mat q3(matrix, cv::Rect(cx, cy, cx, cy));

	q0.copyTo(tmp);
	q3.copyTo(q0);
	tmp.copyTo(q3);

	q1.copyTo(tmp);
	q2.copyTo(q1);
	tmp.copyTo(q2);
	return 0;
}

int FormatConversion::fft2(Mat& Src, Mat& Dst)
{
	if (Src.rows < 1 ||
		Src.cols < 1 ||
		Src.channels() != 1 ||
		Src.type() != CV_64F)
	{
		fprintf(stderr, "fft2(): input check failed!\n\n");
		return -1;
	}
	Mat planes[] = { cv::Mat_<double>(Src), Mat::zeros(Src.size(), CV_64F) };
	Mat complexImg;
	merge(planes, 2, complexImg);
	dft(complexImg, Dst);
	return 0;
}

/*------------------------------------------------*/
/*               �ڱ�һ�����ݶ�ȡ����             */
/*------------------------------------------------*/
Sentinel1Reader::Sentinel1Reader()
{
	bXmlLoad = false;
	isDataAvailable = false;
	memset(m_xmlFileName, 0, 2048);
	memset(this->error_head, 0, 256);
	strcpy(this->error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using ");
	this->azimuthPixelSpacing = 0.0;
	this->azimuthSteeringRate = 0.0;
	this->azimuthTimeInterval = 0.0;
	this->numberOfLines = 0;
	this->headingAngle = 0.0;
	this->numberOfSamples = 0;
	this->burstCount = 0;
	this->linesPerBurst = 0;
	this->pass = "";
	this->polarization = "";
	this->radarFrequency = 0.0;
	this->rangePixelSpacing = 0.0;
	this->rangeSamplingRate = 0.0;
	this->sensor = "sentinel";
	this->swath = "";
	this->slantRangeTime = 0.0;

}

Sentinel1Reader::Sentinel1Reader(const char* xmlfile, const char* tiffFile, const char* PODFile)
{
	bXmlLoad = false;
	isDataAvailable = false;
	memset(m_xmlFileName, 0, 2048);
	memset(this->error_head, 0, 256);
	strcpy(this->error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using ");
	this->azimuthPixelSpacing = 0.0;
	this->azimuthSteeringRate = 0.0;
	this->azimuthTimeInterval = 0.0;
	this->numberOfLines = 0;
	this->headingAngle = 0.0;
	this->numberOfSamples = 0;
	this->burstCount = 0;
	this->linesPerBurst = 0;
	this->pass = "";
	this->polarization = "";
	this->radarFrequency = 0.0;
	this->rangePixelSpacing = 0.0;
	this->rangeSamplingRate = 0.0;
	this->sensor = "sentinel";
	this->swath = "";
	this->slantRangeTime = 0.0;
	this->tiffFile = tiffFile;
	if (PODFile) this->PODFile = PODFile;
	if (!xmldoc.XMLFile_load(xmlfile)) {
		bXmlLoad = true;
		std::strcpy(this->m_xmlFileName, xmlfile);
	}
}

Sentinel1Reader::~Sentinel1Reader()
{

}

int Sentinel1Reader::load(const char* xmlfile, const char* tiffFile)
{
	if (bXmlLoad) return 0;//ֻ�������1��
	this->tiffFile = tiffFile;
	if (!xmldoc.XMLFile_load(xmlfile)) {
		bXmlLoad = true;
		std::strcpy(this->m_xmlFileName, xmlfile);
	}
	else
	{
		fprintf(stderr, "Sentinel1Reader::load(): can't load file %s\n", xmlfile);
		return -1;
	}
	return 0;
}

int Sentinel1Reader::getDcEstimateList()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getDcEstimateList(): input check failed!\n");
		return -1;
	}
	TiXmlElement* pnode, * pchild1;
	// removed unused: pchild2 (copy-paste remnant from similar XML parsing)
	int ret, numOfDcEstimates, polynomialDegree;
	ret = xmldoc.find_node("dopplerCentroid", pnode);
	if (return_check(ret, "find_node", error_head)) return -1;
	ret = sscanf(pnode->FirstChildElement()->FirstAttribute()->Value(), "%d", &numOfDcEstimates);
	ret = xmldoc._find_node(pnode, "dataDcPolynomial", pchild1);
	ret = sscanf(pchild1->FirstAttribute()->Value(), "%d", &polynomialDegree);
	DcEstimateList.create(numOfDcEstimates, polynomialDegree + 2, CV_64F);
	double t0, c;
	char* ptr;
	ret = xmldoc.find_node("dcEstimate", pnode);
	if (return_check(ret, "find_node", error_head)) return -1;
	for (int i = 0; i < numOfDcEstimates; i++)
	{
		if (!pnode) break;

		ret = xmldoc._find_node(pnode, "azimuthTime", pchild1);
		UTC2GPS(pchild1->GetText(), &t0);
		DcEstimateList.at<double>(i, 0) = t0;

		ret = xmldoc._find_node(pnode, "t0", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &t0);
		DcEstimateList.at<double>(i, 1) = t0;

		ret = xmldoc._find_node(pnode, "dataDcPolynomial", pchild1);
		c = strtod(pchild1->GetText(), &ptr);
		DcEstimateList.at<double>(i, 2) = c;
		for (int j = 1; j < polynomialDegree; j++)
		{
			c = strtod(ptr, &ptr);
			DcEstimateList.at<double>(i, j + 2) = c;
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getAzimuthFmRateList()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getAzimuthFmRateList(): input check failed!\n");
		return -1;
	}
	TiXmlElement* pnode, * pchild1;
	// removed unused: pchild2 (copy-paste remnant from similar XML parsing)
	int ret, numOfFmEstimates, polynomialDegree;
	ret = xmldoc.find_node("azimuthFmRateList", pnode);
	if (return_check(ret, "find_node", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfFmEstimates);
	ret = xmldoc._find_node(pnode, "azimuthFmRatePolynomial", pchild1);
	ret = sscanf(pchild1->FirstAttribute()->Value(), "%d", &polynomialDegree);
	AzimuthFmRateList.create(numOfFmEstimates, polynomialDegree + 2, CV_64F);
	double t0, c;
	char* ptr;
	ret = xmldoc.find_node("azimuthFmRate", pnode);
	if (return_check(ret, "find_node", error_head)) return -1;
	for (int i = 0; i < numOfFmEstimates; i++)
	{
		if (!pnode) break;

		ret = xmldoc._find_node(pnode, "azimuthTime", pchild1);
		UTC2GPS(pchild1->GetText(), &t0);
		AzimuthFmRateList.at<double>(i, 0) = t0;

		ret = xmldoc._find_node(pnode, "t0", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &t0);
		AzimuthFmRateList.at<double>(i, 1) = t0;

		ret = xmldoc._find_node(pnode, "azimuthFmRatePolynomial", pchild1);
		c = strtod(pchild1->GetText(), &ptr);
		AzimuthFmRateList.at<double>(i, 2) = c;
		for (int j = 1; j < polynomialDegree; j++)
		{
			c = strtod(ptr, &ptr);
			AzimuthFmRateList.at<double>(i, j + 2) = c;
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getAntennaPattern()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getAntennaPattern(): input check failed!\n");
		return -1;
	}
	TiXmlElement* pnode, * pchild1;
	// removed unused: pchild2 (copy-paste remnant from similar XML parsing)
	int ret = xmldoc.find_node("antennaPatternList", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	int count = -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &count);
	ret = xmldoc._find_node(pnode, "slantRangeTime", pchild1);
	int count2 = -1;
	ret = sscanf(pchild1->FirstAttribute()->Value(), "%d", &count2);
	antennaPattern_slantRangeTime.create(count, count2, CV_64F);
	antennaPattern_elevationAngle.create(count, count2, CV_64F);
	Mat Array;
	ret = xmldoc._find_node(pnode, "antennaPattern", pchild1);
	for (int i = 0; i < count; i++)
	{
		if (!pchild1) break;
		ret = xmldoc.getDoubleArray("slantRangeTime", Array, pchild1);
		Array.copyTo(antennaPattern_slantRangeTime(cv::Range(i, i + 1), cv::Range(0, count2)));
		ret = xmldoc.getDoubleArray("elevationAngle", Array, pchild1);
		Array.copyTo(antennaPattern_elevationAngle(cv::Range(i, i + 1), cv::Range(0, count2)));
		pchild1 = pchild1->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getBurstCount(int* burstCount)
{
	if (!bXmlLoad || !burstCount)
	{
		fprintf(stderr, "Sentinel1Reader::getBurstCount(): input check failed!\n");
		return -1;
	}
	int ret;
	TiXmlElement* pnode;
	ret = xmldoc.find_node("burstList", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", burstCount);
	return 0;
}

int Sentinel1Reader::getBurstAzimuthTime()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getBurstAzimuthTime(): input check failed!\n");
		return -1;
	}
	int ret, burstCount; double t;
	TiXmlElement* pnode, * pchild;
	ret = getBurstCount(&burstCount);
	burstAzimuthTime.create(burstCount, 1, CV_64F);
	ret = xmldoc.find_node("burst", pnode);
	for (int i = 0; i < burstCount; i++)
	{
		if (!pnode) break;
		ret = xmldoc._find_node(pnode, "azimuthTime", pchild);
		UTC2GPS(pchild->GetText(), &t);
		burstAzimuthTime.at<double>(i, 0) = t;
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getFirstValidSample()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getFirstValidSample(): input check failed!\n");
		return -1;
	}
	int ret, burstCount;
	TiXmlElement* pnode;
	Mat tmp;
	ret = getBurstCount(&burstCount);
	if (return_check(ret, "getBurstCount()", error_head)) return -1;
	firstValidSample.create(burstCount, 1, CV_32S);
	ret = xmldoc.find_node("burst", pnode);
	for (int i = 0; i < burstCount; i++)
	{
		if (!pnode) break;
		xmldoc.getIntArray("firstValidSample", tmp, pnode);
		for (int j = 0; j < tmp.cols; j++)
		{
			if (tmp.at<int>(0, j) != -1)
			{
				firstValidSample.at<int>(i, 0) = tmp.at<int>(0, j);
				break;
			}
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getLastValidSample()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getLastValidSample(): input check failed!\n");
		return -1;
	}
	int ret, burstCount;
	TiXmlElement* pnode;
	Mat tmp;
	ret = getBurstCount(&burstCount);
	if (return_check(ret, "getBurstCount()", error_head)) return -1;
	lastValidSample.create(burstCount, 1, CV_32S);
	ret = xmldoc.find_node("burst", pnode);
	for (int i = 0; i < burstCount; i++)
	{
		if (!pnode) break;
		xmldoc.getIntArray("lastValidSample", tmp, pnode);
		for (int j = tmp.cols - 1; j >= 0; j--)
		{
			if (tmp.at<int>(0, j) != -1)
			{
				lastValidSample.at<int>(i, 0) = tmp.at<int>(0, j);
				break;
			}
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getFirstValidLine()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getFirstValidLine(): input check failed!\n");
		return -1;
	}
	int ret, burstCount;
	TiXmlElement* pnode;
	Mat tmp;
	ret = getBurstCount(&burstCount);
	if (return_check(ret, "getBurstCount()", error_head)) return -1;
	firstValidLine.create(burstCount, 1, CV_32S);
	ret = xmldoc.find_node("burst", pnode);
	for (int i = 0; i < burstCount; i++)
	{
		if (!pnode) break;
		xmldoc.getIntArray("firstValidSample", tmp, pnode);
		for (int j = 0; j < tmp.cols; j++)
		{
			if (tmp.at<int>(0, j) != -1)
			{
				firstValidLine.at<int>(i, 0) = j + 1;
				break;
			}
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getLastValidLine()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getLastValidLine(): input check failed!\n");
		return -1;
	}
	int ret, burstCount;
	TiXmlElement* pnode;
	Mat tmp;
	ret = getBurstCount(&burstCount);
	if (return_check(ret, "getBurstCount()", error_head)) return -1;
	lastValidLine.create(burstCount, 1, CV_32S);
	ret = xmldoc.find_node("burst", pnode);
	for (int i = 0; i < burstCount; i++)
	{
		if (!pnode) break;
		xmldoc.getIntArray("lastValidSample", tmp, pnode);
		for (int j = tmp.cols - 1; j >= 0; j--)
		{
			if (tmp.at<int>(0, j) != -1)
			{
				lastValidLine.at<int>(i, 0) = j + 1;
				break;
			}
		}
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getGeolocationGridPoint()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getGeolocationGridPoint(): input check failed!\n");
		return -1;
	}
	/*
	* ȷ�����Ƶ����
	*/
	int n_gcps = 1;
	int ret;
	TiXmlElement* pnode = NULL;
	ret = xmldoc.find_node("geolocationGridPointList", pnode);
	if (ret < 0)
	{
		fprintf(stderr, "Sentinel1Reader::getGeolocationGridPoint(): node geolocationGridPointList not found!\n");
		return -1;
	}
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &n_gcps);
	geolocationGridPoint.create(n_gcps, 7, CV_64F);//���һ���Ƿ�λ��ʱ��
	TiXmlElement* pchild = NULL;
	ret = xmldoc._find_node(pnode, "geolocationGridPoint", pchild);
	double lon, lat, height, row, col, inc, azimuthTime;
	for (int i = 0; i < n_gcps; i++)
	{
		if (!pchild) break;

		//longtitude
		ret = xmldoc._find_node(pchild, "longitude", pnode);
		ret = sscanf(pnode->GetText(), "%lf", &lon);

		//latitude
		ret = xmldoc._find_node(pchild, "latitude", pnode);
		ret = sscanf(pnode->GetText(), "%lf", &lat);

		//height
		ret = xmldoc._find_node(pchild, "height", pnode);
		ret = sscanf(pnode->GetText(), "%lf", &height);

		//row
		ret = xmldoc._find_node(pchild, "line", pnode);
		ret = sscanf(pnode->GetText(), "%lf", &row);

		//col
		ret = xmldoc._find_node(pchild, "pixel", pnode);
		ret = sscanf(pnode->GetText(), "%lf", &col);

		//incidence angle
		ret = xmldoc._find_node(pchild, "incidenceAngle", pnode);
		ret = sscanf(pnode->GetText(), "%lf", &inc);

		//azimuthTime
		ret = xmldoc._find_node(pchild, "azimuthTime", pnode);
		UTC2GPS(pnode->GetText(), &azimuthTime);

		//assignment
		geolocationGridPoint.at<double>(i, 0) = lon;
		geolocationGridPoint.at<double>(i, 1) = lat;
		geolocationGridPoint.at<double>(i, 2) = height;
		geolocationGridPoint.at<double>(i, 3) = row;
		geolocationGridPoint.at<double>(i, 4) = col;
		geolocationGridPoint.at<double>(i, 5) = inc;
		geolocationGridPoint.at<double>(i, 6) = azimuthTime;
		pchild = pchild->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::updateGeolocationGridPoint()
{
	if (geolocationGridPoint.empty()) return 0;
	double startTime = geolocationGridPoint.at<double>(0, 6);
	for (int i = 0; i < geolocationGridPoint.rows; i++)
	{
		double time = geolocationGridPoint.at<double>(i, 6);

		if (fabs(time - startTime) > azimuthTimeInterval)
		{
			geolocationGridPoint.at<double>(i, 3) = (time - startTime) / azimuthTimeInterval;
		}
	}

	return 0;
}

int Sentinel1Reader::fitCoordinateConversionCoefficient()
{
	double mean_lon, mean_lat, mean_inc, max_lon, max_lat, max_inc, min_lon, min_lat, min_inc;
	Mat lon, lat, inc, row, col, gcps;
	geolocationGridPoint(cv::Range(0, geolocationGridPoint.rows), cv::Range(0, 6)).copyTo(gcps);
	gcps(cv::Range(0, gcps.rows), cv::Range(0, 1)).copyTo(lon);
	gcps(cv::Range(0, gcps.rows), cv::Range(1, 2)).copyTo(lat);
	gcps(cv::Range(0, gcps.rows), cv::Range(3, 4)).copyTo(row);
	gcps(cv::Range(0, gcps.rows), cv::Range(4, 5)).copyTo(col);
	gcps(cv::Range(0, gcps.rows), cv::Range(5, 6)).copyTo(inc);
	mean_lon = cv::mean(lon)[0];
	mean_lat = cv::mean(lat)[0];
	mean_inc = cv::mean(inc)[0];
	cv::minMaxLoc(lon, &min_lon, &max_lon);
	cv::minMaxLoc(lat, &min_lat, &max_lat);
	cv::minMaxLoc(inc, &min_inc, &max_inc);
	lon = (lon - mean_lon) / (max_lon - min_lon + 1e-10);
	lat = (lat - mean_lat) / (max_lat - min_lat + 1e-10);
	inc = (inc - mean_inc) / (max_inc - min_inc + 1e-10);
	row = (row + 1 - double(numberOfSamples) * 0.5) / (double(numberOfSamples) + 1e-10);//sentinel�������Ϊ0��+1ͳһΪ1.
	col = (col + 1 - double(numberOfSamples) * 0.5) / (double(numberOfSamples) + 1e-10);

	// ����5�׷����ɾ���
	Mat A, temp, coefficient;
	double rms;
	if (::createVandermondeMatrix(row, col, A) != 0) return -1;

	// ��Ͼ���
	if (::polyFit(A, lon, coefficient, &rms) != 0) return -1;
	temp.create(1, 32, CV_64F);
	temp.at<double>(0, 0) = mean_lon;
	temp.at<double>(0, 1) = max_lon - min_lon + 1e-10;
	temp.at<double>(0, 2) = double(numberOfSamples) * 0.5;
	temp.at<double>(0, 3) = double(numberOfSamples) + 1e-10;
	temp.at<double>(0, 4) = double(numberOfSamples) * 0.5;
	temp.at<double>(0, 5) = double(numberOfSamples) + 1e-10;
	temp.at<double>(0, 31) = rms;
	cv::transpose(coefficient, coefficient);
	coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
	temp.copyTo(lon_coefficient);

	// ���γ��
	if (::polyFit(A, lat, coefficient, &rms) != 0) return -1;
	temp.create(1, 32, CV_64F);
	temp.at<double>(0, 0) = mean_lat;
	temp.at<double>(0, 1) = max_lat - min_lat + 1e-10;
	temp.at<double>(0, 2) = double(numberOfSamples) * 0.5;
	temp.at<double>(0, 3) = double(numberOfSamples) + 1e-10;
	temp.at<double>(0, 4) = double(numberOfSamples) * 0.5;
	temp.at<double>(0, 5) = double(numberOfSamples) + 1e-10;
	temp.at<double>(0, 31) = rms;
	cv::transpose(coefficient, coefficient);
	coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
	temp.copyTo(lat_coefficient);

	//������ӽ�

	Mat b, B, a, a_t, b_t, error;
	inc.copyTo(b);
	A = Mat::ones(inc.rows, 6, CV_64F);
	col.copyTo(A(cv::Range(0, inc.rows), cv::Range(1, 2)));
	temp = col.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(2, 3)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(3, 4)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(4, 5)));
	temp = temp.mul(col);
	temp.copyTo(A(cv::Range(0, inc.rows), cv::Range(5, 6)));
	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 11, CV_64F);
		temp.at<double>(0, 0) = mean_inc;
		temp.at<double>(0, 1) = max_inc - min_inc + 1e-10;
		temp.at<double>(0, 2) = double(numberOfSamples) * 0.5;
		temp.at<double>(0, 3) = double(numberOfSamples) + 1e-10;
		temp.at<double>(0, 10) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(4, 10)));
		temp.copyTo(inc_coefficient);
	}

	//���������

	row.copyTo(b);
	A = Mat::ones(lon.rows, 25, CV_64F);
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	temp = lon.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

	lat.copyTo(temp);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 32, CV_64F);
		temp.at<double>(0, 0) = double(numberOfSamples) * 0.5;
		temp.at<double>(0, 1) = double(numberOfSamples) + 1e-10;
		temp.at<double>(0, 2) = mean_lon;
		temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
		temp.at<double>(0, 4) = mean_lat;
		temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
		temp.at<double>(0, 31) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
		temp.copyTo(row_coefficient);
	}

	//���������

	col.copyTo(b);
	A = Mat::ones(lon.rows, 25, CV_64F);
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	temp = lon.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(3, 4)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(4, 5)));

	lat.copyTo(temp);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(5, 6)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(6, 7)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(7, 8)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(8, 9)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(9, 10)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(10, 11)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(11, 12)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(12, 13)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(13, 14)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(14, 15)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(15, 16)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(16, 17)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(17, 18)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(18, 19)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(19, 20)));

	lat.copyTo(temp);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp = temp.mul(lat);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(20, 21)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(21, 22)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(22, 23)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(23, 24)));
	temp = temp.mul(lon);
	temp.copyTo(A(cv::Range(0, lon.rows), cv::Range(24, 25)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		temp.create(1, 32, CV_64F);
		temp.at<double>(0, 0) = double(numberOfSamples) * 0.5;
		temp.at<double>(0, 1) = double(numberOfSamples) + 1e-10;
		temp.at<double>(0, 2) = mean_lon;
		temp.at<double>(0, 3) = max_lon - min_lon + 1e-10;
		temp.at<double>(0, 4) = mean_lat;
		temp.at<double>(0, 5) = max_lat - min_lat + 1e-10;
		temp.at<double>(0, 31) = rms;
		cv::transpose(coefficient, coefficient);
		coefficient.copyTo(temp(cv::Range(0, 1), cv::Range(6, 31)));
		temp.copyTo(col_coefficient);
	}
	return 0;
}

int Sentinel1Reader::getOrbitList()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getOrbitList(): input check failed!\n");
		return -1;
	}
	TiXmlElement* pnode, * pchild, * pchild1;
	int ret, numOfstateVec;
	ret = xmldoc.find_node("orbitList", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfstateVec);

	ret = xmldoc.find_node("orbit", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	double time, x, y, z, vx, vy, vz;
	orbitList.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPSʱ��
		ret = xmldoc._find_node(pnode, "time", pchild);
		ret = UTC2GPS(pchild->GetText(), &time);
		//λ��x
		ret = xmldoc._find_node(pnode, "position", pchild);
		ret = xmldoc._find_node(pchild, "x", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &x);
		//λ��y
		ret = xmldoc._find_node(pchild, "y", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &y);
		//λ��z
		ret = xmldoc._find_node(pchild, "z", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &z);
		//�ٶ�x
		ret = xmldoc._find_node(pnode, "velocity", pchild);
		ret = xmldoc._find_node(pchild, "x", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &vx);
		//�ٶ�y
		ret = xmldoc._find_node(pchild, "y", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &vy);
		//�ٶ�z
		ret = xmldoc._find_node(pchild, "z", pchild1);
		ret = sscanf(pchild1->GetText(), "%lf", &vz);

		//��ֵ
		orbitList.at<double>(i, 0) = time;
		orbitList.at<double>(i, 1) = x;
		orbitList.at<double>(i, 2) = y;
		orbitList.at<double>(i, 3) = z;
		orbitList.at<double>(i, 4) = vx;
		orbitList.at<double>(i, 5) = vy;
		orbitList.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}
	return 0;
}

int Sentinel1Reader::getPOD(const char* POD_file)
{
	if (!bXmlLoad || !POD_file)
	{
		fprintf(stderr, "Sentinel1Reader::getPOD(): input check failed!\n");
		return -1;
	}
	/*
	* ��ȡ���ܹ������
	*/

	int ret, numOfstateVec;
	double start_time, stop_time;
	string start_time_str, stop_time_str;
	ret = xmldoc.get_str_para("startTime", start_time_str);
	if (return_check(ret, "get_str_para()", error_head)) return -1;
	ret = xmldoc.get_str_para("stopTime", stop_time_str);
	UTC2GPS(start_time_str.c_str(), &start_time);
	UTC2GPS(stop_time_str.c_str(), &stop_time);
	TiXmlElement* pnode, * pchild;
	XMLFile doc;
	ret = doc.XMLFile_load(POD_file);
	if (return_check(ret, "XMLFile_load()", error_head)) return -1;
	ret = doc.find_node("List_of_OSVs", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfstateVec);


	Mat tmp = Mat::zeros(numOfstateVec, 7, CV_64F);
	ret = doc.find_node("OSV", pnode);
	if (return_check(ret, "find_node()", error_head)) return -1;
	string str;
	double gps_time, x, y, z, vx, vy, vz;
	bool start = false; bool stop = false;
	int count = 0;
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode || stop) break;
		ret = doc._find_node(pnode, "UTC", pchild);
		str = pchild->GetText();
		str = str.substr(4);
		ret = UTC2GPS(str.c_str(), &gps_time);
		if (gps_time <= start_time && fabs(gps_time - start_time) <= 100.0) start = true;
		if (gps_time >= stop_time && fabs(gps_time - stop_time) >= 100.0) stop = true;

		if (start && !stop)//��ʼ��¼
		{
			ret = doc._find_node(pnode, "X", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &x);
			ret = doc._find_node(pnode, "Y", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &y);
			ret = doc._find_node(pnode, "Z", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &z);
			ret = doc._find_node(pnode, "VX", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &vx);
			ret = doc._find_node(pnode, "VY", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &vy);
			ret = doc._find_node(pnode, "VZ", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &vz);

			tmp.at<double>(count, 0) = gps_time;
			tmp.at<double>(count, 1) = x;
			tmp.at<double>(count, 2) = y;
			tmp.at<double>(count, 3) = z;
			tmp.at<double>(count, 4) = vx;
			tmp.at<double>(count, 5) = vy;
			tmp.at<double>(count, 6) = vz;
			count++;
		}

		pnode = pnode->NextSiblingElement();
	}
	if (count < 1)
	{
		fprintf(stderr, "Sentinel1Reader::getPOD(): orbit mismatch! please check if POD file!\n");
		return -1;
	}
	tmp(cv::Range(0, count), cv::Range(0, 7)).copyTo(preciseOrbitList);
	return 0;
}

int Sentinel1Reader::getOtherParameters()
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getOtherParameters(): input check failed!\n");
		return -1;
	}
	int ret = xmldoc.get_double_para("azimuthPixelSpacing", &this->azimuthPixelSpacing);
	if (return_check(ret, "get_double_para()", error_head)) return -1;
	ret = xmldoc.get_double_para("azimuthSteeringRate", &this->azimuthSteeringRate);
	ret = xmldoc.get_double_para("azimuthTimeInterval", &this->azimuthTimeInterval);
	ret = xmldoc.get_double_para("platformHeading", &this->headingAngle);
	ret = xmldoc.get_double_para("radarFrequency", &this->radarFrequency);
	ret = xmldoc.get_double_para("rangePixelSpacing", &this->rangePixelSpacing);
	ret = xmldoc.get_double_para("rangeSamplingRate", &this->rangeSamplingRate);
	ret = xmldoc.get_double_para("slantRangeTime", &this->slantRangeTime);
	ret = xmldoc.get_double_para("incidenceAngleMidSwath", &this->incidence_center);

	ret = xmldoc.get_int_para("numberOfLines", &this->numberOfLines);
	ret = xmldoc.get_int_para("numberOfSamples", &this->numberOfSamples);
	ret = xmldoc.get_int_para("linesPerBurst", &this->linesPerBurst);
	ret = getBurstCount(&this->burstCount);

	ret = xmldoc.get_str_para("polarisation", this->polarization);
	ret = xmldoc.get_str_para("swath", this->swath);
	ret = xmldoc.get_str_para("pass", this->pass);
	xmldoc.get_str_para("startTime", this->startTime);
	xmldoc.get_str_para("stopTime", this->stopTime);
	return 0;
}

int Sentinel1Reader::prepareData(const char* PODFile)
{
	if (!bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::prepareData(): input check failed!\n");
		return -1;
	}
	int ret;
	ret = getOtherParameters();
	if (return_check(ret, "getOtherParameters()", error_head)) return -1;
	this->getAntennaPattern();
	this->getAzimuthFmRateList();
	this->getBurstAzimuthTime();
	this->getDcEstimateList();
	this->getFirstValidLine();
	this->getFirstValidSample();
	this->getGeolocationGridPoint();
	this->getLastValidLine();
	this->getLastValidSample();
	this->getOrbitList();
	if (PODFile)
	{
		this->getPOD(PODFile);
	}
	//���¿��Ƶ�
	updateGeolocationGridPoint();
	//�������ת��ϵ��
	fitCoordinateConversionCoefficient();
	isDataAvailable = true;
	return 0;
}

int Sentinel1Reader::getSLC(ComplexMat& slc)
{
	int ret;
	if (this->tiffFile.empty() || !bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::getSLC(): input check failed!\n");
		return -1;
	}
	FILE* fp = fopen(this->tiffFile.c_str(), "rb");
	if (!fp) {
		fprintf(stderr, "getSLC(): can't open %s\n", tiffFile.c_str());
		return -1;
	}
	int byteOffset;
	ret = xmldoc.get_int_para("byteOffset", &byteOffset);
	if (return_check(ret, "get_int_para()", error_head)) {
		if (fp) fclose(fp);
		return -1;
	}
	size_t size = numberOfLines * numberOfSamples * 2 * sizeof(short);
	short* buf = (short*)malloc(size);
	if (!buf) {
		fprintf(stderr, "getSLC(): out of memory!\n");
		if (fp) fclose(fp);
		return -1;
	}
	fseek(fp, byteOffset, SEEK_SET);
	fread(buf, sizeof(short), numberOfLines * numberOfSamples * 2, fp);
	if (fp) fclose(fp);
	size_t offset = 0;
	slc.re.create(numberOfLines, numberOfSamples, CV_16S);
	slc.im.create(numberOfLines, numberOfSamples, CV_16S);
	for (int j = 0; j < numberOfLines; j++)
	{
		for (int k = 0; k < numberOfSamples; k++)
		{
			slc.re.at<short>(j, k) = buf[offset];
			offset++;
			slc.im.at<short>(j, k) = buf[offset];
			offset++;
		}
	}
	if (buf) free(buf);
	return 0;
}

int Sentinel1Reader::writeToh5(const char* h5File, int start_burst, int end_burst, ProgressCallback progressCallback, void* userData)
{
	H5_LOCK;
	int ret;
	if (!h5File || !bXmlLoad)
	{
		fprintf(stderr, "Sentinel1Reader::writeToh5(): input check failed!\n");
		return -1;
	}
	if (!isDataAvailable)
	{
		if (PODFile.empty()) ret = prepareData();
		else ret = prepareData(PODFile.c_str());
		if (return_check(ret, "prepareData()", error_head)) return -1;
	}

	// Validate burst range
	if (start_burst < 0) start_burst = 0;
	if (end_burst < 0) end_burst = this->burstCount - 1;
	if (start_burst >= this->burstCount || end_burst >= this->burstCount || start_burst > end_burst) {
		fprintf(stderr, "Sentinel1Reader::writeToh5(): invalid burst range [%d, %d]! Total bursts: %d\n", start_burst, end_burst, this->burstCount);
		return -1;
	}

	int num_selected_bursts = end_burst - start_burst + 1;
	int original_burstCount = this->burstCount;
	int original_linesPerBurst = this->linesPerBurst;

	if (num_selected_bursts < original_burstCount) {
		// 1. Sync global UTC start/stop time to prevent orbit drift
		double new_start_gps = this->burstAzimuthTime.at<double>(start_burst, 0);
		double new_stop_gps = new_start_gps + (double)num_selected_bursts * original_linesPerBurst * this->azimuthTimeInterval;
		this->startTime = gps2utc(new_start_gps);
		this->stopTime = gps2utc(new_stop_gps);

		// 2. Slice burst-specific metadata matrices
		cv::Range r(start_burst, end_burst + 1);
		this->firstValidLine = this->firstValidLine(r, cv::Range::all()).clone();
		this->firstValidSample = this->firstValidSample(r, cv::Range::all()).clone();
		this->lastValidLine = this->lastValidLine(r, cv::Range::all()).clone();
		this->lastValidSample = this->lastValidSample(r, cv::Range::all()).clone();
		this->burstAzimuthTime = this->burstAzimuthTime(r, cv::Range::all()).clone();

		// 3. Shift all GCP row coordinates and re-fit geometric polynomials (no filtering to prevent rank-deficiency failure)
		this->updateGeolocationGridPoint(); // Sync/align baseline coordinates first

		double start_line = (double)start_burst * original_linesPerBurst;
		for (int i = 0; i < this->geolocationGridPoint.rows; i++) {
			this->geolocationGridPoint.at<double>(i, 3) -= start_line; // Shift row coordinate to subset system
		}

		this->fitCoordinateConversionCoefficient(); // Re-fit polynomials with the shifted coordinates

		// 4. Update overall dimensions
		this->burstCount = num_selected_bursts;
		this->numberOfLines = num_selected_bursts * original_linesPerBurst;
	}

	FormatConversion conversion;
	ret = conversion.creat_new_h5(h5File);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	ret = conversion.write_str_to_h5(h5File, "polarization", this->polarization.c_str());
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	conversion.write_str_to_h5(h5File, "orbit_dir", pass.c_str());
	conversion.write_str_to_h5(h5File, "swath", swath.c_str());
	conversion.write_str_to_h5(h5File, "imaging_mode", "TOPS");
	conversion.write_str_to_h5(h5File, "sensor", sensor.c_str());
	ret = conversion.write_str_to_h5(h5File, "source_1", this->m_xmlFileName);
	if (return_check(ret, "write_str_to_h5()", error_head)) return -1;
	conversion.write_str_to_h5(h5File, "acquisition_start_time", startTime.c_str());
	conversion.write_str_to_h5(h5File, "acquisition_stop_time", stopTime.c_str());

	conversion.write_double_to_h5(h5File, "azimuth_spacing", this->azimuthPixelSpacing);
	conversion.write_double_to_h5(h5File, "range_spacing", this->rangePixelSpacing);
	conversion.write_double_to_h5(h5File, "azimuthSteeringRate", this->azimuthSteeringRate);
	conversion.write_double_to_h5(h5File, "prf", 1.0 / this->azimuthTimeInterval);
	conversion.write_double_to_h5(h5File, "heading", this->headingAngle);
	conversion.write_double_to_h5(h5File, "inc_center", this->incidence_center);
	conversion.write_double_to_h5(h5File, "azimuth_resolution", 20.0);
	conversion.write_double_to_h5(h5File, "range_resolution", 5.0);

	double mean_alt = 0.0;
	if (!this->orbitList.empty() && this->orbitList.rows > 0) {
		double sum_alt = 0.0;
		int count = 0;
		for (int i = 0; i < this->orbitList.rows; ++i) {
			double x = this->orbitList.at<double>(i, 1);
			double y = this->orbitList.at<double>(i, 2);
			double z = this->orbitList.at<double>(i, 3);
			double dist = std::sqrt(x*x + y*y + z*z);
			if (dist > 6000000.0) {
				sum_alt += (dist - 6378137.0);
				count++;
			}
		}
		if (count > 0) mean_alt = sum_alt / count;
	}
	if (mean_alt <= 0.0) mean_alt = 693000.0;
	conversion.write_double_to_h5(h5File, "orbit_altitude", mean_alt);

	conversion.write_double_to_h5(h5File, "carrier_frequency", this->radarFrequency);
	conversion.write_double_to_h5(h5File, "slant_range_first_pixel", this->slantRangeTime * VEL_C / 2.0);

	conversion.write_int_to_h5(h5File, "burstCount", this->burstCount);
	conversion.write_int_to_h5(h5File, "linesPerBurst", this->linesPerBurst);
	conversion.write_int_to_h5(h5File, "samplesPerBurst", this->numberOfSamples);
	conversion.write_int_to_h5(h5File, "range_len", this->numberOfSamples);
	conversion.write_int_to_h5(h5File, "azimuth_len", this->numberOfLines);
	conversion.write_int_to_h5(h5File, "offset_row", 0);
	conversion.write_int_to_h5(h5File, "offset_col", 0);

	// ��GCP��������ĽǾ�γ��
	if (!geolocationGridPoint.empty() && geolocationGridPoint.rows >= 4) {
		double rows = (double)this->numberOfLines - 1.0;
		double cols = (double)this->numberOfSamples - 1.0;
		double tl_dist = DBL_MAX, tr_dist = DBL_MAX, bl_dist = DBL_MAX, br_dist = DBL_MAX;
		double tl_lon = 0, tl_lat = 0, tr_lon = 0, tr_lat = 0;
		double bl_lon = 0, bl_lat = 0, br_lon = 0, br_lat = 0;
		for (int i = 0; i < geolocationGridPoint.rows; i++) {
			double r = geolocationGridPoint.at<double>(i, 3);
			double c = geolocationGridPoint.at<double>(i, 4);
			double lon = geolocationGridPoint.at<double>(i, 0);
			double lat = geolocationGridPoint.at<double>(i, 1);
			double dr, dc, d;
			// TL: (0, 0)
			dr = r; dc = c; d = dr * dr + dc * dc;
			if (d < tl_dist) { tl_dist = d; tl_lon = lon; tl_lat = lat; }
			// TR: (0, cols)
			dr = r; dc = c - cols; d = dr * dr + dc * dc;
			if (d < tr_dist) { tr_dist = d; tr_lon = lon; tr_lat = lat; }
			// BL: (rows, 0)
			dr = r - rows; dc = c; d = dr * dr + dc * dc;
			if (d < bl_dist) { bl_dist = d; bl_lon = lon; bl_lat = lat; }
			// BR: (rows, cols)
			dr = r - rows; dc = c - cols; d = dr * dr + dc * dc;
			if (d < br_dist) { br_dist = d; br_lon = lon; br_lat = lat; }
		}
		conversion.write_double_to_h5(h5File, "topLeftLon", tl_lon);
		conversion.write_double_to_h5(h5File, "topLeftLat", tl_lat);
		conversion.write_double_to_h5(h5File, "topRightLon", tr_lon);
		conversion.write_double_to_h5(h5File, "topRightLat", tr_lat);
		conversion.write_double_to_h5(h5File, "bottomLeftLon", bl_lon);
		conversion.write_double_to_h5(h5File, "bottomLeftLat", bl_lat);
		conversion.write_double_to_h5(h5File, "bottomRightLon", br_lon);
		conversion.write_double_to_h5(h5File, "bottomRightLat", br_lat);
	}

	conversion.write_array_to_h5(h5File, "antennaPattern_elevationAngle", this->antennaPattern_elevationAngle);
	conversion.write_array_to_h5(h5File, "antennaPattern_slantRangeTime", this->antennaPattern_slantRangeTime);
	conversion.write_array_to_h5(h5File, "azimuthFmRateList", this->AzimuthFmRateList);
	conversion.write_array_to_h5(h5File, "burstAzimuthTime", this->burstAzimuthTime);
	conversion.write_array_to_h5(h5File, "dcEstimateList", this->DcEstimateList);
	conversion.write_array_to_h5(h5File, "doppler_centroid", this->DcEstimateList);
	conversion.write_array_to_h5(h5File, "doppler_coefficient_a", this->DcEstimateList);
	conversion.write_array_to_h5(h5File, "doppler_coefficient_b", this->DcEstimateList);
	conversion.write_array_to_h5(h5File, "firstValidLine", this->firstValidLine);
	conversion.write_array_to_h5(h5File, "firstValidSample", this->firstValidSample);
	conversion.write_array_to_h5(h5File, "lon_coefficient", this->lon_coefficient);
	conversion.write_array_to_h5(h5File, "lat_coefficient", this->lat_coefficient);
	conversion.write_array_to_h5(h5File, "row_coefficient", this->row_coefficient);
	conversion.write_array_to_h5(h5File, "col_coefficient", this->col_coefficient);
	conversion.write_array_to_h5(h5File, "inc_coefficient", this->inc_coefficient);


	Mat gcps;
	if (geolocationGridPoint.cols > 6) geolocationGridPoint(cv::Range(0, geolocationGridPoint.rows), cv::Range(0, 6)).copyTo(gcps);
	else geolocationGridPoint.copyTo(gcps);
	conversion.write_array_to_h5(h5File, "gcps", gcps);
	conversion.write_array_to_h5(h5File, "lastValidLine", this->lastValidLine);
	conversion.write_array_to_h5(h5File, "lastValidSample", this->lastValidSample);
	conversion.write_array_to_h5(h5File, "state_vec", this->orbitList);
	if(!preciseOrbitList.empty())
		conversion.write_array_to_h5(h5File, "fine_state_vec", this->preciseOrbitList);

	//д��ͼ������
	if (this->tiffFile.empty())
	{
		fprintf(stderr, "Sentinel1Reader::writeToh5(): tiffFile path is empty!\n");
		return -1;
	}
	FILE* fp_tiff = fopen(this->tiffFile.c_str(), "rb");
	if (!fp_tiff) {
		fprintf(stderr, "Sentinel1Reader::writeToh5(): can't open %s\n", tiffFile.c_str());
		return -1;
	}
	int byteOffset;
	ret = xmldoc.get_int_para("byteOffset", &byteOffset);
	if (return_check(ret, "get_int_para()", error_head)) {
		fclose(fp_tiff);
		return -1;
	}

	ret = conversion.write_zero_array_to_h5(h5File, "s_re", CV_16S, this->numberOfLines, this->numberOfSamples);
	if (return_check(ret, "write_zero_array_to_h5(s_re)", error_head)) { fclose(fp_tiff); return -1; }
	ret = conversion.write_zero_array_to_h5(h5File, "s_im", CV_16S, this->numberOfLines, this->numberOfSamples);
	if (return_check(ret, "write_zero_array_to_h5(s_im)", error_head)) { fclose(fp_tiff); return -1; }

	size_t start_pixel_offset = (size_t)start_burst * original_linesPerBurst * this->numberOfSamples * 2;
#ifdef _MSC_VER
	_fseeki64(fp_tiff, (__int64)byteOffset + (__int64)start_pixel_offset * sizeof(short), SEEK_SET);
#else
	fseeko(fp_tiff, (off_t)byteOffset + (off_t)start_pixel_offset * sizeof(short), SEEK_SET);
#endif

	int block_height = 1024;
	int num_blocks = (this->numberOfLines + block_height - 1) / block_height;
	size_t block_samples_limit = (size_t)block_height * this->numberOfSamples * 2;
	short* block_buf = (short*)malloc(block_samples_limit * sizeof(short));
	if (!block_buf) {
		fprintf(stderr, "Sentinel1Reader::writeToh5(): out of memory for block buffer!\n");
		fclose(fp_tiff);
		return -1;
	}

	for (int i = 0; i < num_blocks; ++i) {
		int current_offset = i * block_height;
		int current_rows = std::min(block_height, this->numberOfLines - current_offset);

		size_t read_count = (size_t)current_rows * this->numberOfSamples * 2;
		size_t read_bytes = fread(block_buf, sizeof(short), read_count, fp_tiff);
		if (read_bytes != read_count) {
			fprintf(stderr, "Sentinel1Reader::writeToh5(): warning: fread bytes mismatch!\n");
		}

		cv::Mat block_re;
		cv::Mat block_im;
		try {
			block_re.create(current_rows, this->numberOfSamples, CV_16S);
			block_im.create(current_rows, this->numberOfSamples, CV_16S);
		}
		catch (const cv::Exception& e) {
			fprintf(stderr, "Sentinel1Reader::writeToh5(): out of memory for block matrices! (OpenCV exception: %s)\n", e.what());
			free(block_buf);
			fclose(fp_tiff);
			return -1;
		}

		#pragma omp parallel for schedule(guided)
		for (int r = 0; r < current_rows; ++r) {
			short* ptr_re = block_re.ptr<short>(r);
			short* ptr_im = block_im.ptr<short>(r);
			const short* p_src = &block_buf[r * this->numberOfSamples * 2];
			for (int c = 0; c < this->numberOfSamples; ++c) {
				ptr_re[c] = p_src[2 * c];
				ptr_im[c] = p_src[2 * c + 1];
			}
		}

		ret = conversion.write_subarray_to_h5(h5File, "s_re", block_re, current_offset, 0, current_rows, this->numberOfSamples);
		if (return_check(ret, "write_subarray_to_h5(s_re)", error_head)) { free(block_buf); fclose(fp_tiff); return -1; }
		ret = conversion.write_subarray_to_h5(h5File, "s_im", block_im, current_offset, 0, current_rows, this->numberOfSamples);
		if (return_check(ret, "write_subarray_to_h5(s_im)", error_head)) { free(block_buf); fclose(fp_tiff); return -1; }

		// ���ȸ��� (50% -> 95% ֮��)
		int progress = 50 + (i + 1) * 45 / num_blocks;
		if (!report_progress(progressCallback, userData, progress, "����д��Sentinel-1 H5��ͼ��ֿ�����...")) {
			free(block_buf);
			fclose(fp_tiff);
			return -2; // ���û���ֹ
		}
	}

	free(block_buf);
	fclose(fp_tiff);
	return 0;
}

/*------------------------------------------------*/
/*             �ڱ�һ������/�����ȡ����          */
/*------------------------------------------------*/

Sentinel1Utils::Sentinel1Utils(const char* h5File)
{
	bInitialized = false;
	memset(m_xmlFileName, 0, 2048);
	memset(this->error_head, 0, 256);
	strcpy(this->error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using ");
	this->azimuthPixelSpacing = 0.0;
	this->azimuthSteeringRate = 0.0;
	this->azimuthTimeInterval = 0.0;
	this->numberOfLines = 0;
	this->headingAngle = 0.0;
	this->numberOfSamples = 0;
	this->samplesPerBurst = 0;
	this->burstCount = 0;
	this->linesPerBurst = 0;
	this->pass = "";
	this->polarization = "";
	this->radarFrequency = 0.0;
	this->rangePixelSpacing = 0.0;
	this->rangeSamplingRate = 0.0;
	this->sensor = "sentinel";
	this->swath = "";
	this->slantRangeTime = 0.0;
	this->h5File = h5File;
	stateVectors = NULL;

	this->isDopplerCentroidAvailable = false;
	this->isDopplerRateAvailable = false;
	this->isRangeDependDopplerRateAvailiable = false;
	this->isReferenceTimeAvailable = false;

	this->burstOffset = -9999;

}

Sentinel1Utils::~Sentinel1Utils()
{
	if (stateVectors)
	{
		delete stateVectors;
		stateVectors = NULL;
	}
}

int Sentinel1Utils::init()
{
	if (h5File.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret; FormatConversion conversion;
	//��ȡ����
	ret = conversion.read_double_from_h5(h5File.c_str(), "azimuth_spacing", &this->azimuthPixelSpacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(h5File.c_str(), "azimuthSteeringRate", &this->azimuthSteeringRate);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(h5File.c_str(), "prf", &this->azimuthTimeInterval);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	azimuthTimeInterval = 1.0 / azimuthTimeInterval;
	ret = conversion.read_double_from_h5(h5File.c_str(), "carrier_frequency", &this->radarFrequency);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(h5File.c_str(), "range_spacing", &this->rangePixelSpacing);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	ret = conversion.read_double_from_h5(h5File.c_str(), "slant_range_first_pixel", &this->slantRangeTime);
	if (return_check(ret, "read_double_from_h5()", error_head)) return -1;
	slantRangeTime = 2.0 * slantRangeTime / VEL_C;

	ret = conversion.read_int_from_h5(h5File.c_str(), "burstCount", &this->burstCount);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(h5File.c_str(), "linesPerBurst", &this->linesPerBurst);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	ret = conversion.read_int_from_h5(h5File.c_str(), "range_len", &this->numberOfSamples);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	this->samplesPerBurst = this->numberOfSamples;
	ret = conversion.read_int_from_h5(h5File.c_str(), "azimuth_len", &this->numberOfLines);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;

	const int swathReadStatus = conversion.read_str_from_h5(h5File.c_str(), "swath", this->swath);
	const int polarizationReadStatus = conversion.read_str_from_h5(h5File.c_str(), "polarization", this->polarization);
	std::ostringstream identityDetail;
	identityDetail << "swath_rc=" << swathReadStatus << ", swath=" << this->swath
		<< ", polarization_rc=" << polarizationReadStatus << ", polarization=" << this->polarization;
	const std::string identityDetailText = identityDetail.str();
	// Sentinel-1 identity is required by the new-engine contract; fail closed on missing values.
	if (swathReadStatus != 0 || polarizationReadStatus != 0 || this->swath.empty() || this->polarization.empty())
	{
		const int identityStatus = swathReadStatus != 0 ? swathReadStatus :
			(polarizationReadStatus != 0 ? polarizationReadStatus : -1);
		const char* identityDataset = swathReadStatus != 0 || this->swath.empty() ? "swath" : "polarization";
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "h5_identity", "load", "Sentinel-1 H5 identity is missing or unreadable.",
			identityDetailText.c_str(), h5File.c_str(), identityDataset, identityStatus);
		return -1;
	}
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "h5_identity", "load", "Sentinel-1 H5 identity loaded.",
		identityDetailText.c_str(), h5File.c_str(), "swath/polarization");

	ret = conversion.read_array_from_h5(h5File.c_str(), "azimuthFmRateList", this->AzimuthFmRateList);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "burstAzimuthTime", this->burstAzimuthTime);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "dcEstimateList", this->DcEstimateList);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "firstValidSample", this->firstValidSample);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "firstValidLine", this->firstValidLine);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "lastValidLine", this->lastValidLine);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "lastValidSample", this->lastValidSample);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "state_vec", this->orbitList);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "fine_state_vec", this->preciseOrbitList);
	if (ret == 0) {
		char message[256] = {};
		sprintf_s(message, "Loaded precise orbit state vectors: count=%d.", this->preciseOrbitList.rows);
		emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "orbit", "load_precise_orbit", message,
			"Using fine_state_vec for geometric positioning.", h5File.c_str(), "fine_state_vec");
	} else {
		char message[256] = {};
		sprintf_s(message, "Precise orbit state vectors are unavailable; using default orbit vectors: count=%d.", this->orbitList.rows);
		emit_diagnostic(INSAR_DIAGNOSTIC_WARNING, "orbit", "load_precise_orbit", message,
			"fine_state_vec was not available; state_vec will be used.", h5File.c_str(), "fine_state_vec");
	}
	ret = conversion.read_array_from_h5(h5File.c_str(), "antennaPattern_elevationAngle", this->antennaPattern_elevationAngle);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "antennaPattern_slantRangeTime", this->antennaPattern_slantRangeTime);
	//if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	ret = conversion.read_array_from_h5(h5File.c_str(), "gcps", this->geolocationGridPoint);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;

	string str;
	double startTime, stopTime;
	ret = conversion.read_str_from_h5(h5File.c_str(), "acquisition_start_time", str);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	UTC2GPS(str.c_str(), &startTime);
	ret = conversion.read_str_from_h5(h5File.c_str(), "acquisition_stop_time", str);
	if (return_check(ret, "read_str_from_h5()", error_head)) return -1;
	UTC2GPS(str.c_str(), &stopTime);

	if (!preciseOrbitList.empty())
	{
		this->stateVectors = new orbitStateVectors(preciseOrbitList, startTime, stopTime);
	}
	else
	{
		this->stateVectors = new orbitStateVectors(orbitList, startTime, stopTime);
	}
	ret = this->stateVectors->applyOrbit();
	if (ret == -2) return -2;
	if (return_check(ret, "applyOrbit()", error_head)) return -1;
	bInitialized = true;
	computeDopplerCentroid();
	return 0;
}

int Sentinel1Utils::computeReferenceTime()
{
	if (!bInitialized)
	{
		fprintf(stderr, "Sentinel1Utils::computeReferenceTime(): input check failed!\n");
		return -1;
	}
	int ret;
	if (!isDopplerCentroidAvailable) {
		ret = computeDopplerCentroid();
		if (return_check(ret, "computeDopplerCentroid()", error_head)) return -1;
	}

	if (!isRangeDependDopplerRateAvailiable) {
		ret = computeRangeDependDopplerRate();
		if (return_check(ret, "computeRangeDependDopplerRate()", error_head)) return -1;
	}
	double tmp;
	tmp = (double)linesPerBurst * azimuthTimeInterval / 2.0;
	referenceTime.create(burstCount, samplesPerBurst, CV_64F);
	for (int i = 0; i < burstCount; i++)
	{
		double tmp2 = tmp + dopplerCentroid.at<double>(i, firstValidSample.at<int>(i, 0)) /
			rangeDependDopplerRate.at<double>(i, firstValidSample.at<int>(i, 0));
		for (int j = 0; j < samplesPerBurst; j++)
		{
			referenceTime.at<double>(i, j) = tmp2 - dopplerCentroid.at<double>(i, j) / rangeDependDopplerRate.at<double>(i, j);
		}
	}
	isReferenceTimeAvailable = true;
	return 0;
}

int Sentinel1Utils::computeRangeDependDopplerRate()
{
	if (!bInitialized)
	{
		fprintf(stderr, "Sentinel1Utils::computeRangeDependDopplerRate(): input check failed!\n");
		return -1;
	}
	// removed unused: ret (direct computation, no API calls needing error checks)
	rangeDependDopplerRate.create(burstCount, samplesPerBurst, CV_64F);
	for (int i = 0; i < burstCount; i++)
	{
		for (int j = 0; j < samplesPerBurst; j++)
		{
			double slrt = 2 * (slantRangeTime / 2 + j * rangePixelSpacing / VEL_C);
			double dt; int k;
			if (burstAzimuthTime.at<double>(i, 0) <= AzimuthFmRateList.at<double>(0, 0))
			{
				k = 0;
				dt = slrt - AzimuthFmRateList.at<double>(k, 1);
			}
			else if (burstAzimuthTime.at<double>(i, 0) > AzimuthFmRateList.at<double>(AzimuthFmRateList.rows - 1, 0))
			{
				k = AzimuthFmRateList.rows - 1;
				dt = slrt - AzimuthFmRateList.at<double>(k, 1);

			}
			else
			{
				for (k = 1; k < AzimuthFmRateList.rows; k++)
				{
					if (AzimuthFmRateList.at<double>(k, 0) >= burstAzimuthTime.at<double>(i, 0) &&
						AzimuthFmRateList.at<double>(k - 1, 0) < burstAzimuthTime.at<double>(i, 0))
					{
						dt = slrt - AzimuthFmRateList.at<double>(k, 1);
						break;
					}
				}
			}
			double c0, c1, c2;
			c0 = AzimuthFmRateList.at<double>(k, 2);
			c1 = AzimuthFmRateList.at<double>(k, 3);
			c2 = AzimuthFmRateList.at<double>(k, 4);
			rangeDependDopplerRate.at<double>(i, j) = c0 + c1 * dt + c2 * dt * dt;

		}
	}
	isRangeDependDopplerRateAvailiable = true;
	return 0;
}

int Sentinel1Utils::computeDopplerCentroid()
{
	if (!bInitialized)
	{
		fprintf(stderr, "Sentinel1Utils::computeDopplerCentroid(): input check failed!\n");
		return -1;
	}
	// removed unused: ret (direct computation, no API calls needing error checks)
	dopplerCentroid.create(burstCount, samplesPerBurst, CV_64F);
	for (int i = 0; i < burstCount; i++)
	{
		for (int j = 0; j < samplesPerBurst; j++)
		{
			double slrt = 2 * (slantRangeTime / 2 + j * rangePixelSpacing / VEL_C);
			double dt; int k;
			if (burstAzimuthTime.at<double>(i, 0) <= DcEstimateList.at<double>(0, 0))
			{
				k = 0;
				dt = slrt - DcEstimateList.at<double>(k, 1);
			}
			else if (burstAzimuthTime.at<double>(i, 0) > DcEstimateList.at<double>(DcEstimateList.rows - 1, 0))
			{
				k = DcEstimateList.rows - 1;
				dt = slrt - DcEstimateList.at<double>(k, 1);

			}
			else
			{
				for (k = 1; k < DcEstimateList.rows; k++)
				{
					if (DcEstimateList.at<double>(k, 0) >= burstAzimuthTime.at<double>(i, 0) &&
						DcEstimateList.at<double>(k - 1, 0) < burstAzimuthTime.at<double>(i, 0))
					{
						dt = slrt - DcEstimateList.at<double>(k, 1);
						break;
					}
				}
			}
			double c0, c1, c2;
			c0 = DcEstimateList.at<double>(k, 2);
			c1 = DcEstimateList.at<double>(k, 3);
			c2 = DcEstimateList.at<double>(k, 4);
			dopplerCentroid.at<double>(i, j) = c0 + c1 * dt + c2 * dt * dt;

		}
	}
	isDopplerCentroidAvailable = true;
	return 0;
}

int Sentinel1Utils::computeDopplerRate()
{
	if (!bInitialized)
	{
		fprintf(stderr, "Sentinel1Utils::computeDopplerRate(): input check failed!\n");
		return -1;
	}
	int ret;
	if (!isRangeDependDopplerRateAvailiable)
	{
		ret = computeRangeDependDopplerRate();
		if (return_check(ret, "computeRangeDependDopplerRate()", error_head)) return -1;
	}
	double waveLength = VEL_C / radarFrequency;
	dopplerRate.create(burstCount, samplesPerBurst, CV_64F);
	for (int i = 0; i < burstCount; i++)
	{
		double v = sqrt(orbitList.at<double>(0, 6) * orbitList.at<double>(0, 6) +
			orbitList.at<double>(0, 5) * orbitList.at<double>(0, 5) +
			orbitList.at<double>(0, 4) * orbitList.at<double>(0, 4));
		double krot = 2 * v * azimuthSteeringRate * PI / 180.0 / waveLength;
		for (int j = 0; j < samplesPerBurst; j++)
		{
			dopplerRate.at<double>(i, j) = rangeDependDopplerRate.at<double>(i, j) * krot /
				(rangeDependDopplerRate.at<double>(i, j) - krot);
		}
	}
	isDopplerRateAvailable = true;
	return 0;
}

int Sentinel1Utils::computeDerampDemodPhase(
	int burstIndex,
	Mat& derampDemodPhase
)
{
	if (!bInitialized)
	{
		fprintf(stderr, "Sentinel1Utils::computeDerampDemodPhase(): input check failed!\n");
		return -1;
	}
	if (burstIndex < 1 || burstIndex > burstCount)
	{
		fprintf(stderr, "Sentinel1Utils::computeDerampDemodPhase(): input check failed!\n");
		return -1;
	}
	int ret;
	if (!isDopplerRateAvailable)
	{
		ret = computeDopplerRate();
		if (return_check(ret, "computeDopplerRate()", error_head)) return -1;
	}
	if (!isDopplerCentroidAvailable)
	{
		ret = computeDopplerCentroid();
		if (return_check(ret, "computeDopplerCentroid()", error_head)) return -1;
	}
	if (!isReferenceTimeAvailable)
	{
		ret = computeReferenceTime();
		if (return_check(ret, "computeReferenceTime()", error_head)) return -1;
	}
	derampDemodPhase.create(linesPerBurst, samplesPerBurst, CV_64F);
	int firstLineInBurst = (burstIndex - 1) * linesPerBurst;
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < linesPerBurst; i++)
	{
		//double ta = double(i - firstLineInBurst) * azimuthTimeInterval;
		double ta = (double)i * azimuthTimeInterval;
		for (int j = 0; j < samplesPerBurst; j++)
		{
			double kt = dopplerRate.at<double>(burstIndex - 1, j);
			double deramp = -PI * kt * pow(ta - referenceTime.at<double>(burstIndex - 1, j), 2.0);
			double demod = -2 * PI * ta * dopplerCentroid.at<double>(burstIndex - 1, j);
			derampDemodPhase.at<double>(i, j) = deramp + demod;
		}
	}
	return 0;
}

int Sentinel1Utils::getBurst(int burstIndex, ComplexMat& burstSLC)
{
	if (!bInitialized)
	{
		fprintf(stderr, "Sentinel1Utils::getBurst(): input check failed!\n");
		return -1;
	}
	if (burstIndex < 1 || burstIndex > burstCount)
	{
		fprintf(stderr, "Sentinel1Utils::getBurst(): burstIndex out of legal range!\n");
		return -1;
	}
	int ret;
	FormatConversion conversion;
	ret = conversion.read_subarray_from_h5(this->h5File.c_str(), "s_re", linesPerBurst * (burstIndex - 1),
		0, linesPerBurst, samplesPerBurst, burstSLC.re);
	if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
	ret = conversion.read_subarray_from_h5(this->h5File.c_str(), "s_im", linesPerBurst * (burstIndex - 1),
		0, linesPerBurst, samplesPerBurst, burstSLC.im);
	if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;

	return 0;
}

int Sentinel1Utils::getDopplerFrequency(
	Position groundPosition,
	Position satellitePosition,
	Velocity satelliteVelocity,
	double* dopplerFrequency
)
{
	if (!bInitialized || !dopplerFrequency)
	{
		fprintf(stderr, "getDopplerFrequency(): input check failed!");
		return -1;
	}
	double waveLength = VEL_C / radarFrequency;
	double xdiff = groundPosition.x - satellitePosition.x;
	double ydiff = groundPosition.y - satellitePosition.y;
	double zdiff = groundPosition.z - satellitePosition.z;
	double distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	*dopplerFrequency = 2.0 * (xdiff * satelliteVelocity.vx + ydiff * satelliteVelocity.vy + zdiff * satelliteVelocity.vz) / (waveLength * distance);
	return 0;
}

int Sentinel1Utils::getZeroDopplerTime(Position groundPosition, double* zeroDopplerTime, double dopplerFrequency)
{
	g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_NONE;
	g_has_thread_zero_doppler_diagnostic = false;
	if (!bInitialized || !zeroDopplerTime || !stateVectors || !std::isfinite(radarFrequency) || radarFrequency <= 0.0 ||
		!std::isfinite(azimuthTimeInterval) || azimuthTimeInterval <= 0.0)
	{
		if (zeroDopplerTime) *zeroDopplerTime = std::numeric_limits<double>::quiet_NaN();
		g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_INVALID_INPUT;
		record_zero_doppler_diagnostic(this, groundPosition, dopplerFrequency, g_zero_doppler_failure_reason);
		return -1;
	}

	double wavelength = VEL_C / radarFrequency;
	double distance;
	if (!orbitStateVectors::findZeroDopplerTime(*stateVectors, groundPosition, wavelength, azimuthTimeInterval, dopplerFrequency, *zeroDopplerTime, distance, 0.01)) {
		*zeroDopplerTime = std::numeric_limits<double>::quiet_NaN();
		g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_NO_BRACKET;
		record_zero_doppler_diagnostic(this, groundPosition, dopplerFrequency, g_zero_doppler_failure_reason);
		return -1;
	}
	if (!std::isfinite(*zeroDopplerTime) || !std::isfinite(distance))
	{
		*zeroDopplerTime = std::numeric_limits<double>::quiet_NaN();
		g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT;
		record_zero_doppler_diagnostic(this, groundPosition, dopplerFrequency, g_zero_doppler_failure_reason);
		return -1;
	}

	return 0;
}

int Sentinel1Utils::getRgAzPosition(
	int burstIndex,
	Position groundPosition,
	double* rangeIndex,
	double* azimuthIndex
)
{
	int ret;
	g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_NONE;
	g_has_thread_zero_doppler_diagnostic = false;
	g_rgaz_projection_failure_reason = RGAZ_PROJECTION_NONE;
	if (!bInitialized || !rangeIndex || !azimuthIndex)
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_INVALID_INPUT;
		g_zero_doppler_failure_reason = SENTINEL_ZERO_DOPPLER_INVALID_INPUT;
		record_zero_doppler_diagnostic(this, groundPosition, 0.0, g_zero_doppler_failure_reason);
		return -1;
	}
	double zeroDopplerTime, slantRange;
	ret = getZeroDopplerTime(groundPosition, &zeroDopplerTime, 0.0);
	if (return_failed(ret)) return -1;
	*azimuthIndex = (zeroDopplerTime - burstAzimuthTime.at<double>(burstIndex - 1)) / azimuthTimeInterval;
	ret = getSlantRange(zeroDopplerTime, groundPosition, &slantRange);
	if (return_check(ret, "getSlantRange()", error_head))
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_SLANT_RANGE;
		return -1;
	}
	*rangeIndex = (slantRange - slantRangeTime * VEL_C * 0.5) / rangePixelSpacing;

	if (*rangeIndex < 0.0 || *rangeIndex >= samplesPerBurst)
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_RANGE_OUT_OF_BOUNDS;
		return -1;
	}
	if (*azimuthIndex < 0.0 || *azimuthIndex >= linesPerBurst)
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_BURST_OUT_OF_BOUNDS;
		return -1;
	}
	int x = static_cast<int>(*rangeIndex - 1); x = x < 0 ? 0 : x;
	ret = getZeroDopplerTime(groundPosition, &zeroDopplerTime, 0.0);
	if (return_failed(ret)) return -1;
	*azimuthIndex = (zeroDopplerTime - burstAzimuthTime.at<double>(burstIndex - 1)) / azimuthTimeInterval;
	ret = getSlantRange(zeroDopplerTime, groundPosition, &slantRange);
	if (return_check(ret, "getSlantRange()", error_head))
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_SLANT_RANGE;
		return -1;
	}
	*rangeIndex = (slantRange - slantRangeTime * VEL_C * 0.5) / rangePixelSpacing;
	if (*rangeIndex < 0.0 || *rangeIndex >= samplesPerBurst)
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_RANGE_OUT_OF_BOUNDS;
		return -1;
	}
	if (*azimuthIndex < 0.0 || *azimuthIndex >= linesPerBurst)
	{
		g_rgaz_projection_failure_reason = RGAZ_PROJECTION_BURST_OUT_OF_BOUNDS;
		return -1;
	}

	return 0;
}

int Sentinel1Utils::getSlantRange(double azimuthTime, Position groundPosition, double* slantRange)
{
	// removed unused: ret (direct computation, no API calls needing error checks)
	if (!bInitialized || !slantRange)
	{
		fprintf(stderr, "getSlantRange(): input check failed!");
		return -1;
	}
	Position satellitePosition;
	stateVectors->getPosition(azimuthTime, satellitePosition);
	double xdiff = groundPosition.x - satellitePosition.x;
	double ydiff = groundPosition.y - satellitePosition.y;
	double zdiff = groundPosition.z - satellitePosition.z;
	*slantRange = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	return 0;
}

int Sentinel1Utils::getBurstIndice(Position groundPosition, BurstIndices& burstIndice)
{
	if (!bInitialized)
	{
		fprintf(stderr, "getBurstIndice(): input check failed!");
		return -1;
	}
	int ret;
	double zeroDopplerTime;
	ret = getZeroDopplerTime(groundPosition, &zeroDopplerTime);
	if (return_failed(ret)) return -1;
	int k = 0;
	double burstFirstLineTime, burstLastLineTime;
	for (int i = 0; i < burstCount; i++) {
		burstFirstLineTime = burstAzimuthTime.at<double>(i, 0);
		burstLastLineTime = burstFirstLineTime + azimuthTimeInterval * (linesPerBurst - 1);
		if (zeroDopplerTime >= burstFirstLineTime && zeroDopplerTime < burstLastLineTime) {
			bool inUpperPartOfBurst = (zeroDopplerTime >= (burstFirstLineTime + burstLastLineTime) / 2.0);

			if (k == 0) {
				burstIndice.firstBurstIndex = i + 1;
				burstIndice.inUpperPartOfFirstBurst = inUpperPartOfBurst;
			}
			else {
				burstIndice.secondBurstIndex = i + 1;
				burstIndice.inUpperPartOfSecondBurst = inUpperPartOfBurst;
				break;
			}
			++k;
		}
	}
	if (k == 0) return -1;
	return 0;
}

int Sentinel1Utils::computeImageGeoBoundry(
	double* lonMin,
	double* lonMax,
	double* latMin,
	double* latMax
)
{
	if (!bInitialized || !lonMin || !lonMax || !latMin || !latMax)
	{
		fprintf(stderr, "computeImageGeoBoundry(): input check failed!");
		return -1;
	}
	*lonMin = 181.0;
	*lonMax = -181.0;
	*latMin = 91.0;
	*latMax = -91.0;
	for (int i = 0; i < geolocationGridPoint.rows; i++)
	{
		double lon = geolocationGridPoint.at<double>(i, 0);
		double lat = geolocationGridPoint.at<double>(i, 1);
		*lonMin = *lonMin > lon ? lon : *lonMin;
		*lonMax = *lonMax < lon ? lon : *lonMax;
		*latMin = *latMin > lat ? lat : *latMin;
		*latMax = *latMax < lat ? lat : *latMax;
	}
	double extra = 5.0 / 6000;
	*lonMin = *lonMin - extra * 100;
	*lonMax = *lonMax + extra * 100;
	*latMin = *latMin - extra * 100;
	*latMax = *latMax + extra * 100;
	return 0;
}

int Sentinel1Utils::computeImageGeoBoundry(double* lonMin, double* lonMax, double* latMin, double* latMax, int burstIndex)
{
	if (!bInitialized || !lonMin || !lonMax || !latMin || !latMax || burstIndex < 1 || burstIndex > this->burstCount)
	{
		fprintf(stderr, "computeImageGeoBoundry(): input check failed!");
		return -1;
	}
	*lonMin = 181.0;
	*lonMax = -181.0;
	*latMin = 91.0;
	*latMax = -91.0;
	// ���� GCPs ��Ӧ�ķ�λ���кţ��� 3 �У���׼ƥ�䵱ǰ burst ���з�Χ
	// �������û����ֵ��� burst ʱ��ʹ�� geolocationGridPoint.rows �����������µ�����ƫ�� bug
	bool found = false;
	double minR = (burstIndex - 1) * this->linesPerBurst;
	double maxR = burstIndex * this->linesPerBurst;
	for (int i = 0; i < geolocationGridPoint.rows; i++)
	{
		double r = geolocationGridPoint.at<double>(i, 3);
		if (r >= minR && r < maxR)
		{
			double lon = geolocationGridPoint.at<double>(i, 0);
			double lat = geolocationGridPoint.at<double>(i, 1);
			*lonMin = *lonMin > lon ? lon : *lonMin;
			*lonMax = *lonMax < lon ? lon : *lonMax;
			*latMin = *latMin > lat ? lat : *latMin;
			*latMax = *latMax < lat ? lat : *latMax;
			found = true;
		}
	}
	if (!found)
	{
		// ���þ����߼�
		int cols_num = geolocationGridPoint.rows / (this->burstCount + 1);
		int start_row = cols_num * (burstIndex - 1);
		int end_row = start_row + cols_num;
		if (end_row > geolocationGridPoint.rows) end_row = geolocationGridPoint.rows;
		for (int i = start_row; i < end_row; i++)
		{
			double lon = geolocationGridPoint.at<double>(i, 0);
			double lat = geolocationGridPoint.at<double>(i, 1);
			*lonMin = *lonMin > lon ? lon : *lonMin;
			*lonMax = *lonMax < lon ? lon : *lonMax;
			*latMin = *latMin > lat ? lat : *latMin;
			*latMax = *latMax < lat ? lat : *latMax;
		}
	}
	double extra = 5.0 / 6000;
	*lonMin = *lonMin - extra * 50;
	*lonMax = *lonMax + extra * 50;
	*latMin = *latMin - extra * 50;
	*latMax = *latMax + extra * 50;
	return 0;
}

int Sentinel1Utils::deburst(const char* outFile)
{
	if (!bInitialized || !outFile)
	{
		fprintf(stderr, "deburst(): input check failed!\n");
		return -1;
	}

	//�����µ�deburst�ļ�

	FormatConversion conversion;
	int ret = conversion.validate_distinct_h5_output(this->h5File.c_str(), outFile);
	if (return_check(ret, "validate_distinct_h5_output()", error_head)) return -1;
	ret = conversion.creat_new_h5(outFile);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	Mat start(this->burstCount, 1, CV_32S), end(this->burstCount, 1, CV_32S);
	start.at<int>(0, 0) = 1;
	end.at<int>(0, 0) = this->lastValidLine.at<int>(0, 0);
	double lastValidTime = this->burstAzimuthTime.at<double>(0, 0) +
		(this->lastValidLine.at<int>(0, 0) - 1) * this->azimuthTimeInterval;
	double firstValidTime;
	int deburstLines = 0;
	int overlap;
	for (int i = 1; i < this->burstCount; i++)
	{
		firstValidTime = this->burstAzimuthTime.at<double>(i, 0) + (this->firstValidLine.at<int>(i, 0) - 1) *
			this->azimuthTimeInterval;

		overlap = static_cast<int>(round((lastValidTime - firstValidTime) / this->azimuthTimeInterval + 1));

		end.at<int>(i - 1, 0) = end.at<int>(i - 1, 0) - int(overlap / 2);

		start.at<int>(i, 0) = this->linesPerBurst * i + this->firstValidLine.at<int>(i, 0) + overlap - int(overlap / 2);

		end.at<int>(i, 0) = this->linesPerBurst * i + this->lastValidLine.at<int>(i, 0);

		lastValidTime = this->burstAzimuthTime.at<double>(i, 0) +
			(this->lastValidLine.at<int>(i, 0) - 1) * this->azimuthTimeInterval;
	}
	end.at<int>(this->burstCount - 1, 0) = this->linesPerBurst * this->burstCount;
	start -= 1;
	//deburst
	ComplexMat tmp, tmp2, slc;
	ret = conversion.read_slc_from_h5(this->h5File.c_str(), tmp);
	//tmp.convertTo(tmp, CV_32F);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	slc = tmp(cv::Range(start.at<int>(0, 0), end.at<int>(0, 0)), cv::Range(0, this->samplesPerBurst));
	for (int i = 1; i < this->burstCount; i++)
	{
		tmp2 = tmp(cv::Range(start.at<int>(i, 0), end.at<int>(i, 0)), cv::Range(0, this->samplesPerBurst));
		cv::vconcat(slc.re, tmp2.re, slc.re);
		cv::vconcat(slc.im, tmp2.im, slc.im);
	}
	//���ļ���д���������ݺ͸�������
	ret = conversion.write_slc_to_h5(outFile, slc);
	if (return_check(ret, "write_slc_to_h5()", error_head)) return -1;
	ret = conversion.Copy_para_from_h5_2_h5(this->h5File.c_str(), outFile);
	if (return_check(ret, "Copy_para_from_h5_2_h5()", error_head)) return -1;
	conversion.write_str_to_h5(outFile, "process_state", "deburst");
	conversion.write_str_to_h5(outFile, "comment", "complex-1.0");
	conversion.write_int_to_h5(outFile, "offset_row", 0);
	conversion.write_int_to_h5(outFile, "offset_col", 0);
	conversion.write_int_to_h5(outFile, "azimuth_len", slc.GetRows());
	conversion.write_int_to_h5(outFile, "range_len", slc.GetCols());

	return 0;
}













DigitalElevationModel::DigitalElevationModel()
{
	memset(this->error_head, 0, 256);
	strcpy(this->error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using ");
	this->lonSpacing = 5.0 / 6000.0;
	this->latSpacing = 5.0 / 6000.0;
}

DigitalElevationModel::~DigitalElevationModel()
{
}

int DigitalElevationModel::getSRTMFileName(
	double lonMin,
	double lonMax,
	double latMin,
	double latMax,
	vector<string>& name
)
{
	if (fabs(lonMin) > 180.0 ||
		fabs(lonMax) > 180.0 ||
		fabs(latMin) >= 60.0 ||
		fabs(latMax) >= 60.0
		)
	{
		fprintf(stderr, "getSRTMFileName(): input check failed!\n");
		return -1;
	}
	name.clear();
	int startRow, endRow, startCol, endCol;
	double spacing = 5.0;
	startRow = (int)((60.0 - latMax) / spacing) + 1;
	endRow = (int)((60.0 - latMin) / spacing) + 1;
	startCol = (int)((lonMin + 180.0) / spacing) + 1;
	endCol = (int)((lonMax + 180.0) / spacing) + 1;
	if (startRow == endRow)
	{
		if (startCol == endCol)
		{
			name.push_back(formatSRTMName(startCol, startRow));
		}
		else
		{
			name.push_back(formatSRTMName(startCol, startRow));
			name.push_back(formatSRTMName(endCol, startRow));
		}
	}
	else
	{
		if (startCol == endCol)
		{
			name.push_back(formatSRTMName(startCol, startRow));
			name.push_back(formatSRTMName(startCol, endRow));
		}
		else
		{
			name.push_back(formatSRTMName(startCol, startRow));
			name.push_back(formatSRTMName(endCol, startRow));
			name.push_back(formatSRTMName(endCol, endRow));
			name.push_back(formatSRTMName(startCol, endRow));
		}
	}

	return 0;
}

int DigitalElevationModel::downloadSRTM(const char* name)
{
	// removed unused: ret (HRESULT used for error check instead)
	string url = this->SRTMURL + name;
	string savefile = this->DEMPath + string("\\") + name;
	std::replace(savefile.begin(), savefile.end(), '/', '\\');
	HRESULT Result = URLDownloadToFileA(NULL, url.c_str(), savefile.c_str(), 0, NULL);
	if (Result != S_OK)
	{
		fprintf(stderr, "downloadSRTM(): download failded!\n");
		return -1;
	}
	return 0;
}

int DigitalElevationModel::getRawDEM(
	const char* filepath,
	double lonMin,
	double lonMax,
	double latMin,
	double latMax
)
{
	if (!filepath) return -1;
	this->DEMPath = filepath;

	string finalPath = filepath;
	DWORD attr = GetFileAttributesA(filepath);
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		string searchPath = string(filepath) + "\\*.tif";
		std::replace(searchPath.begin(), searchPath.end(), '/', '\\');

		intptr_t handle;
		struct _finddata_t fileinfo;
		handle = _findfirst(searchPath.c_str(), &fileinfo);
		if (handle != -1)
		{
			finalPath = string(filepath) + "\\" + fileinfo.name;
			_findclose(handle);
		}
		else
		{
			searchPath = string(filepath) + "\\*.tiff";
			std::replace(searchPath.begin(), searchPath.end(), '/', '\\');
			handle = _findfirst(searchPath.c_str(), &fileinfo);
			if (handle != -1)
			{
				finalPath = string(filepath) + "\\" + fileinfo.name;
				_findclose(handle);
			}
			else
			{
				fprintf(stderr, "getRawDEM(): No .tif/.tiff found in directory %s!\n", filepath);
				return -1;
			}
		}
	}

	InitializeGDALOnce();

	GDALDataset* poDataset = (GDALDataset*)GDALOpen(finalPath.c_str(), GA_ReadOnly);
	if (poDataset == NULL)
	{
		fprintf(stderr, "getRawDEM(): failed to open DEM file %s!\n", finalPath.c_str());
		return -1;
	}

	int nBand = poDataset->GetRasterCount();
	if (nBand < 1)
	{
		fprintf(stderr, "getRawDEM(): DEM file has no bands!\n");
		GDALClose(poDataset);
		return -1;
	}

	GDALRasterBand* poBand = poDataset->GetRasterBand(1);
	if (poBand == NULL)
	{
		fprintf(stderr, "getRawDEM(): failed to get DEM raster band 1!\n");
		GDALClose(poDataset);
		return -1;
	}

	int xsize = poBand->GetXSize();
	int ysize = poBand->GetYSize();

	double adfGeoTransform[6];
	if (poDataset->GetGeoTransform(adfGeoTransform) != CE_None)
	{
		fprintf(stderr, "getRawDEM(): failed to get GeoTransform from %s!\n", finalPath.c_str());
		GDALClose(poDataset);
		return -1;
	}

	double colMin = (lonMin - adfGeoTransform[0]) / adfGeoTransform[1];
	double colMax = (lonMax - adfGeoTransform[0]) / adfGeoTransform[1];
	double rowMin = (latMax - adfGeoTransform[3]) / adfGeoTransform[5];
	double rowMax = (latMin - adfGeoTransform[3]) / adfGeoTransform[5];

	int cMin = (int)floor(colMin);
	int cMax = (int)ceil(colMax);
	int rMin = (int)floor(rowMin);
	int rMax = (int)ceil(rowMax);

	if (rMin > rMax) std::swap(rMin, rMax);

	cMin = std::max(0, std::min(cMin, xsize - 1));
	cMax = std::max(0, std::min(cMax, xsize - 1));
	rMin = std::max(0, std::min(rMin, ysize - 1));
	rMax = std::max(0, std::min(rMax, ysize - 1));

	int readCols = cMax - cMin + 1;
	int readRows = rMax - rMin + 1;

	if (readCols <= 0 || readRows <= 0)
	{
		fprintf(stderr, "getRawDEM(): computed crop dimensions are invalid! cols=%d rows=%d\n", readCols, readRows);
		GDALClose(poDataset);
		return -1;
	}

	short* pbuf = (short*)malloc(sizeof(short) * readCols * readRows);
	if (!pbuf)
	{
		fprintf(stderr, "getRawDEM(): out of memory for pbuf!\n");
		GDALClose(poDataset);
		return -1;
	}

	if (poBand->RasterIO(GF_Read, cMin, rMin, readCols, readRows, pbuf, readCols, readRows, GDT_Int16, 0, 0) != CE_None)
	{
		fprintf(stderr, "getRawDEM(): RasterIO failed reading %s!\n", finalPath.c_str());
		free(pbuf);
		GDALClose(poDataset);
		return -1;
	}

	this->rawDEM.create(readRows, readCols, CV_16S);
	memcpy(this->rawDEM.data, pbuf, sizeof(short) * readCols * readRows);
	free(pbuf);
	GDALClose(poDataset);

	for (int i = 0; i < this->rawDEM.rows; i++)
	{
		for (int j = 0; j < this->rawDEM.cols; j++)
		{
			if (this->rawDEM.at<short>(i, j) < 0)
				this->rawDEM.at<short>(i, j) = 0;
		}
	}

	this->rows = readRows;
	this->cols = readCols;
	this->lonSpacing = adfGeoTransform[1];
	this->latSpacing = -adfGeoTransform[5];
	this->lonUpperLeft = adfGeoTransform[0] + cMin * adfGeoTransform[1];
	this->latUpperLeft = adfGeoTransform[3] + rMin * adfGeoTransform[5];

	return 0;
}

int DigitalElevationModel::getElevation(double lon, double lat, double* elevation)
{
	if (!elevation) return -1;
	int row, col;
	row = cvRound((this->latUpperLeft - lat) / this->latSpacing);
	col = cvRound((lon - this->lonUpperLeft) / this->lonSpacing);
	if (row < 0 || col < 0 || row >= this->rows || col >= this->cols) return -1;
	double elevationUL, elevationUR, elevationLL, elevationLR;
	// removed unused: upper, lower (bilinear interpolation remnant, using simple average instead)
	int r, c, r1, c1;
	r = row; c = col; r1 = r + 1; c1 = c + 1;
	r1 = r1 > this->rows - 1 ? this->rows - 1 : r1;
	c1 = c1 > this->cols - 1 ? this->cols - 1 : c1;
	elevationUL = this->rawDEM.at<short>(r, c);
	elevationUR = this->rawDEM.at<short>(r, c1);
	elevationLL = this->rawDEM.at<short>(r1, c);
	elevationLR = this->rawDEM.at<short>(r1, c1);
	*elevation = (elevationLL + elevationLR + elevationUL + elevationUR) / 4.0;
	return 0;
}

    int DigitalElevationModel::geotiffread(const char* filename, Mat& outDEM)
    {
        if (!filename)
            return -1;

        InitializeGDALOnce();    // ע����֪���������̰߳�ȫ

        GDALDataset* poDataset = (GDALDataset*)GDALOpen(filename, GA_ReadOnly);
        if (poDataset == NULL)
        {
            fprintf(stderr, "geotiffread(): failed to open %s!\n", filename);
            return -1;
        }

        int nBand = poDataset->GetRasterCount();
        if (nBand != 1)
        {
            fprintf(stderr, "geotiffread(): number of Bands != 1\n");
            GDALClose(poDataset);
            return -1;
        }

        GDALRasterBand* poBand = poDataset->GetRasterBand(1);
        if (poBand == NULL)
        {
            fprintf(stderr, "geotiffread(): failed to get band!\n");
            GDALClose(poDataset);
            return -1;
        }

        int xsize = poBand->GetXSize();  // cols
        int ysize = poBand->GetYSize();  // rows
        if (xsize <= 0 || ysize <= 0)
        {
            fprintf(stderr, "geotiffread(): band rows and cols error!\n");
            GDALClose(poDataset);
            return -1;
        }

        /* ԭʼ�������ͣ�����Ϊ GDT_Int16�� */
        GDALDataType srcType = poBand->GetRasterDataType();

        /* ���仺������short�� */
        short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize);
        if (!pbuf)
        {
            fprintf(stderr, "geotiffread(): out of memory!\n");
            GDALClose(poDataset);
            return -1;
        }

        /* ��ȡ��ǿ���� GDT_Int16 ��������� memcpy ���Ͳ�ƥ�� */
        if (poBand->RasterIO(GF_Read, 0, 0, xsize, ysize, pbuf, xsize, ysize, GDT_Int16, 0,
  0) != CE_None)
        {
            fprintf(stderr, "geotiffread(): RasterIO failed\n");
            free(pbuf);
            GDALClose(poDataset);
            return -1;
        }

        outDEM.create(ysize, xsize, CV_16S);
        memcpy(outDEM.data, pbuf, sizeof(short) * xsize * ysize);

        free(pbuf);
        pbuf = NULL;
        GDALClose(poDataset);

        /* ����ֵ���㣨������ԭ���� OpenMP �߼��� */
        int rows = outDEM.rows;
        int cols = outDEM.cols;

    #pragma omp parallel for schedule(guided)
        for (int i = 0; i < rows; i++)
        {
            short* rowp = outDEM.ptr<short>(i);
            for (int j = 0; j < cols; j++)
            {
                if (rowp[j] < 0)
                    rowp[j] = 0;
            }
        }

        return 0;
    }




int DigitalElevationModel::unzip(const char* srcFile, const char* dstPath)
{
	if (!srcFile || !dstPath)
	{
		fprintf(stderr, "unzip(): input check failed!\n");
		return -1;
	}
	int ret;
	//���Ŀ���ļ��в����ڣ��򴴽�
	if (-1 == GetFileAttributesA(dstPath))
	{
		ret = _mkdir(dstPath);
		if (ret < 0)return -1;
	}
	//////////////////////////����������unzip.exe����///////////////////////////////
	char szFilePath[MAX_PATH + 1] = { 0 };
	GetModuleFileNameA(NULL, szFilePath, MAX_PATH);
	string str(szFilePath);
	str = str.substr(0, str.rfind("\\"));
	string commandline = str + string("\\unzip.exe ") + string(srcFile) + string(" ") + string(dstPath);
	char szCommandLine[1024];
	strcpy(szCommandLine, commandline.c_str());
	STARTUPINFOA si;
	PROCESS_INFORMATION p_i;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&p_i, sizeof(p_i));
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = FALSE;
	BOOL bRet = ::CreateProcessA(
		NULL,           // ���ڴ�ָ����ִ���ļ����ļ���
		szCommandLine,      // �����в���
		NULL,           // Ĭ�Ͻ��̰�ȫ��
		NULL,           // Ĭ���̰߳�ȫ��
		FALSE,          // ָ����ǰ�����ڵľ�������Ա��ӽ��̼̳�
		CREATE_NEW_CONSOLE, // Ϊ�½��̴���һ���µĿ���̨����
		NULL,           // ʹ�ñ����̵Ļ�������
		NULL,           // ʹ�ñ����̵���������Ŀ¼
		&si,
		&p_i);
	if (bRet)
	{
		char snaphu_job_name[512]; snaphu_job_name[0] = 0;
		time_t tt = std::time(0);
		sprintf(snaphu_job_name, "UNZIP_%lld", tt);
		string snaphu_job_name_string(snaphu_job_name);
		HANDLE hd = CreateJobObjectA(NULL, snaphu_job_name_string.c_str());
		if (hd)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION extLimitInfo;
			extLimitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			BOOL retval = SetInformationJobObject(hd, JobObjectExtendedLimitInformation, &extLimitInfo, sizeof(extLimitInfo));
			if (retval)
			{
				if (p_i.hProcess)
				{
					retval = AssignProcessToJobObject(hd, p_i.hProcess);
				}
			}
		}
		WaitForSingleObject(p_i.hProcess, INFINITE);
		::CloseHandle(p_i.hThread);
		::CloseHandle(p_i.hProcess);
	}
	else
	{
		fprintf(stderr, "unzip(): create unzip.exe process failed!\n\n");
		return -1;
	}
	return 0;
}






















Sentinel1BackGeocoding::Sentinel1BackGeocoding()
{
	cancelRequested.store(false, std::memory_order_relaxed);
	diagnosticCallback = nullptr;
	diagnosticUserData = nullptr;
	memset(this->error_head, 0, 256);
	strcpy(this->error_head, "FORMATCONVERSION_DLL_ERROR: error happens when using ");
	this->dem = NULL;
	this->burstOffsetComputed = false;
	this->isMasterRgAzComputed = false;
	this->isdeBurstConfig = false;
	this->deferFinalDeburstOutput = false;
	this->masterIndex = 1;
	this->numOfImages = 0;
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		g_sentinel_diagnostic_state[this] = SentinelBackGeocodingDiagnosticState();
	}
}

void Sentinel1BackGeocoding::requestCancel() noexcept
{
	cancelRequested.store(true, std::memory_order_release);
}

void Sentinel1BackGeocoding::clearCancelRequest() noexcept
{
	cancelRequested.store(false, std::memory_order_release);
}

bool Sentinel1BackGeocoding::isCancelRequested() const noexcept
{
	return cancelRequested.load(std::memory_order_acquire);
}

void Sentinel1BackGeocoding::setDiagnosticCallback(
	InSARDiagnosticCallback callback,
	void* userData) noexcept
{
	diagnosticCallback = callback;
	diagnosticUserData = userData;
}

Sentinel1BackGeocoding::~Sentinel1BackGeocoding()
{
	if (dem)
	{
		delete dem; dem = NULL;
	}
	for (int i = 0; i < su.size(); i++)
	{
		if (su[i])
		{
			delete su[i]; su[i] = NULL;
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		g_sentinel_diagnostic_state.erase(this);
	}
}

int Sentinel1BackGeocoding::init(
	vector<string>& h5Files,
	vector<string>& outFiles,
	const char* DEMPath,
	int masterIndex,
	InSARDiagnosticCallback callback,
	void* userData
)
{
	if (callback || userData)
		setDiagnosticCallback(callback, userData);
	ScopedDiagnosticContext diagnosticScope(diagnosticCallback, diagnosticUserData);
	clearCancelRequest();
	char initMessage[512] = {};
	sprintf_s(initMessage, "Initializing Sentinel-1 back-geocoding: images=%zu, master=%d.", h5Files.size(), masterIndex);
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "coregistration", "initialize", initMessage,
		"Input metadata, DEM configuration, and output datasets will be validated.", DEMPath);
	emit_progress(10, "Initializing Sentinel-1 back-geocoding.");
	if (h5Files.size() < 2 || outFiles.size() != h5Files.size() || !DEMPath || !*DEMPath ||
		masterIndex < 1 || masterIndex > static_cast<int>(h5Files.size()))
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "coregistration", "initialize.validate_input", "Invalid back-geocoding input contract.",
			"At least two input files, matching output files, a DEM path, and a valid master index are required.", DEMPath, nullptr, -1101);
		return -1;
	}
	for (size_t i = 0; i < h5Files.size(); i++)
	{
		if (h5Files[i].empty() || outFiles[i].empty())
		{
			emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "coregistration", "initialize.validate_input", "Input or output path is empty.",
				"Each image must have a non-empty input HDF5 path and output HDF5 path.", h5Files[i].c_str(), nullptr, -1102);
			return -1;
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		SentinelBackGeocodingDiagnosticState& diagnosticState = g_sentinel_diagnostic_state[this];
		diagnosticState.burstStatus.clear();
		diagnosticState.zeroDopplerFailureStatistics.clear();
		diagnosticState.hasZeroDopplerDiagnostic = false;
		diagnosticState.zeroOffsetFallback = false;
	}
	int ret;
	ret = loadData(h5Files);
	if (return_check(ret, "loadData()", error_head)) return -1;
	for (int i = 0; i < numOfImages; i++)
	{
		if (validate_sentinel_metadata(su[i]) < 0)
		{
			ScopedDiagnosticContext imageDiagnosticScope(diagnosticCallback, diagnosticUserData, i + 1);
			emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "coregistration", "initialize.validate_metadata", "Sentinel-1 metadata contract validation failed.",
				"Required burst, timing, valid-line, and orbit-vector metadata are incomplete or inconsistent.", su[i]->h5File.c_str(), nullptr, -1103);
			return -1;
		}
	}
	ret = setDEMPath(DEMPath);
	if (return_check(ret, "setDEMPath()", error_head)) return -1;
	ret = loadOutFiles(outFiles);
	if (return_check(ret, "loadOutFiles()", error_head)) return -1;
	ret = setMasterIndex(masterIndex);
	if (return_check(ret, "setMasterIndex()", error_head)) return -1;
	ret = deBurstConfig();
	if (return_check(ret, "deBurstConfig()", error_head)) return -1;
	ret = prepareOutFiles();
	if (return_check(ret, "prepareOutFiles()", error_head)) return -1;
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "coregistration", "initialize.complete", "Back-geocoding initialization completed.",
		"Input metadata and output dataset preparation succeeded.", DEMPath);
	emit_progress(15, "Input metadata and working output are prepared.");

	return 0;
}

int Sentinel1BackGeocoding::loadData(vector<string>& h5Files)
{
	ScopedDiagnosticContext diagnosticScope(diagnosticCallback, diagnosticUserData);
	if (h5Files.size() < 2)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "coregistration", "load_data.validate_input", "Insufficient Sentinel-1 input files.",
			"At least a master and one slave image are required.", nullptr, nullptr, -1110);
		return -1;
	}
	int ret;
	this->numOfImages = static_cast<int>(h5Files.size());
	//�����������
	for (int i = 0; i < su.size(); i++)
	{
		if (su[i])
		{
			delete su[i]; su[i] = NULL;
		}
	}
	su.clear();
	for (int i = 0; i < numOfImages; i++)
	{
		ScopedDiagnosticContext imageDiagnosticScope(diagnosticCallback, diagnosticUserData, i + 1);
		emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "coregistration", "load_data.image", "Loading Sentinel-1 image metadata.",
			nullptr, h5Files[i].c_str());
		su.push_back(new Sentinel1Utils(h5Files[i].c_str()));
		if (su[i])
		{
			ret = su[i]->init();
			if (return_check(ret, "init()", error_head)) return -1;
			char message[512] = {};
			sprintf_s(message, "Sentinel-1 metadata loaded: swath=%s, polarization=%s, bursts=%d, linesPerBurst=%d, samplesPerBurst=%d, stateVectors=%d, preciseStateVectors=%d.",
				su[i]->swath.c_str(), su[i]->polarization.c_str(), su[i]->burstCount, su[i]->linesPerBurst,
				su[i]->samplesPerBurst, su[i]->orbitList.rows, su[i]->preciseOrbitList.rows);
			emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "coregistration", "load_data.metadata", message,
				"Acquisition timing is represented by burst azimuth time metadata.", su[i]->h5File.c_str());
		}
	}
	return 0;
}

int Sentinel1BackGeocoding::setDEMPath(const char* DEMPath)
{
	if (!DEMPath)
	{
		fprintf(stderr, "setDEMPath(): input check failed!\n");
		return -1;
	}
	this->DEMPath = DEMPath;
	return 0;
}

int Sentinel1BackGeocoding::loadDEM(
	const char* filepath,
	double lonMin,
	double lonMax,
	double latMin,
	double latMax
)
{
	int ret;
	if (dem) {
		delete dem; dem = NULL;
	}
	dem = new DigitalElevationModel();
	const ULONGLONG loadStartTick = GetTickCount64();
	ret = dem->getRawDEM(filepath, lonMin, lonMax, latMin, latMax);
	if (return_check(ret, "getRawDEM()", error_head)) return -1;
	const double lonLowerRight = dem->lonUpperLeft + (dem->cols - 1) * dem->lonSpacing;
	const double latLowerRight = dem->latUpperLeft - (dem->rows - 1) * dem->latSpacing;
	char message[768] = {};
	sprintf_s(message, "DEM loaded: raster=%dx%d, longitude=[%.8f, %.8f], latitude=[%.8f, %.8f], spacing=(%.10f, %.10f), memoryBytes=%lld.",
		dem->cols, dem->rows, dem->lonUpperLeft, lonLowerRight, latLowerRight, dem->latUpperLeft,
		dem->lonSpacing, dem->latSpacing, static_cast<long long>(dem->rawDEM.total() * dem->rawDEM.elemSize()));
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "dem", "load.complete", message,
		"DEM NoData is normalized by the current reader and cannot be distinguished from valid zero elevation.",
		filepath, nullptr, 0, dem->rows, dem->cols, dem->rawDEM.type(),
		static_cast<long long>(GetTickCount64() - loadStartTick));
	return 0;
}

int Sentinel1BackGeocoding::loadOutFiles(vector<string>& outFiles)
{
	if (outFiles.size() != su.size())
	{
		fprintf(stderr, "loadOutFiles(): input check failed!\n");
		return -1;
	}
	// Preserve the group paths before inspecting its transaction state.  A failed
	// initialization must still leave enough context for the caller to invoke
	// recoverPostRegistrationRefinementTransaction().
	this->outFiles = outFiles;
	if (validate_refinement_manifest(outFiles) != 0)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "refinement", "refinement.load_rejected",
			"Refinement output group is incomplete, failed, or transaction-inconsistent.",
			"A directory manifest is authoritative; do not load or publish this output group until it is recovered.");
		return -1;
	}
	return 0;
}

int Sentinel1BackGeocoding::prepareOutFiles()
{
	const ULONGLONG preparationStartTick = GetTickCount64();
	int ret; FormatConversion conversion;
	if (!isdeBurstConfig)
	{
		ret = deBurstConfig();
		if (return_check(ret, "deBurstConfig()", error_head)) return -1;
	}
	if (numOfImages < 2) return -1;
	ComplexMat fullBurst;
	ret = conversion.read_slc_from_h5(su[masterIndex - 1]->h5File.c_str(), fullBurst);
	fullBurst.convertTo(fullBurst, CV_32F);
	if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
	const int fullBurstLines = su[masterIndex - 1]->burstCount * su[masterIndex - 1]->linesPerBurst;
	if (fullBurst.GetRows() != fullBurstLines || fullBurst.GetCols() != su[masterIndex - 1]->samplesPerBurst)
		return -1;
	this->deburstLines = 0;
	for (int burst = 0; burst < start.rows; ++burst)
		this->deburstLines += end.at<int>(burst, 0) - start.at<int>(burst, 0);
	this->fullBurstFiles.assign(numOfImages, std::string());
	for (int i = 0; i < numOfImages; i++)
	{
		ScopedDiagnosticContext imageDiagnosticScope(diagnosticCallback, diagnosticUserData, i + 1);
		const ULONGLONG outputStartTick = GetTickCount64();
		this->fullBurstFiles[i] = this->outFiles[i] + ".fullburst";
		ret = conversion.creat_new_h5(this->fullBurstFiles[i].c_str());
			if (return_check(ret, "creat_new_h5()", error_head)) return -1;
		ret = conversion.write_slc_to_h5(this->fullBurstFiles[i].c_str(), fullBurst);
		if (return_check(ret, "write_slc_to_h5()", error_head)) return -1;
		char message[384] = {};
		sprintf_s(message, "Prepared full-burst working HDF5: dimensions=%dx%d, datasets=s_re,s_im.", fullBurst.GetRows(), fullBurst.GetCols());
		emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "hdf5", "prepare_output.complete", message,
			nullptr, this->fullBurstFiles[i].c_str(), nullptr, 0, fullBurst.GetRows(), fullBurst.GetCols(), fullBurst.type(),
			static_cast<long long>(GetTickCount64() - outputStartTick));
	}
	char message[384] = {};
	sprintf_s(message, "Full-burst working output preparation completed: bursts=%d, lines=%d, samples=%d; final deburst lines=%d.",
		su[masterIndex - 1]->burstCount, fullBurstLines, su[masterIndex - 1]->samplesPerBurst, deburstLines);
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "deburst", "prepare_full_burst.complete", message,
		nullptr, nullptr, nullptr, 0, fullBurstLines, su[masterIndex - 1]->samplesPerBurst, CV_32F,
		static_cast<long long>(GetTickCount64() - preparationStartTick));
	return 0;
}

int Sentinel1BackGeocoding::materializeDeburstOutput(const char* fullBurstFile, const char* deburstFile)
{
	if (!fullBurstFile || !deburstFile || start.empty() || end.empty() || start.rows != end.rows ||
		masterIndex < 1 || masterIndex > static_cast<int>(su.size()) || !su[masterIndex - 1])
		return -1;
	const ULONGLONG materializeStartTick = GetTickCount64();
	const Sentinel1Utils* master = su[masterIndex - 1];
	const int linesPerBurst = master->linesPerBurst;
	if (GetFileAttributesA(deburstFile) != INVALID_FILE_ATTRIBUTES && !DeleteFileA(deburstFile)) return -1;
	if (!CopyFileA(fullBurstFile, deburstFile, TRUE)) return -1;

	int reRows = 0, reColumns = 0, imRows = 0, imColumns = 0;
	if (Hdf5IO::getDatasetDims(fullBurstFile, "s_re", &reRows, &reColumns) != 0 ||
		Hdf5IO::getDatasetDims(fullBurstFile, "s_im", &imRows, &imColumns) != 0 ||
		reRows != imRows || reColumns != imColumns ||
		reRows != master->burstCount * linesPerBurst || reColumns != master->samplesPerBurst)
		return -1;

	int materializedRows = 0;
	for (int burst = 0; burst < start.rows; ++burst)
	{
		const int firstRow = start.at<int>(burst, 0);
		const int lastRow = end.at<int>(burst, 0);
		const int burstFirstRow = burst * linesPerBurst;
		const int burstLastRow = burstFirstRow + linesPerBurst;
		// deBurstConfig stores full-burst coordinates.  Keep every retained span
		// inside its source burst so a malformed configuration cannot mix bursts.
		if (firstRow < burstFirstRow || lastRow <= firstRow || lastRow > burstLastRow) return -1;
		materializedRows += lastRow - firstRow;
	}
	if (materializedRows != deburstLines) return -1;

	// Probe the stored type without materializing the full SLC, then replace the
	// copied full-burst datasets with final-size chunked datasets.
	cv::Mat reSample, imSample;
	const int firstRetainedRow = start.at<int>(0, 0);
	if (Hdf5IO::readSubarray(fullBurstFile, "s_re", firstRetainedRow, 0, 1, reColumns, reSample) != 0 ||
		Hdf5IO::readSubarray(fullBurstFile, "s_im", firstRetainedRow, 0, 1, imColumns, imSample) != 0 ||
		reSample.empty() || imSample.empty() || reSample.type() != imSample.type())
		return -1;
	if (Hdf5IO::removeDatasetIfPresent(deburstFile, "s_re") < 0 ||
		Hdf5IO::removeDatasetIfPresent(deburstFile, "s_im") < 0 ||
		Hdf5IO::createEmptyDataset(deburstFile, "s_re", deburstLines, reColumns, reSample.type()) != 0 ||
		Hdf5IO::createEmptyDataset(deburstFile, "s_im", deburstLines, imColumns, imSample.type()) != 0)
		return -1;

	cv::Mat burstPart;
	int outputRow = 0;
	for (int burst = 0; burst < start.rows; ++burst)
	{
		const int inputRow = start.at<int>(burst, 0);
		const int retainedRows = end.at<int>(burst, 0) - inputRow;
		if (Hdf5IO::readSubarray(fullBurstFile, "s_re", inputRow, 0, retainedRows, reColumns, burstPart) != 0 ||
			Hdf5IO::writeSubarray(deburstFile, "s_re", burstPart, outputRow, 0) != 0 ||
			Hdf5IO::readSubarray(fullBurstFile, "s_im", inputRow, 0, retainedRows, imColumns, burstPart) != 0 ||
			Hdf5IO::writeSubarray(deburstFile, "s_im", burstPart, outputRow, 0) != 0)
			return -1;
		outputRow += retainedRows;
	}
	if (outputRow != deburstLines) return -1;
	char message[384] = {};
	if (Hdf5IO::writeInt(deburstFile, "deburst_first_source_row", firstRetainedRow) != 0 ||
		Hdf5IO::writeInt(deburstFile, "deburst_first_source_column", 0) != 0 ||
		Hdf5IO::writeInt(deburstFile, "deburst_index_base", 0) != 0)
		return -1;

	sprintf_s(message, "Materialized final deburst HDF5 from full-burst dimensions=%dx%d to dimensions=%dx%d; first output sample maps to full-burst row=%d, column=0.",
		reRows, reColumns, deburstLines, reColumns, firstRetainedRow);
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "deburst", "materialize.complete", message,
		"Full-burst data remains the refinement source until the output transaction commits.", deburstFile,
		nullptr, 0, deburstLines, reColumns, reSample.type(),
		static_cast<long long>(GetTickCount64() - materializeStartTick));
	return 0;
}

int Sentinel1BackGeocoding::setMasterIndex(int masterIndex)
{
	if (masterIndex < 1 || masterIndex > numOfImages)
	{
		fprintf(stderr, "setMasterIndex(): input check failed!\n");
		return -1;
	}
	this->masterIndex = masterIndex;
	return 0;
}

int Sentinel1BackGeocoding::computeBurstOffset()
{
	if (burstOffsetComputed) return 0;
	if (!dem || su.size() < 2)
	{
		fprintf(stderr, "computeBurstOffset(): input check failed!\n");
		return -1;
	}
	int ret, numOfGeoLocationPoints;
	Position earthPoint;
	FormatConversion conversion;
	double lon, lat, elevation;
	numOfGeoLocationPoints = su[masterIndex - 1]->geolocationGridPoint.rows;
	for (int i = 0; i < numOfGeoLocationPoints; i++)
	{
		lon = su[masterIndex - 1]->geolocationGridPoint.at<double>(i, 0);
		lat = su[masterIndex - 1]->geolocationGridPoint.at<double>(i, 1);
		dem->getElevation(lon, lat, &elevation);
		conversion.ell2xyz(lon, lat, elevation, earthPoint);
		BurstIndices mBurstIndices, sBurstIndices;
		ret = su[masterIndex - 1]->getBurstIndice(earthPoint, mBurstIndices);
		if (ret < 0) continue;
		for (int j = 0; j < numOfImages; j++)
		{
			if (j == masterIndex - 1) continue;
			ret = su[j]->getBurstIndice(earthPoint, sBurstIndices);
			if (ret < 0 || (mBurstIndices.firstBurstIndex == -1 && mBurstIndices.secondBurstIndex == -1) ||
				(sBurstIndices.firstBurstIndex == -1 && sBurstIndices.secondBurstIndex == -1)) {
				continue;
			}
			if (mBurstIndices.inUpperPartOfFirstBurst == sBurstIndices.inUpperPartOfFirstBurst) {
				su[j]->burstOffset = sBurstIndices.firstBurstIndex - mBurstIndices.firstBurstIndex;
			}
			else if (sBurstIndices.secondBurstIndex != -1 &&
				mBurstIndices.inUpperPartOfFirstBurst == sBurstIndices.inUpperPartOfSecondBurst) {
				su[j]->burstOffset = sBurstIndices.secondBurstIndex - mBurstIndices.firstBurstIndex;
			}
			else if (mBurstIndices.secondBurstIndex != -1 &&
				mBurstIndices.inUpperPartOfSecondBurst == sBurstIndices.inUpperPartOfFirstBurst) {
				su[j]->burstOffset = sBurstIndices.firstBurstIndex - mBurstIndices.secondBurstIndex;
			}
			else if (mBurstIndices.secondBurstIndex != -1 && sBurstIndices.secondBurstIndex != -1 &&
				mBurstIndices.inUpperPartOfSecondBurst == sBurstIndices.inUpperPartOfSecondBurst) {
				su[j]->burstOffset = sBurstIndices.secondBurstIndex - mBurstIndices.secondBurstIndex;
			}
		}
		bool allComputed = true;
		for (int j = 0; j < numOfImages; j++) {
			if (j == masterIndex - 1) continue;
			if (su[j]->burstOffset == -9999) {
				allComputed = false;
				break;
			}
		}
		if (!allComputed)
			continue;

		burstOffsetComputed = true;
		for (int j = 0; j < numOfImages; j++) {
			if (j == masterIndex - 1) continue;
			char message[256] = {};
			sprintf_s(message, "Burst alignment calculated: master=%d, slave=%d, offset=%d.",
				masterIndex, j + 1, su[j]->burstOffset);
			ScopedDiagnosticContext slaveDiagnosticScope(diagnosticCallback, diagnosticUserData, j + 1);
			emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "coregistration", "burst_alignment", message);
		}
		return 0;
	}
	for (int j = 0; j < numOfImages; j++) {
		if (j == masterIndex - 1) continue;
		su[j]->burstOffset = 0;
	}
	burstOffsetComputed = true;
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		g_sentinel_diagnostic_state[this].zeroOffsetFallback = true;
	}
	emit_diagnostic(INSAR_DIAGNOSTIC_WARNING, "coregistration", "burst_alignment.fallback",
		"Burst alignment could not be estimated; all burst offsets were set to zero.",
		"Geometric quality may be reduced. Inspect burst overlap and orbit diagnostics.");
	return 0;
}

int Sentinel1BackGeocoding::performDerampDemod(Mat& derampDemodPhase, ComplexMat& slc)
{
	if (derampDemodPhase.size != slc.re.size || derampDemodPhase.size != slc.im.size || derampDemodPhase.empty())
	{
		fprintf(stderr, "performDerampDemod(): input check failed!\n");
		return -1;
	}
	ComplexMat tmp;
	FormatConversion conversion;
	if (derampDemodPhase.type() != CV_64F) derampDemodPhase.convertTo(derampDemodPhase, CV_64F);
	conversion.phase2cos(derampDemodPhase, tmp.re, tmp.im);
	if (slc.type() != CV_64F) slc.convertTo(slc, CV_64F);
	slc.Mul(tmp, slc, false);

	return 0;
}

int Sentinel1BackGeocoding::computeSlavePosition(int slaveImagesIndex, int mBurstIndex)
{
	if (slaveImagesIndex < 1 || slaveImagesIndex > numOfImages)
	{
		fprintf(stderr, "computeSlavePosition(): input check failed!\n");
		return -1;
	}
	int sBurstIndex = mBurstIndex + su[slaveImagesIndex - 1]->burstOffset;
	if (sBurstIndex < 1 || sBurstIndex > su[slaveImagesIndex - 1]->burstCount) {
		return -1;
	}
	// removed unused: ret (no API calls needing error checks in this function)
	// removed unused: lonMin, lonMax, latMin, latMax (DEM bounds used via dem-> members)
	FormatConversion conversion;
	if (!isMasterRgAzComputed)
	{
		masterAzimuth.create(dem->rows, dem->cols, CV_64F);
		slaveAzimuth.create(dem->rows, dem->cols, CV_64F);
		masterRange.create(dem->rows, dem->cols, CV_64F);
		slaveRange.create(dem->rows, dem->cols, CV_64F);
	}
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		g_sentinel_diagnostic_state[this].hasZeroDopplerDiagnostic = false;
	}
	if (!isMasterRgAzComputed)
		clear_zero_doppler_failure_statistics(this, masterIndex, mBurstIndex);
	clear_zero_doppler_failure_statistics(this, slaveImagesIndex, sBurstIndex);
	int zeroDopplerFailures = 0;
	int rangeOrBurstFailures = 0;
	int rangeOutOfBoundsFailures = 0;
	int burstOutOfBoundsFailures = 0;
	int slantRangeFailures = 0;
	int invalidInputFailures = 0;
	int masterZeroDopplerFailures = 0;
	int masterRangeOrBurstFailures = 0;
	ZeroDopplerFailureAccumulator masterFailures;
	ZeroDopplerFailureAccumulator slaveFailures;
	const ActiveDiagnosticContext progressContext = g_active_diagnostic_context;
	const int totalBurstCount = su[masterIndex - 1]->burstCount;
	const int progressStart = 20 + 50 * (mBurstIndex - 1) / totalBurstCount;
	const int progressEnd = 20 + 50 * mBurstIndex / totalBurstCount;
	const int progressReportInterval = std::max(1, dem->rows / 25);
	int completedProjectionRows = 0;
	int lastProjectionProgress = progressStart;
#pragma omp parallel
	{
		ZeroDopplerFailureAccumulator localMasterFailures;
		ZeroDopplerFailureAccumulator localSlaveFailures;
		int localZeroDopplerFailures = 0;
		int localRangeOrBurstFailures = 0;
		int localMasterZeroDopplerFailures = 0;
		int localMasterRangeOrBurstFailures = 0;
		int localRangeOutOfBoundsFailures = 0;
		int localBurstOutOfBoundsFailures = 0;
		int localSlantRangeFailures = 0;
		int localInvalidInputFailures = 0;
#pragma omp for schedule(guided)
	for (int i = 0; i < dem->rows; i++)
	{
		if (isCancelRequested()) continue;
		double lon, lat, elevation, rangeIndex, azimuthIndex;
		Position earthPoint;
		for (int j = 0; j < dem->cols; j++)
		{
			lat = dem->latUpperLeft - i * dem->latSpacing;
			lon = dem->lonUpperLeft + j * dem->lonSpacing;
			lon = lon > 180.0 ? lon - 360.0 : lon;
			elevation = dem->rawDEM.at<short>(i, j);
			conversion.ell2xyz(lon, lat, elevation, earthPoint);
			if (!isMasterRgAzComputed)
			{
				if (su[masterIndex - 1]->getRgAzPosition(mBurstIndex, earthPoint, &rangeIndex, &azimuthIndex) == 0)
				{
					masterAzimuth.at<double>(i, j) = azimuthIndex;
					masterRange.at<double>(i, j) = rangeIndex;
				}
				else
				{
					masterAzimuth.at<double>(i, j) = invalidRgAzIndex;
					masterRange.at<double>(i, j) = invalidRgAzIndex;
					if (collect_zero_doppler_failure(localMasterFailures, masterIndex, mBurstIndex, i, j,
						SENTINEL_ZERO_DOPPLER_CALL_MASTER_RG_AZ, earthPoint))
						localMasterZeroDopplerFailures++;
					else
						localMasterRangeOrBurstFailures++;
				}
			}
			if (su[slaveImagesIndex - 1]->getRgAzPosition(sBurstIndex, earthPoint, &rangeIndex, &azimuthIndex) == 0)
			{
				slaveAzimuth.at<double>(i, j) = azimuthIndex;
				slaveRange.at<double>(i, j) = rangeIndex;
			}
			else
			{
				 slaveAzimuth.at<double>(i, j) = invalidRgAzIndex;
				 slaveRange.at<double>(i, j) = invalidRgAzIndex;
				if (collect_zero_doppler_failure(localSlaveFailures, slaveImagesIndex, sBurstIndex, i, j,
					SENTINEL_ZERO_DOPPLER_CALL_SLAVE_RG_AZ, earthPoint))
					localZeroDopplerFailures++;
				else
				{
					localRangeOrBurstFailures++;
					switch (g_rgaz_projection_failure_reason)
					{
					case RGAZ_PROJECTION_RANGE_OUT_OF_BOUNDS: localRangeOutOfBoundsFailures++; break;
					case RGAZ_PROJECTION_BURST_OUT_OF_BOUNDS: localBurstOutOfBoundsFailures++; break;
					case RGAZ_PROJECTION_SLANT_RANGE: localSlantRangeFailures++; break;
					case RGAZ_PROJECTION_INVALID_INPUT: localInvalidInputFailures++; break;
					default: break;
					}
				}
			}
		}
		int completedRows = 0;
#pragma omp critical(sentinel_projection_row_count)
		{
			completedRows = ++completedProjectionRows;
		}
		if (completedRows % progressReportInterval == 0 || completedRows == dem->rows)
		{
#pragma omp critical(sentinel_projection_progress)
			{
				const int progress = progressStart + (progressEnd - progressStart) * completedRows / dem->rows;
				if (progress > lastProjectionProgress)
				{
					lastProjectionProgress = progress;
					char progressMessage[128] = {};
					sprintf_s(progressMessage, "Projecting burst %d: %d%% of DEM rows processed.", mBurstIndex, completedRows * 100 / dem->rows);
					emit_progress_for_context(progressContext, progress, progressMessage);
				}
			}
		}
	}
	// ͳ����Ч/��ЧͶӰ������������Ч�������屣�ֲ��䡣
#pragma omp critical(sentinel_zero_doppler_accumulator)
	{
		masterFailures.merge(localMasterFailures);
		slaveFailures.merge(localSlaveFailures);
		masterZeroDopplerFailures += localMasterZeroDopplerFailures;
		masterRangeOrBurstFailures += localMasterRangeOrBurstFailures;
		zeroDopplerFailures += localZeroDopplerFailures;
		rangeOrBurstFailures += localRangeOrBurstFailures;
		rangeOutOfBoundsFailures += localRangeOutOfBoundsFailures;
		burstOutOfBoundsFailures += localBurstOutOfBoundsFailures;
		slantRangeFailures += localSlantRangeFailures;
		invalidInputFailures += localInvalidInputFailures;
	}
	}
	if (isCancelRequested()) return -2;
	for (int reason = SENTINEL_ZERO_DOPPLER_INVALID_INPUT;
		reason <= SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT; ++reason)
	{
		if (masterFailures.counts[reason] > 0)
			record_zero_doppler_failure(this, masterFailures.diagnostics[reason], masterFailures.counts[reason]);
		if (slaveFailures.counts[reason] > 0)
			record_zero_doppler_failure(this, slaveFailures.diagnostics[reason], slaveFailures.counts[reason]);
	}
	int masterValid = 0, slaveValid = 0, total = dem->rows * dem->cols;
	int validRowMin = dem->rows, validRowMax = -1, validColMin = dem->cols, validColMax = -1;
	for (int i = 0; i < dem->rows; i++) {
		for (int j = 0; j < dem->cols; j++) {
			if (masterAzimuth.at<double>(i, j) > -0.5 && masterRange.at<double>(i, j) > -0.5) masterValid++;
			if (slaveAzimuth.at<double>(i, j) > -0.5 && slaveRange.at<double>(i, j) > -0.5)
			{
				slaveValid++;
				validRowMin = std::min(validRowMin, i);
				validRowMax = std::max(validRowMax, i);
				validColMin = std::min(validColMin, j);
				validColMax = std::max(validColMax, j);
			}
		}
	}
	SentinelBurstQualityStatus status;
	status.imageIndex = slaveImagesIndex;
	status.burstIndex = mBurstIndex;
	status.attemptedPoints = total;
	status.validPoints = slaveValid;
	status.invalidPoints = total - slaveValid;
	status.zeroDopplerFailures = zeroDopplerFailures;
	status.rangeOrBurstFailures = rangeOrBurstFailures;
	status.slantRangeFailures = slantRangeFailures;
	status.invalidInputFailures = invalidInputFailures;
	const bool hasGeometricFailure = status.zeroDopplerFailures > 0 ||
		status.slantRangeFailures > 0 || status.invalidInputFailures > 0;
	status.qualityCode = hasGeometricFailure ? SENTINEL_BURST_WARNING_PARTIAL_INVALID : SENTINEL_BURST_NOMINAL;
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		if (g_sentinel_diagnostic_state[this].zeroOffsetFallback)
			status.qualityCode = SENTINEL_BURST_WARNING_ZERO_OFFSET_FALLBACK;
	}
	record_burst_quality(this, status);
	SentinelZeroDopplerDiagnostic diagnostic;
	bool hasZeroDopplerDiagnostic = false;
	{
		std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
		SentinelBackGeocodingDiagnosticState& diagnosticState = g_sentinel_diagnostic_state[this];
		if (diagnosticState.hasZeroDopplerDiagnostic)
		{
			diagnostic = diagnosticState.lastZeroDopplerDiagnostic;
			hasZeroDopplerDiagnostic = true;
		}
	}
	if (hasZeroDopplerDiagnostic)
	{
		char message[1024] = {};
		sprintf_s(message, "Zero-Doppler failure: scene=%s, swath=%s, polarization=%s, line=%d, sample=%d, reason=%d, path=%d, return=%d, ground=(%.3f,%.3f,%.3f), targetDoppler=%.6f, orbitRange=[%.6f,%.6f], nearestOrbitTime=%.6f, stateVectors=%d.",
			diagnostic.scene, diagnostic.swath, diagnostic.polarization,
			diagnostic.line, diagnostic.sample, diagnostic.reason, diagnostic.callPath, diagnostic.returnCode,
			diagnostic.groundPosition.x, diagnostic.groundPosition.y, diagnostic.groundPosition.z,
			diagnostic.targetDoppler, diagnostic.orbitStartTime, diagnostic.orbitStopTime,
			diagnostic.nearestOrbitTime, diagnostic.stateVectorCount);
		emit_diagnostic(INSAR_DIAGNOSTIC_WARNING, "geometry", "zero_doppler.summary", message,
			"The first representative failure for this reason is reported; aggregate counts are included in the projection summary.",
			diagnostic.scene, nullptr, diagnostic.returnCode);
	}
	double masterRatio = total > 0 ? 100.0 * masterValid / total : 0.0;
	double slaveRatio = total > 0 ? 100.0 * slaveValid / total : 0.0;
	char validExtent[384] = {};
	if (validRowMax >= validRowMin && validColMax >= validColMin)
	{
		const double west = dem->lonUpperLeft + validColMin * dem->lonSpacing;
		const double east = dem->lonUpperLeft + validColMax * dem->lonSpacing;
		const double north = dem->latUpperLeft - validRowMin * dem->latSpacing;
		const double south = dem->latUpperLeft - validRowMax * dem->latSpacing;
		sprintf_s(validExtent, "slaveValidExtentRows=[%d,%d], cols=[%d,%d], lon=[%.8f,%.8f], lat=[%.8f,%.8f]",
			validRowMin, validRowMax, validColMin, validColMax, west, east, south, north);
	}
	else
	{
		strcpy_s(validExtent, "slaveValidExtent=empty");
	}
	char projectionMessage[1024] = {};
	sprintf_s(projectionMessage, "Projection summary: DEM=%dx%d, masterValid=%d/%d (%.1f%%), slaveValid=%d/%d (%.1f%%), masterZeroDopplerFailures=%d, masterRangeOrBurstFailures=%d, slaveZeroDopplerFailures=%d, slaveRangeOrBurstFailures=%d, slaveRangeOutOfBounds=%d, slaveBurstOutOfBounds=%d, slaveSlantRangeFailures=%d, slaveInvalidInputFailures=%d.",
		dem->rows, dem->cols, masterValid, total, masterRatio, slaveValid, total, slaveRatio,
		masterZeroDopplerFailures, masterRangeOrBurstFailures, zeroDopplerFailures, rangeOrBurstFailures,
		rangeOutOfBoundsFailures, burstOutOfBoundsFailures, slantRangeFailures, invalidInputFailures);
	emit_diagnostic(status.qualityCode == SENTINEL_BURST_WARNING_PARTIAL_INVALID ? INSAR_DIAGNOSTIC_WARNING : INSAR_DIAGNOSTIC_DEBUG,
		"geometry", "projection.summary", projectionMessage,
		validExtent,
		su[slaveImagesIndex - 1]->h5File.c_str(), nullptr, status.qualityCode,
		dem->rows, dem->cols);

	if (!isMasterRgAzComputed)isMasterRgAzComputed = true;
	return 0;
}

int Sentinel1BackGeocoding::computeSlaveOffset(Mat& slaveAzimuthOffset, Mat& slaveRangeOffset)
{
	if (!isMasterRgAzComputed)
	{
		fprintf(stderr, "computeSlaveOffset(): input check failed!\n");
		return -1;
	}
	slaveAzimuthOffset.create(masterRange.size(), CV_64F);
	slaveRangeOffset.create(masterRange.size(), CV_64F);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < slaveAzimuthOffset.rows; i++)
	{
		if (isCancelRequested()) continue;
		for (int j = 0; j < slaveAzimuthOffset.cols; j++)
		{
			//��λ��ƫ��������
			if (masterAzimuth.at<double>(i, j) < -0.5 || slaveAzimuth.at<double>(i, j) < -0.5)
			{
				slaveAzimuthOffset.at<double>(i, j) = invalidOffset;
			}
			else
			{
				slaveAzimuthOffset.at<double>(i, j) = slaveAzimuth.at<double>(i, j) - masterAzimuth.at<double>(i, j);
			}
			//������ƫ��������
			if (masterRange.at<double>(i, j) < -0.5 || slaveRange.at<double>(i, j) < -0.5)
			{
				slaveRangeOffset.at<double>(i, j) = invalidOffset;
			}
			else
			{
				slaveRangeOffset.at<double>(i, j) = slaveRange.at<double>(i, j) - masterRange.at<double>(i, j);
			}
		}
	}
	if (isCancelRequested()) return -2;
	return 0;
}

int Sentinel1BackGeocoding::fitSlaveOffset(
	Mat& slaveOffset,
	double* a0,
	double* a1,
	double* a2
)
{
	if (isCancelRequested()) return -2;
	if (slaveOffset.empty() || slaveOffset.type() != CV_64F)
	{
		fprintf(stderr, "fitSlaveOffset(): input check failed!\n");
		return -1;
	}
	int count = 0, nr, nc;
	nr = slaveOffset.rows;
	nc = slaveOffset.cols;
	const int candidatePoints = nr * nc;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (fabs(slaveOffset.at<double>(i, j) - invalidOffset) > 0.0001) count++;
		}
	}
	if (count < 4)
	{
		fprintf(stderr, "fitSlaveOffset(): not enough valid offsetpoints!\n");
		return -1;
	}
	Mat offset(count, 1, CV_64F);
	Mat range(count, 1, CV_64F);
	Mat azimuth(count, 1, CV_64F);
	Mat A = Mat::ones(count, 3, CV_64F);
	count = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (fabs(slaveOffset.at<double>(i, j) - invalidOffset) > 0.0001)
			{
				offset.at<double>(count, 0) = slaveOffset.at<double>(i, j);
				range.at<double>(count, 0) = masterRange.at<double>(i, j);
				azimuth.at<double>(count++, 0) = masterAzimuth.at<double>(i, j);
			}
		}
	}
	range.copyTo(A(cv::Range(0, count), cv::Range(1, 2)));
	azimuth.copyTo(A(cv::Range(0, count), cv::Range(2, 3)));
	Mat A_original = A.clone(); // ����ԭʼ A �Լ���в�
	Mat A_t, b, coef;
	cv::transpose(A, A_t);
	A = A_t * A;
	b = A_t * offset;
	if (!cv::solve(A, b, coef, cv::DECOMP_NORMAL))
	{
		fprintf(stderr, "fitSlaveOffset(): matrix deficiency!\n");
		return -1;
	}
	if (a0) *a0 = coef.at<double>(0, 0);
	if (a1) *a1 = coef.at<double>(1, 0);
	if (a2) *a2 = coef.at<double>(2, 0);

	// ���� RMS �в� ����ӡ��־ (ʹ�� coef.at ��ֹ��ָ�����)
	Mat residual = offset - A_original * coef;
	double rms = cv::norm(residual) / sqrt(count);
	char fitMessage[512] = {};
	sprintf_s(fitMessage, "Offset fit accepted: candidates=%d, fitted=%d, rejected=%d, a0=%.8f, a1=%.10f, a2=%.10f, rms=%.8f.",
		candidatePoints, count, candidatePoints - count, coef.at<double>(0, 0), coef.at<double>(1, 0), coef.at<double>(2, 0), rms);
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "geometry", "offset_fit", fitMessage,
		"Model: offset = a0 + a1 * range + a2 * azimuth; offsets are in image pixels. Acceptance means the normal-equation solver succeeded.");

	return 0;
}

int Sentinel1BackGeocoding::performBilinearResampling(
	ComplexMat& slave,
	int dstHeight,
	int dstWidth,
	double a0Rg, double a1Rg, double a2Rg,
	double a0Az, double a1Az, double a2Az
)
{
	if (isCancelRequested()) return -2;
	if (slave.isEmpty() || dstHeight < 2 || dstWidth < 2)
	{
		fprintf(stderr, "performBilinearResampling(): input check failed!\n");
		return -1;
	}
	ComplexMat slcResampled;
	if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
	slcResampled.re.create(dstHeight, dstWidth, CV_64F);
	slcResampled.im.create(dstHeight, dstWidth, CV_64F);
	Mat coef_r(3, 1, CV_64F), coef_c(3, 1, CV_64F);
	coef_r.at<double>(0, 0) = a0Az;
	coef_r.at<double>(1, 0) = a1Az;
	coef_r.at<double>(2, 0) = a2Az;
	coef_c.at<double>(0, 0) = a0Rg;
	coef_c.at<double>(1, 0) = a1Rg;
	coef_c.at<double>(2, 0) = a2Rg;
	int rows = dstHeight; int cols = dstWidth;
	int cols_slave = slave.GetCols(); int rows_slave = slave.GetRows();
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (isCancelRequested()) continue;
		double ii, jj; Mat tmp(1, 3, CV_64F); Mat result;
		// removed unused: x, y (pixel coordinates computed as ii, jj instead)
		int mm, nn, mm1, nn1;
		double offset_rows, offset_cols, upper, lower;
		for (int j = 0; j < cols; j++)
		{
			jj = (double)j;
			ii = (double)i;
			tmp.at<double>(0, 0) = 1.0;
			tmp.at<double>(0, 1) = jj;
			tmp.at<double>(0, 2) = ii;

			result = tmp * coef_r;
			offset_rows = result.at<double>(0, 0);
			result = tmp * coef_c;
			offset_cols = result.at<double>(0, 0);

			ii += offset_rows /*+ 0.0053*/;
			jj += offset_cols;

			mm = (int)floor(ii); nn = (int)floor(jj);
			if (mm < 0 || nn < 0 || mm > rows_slave - 1 || nn > cols_slave - 1)
			{
				slcResampled.re.at<double>(i, j) = 0.0;
				slcResampled.im.at<double>(i, j) = 0.0;
			}
			else
			{
				mm1 = mm + 1; nn1 = nn + 1;
				mm1 = mm1 >= rows_slave - 1 ? rows_slave - 1 : mm1;
				nn1 = nn1 >= cols_slave - 1 ? cols_slave - 1 : nn1;
				//ʵ����ֵ
				upper = slave.re.at<double>(mm, nn) + (slave.re.at<double>(mm, nn1) - slave.re.at<double>(mm, nn)) * (jj - (double)nn);
				lower = slave.re.at<double>(mm1, nn) + (slave.re.at<double>(mm1, nn1) - slave.re.at<double>(mm1, nn)) * (jj - (double)nn);
				slcResampled.re.at<double>(i, j) = upper + (lower - upper) * (ii - (double)mm);
				//�鲿��ֵ
				upper = slave.im.at<double>(mm, nn) + (slave.im.at<double>(mm, nn1) - slave.im.at<double>(mm, nn)) * (jj - (double)nn);
				lower = slave.im.at<double>(mm1, nn) + (slave.im.at<double>(mm1, nn1) - slave.im.at<double>(mm1, nn)) * (jj - (double)nn);
				slcResampled.im.at<double>(i, j) = upper + (lower - upper) * (ii - (double)mm);
			}

		}
	}
	if (isCancelRequested()) return -2;
	slave = slcResampled;
	return 0;
}

int Sentinel1BackGeocoding::performSincResampling(
	ComplexMat& slave,
	int dstHeight,
	int dstWidth,
	double a0Rg, double a1Rg, double a2Rg,
	double a0Az, double a1Az, double a2Az
)
{
	if (isCancelRequested()) return -2;
	if (slave.isEmpty() || dstHeight < 2 || dstWidth < 2)
	{
		fprintf(stderr, "performSincResampling(): input check failed!\n");
		return -1;
	}

	ComplexMat slcResampled;

	// ͳһת�� double������ sinc ��ֵ
	if (slave.type() != CV_64F)
	{
		slave.convertTo(slave, CV_64F);
	}

	slcResampled.re.create(dstHeight, dstWidth, CV_64F);
	slcResampled.im.create(dstHeight, dstWidth, CV_64F);

	int rows = dstHeight;
	int cols = dstWidth;

	const int SINC_RADIUS = 4;
	long long zeroFilledSampleCount = 0;
	char resamplingMessage[512] = {};
	sprintf_s(resamplingMessage, "Sinc resampling: output=%dx%d, kernelRadius=%d, range={%.8f, %.10f, %.10f}, azimuth={%.8f, %.10f, %.10f}.",
		dstWidth, dstHeight, SINC_RADIUS, a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az);
	emit_diagnostic(INSAR_DIAGNOSTIC_TRACE, "resampling", "sinc.start", resamplingMessage,
		"Pull sampling uses srcRow=dstRow+azimuthOffset and srcCol=dstCol+rangeOffset; any source coordinate outside the radius-4 safe support is zero-filled.",
		nullptr, nullptr, 0, dstHeight, dstWidth);
	const ULONGLONG resamplingStartTick = GetTickCount64();

	// ��ǰȡ��ϵ��������ÿ����Ԫ���� Mat ��������˷�
	const double cr0 = a0Az;
	const double cr1 = a1Az;
	const double cr2 = a2Az;

	const double cc0 = a0Rg;
	const double cc1 = a1Rg;
	const double cc2 = a2Rg;

#pragma omp parallel for schedule(guided) reduction(+:zeroFilledSampleCount)
	for (int i = 0; i < rows; i++)
	{
		if (isCancelRequested()) continue;
		for (int j = 0; j < cols; j++)
		{
			double ii = static_cast<double>(i);
			double jj = static_cast<double>(j);

			// �з���ƫ�ƣ�azimuth offset
			double offset_rows = cr0 + cr1 * jj + cr2 * ii;

			// �з���ƫ�ƣ�range offset
			double offset_cols = cc0 + cc1 * jj + cc2 * ii;

			double src_row = ii + offset_rows;  // /* + 0.0053 */ ����㻹��Ҫ��������������ɼ�������
			double src_col = jj + offset_cols;
			if (src_row < SINC_RADIUS || src_col < SINC_RADIUS || src_row > rows - 1 - SINC_RADIUS || src_col > cols - 1 - SINC_RADIUS)
			{
				++zeroFilledSampleCount;
			}

			double re_value = sinc_interp2d(slave.re, src_row, src_col, SINC_RADIUS);
			double im_value = sinc_interp2d(slave.im, src_row, src_col, SINC_RADIUS);

			slcResampled.re.at<double>(i, j) = re_value;
			slcResampled.im.at<double>(i, j) = im_value;
		}
	}
	if (isCancelRequested()) return -2;

	slave = slcResampled;
	char completionMessage[256] = {};
	sprintf_s(completionMessage, "Sinc resampling completed: zeroFilledOutputSamples=%lld of %lld.", zeroFilledSampleCount, static_cast<long long>(rows) * cols);
	emit_diagnostic(INSAR_DIAGNOSTIC_TRACE, "resampling", "sinc.complete", completionMessage,
		nullptr, nullptr, nullptr, 0, dstHeight, dstWidth, -1,
		static_cast<long long>(GetTickCount64() - resamplingStartTick));

	return 0;
}

int Sentinel1BackGeocoding::slaveSincInterpolation(
	int mBurstIndex,
	int slaveImageIndex,
	ComplexMat& slave
)
{
	ScopedDiagnosticContext diagnosticScope(diagnosticCallback, diagnosticUserData, slaveImageIndex, mBurstIndex);
	if (slaveImageIndex < 1 ||
		slaveImageIndex > numOfImages ||
		mBurstIndex < 1 ||
		mBurstIndex > su[masterIndex - 1]->burstCount)
	{
		fprintf(stderr, "slaveSincInterpolation(): input check failed!\n");
		return -1;
	}
	int ret;
	int sBurstIndex = mBurstIndex + su[slaveImageIndex - 1]->burstOffset;
	if (sBurstIndex < 1 || sBurstIndex > su[slaveImageIndex - 1]->burstCount) {
		return -1;
	}
	ComplexMat tmp;
	double a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az;
	char stageMessage[384] = {};
	sprintf_s(stageMessage, "Starting slave burst processing: masterBurst=%d, slaveBurst=%d, burstOffset=%d, sourceDimensions=%dx%d, ompThreads=%d.",
		mBurstIndex, sBurstIndex, su[slaveImageIndex - 1]->burstOffset, su[slaveImageIndex - 1]->linesPerBurst,
		su[slaveImageIndex - 1]->samplesPerBurst, omp_get_max_threads());
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "coregistration", "slave_burst.start", stageMessage);
	ret = su[slaveImageIndex - 1]->getBurst(sBurstIndex, slave);
	if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
	if (return_check(ret, "getBurst()", error_head)) return -1;
	Mat derampDemodPhase;
	ret = su[slaveImageIndex - 1]->computeDerampDemodPhase(sBurstIndex, derampDemodPhase);
	if (return_check(ret, "computeDerampDemodPhase()", error_head)) return -1;
	ret = performDerampDemod(derampDemodPhase, slave);
	if (return_check(ret, "performDerampDemod()", error_head)) return -1;
	const ULONGLONG geometryStartTick = GetTickCount64();
	ret = computeSlavePosition(slaveImageIndex, mBurstIndex);
	if (ret == -2) return -2;
	if (return_check(ret, "computeSlavePosition()", error_head)) return -1;
	Mat slaveAzimuthOffset, slaveRangeOffset;
	ret = computeSlaveOffset(slaveAzimuthOffset, slaveRangeOffset);
	if (ret == -2) return -2;
	if (return_check(ret, "computeSlaveOffset()", error_head)) return -1;
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "geometry", "projection_and_offset.complete", "Geometric projection and offset-grid generation completed.",
		nullptr, su[slaveImageIndex - 1]->h5File.c_str(), nullptr, 0, slaveRangeOffset.rows, slaveRangeOffset.cols,
		slaveRangeOffset.type(), static_cast<long long>(GetTickCount64() - geometryStartTick));
	FormatConversion conversion;
	const ULONGLONG fitStartTick = GetTickCount64();
	ret = fitSlaveOffset(slaveAzimuthOffset, &a0Az, &a1Az, &a2Az);
	if (ret == -2) return -2;
	if (return_check(ret, "fitSlaveOffset()", error_head)) return -1;
	ret = fitSlaveOffset(slaveRangeOffset, &a0Rg, &a1Rg, &a2Rg);
	if (ret == -2) return -2;
	if (return_check(ret, "fitSlaveOffset()", error_head)) return -1;
	sprintf_s(stageMessage, "Offset fitting completed: range={%.8f, %.10f, %.10f}, azimuth={%.8f, %.10f, %.10f}.",
		a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az);
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "geometry", "offset_fit.complete", stageMessage,
		"Range and azimuth offsets use the model a0 + a1 * range + a2 * azimuth, in pixels.",
		nullptr, nullptr, 0, slaveRangeOffset.rows, slaveRangeOffset.cols, slaveRangeOffset.type(),
		static_cast<long long>(GetTickCount64() - fitStartTick));
	const ULONGLONG resamplingStartTick = GetTickCount64();
	ret = performSincResampling(slave, su[masterIndex - 1]->linesPerBurst, su[masterIndex - 1]->samplesPerBurst,
		a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az);
	if (ret == -2) return -2;
	if (return_check(ret, "performSincResampling()", error_head)) return -1;
	tmp.SetRe(derampDemodPhase); tmp.SetIm(derampDemodPhase);
	ret = performSincResampling(tmp, su[masterIndex - 1]->linesPerBurst, su[masterIndex - 1]->samplesPerBurst,
		a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az);
	if (ret == -2) return -2;
	if (return_check(ret, "performSincResampling()", error_head)) return -1;
	tmp.re.copyTo(derampDemodPhase);
	conversion.phase2cos(derampDemodPhase, tmp.re, tmp.im);
	slave.Mul(tmp, slave, true);//reramp
	slave.convertTo(slave, CV_32F);
	if (slaveImageIndex - 1 >= static_cast<int>(fullBurstFiles.size())) return -1;

	cv::Mat coefficients(1, 6, CV_64F);
	coefficients.at<double>(0) = a0Rg;
	coefficients.at<double>(1) = a1Rg;
	coefficients.at<double>(2) = a2Rg;
	coefficients.at<double>(3) = a0Az;
	coefficients.at<double>(4) = a1Az;
	coefficients.at<double>(5) = a2Az;
	const std::string coefficientName = "burst_" + std::to_string(mBurstIndex) + "_coef";
	if (Hdf5IO::writeArray(fullBurstFiles[slaveImageIndex - 1].c_str(), coefficientName.c_str(), coefficients) != 0)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "geometry", "offset_fit.persist", "Failed to persist geometric registration coefficients.",
			"Post-registration refinement requires this exact 1x6 CV_64F coefficient vector.", fullBurstFiles[slaveImageIndex - 1].c_str(), coefficientName.c_str(), -1201);
		return -1;
	}
	char coefficientMessage[384] = {};
	sprintf_s(coefficientMessage, "Persisted geometric coefficients: range={%.12g, %.12g, %.12g}, azimuth={%.12g, %.12g, %.12g}.",
		a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az);
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "geometry", "offset_fit.persist", coefficientMessage,
		"Stored as a 1x6 CV_64F vector in the old Worker coefficient order.", fullBurstFiles[slaveImageIndex - 1].c_str(), coefficientName.c_str(), 0, 1, 6, CV_64F);
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "resampling", "sinc.complete", "Complex data and reramp phase sinc resampling completed.",
		nullptr, nullptr, nullptr, 0, slave.GetRows(), slave.GetCols(), slave.type(),
		static_cast<long long>(GetTickCount64() - resamplingStartTick));
	return 0;
}

int Sentinel1BackGeocoding::getLastZeroDopplerDiagnostic(SentinelZeroDopplerDiagnostic& diagnostic) const
{
	std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
	std::map<const Sentinel1BackGeocoding*, SentinelBackGeocodingDiagnosticState>::const_iterator it =
		g_sentinel_diagnostic_state.find(this);
	if (it == g_sentinel_diagnostic_state.end() || !it->second.hasZeroDopplerDiagnostic) return -1;
	diagnostic = it->second.lastZeroDopplerDiagnostic;
	return 0;
}

int Sentinel1BackGeocoding::getZeroDopplerFailureStatistics(
	vector<SentinelZeroDopplerFailureStatistic>& statistics) const
{
	std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
	std::map<const Sentinel1BackGeocoding*, SentinelBackGeocodingDiagnosticState>::const_iterator it =
		g_sentinel_diagnostic_state.find(this);
	if (it == g_sentinel_diagnostic_state.end()) return -1;
	statistics = it->second.zeroDopplerFailureStatistics;
	return 0;
}

int Sentinel1BackGeocoding::getBurstQualityStatus(vector<SentinelBurstQualityStatus>& status) const
{
	std::lock_guard<std::mutex> lock(g_sentinel_diagnostic_mutex);
	std::map<const Sentinel1BackGeocoding*, SentinelBackGeocodingDiagnosticState>::const_iterator it =
		g_sentinel_diagnostic_state.find(this);
	if (it == g_sentinel_diagnostic_state.end()) return -1;
	status = it->second.burstStatus;
	return 0;
}

int Sentinel1BackGeocoding::deBurstConfig()
{
	const ULONGLONG configurationStartTick = GetTickCount64();
	if (isdeBurstConfig) return 0;
	if (masterIndex < 1 || masterIndex > static_cast<int>(su.size()) || !su[masterIndex - 1] ||
		su[masterIndex - 1]->burstCount <= 0 || su[masterIndex - 1]->linesPerBurst <= 0 ||
		!std::isfinite(su[masterIndex - 1]->azimuthTimeInterval) || su[masterIndex - 1]->azimuthTimeInterval <= 0.0 ||
		su[masterIndex - 1]->burstAzimuthTime.rows < su[masterIndex - 1]->burstCount ||
		su[masterIndex - 1]->firstValidLine.rows < su[masterIndex - 1]->burstCount ||
		su[masterIndex - 1]->lastValidLine.rows < su[masterIndex - 1]->burstCount)
	{
		fprintf(stderr, "deBurstConfig(): input check failed!\n");
		return -1;
	}
	Mat start(su[masterIndex - 1]->burstCount, 1, CV_32S), end(su[masterIndex - 1]->burstCount, 1, CV_32S);
	start.at<int>(0, 0) = 1;
	end.at<int>(0, 0) = su[masterIndex - 1]->lastValidLine.at<int>(0, 0);
	double lastValidTime = su[masterIndex - 1]->burstAzimuthTime.at<double>(0, 0) +
		(su[masterIndex - 1]->lastValidLine.at<int>(0, 0) - 1) * su[masterIndex - 1]->azimuthTimeInterval;
	double firstValidTime;
	int deburstLines = 0;
	int overlap;
	for (int i = 1; i < su[masterIndex - 1]->burstCount; i++)
	{
		firstValidTime = su[masterIndex - 1]->burstAzimuthTime.at<double>(i, 0) + (su[masterIndex - 1]->firstValidLine.at<int>(i, 0) - 1) *
			su[masterIndex - 1]->azimuthTimeInterval;

		overlap = static_cast<int>(round((lastValidTime - firstValidTime) / su[masterIndex - 1]->azimuthTimeInterval + 1));

		end.at<int>(i - 1, 0) = end.at<int>(i - 1, 0) - int(overlap / 2);

		start.at<int>(i, 0) = su[masterIndex - 1]->linesPerBurst * i + su[masterIndex - 1]->firstValidLine.at<int>(i, 0) + overlap - int(overlap / 2);

		end.at<int>(i, 0) = su[masterIndex - 1]->linesPerBurst * i + su[masterIndex - 1]->lastValidLine.at<int>(i, 0);

		lastValidTime = su[masterIndex - 1]->burstAzimuthTime.at<double>(i, 0) +
			(su[masterIndex - 1]->lastValidLine.at<int>(i, 0) - 1) * su[masterIndex - 1]->azimuthTimeInterval;
	}
	end.at<int>(su[masterIndex - 1]->burstCount - 1, 0) = su[masterIndex - 1]->linesPerBurst * su[masterIndex - 1]->burstCount;
	start -= 1;
	//end -= 1;
	start.copyTo(this->start);
	end.copyTo(this->end);
	//for (int i = 0; i < su[masterIndex - 1]->burstCount; i++)
	//{
	//	deburstLines += end.at<int>(i, 0) - start.at<int>(i, 0);
	//}
	//this->deburstLines = deburstLines;
	isdeBurstConfig = true;
	int retainedLines = 0;
	for (int i = 0; i < start.rows; ++i)
		retainedLines += end.at<int>(i, 0) - start.at<int>(i, 0);
	for (int burst = 0; burst < start.rows; ++burst)
	{
		char spanMessage[384] = {};
		sprintf_s(spanMessage, "Deburst span: masterBurst=%d, start=%d, endExclusive=%d, retainedRows=%d, firstValidLine=%d, lastValidLine=%d.",
			burst + 1, start.at<int>(burst, 0), end.at<int>(burst, 0), end.at<int>(burst, 0) - start.at<int>(burst, 0),
			su[masterIndex - 1]->firstValidLine.at<int>(burst, 0), su[masterIndex - 1]->lastValidLine.at<int>(burst, 0));
		emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "deburst", "configuration.span", spanMessage,
			"start/end are 0-based full-burst rows with end exclusive; firstValidLine/lastValidLine retain Sentinel metadata's 1-based convention.",
			nullptr, nullptr, 0, end.at<int>(burst, 0) - start.at<int>(burst, 0), su[masterIndex - 1]->samplesPerBurst, CV_32S);
	}

	char message[384] = {};
	sprintf_s(message, "Deburst configuration: bursts=%d, sourceLinesPerBurst=%d, retainedLines=%d, samples=%d.",
		su[masterIndex - 1]->burstCount, su[masterIndex - 1]->linesPerBurst, retainedLines,
		su[masterIndex - 1]->samplesPerBurst);
	emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "deburst", "configuration.complete", message,
		"Overlap trimming is derived from burst azimuth timing and valid-line metadata.",
		nullptr, nullptr, 0, retainedLines, su[masterIndex - 1]->samplesPerBurst, CV_32S,
		static_cast<long long>(GetTickCount64() - configurationStartTick));
	return 0;
}

int Sentinel1BackGeocoding::backGeoCodingCoregistration(
	InSARDiagnosticCallback callback,
	void* userData)
{
	if (callback || userData)
		setDiagnosticCallback(callback, userData);
	ScopedDiagnosticContext diagnosticScope(diagnosticCallback, diagnosticUserData);
	const ULONGLONG taskStartTick = GetTickCount64();
	char processMessage[512] = {};
	sprintf_s(processMessage, "Starting Sentinel-1 back-geocoding: images=%d, master=%d, bursts=%d, ompMaxThreads=%d.",
		numOfImages, masterIndex, su[masterIndex - 1]->burstCount, omp_get_max_threads());
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "coregistration", "process.start", processMessage,
		"DEM projection, burst alignment, resampling, and output writing will be performed.");
	emit_progress(20, "Loading DEM and preparing geometric projection.");
	int ret;
	if (!isdeBurstConfig)
	{
		ret = deBurstConfig();
		if (return_check(ret, "deBurstConfig()", error_head)) return -1;
	}
	FormatConversion conversion;
	ComplexMat slaveSLC, tmp;
	if (static_cast<int>(fullBurstFiles.size()) != numOfImages) return -1;
	int linesPerBurst;
	int samplesPerBurst = su[masterIndex - 1]->samplesPerBurst;
	int lines = 0;
	double lonMin, lonMax, latMin, latMax;
	ret = su[masterIndex - 1]->computeImageGeoBoundry(&lonMin, &lonMax, &latMin, &latMax);
	if (return_check(ret, "computeImageGeoBoundry()", error_head)) return -1;
	ret = loadDEM(this->DEMPath.c_str(), lonMin, lonMax, latMin, latMax);
	if (return_check(ret, "loadDEM()", error_head)) return -1;
	for (int i = 0; i < su[masterIndex - 1]->burstCount; i++)
	{
		const ULONGLONG burstStartTick = GetTickCount64();
		ScopedDiagnosticContext burstDiagnosticScope(diagnosticCallback, diagnosticUserData, -1, i + 1);
		char burstMessage[256] = {};
		sprintf_s(burstMessage, "Processing master burst %d of %d.", i + 1, su[masterIndex - 1]->burstCount);
		emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "coregistration", "burst.start", burstMessage);
		if (isCancelRequested()) return -2;
		if (!burstOffsetComputed)
		{
			int ret = computeBurstOffset();
			if (return_check(ret, "computeBurstOffset()", error_head)) return -1;
		}
		lines = i * su[masterIndex - 1]->linesPerBurst;
		linesPerBurst = su[masterIndex - 1]->linesPerBurst;
		for (int j = 0; j < numOfImages; j++)
		{
			if (isCancelRequested()) return -2;
			if (j == masterIndex - 1) continue;
			ret = slaveSincInterpolation(i + 1, j + 1, slaveSLC);
			if (ret == -2) return -2;
			if (return_check(ret, "slaveSincInterpolation()", error_head)) return -1;
			if (slaveSLC.GetRows() != linesPerBurst || slaveSLC.GetCols() != samplesPerBurst ||
				slaveSLC.re.size() != slaveSLC.im.size())
				return -1;
			tmp = slaveSLC;
			ret = conversion.write_subarray_to_h5(this->fullBurstFiles[j].c_str(), "s_re", tmp.re,
				lines, 0, linesPerBurst, samplesPerBurst);
			if (return_check(ret, "write_subarray_to_h5()", error_head)) return -1;
			ret = conversion.write_subarray_to_h5(this->fullBurstFiles[j].c_str(), "s_im", tmp.im,
				lines, 0, linesPerBurst, samplesPerBurst);
			if (return_check(ret, "write_subarray_to_h5()", error_head)) return -1;
		}
		isMasterRgAzComputed = false;
		sprintf_s(burstMessage, "Completed master burst %d of %d.", i + 1, su[masterIndex - 1]->burstCount);
		emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "coregistration", "burst.complete", burstMessage,
			nullptr, nullptr, nullptr, 0, -1, -1, -1,
			static_cast<long long>(GetTickCount64() - burstStartTick));
	}
	if (!deferFinalDeburstOutput)
	{
		for (int image = 0; image < numOfImages; ++image)
			if (materializeDeburstOutput(fullBurstFiles[image].c_str(), outFiles[image].c_str()) != 0)
				return -1;
	}
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "coregistration", "process.complete", "Sentinel-1 back-geocoding completed.",
		nullptr, nullptr, nullptr, 0, -1, -1, -1,
		static_cast<long long>(GetTickCount64() - taskStartTick));
	return 0;
}

int Sentinel1BackGeocoding::backGeoCodingCoregistration(
	const SentinelRefinementOptions* refinementOptions,
	SentinelRefinementResult* refinementResult,
	InSARDiagnosticCallback callback,
	void* userData)
{
	if (!refinementOptions && !refinementResult)
		return backGeoCodingCoregistration(callback, userData);
	if (!refinementOptions || !refinementResult) return -1;
	if (refinementOptions->enableEsd == 0 && refinementOptions->rangeOffsetMode == SENTINEL_RANGE_OFFSET_NONE &&
		refinementOptions->rangeOffsets == nullptr && refinementOptions->rangeOffsetCount == 0)
	{
		if (refinementOptions->version != SENTINEL_REFINEMENT_OPTIONS_VERSION ||
			refinementOptions->structSize < sizeof(SentinelRefinementOptions) ||
			refinementResult->version != SENTINEL_REFINEMENT_RESULT_VERSION ||
			refinementResult->structSize < sizeof(SentinelRefinementResult)) return -1;
		refinementResult->imageCount = 0;
		emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "refinement", "baseline.core_only",
			"ESD and range refinement are both disabled; invoking the exact legacy core path.",
			"This call does not defer Deburst or execute a separate geometric branch.");
		const int coreResult = backGeoCodingCoregistration(callback, userData);
		if (coreResult == 0)
			refinementResult->executionPath = SENTINEL_EXECUTION_PATH_CORE_ONLY_BASELINE;
		return coreResult;
	}

	deferFinalDeburstOutput = true;
	const int coreResult = backGeoCodingCoregistration(callback, userData);
	deferFinalDeburstOutput = false;
	if (coreResult != 0) return coreResult;
	return applyPostRegistrationRefinement(*refinementOptions, *refinementResult, callback, userData);
}

namespace
{
	constexpr int kRefinementTransactionError = -1301;
	constexpr int kRefinementContractError = -1302;

	struct RefinementTransactionFile
	{
		std::string outputPath;
		std::string temporaryPath;
		std::string fullBurstPath;
		std::string fullBurstTemporaryPath;
		std::string backupPath;
		bool hadOriginalOutput;
	};

	std::string refinement_directory(const std::string& path)
	{
		const std::string::size_type separator = path.find_last_of("\\/");
		return separator == std::string::npos ? std::string(".") : path.substr(0, separator);
	}

	std::string json_escape(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size() + 8);
		for (char character : value)
		{
			if (character == '\\' || character == '"') escaped.push_back('\\');
			escaped.push_back(character);
		}
		return escaped;
	}

	bool write_refinement_manifest(const std::string& manifestPath, const std::string& transactionId,
		const char* state, const std::vector<RefinementTransactionFile>& files)
	{
		std::ostringstream json;
		json << "{\n  \"version\": 1,\n  \"transactionId\": \"" << json_escape(transactionId)
			<< "\",\n  \"state\": \"" << state << "\",\n  \"outputs\": [\n";
		for (size_t index = 0; index < files.size(); ++index)
		{
			json << "    {\"output\": \"" << json_escape(files[index].outputPath)
				<< "\", \"temporary\": \"" << json_escape(files[index].temporaryPath)
				<< "\", \"fullBurst\": \"" << json_escape(files[index].fullBurstPath)
				<< "\", \"fullBurstTemporary\": \"" << json_escape(files[index].fullBurstTemporaryPath)
				<< "\", \"backup\": \"" << json_escape(files[index].backupPath)
				<< "\", \"hadOriginalOutput\": " << (files[index].hadOriginalOutput ? "true" : "false") << "}";
			if (index + 1 < files.size()) json << ',';
			json << '\n';
		}
		json << "  ]\n}\n";

		const std::string temporaryManifest = manifestPath + ".tmp";
		HANDLE file = CreateFileA(temporaryManifest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		const std::string content = json.str();
		DWORD written = 0;
		const bool succeeded = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
			written == content.size() && FlushFileBuffers(file);
		CloseHandle(file);
		if (!succeeded) return false;
		return MoveFileExA(temporaryManifest.c_str(), manifestPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	}

	bool compute_esd_overlap_lines(const Sentinel1Utils* master, std::vector<int>& overlapLines)
	{
		if (!master || master->burstCount < 2 || master->burstAzimuthTime.rows < master->burstCount ||
			master->firstValidLine.rows < master->burstCount || master->lastValidLine.rows < master->burstCount ||
			!std::isfinite(master->azimuthTimeInterval) || master->azimuthTimeInterval <= 0.0)
			return false;

		overlapLines.clear();
		double lastValidTime = master->burstAzimuthTime.at<double>(0, 0) +
			(master->lastValidLine.at<int>(0, 0) - 1) * master->azimuthTimeInterval;
		for (int burst = 1; burst < master->burstCount; ++burst)
		{
			const double firstValidTime = master->burstAzimuthTime.at<double>(burst, 0) +
				(master->firstValidLine.at<int>(burst, 0) - 1) * master->azimuthTimeInterval;
			const int overlap = static_cast<int>(round((lastValidTime - firstValidTime) /
				master->azimuthTimeInterval + 1.0));
			if (overlap < 1 || overlap > master->linesPerBurst) return false;
			overlapLines.push_back(overlap);
			lastValidTime = master->burstAzimuthTime.at<double>(burst, 0) +
				(master->lastValidLine.at<int>(burst, 0) - 1) * master->azimuthTimeInterval;
		}
		return true;
	}

	int validate_refinement_contract(int imageCount, int masterImageIndex, const std::vector<std::string>& outputFiles,
		const SentinelRefinementOptions& options, SentinelRefinementResult& result,
		std::vector<double>& rangeOffsets)
	{
		if (options.version != SENTINEL_REFINEMENT_OPTIONS_VERSION ||
			options.structSize < sizeof(SentinelRefinementOptions) ||
			result.version != SENTINEL_REFINEMENT_RESULT_VERSION ||
			result.structSize < sizeof(SentinelRefinementResult) ||
			imageCount < 2 || masterImageIndex < 1 ||
			masterImageIndex > imageCount ||
			static_cast<int>(outputFiles.size()) != imageCount)
			return kRefinementContractError;

		const int slaveCount = imageCount - 1;
		if (!result.images || result.imageCapacity < slaveCount ||
			(options.enableEsd == 0 && options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_NONE)) return kRefinementContractError;
		if (options.rangeOffsetMode < SENTINEL_RANGE_OFFSET_NONE || options.rangeOffsetMode > SENTINEL_RANGE_OFFSET_ESTIMATE ||
			(options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_NONE && (options.rangeOffsets != nullptr || options.rangeOffsetCount != 0)) ||
			(options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_PROVIDED && (options.rangeOffsets == nullptr || options.rangeOffsetCount != slaveCount)) ||
			(options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_ESTIMATE && (options.rangeOffsets != nullptr || options.rangeOffsetCount != 0)))
			return kRefinementContractError;

		const int rangeMultilook = options.esdRangeMultilook == 0 ? 16 : options.esdRangeMultilook;
		const int azimuthMultilook = options.esdAzimuthMultilook == 0 ? 4 : options.esdAzimuthMultilook;
		const double coherence = options.esdCoherenceThreshold == 0.0 ? 0.4 : options.esdCoherenceThreshold;
		const double binSize = options.esdHistogramBinSize == 0.0 ? 0.1 : options.esdHistogramBinSize;
		const double dopplerRate = options.esdDopplerRateHz == 0.0 ? 4500.0 : options.esdDopplerRateHz;
		const double bandwidth = options.esdAzimuthBandwidthHz == 0.0 ? 486.0 : options.esdAzimuthBandwidthHz;
		const int rangePointCount = options.rangeSamplePointCount == 0 ? 5 : options.rangeSamplePointCount;
		const int rangeTemplateSize = options.rangeTemplateSize == 0 ? 200 : options.rangeTemplateSize;
		const int rangeSearchSize = options.rangeSearchSize == 0 ? 206 : options.rangeSearchSize;
		if ((options.enableEsd != 0 && (rangeMultilook != 16 || azimuthMultilook != 4 || coherence != 0.4 ||
			binSize != 0.1 || dopplerRate != 4500.0 || bandwidth != 486.0)) ||
			(rangePointCount != 5 || rangeTemplateSize != 200 || rangeSearchSize != 206) ||
			!std::isfinite(coherence) || !std::isfinite(binSize) || !std::isfinite(dopplerRate) || !std::isfinite(bandwidth))
			return kRefinementContractError;

		rangeOffsets.assign(imageCount, 0.0);
		if (options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_PROVIDED)
		{
			std::vector<bool> seen(imageCount, false);
			for (int index = 0; index < options.rangeOffsetCount; ++index)
			{
				const SentinelRangeOffsetInput& input = options.rangeOffsets[index];
				if (input.imageIndex < 1 || input.imageIndex > imageCount ||
					input.imageIndex == masterImageIndex || seen[input.imageIndex - 1] || !std::isfinite(input.offset))
					return kRefinementContractError;
				seen[input.imageIndex - 1] = true;
				rangeOffsets[input.imageIndex - 1] = input.offset;
			}
			for (int image = 1; image <= imageCount; ++image)
				if (image != masterImageIndex && !seen[image - 1]) return kRefinementContractError;
		}
		return 0;
	}

	int estimate_range_offset(const char* masterPath, const char* slavePath, int imageIndex,
		InSARDiagnosticCallback callback, void* userData, double& offset)
	{
		offset = 0.0;
		Point2D points[5];
		if (DetectAdaptiveSamplingPoints(masterPath, points, 5) < 0) return -1;
		AlignmentResult results[5] = {};
		for (int i = 0; i < 5; ++i) results[i].structSize = sizeof(AlignmentResult);
		for (int index = 0; index < 5; ++index)
		{
			results[index].heatmap_rgb = nullptr;
			results[index].overlay_rgb = nullptr;
		}
		const int calculationResult = CalculateOffsetAndCoherenceWithDiagnostics(masterPath, slavePath, points, 5,
			200, 206, results, callback, userData);
		if (calculationResult != 0)
		{
			FreeAlignmentResults(results, 5);
			return -1;
		}

		double sum = 0.0;
		std::vector<double> validOffsets;
		bool eligible[5] = {};
		bool used[5] = {};
		for (int index = 0; index < 5; ++index)
		{
			eligible[index] = results[index].maxCorrelation >= 0.15 && std::abs(results[index].offsetX) <= 1;
			if (eligible[index])
			{
				validOffsets.push_back(results[index].offsetX);
				sum += results[index].offsetX;
			}
		}
		if (validOffsets.size() >= 2)
		{
			const double minimum = *std::min_element(validOffsets.begin(), validOffsets.end());
			const double maximum = *std::max_element(validOffsets.begin(), validOffsets.end());
			if (maximum - minimum <= 1)
			{
				offset = sum / validOffsets.size();
				for (int index = 0; index < 5; ++index) used[index] = eligible[index];
			}
		}
		else if (validOffsets.size() == 1)
		{
			for (int index = 0; index < 5; ++index)
			{
				if (results[index].maxCorrelation >= 0.15 && std::abs(results[index].offsetX) <= 1 &&
					results[index].maxCorrelation >= 0.20)
				{
					offset = results[index].offsetX;
					used[index] = true;
					break;
				}
			}
		}
		for (int index = 0; index < 5; ++index)
		{
			const char* reason = nullptr;
			if (!eligible[index])
			{
				if (!std::isfinite(results[index].maxCorrelation)) reason = "correlation_not_finite";
				else if (results[index].maxCorrelation < 0.15) reason = "correlation_below_threshold";
				else if (!std::isfinite(results[index].offsetX)) reason = "range_offset_not_finite";
				else reason = "range_offset_out_of_bounds";
			}
			else if (used[index]) reason = "used_for_final_estimate";
			else if (validOffsets.size() >= 2) reason = "eligible_offsets_inconsistent";
			else reason = "single_eligible_correlation_below_threshold";

			char message[384] = {};
			sprintf_s(message,
				"image=%d sample=%d/5 correlation=%.6f; range_offset=%.6f azimuth_offset=%.6f; eligible=%s used=%s; reason=%s",
				imageIndex, index + 1, results[index].maxCorrelation, results[index].offsetX, results[index].offsetY,
				eligible[index] ? "true" : "false", used[index] ? "true" : "false", reason);
			emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "refinement", "refinement.range_sample", message,
				"Range amplitude sample decision after threshold and whole-set consistency evaluation.", slavePath, "offset_r");
		}
		FreeAlignmentResults(results, 5);
		return 0;
	}
}

int Sentinel1BackGeocoding::applyPostRegistrationRefinement(
	const SentinelRefinementOptions& options,
	SentinelRefinementResult& result,
	InSARDiagnosticCallback callback,
	void* userData)
{
	if (callback || userData) setDiagnosticCallback(callback, userData);
	ScopedDiagnosticContext diagnosticScope(diagnosticCallback, diagnosticUserData);
	std::vector<double> rangeOffsets;
	const int contractResult = validate_refinement_contract(numOfImages, masterIndex, outFiles, options, result, rangeOffsets);
	if (contractResult != 0)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "refinement", "refinement.validate", "Invalid Sentinel refinement contract.",
			"Versions, 1-based range offset entries, output buffers, and frozen ESD constants are required.", nullptr, nullptr, contractResult);
		return contractResult;
	}
	if (isCancelRequested()) return -2;
	if (static_cast<int>(fullBurstFiles.size()) != numOfImages) return kRefinementContractError;
	const Sentinel1Utils* fullBurstMaster = su[masterIndex - 1];
	if (!fullBurstMaster) return kRefinementContractError;
	const int expectedFullBurstRows = fullBurstMaster->burstCount * fullBurstMaster->linesPerBurst;
	for (int image = 0; image < numOfImages; ++image)
	{
		int rows = 0, columns = 0;
		if (fullBurstFiles[image].empty() || Hdf5IO::getDatasetDims(fullBurstFiles[image].c_str(), "s_re", &rows, &columns) != 0 ||
			rows != expectedFullBurstRows || columns != fullBurstMaster->samplesPerBurst)
			return kRefinementContractError;
	}

	const std::string transactionDirectory = options.transactionDirectory && *options.transactionDirectory ?
		options.transactionDirectory : refinement_directory(outFiles[masterIndex - 1]);
	for (const std::string& outputPath : outFiles)
		if (_stricmp(refinement_directory(outputPath).c_str(), transactionDirectory.c_str()) != 0)
			return kRefinementContractError;
	const std::string manifestPath = transactionDirectory + "\\refinement_transaction.json";
	if (GetFileAttributesA(manifestPath.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "refinement", "refinement.transaction_exists", "A refinement transaction already exists.",
			"Existing complete, in-progress, or failed transactions must not be overwritten.", manifestPath.c_str(), nullptr, kRefinementTransactionError);
		return kRefinementTransactionError;
	}

	const std::string transactionId = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
	std::vector<RefinementTransactionFile> transactionFiles;
	transactionFiles.reserve(outFiles.size());
	for (size_t image = 0; image < outFiles.size(); ++image)
		transactionFiles.push_back({ outFiles[image], outFiles[image] + ".refinement-" + transactionId + ".tmp",
			fullBurstFiles[image],
			outFiles[image] + ".refinement-" + transactionId + ".fullburst.tmp",
			outFiles[image] + ".refinement-" + transactionId + ".bak",
			GetFileAttributesA(outFiles[image].c_str()) != INVALID_FILE_ATTRIBUTES });
	if (!write_refinement_manifest(manifestPath, transactionId, "in_progress", transactionFiles)) return kRefinementTransactionError;

	auto fail = [&](int code, const char* phase, const char* message) {
		write_refinement_manifest(manifestPath, transactionId, code == -2 ? "in_progress" : "failed", transactionFiles);
		emit_diagnostic(code == -2 ? INSAR_DIAGNOSTIC_WARNING : INSAR_DIAGNOSTIC_ERROR, "refinement", phase, message,
			"The output group remains invalid until its transaction manifest is recovered or cleaned.", manifestPath.c_str(), nullptr, code);
		return code;
	};

	for (size_t image = 0; image < transactionFiles.size(); ++image)
	{
		const RefinementTransactionFile& file = transactionFiles[image];
		if (!CopyFileA(file.fullBurstPath.c_str(), file.fullBurstTemporaryPath.c_str(), TRUE) ||
			Hdf5IO::writeString(file.fullBurstTemporaryPath.c_str(), "refinement_transaction_id", transactionId.c_str()) != 0 ||
			Hdf5IO::writeString(file.fullBurstTemporaryPath.c_str(), "refinement_state", "in_progress") != 0)
			return fail(kRefinementTransactionError, "refinement.transaction_prepare", "Failed to prepare a refinement transaction output.");
	}

	emit_progress(75, "Preparing post-registration refinement transaction.");
	const Sentinel1Utils* master = su[masterIndex - 1];
	if (!master) return fail(kRefinementContractError, "refinement.validate", "Master Sentinel metadata is unavailable.");
	std::vector<int> overlapLines;
	if (options.enableEsd != 0 && !compute_esd_overlap_lines(master, overlapLines))
		return fail(kRefinementContractError, "esd.overlap", "Unable to construct valid ESD burst overlap geometry.");

	result.imageCount = 0;
	for (int image = 1; image <= numOfImages; ++image)
	{
		if (image == masterIndex) continue;
		SentinelRefinementImageResult& imageResult = result.images[result.imageCount++];
		imageResult = {};
		imageResult.imageIndex = image;
		imageResult.esdQualityCode = options.enableEsd ? SENTINEL_REFINEMENT_QUALITY_UNAVAILABLE : SENTINEL_REFINEMENT_QUALITY_NOT_REQUESTED;
		imageResult.rangeOffset = rangeOffsets[image - 1];
		imageResult.rangeQualityCode = options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_PROVIDED ?
			(fabs(imageResult.rangeOffset) < 1.0e-12 ? SENTINEL_REFINEMENT_QUALITY_APPLIED_ZERO : SENTINEL_REFINEMENT_QUALITY_APPLIED) :
			(options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_ESTIMATE ? SENTINEL_REFINEMENT_QUALITY_UNAVAILABLE : SENTINEL_REFINEMENT_QUALITY_NOT_REQUESTED);
		if (options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_NONE) imageResult.warningFlags |= SENTINEL_REFINEMENT_WARNING_RANGE_NOT_REQUESTED;
		else if (options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_PROVIDED && fabs(imageResult.rangeOffset) < 1.0e-12)
			imageResult.warningFlags |= SENTINEL_REFINEMENT_WARNING_RANGE_EXPLICIT_ZERO;
	}
	if (options.rangeOffsetMode != SENTINEL_RANGE_OFFSET_NONE)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "range_refinement", "range.started", "Starting range amplitude refinement.",
			"Frozen Registration parameters: five adaptive points, template=200, search=206, correlation and consistency voting.");
		for (int resultIndex = 0; resultIndex < result.imageCount; ++resultIndex)
		{
			if (isCancelRequested()) return fail(-2, "range.cancelled", "Range amplitude refinement was cancelled.");
			SentinelRefinementImageResult& imageResult = result.images[resultIndex];
			const int slaveIndex = imageResult.imageIndex - 1;
			ScopedDiagnosticContext imageScope(diagnosticCallback, diagnosticUserData, imageResult.imageIndex);
			if (options.rangeOffsetMode == SENTINEL_RANGE_OFFSET_ESTIMATE)
			{
				if (estimate_range_offset(transactionFiles[masterIndex - 1].fullBurstTemporaryPath.c_str(),
					transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), imageResult.imageIndex, diagnosticCallback, diagnosticUserData,
					imageResult.rangeOffset) != 0)
				{
					imageResult.rangeOffset = 0.0;
					imageResult.rangeQualityCode = SENTINEL_REFINEMENT_QUALITY_UNAVAILABLE;
					emit_diagnostic(INSAR_DIAGNOSTIC_WARNING, "range_refinement", "range.warning",
						"Range offset estimation was unavailable; zero correction will be retained.", nullptr,
						transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "offset_r", imageResult.rangeQualityCode);
				}
				else
				{
					imageResult.rangeQualityCode = fabs(imageResult.rangeOffset) < 1.0e-12 ?
						SENTINEL_REFINEMENT_QUALITY_APPLIED_ZERO : SENTINEL_REFINEMENT_QUALITY_APPLIED;
					if (imageResult.rangeQualityCode == SENTINEL_REFINEMENT_QUALITY_APPLIED_ZERO)
						imageResult.warningFlags |= SENTINEL_REFINEMENT_WARNING_RANGE_EXPLICIT_ZERO;
				}
			}
			if (Hdf5IO::writeDouble(transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "offset_r", imageResult.rangeOffset) != 0)
				return fail(kRefinementTransactionError, "range.write_offset", "Failed to write range refinement offset.");
			char message[256] = {};
			sprintf_s(message, "Range offset for slave image %d is %.12g pixels; pull resampling uses src_col=dst_col+offset_r, so a positive offset increases the source column.", imageResult.imageIndex, imageResult.rangeOffset);
			emit_diagnostic(imageResult.rangeQualityCode == SENTINEL_REFINEMENT_QUALITY_UNAVAILABLE ? INSAR_DIAGNOSTIC_WARNING : INSAR_DIAGNOSTIC_INFO,
				"range_refinement", "range.image_offset", message, nullptr,
				transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "offset_r", imageResult.rangeQualityCode);
		}
	}

	Utils util;
	if (options.enableEsd != 0)
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "esd", "esd.started", "Starting Enhanced Spectral Diversity correction.",
			"Frozen parameters: multilook=16x4, coherence=0.4, histogramBin=0.1, conversion=486/(2*pi*4500).");
		emit_progress(80, "Estimating Enhanced Spectral Diversity correction.");
		const int sampleCount = master->samplesPerBurst;
		int totalOverlapLines = 0;
		for (int overlap : overlapLines) totalOverlapLines += overlap;
		for (int resultIndex = 0; resultIndex < result.imageCount; ++resultIndex)
		{
			if (isCancelRequested()) return fail(-2, "esd.cancelled", "ESD correction was cancelled.");
			SentinelRefinementImageResult& imageResult = result.images[resultIndex];
			const int slaveIndex = imageResult.imageIndex - 1;
			ScopedDiagnosticContext imageScope(diagnosticCallback, diagnosticUserData, imageResult.imageIndex);
			cv::Mat overlapPhase(totalOverlapLines, sampleCount, CV_64F);
			ComplexMat masterUp, masterDown, slaveUp, slaveDown;
			int destinationRow = 0;
			for (int burst = 1; burst < master->burstCount; ++burst)
			{
				const int overlap = overlapLines[burst - 1];
				const int upperRow = (burst - 1) * master->linesPerBurst + master->lastValidLine.at<int>(burst - 1, 0) - overlap;
				const int lowerRow = burst * master->linesPerBurst + master->firstValidLine.at<int>(burst, 0) - 1;
				const char* masterPath = transactionFiles[masterIndex - 1].fullBurstTemporaryPath.c_str();
				const char* slavePath = transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str();
				if (Hdf5IO::readSubarray(masterPath, "s_re", upperRow, 0, overlap, sampleCount, masterUp.re) != 0 ||
					Hdf5IO::readSubarray(masterPath, "s_im", upperRow, 0, overlap, sampleCount, masterUp.im) != 0 ||
					Hdf5IO::readSubarray(slavePath, "s_re", upperRow, 0, overlap, sampleCount, slaveUp.re) != 0 ||
					Hdf5IO::readSubarray(slavePath, "s_im", upperRow, 0, overlap, sampleCount, slaveUp.im) != 0 ||
					Hdf5IO::readSubarray(masterPath, "s_re", lowerRow, 0, overlap, sampleCount, masterDown.re) != 0 ||
					Hdf5IO::readSubarray(masterPath, "s_im", lowerRow, 0, overlap, sampleCount, masterDown.im) != 0 ||
					Hdf5IO::readSubarray(slavePath, "s_re", lowerRow, 0, overlap, sampleCount, slaveDown.re) != 0 ||
					Hdf5IO::readSubarray(slavePath, "s_im", lowerRow, 0, overlap, sampleCount, slaveDown.im) != 0)
					return fail(kRefinementTransactionError, "esd.read_overlap", "Failed to read ESD overlap SLC data.");
				masterUp.convertTo(masterUp, CV_64F); masterDown.convertTo(masterDown, CV_64F);
				slaveUp.convertTo(slaveUp, CV_64F); slaveDown.convertTo(slaveDown, CV_64F);
				masterUp.Mul(slaveUp, masterUp, true);
				masterDown.Mul(slaveDown, masterDown, true);
				masterUp.Mul(masterDown, masterUp, true);
				masterUp.GetPhase().copyTo(overlapPhase(cv::Range(destinationRow, destinationRow + overlap), cv::Range(0, sampleCount)));
				destinationRow += overlap;
			}
			cv::Mat multilookedPhase, coherence;
			util.multilook(overlapPhase, multilookedPhase, 16, 4);
			util.phase_coherence(multilookedPhase, coherence);
			int coherentSamples = 0;
			for (int row = 0; row < coherence.rows; ++row)
				for (int column = 0; column < coherence.cols; ++column)
					if (coherence.at<double>(row, column) < 0.4) multilookedPhase.at<double>(row, column) = 0.0;
					else ++coherentSamples;
			cv::Mat histogramX, histogramY;
			multilookedPhase = multilookedPhase.reshape(0, 1);
			util.hist(multilookedPhase, -PI, PI, 0.1, histogramX, histogramY);
			if (histogramX.empty() || histogramY.empty()) return fail(kRefinementTransactionError, "esd.histogram", "Failed to construct ESD phase histogram.");
			if (histogramX.type() != CV_64F) histogramX.convertTo(histogramX, CV_64F);
			if (histogramY.type() != CV_64F) histogramY.convertTo(histogramY, CV_64F);
			if (histogramY.total() > 33)
			{
				const double x = 31.0, x0 = 29.0, x1 = 30.0, x2 = 32.0, x3 = 33.0;
				const double y0 = histogramY.at<double>(29), y1 = histogramY.at<double>(30);
				const double y2 = histogramY.at<double>(32), y3 = histogramY.at<double>(33);
				histogramY.at<double>(31) = (x - x1) * (x - x2) * (x - x3) / ((x0 - x1) * (x0 - x2) * (x0 - x3)) * y0 +
					(x - x0) * (x - x2) * (x - x3) / ((x1 - x0) * (x1 - x2) * (x1 - x3)) * y1 +
					(x - x0) * (x - x1) * (x - x3) / ((x2 - x0) * (x2 - x1) * (x2 - x3)) * y2 +
					(x - x0) * (x - x1) * (x - x2) / ((x3 - x0) * (x3 - x1) * (x3 - x2)) * y3;
			}
			cv::Point peak;
			cv::minMaxLoc(histogramY, nullptr, nullptr, nullptr, &peak);
			const double phaseOffset = peak.x >= 0 && peak.x < histogramX.total() ? histogramX.at<double>(peak.x) : 0.0;
			imageResult.esdAzimuthOffset = phaseOffset / (2.0 * PI * 4500.0) * 486.0;
			imageResult.esdQualityCode = coherentSamples > 0 ? SENTINEL_REFINEMENT_QUALITY_APPLIED : SENTINEL_REFINEMENT_QUALITY_WARNING_LOW_COHERENCE;
			if (coherentSamples == 0) imageResult.warningFlags |= SENTINEL_REFINEMENT_WARNING_ESD_LOW_COHERENCE;
			if (Hdf5IO::writeDouble(transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "offset_a", imageResult.esdAzimuthOffset) != 0)
				return fail(kRefinementTransactionError, "esd.write_offset", "Failed to write ESD azimuth offset.");
			char message[256] = {};
			sprintf_s(message, "ESD azimuth offset for slave image %d is %.12g pixels; pull resampling uses src_row=dst_row+offset_a, so a positive offset increases the source row.", imageResult.imageIndex, imageResult.esdAzimuthOffset);
			emit_diagnostic(coherentSamples > 0 ? INSAR_DIAGNOSTIC_INFO : INSAR_DIAGNOSTIC_WARNING, "esd",
				coherentSamples > 0 ? "esd.image_offset" : "esd.warning", message, nullptr,
				transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "offset_a", imageResult.esdQualityCode);
			emit_progress(84, "ESD offset estimation completed.");
		}
	}

	if (!burstOffsetComputed && computeBurstOffset() != 0)
		return fail(kRefinementTransactionError, "refinement.burst_offset", "Failed to compute Sentinel burst offsets for compensation.");
	ComplexMat slaveSlc, reramp;
	for (int burst = 0; burst < master->burstCount; ++burst)
	{
		if (isCancelRequested()) return fail(-2, "refinement.cancelled", "Post-registration refinement was cancelled.");
		for (int resultIndex = 0; resultIndex < result.imageCount; ++resultIndex)
		{
			SentinelRefinementImageResult& imageResult = result.images[resultIndex];
			const int slaveIndex = imageResult.imageIndex - 1;
			if (fabs(imageResult.esdAzimuthOffset) < 0.0001 && fabs(imageResult.rangeOffset) < 0.01) continue;
			const int slaveBurst = burst + 1 + su[slaveIndex]->burstOffset;
			if (slaveBurst < 1 || slaveBurst > su[slaveIndex]->burstCount) continue;
			if (su[slaveIndex]->getBurst(slaveBurst, slaveSlc) != 0) return fail(kRefinementTransactionError, "refinement.read_burst", "Failed to read slave burst for compensation.");
			char burstMappingMessage[320] = {};
			sprintf_s(burstMappingMessage, "Refinement mapping: masterBurst=%d, slaveBurst=%d, burstOffset=%d, coefficientDataset=burst_%d_coef.",
				burst + 1, slaveBurst, su[slaveIndex]->burstOffset, burst + 1);
			emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "refinement", "burst_mapping", burstMappingMessage,
				"Coefficient datasets are indexed by master burst, while source reads use the burst-offset-adjusted slave burst.");

			if (slaveSlc.type() != CV_64F) slaveSlc.convertTo(slaveSlc, CV_64F);
			cv::Mat derampPhase;
			if (su[slaveIndex]->computeDerampDemodPhase(slaveBurst, derampPhase) != 0 || performDerampDemod(derampPhase, slaveSlc) != 0)
				return fail(kRefinementTransactionError, "refinement.deramp", "Failed to deramp slave burst for compensation.");
			cv::Mat coefficients;
			const std::string coefficientName = "burst_" + std::to_string(burst + 1) + "_coef";
			const int coefficientResult = Hdf5IO::readArray(transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), coefficientName.c_str(), coefficients);
			if (coefficientResult != 0 || coefficients.rows != 1 || coefficients.cols != 6)
				return fail(kRefinementTransactionError, "refinement.read_coefficients", "Missing or invalid initial geometric coefficient vector.");
			if (coefficients.type() != CV_64F)
				return fail(kRefinementTransactionError, "refinement.read_coefficients", "Initial geometric coefficient vector must be stored as CV_64F.");
			for (int coefficientIndex = 0; coefficientIndex < 6; ++coefficientIndex)
			{
				if (!std::isfinite(coefficients.at<double>(coefficientIndex)))
					return fail(kRefinementTransactionError, "refinement.read_coefficients", "Initial geometric coefficient vector contains a non-finite value.");
			}

			double a0Rg = coefficients.at<double>(0) + imageResult.rangeOffset;
			double a1Rg = coefficients.at<double>(1);
			double a2Rg = coefficients.at<double>(2);
			double a0Az = coefficients.at<double>(3) + imageResult.esdAzimuthOffset;
			double a1Az = coefficients.at<double>(4);
			double a2Az = coefficients.at<double>(5);
			char appliedCoefficientMessage[384] = {};
			sprintf_s(appliedCoefficientMessage, "Applying refinement coefficients: range={%.12g, %.12g, %.12g}, azimuth={%.12g, %.12g, %.12g}.",
				a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az);
			emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "refinement", "coefficients.applied", appliedCoefficientMessage,
				"a0Rg=coef[0]+offset_r and a0Az=coef[3]+offset_a; the remaining coefficients are unchanged.",
				transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), coefficientName.c_str(), 0, 1, 6, CV_64F);
			if (performSincResampling(slaveSlc, master->linesPerBurst, master->samplesPerBurst, a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az) != 0)
				return fail(isCancelRequested() ? -2 : kRefinementTransactionError, "refinement.resample", "Failed to resample slave SLC.");
			reramp.SetRe(derampPhase); reramp.SetIm(derampPhase);
			if (performSincResampling(reramp, master->linesPerBurst, master->samplesPerBurst, a0Rg, a1Rg, a2Rg, a0Az, a1Az, a2Az) != 0)
				return fail(isCancelRequested() ? -2 : kRefinementTransactionError, "refinement.resample_phase", "Failed to resample deramp phase.");
			reramp.re.copyTo(derampPhase);
			util.phase2cos(derampPhase, reramp.re, reramp.im);
			slaveSlc.Mul(reramp, slaveSlc, true);
			slaveSlc.convertTo(slaveSlc, CV_32F);
			const int outputRow = burst * master->linesPerBurst;
			if (Hdf5IO::writeSubarray(transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "s_re", slaveSlc.re, outputRow, 0) != 0 ||
				Hdf5IO::writeSubarray(transactionFiles[slaveIndex].fullBurstTemporaryPath.c_str(), "s_im", slaveSlc.im, outputRow, 0) != 0)
				return fail(kRefinementTransactionError, "refinement.write_slc", "Failed to write compensated SLC data.");
		}
		emit_progress(84 + 12 * (burst + 1) / master->burstCount, "Applying ESD and range corrections.");
	}

	emit_progress(96, "Materializing final deburst outputs.");
	for (const RefinementTransactionFile& file : transactionFiles)
	{
		int reRows = 0, reColumns = 0, imRows = 0, imColumns = 0;
		if (Hdf5IO::getDatasetDims(file.fullBurstTemporaryPath.c_str(), "s_re", &reRows, &reColumns) != 0 ||
			Hdf5IO::getDatasetDims(file.fullBurstTemporaryPath.c_str(), "s_im", &imRows, &imColumns) != 0 ||
			reRows != expectedFullBurstRows || imRows != expectedFullBurstRows ||
			reColumns != fullBurstMaster->samplesPerBurst || imColumns != fullBurstMaster->samplesPerBurst)
			return fail(kRefinementTransactionError, "refinement.verify_full_burst", "Refinement full-burst temporary output validation failed.");
		if (materializeDeburstOutput(file.fullBurstTemporaryPath.c_str(), file.temporaryPath.c_str()) != 0)
			return fail(kRefinementTransactionError, "refinement.deburst", "Failed to materialize the final deburst output.");
		if (Hdf5IO::getDatasetDims(file.temporaryPath.c_str(), "s_re", &reRows, &reColumns) != 0 ||
			Hdf5IO::getDatasetDims(file.temporaryPath.c_str(), "s_im", &imRows, &imColumns) != 0 ||
			reRows != deburstLines || imRows != deburstLines ||
			reColumns != fullBurstMaster->samplesPerBurst || imColumns != fullBurstMaster->samplesPerBurst)
			return fail(kRefinementTransactionError, "refinement.verify_deburst", "Final deburst temporary output validation failed.");
		std::string temporaryState;
		std::string temporaryTransactionId;
		if (Hdf5IO::readString(file.temporaryPath.c_str(), "refinement_state", temporaryState) != 0 ||
			Hdf5IO::readString(file.temporaryPath.c_str(), "refinement_transaction_id", temporaryTransactionId) != 0 ||
			temporaryState != "in_progress" || temporaryTransactionId != transactionId)
			return fail(kRefinementTransactionError, "refinement.verify_marker", "Final deburst temporary output transaction marker validation failed.");
	}
	emit_progress(98, "Finalizing output transaction.");
	for (const RefinementTransactionFile& file : transactionFiles)
		if (GetFileAttributesA(file.fullBurstTemporaryPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
			!DeleteFileA(file.fullBurstTemporaryPath.c_str()))
			return fail(kRefinementTransactionError, "refinement.cleanup_full_burst", "Failed to clean the full-burst refinement temporary output.");
	for (const RefinementTransactionFile& file : transactionFiles)
	{
		if ((file.hadOriginalOutput && !MoveFileExA(file.outputPath.c_str(), file.backupPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ||
			(!file.hadOriginalOutput && GetFileAttributesA(file.outputPath.c_str()) != INVALID_FILE_ATTRIBUTES) ||
			!MoveFileExA(file.temporaryPath.c_str(), file.outputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return fail(kRefinementTransactionError, "refinement.commit", "Failed to commit a refinement output file.");
	}
	for (const RefinementTransactionFile& file : transactionFiles)
	{
		std::string state;
		std::string committedTransactionId;
		if (Hdf5IO::writeString(file.outputPath.c_str(), "refinement_transaction_id", transactionId.c_str()) != 0 ||
			Hdf5IO::writeString(file.outputPath.c_str(), "refinement_state", "complete") != 0 ||
			Hdf5IO::readString(file.outputPath.c_str(), "refinement_state", state) != 0 ||
			Hdf5IO::readString(file.outputPath.c_str(), "refinement_transaction_id", committedTransactionId) != 0 ||
			state != "complete" || committedTransactionId != transactionId)
			return fail(kRefinementTransactionError, "refinement.commit_marker", "Failed to finalize a refinement output marker.");
	}
	if (!write_refinement_manifest(manifestPath, transactionId, "complete", transactionFiles))
		return fail(kRefinementTransactionError, "refinement.commit_manifest", "Failed to commit the refinement transaction manifest.");
	for (size_t image = 0; image < transactionFiles.size(); ++image)
	{
		const RefinementTransactionFile& file = transactionFiles[image];
		if (GetFileAttributesA(file.backupPath.c_str()) != INVALID_FILE_ATTRIBUTES)
			DeleteFileA(file.backupPath.c_str());
		if (image < fullBurstFiles.size() && GetFileAttributesA(fullBurstFiles[image].c_str()) != INVALID_FILE_ATTRIBUTES)
			DeleteFileA(fullBurstFiles[image].c_str());
	}

	// A completed transaction has already published and validated its final H5
	// files. Residual working files are therefore cleanup warnings only.
	std::vector<std::string> cleanupResiduals;
	for (const RefinementTransactionFile& file : transactionFiles)
	{
		const std::string paths[] = {
			file.fullBurstPath,
			file.temporaryPath,
			file.fullBurstTemporaryPath,
			file.backupPath
		};
		for (const std::string& path : paths)
		{
			if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
				cleanupResiduals.push_back(path);
		}
	}
	if (cleanupResiduals.empty())
	{
		emit_diagnostic(INSAR_DIAGNOSTIC_DEBUG, "refinement", "refinement.cleanup_verified",
			"Post-commit temporary refinement cleanup verified.",
			"No task-owned .fullburst, .tmp, or .bak files remain.", manifestPath.c_str());
	}
	else
	{
		std::string detail = "Final H5 outputs remain published. Residual files: ";
		for (size_t i = 0; i < cleanupResiduals.size(); ++i)
		{
			if (i > 0) detail += "; ";
			detail += cleanupResiduals[i];
		}
		emit_diagnostic(INSAR_DIAGNOSTIC_WARNING, "refinement", "refinement.cleanup_residual",
			"Post-commit temporary refinement files remain.", detail.c_str(), manifestPath.c_str());
	}

	result.executionPath = SENTINEL_EXECUTION_PATH_POST_REGISTRATION_REFINEMENT;
	emit_progress(100, "Sentinel-1 back-geocoding completed.");
	emit_diagnostic(INSAR_DIAGNOSTIC_INFO, "esd", "refinement.completed", "Post-registration refinement completed.",
		"All output H5 files and the directory transaction manifest are complete.", manifestPath.c_str());
	return 0;
}

int Sentinel1BackGeocoding::getPostRegistrationRefinementTransactionStatus(
	SentinelRefinementTransactionStatus& status) const
{
	if (status.version != SENTINEL_REFINEMENT_TRANSACTION_STATUS_VERSION ||
		status.structSize < sizeof(SentinelRefinementTransactionStatus)) return -1;
	return inspect_refinement_manifest(outFiles, status);
}

int Sentinel1BackGeocoding::verifyOutputsAgainstReference(
	const vector<string>& referenceFiles,
	double coefficientTolerance,
	double offsetTolerance,
	double slcTolerance,
	SentinelBackGeocodingVerificationResult& result) const
{
	if (referenceFiles.size() != outFiles.size() || !result.images || result.imageCapacity < static_cast<int>(outFiles.size()) ||
		!std::isfinite(coefficientTolerance) || !std::isfinite(offsetTolerance) || !std::isfinite(slcTolerance) ||
		coefficientTolerance < 0.0 || offsetTolerance < 0.0 || slcTolerance < 0.0)
		return -1;

	result.imageCount = 0;
	result.passed = 1;
	const int expectedBurstCount = masterIndex >= 1 && masterIndex <= static_cast<int>(su.size()) && su[masterIndex - 1] ?
		su[masterIndex - 1]->burstCount : 0;
	if (expectedBurstCount <= 0) return -1;

	for (int image = 0; image < static_cast<int>(outFiles.size()); ++image)
	{
		ScopedDiagnosticContext imageScope(diagnosticCallback, diagnosticUserData, image + 1);
		SentinelBackGeocodingVerificationImageResult& imageResult = result.images[result.imageCount++];
		imageResult = {};
		imageResult.imageIndex = image + 1;
		imageResult.maxCoefficientAbsError = std::numeric_limits<double>::quiet_NaN();
		imageResult.esdOffsetAbsError = std::numeric_limits<double>::quiet_NaN();
		imageResult.rangeOffsetAbsError = std::numeric_limits<double>::quiet_NaN();
		imageResult.maxSlcComponentAbsError = std::numeric_limits<double>::quiet_NaN();
		imageResult.rmsSlcComponentError = std::numeric_limits<double>::quiet_NaN();

		imageResult.outputFirstSourceRow = -1;
		imageResult.outputFirstSourceColumn = -1;
		imageResult.referenceFirstSourceRow = -1;
		imageResult.referenceFirstSourceColumn = -1;
		cv::Mat outputRe, outputIm, referenceRe, referenceIm;
		if (Hdf5IO::readArray(outFiles[image].c_str(), "s_re", outputRe) != 0 ||
			Hdf5IO::readArray(outFiles[image].c_str(), "s_im", outputIm) != 0 ||
			Hdf5IO::readArray(referenceFiles[image].c_str(), "s_re", referenceRe) != 0 ||
			Hdf5IO::readArray(referenceFiles[image].c_str(), "s_im", referenceIm) != 0)
		{
			imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET;
			result.passed = 0;
			emit_diagnostic(INSAR_DIAGNOSTIC_ERROR, "verification", "slc.read", "Unable to read a current or reference SLC dataset.",
				"Both H5 files must contain s_re and s_im.", outFiles[image].c_str(), "s_re", -1401);
			continue;
		}
		imageResult.outputRows = outputRe.rows;
		imageResult.outputColumns = outputRe.cols;
		imageResult.referenceRows = referenceRe.rows;
		imageResult.referenceColumns = referenceRe.cols;
		if (outputRe.rows != outputIm.rows || outputRe.cols != outputIm.cols ||
			referenceRe.rows != referenceIm.rows || referenceRe.cols != referenceIm.cols ||
			outputRe.rows != referenceRe.rows || outputRe.cols != referenceRe.cols)
		{
			imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_DIMENSIONS;
			result.passed = 0;
			continue;
		}
		outputRe.convertTo(outputRe, CV_64F);
		int outputFirstRow = -1, outputFirstColumn = -1, referenceFirstRow = -1, referenceFirstColumn = -1;
		const int outputOriginResult = Hdf5IO::readInt(outFiles[image].c_str(), "deburst_first_source_row", &outputFirstRow);
		const int outputColumnResult = Hdf5IO::readInt(outFiles[image].c_str(), "deburst_first_source_column", &outputFirstColumn);
		const int referenceOriginResult = Hdf5IO::readInt(referenceFiles[image].c_str(), "deburst_first_source_row", &referenceFirstRow);
		const int referenceColumnResult = Hdf5IO::readInt(referenceFiles[image].c_str(), "deburst_first_source_column", &referenceFirstColumn);
		if (outputOriginResult == 0 && outputColumnResult == 0 && referenceOriginResult == 0 && referenceColumnResult == 0)
		{
			imageResult.outputFirstSourceRow = outputFirstRow;
			imageResult.outputFirstSourceColumn = outputFirstColumn;
			imageResult.referenceFirstSourceRow = referenceFirstRow;
			imageResult.referenceFirstSourceColumn = referenceFirstColumn;
			if (outputFirstRow != referenceFirstRow || outputFirstColumn != referenceFirstColumn)
			{
				imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_DEBURST_ORIGIN;
				result.passed = 0;
			}
		}

		outputIm.convertTo(outputIm, CV_64F);
		referenceRe.convertTo(referenceRe, CV_64F);
		referenceIm.convertTo(referenceIm, CV_64F);
		double maxComponentError = 0.0;
		long double sumSquaredError = 0.0;
		for (int row = 0; row < outputRe.rows; ++row)
			for (int column = 0; column < outputRe.cols; ++column)
			{
				const double reError = outputRe.at<double>(row, column) - referenceRe.at<double>(row, column);
				const double imError = outputIm.at<double>(row, column) - referenceIm.at<double>(row, column);
				maxComponentError = std::max(maxComponentError, std::max(fabs(reError), fabs(imError)));
				sumSquaredError += static_cast<long double>(reError) * reError + static_cast<long double>(imError) * imError;
			}
		imageResult.maxSlcComponentAbsError = maxComponentError;
		imageResult.rmsSlcComponentError = sqrt(static_cast<double>(sumSquaredError / (2.0L * outputRe.total())));
		if (maxComponentError > slcTolerance)
		{
			imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_SLC;
			result.passed = 0;
		}

		if (image + 1 != masterIndex)
		{
			double outputOffset = 0.0, referenceOffset = 0.0;
			int outputOffsetResult = Hdf5IO::readDouble(outFiles[image].c_str(), "offset_a", &outputOffset);
			int referenceOffsetResult = Hdf5IO::readDouble(referenceFiles[image].c_str(), "offset_a", &referenceOffset);
			if ((outputOffsetResult == 0) != (referenceOffsetResult == 0))
			{
				imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_ESD_OFFSET | SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET;
				result.passed = 0;
			}
			else if (outputOffsetResult == 0)
			{
				imageResult.esdOffsetAbsError = fabs(outputOffset - referenceOffset);
				if (imageResult.esdOffsetAbsError > offsetTolerance)
				{
					imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_ESD_OFFSET;
					result.passed = 0;
				}
			}

			outputOffsetResult = Hdf5IO::readDouble(outFiles[image].c_str(), "offset_r", &outputOffset);
			referenceOffsetResult = Hdf5IO::readDouble(referenceFiles[image].c_str(), "offset_r", &referenceOffset);
			if ((outputOffsetResult == 0) != (referenceOffsetResult == 0))
			{
				imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_RANGE_OFFSET | SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET;
				result.passed = 0;
			}
			else if (outputOffsetResult == 0)
			{
				imageResult.rangeOffsetAbsError = fabs(outputOffset - referenceOffset);
				if (imageResult.rangeOffsetAbsError > offsetTolerance)
				{
					imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_RANGE_OFFSET;
					result.passed = 0;
				}
			}

			double maxCoefficientError = 0.0;
			for (int burst = 1; burst <= expectedBurstCount; ++burst)
			{
				const std::string coefficientName = "burst_" + std::to_string(burst) + "_coef";
				cv::Mat outputCoefficients, referenceCoefficients;
				if (Hdf5IO::readArray(outFiles[image].c_str(), coefficientName.c_str(), outputCoefficients) != 0 ||
					Hdf5IO::readArray(referenceFiles[image].c_str(), coefficientName.c_str(), referenceCoefficients) != 0 ||
					outputCoefficients.rows != 1 || outputCoefficients.cols != 6 ||
					referenceCoefficients.rows != 1 || referenceCoefficients.cols != 6)
				{
					imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET;
					result.passed = 0;
					continue;
				}
				outputCoefficients.convertTo(outputCoefficients, CV_64F);
				referenceCoefficients.convertTo(referenceCoefficients, CV_64F);
				for (int coefficient = 0; coefficient < 6; ++coefficient)
				{
					const double difference = fabs(outputCoefficients.at<double>(coefficient) - referenceCoefficients.at<double>(coefficient));
					if (!std::isfinite(difference))
					{
						imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET;
						result.passed = 0;
					}
					maxCoefficientError = std::max(maxCoefficientError, difference);
				}
				++imageResult.comparedCoefficientBursts;
			}
			imageResult.maxCoefficientAbsError = maxCoefficientError;
			if (imageResult.comparedCoefficientBursts != expectedBurstCount)
			{
				imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET;
				result.passed = 0;
			}
			if (maxCoefficientError > coefficientTolerance)
			{
				imageResult.mismatchFlags |= SENTINEL_VERIFICATION_MISMATCH_COEFFICIENTS;
				result.passed = 0;
			}
		}

		char verificationMessage[512] = {};
		sprintf_s(verificationMessage, "Reference verification: flags=0x%X, coefficientMax=%.12g, offsetA=%.12g, offsetR=%.12g, slcMax=%.12g, slcRms=%.12g.",
			imageResult.mismatchFlags, imageResult.maxCoefficientAbsError, imageResult.esdOffsetAbsError,
			imageResult.rangeOffsetAbsError, imageResult.maxSlcComponentAbsError, imageResult.rmsSlcComponentError);
		emit_diagnostic(imageResult.mismatchFlags == SENTINEL_VERIFICATION_MISMATCH_NONE ? INSAR_DIAGNOSTIC_INFO : INSAR_DIAGNOSTIC_WARNING,
			"verification", "reference.compare", verificationMessage, "Reference H5 is compared inside the DLL; all tolerances are caller-supplied.",
			outFiles[image].c_str(), "s_re", static_cast<int>(imageResult.mismatchFlags), imageResult.outputRows, imageResult.outputColumns, CV_64F);
	}
	return 0;
}

int Sentinel1BackGeocoding::recoverPostRegistrationRefinementTransaction()
{
	if (outFiles.empty()) return kRefinementContractError;
	const std::string directory = refinement_directory(outFiles.front());
	for (const std::string& outputPath : outFiles)
		if (_stricmp(refinement_directory(outputPath).c_str(), directory.c_str()) != 0) return kRefinementContractError;
	const std::string manifestPath = directory + "\\refinement_transaction.json";
	if (GetFileAttributesA(manifestPath.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;

	std::ifstream manifest(manifestPath, std::ios::binary);
	if (!manifest) return kRefinementTransactionError;
	const std::string json((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
	std::string state;
	std::string transactionId;
	if (!refinement_manifest_value(json, "state", state) || !refinement_manifest_value(json, "transactionId", transactionId) ||
		(state != "in_progress" && state != "failed")) return kRefinementTransactionError;

	std::vector<RefinementTransactionFile> files;
	files.reserve(outFiles.size());
	const bool hasFullBurstSource = json.find("\"fullBurst\"") != std::string::npos;
	const bool hasFullBurstTemporary = json.find("\"fullBurstTemporary\"") != std::string::npos;
	if (hasFullBurstSource != hasFullBurstTemporary) return kRefinementTransactionError;
	for (const std::string& outputPath : outFiles)
	{
		RefinementTransactionFile file = {
			outputPath,
			outputPath + ".refinement-" + transactionId + ".tmp",
			outputPath + ".fullburst",
			outputPath + ".refinement-" + transactionId + ".fullburst.tmp",
			outputPath + ".refinement-" + transactionId + ".bak",
			true };
		if (json.find(std::string("\"output\": \"") + json_escape(file.outputPath) + "\"") == std::string::npos ||
			json.find(std::string("\"temporary\": \"") + json_escape(file.temporaryPath) + "\"") == std::string::npos ||
			json.find(std::string("\"backup\": \"") + json_escape(file.backupPath) + "\"") == std::string::npos ||
			(hasFullBurstSource && json.find(std::string("\"fullBurst\": \"") +
				json_escape(file.fullBurstPath) + "\"") == std::string::npos) ||
			(hasFullBurstTemporary && json.find(std::string("\"fullBurstTemporary\": \"") +
				json_escape(file.fullBurstTemporaryPath) + "\"") == std::string::npos))
			return kRefinementTransactionError;
		const std::string outputToken = std::string("\"output\": \"") + json_escape(file.outputPath) + "\"";
		const std::string::size_type outputPosition = json.find(outputToken);
		const std::string::size_type outputEnd = outputPosition == std::string::npos ? std::string::npos : json.find('}', outputPosition);
		if (outputEnd != std::string::npos && json.substr(outputPosition, outputEnd - outputPosition).find("\"hadOriginalOutput\": false") != std::string::npos)
			file.hadOriginalOutput = false;
		files.push_back(file);
	}

	for (const RefinementTransactionFile& file : files)
	{
		const bool backupExists = GetFileAttributesA(file.backupPath.c_str()) != INVALID_FILE_ATTRIBUTES;
		const bool outputExists = GetFileAttributesA(file.outputPath.c_str()) != INVALID_FILE_ATTRIBUTES;
		if (backupExists)
		{
			if (outputExists && !DeleteFileA(file.outputPath.c_str())) return kRefinementTransactionError;
			if (!MoveFileExA(file.backupPath.c_str(), file.outputPath.c_str(), MOVEFILE_WRITE_THROUGH)) return kRefinementTransactionError;
		}
		else if (!file.hadOriginalOutput)
		{
			if (outputExists && !DeleteFileA(file.outputPath.c_str())) return kRefinementTransactionError;
		}
		else if (!outputExists)
		{
			return kRefinementTransactionError;
		}
		if (GetFileAttributesA(file.temporaryPath.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileA(file.temporaryPath.c_str()))
			return kRefinementTransactionError;
		if (GetFileAttributesA(file.fullBurstTemporaryPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
			!DeleteFileA(file.fullBurstTemporaryPath.c_str()))
			return kRefinementTransactionError;
		// A manifest that explicitly names a full-burst source belongs to the
		// interrupted refinement run.  It is not a published result and must not
		// survive a successful rollback.
		if (hasFullBurstSource && GetFileAttributesA(file.fullBurstPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
			!DeleteFileA(file.fullBurstPath.c_str()))
			return kRefinementTransactionError;
	}

	for (const RefinementTransactionFile& file : files)
	{
		if (GetFileAttributesA(file.outputPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
		const int stateResult = Hdf5IO::removeDatasetIfPresent(file.outputPath.c_str(), "refinement_state");
		const int idResult = Hdf5IO::removeDatasetIfPresent(file.outputPath.c_str(), "refinement_transaction_id");
		if (stateResult < 0 || idResult < 0) return kRefinementTransactionError;
	}
	return DeleteFileA(manifestPath.c_str()) ? 0 : kRefinementTransactionError;
}


























/*
orbitStateVectors::orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime)
{
	this->startTime = startTime;
	this->stopTime = stopTime;
	this->isOrbitUpdated = false;
	stateVectors.copyTo(this->stateVectors);
	if (this->stateVectors.type() != CV_64F) this->stateVectors.convertTo(this->stateVectors, CV_64F);
	double delta_time = stateVectors.at<double>(1, 0) - stateVectors.at<double>(0, 0);
	if (fabs(delta_time) <= 1.0)
	{
		this->isOrbitUpdated = true;//�����ٸ��¹��
		this->stateVectors.copyTo(this->newStateVectors);
	}
	this->dt = delta_time;
	setSceneStartStopTime(startTime, stopTime);

}

orbitStateVectors::orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime, double delta_time)
{
	this->dt = delta_time;
	this->isOrbitUpdated = false;
	stateVectors.copyTo(this->stateVectors);
	if (this->stateVectors.type() != CV_64F) this->stateVectors.convertTo(this->stateVectors, CV_64F);
	if (fabs(delta_time) <= 1.0)
	{
		this->isOrbitUpdated = true;//�����ٸ��¹��
		this->stateVectors.copyTo(this->newStateVectors);
	}



	setSceneStartStopTime(startTime, stopTime);
}

orbitStateVectors::~orbitStateVectors()
{
}

int orbitStateVectors::setSceneStartStopTime(double startTime, double stopTime)
{
	this->startTime = startTime;
	this->stopTime = stopTime;
	return 0;
}

double orbitStateVectors::get_start_time()
{
	return startTime;
}

double orbitStateVectors::get_stop_time()
{
	return stopTime;
}

int orbitStateVectors::getPosition(double azimuthTime, Position& position)
{
	if (newStateVectors.cols != 7 || newStateVectors.rows < 2 || !isOrbitUpdated)
	{
		fprintf(stderr, "getPosition(): input check failed!\n");
		return -1;
	}
	if (azimuthTime < newStateVectors.at<double>(0, 0) || azimuthTime > newStateVectors.at<double>(newStateVectors.rows - 1, 0))
	{
		fprintf(stderr, "getPosition(): azimuthTime out of legal range\n");
		return -1;
	}
	int i0, iN;
	if (newStateVectors.rows <= nv) {
		i0 = 0;
		iN = newStateVectors.rows - 1;
	}
	else {
		i0 = max((int)((azimuthTime - newStateVectors.at<double>(0, 0)) / dt) - nv / 2 + 1, 0);
		iN = min(i0 + nv - 1, newStateVectors.rows - 1);
		i0 = (iN < newStateVectors.rows - 1 ? i0 : iN - nv + 1);
	}
	position.x = 0.0;
	position.y = 0.0;
	position.z = 0.0;
	for (int i = i0; i <= iN; ++i) {
		double weight = 1;
		for (int j = i0; j <= iN; ++j) {
			if (j != i) {
				double time2 = newStateVectors.at<double>(j, 0);
				weight *= (azimuthTime - time2) / (newStateVectors.at<double>(i, 0) - time2);
			}
		}
		position.x += weight * newStateVectors.at<double>(i, 1);
		position.y += weight * newStateVectors.at<double>(i, 2);
		position.z += weight * newStateVectors.at<double>(i, 3);
	}
	return 0;
}

int orbitStateVectors::getVelocity(double azimuthTime, Velocity& velocity)
{
	if (newStateVectors.cols != 7 || newStateVectors.rows < 2 || !isOrbitUpdated)
	{
		fprintf(stderr, "getVelocity(): input check failed!\n");
		return -1;
	}
	if (azimuthTime < newStateVectors.at<double>(0, 0) || azimuthTime > newStateVectors.at<double>(newStateVectors.rows - 1, 0))
	{
		fprintf(stderr, "getVelocity(): azimuthTime out of legal range\n");
		return -1;
	}
	int i0, iN;
	if (newStateVectors.rows <= nv) {
		i0 = 0;
		iN = newStateVectors.rows - 1;
	}
	else {
		i0 = max((int)((azimuthTime - newStateVectors.at<double>(0, 0)) / dt) - nv / 2 + 1, 0);
		iN = min(i0 + nv - 1, newStateVectors.rows - 1);
		i0 = (iN < newStateVectors.rows - 1 ? i0 : iN - nv + 1);
	}
	velocity.vx = 0.0;
	velocity.vy = 0.0;
	velocity.vz = 0.0;
	for (int i = i0; i <= iN; ++i) {
		double weight = 1.0;
		for (int j = i0; j <= iN; ++j) {
			if (j != i) {
				double time2 = newStateVectors.at<double>(j, 0);
				weight *= (azimuthTime - time2) / (newStateVectors.at<double>(i, 0) - time2);
			}
		}
		velocity.vx += weight * newStateVectors.at<double>(i, 4);
		velocity.vy += weight * newStateVectors.at<double>(i, 5);
		velocity.vz += weight * newStateVectors.at<double>(i, 6);
	}
	return 0;
}

int orbitStateVectors::getOrbitData(double time, OSV* osv)
{
	if (!osv || time < 0 || stateVectors.empty()|| isOrbitUpdated)
	{
		fprintf(stderr, "getOrbitData(): input check failed!\n");
		return -1;
	}
	int ret;
	int numVectors = stateVectors.rows;
	double t0 = stateVectors.at<double>(0, 0);
	double tN = stateVectors.at<double>(numVectors - 1, 0);

	int numVecPolyFit = polyDegree + 1; //4;
	int halfNumVecPolyFit = numVecPolyFit / 2;
	Mat vectorIndices = Mat::zeros(1, numVecPolyFit, CV_32S);
	int vecIdx = (int)((time - t0) / (tN - t0) * (numVectors - 1));
	if (vecIdx <= halfNumVecPolyFit - 1) {
		for (int i = 0; i < numVecPolyFit; i++) {
			vectorIndices.at<int>(0, i) = i;
		}
	}
	else if (vecIdx >= numVectors - halfNumVecPolyFit) {
		for (int i = 0; i < numVecPolyFit; i++) {
			vectorIndices.at<int>(0, i) = numVectors - numVecPolyFit + i;
		}
	}
	else {
		for (int i = 0; i < numVecPolyFit; i++) {
			vectorIndices.at<int>(0, i) = vecIdx - halfNumVecPolyFit + 1 + i;
		}
	}

	Mat timeArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat xPosArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat yPosArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat zPosArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat xVelArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat yVelArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat zVelArray = Mat::zeros(numVecPolyFit, 1, CV_64F);


	FormatConversion::createVandermondeMatrix(timeArray, A, polyDegree);
	ret = FormatConversion::polyFit(A, xPosArray, xPosCoeff);
	if (ret < 0) return -1;
	ret = FormatConversion::polyFit(A, yPosArray, yPosCoeff);
	if (ret < 0) return -1;
	ret = FormatConversion::polyFit(A, zPosArray, zPosCoeff);
	if (ret < 0) return -1;
	ret = FormatConversion::polyFit(A, xVelArray, xVelCoeff);
	if (ret < 0) return -1;
	ret = FormatConversion::polyFit(A, yVelArray, yVelCoeff);
	if (ret < 0) return -1;
	ret = FormatConversion::polyFit(A, zVelArray, zVelCoeff);
	if (ret < 0) return -1;
	double normalizedTime = time - t0;

	osv->time = time;
	ret = FormatConversion::polyVal(xPosCoeff, normalizedTime, &osv->x);
	if (ret < 0) return -1;
	ret = FormatConversion::polyVal(yPosCoeff, normalizedTime, &osv->y);
	if (ret < 0) return -1;
	ret = FormatConversion::polyVal(zPosCoeff, normalizedTime, &osv->z);
	if (ret < 0) return -1;
	ret = FormatConversion::polyVal(xVelCoeff, normalizedTime, &osv->vx);
	if (ret < 0) return -1;
	ret = FormatConversion::polyVal(yVelCoeff, normalizedTime, &osv->vy);
	if (ret < 0) return -1;
	ret = FormatConversion::polyVal(zVelCoeff, normalizedTime, &osv->vz);
	if (ret < 0) return -1;
	return 0;
}

int orbitStateVectors::applyOrbit(ProgressCallback progressCallback, void* userData)
{
	if (progressCallback && !progressCallback(0, "Updating orbit state vectors...", userData)) return -2;
	if (isOrbitUpdated) return 0;
	double delta_t = 1.0;//1.0s
	this->dt = delta_t;
	double extra = 10.0;//10.0s
	double start = startTime - extra;
	double stop = stopTime + extra;
	int numVectors = (int)((stop - start) / delta_t);
	OSV osv;
	Mat newStateVectors = Mat::zeros(numVectors, 7, CV_64F);
	int ret;
	for (int i = 0; i < numVectors; i++)
	{
		if (progressCallback && i % 16 == 0 &&
			!progressCallback(i * 100 / std::max(1, numVectors), "Updating orbit state vectors...", userData)) return -2;
		ret = getOrbitData(start + (double)i * delta_t, &osv);
		if (ret < 0) return -1;
		newStateVectors.at<double>(i, 0) = start + (double)i * delta_t;
		newStateVectors.at<double>(i, 1) = osv.x;
		newStateVectors.at<double>(i, 2) = osv.y;
		newStateVectors.at<double>(i, 3) = osv.z;
		newStateVectors.at<double>(i, 4) = osv.vx;
		newStateVectors.at<double>(i, 5) = osv.vy;
		newStateVectors.at<double>(i, 6) = osv.vz;


	}
	newStateVectors.copyTo(this->newStateVectors);
	isOrbitUpdated = true;
	if (progressCallback && !progressCallback(100, "Orbit state vectors updated.", userData)) return -2;
	return 0;
}

bool orbitStateVectors::findZeroDopplerTime(
	orbitStateVectors& stateVectors,
	const Position& groundPosition,
	double wavelength,
	double time_interval,
	double dopplerFrequency,
	double& zeroDopplerTime,
	double& distance,
	double dopplerThreshold)
{
	int numOrbitVec = stateVectors.newStateVectors.rows;
	double firstVecTime = 0.0;
	double secondVecTime = 0.0;
	double firstVecFreq = 0.0;
	double secondVecFreq = 0.0;
	double currentFreq, xdiff, ydiff, zdiff;

	for (int ii = 0; ii < numOrbitVec; ii++) {
		Position orb_pos(stateVectors.newStateVectors.at<double>(ii, 1), stateVectors.newStateVectors.at<double>(ii, 2),
			stateVectors.newStateVectors.at<double>(ii, 3));
		Velocity orb_vel(stateVectors.newStateVectors.at<double>(ii, 4), stateVectors.newStateVectors.at<double>(ii, 5),
			stateVectors.newStateVectors.at<double>(ii, 6));
		xdiff = groundPosition.x - orb_pos.x;
		ydiff = groundPosition.y - orb_pos.y;
		zdiff = groundPosition.z - orb_pos.z;
		double dist = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
		currentFreq = 2.0 * (xdiff * orb_vel.vx + ydiff * orb_vel.vy + zdiff * orb_vel.vz) / (wavelength * dist);
		if (ii == 0 || (firstVecFreq - dopplerFrequency) * (currentFreq - dopplerFrequency) > 0) {
			firstVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
			firstVecFreq = currentFreq;
		}
		else {
			secondVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
			secondVecFreq = currentFreq;
			break;
		}
	}

	if ((firstVecFreq - dopplerFrequency) * (secondVecFreq - dopplerFrequency) >= 0.0) {
		return false;
	}

	double lowerBoundTime = firstVecTime;
	double upperBoundTime = secondVecTime;
	double lowerBoundFreq = firstVecFreq;
	double upperBoundFreq = secondVecFreq;
	double midTime, midFreq;
	double diffTime = fabs(upperBoundTime - lowerBoundTime);
	double absLineTimeInterval = time_interval;

	int totalIterations = (int)(diffTime / absLineTimeInterval) + 1;
	int numIterations = 0;
	Position pos; Velocity vel;
	while (diffTime > absLineTimeInterval * 0.01 && numIterations <= totalIterations) {
		midTime = (upperBoundTime + lowerBoundTime) / 2.0;
		stateVectors.getPosition(midTime, pos);
		stateVectors.getVelocity(midTime, vel);
		xdiff = groundPosition.x - pos.x;
		ydiff = groundPosition.y - pos.y;
		zdiff = groundPosition.z - pos.z;
		double dist = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
		midFreq = 2.0 * (xdiff * vel.vx + ydiff * vel.vy + zdiff * vel.vz) / (wavelength * dist);
		if ((midFreq - dopplerFrequency) * (lowerBoundFreq - dopplerFrequency) > 0.0) {
			lowerBoundTime = midTime;
			lowerBoundFreq = midFreq;
		}
		else if ((midFreq - dopplerFrequency) * (upperBoundFreq - dopplerFrequency) > 0.0) {
			upperBoundTime = midTime;
			upperBoundFreq = midFreq;
		}
		else if (fabs(midFreq - dopplerFrequency) < dopplerThreshold) {
			lowerBoundTime = midTime;
			break;
		}
		diffTime = fabs(upperBoundTime - lowerBoundTime);
		numIterations++;
	}

	zeroDopplerTime = lowerBoundTime + (dopplerFrequency - lowerBoundFreq) * (upperBoundTime - lowerBoundTime) / (upperBoundFreq - lowerBoundFreq);
	stateVectors.getPosition(zeroDopplerTime, pos);
	xdiff = groundPosition.x - pos.x;
	ydiff = groundPosition.y - pos.y;
	zdiff = groundPosition.z - pos.z;
	distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	return true;
}

*/
