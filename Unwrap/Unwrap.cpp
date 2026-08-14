// Unwrap.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include"..\include\Unwrap.h"
#include"..\include\FormatConversion.h"
#include<tchar.h>
#include <atlconv.h>
#include<queue>
#include <atomic>
#include <cerrno>
#include <climits>
#include <limits>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>

#ifdef _DEBUG
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#pragma comment(lib, "FormatConversion_d.lib")
#else
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "FormatConversion.lib")
#endif // _DEBUG
using namespace cv;

#include <vector>
#include <ctime>
#include <string>

	namespace {
	thread_local UnwrapDiagnostic* g_activeDiagnostic = nullptr;

	struct SnaphuRunContext
	{
		SnaphuRunOptionsV1 options = {};
		SnaphuRunEventCallbackV1 callback = nullptr;
		void* userData = nullptr;
	};

	thread_local SnaphuRunContext* g_activeSnaphuRun = nullptr;

	bool normalizeSnaphuOptions(const SnaphuRunOptionsV1* supplied, SnaphuRunOptionsV1& normalized)
	{
		memset(&normalized, 0, sizeof(normalized));
		normalized.structSize = sizeof(normalized);
		normalized.version = 1;
		normalized.tileRows = 1;
		normalized.tileCols = 1;
		normalized.requestedProcessCount = 1;
		normalized.heartbeatMilliseconds = 1000;
		if (!supplied) return true;
		if (supplied->version != 1 || supplied->structSize < sizeof(SnaphuRunOptionsV1)) return false;
		normalized = *supplied;
		if ((normalized.flags & ~SNAPHU_RUN_OPTION_KEEP_ARTIFACTS_ON_SUCCESS) != 0 || normalized.reserved0 != 0) return false;
		for (size_t i = 0; i < sizeof(normalized.reserved) / sizeof(normalized.reserved[0]); ++i)
			if (normalized.reserved[i] != 0) return false;
		if (normalized.tileRows == 0 || normalized.tileRows > 256 || normalized.tileCols == 0 ||
			normalized.tileCols > 256 || normalized.requestedProcessCount == 0 || normalized.requestedProcessCount > 256) return false;
		if ((normalized.tileRows == 1 && normalized.tileCols == 1) &&
			(normalized.rowOverlap != 0 || normalized.colOverlap != 0)) return false;
		if ((normalized.tileRows > 1 || normalized.tileCols > 1) &&
			(normalized.rowOverlap < 400 || normalized.colOverlap < 400)) return false;
		if (normalized.heartbeatMilliseconds == 0) normalized.heartbeatMilliseconds = 1000;
		if (normalized.heartbeatMilliseconds < 100 || normalized.heartbeatMilliseconds > 60000) return false;
		const uint64_t maximumTimeout = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
		if (normalized.wallTimeoutMilliseconds != 0 &&
			(normalized.wallTimeoutMilliseconds < 1000 || normalized.wallTimeoutMilliseconds > maximumTimeout)) return false;
		return true;
	}

	class ScopedSnaphuRunContext
	{
	public:
		ScopedSnaphuRunContext(const SnaphuRunOptionsV1& options, SnaphuRunEventCallbackV1 callback, void* userData)
			: previous_(g_activeSnaphuRun)
		{
			context_.options = options;
			context_.callback = callback;
			context_.userData = userData;
			g_activeSnaphuRun = &context_;
		}
		~ScopedSnaphuRunContext() { g_activeSnaphuRun = previous_; }
	private:
		SnaphuRunContext context_;
		SnaphuRunContext* previous_;
	};

	void copyDiagnosticText(char* destination, size_t capacity, const std::string& text)
	{
		if (!destination || capacity == 0) return;
		const size_t length = std::min(capacity - 1, text.size());
		memcpy(destination, text.data(), length);
		destination[length] = '\0';
	}

	bool initializeDiagnostic(UnwrapDiagnostic* diagnostic, uint32_t algorithm)
	{
		if (!diagnostic || diagnostic->structSize < sizeof(UnwrapDiagnostic)) return false;
		memset(diagnostic, 0, sizeof(UnwrapDiagnostic));
		diagnostic->structSize = sizeof(UnwrapDiagnostic);
		diagnostic->algorithm = algorithm;
		diagnostic->stage = UNWRAP_DIAGNOSTIC_STAGE_INTERNAL;
		diagnostic->operationStatus = -1;
		diagnostic->exitCode = STILL_ACTIVE;
		return true;
	}

	class ScopedPublicDiagnostic
	{
	public:
		ScopedPublicDiagnostic(UnwrapDiagnostic* diagnostic, uint32_t algorithm)
			: previous_(g_activeDiagnostic), current_(initializeDiagnostic(diagnostic, algorithm) ? diagnostic : nullptr)
		{
			g_activeDiagnostic = current_;
		}

		~ScopedPublicDiagnostic() { g_activeDiagnostic = previous_; }

		void finish(int status)
		{
			if (!current_) return;
			current_->operationStatus = status;
			if (status == 0 && current_->summary[0] == '\0')
			{
				current_->stage = UNWRAP_DIAGNOSTIC_STAGE_COMPLETED;
				copyDiagnosticText(current_->summary, sizeof(current_->summary), "completed");
			}
			else if (current_->summary[0] == '\0')
			{
				copyDiagnosticText(current_->summary, sizeof(current_->summary),
					"operation failed before an external diagnostic was available");
			}
		}

	private:
		UnwrapDiagnostic* previous_;
		UnwrapDiagnostic* current_;
	};

	std::string redactExternalText(const std::string& text)
	{
		std::string result;
		result.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			const bool drivePath = i + 2 < text.size() &&
				((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')) &&
				text[i + 1] == ':' && (text[i + 2] == '\\' || text[i + 2] == '/');
			const bool uncPath = i + 1 < text.size() &&
				((text[i] == '\\' && text[i + 1] == '\\') || (text[i] == '/' && text[i + 1] == '/'));
			if (!drivePath && !uncPath)
			{
				result.push_back(text[i++]);
				continue;
			}
			result += "<path>";
			i += drivePath ? 3 : 2;
			while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' &&
				text[i] != '\"' && text[i] != '\'') ++i;
		}
		return result;
	}

	void appendBounded(std::string& target, const char* data, size_t length, size_t capacity)
	{
		if (!data || length == 0 || capacity == 0) return;
		if (length >= capacity)
		{
			target.assign(data + length - capacity, capacity);
			return;
		}
		if (target.size() + length > capacity) target.erase(0, target.size() + length - capacity);
		target.append(data, length);
	}

	void drainPipe(HANDLE pipe, std::string& tail, std::string* captured = nullptr)
	{
		if (!pipe || pipe == INVALID_HANDLE_VALUE) return;
		while (true)
		{
			DWORD available = 0;
			if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) return;
			char buffer[512];
			DWORD read = 0;
			const DWORD requested = std::min<DWORD>(available, sizeof(buffer));
			if (!ReadFile(pipe, buffer, requested, &read, nullptr) || read == 0) return;
			appendBounded(tail, buffer, read, 2048);
			if (captured) appendBounded(*captured, buffer, read, 480);
		}
	}

	void drainStderrPipe(HANDLE pipe, std::string& tail)
	{
		drainPipe(pipe, tail);
	}

	struct ExternalToolResult
	{
		std::string tool;
		std::string phase;
		DWORD win32Error = ERROR_SUCCESS;
		DWORD exitCode = STILL_ACTIVE;
		bool cancelled = false;
		bool timedOut = false;
		bool terminationUncertain = false;
		std::string validationFailure;
		std::string stderrTail;
		std::vector<std::wstring> managedArtifacts;
		std::vector<std::pair<std::wstring, DWORD>> cleanupResiduals;

		~ExternalToolResult()
		{
			if (!g_activeDiagnostic || (!cancelled && !timedOut && !terminationUncertain && win32Error == ERROR_SUCCESS &&
				(exitCode == STILL_ACTIVE || exitCode == 0) && validationFailure.empty() && cleanupResiduals.empty())) return;
			UnwrapDiagnostic& diagnostic = *g_activeDiagnostic;
			if (tool == "MCF") copyDiagnosticText(diagnostic.tool, sizeof(diagnostic.tool), "MCF");
			else if (tool == "SNAPHU") copyDiagnosticText(diagnostic.tool, sizeof(diagnostic.tool), "SNAPHU");
			else copyDiagnosticText(diagnostic.tool, sizeof(diagnostic.tool), tool);
			if (phase == "path conversion") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_PATH;
			else if (phase == "prepare solution") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_PREPARE;
			else if (phase == "create job" || phase == "configure job" || phase == "bind job" || phase == "resume process") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_JOB;
			else if (phase == "cancel" || phase == "timeout") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_CANCEL;
			else if (phase == "process exit") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_PROCESS_EXIT;
			else if (phase == "validate solution" || phase == "validate output" || phase == "record solution" || phase == "publish solution") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_OUTPUT;
			else if (phase == "launch") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_LAUNCH;
			else if (phase == "input") diagnostic.stage = UNWRAP_DIAGNOSTIC_STAGE_INPUT;
			diagnostic.win32Error = win32Error;
			diagnostic.exitCode = exitCode;
			diagnostic.cancelled = cancelled ? 1 : 0;
			if (!validationFailure.empty()) copyDiagnosticText(diagnostic.summary, sizeof(diagnostic.summary), validationFailure);
			else if (terminationUncertain) copyDiagnosticText(diagnostic.summary, sizeof(diagnostic.summary), "external tool termination could not be confirmed");
			else if (timedOut) copyDiagnosticText(diagnostic.summary, sizeof(diagnostic.summary), "external tool timed out");
			else if (cancelled) copyDiagnosticText(diagnostic.summary, sizeof(diagnostic.summary), "external tool cancelled");
			else if (!cleanupResiduals.empty()) copyDiagnosticText(diagnostic.summary, sizeof(diagnostic.summary), "external tool cleanup left managed artifacts");
			else copyDiagnosticText(diagnostic.summary, sizeof(diagnostic.summary), "external tool process failed");
			copyDiagnosticText(diagnostic.stderrTail, sizeof(diagnostic.stderrTail), redactExternalText(stderrTail));
		}
	};

	bool emitSnaphuRunEvent(uint32_t type, const std::string& message, ULONGLONG startedAt, HANDLE job)
	{
		if (!g_activeSnaphuRun || !g_activeSnaphuRun->callback) return true;
		SnaphuRunEventV1 event = {};
		event.structSize = sizeof(event);
		event.version = 1;
		event.type = type;
		// Windows SNAPHU currently forces tile workers to one process.
		event.effectiveProcessCount = 1;
		event.elapsedMilliseconds = GetTickCount64() - startedAt;
		if (job)
		{
			JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION accounting = {};
			if (QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation,
				&accounting, sizeof(accounting), nullptr))
			{
				event.totalCpuMilliseconds = static_cast<uint64_t>((accounting.BasicInfo.TotalUserTime.QuadPart +
					accounting.BasicInfo.TotalKernelTime.QuadPart) / 10000);
				event.readBytes = accounting.IoInfo.ReadTransferCount;
				event.writeBytes = accounting.IoInfo.WriteTransferCount;
				event.metricAvailability |= SNAPHU_RUN_METRIC_CPU_TIME | SNAPHU_RUN_METRIC_READ_BYTES |
					SNAPHU_RUN_METRIC_WRITE_BYTES;
			}
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
			if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
				&limits, sizeof(limits), nullptr))
			{
				event.peakJobMemoryBytes = static_cast<uint64_t>(limits.PeakJobMemoryUsed);
				event.metricAvailability |= SNAPHU_RUN_METRIC_PEAK_JOB_MEMORY;
			}
		}
		copyDiagnosticText(event.message, sizeof(event.message), message);
		try
		{
			return g_activeSnaphuRun->callback(&event, g_activeSnaphuRun->userData);
		}
		catch (...)
		{
			return false;
		}
	}

	bool drainSnaphuPipe(HANDLE pipe, std::string& tail, const char* source, ULONGLONG startedAt, HANDLE job)
	{
		std::string captured;
		drainPipe(pipe, tail, &captured);
		if (captured.empty()) return true;
		return emitSnaphuRunEvent(SNAPHU_RUN_EVENT_LOG, std::string(source) + ": " + captured, startedAt, job);
	}

	bool absolutePath(const std::wstring& path, std::wstring& absolute)
	{
		const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
		if (required == 0) return false;
		std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1, L'\0');
		const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
		if (length == 0 || length >= buffer.size()) return false;
		absolute.assign(buffer.data(), length);
		return true;
	}

	bool emitSnaphuPreparedEvent(const std::wstring& taskDirectory, const std::wstring& configPath)
	{
		if (!g_activeSnaphuRun || !g_activeSnaphuRun->callback) return true;
		std::wstring absoluteTask;
		std::wstring absoluteConfig;
		PathResolver::Error error = PathResolver::Error::None;
		std::string taskUtf8;
		std::string configUtf8;
		if (!absolutePath(taskDirectory, absoluteTask) || !absolutePath(configPath, absoluteConfig) ||
			!PathResolver::wideToUtf8(absoluteTask, taskUtf8, &error) ||
			!PathResolver::wideToUtf8(absoluteConfig, configUtf8, &error) ||
			taskUtf8.size() >= SNAPHU_RUN_PATH_CAPACITY ||
			configUtf8.size() >= SNAPHU_RUN_PATH_CAPACITY) return false;
		SnaphuRunEventV1 event = {};
		event.structSize = sizeof(event);
		event.version = 1;
		event.type = SNAPHU_RUN_EVENT_PREPARED;
		event.effectiveProcessCount = 1;
		copyDiagnosticText(event.taskDirectory, sizeof(event.taskDirectory), taskUtf8);
		copyDiagnosticText(event.configPath, sizeof(event.configPath), configUtf8);
		copyDiagnosticText(event.message, sizeof(event.message), "SNAPHU staging and config are ready.");
		try
		{
			return g_activeSnaphuRun->callback(&event, g_activeSnaphuRun->userData);
		}
		catch (...)
		{
			return false;
		}
	}

	std::wstring quoteCommandArgument(const std::wstring& argument)
	{
		std::wstring quoted = L"\"";
		size_t slashCount = 0;
		for (const wchar_t ch : argument)
		{
			if (ch == L'\\') { ++slashCount; continue; }
			if (ch == L'\"') quoted.append(slashCount * 2 + 1, L'\\');
			else quoted.append(slashCount, L'\\');
			quoted.push_back(ch);
			slashCount = 0;
		}
		quoted.append(slashCount * 2, L'\\');
		quoted.push_back(L'\"');
		return quoted;
	}

	bool quoteSnaphuConfigPath(const std::string& path, std::string& quoted)
	{
		if (path.empty() || path.find('\0') != std::string::npos || path.find_first_of("\"\r\n") != std::string::npos) return false;
		quoted = "\"" + path + "\"";
		return true;
	}

	struct DimacsArc
	{
		long tail;
		long head;
		long long lower;
		long long upper;
		long long cost;
	};

	bool readToolTextFile(const std::string& utf8Path, std::string& text)
	{
		std::wstring path;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(utf8Path, path, &error)) return false;
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size = {};
		if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 64LL * 1024 * 1024)
		{
			CloseHandle(file);
			return false;
		}
		text.resize(static_cast<size_t>(size.QuadPart));
		DWORD total = 0;
		while (total < text.size())
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - total, 0x7ffff000));
			DWORD read = 0;
			if (!ReadFile(file, &text[total], chunk, &read, nullptr) || read != chunk) { CloseHandle(file); return false; }
			total += read;
		}
		const bool closed = CloseHandle(file) != FALSE;
		return closed && text.find('\0') == std::string::npos;
	}

	bool parseDimacsInteger(const std::string& value, long long& output)
	{
		if (value.empty()) return false;
		char* end = nullptr;
		errno = 0;
		output = _strtoi64(value.c_str(), &end, 10);
		return errno != ERANGE && end && *end == '\0';
	}

	bool parseDimacsNetwork(const std::string& text, std::vector<long long>& supply,
		std::vector<DimacsArc>& arcs, std::map<std::pair<long, long>, size_t>& arcIndex, std::string& reason)
	{
		std::istringstream input(text);
		std::string line;
		long nodes = 0;
		long expectedArcs = 0;
		bool gotProblem = false;
		std::set<long> suppliedNodes;
		while (std::getline(input, line))
		{
			std::istringstream fields(line);
			char type = 0;
			if (!(fields >> type) || type == 'c') continue;
			if (type == 'p')
			{
				std::string kind, extra;
				if (gotProblem || !(fields >> kind >> nodes >> expectedArcs) || (fields >> extra) || kind != "min" || nodes < 1 || expectedArcs < 0)
				{ reason = "invalid DIMACS problem line"; return false; }
				gotProblem = true;
				supply.assign(static_cast<size_t>(nodes) + 1, 0);
				arcs.reserve(static_cast<size_t>(expectedArcs));
			}
			else if (type == 'n')
			{
				long node = 0; std::string value, extra; long long amount = 0;
				if (!gotProblem || !(fields >> node >> value) || (fields >> extra) || node < 1 || node > nodes || !parseDimacsInteger(value, amount) ||
					amount < LONG_MIN || amount > LONG_MAX || !suppliedNodes.insert(node).second)
				{ reason = "invalid DIMACS supply"; return false; }
				supply[node] = amount;
			}
			else if (type == 'a')
			{
				DimacsArc arc = {}; std::string lower, upper, cost, extra;
				if (!gotProblem || !(fields >> arc.tail >> arc.head >> lower >> upper >> cost) || (fields >> extra) ||
					arc.tail < 1 || arc.tail > nodes || arc.head < 1 || arc.head > nodes || !parseDimacsInteger(lower, arc.lower) ||
					!parseDimacsInteger(upper, arc.upper) || !parseDimacsInteger(cost, arc.cost) || arc.lower < 0 || arc.upper < arc.lower || arc.cost < 0 ||
					arc.upper > LONG_MAX || arc.cost > LONG_MAX || arcs.size() >= static_cast<size_t>(expectedArcs) ||
					!arcIndex.emplace(std::make_pair(arc.tail, arc.head), arcs.size()).second)
				{ reason = "invalid or duplicate DIMACS arc"; return false; }
				arcs.push_back(arc);
			}
			else { reason = "unknown DIMACS record"; return false; }
		}
		if (!gotProblem || arcs.size() != static_cast<size_t>(expectedArcs)) { reason = "incomplete DIMACS network"; return false; }
		long long total = 0;
		for (size_t i = 1; i < supply.size(); ++i) { if ((supply[i] > 0 && total > LLONG_MAX - supply[i]) || (supply[i] < 0 && total < LLONG_MIN - supply[i])) { reason = "supply overflow"; return false; } total += supply[i]; }
		if (total != 0) { reason = "unbalanced DIMACS network"; return false; }
		return true;
	}

	bool validateMcfSolution(const std::string& networkPath, const std::string& solutionPath, std::string& reason)
	{
		std::string networkText; std::string solutionText;
		if (!readToolTextFile(networkPath, networkText) || !readToolTextFile(solutionPath, solutionText)) { reason = "cannot read DIMACS artifact"; return false; }
		std::vector<long long> supply; std::vector<DimacsArc> arcs; std::map<std::pair<long, long>, size_t> arcIndex;
		if (!parseDimacsNetwork(networkText, supply, arcs, arcIndex, reason)) return false;
		std::vector<long long> flow(arcs.size(), 0);
		std::set<std::pair<long, long>> seen;
		std::istringstream input(solutionText); std::string line; bool sawStatus = false; long long declaredObjective = -1;
		while (std::getline(input, line))
		{
			std::istringstream fields(line); char type = 0;
			if (!(fields >> type) || type == 'c') continue;
			std::string first, second, third, extra;
			if (type == 's')
			{
				if (sawStatus || !(fields >> first) || (fields >> extra) || !parseDimacsInteger(first, declaredObjective) || declaredObjective < 0) { reason = "invalid solution objective"; return false; }
				sawStatus = true;
			}
			else if (type == 'f')
			{
				long long tail = 0, head = 0, value = 0;
				if (!sawStatus || !(fields >> first >> second >> third) || (fields >> extra) || !parseDimacsInteger(first, tail) || !parseDimacsInteger(second, head) || !parseDimacsInteger(third, value) ||
					tail < LONG_MIN || tail > LONG_MAX || head < LONG_MIN || head > LONG_MAX || value <= 0) { reason = "invalid solution flow"; return false; }
				const std::pair<long, long> key(static_cast<long>(tail), static_cast<long>(head));
				auto arc = arcIndex.find(key);
				if (arc == arcIndex.end() || !seen.insert(key).second) { reason = "unknown or duplicate solution arc"; return false; }
				flow[arc->second] = value;
			}
			else { reason = "unknown solution record"; return false; }
		}
		if (!sawStatus) { reason = "missing solution objective"; return false; }
		long long objective = 0;
		for (size_t i = 0; i < arcs.size(); ++i)
		{
			const DimacsArc& arc = arcs[i];
			if (flow[i] < arc.lower || flow[i] > arc.upper || (flow[i] != 0 && arc.cost > LLONG_MAX / flow[i]) ||
				flow[i] * arc.cost > LLONG_MAX - objective) { reason = "solution bounds or objective overflow"; return false; }
			objective += flow[i] * arc.cost;
			if ((flow[i] > 0 && supply[arc.tail] < LLONG_MIN + flow[i]) || (flow[i] > 0 && supply[arc.head] > LLONG_MAX - flow[i])) { reason = "solution conservation overflow"; return false; }
			supply[arc.tail] -= flow[i]; supply[arc.head] += flow[i];
		}
		for (size_t i = 1; i < supply.size(); ++i) if (supply[i] != 0) { reason = "solution violates node conservation"; return false; }
		if (objective != declaredObjective) { reason = "solution objective mismatch"; return false; }
		return true;
	}

	bool runExternalProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments,
		const std::string& jobPrefix, const std::string& errorMsgPrefix, UnwrapProgressCallback cb = nullptr,
		ExternalToolResult* output = nullptr)
	{
		ExternalToolResult result = output ? *output : ExternalToolResult{};
		result.tool = jobPrefix;
		result.phase = "create job";
		STARTUPINFO si = {};
		PROCESS_INFORMATION pi = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
		si.wShowWindow = FALSE;

		SECURITY_ATTRIBUTES inheritable = {};
		inheritable.nLength = sizeof(inheritable);
		inheritable.bInheritHandle = TRUE;
		HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		HANDLE stdoutRead = INVALID_HANDLE_VALUE;
		HANDLE stdoutWrite = INVALID_HANDLE_VALUE;
		HANDLE stderrRead = INVALID_HANDLE_VALUE;
		HANDLE stderrWrite = INVALID_HANDLE_VALUE;
		if (nullInput == INVALID_HANDLE_VALUE ||
			!CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0) ||
			!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
			!CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0) ||
			!SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0))
		{
			result.win32Error = GetLastError();
			if (stdoutRead != INVALID_HANDLE_VALUE) CloseHandle(stdoutRead);
			if (stdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(stdoutWrite);
			if (stderrRead != INVALID_HANDLE_VALUE) CloseHandle(stderrRead);
			if (stderrWrite != INVALID_HANDLE_VALUE) CloseHandle(stderrWrite);
			if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
			if (output) *output = result;
			return false;
		}
		si.hStdInput = nullInput;
		si.hStdOutput = stdoutWrite;
		si.hStdError = stderrWrite;

		std::wstring commandLine = quoteCommandArgument(executable);
		for (const std::wstring& argument : arguments) commandLine += L" " + quoteCommandArgument(argument);
		std::vector<wchar_t> cmdLineCopy(commandLine.begin(), commandLine.end());
		cmdLineCopy.push_back(L'\0');

		HANDLE job = CreateJobObjectW(nullptr, nullptr);
		if (!job)
		{
			result.win32Error = GetLastError();
			CloseHandle(stdoutRead);
			CloseHandle(stdoutWrite);
			CloseHandle(stderrRead);
			CloseHandle(stderrWrite);
			CloseHandle(nullInput);
			if (output) *output = result;
			return false;
		}
		{
			result.phase = "configure job";
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
			{
				result.win32Error = GetLastError();
				CloseHandle(job);
				CloseHandle(stdoutRead);
				CloseHandle(stdoutWrite);
				CloseHandle(stderrRead);
				CloseHandle(stderrWrite);
				CloseHandle(nullInput);
				if (output) *output = result;
				return false;
			}
		}

		result.phase = "launch";
		BOOL bRet = ::CreateProcessW(
			executable.c_str(),
			cmdLineCopy.data(),
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED,
			NULL,
			NULL,
			&si,
			&pi);

		if (!bRet)
		{
			result.win32Error = GetLastError();
			fprintf(stderr, "%s: CreateProcessW failed (%lu).\n\n", errorMsgPrefix.c_str(), result.win32Error);
			CloseHandle(job);
			CloseHandle(stdoutRead);
			CloseHandle(stdoutWrite);
			CloseHandle(stderrRead);
			CloseHandle(stderrWrite);
			CloseHandle(nullInput);
			if (output) *output = result;
			return false;
		}
		CloseHandle(stdoutWrite);
		stdoutWrite = INVALID_HANDLE_VALUE;
		CloseHandle(stderrWrite);
		stderrWrite = INVALID_HANDLE_VALUE;
		CloseHandle(nullInput);
		result.phase = "bind job";
		if (!AssignProcessToJobObject(job, pi.hProcess))
		{
			result.win32Error = GetLastError();
			TerminateProcess(pi.hProcess, static_cast<UINT>(-3));
			if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) result.terminationUncertain = true;
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			CloseHandle(job);
			drainPipe(stdoutRead, result.stderrTail);
			CloseHandle(stdoutRead);
			drainStderrPipe(stderrRead, result.stderrTail);
			CloseHandle(stderrRead);
			if (output) *output = result;
			return false;
		}
		if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
		{
			result.win32Error = GetLastError();
			result.phase = "resume process";
			TerminateJobObject(job, static_cast<UINT>(-3));
			if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) result.terminationUncertain = true;
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			CloseHandle(job);
			drainPipe(stdoutRead, result.stderrTail);
			CloseHandle(stdoutRead);
			drainStderrPipe(stderrRead, result.stderrTail);
			CloseHandle(stderrRead);
			if (output) *output = result;
			return false;
		}

		bool is_cancelled = false;
		bool wait_failed = false;
		const ULONGLONG startedAt = GetTickCount64();
		ULONGLONG lastHeartbeatAt = startedAt;
		result.phase = "process exit";
		const std::string runningMessage = "Running external solver " + jobPrefix + " (progress unavailable)...";
		if (cb && !cb(0, runningMessage.c_str())) is_cancelled = true;
		if (!is_cancelled && !emitSnaphuRunEvent(SNAPHU_RUN_EVENT_STARTED, runningMessage, startedAt, job)) is_cancelled = true;
		if (g_activeSnaphuRun && g_activeSnaphuRun->options.requestedProcessCount != 1)
		{
			if (!emitSnaphuRunEvent(SNAPHU_RUN_EVENT_WARNING,
				"Windows SNAPHU runs with NPROC=1; the requested process count was downgraded.", startedAt, job)) is_cancelled = true;
		}
		while (!is_cancelled && !result.timedOut)
		{
			const DWORD waitResult = WaitForSingleObject(pi.hProcess, 100);
			if (!drainSnaphuPipe(stdoutRead, result.stderrTail, "stdout", startedAt, job)) is_cancelled = true;
			if (!drainSnaphuPipe(stderrRead, result.stderrTail, "stderr", startedAt, job)) is_cancelled = true;
			const ULONGLONG now = GetTickCount64();
			if (g_activeSnaphuRun && now - lastHeartbeatAt >= g_activeSnaphuRun->options.heartbeatMilliseconds)
			{
				if (!emitSnaphuRunEvent(SNAPHU_RUN_EVENT_HEARTBEAT, runningMessage, startedAt, job)) is_cancelled = true;
				lastHeartbeatAt = now;
			}
			if (g_activeSnaphuRun && g_activeSnaphuRun->options.wallTimeoutMilliseconds != 0 &&
				now - startedAt >= g_activeSnaphuRun->options.wallTimeoutMilliseconds)
			{
				result.timedOut = true;
				result.phase = "timeout";
				emitSnaphuRunEvent(SNAPHU_RUN_EVENT_TIMED_OUT, "SNAPHU wall-clock timeout reached.", startedAt, job);
				break;
			}
			if (waitResult == WAIT_OBJECT_0) break;
			if (waitResult != WAIT_TIMEOUT)
			{
				result.win32Error = GetLastError();
				wait_failed = true;
				break;
			}
		}

		if (is_cancelled || result.timedOut || wait_failed)
		{
			result.cancelled = is_cancelled;
			if (is_cancelled) result.phase = "cancel";
			if (!TerminateJobObject(job, static_cast<UINT>(is_cancelled ? -2 : -3))) result.win32Error = GetLastError();
			// A hard stop must not make the caller wait forever or clean an active task directory.
			const ULONGLONG terminationDeadline = GetTickCount64() + 5000;
			while (true)
			{
				JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting = {};
				const bool jobKnown = QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
					&accounting, sizeof(accounting), nullptr) != FALSE;
				const DWORD processState = WaitForSingleObject(pi.hProcess, 0);
				if (jobKnown && accounting.ActiveProcesses == 0 && processState == WAIT_OBJECT_0) break;
				if (!jobKnown || processState == WAIT_FAILED || GetTickCount64() >= terminationDeadline)
				{
					result.terminationUncertain = true;
					if ((!jobKnown || processState == WAIT_FAILED) && result.win32Error == ERROR_SUCCESS) result.win32Error = GetLastError();
					break;
				}
				drainPipe(stdoutRead, result.stderrTail);
				drainStderrPipe(stderrRead, result.stderrTail);
				Sleep(25);
			}
		}
		else
		{
			// The root process has exited, but Job accounting can lag briefly.  Wait a
			// bounded interval before treating a remaining descendant as uncertain.
			const ULONGLONG completionDeadline = GetTickCount64() + 5000;
			while (true)
			{
				JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting = {};
				if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
					&accounting, sizeof(accounting), nullptr))
				{
					result.terminationUncertain = true;
					if (result.win32Error == ERROR_SUCCESS) result.win32Error = GetLastError();
					break;
				}
				if (accounting.ActiveProcesses == 0) break;
				if (GetTickCount64() >= completionDeadline)
				{
					result.terminationUncertain = true;
					if (result.win32Error == ERROR_SUCCESS) result.win32Error = ERROR_BUSY;
					break;
				}
				drainPipe(stdoutRead, result.stderrTail);
				drainStderrPipe(stderrRead, result.stderrTail);
				Sleep(25);
			}
		}

		if (!result.terminationUncertain && !GetExitCodeProcess(pi.hProcess, &result.exitCode)) result.win32Error = GetLastError();
		drainPipe(stdoutRead, result.stderrTail);
		drainStderrPipe(stderrRead, result.stderrTail);
		if (result.cancelled) emitSnaphuRunEvent(SNAPHU_RUN_EVENT_CANCELLED, "SNAPHU cancellation completed.", startedAt, job);

		::CloseHandle(pi.hThread);
		::CloseHandle(pi.hProcess);
		::CloseHandle(job);
		CloseHandle(stdoutRead);
		CloseHandle(stderrRead);
		if (result.cancelled || result.timedOut || result.terminationUncertain) { if (output) *output = result; return false; }
		if (result.win32Error != ERROR_SUCCESS || result.exitCode != 0)
		{
			fprintf(stderr, "%s: process failed (exit=%lu, win32=%lu).\n\n", errorMsgPrefix.c_str(), result.exitCode, result.win32Error);
			if (output) *output = result;
			return false;
		}
		const std::string completedMessage = "External solver " + jobPrefix + " completed.";
		emitSnaphuRunEvent(SNAPHU_RUN_EVENT_COMPLETED, completedMessage, startedAt, nullptr);
		if (cb && !cb(100, completedMessage.c_str()))
		{
			result.cancelled = true;
			result.phase = "cancel";
			if (output) *output = result;
			return false;
		}
		if (output) *output = result;
		return true;
	}

	bool runExternalProcessUtf8(const std::string& executableFolder, const wchar_t* executableName,
		const std::string& argument, const std::string& jobPrefix, const std::string& errorMsgPrefix,
		UnwrapProgressCallback cb, ExternalToolResult* output = nullptr)
	{
		std::wstring folder;
		std::wstring wideArgument;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(executableFolder, folder, &error) ||
			!PathResolver::utf8ToWide(argument, wideArgument, &error))
		{
			fprintf(stderr, "%s: %s.\n\n", errorMsgPrefix.c_str(), PathResolver::errorMessage(error));
			if (output) { output->tool = jobPrefix; output->phase = "path conversion"; output->validationFailure = PathResolver::errorMessage(error); }
			return false;
		}
		if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/') folder.push_back(L'\\');
		return runExternalProcess(folder + executableName, { wideArgument }, jobPrefix, errorMsgPrefix, cb, output);
	}

	bool runExternalProcessUtf8(const std::string& executableFolder, const wchar_t* executableName,
		const std::vector<std::wstring>& arguments, const std::string& jobPrefix, const std::string& errorMsgPrefix,
		UnwrapProgressCallback cb, ExternalToolResult* output = nullptr)
	{
		std::wstring folder;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(executableFolder, folder, &error))
		{
			fprintf(stderr, "%s: %s.\n\n", errorMsgPrefix.c_str(), PathResolver::errorMessage(error));
			if (output) { output->tool = jobPrefix; output->phase = "path conversion"; output->validationFailure = PathResolver::errorMessage(error); }
			return false;
		}
		if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/') folder.push_back(L'\\');
		return runExternalProcess(folder + executableName, arguments, jobPrefix, errorMsgPrefix, cb, output);
	}

	bool removeOwnedTaskTree(const std::wstring& directory, ExternalToolResult* result)
	{
		const DWORD attributes = GetFileAttributesW(directory.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			if (result) result->cleanupResiduals.emplace_back(directory, ERROR_CANT_ACCESS_FILE);
			return false;
		}
		const std::wstring pattern = directory + L"\\*";
		WIN32_FIND_DATAW found = {};
		HANDLE search = FindFirstFileW(pattern.c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
		{
			if (result) result->cleanupResiduals.emplace_back(directory, GetLastError());
			return false;
		}
		bool success = true;
		do
		{
			if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
			const std::wstring child = directory + L"\\" + found.cFileName;
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				if (result) result->cleanupResiduals.emplace_back(child, ERROR_CANT_ACCESS_FILE);
				success = false;
				continue;
			}
			const bool childOk = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
				? removeOwnedTaskTree(child, result)
				: DeleteFileW(child.c_str()) != FALSE;
			if (!childOk)
			{
				if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && result)
					result->cleanupResiduals.emplace_back(child, GetLastError());
				success = false;
			}
		} while (FindNextFileW(search, &found));
		const DWORD enumerateError = GetLastError();
		FindClose(search);
		if (enumerateError != ERROR_NO_MORE_FILES)
		{
			if (result) result->cleanupResiduals.emplace_back(directory, enumerateError);
			success = false;
		}
		if (!success || !RemoveDirectoryW(directory.c_str()))
		{
			if (result && !success) result->cleanupResiduals.emplace_back(directory, ERROR_DIR_NOT_EMPTY);
			else if (result) result->cleanupResiduals.emplace_back(directory, GetLastError());
			return false;
		}
		return true;
	}

	class ScopedArtifactDirectory
	{
	public:
		explicit ScopedArtifactDirectory(const std::wstring& directory, ExternalToolResult* result = nullptr)
			: directory_(directory), result_(result), preserve_(false), completed_(false) {}
		~ScopedArtifactDirectory()
		{
			const bool failed = result_ && (result_->cancelled || result_->timedOut || result_->terminationUncertain ||
				result_->win32Error != ERROR_SUCCESS || result_->exitCode != STILL_ACTIVE && result_->exitCode != 0 ||
				!result_->validationFailure.empty());
			const bool keepOnSuccess = g_activeSnaphuRun &&
				(g_activeSnaphuRun->options.flags & SNAPHU_RUN_OPTION_KEEP_ARTIFACTS_ON_SUCCESS) != 0;
			// Ex2 keeps every incomplete task. Only code after output validation may
			// explicitly opt into deletion, and the process wrapper proved job exit.
			if (preserve_ || failed || keepOnSuccess || (g_activeSnaphuRun && !completed_)) return;
			if (!directory_.empty() && !removeOwnedTaskTree(directory_, result_))
				fprintf(stderr, "External tool cleanup left task directory.\n");
		}

		bool registerCandidate(const std::wstring& file)
		{
			if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
			files_.push_back({ file, false });
			return true;
		}

		bool markOwned(const std::wstring& file)
		{
			for (Artifact& artifact : files_)
			{
				if (artifact.path != file) continue;
				if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
				artifact.owned = true;
				if (result_ && std::find(result_->managedArtifacts.begin(), result_->managedArtifacts.end(), file) == result_->managedArtifacts.end())
					result_->managedArtifacts.push_back(file);
				return true;
			}
			return false;
		}

		bool clearOwned(const std::wstring& file)
		{
			for (Artifact& artifact : files_)
			{
				if (artifact.path != file) continue;
				if (!artifact.owned) return GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES;
				if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(file.c_str()))
				{
					const DWORD error = GetLastError();
					if (result_) result_->cleanupResiduals.emplace_back(file, error);
					return false;
				}
				artifact.owned = false;
				return true;
			}
			return false;
		}

		void preserve() { preserve_ = true; }
		void markCompleted() { completed_ = true; }

	private:
		struct Artifact { std::wstring path; bool owned; };
		std::wstring directory_;
		std::vector<Artifact> files_;
		ExternalToolResult* result_;
		bool preserve_;
		bool completed_;
	};

	bool runMcfProcess(const char* executableFolder, const char* networkFile, const std::string& errorMessage,
		UnwrapProgressCallback cb, ScopedArtifactDirectory* artifacts = nullptr, ExternalToolResult* output = nullptr)
	{
		ExternalToolResult result = output ? *output : ExternalToolResult{};
		if (!executableFolder || !networkFile)
		{
			result.tool = "MCF";
			result.phase = "input";
			result.validationFailure = "missing executable or network path";
			if (output) *output = result;
			return false;
		}
		const std::string solution = std::string(networkFile) + ".sol";
		std::wstring wideSolution;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(solution, wideSolution, &error) ||
			GetFileAttributesW(wideSolution.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			result.tool = "MCF";
			result.phase = "prepare solution";
			result.validationFailure = "pre-existing or invalid solution artifact";
			fprintf(stderr, "%s: refusing a pre-existing or invalid solution artifact.\n", errorMessage.c_str());
			if (output) *output = result;
			return false;
		}
		if (!runExternalProcessUtf8(executableFolder, L"mcf.exe", networkFile, "MCF", errorMessage, cb, &result))
		{
			if (artifacts && GetFileAttributesW(wideSolution.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				artifacts->markOwned(wideSolution);
				if (std::find(result.managedArtifacts.begin(), result.managedArtifacts.end(), wideSolution) == result.managedArtifacts.end())
					result.managedArtifacts.push_back(wideSolution);
			}
			if (output) *output = result;
			return false;
		}
		if (artifacts && !artifacts->markOwned(wideSolution))
		{
			result.phase = "record solution";
			result.validationFailure = "solution was not created in this task";
			if (output) *output = result;
			return false;
		}
		if (std::find(result.managedArtifacts.begin(), result.managedArtifacts.end(), wideSolution) == result.managedArtifacts.end())
			result.managedArtifacts.push_back(wideSolution);
		std::string validationFailure;
		if (!validateMcfSolution(networkFile, solution, validationFailure))
		{
			result.phase = "validate solution";
			result.validationFailure = validationFailure;
			fprintf(stderr, "%s: invalid mcf solution (%s).\n\n", errorMessage.c_str(), validationFailure.c_str());
			if (output) *output = result;
			return false;
		}
		if (output) *output = result;
		return true;
	}

	bool createTaskDirectory(const std::string& parentUtf8, std::string& outputUtf8, std::wstring& outputWide)
	{
		PathResolver::Error error = PathResolver::Error::None;
		std::wstring parent;
		if (!PathResolver::utf8ToWide(parentUtf8, parent, &error) || parent.empty()) return false;
		static std::atomic<unsigned long> sequence{ 0 };
		for (unsigned int attempt = 0; attempt != 128; ++attempt)
		{
			std::wostringstream name;
			name << parent;
			if (parent.back() != L'\\' && parent.back() != L'/') name << L'\\';
			name << L"insar-snaphu-" << GetCurrentProcessId() << L"-" << GetTickCount64() << L"-" << ++sequence;
			outputWide = name.str();
			if (CreateDirectoryW(outputWide.c_str(), nullptr))
				return PathResolver::wideToUtf8(outputWide, outputUtf8, &error);
			if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
		}
		return false;
	}

	bool createMcfTaskDirectory(const char* requestedNetworkPath, std::string& outputUtf8, std::wstring& outputWide)
	{
		if (!requestedNetworkPath || !*requestedNetworkPath) return false;
		std::wstring requested;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::utf8ToWide(requestedNetworkPath, requested, &error)) return false;
		const size_t separator = requested.find_last_of(L"\\/");
		std::string parent;
		const std::wstring parentWide = separator == std::wstring::npos ? L"." : requested.substr(0, separator);
		if (!PathResolver::wideToUtf8(parentWide, parent, &error)) return false;
		return createTaskDirectory(parent, outputUtf8, outputWide);
	}

	bool appendSnaphuTilingConfig(std::ostringstream& config, const std::wstring& taskFolderWide,
		int rows, int cols, ExternalToolResult& result)
	{
		if (!g_activeSnaphuRun) return true;
		const SnaphuRunOptionsV1& options = g_activeSnaphuRun->options;
		const bool tiled = options.tileRows > 1 || options.tileCols > 1;
		if (!tiled)
		{
			config << "NPROC 1\n";
			return true;
		}
		if (options.tileRows > static_cast<uint32_t>(rows) || options.tileCols > static_cast<uint32_t>(cols) ||
			options.rowOverlap >= static_cast<uint32_t>(rows) / options.tileRows ||
			options.colOverlap >= static_cast<uint32_t>(cols) / options.tileCols)
		{
			result.phase = "input";
			result.validationFailure = "SNAPHU tile count or overlap is incompatible with input dimensions";
			return false;
		}
		const std::wstring tileDirectoryWide = taskFolderWide + L"\\tiles";
		if (!CreateDirectoryW(tileDirectoryWide.c_str(), nullptr))
		{
			result.phase = "prepare solution";
			result.win32Error = GetLastError();
			result.validationFailure = "cannot create SNAPHU tile directory";
			return false;
		}
		std::string tileDirectory;
		PathResolver::Error error = PathResolver::Error::None;
		if (!PathResolver::wideToUtf8(tileDirectoryWide, tileDirectory, &error))
		{
			result.phase = "path conversion";
			result.validationFailure = PathResolver::errorMessage(error);
			return false;
		}
		std::string quotedTileDirectory;
		if (!quoteSnaphuConfigPath(tileDirectory, quotedTileDirectory))
		{
			result.phase = "path conversion";
			result.validationFailure = "SNAPHU tile directory cannot be represented in config";
			return false;
		}
		config << "NTILEROW " << options.tileRows << "\nNTILECOL " << options.tileCols << "\n";
		config << "NPROC 1\nROWOVRLP " << options.rowOverlap << "\nCOLOVRLP " << options.colOverlap << "\n";
		config << "TILEDIR " << quotedTileDirectory << "\nRMTMPTILE FALSE\n";
		return true;
	}

	bool writeBytes(const std::wstring& path, const void* bytes, size_t byteCount, ScopedArtifactDirectory* artifacts = nullptr)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		if (artifacts && !artifacts->markOwned(path)) { CloseHandle(file); return false; }
		const unsigned char* cursor = static_cast<const unsigned char*>(bytes);
		size_t remaining = byteCount;
		bool ok = true;
		while (remaining != 0)
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 0x7ffff000));
			DWORD written = 0;
			if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) { ok = false; break; }
			cursor += written;
			remaining -= written;
		}
		if (!CloseHandle(file)) ok = false;
		return ok;
	}

	bool writeFloatRaster(const std::wstring& path, const Mat& input, ScopedArtifactDirectory* artifacts = nullptr)
	{
		if (input.empty() || input.type() != CV_32F) return false;
		const Mat contiguous = input.isContinuous() ? input : input.clone();
		return writeBytes(path, contiguous.data, contiguous.total() * sizeof(float), artifacts);
	}

	std::string correlationDimensions(const Mat& correlation)
	{
		std::ostringstream text;
		text << correlation.rows << "x" << correlation.cols;
		return text.str();
	}

	bool validateCorrelation(const Mat& correlation, int expectedRows, int expectedColumns, std::string& reason)
	{
		if (correlation.empty())
		{
			reason = "empty";
			return false;
		}
		if (correlation.dims != 2)
		{
			reason = "rank_mismatch";
			return false;
		}
		if (correlation.channels() != 1)
		{
			reason = "channel_mismatch";
			return false;
		}
		if (correlation.rows != expectedRows || correlation.cols != expectedColumns)
		{
			std::ostringstream text;
			text << "dimension_mismatch(" << correlationDimensions(correlation) << ", expected="
				<< expectedRows << "x" << expectedColumns << ")";
			reason = text.str();
			return false;
		}
		if (correlation.type() != CV_32FC1 && correlation.type() != CV_64FC1)
		{
			std::ostringstream text;
			text << "type_mismatch(expected=CV_32FC1|CV_64FC1, actual=" << correlation.type() << ")";
			reason = text.str();
			return false;
		}

		for (int row = 0; row < correlation.rows; ++row)
		{
			for (int column = 0; column < correlation.cols; ++column)
			{
				const double value = correlation.type() == CV_32FC1 ?
					static_cast<double>(correlation.ptr<float>(row)[column]) : correlation.ptr<double>(row)[column];
				if (!std::isfinite(value))
				{
					std::ostringstream text;
					text << "non_finite(row=" << row << ", col=" << column << ")";
					reason = text.str();
					return false;
				}
				if (value < 0.0 || value > 1.0)
				{
					std::ostringstream text;
					text << "out_of_range(row=" << row << ", col=" << column << ")";
					reason = text.str();
					return false;
				}
			}
		}
		reason.clear();
		return true;
	}

	bool readValidatedFloatRaster(const std::wstring& path, int rows, int columns, Mat& output, std::string* reason = nullptr)
	{
		if (rows <= 0 || columns <= 0 || static_cast<unsigned long long>(rows) * columns >
			std::numeric_limits<size_t>::max() / sizeof(float)) { if (reason) *reason = "invalid output dimensions"; return false; }
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) { if (reason) *reason = "output file was not created"; return false; }
		LARGE_INTEGER size = {};
		const unsigned long long expected = static_cast<unsigned long long>(rows) * columns * sizeof(float);
		bool ok = GetFileSizeEx(file, &size) && static_cast<unsigned long long>(size.QuadPart) == expected;
		output.create(rows, columns, CV_32F);
		unsigned char* cursor = output.data;
		size_t remaining = static_cast<size_t>(expected);
		while (ok && remaining != 0)
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 0x7ffff000));
			DWORD read = 0;
			if (!ReadFile(file, cursor, chunk, &read, nullptr) || read != chunk) { ok = false; break; }
			cursor += read;
			remaining -= read;
		}
		CloseHandle(file);
		if (!ok) { output.release(); if (reason) *reason = "output length or read does not match raster dimensions"; return false; }
		for (int row = 0; row < rows; ++row)
			for (int column = 0; column < columns; ++column)
				if (!std::isfinite(output.at<float>(row, column))) { output.release(); if (reason) *reason = "output contains NaN or Inf"; return false; }
		return true;
	}

	// 质量引导解缠专用：包含 4 邻域的梯度与相位更新控制参数
	struct QualityGuidedDirection {
		int dr, dc;
		bool is_horizontal;  // true 表示使用 k2 (水平)，false 表示使用 k1 (垂直)
		int k_row_offset;    // 相对于 node.row 的 k 矩阵行偏移
		int k_col_offset;    // 相对于 node.col 的 k 矩阵列偏移
		double sign;         // 2 * PI * k 积分时的正负符号 (+1.0 或 -1.0)
	};

	// 4 邻域方向数组：按 [左, 右, 上, 下] 顺序排列，严格对应原硬编码公式
	const QualityGuidedDirection QUALITY_GUIDED_DIRS[4] = {
		{0, -1, true,  0, -1,  1.0}, // 左: 使用 k2(row, col-1), 符号为 +
		{0,  1, true,  0,  0, -1.0}, // 右: 使用 k2(row, col), 符号为 -
		{-1, 0, false, -1, 0,  1.0}, // 上: 使用 k1(row-1, col), 符号为 +
		{ 1, 0, false,  0, 0, -1.0}  // 下: 使用 k1(row, col), 符号为 -
	};

	// 基础 4 方向偏移量 (左, 右, 上, 下)
	const int DIR_DR[4] = {0, 0, -1, 1}; // 行 (y) 偏移
	const int DIR_DC[4] = {-1, 1, 0, 0}; // 列 (x) 偏移

	// 队列初始化方向优先级：下, 上, 左, 右
	const int INIT_DR[4] = {1, -1, 0, 0};
	const int INIT_DC[4] = {0, 0, -1, 1};
}




Unwrap::Unwrap()
{
	memset(this->error_head, 0, 256);
	memset(this->parallel_error_head, 0, 256);
	strcpy(this->error_head, "UNWRAP_DLL_ERROR: error happens when using ");
	strcpy(this->parallel_error_head, "UNWRAP_DLL_ERROR: error happens when using parallel computing in function: ");
}

Unwrap::~Unwrap()
{
}

int Unwrap::MCFInternal(
	Mat& wrapped_phase,
	Mat& unwrapped_phase,
	Mat& coherence,
	Mat& residue,
	const char* MCF_problem_file,
	const char* MCF_EXE_PATH,
	UnwrapProgressCallback cb
)

{
	if (wrapped_phase.rows < 2 ||
		wrapped_phase.cols < 2 ||
		(wrapped_phase.rows - residue.rows) != 1 ||
		((wrapped_phase.cols - residue.cols)) != 1 ||
		wrapped_phase.rows != coherence.rows ||
		wrapped_phase.cols != coherence.cols ||
		wrapped_phase.type() != CV_64F||
		coherence.type() != CV_64F||
		residue.type() != CV_64F)
	{
		fprintf(stderr, "MCF(): input check failed!\n\n");
		return -1;
	}
	USES_CONVERSION;
	Utils util;
	int ret;
	int residue_count = 0;
	for (int i = 0; i < residue.rows; i++)
	{
		for (int j = 0; j < residue.cols; j++)
		{
			if (fabs(residue.at<double>(i, j)) > 0.5) residue_count++;
		}
	}
	if (residue_count == 0)
	{
		Mat k1, k2;
		k1 = Mat::zeros(wrapped_phase.rows - 1, wrapped_phase.cols, CV_64F);
		k2 = Mat::zeros(wrapped_phase.rows, wrapped_phase.cols - 1, CV_64F);
		Mat diff_1, diff_2;
		ret = util.diff(wrapped_phase, diff_1, diff_2, false);
		if (return_check(ret, "diff(*, *, *, *)", error_head)) return -1;
		ret = util.wrap(diff_1, diff_1);
		if (return_check(ret, "wrap(*, *)", error_head)) return -1;
		ret = util.wrap(diff_2, diff_2);
		if (return_check(ret, "wrap(*, *)", error_head)) return -1;
		double pi = 3.1415926535;
		diff_1 = diff_1 / (2 * pi);
		diff_2 = diff_2 / (2 * pi);
		diff_1 = diff_1 - k1;
		diff_2 = diff_2 - k2;
		Mat tmp = diff_2(Range(0, 1), Range(0, diff_2.cols));
		copyMakeBorder(tmp, tmp, 0, 0, 1, 0, BORDER_CONSTANT, Scalar(0.0));
		ret = util.cumsum(tmp, 2);
		if (return_check(ret, "cumsum(*, *)", error_head)) return -1;
		copyMakeBorder(diff_1, diff_1, 1, 0, 0, 0, BORDER_CONSTANT, Scalar(0));
		for (int i = 0; i < diff_1.cols; i++)
		{
			diff_1.at<double>(0, i) = tmp.at<double>(0, i);
		}
		ret = util.cumsum(diff_1, 1);
		if (return_check(ret, "cumsum(*, *)", error_head)) return -1;
		unwrapped_phase = (diff_1) * 2 * pi;
		unwrapped_phase = unwrapped_phase + wrapped_phase.at<double>(0, 0);
		return 0;
	}
	string taskFolder;
	std::wstring taskFolderWide;
	if (!createMcfTaskDirectory(MCF_problem_file, taskFolder, taskFolderWide)) return -1;
	ExternalToolResult toolResult;
	ScopedArtifactDirectory artifacts(taskFolderWide, &toolResult);
	const string taskNetwork = taskFolder + "\\mcf.net";
	const string taskSolution = taskNetwork + ".sol";
	std::wstring taskNetworkWide;
	std::wstring taskSolutionWide;
	PathResolver::Error artifactError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(taskNetwork, taskNetworkWide, &artifactError) ||
		!PathResolver::utf8ToWide(taskSolution, taskSolutionWide, &artifactError) ||
		!artifacts.registerCandidate(taskNetworkWide) || !artifacts.registerCandidate(taskSolutionWide)) return -1;
	ret = util.write_DIMACS(taskNetwork.c_str(), residue, coherence, 0.5);
	if (GetFileAttributesW(taskNetworkWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(taskNetworkWide);
	if (return_check(ret, "write_DIMACS(*, *, *)", error_head)) return -1;
	//////////////////////////创建并调用最小费用流法进程///////////////////////////////
	if (!runMcfProcess(MCF_EXE_PATH, taskNetwork.c_str(), "MCF(): mcf.exe failed!", cb, &artifacts, &toolResult))
	{
		return -2;
	}
	Mat k1, k2;
	ret = util.read_DIMACS(taskSolution.c_str(), k1, k2, wrapped_phase.rows, wrapped_phase.cols);
	if (return_check(ret, "read_DIMACS(*, *, *)", error_head)) return -1;
	Mat diff_1, diff_2;
	ret = util.diff(wrapped_phase, diff_1, diff_2, false);
	if (return_check(ret, "diff(*, *, *, *)", error_head)) return -1;
	ret = util.wrap(diff_1, diff_1);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	ret = util.wrap(diff_2, diff_2);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	double pi = 3.1415926535;
	diff_1 = diff_1 / (2 * pi);
	diff_2 = diff_2 / (2 * pi);
	diff_1 = diff_1 - k1;
	diff_2 = diff_2 - k2;
	Mat tmp = diff_2(Range(0, 1), Range(0, diff_2.cols));
	copyMakeBorder(tmp, tmp, 0, 0, 1, 0, BORDER_CONSTANT, Scalar(0.0));
	ret = util.cumsum(tmp, 2);
	if (return_check(ret, "cumsum(*, *)", error_head)) return -1;
	copyMakeBorder(diff_1, diff_1, 1, 0, 0, 0, BORDER_CONSTANT, Scalar(0));
	for (int i = 0; i < diff_1.cols; i++)
	{
		diff_1.at<double>(0, i) = tmp.at<double>(0, i);
	}
	ret = util.cumsum(diff_1, 1);
	if (return_check(ret, "cumsum(*, *)", error_head)) return -1;
	unwrapped_phase = (diff_1)* 2 * pi;
	unwrapped_phase = unwrapped_phase + wrapped_phase.at<double>(0, 0);
	return 0;
}

int Unwrap::MCFImprovedInternal(
	Mat& wrapped_phase, 
	Mat& unwrapped_phase,
	const char* MCF_problem_file,
	const char* MCF_exe_path,
	double coh_thresh,
	UnwrapProgressCallback cb
)
{
	if (wrapped_phase.rows < 2 ||
		wrapped_phase.cols < 2 ||
		wrapped_phase.type() != CV_64F)
	{
		fprintf(stderr, "MCF_improved(): input check failed!\n\n");
		return -1;
	}
	USES_CONVERSION;
	Utils util;
	Mat residue, cost, mask, phase_derivatives_variance;
	int ret;
	ret = util.residue(wrapped_phase, residue);
	if (return_check(ret, "residue()", error_head)) return -1;
	/*ret = util.phase_coherence(wrapped_phase, coherence);
	if (return_check(ret, "phase_coherence()", error_head)) return -1;*/
	ret = util.phase_derivatives_variance(wrapped_phase, phase_derivatives_variance);
	//ret = util.gen_mask(phase_derivatives_variance, mask, 7, coh_thresh);
	ret = util.gen_mask_pdv(phase_derivatives_variance, mask, 3, coh_thresh);
	if (return_check(ret, "gen_mask_pdv()", error_head)) return -1;
	cost = phase_derivatives_variance + 0.001;
	cost = 1 / cost;
	string taskFolder;
	std::wstring taskFolderWide;
	if (!createMcfTaskDirectory(MCF_problem_file, taskFolder, taskFolderWide)) return -1;
	ExternalToolResult toolResult;
	ScopedArtifactDirectory artifacts(taskFolderWide, &toolResult);
	const string taskNetwork = taskFolder + "\\mcf.net";
	const string taskSolution = taskNetwork + ".sol";
	std::wstring taskNetworkWide;
	std::wstring taskSolutionWide;
	PathResolver::Error artifactError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(taskNetwork, taskNetworkWide, &artifactError) ||
		!PathResolver::utf8ToWide(taskSolution, taskSolutionWide, &artifactError) ||
		!artifacts.registerCandidate(taskNetworkWide) || !artifacts.registerCandidate(taskSolutionWide)) return -1;
	ret = util.write_DIMACS(taskNetwork.c_str(), residue, mask, cost);
	if (GetFileAttributesW(taskNetworkWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(taskNetworkWide);
	if (return_check(ret, "write_DIMACS(*, *, *)", error_head)) return -1;
	Mat m; mask.convertTo(m, CV_64F);
	// 调试保存中间数据（若需本地调试，可取消注释并修改为自己的本地路径）
	// util.cvmat2bin("E:\\zgb1\\functions\\mask.bin", m);
	//////////////////////////创建并调用最小费用流法进程///////////////////////////////
	if (!runMcfProcess(MCF_exe_path, taskNetwork.c_str(), "MCF_improved(): mcf.exe failed!", cb, &artifacts, &toolResult))
	{
		return -2;
	}
	Mat k1, k2;
	ret = util.read_DIMACS(taskSolution.c_str(), k1, k2, wrapped_phase.rows, wrapped_phase.cols);
	if (return_check(ret, "read_DIMACS(*, *, *)", error_head)) return -1;
	// 调试保存中间数据（若需本地调试，可取消注释并修改为自己的本地路径）
	// util.cvmat2bin("E:\\zgb1\\functions\\k1.bin", k1);
	// util.cvmat2bin("E:\\zgb1\\functions\\k2.bin", k2);
	Mat quality;
	ret = util.phase_derivatives_variance(wrapped_phase, quality);
	if (return_check(ret, "phase_derivatives_variance()", error_head)) return -1;
	ret = qualityGuidedFloodfill(wrapped_phase, unwrapped_phase, mask, quality, k1, k2);
	if (return_check(ret, "qualityGuidedFloodfill()", error_head)) return -1;
	/*Mat diff_1, diff_2;
	ret = util.diff(wrapped_phase, diff_1, diff_2, false);
	if (return_check(ret, "diff(*, *, *, *)", error_head)) return -1;
	ret = util.wrap(diff_1, diff_1);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	ret = util.wrap(diff_2, diff_2);
	if (return_check(ret, "wrap(*, *)", error_head)) return -1;
	double pi = 3.1415926535;
	diff_1 = diff_1 / (2 * pi);
	diff_2 = diff_2 / (2 * pi);
	diff_1 = diff_1 - k1;
	diff_2 = diff_2 - k2;
	Mat tmp = diff_2(Range(0, 1), Range(0, diff_2.cols));
	copyMakeBorder(tmp, tmp, 0, 0, 1, 0, BORDER_CONSTANT, Scalar(0.0));
	ret = util.cumsum(tmp, 2);
	if (return_check(ret, "cumsum(*, *)", error_head)) return -1;
	copyMakeBorder(diff_1, diff_1, 1, 0, 0, 0, BORDER_CONSTANT, Scalar(0));
	for (int i = 0; i < diff_1.cols; i++)
	{
		diff_1.at<double>(0, i) = tmp.at<double>(0, i);
	}
	ret = util.cumsum(diff_1, 1);
	if (return_check(ret, "cumsum(*, *)", error_head)) return -1;
	unwrapped_phase = (diff_1) * 2 * pi;
	unwrapped_phase = unwrapped_phase + wrapped_phase.at<double>(0, 0);*/
	return 0;
}

int Unwrap::qualityGuidedFloodfill(Mat& wrapped_phase, Mat& unwrapped_phase, Mat& mask, Mat& quality, Mat& k1, Mat& k2, UnwrapProgressCallback cb)
{
	if (wrapped_phase.type() != CV_64F ||
		quality.type() != CV_64F ||
		mask.type() != CV_32S ||
		mask.rows != wrapped_phase.rows ||
		mask.cols != wrapped_phase.cols ||
		k1.type() != CV_64F ||
		k2.type() != CV_64F ||
		wrapped_phase.empty() ||
		k1.rows != (wrapped_phase.rows - 1) ||
		k1.cols != wrapped_phase.cols ||
		k2.rows != wrapped_phase.rows ||
		k2.cols != (wrapped_phase.cols - 1) ||
		k1.empty() ||
		k2.empty()
		)
	{
		fprintf(stderr, "qualityGuidedFloodfill(): input check failed!\n");
		return -1;
	}
	int nr = wrapped_phase.rows;
	int nc = wrapped_phase.cols;
	wrapped_phase.copyTo(unwrapped_phase);
	Mat unwrapped_status = Mat::zeros(nr, nc, CV_32S);

	/*策略1：高质量先解缠，低质量后解缠，都采用洪水淹没法*/
#if 0
	//找到质量最高点
	double max_quailty = 1000000000.0;
	int i_start = 0, j_start = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (quality.at<int>(i, j) < max_quailty)
			{
				i_start = i; j_start = j; max_quailty = quality.at<int>(i, j);
			}
		}
	}
	priority_queue<node_index> que;
	node_index node, node2;
	node.row = i_start; node.col = j_start; node.quality = max_quailty;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		node = que.top();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row + 1; jj = node.col ;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
	}
	queue<node_index> que2;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			ii = i + 1 > nr - 1 ? nr - 1 : i + 1; jj = j;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
			ii = i - 1 < 0 ? 0 : i - 1; jj = j;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
			ii = i; jj = j - 1 < 0 ? 0 : j - 1;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
			ii = i; jj = j + 1 > nc - 1 ? nc - 1 : j + 1;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
		}
	}
	while (!que2.empty())
	{
		node = que2.front();
		que2.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
	}
#endif
	/*策略2：低质量先解缠，高质量后解缠，都采用洪水淹没法*/
#if 0
	double max_quailty = 1000000000.0;
	int i_start = 0, j_start = 0;
	bool isbreak = false;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (mask.at<int>(i, j) == 0)
			{
				i_start = i; j_start = j;
				isbreak = true;
				break;
			}
		}
		if (isbreak) break;
	}
	queue<node_index> que;
	node_index node, node2;
	node.row = i_start; node.col = j_start;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		node = que.front();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
	}
	priority_queue<node_index> que2;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			ii = i + 1 > nr - 1 ? nr - 1 : i + 1; jj = j;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj; node.quality = quality.at<double>(ii, jj);
				}
				else
				{
					node.row = i; node.col = j; node.quality = quality.at<double>(i, j);
				}
				que2.push(node);
				continue;
			}
			ii = i - 1 < 0 ? 0 : i - 1; jj = j;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj; node.quality = quality.at<double>(ii, jj);
				}
				else
				{
					node.row = i; node.col = j; node.quality = quality.at<double>(i, j);
				}
				que2.push(node);
				continue;
			}
			ii = i; jj = j - 1 < 0 ? 0 : j - 1;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj; node.quality = quality.at<double>(ii, jj);
				}
				else
				{
					node.row = i; node.col = j; node.quality = quality.at<double>(i, j);
				}
				que2.push(node);
				continue;
			}
			ii = i; jj = j + 1 > nc - 1 ? nc - 1 : j + 1;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj; node.quality = quality.at<double>(ii, jj);
				}
				else
				{
					node.row = i; node.col = j; node.quality = quality.at<double>(i, j);
				}
				que2.push(node);
				continue;
			}
		}
	}
	while (!que2.empty())
	{
		node = que2.top();
		que2.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que2.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
	}
#endif
	/*策略3：同时解缠，采用洪水淹没法*/
#if 0
	double max_quailty = 1000000000.0;
	int i_start = nr / 2, j_start = nc / 2;
	queue<node_index> que;
	node_index node, node2;
	node.row = 1; node.col = 1;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		node = que.front();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
	}
#endif
	/*策略4：同时解缠，采用洪水淹没法，并绕过枝切线*/
#if  0
	double max_quailty = 1000000000.0;
	int i_start = nr / 2, j_start = nc / 2;
	queue<node_index> que;
	node_index node, node2;
	node.row = i_start; node.col = j_start;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		node = que.front();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k2.at<double>(node.row, jj)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
			
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k2.at<double>(node.row, node.col)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k1.at<double>(ii, node.col)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k1.at<double>(node.row, node.col)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
	}
#endif
	/*策略5：高质量先解缠，低质量后解缠，都采用洪水淹没法，积分时绕过枝切线*/
#if 0
	//找到质量最高点
	double max_quailty = 1000000000.0;
	int i_start = 0, j_start = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (quality.at<int>(i, j) < max_quailty)
			{
				i_start = i; j_start = j; max_quailty = quality.at<int>(i, j);
			}
		}
	}
	priority_queue<node_index> que;
	node_index node, node2;
	node.row = i_start; node.col = j_start; node.quality = max_quailty;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		node = que.top();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0 && mask.at<int>(ii, jj) == 1)
		{
			node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
			que.push(node2);
			grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
			grad = atan2(sin(grad), cos(grad));
			unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
			unwrapped_status.at<int>(ii, jj) = 1;
		}
	}
	queue<node_index> que2;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			ii = i + 1 > nr - 1 ? nr - 1 : i + 1; jj = j;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
			ii = i - 1 < 0 ? 0 : i - 1; jj = j;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
			ii = i; jj = j - 1 < 0 ? 0 : j - 1;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
			ii = i; jj = j + 1 > nc - 1 ? nc - 1 : j + 1;
			if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
			{
				if (unwrapped_status.at<int>(ii, jj) == 1)
				{
					node.row = ii; node.col = jj;
				}
				else
				{
					node.row = i; node.col = j;
				}
				que2.push(node);
				continue;
			}
		}
	}
	while (!que2.empty())
	{
		node = que2.front();
		que2.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		ii = node.row; jj = node.col - 1;
		if (jj > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k2.at<double>(node.row, jj)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que2.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k2.at<double>(node.row, jj);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
		ii = node.row; jj = node.col + 1;
		if (jj < nc && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k2.at<double>(node.row, node.col)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que2.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k2.at<double>(node.row, node.col);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
		ii = node.row - 1; jj = node.col;
		if (ii > 0 && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k1.at<double>(ii, node.col)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que2.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + 2 * PI * k1.at<double>(ii, node.col);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
		ii = node.row + 1; jj = node.col;
		if (ii < nr && unwrapped_status.at<int>(ii, jj) == 0)
		{
			if (fabs(k1.at<double>(node.row, node.col)) < 0.1)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que2.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad - 2 * PI * k1.at<double>(node.row, node.col);
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
	}
#endif
	/*策略6：质量图引导法同时解缠，积分时绕过枝切线*/
#if 1
	//找到质量最高点
	double max_quailty = 1000000000.0;
	int i_start = 0, j_start = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (quality.at<double>(i, j) < max_quailty)
			{
				i_start = i; j_start = j; max_quailty = quality.at<double>(i, j);
			}
		}
	}
	int total_pixels = nr * nc;
	int step = std::max(1, total_pixels / 100);
	int completed_pixels = 0;

	priority_queue<node_index> que;
	node_index node, node2;
	node.row = i_start; node.col = j_start; node.quality = max_quailty;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		completed_pixels++;
		if (cb && completed_pixels % step == 0)
		{
			int progress = completed_pixels * 100 / total_pixels;
			if (!cb(progress, "Unwrapping (Phase 1)..."))
			{
				return -2;
			}
		}
		node = que.top();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		for (int d = 0; d < 4; ++d)
		{
			ii = node.row + QUALITY_GUIDED_DIRS[d].dr;
			jj = node.col + QUALITY_GUIDED_DIRS[d].dc;
			bool in_bounds = false;
			if (d == 0) in_bounds = (jj > 0);
			else if (d == 1) in_bounds = (jj < nc);
			else if (d == 2) in_bounds = (ii > 0);
			else if (d == 3) in_bounds = (ii < nr);

			if (in_bounds && unwrapped_status.at<int>(ii, jj) == 0)
			{
				double k_val = QUALITY_GUIDED_DIRS[d].is_horizontal ?
					k2.at<double>(node.row + QUALITY_GUIDED_DIRS[d].k_row_offset, node.col + QUALITY_GUIDED_DIRS[d].k_col_offset) :
					k1.at<double>(node.row + QUALITY_GUIDED_DIRS[d].k_row_offset, node.col + QUALITY_GUIDED_DIRS[d].k_col_offset);

				if (fabs(k_val) < 0.1)
				{
					node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
					que.push(node2);
					grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
					grad = atan2(sin(grad), cos(grad));
					unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + QUALITY_GUIDED_DIRS[d].sign * 2 * PI * k_val;
					unwrapped_status.at<int>(ii, jj) = 1;
				}
			}
		}
	}

	if ((int)cv::sum(unwrapped_status)[0] == nr * nc) return 0;
	//处理未解缠的像素
	queue<node_index> que2;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			for (int d = 0; d < 4; ++d)
			{
				ii = i + INIT_DR[d];
				jj = j + INIT_DC[d];
				if (ii >= 0 && ii < nr && jj >= 0 && jj < nc)
				{
					if ((unwrapped_status.at<int>(ii, jj) + unwrapped_status.at<int>(i, j)) == 1)
					{
						if (unwrapped_status.at<int>(ii, jj) == 1)
						{
							node.row = ii; node.col = jj;
						}
						else
						{
							node.row = i; node.col = j;
						}
						que2.push(node);
						break;
					}
				}
			}
		}
	}
	while (!que2.empty())
	{
		completed_pixels++;
		if (cb && completed_pixels % step == 0)
		{
			int progress = completed_pixels * 100 / total_pixels;
			if (!cb(progress, "Unwrapping (Phase 2)..."))
			{
				return -2;
			}
		}
		node = que2.front();
		que2.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		for (int d = 0; d < 4; ++d)
		{
			ii = node.row + QUALITY_GUIDED_DIRS[d].dr;
			jj = node.col + QUALITY_GUIDED_DIRS[d].dc;
			bool in_bounds = false;
			if (d == 0) in_bounds = (jj > 0);
			else if (d == 1) in_bounds = (jj < nc);
			else if (d == 2) in_bounds = (ii > 0);
			else if (d == 3) in_bounds = (ii < nr);

			if (in_bounds && unwrapped_status.at<int>(ii, jj) == 0)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que2.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				double k_val = QUALITY_GUIDED_DIRS[d].is_horizontal ?
					k2.at<double>(node.row + QUALITY_GUIDED_DIRS[d].k_row_offset, node.col + QUALITY_GUIDED_DIRS[d].k_col_offset) :
					k1.at<double>(node.row + QUALITY_GUIDED_DIRS[d].k_row_offset, node.col + QUALITY_GUIDED_DIRS[d].k_col_offset);
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad + QUALITY_GUIDED_DIRS[d].sign * 2 * PI * k_val;
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
	}
#endif 

	return 0;
}


int Unwrap::MCF(
	Mat& wrapped_phase,
	Mat& unwrapped_phase,
	Mat& mask,
	vector<tri_node>& nodes,
	tri_edge* edges,
	int num_edges, 
	int start,
	bool pass,
	double thresh,
	UnwrapProgressCallback cb
)
{
	if (wrapped_phase.rows < 2 ||
		wrapped_phase.cols < 2 ||
		wrapped_phase.type() != CV_64F ||
		wrapped_phase.channels() != 1||
		mask.rows != wrapped_phase.rows||
		mask.cols != wrapped_phase.cols||
		mask.type() != CV_32S||
		mask.channels() != 1||
		nodes.size() < 3||
		edges == NULL||
		num_edges < 1||
		start < 1||
		start > nodes.size()
		)
	{
		fprintf(stderr, "MCF(): input check failed!\n\n");
		return -1;
	}
	wrapped_phase.copyTo(unwrapped_phase);
	int num_nodes = static_cast<int>(nodes.size());
	int number, ret, end2;
	double distance, grad, phi1, phi2, gain, tt, min_val, max_val;
	min_val = 1000000000.0;
	max_val = -1000000000.0;
	if (pass) tt = 0.5;
	else
	{
		tt = 100000.0;
	}
	queue<int> que;
	//int start = 1;//起始点默认为第一个点，后续可以自己设定
	nodes[start - 1].set_status(true);
	for (long edge_val : nodes[start - 1].get_neigh_edges())
	{
		if (edge_val < 1 || edge_val > num_edges)
		{
			fprintf(stderr, "MCF(): edge index exceed legal range!\n");
			return -1;
		}
		end2 = (edges + edge_val - 1)->end1 == start ? (edges + edge_val - 1)->end2 : (edges + edge_val - 1)->end1;
		if (end2 < 1 || end2 > num_nodes)
		{
			fprintf(stderr, "MCF(): node index exceed legal range!\n");
			return -1;
		}
		nodes[start - 1].get_distance(nodes[end2 - 1], &distance);
		if (!nodes[end2 - 1].get_status() &&
			distance <= thresh &&
			fabs((edges + edge_val - 1)->gain) < tt &&
			nodes[end2 - 1].get_balance() &&
			!((edges + edge_val - 1)->isBoundary && fabs((edges + edge_val - 1)->gain) > 0.5)
			)
		{
			que.push(end2);
			//解缠
			nodes[start - 1].get_phase(&phi1);
			nodes[end2 - 1].get_phase(&phi2);
			grad = phi2 - phi1;
			grad = atan2(sin(grad), cos(grad));
			gain = start > end2 ? 2 * PI * (edges + edge_val - 1)->gain : -2 * PI * (edges + edge_val - 1)->gain;
			nodes[end2 - 1].set_phase(grad + phi1 + gain);
			min_val = min_val > (grad + phi1 + gain) ? (grad + phi1 + gain) : min_val;
			max_val = max_val < (grad + phi1 + gain) ? (grad + phi1 + gain) : max_val;
			nodes[end2 - 1].set_status(true);
		}
	}

	int step = std::max(1, num_nodes / 100);
	int completed_nodes = 0;

	while (que.size() != 0)
	{
		completed_nodes++;
		if (cb && completed_nodes % step == 0)
		{
			int progress = completed_nodes * 100 / num_nodes;
			if (!cb(progress, "Unwrapping (MCF)..."))
			{
				return -2;
			}
		}
		number = que.front();
		que.pop();
		if (number < 1 || number > num_nodes)
		{
			fprintf(stderr, "MCF(): node index exceed legal range!\n");
			return -1;
		}
		for (long edge_val : nodes[number - 1].get_neigh_edges())
		{
			if (edge_val < 1 || edge_val > num_edges)
			{
				fprintf(stderr, "MCF(): edge index exceed legal range!\n");
				return -1;
			}
			end2 = (edges + edge_val - 1)->end1 == number ? (edges + edge_val - 1)->end2 : (edges + edge_val - 1)->end1;
			if (end2 < 1 || end2 > num_nodes)
			{
				fprintf(stderr, "MCF(): node index exceed legal range!\n");
				return -1;
			}
			nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
			if (!nodes[end2 - 1].get_status() &&
				distance <= thresh &&
				fabs((edges + edge_val - 1)->gain) < tt &&
				nodes[end2 - 1].get_balance()&&
				!((edges + edge_val - 1)->isBoundary && fabs((edges + edge_val - 1)->gain) > 0.5)
				)
			{
				que.push(end2);
				//解缠
				nodes[number - 1].get_phase(&phi1);
				nodes[end2 - 1].get_phase(&phi2);
				grad = phi2 - phi1;
				grad = atan2(sin(grad), cos(grad));
				gain = number > end2 ? 2 * PI * (edges + edge_val - 1)->gain : -2 * PI * (edges + edge_val - 1)->gain;
				min_val = min_val > (grad + phi1 + gain) ? (grad + phi1 + gain) : min_val;
				max_val = max_val < (grad + phi1 + gain) ? (grad + phi1 + gain) : max_val;
				nodes[end2 - 1].set_phase(grad + phi1 + gain);
				nodes[end2 - 1].set_status(true);
			}
		}
	}
	int rows, cols;
	int nr = unwrapped_phase.rows;
	int nc = unwrapped_phase.cols;
	double phi;
	Mat _mask = Mat::zeros(nr, nc, CV_32S);
	for (int i = 0; i < num_nodes; i++)
	{
		if (nodes[i].get_status())
		{
			ret = nodes[i].get_pos(&rows, &cols);
			if (rows > nr - 1 || cols > nc - 1 || rows < 0 || cols < 0)
			{
				fprintf(stderr, "MCF(): node posistion exceed legal range!\n");
				return -1;
			}
			ret = nodes[i].get_phase(&phi);
			unwrapped_phase.at<double>(rows, cols) = phi;
			_mask.at<int>(rows, cols) = 1;
		}
	}
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (_mask.at<int>(i, j) < 1)
			{
				unwrapped_phase.at<double>(i, j) = std::numeric_limits<double>::quiet_NaN();
			}
		}
	}
	_mask.copyTo(mask);
	return 0;
}

int Unwrap::MCF(
	const Mat& wrapped_phase,
	Mat& unwrapped_phase,
	Mat& out_mask,
	const Mat& mask,
	vector<tri_node>& nodes,
	vector<tri_edge>& edges,
	int start,
	bool pass,
	double thresh,
	UnwrapProgressCallback cb
)
{
	if (wrapped_phase.rows < 2 ||
		wrapped_phase.cols < 2 ||
		wrapped_phase.type() != CV_64F ||
		wrapped_phase.channels() != 1 ||
		mask.rows != wrapped_phase.rows ||
		mask.cols != wrapped_phase.cols ||
		mask.type() != CV_32S ||
		mask.channels() != 1 ||
		nodes.size() < 3 ||
		edges.size() < 3 ||
		start < 1 ||
		start > nodes.size()
		)
	{
		fprintf(stderr, "MCF(): input check failed!\n\n");
		return -1;
	}
	if (unwrapped_phase.rows != wrapped_phase.rows ||
		unwrapped_phase.cols != wrapped_phase.cols ||
		unwrapped_phase.type() != CV_64F)
	{
		wrapped_phase.copyTo(unwrapped_phase);
	}
	int num_nodes = static_cast<int>(nodes.size());
	int number, ret, end2;
	double distance, grad, phi1, phi2, gain, tt, min_val, max_val;
	min_val = 1000000000.0;
	max_val = -1000000000.0;
	if (pass) tt = 0.5;
	else
	{
		tt = 100000.0;
	}
	int num_edges = static_cast<int>(edges.size());
	queue<int> que;
	nodes[start - 1].set_status(true);
	for (long edge_val : nodes[start - 1].get_neigh_edges())
	{
		if (edge_val < 1 || edge_val > num_edges)
		{
			fprintf(stderr, "MCF(): edge index exceed legal range!\n");
			return -1;
		}
		end2 = edges[edge_val - 1].end1 == start ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
		if (end2 < 1 || end2 > num_nodes)
		{
			fprintf(stderr, "MCF(): node index exceed legal range!\n");
			return -1;
		}
		nodes[start - 1].get_distance(nodes[end2 - 1], &distance);
		if (!nodes[end2 - 1].get_status() &&
			distance <= thresh &&
			/*!edges[edge_val - 1].isBoundary &&*/
			!(edges[edge_val - 1].isBoundary && fabs(edges[edge_val - 1].gain) > 0.5) &&
			fabs(edges[edge_val - 1].gain) < tt /*&&
			nodes[end2 - 1].get_balance()*/
			)
		{
			que.push(end2);
			//解缠
			nodes[start - 1].get_phase(&phi1);
			nodes[end2 - 1].get_phase(&phi2);
			grad = phi2 - phi1;
			grad = atan2(sin(grad), cos(grad));
			gain = start < end2 ? 2 * PI * edges[edge_val - 1].gain : -2 * PI * edges[edge_val - 1].gain;
			nodes[end2 - 1].set_phase(grad + phi1 + gain);
			min_val = min_val > (grad + phi1 + gain) ? (grad + phi1 + gain) : min_val;
			max_val = max_val < (grad + phi1 + gain) ? (grad + phi1 + gain) : max_val;
			nodes[end2 - 1].set_status(true);
		}
	}

	int step = std::max(1, num_nodes / 100);
	int completed_nodes = 0;

	while (que.size() != 0)
	{
		completed_nodes++;
		if (cb && completed_nodes % step == 0)
		{
			int progress = completed_nodes * 100 / num_nodes;
			if (!cb(progress, "Unwrapping (MCF)..."))
			{
				return -2;
			}
		}
		number = que.front();
		que.pop();
		if (number < 1 || number > num_nodes)
		{
			fprintf(stderr, "MCF(): node index exceed legal range!\n");
			return -1;
		}
		for (long edge_val : nodes[number - 1].get_neigh_edges())
		{
			// removed unused: end1_row, end2_row, end1_col, end2_col (planned edge endpoint coords, never implemented)
			if (edge_val < 1 || edge_val > num_edges)
			{
				fprintf(stderr, "MCF(): edge index exceed legal range!\n");
				return -1;
			}
			end2 = edges[edge_val - 1].end1 == number ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
			if (end2 < 1 || end2 > num_nodes)
			{
				fprintf(stderr, "MCF(): node index exceed legal range!\n");
				return -1;
			}
			nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
			if (!nodes[end2 - 1].get_status() &&
				distance <= thresh &&
				/*!edges[edge_val - 1].isBoundary &&*/
				!(edges[edge_val - 1].isBoundary && fabs(edges[edge_val - 1].gain) > 0.5) &&
				fabs(edges[edge_val - 1].gain) < tt/* &&
				nodes[end2 - 1].get_balance() */
				)
			{
				que.push(end2);
				//解缠
				nodes[number - 1].get_phase(&phi1);
				nodes[end2 - 1].get_phase(&phi2);
				grad = phi2 - phi1;
				grad = atan2(sin(grad), cos(grad));
				gain = number < end2 ? 2 * PI * edges[edge_val - 1].gain : -2 * PI * edges[edge_val - 1].gain;
				min_val = min_val > (grad + phi1 + gain) ? (grad + phi1 + gain) : min_val;
				max_val = max_val < (grad + phi1 + gain) ? (grad + phi1 + gain) : max_val;
				nodes[end2 - 1].set_phase(grad + phi1 + gain);
				nodes[end2 - 1].set_status(true);
			}
		}
	}
	int rows, cols;
	int nr = unwrapped_phase.rows;
	int nc = unwrapped_phase.cols;
	double phi;
	Mat _mask = Mat::zeros(nr, nc, CV_32S);
	for (int i = 0; i < num_nodes; i++)
	{
		if (nodes[i].get_status())
		{
			ret = nodes[i].get_pos(&rows, &cols);
			ret = nodes[i].get_phase(&phi);
			unwrapped_phase.at<double>(rows, cols) = phi;
			_mask.at<int>(rows, cols) = 1;
		}
	}
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			if (_mask.at<int>(i, j) < 1)
//			{
//				unwrapped_phase.at<double>(i, j) = min_val - 0.01 * (max_val - min_val);
//			}
//		}
//	}
	_mask.copyTo(out_mask);
	return 0;
}

int Unwrap::MCF_second(Mat& unwrapped_phase, vector<tri_node>& nodes, tri_edge* edges, int num_edges, bool pass, double thresh, UnwrapProgressCallback cb)
{
	if (unwrapped_phase.rows < 2 ||
		unwrapped_phase.cols < 2 ||
		unwrapped_phase.type() != CV_64F ||
		unwrapped_phase.channels() != 1 ||
		nodes.size() < 3 ||
		edges == NULL ||
		num_edges < 3
		)
	{
		fprintf(stderr, "MCF_second(): input check failed!\n\n");
		return -1;
	}
	int num_nodes = static_cast<int>(nodes.size());
	int number, ret, end2;
	// removed unused: row_start, col_start (planned start position tracking, never implemented)
	double distance, grad, phi1, phi2, gain, tt;
	if (pass) tt = 0.5;
	else
	{
		tt = 100000.0;
	}
	queue<int> que;
	queue<int> start_que;
	int nr = unwrapped_phase.rows;
	int nc = unwrapped_phase.cols;
	//未解缠序列号矩阵

	//Mat wrapped_num = Mat::zeros(nr, nc, CV_32S);
	//Mat wrapped_mask = Mat::zeros(nr, nc, CV_32S);
	//Mat grad_updown, grad_leftright;
	//int wrapped_row, wrapped_col;
	//int row_unwrap_start, col_unwrap_start;
	//for (int i = 0; i < num_nodes; i++)
	//{
	//	nodes[i].get_pos(&wrapped_row, &wrapped_col);
	//	wrapped_num.at<int>(wrapped_row, wrapped_col) = i + 1;
	//	wrapped_mask.at<int>(wrapped_row, wrapped_col) = 1;
	//}
	//grad_updown = wrapped_mask(cv::Range(1, nr), cv::Range(0, nc)) - wrapped_mask(cv::Range(0, nr - 1), cv::Range(0, nc));
	//grad_leftright = wrapped_mask(cv::Range(0, nr), cv::Range(1, nc)) - wrapped_mask(cv::Range(0, nr), cv::Range(0, nc - 1));
	//for (int i = 0; i < grad_updown.rows; i++)
	//{
	//	for (int j = 0; j < grad_updown.cols; j++)
	//	{
	//		if (grad_updown.at<int>(i, j) > 0)
	//		{
	//			start_que.push(wrapped_num.at<int>(i + 1, j));
	//		}
	//		if (grad_updown.at<int>(i, j) < 0)
	//		{
	//			start_que.push(wrapped_num.at<int>(i, j));
	//		}
	//	}
	//}
	//for (int i = 0; i < grad_leftright.rows; i++)
	//{
	//	for (int j = 0; j < grad_leftright.cols; j++)
	//	{
	//		if (grad_leftright.at<int>(i, j) > 0)
	//		{
	//			start_que.push(wrapped_num.at<int>(i, j + 1));
	//		}
	//		if (grad_leftright.at<int>(i, j) < 0)
	//		{
	//			start_que.push(wrapped_num.at<int>(i, j));
	//		}
	//	}
	//}
	//int row_search_start, col_search_start, row_search_end, col_search_end;
	//bool b_continue = true;
	//while (start_que.size() != 0)
	//{
	//	number = start_que.front();
	//	start_que.pop();
	//	if (!nodes[number - 1].get_status())
	//	{
	//		nodes[number - 1].get_pos(&row_unwrap_start, &col_unwrap_start);
	//		nodes[number - 1].get_phase(&phi1);
	//		row_search_start = row_unwrap_start - 1 >= 0 ? row_unwrap_start - 1 : row_unwrap_start;
	//		col_search_start = col_unwrap_start - 1 >= 0 ? col_unwrap_start - 1 : col_unwrap_start;
	//		row_search_end = row_unwrap_start + 1 < nr ? row_unwrap_start - 1 : row_unwrap_start;
	//		col_search_end = col_unwrap_start + 1 < nc ? col_unwrap_start - 1 : col_unwrap_start;
	//		b_continue = true;
	//		for (int i = row_search_start; i <= row_search_end; i++)
	//		{
	//			if (b_continue)
	//			{
	//				for (int j = col_search_start; j <= col_search_end; j++)
	//				{
	//					if (wrapped_num.at<int>(i, j) == 0)//找到附近已经解缠的点
	//					{
	//						grad = atan2(sin(phi1 - unwrapped_phase.at<double>(i, j)), cos(phi1 - unwrapped_phase.at<double>(i, j)));
	//						phi1 = unwrapped_phase.at<double>(i, j) + grad;
	//						nodes[number - 1].set_phase(phi1);
	//						nodes[number - 1].set_status(true);
	//						b_continue = false;
	//						que.push(number);
	//						break;
	//					}
	//				}
	//			}
	//			else
	//			{
	//				break;
	//			}
	//		}
	//	}
	//	
	//}

	//找到已解缠邻接节点，并以已解缠邻接节点为起始点开始解缠

	for (int i = 0; i < num_nodes; i++)
	{
		if (nodes[i].get_status())
		{
			que.push(i + 1);
		}
	}
	int completed_nodes = 0;
	int step = std::max(1, num_nodes / 100);
	while (que.size() != 0)
	{
		number = que.front();
		que.pop();
		for (long edge_val : nodes[number - 1].get_neigh_edges())
		{
			end2 = (edges + edge_val - 1)->end1 == number ?
				(edges + edge_val - 1)->end2 : (edges + edge_val - 1)->end1;
			nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
			if (!nodes[end2 - 1].get_status() &&
				distance <= thresh &&
				fabs((edges + edge_val - 1)->gain) < tt &&
				!(edges + edge_val - 1)->isResidueEdge /*非超过阈值的残差边*/
				)
			{
				que.push(end2);
				nodes[number - 1].get_phase(&phi1);
				nodes[end2 - 1].get_phase(&phi2);
				grad = phi2 - phi1;
				grad = atan2(sin(grad), cos(grad));
				gain = number > end2 ? 2 * PI * (edges + edge_val - 1)->gain : -2 * PI * (edges + edge_val - 1)->gain;
				nodes[end2 - 1].set_phase(grad + phi1 + gain);
				nodes[end2 - 1].set_status(true);
			}
		}
		completed_nodes++;
		if (cb && completed_nodes % step == 0)
		{
			if (!cb(completed_nodes * 100 / num_nodes, "Secondary unwrapping..."))
			{
				return -2;
			}
		}
	}
	
	int row, col;
	for (int i = 0; i < num_nodes; i++)
	{
		if (nodes[i].get_status())
		{
			ret = nodes[i].get_pos(&row, &col);
			ret = nodes[i].get_phase(&phi1);
			unwrapped_phase.at<double>(row, col) = phi1;
		}
	}

	return 0;
}

int Unwrap::McfDelaunayInternal(const char* MCF_problem_file, const char* MCF_EXE_PATH, UnwrapProgressCallback cb)
{
	if (MCF_problem_file == NULL ||
		MCF_EXE_PATH == NULL
		)
	{
		fprintf(stderr, "mcf_delaunay(): input check failed!\n\n");
		return -1;
	}
	std::wstring requestedNetwork;
	PathResolver::Error pathError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(MCF_problem_file, requestedNetwork, &pathError)) return -1;
	const std::wstring requestedSolution = requestedNetwork + L".sol";
	if (GetFileAttributesW(requestedSolution.c_str()) != INVALID_FILE_ATTRIBUTES) return -1;

	string taskFolder;
	std::wstring taskFolderWide;
	if (!createMcfTaskDirectory(MCF_problem_file, taskFolder, taskFolderWide)) return -1;
	ExternalToolResult toolResult;
	ScopedArtifactDirectory artifacts(taskFolderWide, &toolResult);
	const string taskNetwork = taskFolder + "\\mcf_delaunay.net";
	const string taskSolution = taskNetwork + ".sol";
	std::wstring taskNetworkWide;
	std::wstring taskSolutionWide;
	PathResolver::Error artifactError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(taskNetwork, taskNetworkWide, &artifactError) ||
		!PathResolver::utf8ToWide(taskSolution, taskSolutionWide, &artifactError) ||
		!artifacts.registerCandidate(taskNetworkWide) || !artifacts.registerCandidate(taskSolutionWide)) return -1;
	if (!CopyFileW(requestedNetwork.c_str(), taskNetworkWide.c_str(), TRUE) || !artifacts.markOwned(taskNetworkWide))
	{
		toolResult.phase = "prepare input";
		toolResult.win32Error = GetLastError();
		return -1;
	}
	if (!runMcfProcess(MCF_EXE_PATH, taskNetwork.c_str(), "mcf_delaunay(): mcf.exe failed!", cb, &artifacts, &toolResult))
	{
		return -2;
	}
	// The task artifacts are cleaned on scope exit. Publish only the validated result.
	if (!CopyFileW(taskSolutionWide.c_str(), requestedSolution.c_str(), TRUE))
	{
		toolResult.phase = "publish solution";
		toolResult.win32Error = GetLastError();
		return -1;
	}
	return 0;
}

int Unwrap::QualityMap_MCF(Mat& wrapped_phase, Mat& unwrapped_phase, Mat& mask, vector<tri_node>& nodes, tri_edge* edges, int num_edges, int start, bool pass, double thresh, UnwrapProgressCallback cb)
{
	if (wrapped_phase.rows < 2 ||
		wrapped_phase.cols < 2 ||
		wrapped_phase.type() != CV_64F ||
		wrapped_phase.channels() != 1 ||
		mask.rows != wrapped_phase.rows ||
		mask.cols != wrapped_phase.cols ||
		mask.type() != CV_32S ||
		mask.channels() != 1 ||
		nodes.size() < 3 ||
		edges == NULL ||
		num_edges < 1 ||
		start < 1 ||
		start > nodes.size()
		)
	{
		fprintf(stderr, "MCF(): input check failed!\n\n");
		return -1;
	}
	wrapped_phase.copyTo(unwrapped_phase);
	int num_nodes = static_cast<int>(nodes.size());
	int number, ret, end2;
	double distance, grad, phi1, phi2, gain, tt, min_val, max_val;
	min_val = 1000000000.0;
	max_val = -1000000000.0;
	if (pass) tt = 0.5;
	else
	{
		tt = 100000.0;
	}
	priority_queue<edge_index> neighbour_que;
	edge_index tmp_edge_index;
	bool early_break = false;
	if (start > num_nodes) start = 1;
	//////////寻找相关系数最大的边为起始边////////////
	int ix = 0;
	double qua = 100000.0;
	for (int i = 0; i < num_edges; i++)
	{
		if ((edges + i)->quality < qua)
		{
			ix = i;
			qua = (edges + i)->quality;
		}
	}
	start = (edges + ix)->end1;
	nodes[start - 1].set_status(true);
	for (long edge_val : nodes[start - 1].get_neigh_edges())
	{
		end2 = (edges + edge_val - 1)->end1 == start ? (edges + edge_val - 1)->end2 : (edges + edge_val - 1)->end1;
		nodes[start - 1].get_distance(nodes[end2 - 1], &distance);
		if (!nodes[end2 - 1].get_status() &&
			distance <= thresh &&
			fabs((edges + edge_val - 1)->gain) < tt &&
			nodes[end2 - 1].get_balance()
			)
		{
			tmp_edge_index.num = edge_val;
			tmp_edge_index.quality = (edges + edge_val - 1)->quality;
			neighbour_que.push(tmp_edge_index);
		}
	}

	int completed_nodes = 0;
	int step = std::max(1, num_nodes / 100);
	while (neighbour_que.size() != 0)
	{
		tmp_edge_index = neighbour_que.top();
		neighbour_que.pop();
		if (nodes[(edges + tmp_edge_index.num - 1)->end1 - 1].get_status())
		{
			number = (edges + tmp_edge_index.num - 1)->end1;
			end2 = (edges + tmp_edge_index.num - 1)->end2;
		}
		else
		{
			number = (edges + tmp_edge_index.num - 1)->end2;
			end2 = (edges + tmp_edge_index.num - 1)->end1;
		}
		nodes[number - 1].get_phase(&phi1);
		nodes[end2 - 1].get_phase(&phi2);
		grad = phi2 - phi1;
		if (!nodes[end2 - 1].get_status() &&
			fabs((edges + tmp_edge_index.num - 1)->gain) < tt &&
			nodes[end2 - 1].get_balance()
			)
		{
			grad = atan2(sin(grad), cos(grad));
			gain = 0.0;
			min_val = min_val > (grad + phi1 + gain) ? (grad + phi1 + gain) : min_val;
			max_val = max_val < (grad + phi1 + gain) ? (grad + phi1 + gain) : max_val;
			nodes[end2 - 1].set_phase(grad + phi1 + gain);
			nodes[end2 - 1].set_status(true);

			number = end2;
			for (long edge_val : nodes[number - 1].get_neigh_edges())
			{
				end2 = (edges + edge_val - 1)->end1 == number ? (edges + edge_val - 1)->end2 : (edges + edge_val - 1)->end1;
				nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
				if (!nodes[end2 - 1].get_status() &&
					distance <= thresh &&
					fabs((edges + edge_val - 1)->gain) < tt &&
					nodes[end2 - 1].get_balance()
					)
				{
					tmp_edge_index.num = edge_val;
					tmp_edge_index.quality = (edges + edge_val - 1)->quality;
					neighbour_que.push(tmp_edge_index);
				}
			}
		}
		completed_nodes++;
		if (cb && completed_nodes % step == 0)
		{
			if (!cb(completed_nodes * 100 / num_nodes, "Quality guided unwrapping..."))
			{
				return -2;
			}
		}
	}

	int rows, cols;
	int nr = unwrapped_phase.rows;
	int nc = unwrapped_phase.cols;
	double phi;
	Mat _mask = Mat::zeros(nr, nc, CV_32S);
	for (int i = 0; i < num_nodes; i++)
	{
		if (nodes[i].get_status())
		{
			ret = nodes[i].get_pos(&rows, &cols);
			if (rows > nr - 1 || cols > nc - 1 || rows < 0 || cols < 0)
			{
				fprintf(stderr, "MCF(): node posistion exceed legal range!\n");
				return -1;
			}
			ret = nodes[i].get_phase(&phi);
			unwrapped_phase.at<double>(rows, cols) = phi;
			_mask.at<int>(rows, cols) = 1;
		}
	}
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (_mask.at<int>(i, j) < 1)
			{
				unwrapped_phase.at<double>(i, j) = std::numeric_limits<double>::quiet_NaN();
			}
		}
	}
	_mask.copyTo(mask);
	return 0;
}

int Unwrap::_QualityGuided_MCF_1(
	const Mat& wrapped_phase, 
	Mat& unwrapped_phase,
	Mat& out_mask,
	vector<tri_node>& nodes,
	vector<tri_edge>& edges,
	double distance_thresh, 
	bool pass,
	UnwrapProgressCallback cb
)
{
	if (wrapped_phase.empty() ||
		wrapped_phase.type() != CV_64F ||
		nodes.size() < 3 ||
		edges.size() < 3 ||
		distance_thresh < 1.0
		)
	{
		fprintf(stderr, "_Quality_MCF_1(): input check failed!\n");
		return -1;
	}

	wrapped_phase.copyTo(unwrapped_phase);
	int num_nodes = static_cast<int>(nodes.size());
	int number, end2, start;
	// removed unused: tt (threshold logic removed from quality-guided MCF)
	double distance, grad, phi1, phi2, gain, min_val, max_val;
	min_val = 1000000000.0;
	max_val = -1000000000.0;
	priority_queue<edge_index> neighbour_que;
	edge_index tmp_edge_index;
	bool early_break = false;
	size_t num_edges = edges.size();
	//////////寻找相关系数最大的边为起始边(对应quality最小的边)////////////
	int ix = 0;
	double qua = 100000.0;
	for (int i = 0; i < num_edges; i++)
	{
		if (edges[i].quality < qua)
		{
			ix = i;
			qua = edges[i].quality;
		}
	}
	start = edges[ix].end1;

	nodes[start - 1].set_status(true);
	for (long edge_val : nodes[start - 1].get_neigh_edges())
	{
		end2 = edges[edge_val - 1].end1 == start ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
		nodes[start - 1].get_distance(nodes[end2 - 1], &distance);
		if (!nodes[end2 - 1].get_status() &&
			distance <= distance_thresh &&
			!edges[edge_val - 1].isBoundary &&
			nodes[end2 - 1].get_balance() &&
			!edges[edge_val - 1].isResidueEdge
			)
		{
			tmp_edge_index.num = edge_val;
			tmp_edge_index.quality = edges[edge_val - 1].quality;
			neighbour_que.push(tmp_edge_index);
		}
	}

	int completed_nodes = 0;
	int step = std::max(1, num_nodes / 100);
	while (neighbour_que.size() != 0)
	{
		tmp_edge_index = neighbour_que.top();
		neighbour_que.pop();
		if (nodes[edges[tmp_edge_index.num - 1].end1 - 1].get_status())
		{
			number = edges[tmp_edge_index.num - 1].end1;
			end2 = edges[tmp_edge_index.num - 1].end2;
		}
		else
		{
			number = edges[tmp_edge_index.num - 1].end2;
			end2 = edges[tmp_edge_index.num - 1].end1;
		}
		if (!nodes[end2 - 1].get_status() &&
			!edges[tmp_edge_index.num - 1].isBoundary &&
			nodes[end2 - 1].get_balance() && 
			!edges[tmp_edge_index.num - 1].isResidueEdge
			)
		{
			nodes[number - 1].get_phase(&phi1);
			nodes[end2 - 1].get_phase(&phi2);
			grad = phi2 - phi1;
			grad = atan2(sin(grad), cos(grad));
			gain = 0.0;
			//min_val = min_val > (grad + phi1 + gain) ? (grad + phi1 + gain) : min_val;
			//max_val = max_val < (grad + phi1 + gain) ? (grad + phi1 + gain) : max_val;
			nodes[end2 - 1].set_phase(grad + phi1 + gain);
			nodes[end2 - 1].set_status(true);


			number = end2;
			for (long edge_val : nodes[number - 1].get_neigh_edges())
			{
				end2 = edges[edge_val - 1].end1 == number ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
				nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
				if (!nodes[end2 - 1].get_status() &&
					distance <= distance_thresh &&
					!edges[edge_val - 1].isBoundary &&
					nodes[end2 - 1].get_balance()&&
					!edges[edge_val - 1].isResidueEdge
					)
				{
					tmp_edge_index.num = edge_val;
					tmp_edge_index.quality = edges[edge_val - 1].quality;
					neighbour_que.push(tmp_edge_index);
				}
			}
		}
		completed_nodes++;
		if (cb && completed_nodes % step == 0)
		{
			if (!cb(completed_nodes * 100 / num_nodes, "Quality guided unwrapping step 1..."))
			{
				return -2;
			}
		}
	}

	
	int nr = unwrapped_phase.rows;
	int nc = unwrapped_phase.cols;
	// removed unused: phi (copy-paste remnant, phi3 used in parallel loop below)
	Mat _mask = Mat::zeros(nr, nc, CV_32S);
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < num_nodes; i++)
	{
		int rows, cols; double phi3;
		if (nodes[i].get_status())
		{
			nodes[i].get_pos(&rows, &cols);
			nodes[i].get_phase(&phi3);
			unwrapped_phase.at<double>(rows, cols) = phi3;
			_mask.at<int>(rows, cols) = 1;
		}
	}
//#pragma omp parallel for schedule(guided)
//	for (int i = 0; i < nr; i++)
//	{
//		for (int j = 0; j < nc; j++)
//		{
//			if (_mask.at<int>(i, j) < 1)
//			{
//				unwrapped_phase.at<double>(i, j) = min_val - 0.1 * (max_val - min_val);
//			}
//		}
//	}
	_mask.copyTo(out_mask);

	return 0;
}

int Unwrap::_QualityGuided_MCF_2(
	Mat& unwrapped_phase,
	vector<tri_node>& nodes, 
	vector<tri_edge>& edges,
	double distance_thresh,
	UnwrapProgressCallback cb
)
{
	if (unwrapped_phase.rows < 2 ||
		unwrapped_phase.cols < 2 ||
		unwrapped_phase.type() != CV_64F ||
		unwrapped_phase.channels() != 1 ||
		nodes.size() < 3 ||
		edges.size() < 3 
		)
	{
		fprintf(stderr, "_QualityGuided_MCF_2(): input check failed!\n\n");
		return -1;
	}
	int num_nodes = static_cast<int>(nodes.size());
	int number, ret, end2;
	// removed unused: row_start, col_start (planned start position tracking, never implemented)
	// removed unused: tt (threshold logic not used in this quality-guided variant)
	double distance, grad, phi1, phi2, gain;
	queue<int> que;
	//queue<int> start_que;
	int nr = unwrapped_phase.rows;
	int nc = unwrapped_phase.cols;

	//找到已解缠邻接节点，并以已解缠邻接节点为起始点开始解缠

	for (int i = 0; i < num_nodes; i++)
	{
		if (nodes[i].get_status())
		{
			que.push(i + 1);
		}
	}
	int completed_nodes = 0;
	int step = std::max(1, num_nodes / 100);
	while (que.size() != 0)
	{
		number = que.front();
		que.pop();
		for (long edge_val : nodes[number - 1].get_neigh_edges())
		{
			end2 = edges[edge_val - 1].end1 == number ?
				edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
			nodes[number - 1].get_distance(nodes[end2 - 1], &distance);
			if (!nodes[end2 - 1].get_status() &&
				distance <= distance_thresh &&
				!edges[edge_val - 1].isBoundary
				)
			{
				que.push(end2);
				nodes[number - 1].get_phase(&phi1);
				nodes[end2 - 1].get_phase(&phi2);
				grad = phi2 - phi1;
				grad = atan2(sin(grad), cos(grad));
				gain = number < end2 ? 2 * PI * edges[edge_val - 1].gain : -2 * PI * edges[edge_val - 1].gain;
				nodes[end2 - 1].set_phase(grad + phi1 + gain);
				nodes[end2 - 1].set_status(true);
			}
		}
		completed_nodes++;
		if (cb && completed_nodes % step == 0)
		{
			if (!cb(completed_nodes * 100 / num_nodes, "Quality guided unwrapping step 2..."))
			{
				return -2;
			}
		}
	}

#pragma omp parallel for schedule(guided)
	for (int i = 0; i < num_nodes; i++)
	{
		int row, col; double phi3;
		if (nodes[i].get_status())
		{
			ret = nodes[i].get_pos(&row, &col);
			ret = nodes[i].get_phase(&phi3);
			unwrapped_phase.at<double>(row, col) = phi3;
		}
	}

	return 0;
}

int Unwrap::QualityGuidedMCFInternal(
	const Mat& wrapped_phase,
	Mat& unwrapped_phase, 
	double coherence_thresh,
	double distance_thresh,
	const char* tmp_path, 
	const char* EXE_path,
	UnwrapProgressCallback cb
)
{
	if (wrapped_phase.empty() ||
		tmp_path == NULL ||
		EXE_path == NULL ||
		coherence_thresh < 0.0 ||
		coherence_thresh > 1.0
		)
	{
		fprintf(stderr, "QualityGuided_MCF(): input check failed!\n");
		return -1;
	}
	distance_thresh = distance_thresh < 2.0 ? 2.0 : distance_thresh;
	Mat coherence, phase, mask, quality_index;
	Utils util;
	int ret, nr, nc, count = 0;
	nr = wrapped_phase.rows; nc = wrapped_phase.cols;
	wrapped_phase.copyTo(phase);
	ret = util.phase_coherence(phase, 3, 3, coherence);
	if (return_check(ret, "phase_coherence()", error_head)) return -1;
	quality_index = 1 - coherence;
	//mask = Mat::zeros(nr, nc, CV_32S);

	ret = util.gen_mask(coherence, mask, 7, coherence_thresh);
	if (return_check(ret, "gen_mask()", error_head)) return -1;
	count = cv::countNonZero(mask);
	if (count < 100)//高质量像素小于100，直接使用规则网络的MCF
	{
		Mat residue;
		ret = util.residue(phase, residue);
		if (return_check(ret, "residue()", error_head)) return -1;
		string mcf_problem_file(tmp_path);
		mcf_problem_file.append("\\mcf_problem.net");
		ret = MCFInternal(phase, unwrapped_phase, coherence, residue, mcf_problem_file.c_str(), EXE_path, cb);
		if (ret == -2) return -2;
		if (return_check(ret, "MCF()", error_head)) return -1;
		return 0;
	}

	string folder;
	std::wstring taskFolderWide;
	if (!createTaskDirectory(tmp_path, folder, taskFolderWide)) return -1;
	ExternalToolResult toolResult;
	ScopedArtifactDirectory artifacts(taskFolderWide, &toolResult);
	string node_file = folder + "\\triangle.node";
	string generated_node_file = folder + "\\triangle.1.node";
	string edge_file = folder + "\\triangle.1.edge";
	string ele_file = folder + "\\triangle.1.ele";
	string neigh_file = folder + "\\triangle.1.neigh";
	string mcf_problem = folder + "\\mcf_delaunay.net";
	string mcf_solution = folder + "\\mcf_delaunay.net.sol";
	std::wstring nodeFileWide, generatedNodeFileWide, edgeFileWide, eleFileWide, neighFileWide, mcfProblemWide, mcfSolutionWide;
	PathResolver::Error artifactError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(node_file, nodeFileWide, &artifactError) ||
		!PathResolver::utf8ToWide(generated_node_file, generatedNodeFileWide, &artifactError) ||
		!PathResolver::utf8ToWide(edge_file, edgeFileWide, &artifactError) ||
		!PathResolver::utf8ToWide(ele_file, eleFileWide, &artifactError) ||
		!PathResolver::utf8ToWide(neigh_file, neighFileWide, &artifactError) ||
		!PathResolver::utf8ToWide(mcf_problem, mcfProblemWide, &artifactError) ||
		!PathResolver::utf8ToWide(mcf_solution, mcfSolutionWide, &artifactError) ||
		!artifacts.registerCandidate(nodeFileWide) || !artifacts.registerCandidate(generatedNodeFileWide) ||
		!artifacts.registerCandidate(edgeFileWide) || !artifacts.registerCandidate(eleFileWide) ||
		!artifacts.registerCandidate(neighFileWide) || !artifacts.registerCandidate(mcfProblemWide) ||
		!artifacts.registerCandidate(mcfSolutionWide)) return -1;
	auto clearDelaunayOutputs = [&]() {
		return artifacts.clearOwned(generatedNodeFileWide) && artifacts.clearOwned(edgeFileWide) &&
			artifacts.clearOwned(eleFileWide) && artifacts.clearOwned(neighFileWide);
	};
	vector<tri_node> nodes, nodes_sub; vector<tri_edge> edges, edges_sub; vector<triangle> tri, tri_sub;
	vector<int> node_neighbour, node_neighbour_sub;
	long num_nodes = count;
	ret = util.write_node_file(node_file.c_str(), mask);
	if (GetFileAttributesW(nodeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(nodeFileWide);
	if (return_check(ret, "write_node_file()", error_head)) return -1;
	if (!clearDelaunayOutputs()) return -1;
	ret = util.gen_delaunay(node_file.c_str(), EXE_path);
	if (GetFileAttributesW(generatedNodeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(generatedNodeFileWide);
	if (GetFileAttributesW(edgeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(edgeFileWide);
	if (GetFileAttributesW(eleFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(eleFileWide);
	if (GetFileAttributesW(neighFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(neighFileWide);
	if (return_check(ret, "gen_delaunay()", error_head)) return -1;
	ret = util.read_edges(edge_file.c_str(), edges, node_neighbour, num_nodes);
	if (return_check(ret, "read_edges()", error_head)) return -1;
	ret = util.init_tri_node(nodes, phase, mask, edges, node_neighbour, num_nodes);
	if (return_check(ret, "init_tri_node()", error_head)) return -1;
	ret = util.init_edges_quality(quality_index, edges, nodes);
	if (return_check(ret, "init_edges_quality()", error_head)) return -1;
	ret = util.read_triangle(ele_file.c_str(), neigh_file.c_str(), tri, nodes, edges);
	if (return_check(ret, "read_triangle()", error_head)) return -1;
	ret = util.residue(tri, nodes, edges, distance_thresh);
	if (return_check(ret, "residue()", error_head)) return -1;

	Mat out_mask;
	ret = _QualityGuided_MCF_1(phase, unwrapped_phase, out_mask, nodes, edges, distance_thresh, true, cb);
	if (ret == -2) return -2;
	if (return_check(ret, "residue()", error_head)) return -1;

	out_mask.convertTo(mask, CV_64F);
	// 调试保存中间数据（若需本地调试，可取消注释并修改为自己的本地路径）
	// util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\out_mask.bin", mask);
	out_mask.convertTo(mask, CV_32S);
	// util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\unwrapped_phase1.bin", unwrapped_phase);

	Mat mask_2 = Mat::zeros(nr, nc, CV_32S);
	count = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (out_mask.at<int>(i, j) == 0)
			{
				mask_2.at<int>(i, j) = 1; count++;
			}
		}
	}
	Mat _mask_sentinel; mask_2.copyTo(_mask_sentinel);//质量图解缠未解出的区域掩膜
	if (count == 0) return 0;
	//找出邻接已解缠节点
	Mat grad_updown, grad_leftright;
	grad_updown = mask_2(cv::Range(1, nr), cv::Range(0, nc)) - mask_2(cv::Range(0, nr - 1), cv::Range(0, nc));
	grad_leftright = mask_2(cv::Range(0, nr), cv::Range(1, nc)) - mask_2(cv::Range(0, nr), cv::Range(0, nc - 1));
	for (int i = 0; i < grad_updown.rows; i++)
	{
		for (int j = 0; j < grad_updown.cols; j++)
		{
			if (grad_updown.at<int>(i, j) > 0)
			{
				mask_2.at<int>(i, j) = 2;//2代表邻接已解缠节点
			}
			if (grad_updown.at<int>(i, j) < 0)
			{
				mask_2.at<int>(i + 1, j) = 2;
			}
		}
	}
	for (int i = 0; i < grad_leftright.rows; i++)
	{
		for (int j = 0; j < grad_leftright.cols; j++)
		{
			if (grad_leftright.at<int>(i, j) > 0)
			{
				mask_2.at<int>(i, j) = 2;
			}
			if (grad_leftright.at<int>(i, j) < 0)
			{
				mask_2.at<int>(i, j + 1) = 2;
			}
		}
	}

	/*
	* 找到未解缠的点以及其相邻已解缠的点，形成三角网络
	加入总的队列wrapped_que，已解缠的边缘点加入到unwrapped_neighbour_que
	*/
	queue<int> wrapped_que, unwrapped_neighbour_que, low_quality_que;
	num_nodes = cv::countNonZero(mask_2);
	ret = util.write_node_file(node_file.c_str(), mask_2);
	if (GetFileAttributesW(nodeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(nodeFileWide);
	if (return_check(ret, "write_node_file()", error_head)) return -1;
	if (!clearDelaunayOutputs()) return -1;
	ret = util.gen_delaunay(node_file.c_str(), EXE_path);
	if (GetFileAttributesW(generatedNodeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(generatedNodeFileWide);
	if (GetFileAttributesW(edgeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(edgeFileWide);
	if (GetFileAttributesW(eleFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(eleFileWide);
	if (GetFileAttributesW(neighFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(neighFileWide);
	if(return_check(ret, "gen_delaunay()", error_head)) return -1;
	ret = util.read_edges(edge_file.c_str(), edges, node_neighbour, num_nodes);
	if (return_check(ret, "read_edges()", error_head)) return -1;
	ret = util.init_tri_node(nodes, unwrapped_phase, mask_2, edges, node_neighbour, num_nodes);
	if (return_check(ret, "init_tri_node()", error_head)) return -1;

	// removed unused: end1 (struct member .end1 accessed directly, local var never needed)
	int start, end2, ambig, i;
	int row, col, num_triangle, positive, negative;
	double distance, phi, cluster_distance_thresh = 1.2;//低质量聚类距离阈值
	Mat zeros = Mat::zeros(nr, nc, CV_32S);
	Mat ambiguity, new_mask;
	int  mask_sentinel_new = cv::countNonZero(_mask_sentinel);
	int mask_sentinel_total = mask_sentinel_new;
	while (mask_sentinel_new != 0)//只要还存在未解缠的像素就继续循环
	{
		if (cb)
		{
			int progress = mask_sentinel_total > 0 ? (mask_sentinel_total - mask_sentinel_new) * 100 / mask_sentinel_total : 100;
			if (!cb(progress, "Quality guided MCF loop..."))
			{
				return -2;
			}
		}
		for (int i = 0; i < nodes.size(); i++)
		{
			nodes[i].get_pos(&row, &col);
			if (_mask_sentinel.at<int>(row, col) == 1)
			{
				wrapped_que.push(i + 1);
				break;
			}
		}
		//mask_sentinel_old = cv::countNonZero(_mask_sentinel);
		zeros.copyTo(new_mask);//循环前将new_mask清零
		while (!wrapped_que.empty())//寻找低质量点cluster
		{
			
			start = wrapped_que.front();
			wrapped_que.pop();
			if (nodes[start - 1].get_status())
			{
				unwrapped_neighbour_que.push(start);
			}
			nodes[start - 1].get_pos(&row, &col);//设置新的mask
			new_mask.at<int>(row, col) = 1;
			_mask_sentinel.at<int>(row, col) = 0;//未解缠的像素掩膜更新
			nodes[start - 1].set_balance(false);//已加入队列设置为不平衡，避免重复加入队列
			for (long edge_val : nodes[start - 1].get_neigh_edges())
			{
				end2 = edges[edge_val - 1].end1 == start ? edges[edge_val - 1].end2 : edges[edge_val - 1].end1;
				nodes[start - 1].get_distance(nodes[end2 - 1], &distance);
				if (
					distance <= cluster_distance_thresh && //小于低质量聚类距离阈值则为同一类
					nodes[end2 - 1].get_balance()
					)
				{
					wrapped_que.push(end2);
					nodes[end2 - 1].set_balance(false);//已加入队列设置为不平衡，避免重复加入队列
				}
			}
		}

		//new_mask.convertTo(new_mask, CV_64F);
		//util.cvmat2bin("E:\\working_dir\\projects\\software\\InSAR\\bin\\out_mask.bin", new_mask);
		//new_mask.convertTo(new_mask, CV_32S);

		ambiguity = Mat::zeros(1, static_cast<int>(unwrapped_neighbour_que.size()), CV_32S);
		num_nodes = cv::countNonZero(new_mask);
		ret = util.write_node_file(node_file.c_str(), new_mask);
		if (GetFileAttributesW(nodeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(nodeFileWide);
		if (return_check(ret, "write_node_file()", error_head)) return -1;
		if (!clearDelaunayOutputs()) return -1;
		ret = util.gen_delaunay(node_file.c_str(), EXE_path);
		if (GetFileAttributesW(generatedNodeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(generatedNodeFileWide);
		if (GetFileAttributesW(edgeFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(edgeFileWide);
		if (GetFileAttributesW(eleFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(eleFileWide);
		if (GetFileAttributesW(neighFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(neighFileWide);
		if (return_check(ret, "gen_delaunay()", error_head)) return -1;
		ret = util.read_edges(edge_file.c_str(), edges_sub, node_neighbour_sub, num_nodes);
		if (return_check(ret, "read_edges()", error_head)) return -1;
		ret = util.init_tri_node(nodes_sub, wrapped_phase, new_mask, edges_sub, node_neighbour_sub, num_nodes);
		if (return_check(ret, "init_tri_node()", error_head)) return -1;
		ret = util.read_triangle(ele_file.c_str(), neigh_file.c_str(), tri_sub, nodes_sub, edges_sub);
		if (return_check(ret, "read_triangle()", error_head)) return -1;
		ret = util.residue(tri_sub, nodes_sub, edges_sub, 1000.0);
		if (return_check(ret, "residue()", error_head)) return -1;
		/*
		* 检查残差点数，若无残差点则不使用mcf.exe求解
		*/
		num_triangle = static_cast<int>(tri_sub.size()); positive = 0; negative = 0;
		for (int ii = 0; ii < num_triangle; ii++)
		{
			if (tri_sub[ii].residue > 0.7)
			{
				positive++;
			}
			if (tri_sub[ii].residue < -0.7)
			{
				negative++;
			}
		}
		if (positive == 0 && negative == 0)
		{

			ret = MCF(wrapped_phase, unwrapped_phase, out_mask, new_mask, nodes_sub, edges_sub, 1, false, distance_thresh, cb);
			if (ret == -2) return -2;
			if (return_check(ret, "MCF()", error_head)) return -1;
		}
		else
		{
			ret = util.write_DIMACS(mcf_problem.c_str(), tri_sub, nodes_sub, edges_sub, coherence);
			if (GetFileAttributesW(mcfProblemWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(mcfProblemWide);
			if (return_check(ret, "write_DIMACS()", error_head)) return -1;
			if (!runMcfProcess(EXE_path, mcf_problem.c_str(), "mcf_delaunay(): mcf.exe failed!", cb, &artifacts, &toolResult)) ret = -2;
			else ret = 0;
			if (ret == -2) return -2;
			if (return_check(ret, "mcf_delaunay()", error_head)) return -1;
			ret = util.read_DIMACS(mcf_solution.c_str(), edges_sub, nodes_sub, tri_sub);
			if (return_check(ret, "read_DIMACS()", error_head)) return -1;
			ret = MCF(wrapped_phase, unwrapped_phase, out_mask, new_mask, nodes_sub, edges_sub, 1, false, distance_thresh, cb);
			if (ret == -2) return -2;
			if (return_check(ret, "MCF()", error_head)) return -1;
		}

		//校正模糊数
		i = 0;
		while (!unwrapped_neighbour_que.empty())
		{
			end2 = unwrapped_neighbour_que.front();
			unwrapped_neighbour_que.pop();
			nodes[end2 - 1].get_pos(&row, &col);
			nodes[end2 - 1].get_phase(&phi);
			ambig = (int)round((phi - unwrapped_phase.at<double>(row, col)) / (2 * 3.141592653589793238));
			ambiguity.at<int>(0, i++) = ambig;
		}
		ret = util.get_mode_index(ambiguity, &ambig);
		//校正相位
		for (int i = 0; i < nodes_sub.size(); i++)
		{
			if (nodes_sub[i].get_status())
			{
				nodes_sub[i].get_phase(&phi);
				nodes_sub[i].get_pos(&row, &col);
				phi += (double)ambig * 2 * 3.141592653589793238;
				nodes_sub[i].set_phase(phi);
				unwrapped_phase.at<double>(row, col) = phi;
			}
		}
		mask_sentinel_new = cv::countNonZero(_mask_sentinel);
	}
	


	return 0;
}

int Unwrap::SnaphuFileInternal(
	const char* wrapped_phase_file,
	Mat& unwrapped_phase,
	const char* project_path,
	const char* tmp_folder,
	const char* exe_path,
	UnwrapProgressCallback cb
)
{
	if (wrapped_phase_file == NULL || 
		project_path == NULL ||
		tmp_folder == NULL ||
		exe_path == NULL)
	{
		fprintf(stderr, "snaphu(): input check failed!\n");
		return -1;
	}

	FormatConversion conversion;
	Utils util;
	int ret, nr, nc;
	double B_effect, B_parallel;
	Mat wrapped_phase, coherence, amplitude1, amplitude2, lon_coef, lat_coef, state_vec1, state_vec2, prf1, prf2,
		carrier_frequency, offset_row, offset_col;
	ComplexMat master, slave;
	string EXE_path(exe_path);
	std::replace(EXE_path.begin(), EXE_path.end(), '/', '\\');
	string source_1, source_2;
	string folder;
	std::wstring taskFolderWide;
	if (!createTaskDirectory(tmp_folder, folder, taskFolderWide))
	{
		fprintf(stderr, "snaphu(): cannot create a unique task directory.\n");
		return -1;
	}
	ExternalToolResult toolResult;
	ScopedArtifactDirectory artifacts(taskFolderWide, &toolResult);
	string config_file = folder + "\\snaphu.config";
	string ampfile1 = folder + "\\ampfile1.dat";
	string ampfile2 = folder + "\\ampfile2.dat";
	string coherence_file = folder + "\\coherence.dat";
	string IN_file = folder + "\\wrapped_phase.dat";
	string OUT_file = folder + "\\unwrapped_phase.dat";
	std::wstring configFileWide, ampfile1Wide, ampfile2Wide, coherenceFileWide, inFileWide, outFileWide;
	PathResolver::Error artifactPathError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(config_file, configFileWide, &artifactPathError) ||
		!PathResolver::utf8ToWide(ampfile1, ampfile1Wide, &artifactPathError) ||
		!PathResolver::utf8ToWide(ampfile2, ampfile2Wide, &artifactPathError) ||
		!PathResolver::utf8ToWide(coherence_file, coherenceFileWide, &artifactPathError) ||
		!PathResolver::utf8ToWide(IN_file, inFileWide, &artifactPathError) ||
		!PathResolver::utf8ToWide(OUT_file, outFileWide, &artifactPathError) ||
		!artifacts.registerCandidate(configFileWide) || !artifacts.registerCandidate(ampfile1Wide) ||
		!artifacts.registerCandidate(ampfile2Wide) || !artifacts.registerCandidate(coherenceFileWide) ||
		!artifacts.registerCandidate(inFileWide) || !artifacts.registerCandidate(outFileWide))
	{
		fprintf(stderr, "snaphu(): unable to register task artifacts (%s).\n", PathResolver::errorMessage(artifactPathError));
		return -1;
	}

	ret = conversion.read_array_from_h5(wrapped_phase_file, "phase", wrapped_phase);
	if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
	nr = wrapped_phase.rows; nc = wrapped_phase.cols;
	int multilookRg = 1;
	int multilookAz = 1;
	auto readMultilookFactor = [&](const char* dataset, int& factor) -> int
	{
		Mat value;
		if (conversion.read_array_from_h5(wrapped_phase_file, dataset, value) < 0) return 0;
		if (value.total() != 1 || value.channels() != 1)
		{
			fprintf(stderr, "snaphu(): invalid %s metadata shape.\n", dataset);
			return -1;
		}
		Mat numeric;
		value.convertTo(numeric, CV_64F);
		const double rawValue = numeric.at<double>(0, 0);
		if (!std::isfinite(rawValue) || rawValue < 1.0 ||
			rawValue > static_cast<double>(std::numeric_limits<int>::max()) ||
			std::floor(rawValue) != rawValue)
		{
			fprintf(stderr, "snaphu(): invalid %s metadata value.\n", dataset);
			return -1;
		}
		factor = static_cast<int>(rawValue);
		return 1;
	};
	const int multilookRgStatus = readMultilookFactor("multilook_rg", multilookRg);
	const int multilookAzStatus = readMultilookFactor("multilook_az", multilookAz);
	if (multilookRgStatus < 0 || multilookAzStatus < 0) return -1;
	if (multilookRgStatus == 0 || multilookAzStatus == 0)
	{
		multilookRg = 1;
		multilookAz = 1;
		fprintf(stderr, "snaphu(): multilook metadata unavailable; source amplitudes will not be resampled.\n");
	}
	enum class CorrelationSource { InputH5, PhaseDerived, Disabled };
	enum class AmplitudeStatus { Used, Unavailable, OmittedDimensionMismatch };
	CorrelationSource correlationSource = CorrelationSource::Disabled;
	AmplitudeStatus amplitudeStatus = AmplitudeStatus::Unavailable;
	std::string correlationReason;
	//估计基线
	bool b_baseline = true;
	bool b_source = true;
	bool amplitudeAvailable = false;
	PathResolver::SourcePathPair sourcePaths;
	PathResolver::Error pathError = PathResolver::Error::None;
	string pathDetail;
	if (!PathResolver::readSourcePathPair(wrapped_phase_file, project_path, sourcePaths, &pathError, &pathDetail))
	{
		fprintf(stderr, "snaphu(): %s (%s)\n", PathResolver::errorMessage(pathError), pathDetail.c_str());
		return -1;
	}
	source_1 = sourcePaths.source1.utf8;
	source_2 = sourcePaths.source2.utf8;

	const int inputCoherenceReadStatus = conversion.read_array_from_h5(wrapped_phase_file, "coherence", coherence);
	if (inputCoherenceReadStatus == 0)
	{
		if (validateCorrelation(coherence, nr, nc, correlationReason))
		{
			coherence.convertTo(coherence, CV_32F);
			if (!writeFloatRaster(coherenceFileWide, coherence, &artifacts)) return -1;
			correlationSource = CorrelationSource::InputH5;
		}
		else
		{
			fprintf(stderr, "snaphu(): input_coherence_rejected=%s\n", correlationReason.c_str());
		}
	}
	else
	{
		correlationReason = "input_missing_or_unreadable";
		fprintf(stderr, "snaphu(): input_coherence_unavailable=%s\n", correlationReason.c_str());
	}
	if (b_source)
	{
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "lon_coefficient", lon_coef)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "lat_coefficient", lat_coef)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "offset_row", offset_row)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "offset_col", offset_col)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "carrier_frequency", carrier_frequency)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "prf", prf1)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_1.c_str(), "state_vec", state_vec1)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_2.c_str(), "prf", prf2)) b_baseline = false;
		if (0 > conversion.read_array_from_h5(source_2.c_str(), "state_vec", state_vec2)) b_baseline = false;

		const bool masterRead = conversion.read_slc_from_h5(source_1.c_str(), master) >= 0;
		const bool slaveRead = conversion.read_slc_from_h5(source_2.c_str(), slave) >= 0;
		amplitudeAvailable = masterRead && slaveRead;
		if (!amplitudeAvailable)
		{
			fprintf(stderr, "snaphu(): source amplitude unavailable; omitting amplitude files.\n");
		}
	}
	if (b_source && b_baseline)
	{
		ret = util.baseline_estimation(state_vec1, state_vec2, lon_coef, lat_coef, offset_row.at<int>(0, 0),
			offset_col.at<int>(0, 0), nr, nc, 1.0 / prf1.at<double>(0, 0), 1.0 / prf2.at<double>(0, 0), &B_effect, &B_parallel);
		if (ret < 0) b_baseline = false;
	}
	Mat phase;
	wrapped_phase.convertTo(phase, CV_32F);
	if (!writeFloatRaster(inFileWide, phase, &artifacts)) return -1;
	if (b_source && amplitudeAvailable)//有幅度信息
	{
		if (master.type() != CV_64F) master.convertTo(master, CV_64F);
		amplitude1 = master.GetMod();
		amplitude1.convertTo(amplitude1, CV_32F);

		if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
		amplitude2 = slave.GetMod();
		amplitude2.convertTo(amplitude2, CV_32F);
		if (multilookRg > 1 || multilookAz > 1)
		{
			Mat multilookedAmplitude1, multilookedAmplitude2;
			ret = util.multilook_SAR(amplitude1, multilookedAmplitude1, multilookRg, multilookAz, cb);
			if (ret != 0) return ret;
			ret = util.multilook_SAR(amplitude2, multilookedAmplitude2, multilookRg, multilookAz, cb);
			if (ret != 0) return ret;
			amplitude1 = multilookedAmplitude1;
			amplitude2 = multilookedAmplitude2;
		}

		if (amplitude1.rows != nr || amplitude1.cols != nc || amplitude2.rows != nr || amplitude2.cols != nc)
		{
			fprintf(stderr, "snaphu(): source amplitude dimensions (%d x %d, %d x %d) do not match wrapped phase (%d x %d); omitting amplitude files.\n",
				amplitude1.rows, amplitude1.cols, amplitude2.rows, amplitude2.cols, nr, nc);
			amplitudeStatus = AmplitudeStatus::OmittedDimensionMismatch;
		}
		else
		{
			if (!writeFloatRaster(ampfile1Wide, amplitude1, &artifacts)) return -1;
			if (!writeFloatRaster(ampfile2Wide, amplitude2, &artifacts)) return -1;
			amplitudeStatus = AmplitudeStatus::Used;
		}
	}
	if (correlationSource != CorrelationSource::InputH5)
	{
		Mat phaseDerivedCoherence;
		ret = util.phase_coherence(wrapped_phase, phaseDerivedCoherence);
		if (ret < 0)
		{
			correlationReason = "phase_derived_failed";
			fprintf(stderr, "snaphu(): phase-derived coherence unavailable.\n");
		}
		else
		{
			std::string phaseDerivedReason;
			if (!validateCorrelation(phaseDerivedCoherence, nr, nc, phaseDerivedReason))
			{
				correlationReason = "phase_derived_validation_failed(" + phaseDerivedReason + ")";
				fprintf(stderr, "snaphu(): phase_derived_coherence_rejected=%s\n", phaseDerivedReason.c_str());
			}
			else
			{
				phaseDerivedCoherence.convertTo(coherence, CV_32F);
				if (!writeFloatRaster(coherenceFileWide, coherence, &artifacts)) return -1;
				correlationSource = CorrelationSource::PhaseDerived;
			}
		}
	}



	// SNAPHU consumes this configuration as UTF-8; every pathname is UTF-8.
	std::string configInFile, configOutFile, configCoherenceFile, configAmpfile1, configAmpfile2;
	if (!quoteSnaphuConfigPath(IN_file, configInFile) || !quoteSnaphuConfigPath(OUT_file, configOutFile) ||
		!quoteSnaphuConfigPath(coherence_file, configCoherenceFile) || !quoteSnaphuConfigPath(ampfile1, configAmpfile1) ||
		!quoteSnaphuConfigPath(ampfile2, configAmpfile2)) return -1;
	std::ostringstream config;
	config << "INFILEFORMAT FLOAT_DATA\nOUTFILEFORMAT FLOAT_DATA\nCORRFILEFORMAT FLOAT_DATA\nAMPFILEFORMAT FLOAT_DATA\n";
	config << "LINELENGTH " << nc << "\nINFILE " << configInFile << "\nOUTFILE " << configOutFile << "\n";
	if (correlationSource != CorrelationSource::Disabled) config << "CORRFILE " << configCoherenceFile << "\n";
	if (amplitudeStatus == AmplitudeStatus::Used)
	{
		config << "AMPFILE1 " << configAmpfile1 << "\nAMPFILE2 " << configAmpfile2 << "\n";
	}
	if (b_source && b_baseline)
	{
		B_parallel = sqrt(B_effect * B_effect + B_parallel * B_parallel);
		config << "BASELINE " << B_parallel << "\nBPERP " << B_effect << "\nLAMBDA " << 3e8 / carrier_frequency.at<double>(0, 0) << "\n";
	}
	if (b_source)
	{
		Mat DR, DA;
		if (0 == conversion.read_array_from_h5(source_1.c_str(), "range_spacing", DR))
		{
			config << "DR " << DR.at<double>(0, 0) << "\n";
		}
		if (0 == conversion.read_array_from_h5(source_1.c_str(), "azimuth_spacing", DA))
		{
			config << "DA " << DA.at<double>(0, 0) << "\n";
		}
		if (0 == conversion.read_array_from_h5(source_1.c_str(), "range_resolution", DR))
		{
			config << "RANGERES " << DR.at<double>(0, 0) << "\n";
		}
		if (0 == conversion.read_array_from_h5(source_1.c_str(), "azimuth_resolution", DA))
		{
			config << "AZRES " << DA.at<double>(0, 0) << "\n";
		}
	}
	if (!appendSnaphuTilingConfig(config, taskFolderWide, nr, nc, toolResult)) return -1;

	const std::string configText = config.str();
	if (!writeBytes(configFileWide, configText.data(), configText.size(), &artifacts)) return -1;
	if (!emitSnaphuPreparedEvent(taskFolderWide, configFileWide))
	{
		toolResult.cancelled = true;
		toolResult.phase = "cancel";
		toolResult.validationFailure = "SNAPHU prepared callback cancelled or staging path is too long";
		return -2;
	}

	//////////////////////////创建并调用snaphu.exe进程///////////////////////////////
	if (!runExternalProcessUtf8(EXE_path, L"snaphu.exe", { L"-f", configFileWide },
		"SNAPHU", "snaphu(): snaphu.exe failed!", cb, &toolResult))
	{
		if (GetFileAttributesW(outFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(outFileWide);
		return -2;
	}

	// A zero exit code is insufficient: reject stale, truncated, NaN and Inf output.
	if (!artifacts.markOwned(outFileWide)) return -1;
	std::string outputFailure;
	if (!readValidatedFloatRaster(outFileWide, nr, nc, unwrapped_phase, &outputFailure))
	{
		toolResult.phase = "validate output";
		toolResult.validationFailure = outputFailure;
		fprintf(stderr, "snaphu(): invalid output (%s).\n", outputFailure.c_str());
		return -1;
	}
	unwrapped_phase.convertTo(unwrapped_phase, CV_64F);
	if (g_activeDiagnostic)
	{
		std::ostringstream summary;
		summary << "completed; corr=";
		if (correlationSource == CorrelationSource::InputH5)
		{
			summary << "input_h5(" << nr << "x" << nc << ")";
		}
		else if (correlationSource == CorrelationSource::PhaseDerived)
		{
			summary << "phase_derived(" << nr << "x" << nc << "; reason=" << correlationReason << ")";
		}
		else
		{
			summary << "disabled(reason=" << correlationReason << ")";
		}
		summary << "; amp=";
		if (amplitudeStatus == AmplitudeStatus::Used)
		{
			summary << "used(" << nr << "x" << nc << ")";
		}
		else if (amplitudeStatus == AmplitudeStatus::OmittedDimensionMismatch)
		{
			summary << "omitted_dimension_mismatch(" << amplitude1.rows << "x" << amplitude1.cols << ","
				<< amplitude2.rows << "x" << amplitude2.cols << "; expected=" << nr << "x" << nc << ")";
		}
		else
		{
			summary << "unavailable";
		}
		copyDiagnosticText(g_activeDiagnostic->summary, sizeof(g_activeDiagnostic->summary), summary.str());
		g_activeDiagnostic->stage = UNWRAP_DIAGNOSTIC_STAGE_COMPLETED;
		g_activeDiagnostic->operationStatus = 0;
	}
	artifacts.markCompleted();
	return 0;
}

int Unwrap::SnaphuMatrixInternal(Mat& wrapped_phase, Mat& unwrapped_phase, const char* tmp_folder, UnwrapProgressCallback cb)
{
	if (wrapped_phase.type() != CV_64F ||
		wrapped_phase.empty() ||
		!tmp_folder
		)
	{
		fprintf(stderr, "snaphu(): input check failed!\n");
		return -1;
	}
	string folder;
	std::wstring taskFolderWide;
	if (!createTaskDirectory(tmp_folder, folder, taskFolderWide)) return -1;
	ExternalToolResult toolResult;
	ScopedArtifactDirectory artifacts(taskFolderWide, &toolResult);
	string config_file, coh_file, in_file, out_file;
	config_file = folder + "\\config.txt";
	coh_file = folder + "\\coherence.dat";
	in_file = folder + "\\wrapped_phase_snaphu.dat";
	out_file = folder + "\\unwrapped_phase_snaphu.dat";
	std::wstring configFileWide, coherenceFileWide, inFileWide, outFileWide;
	PathResolver::Error artifactPathError = PathResolver::Error::None;
	if (!PathResolver::utf8ToWide(config_file, configFileWide, &artifactPathError) ||
		!PathResolver::utf8ToWide(coh_file, coherenceFileWide, &artifactPathError) ||
		!PathResolver::utf8ToWide(in_file, inFileWide, &artifactPathError) ||
		!PathResolver::utf8ToWide(out_file, outFileWide, &artifactPathError) ||
		!artifacts.registerCandidate(configFileWide) || !artifacts.registerCandidate(coherenceFileWide) ||
		!artifacts.registerCandidate(inFileWide) || !artifacts.registerCandidate(outFileWide)) return -1;
	Utils util;
	Mat coherence, phase;
	int ret, nr, nc;
	nr = wrapped_phase.rows;
	nc = wrapped_phase.cols;
	ret = util.phase_coherence(wrapped_phase, coherence);
	if (return_check(ret, "phase_coherence()", error_head)) return -1;
	wrapped_phase.convertTo(phase, CV_32F);
	if (!writeFloatRaster(inFileWide, phase, &artifacts)) return -1;
	coherence.convertTo(coherence, CV_32F);
	if (!writeFloatRaster(coherenceFileWide, coherence, &artifacts)) return -1;
	std::string configInFile, configOutFile, configCoherenceFile;
	if (!quoteSnaphuConfigPath(in_file, configInFile) || !quoteSnaphuConfigPath(out_file, configOutFile) ||
		!quoteSnaphuConfigPath(coh_file, configCoherenceFile)) return -1;
	std::ostringstream config;
	config << "INFILEFORMAT FLOAT_DATA\nOUTFILEFORMAT FLOAT_DATA\nCORRFILEFORMAT FLOAT_DATA\nAMPFILEFORMAT FLOAT_DATA\n";
	config << "LINELENGTH " << nc << "\nINFILE " << configInFile << "\nOUTFILE " << configOutFile << "\nCORRFILE " << configCoherenceFile << "\n";
	if (!appendSnaphuTilingConfig(config, taskFolderWide, nr, nc, toolResult)) return -1;
	const std::string configText = config.str();
	if (!writeBytes(configFileWide, configText.data(), configText.size(), &artifacts)) return -1;
	if (!emitSnaphuPreparedEvent(taskFolderWide, configFileWide))
	{
		toolResult.cancelled = true;
		toolResult.phase = "cancel";
		toolResult.validationFailure = "SNAPHU prepared callback cancelled or staging path is too long";
		return -2;
	}


	//////////////////////////创建并调用snaphu.exe进程///////////////////////////////
	std::vector<wchar_t> modulePath(32768, L'\0');
	const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
	if (moduleLength == 0 || moduleLength >= modulePath.size())
	{
		fprintf(stderr, "snaphu(): unable to resolve executable path.\n");
		return -1;
	}
	std::wstring executable(modulePath.data(), moduleLength);
	const size_t separator = executable.find_last_of(L"\\/");
	if (separator == std::wstring::npos) return -1;
	executable.resize(separator + 1);
	executable += L"snaphu.exe";
	if (!runExternalProcess(executable, { L"-f", configFileWide },
		"SNAPHU", "snaphu(): snaphu.exe failed!", cb, &toolResult))
	{
		if (GetFileAttributesW(outFileWide.c_str()) != INVALID_FILE_ATTRIBUTES) artifacts.markOwned(outFileWide);
		return -2;
	}

	if (!artifacts.markOwned(outFileWide)) return -1;
	std::string outputFailure;
	if (!readValidatedFloatRaster(outFileWide, nr, nc, unwrapped_phase, &outputFailure))
	{
		toolResult.phase = "validate output";
		toolResult.validationFailure = outputFailure;
		fprintf(stderr, "snaphu(): invalid output (%s).\n", outputFailure.c_str());
		return -1;
	}
	unwrapped_phase.convertTo(unwrapped_phase, CV_64F);

	artifacts.markCompleted();
	return 0;
}

int Unwrap::qualityGuided(Mat& wrapped_phase, Mat& unwrapped_phase, Mat& quality)
{
	if (wrapped_phase.type() != CV_64F ||
		wrapped_phase.empty() ||
		quality.type() != CV_64F ||
		quality.rows != wrapped_phase.rows ||
		quality.cols != wrapped_phase.cols
		)
	{
		fprintf(stderr, "qualityGuided(): input check failed!\n");
		return -1;
	}
	int nr = wrapped_phase.rows;
	int nc = wrapped_phase.cols;
	wrapped_phase.copyTo(unwrapped_phase);
	Mat unwrapped_status = Mat::zeros(nr, nc, CV_32S);

	double max_quailty = 1000000000.0;
	int i_start = 0, j_start = 0;
	for (int i = 0; i < nr; i++)
	{
		for (int j = 0; j < nc; j++)
		{
			if (quality.at<double>(i, j) < max_quailty)
			{
				i_start = i; j_start = j; max_quailty = quality.at<double>(i, j);
			}
		}
	}
	priority_queue<node_index> que;
	node_index node, node2;
	node.row = i_start; node.col = j_start; node.quality = max_quailty;
	que.push(node);
	int ii, jj;
	double grad;
	while (!que.empty())
	{
		node = que.top();
		que.pop();
		unwrapped_status.at<int>(node.row, node.col) = 1;
		for (int d = 0; d < 4; ++d)
		{
			ii = node.row + DIR_DR[d];
			jj = node.col + DIR_DC[d];
			bool in_bounds = false;
			if (d == 0) in_bounds = (jj > 0);
			else if (d == 1) in_bounds = (jj < nc);
			else if (d == 2) in_bounds = (ii > 0);
			else if (d == 3) in_bounds = (ii < nr);

			if (in_bounds && unwrapped_status.at<int>(ii, jj) == 0)
			{
				node2.col = jj; node2.row = ii; node2.quality = quality.at<double>(ii, jj);
				que.push(node2);
				grad = wrapped_phase.at<double>(ii, jj) - wrapped_phase.at<double>(node.row, node.col);
				grad = atan2(sin(grad), cos(grad));
				unwrapped_phase.at<double>(ii, jj) = unwrapped_phase.at<double>(node.row, node.col) + grad;
				unwrapped_status.at<int>(ii, jj) = 1;
			}
		}
	}

	return 0;
}

int Unwrap::GetSPD(Mat& wrapped_phase, Mat& SPD)
{
	if (wrapped_phase.rows < 1 ||
		wrapped_phase.cols < 1 ||
		wrapped_phase.type() != CV_64F ||
		wrapped_phase.channels() != 1)
	{
		fprintf(stderr, "GetSPD(): input check failed!\n\n");
		return -1;
	}
	int win_w = 3;
	int win_h = 3;
	int armw = win_w / 2;
	int armh = win_h / 2;
	Mat padded;
	copyMakeBorder(wrapped_phase, padded, armh, armh, armw, armw, BORDER_REFLECT_101);//镜像翻转边缘
	int width = padded.cols;
	int height = padded.rows;
	std::atomic<bool> parallel_flag(true);
	int ret = 0;
#pragma omp parallel for schedule(guided) \
	private(ret)
	for (int i = armh; i < height - armh; i++)
	{
		if (!parallel_flag) continue;
		const double* row_prev = padded.ptr<double>(i - 1);
		const double* row_curr = padded.ptr<double>(i);
		const double* row_next = padded.ptr<double>(i + 1);
		double* row_spd = SPD.ptr<double>(i - armh);

		for (int j = armw; j < width - armw; j++)
		{
			if (!parallel_flag) continue;
			double sum = 0;
			double val_center = row_curr[j];
			/*在3*3的窗口内计算与中心像素的梯度绝对值和*/
			for (int m = i - 1; m < i + 2; m++)
			{
				const double* row_m = (m == i - 1) ? row_prev : ((m == i) ? row_curr : row_next);
				for (int n = j - 1; n < j + 2; n++)
				{
					double delta = val_center - row_m[n];
					/*梯度取主值*/
					if (delta <= -PI)
						sum += abs(delta + 2 * PI);
					else if (delta > -PI && delta < PI)
						sum += abs(delta);
					else
						sum += abs(delta - 2 * PI);
				}
			}
			row_spd[j - armw] = sqrt(sum / 8);
		}
	}
	if (parallel_check(parallel_flag, "GetSPD()", parallel_error_head)) return -1;
	return 0;
}

int Unwrap::unwrap(Mat& src, Mat& dst, int x0, int y0, int x1, int y1, Mat& flag, Mat& adjoin, Mat& SPD, Heap& Q)
{
	if (src.rows < 1 ||
		src.cols < 1 ||
		src.type() != CV_64F ||
		src.channels() != 1 ||
		x0 < 0 || x0 >= src.cols || y0 < 0 || y0 >= src.rows ||
		x1 < 0 || x1 >= src.cols || y1 < 0 || y1 >= src.rows ||
		flag.size() != src.size() ||
		flag.type() != CV_64F ||
		flag.channels() != 1 ||
		adjoin.size() != src.size() ||
		adjoin.type() != CV_64F ||
		adjoin.channels() != 1 ||
		SPD.size() != src.size() ||
		SPD.type() != CV_64F ||
		SPD.channels() != 1
		)
	{
		fprintf(stderr, "unwrap(): input check failed!\n\n");
		return -1;
	}
	int width = src.cols;
	int height = src.rows;
	double delta = atan2(sin(src.ptr<double>(y0)[x0] - src.ptr<double>(y1)[x1]), cos(src.ptr<double>(y0)[x0] - src.ptr<double>(y1)[x1]));
	dst.ptr<double>(y1)[x1] = dst.ptr<double>(y0)[x0] - delta;
	flag.ptr<double>(y1)[x1] = 0;
	adjoin.ptr<double>(y1)[x1] = 0;
	int ret;
	for (int d = 0; d < 4; ++d)
	{
		int nx = x1 + DIR_DC[d];
		int ny = y1 + DIR_DR[d];
		if (nx >= 0 && nx < width && ny >= 0 && ny < height)
		{
			if ((flag.ptr<double>(ny)[nx] == 1) && (adjoin.ptr<double>(ny)[nx] == 0))
			{
				adjoin.ptr<double>(ny)[nx] = 1;
				ret = Q.push(SPD.ptr<double>(ny)[nx], nx, ny);
				if (ret < 0)
					return -1;
			}
		}
	}
	return 0;
}

int Unwrap::SPD_Guided_Unwrap(Mat& wrapped_phase, Mat& unwrapped_phase, UnwrapProgressCallback cb)
{
	if (wrapped_phase.rows < 1 ||
		wrapped_phase.cols < 1 ||
		wrapped_phase.type() != CV_64F ||
		wrapped_phase.channels() != 1)
	{
		fprintf(stderr, "SPD_Guided_Unwrap(): input check failed!\n\n");
		return -1;
	}
	Heap Heap;
	Mat tmp = Mat::zeros(wrapped_phase.size(), CV_64FC1);
	Mat SPD = Mat::zeros(wrapped_phase.size(), CV_64FC1);
	int ret = GetSPD(wrapped_phase, SPD);
	if (ret < 0)
		return -1;
	int count = 0;
	int width = wrapped_phase.cols;
	int height = wrapped_phase.rows;
	Mat flag = Mat::ones(wrapped_phase.size(), CV_64FC1);
	Mat adjoin = Mat::zeros(wrapped_phase.size(), CV_64FC1);
	Point Min;
	minMaxLoc(SPD, NULL, NULL, &Min, NULL);
	int x = Min.x;
	int y = Min.y;
	int mark = 0;
	tmp.ptr<double>(y)[x] = wrapped_phase.ptr<double>(y)[x];
	count++;
	flag.ptr<double>(y)[x] = 0;
	for (int d = 0; d < 4; ++d)
	{
		int nx = x + DIR_DC[d];
		int ny = y + DIR_DR[d];
		if (nx >= 0 && nx < width && ny >= 0 && ny < height)
		{
			ret = unwrap(wrapped_phase, tmp, x, y, nx, ny, flag, adjoin, SPD, Heap);
			if (ret < 0)
				return -1;
			count++;
		}
	}
	int top = height * width;
	int step = std::max(1, top / 100);
	while (count < top)
	{
		mark = 0;
		if (Heap.size != 0)
		{
			ret = Heap.top(&x, &y);
			if (ret < 0)
				return -1;
			Heap.pop();
		}
		const int prop_dr[4] = {-1, 1, 0, 0};
		const int prop_dc[4] = {0, 0, -1, 1};
		for (int d = 0; d < 4; ++d)
		{
			int nx = x + prop_dc[d];
			int ny = y + prop_dr[d];
			if (nx >= 0 && nx < width && ny >= 0 && ny < height)
			{
				if (flag.ptr<double>(ny)[nx] == 0)
				{
					ret = unwrap(wrapped_phase, tmp, nx, ny, x, y, flag, adjoin, SPD, Heap);
					if (ret < 0)
						return -1;
					mark = 1;
					break;
				}
			}
		}
		if (mark == 1)
			count++;
		if (cb && count % step == 0)
		{
			if (!cb(count * 100 / top, "SPD guided flood-fill unwrapping..."))
			{
				return -2;
			}
		}
	}
	tmp.copyTo(unwrapped_phase);
	return 0;
}

int Unwrap::MCFEx(Mat& wrapped_phase, Mat& unwrapped_phase, Mat& coherence, Mat& residue,
	const char* MCF_problem_file, const char* MCF_EXE_PATH, UnwrapProgressCallback cb,
	UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_MCF);
	const int status = MCFInternal(wrapped_phase, unwrapped_phase, coherence, residue, MCF_problem_file, MCF_EXE_PATH, cb);
	scope.finish(status);
	return status;
}

int Unwrap::MCFImprovedEx(Mat& wrapped_phase, Mat& unwrapped_phase, const char* MCF_problem_file,
	const char* MCF_exe_path, double coh_thresh, UnwrapProgressCallback cb, UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_MCF_IMPROVED);
	const int status = MCFImprovedInternal(wrapped_phase, unwrapped_phase, MCF_problem_file, MCF_exe_path, coh_thresh, cb);
	scope.finish(status);
	return status;
}

int Unwrap::McfDelaunayEx(const char* MCF_problem_file, const char* MCF_EXE_PATH,
	UnwrapProgressCallback cb, UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_MCF_DELAUNAY);
	const int status = McfDelaunayInternal(MCF_problem_file, MCF_EXE_PATH, cb);
	scope.finish(status);
	return status;
}

int Unwrap::QualityGuidedMCFEx(const Mat& wrapped_phase, Mat& unwrapped_phase,
	double coherence_thresh, double distance_thresh, const char* tmp_path, const char* EXE_path,
	UnwrapProgressCallback cb, UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_QUALITY_GUIDED_MCF);
	const int status = QualityGuidedMCFInternal(wrapped_phase, unwrapped_phase, coherence_thresh, distance_thresh,
		tmp_path, EXE_path, cb);
	scope.finish(status);
	return status;
}

int Unwrap::SnaphuFileEx(const char* wrapped_phase_file, Mat& unwrapped_phase, const char* project_path,
	const char* tmp_folder, const char* exe_path, UnwrapProgressCallback cb, UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_SNAPHU_FILE);
	const int status = SnaphuFileInternal(wrapped_phase_file, unwrapped_phase, project_path, tmp_folder, exe_path, cb);
	scope.finish(status);
	return status;
}

int Unwrap::SnaphuFileEx2(const char* wrapped_phase_file, Mat& unwrapped_phase, const char* project_path,
	const char* tmp_folder, const char* exe_path, const SnaphuRunOptionsV1* options,
	SnaphuRunEventCallbackV1 eventCallback, void* eventUserData, UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_SNAPHU_FILE);
	SnaphuRunOptionsV1 normalized;
	if (!normalizeSnaphuOptions(options, normalized))
	{
		if (g_activeDiagnostic) copyDiagnosticText(g_activeDiagnostic->summary, sizeof(g_activeDiagnostic->summary),
			"invalid SnaphuRunOptionsV1");
		scope.finish(-1);
		return -1;
	}
	ScopedSnaphuRunContext runContext(normalized, eventCallback, eventUserData);
	const int status = SnaphuFileInternal(wrapped_phase_file, unwrapped_phase, project_path, tmp_folder, exe_path, nullptr);
	scope.finish(status);
	return status;
}

int Unwrap::SnaphuMatrixEx(Mat& wrapped_phase, Mat& unwrapped_phase, const char* tmp_folder,
	UnwrapProgressCallback cb, UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_SNAPHU_MATRIX);
	const int status = SnaphuMatrixInternal(wrapped_phase, unwrapped_phase, tmp_folder, cb);
	scope.finish(status);
	return status;
}

int Unwrap::SnaphuMatrixEx2(Mat& wrapped_phase, Mat& unwrapped_phase, const char* tmp_folder,
	const SnaphuRunOptionsV1* options, SnaphuRunEventCallbackV1 eventCallback, void* eventUserData,
	UnwrapDiagnostic* diagnostic)
{
	ScopedPublicDiagnostic scope(diagnostic, UNWRAP_DIAGNOSTIC_ALGORITHM_SNAPHU_MATRIX);
	SnaphuRunOptionsV1 normalized;
	if (!normalizeSnaphuOptions(options, normalized))
	{
		if (g_activeDiagnostic) copyDiagnosticText(g_activeDiagnostic->summary, sizeof(g_activeDiagnostic->summary),
			"invalid SnaphuRunOptionsV1");
		scope.finish(-1);
		return -1;
	}
	ScopedSnaphuRunContext runContext(normalized, eventCallback, eventUserData);
	const int status = SnaphuMatrixInternal(wrapped_phase, unwrapped_phase, tmp_folder, nullptr);
	scope.finish(status);
	return status;
}

int Unwrap::MCF(Mat& wrapped_phase, Mat& unwrapped_phase, Mat& coherence, Mat& residue,
	const char* MCF_problem_file, const char* MCF_EXE_PATH, UnwrapProgressCallback cb)
{
	return MCFEx(wrapped_phase, unwrapped_phase, coherence, residue, MCF_problem_file, MCF_EXE_PATH, cb, nullptr);
}

int Unwrap::MCF_improved(Mat& wrapped_phase, Mat& unwrapped_phase, const char* MCF_problem_file,
	const char* MCF_exe_path, double coh_thresh, UnwrapProgressCallback cb)
{
	return MCFImprovedEx(wrapped_phase, unwrapped_phase, MCF_problem_file, MCF_exe_path, coh_thresh, cb, nullptr);
}

int Unwrap::mcf_delaunay(const char* MCF_problem_file, const char* MCF_EXE_PATH, UnwrapProgressCallback cb)
{
	return McfDelaunayEx(MCF_problem_file, MCF_EXE_PATH, cb, nullptr);
}

int Unwrap::QualityGuided_MCF(const Mat& wrapped_phase, Mat& unwrapped_phase, double coherence_thresh,
	double distance_thresh, const char* tmp_path, const char* EXE_path, UnwrapProgressCallback cb)
{
	return QualityGuidedMCFEx(wrapped_phase, unwrapped_phase, coherence_thresh, distance_thresh,
		tmp_path, EXE_path, cb, nullptr);
}

int Unwrap::snaphu(const char* wrapped_phase_file, Mat& unwrapped_phase, const char* project_path,
	const char* tmp_folder, const char* exe_path, UnwrapProgressCallback cb)
{
	return SnaphuFileEx(wrapped_phase_file, unwrapped_phase, project_path, tmp_folder, exe_path, cb, nullptr);
}

int Unwrap::snaphu(Mat& wrapped_phase, Mat& unwrapped_phase, const char* tmp_folder, UnwrapProgressCallback cb)
{
	return SnaphuMatrixEx(wrapped_phase, unwrapped_phase, tmp_folder, cb, nullptr);
}
