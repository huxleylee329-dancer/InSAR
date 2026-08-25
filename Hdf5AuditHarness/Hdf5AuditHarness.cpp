#define NOMINMAX
#include <windows.h>

#include "../include/Hdf5IO.h"
#include "../include/Dem.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <hdf5.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>

#ifdef _DEBUG
#pragma comment(lib, "Dem_d.lib")
#else
#pragma comment(lib, "Dem.lib")
#endif

namespace
{
	constexpr int kColumns = 16;
	constexpr DWORD kScenarioTimeoutMilliseconds = 120000;

	std::string temporaryPath(const char* label, int worker)
	{
		char directory[MAX_PATH] = {};
		if (GetTempPathA(MAX_PATH, directory) == 0) return std::string();
		char filename[MAX_PATH] = {};
		sprintf_s(filename, "%sInSAR_Hdf5Audit_%lu_%s_%d.h5", directory,
			GetCurrentProcessId(), label, worker);
		return filename;
	}

	bool verifyAuditEvents()
	{
		Hdf5IO::Hdf5AuditStatus status = {};
		if (Hdf5IO::getAuditStatus(&status) != 0 || !status.enabled || !status.evidenceComplete ||
			status.droppedEventCount != 0) return false;

		std::map<uint64_t, Hdf5IO::Hdf5AuditEvent> opens;
		for (;;)
		{
			Hdf5IO::Hdf5AuditEvent event = {};
			const int pollResult = Hdf5IO::pollAuditEvent(&event);
			if (pollResult == 1) break;
			if (pollResult != 0) return false;
			if (event.operationId == 0) return false;
			if (event.kind == Hdf5IO::HDF5_AUDIT_OPEN)
			{
				if (!event.success)
				{
					if (event.fileHandleId != 0) return false;
					continue;
				}
				if (event.fileHandleId == 0 || (event.volumeSerialNumber == 0 &&
					event.fileIndexHigh == 0 && event.fileIndexLow == 0) ||
					event.hdf5MajorVersion == 0 || event.path[0] == '\0' || event.mode[0] == '\0' ||
					opens.count(event.fileHandleId) != 0) return false;
				opens.emplace(event.fileHandleId, event);
			}
			else if (event.kind == Hdf5IO::HDF5_AUDIT_CLOSE)
			{
				if (!event.success || event.hdf5Status < 0) return false;
				const auto open = opens.find(event.fileHandleId);
				if (open == opens.end() ||
					open->second.operationId != event.operationId ||
					open->second.volumeSerialNumber != event.volumeSerialNumber ||
					open->second.fileIndexHigh != event.fileIndexHigh ||
					open->second.fileIndexLow != event.fileIndexLow ||
					strcmp(open->second.path, event.path) != 0 || strcmp(open->second.mode, event.mode) != 0)
					return false;
				opens.erase(open);
			}
			else return false;
		}
		return opens.empty();
	}

	int exercisePath(const char* path, uint64_t operationBase, int iterations)
	{
		Hdf5IO::setAuditOperationId(operationBase);
		if (Hdf5IO::createFile(path) != 0) return 1;
		for (int iteration = 0; iteration < iterations; ++iteration)
		{
			Hdf5IO::setAuditOperationId(operationBase + static_cast<uint64_t>(iteration) + 1);
			cv::Mat expected(1, kColumns, CV_32S, cv::Scalar(iteration));
			Hdf5IO::WriteSession* writer = Hdf5IO::openWriteSession(path);
			if (!writer) return 1;
			const int writeResult = Hdf5IO::writeArrayReplace(writer, "payload", expected);
			Hdf5IO::closeWriteSession(writer);
			if (writeResult != 0) return 1;

			Hdf5IO::ReadSession* reader = Hdf5IO::openReadSession(path);
			if (!reader) return 1;
			cv::Mat actual;
			const int readResult = Hdf5IO::readArray(reader, "payload", actual);
			Hdf5IO::closeReadSession(reader);
			if (readResult != 0 || actual.size() != expected.size() || actual.type() != expected.type() ||
				cv::countNonZero(actual != expected) != 0) return 1;
		}
		return 0;
	}

	bool readUniformPayload(const char* path, int expectedRows, int minimumValue, int maximumValue)
	{
		cv::Mat payload;
		if (Hdf5IO::readArray(path, "payload", payload) != 0 || payload.rows != expectedRows ||
			payload.cols != kColumns || payload.type() != CV_32S) return false;
		const int value = payload.at<int>(0, 0);
		if (value < minimumValue || value > maximumValue) return false;
		for (int row = 0; row < payload.rows; ++row)
		{
			for (int column = 0; column < payload.cols; ++column)
			{
				if (payload.at<int>(row, column) != value) return false;
			}
		}
		return true;
	}

	int createPayload(const char* path, int value, uint64_t operationId)
	{
		if (operationId == 0) return 1;
		Hdf5IO::setAuditOperationId(operationId);
		cv::Mat payload(1, kColumns, CV_32S, cv::Scalar(value));
		if (Hdf5IO::createFile(path) != 0) return 1;
		return Hdf5IO::writeArray(path, "payload", payload) == 0 ? 0 : 1;
	}

	int runSameTargetWriteContention(int iterations)
	{
		const std::string target = temporaryPath("same_target_write", 0);
		if (target.empty() || createPayload(target.c_str(), 0, 0x010000000000ULL) != 0) return 1;
		std::atomic<int> failure{ 0 };
		auto writer = [&target, iterations, &failure](int worker)
		{
			const int firstValue = (worker + 1) * 100000;
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				Hdf5IO::setAuditOperationId((static_cast<uint64_t>(worker) + 1) << 40 | static_cast<uint64_t>(iteration + 1));
				cv::Mat payload(1, kColumns, CV_32S, cv::Scalar(firstValue + iteration));
				if (Hdf5IO::writeArrayReplace(target.c_str(), "payload", payload) != 0)
				{
					failure.store(1);
					return;
				}
			}
		};
		std::thread first(writer, 0);
		std::thread second(writer, 1);
		first.join();
		second.join();
		return failure.load() == 0 && readUniformPayload(target.c_str(), 1, 100000, 200000 + iterations - 1) ? 0 : 1;
	}

	int runSameFileReadWriteContention(int iterations)
	{
		const std::string target = temporaryPath("same_file_read_write", 0);
		if (target.empty() || createPayload(target.c_str(), 0, 0x020000000000ULL) != 0) return 1;
		std::atomic<int> failure{ 0 };
		std::thread writer([&target, iterations, &failure]()
		{
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				Hdf5IO::setAuditOperationId(0x100000000000ULL | static_cast<uint64_t>(iteration + 1));
				cv::Mat payload(1, kColumns, CV_32S, cv::Scalar(300000 + iteration));
				if (Hdf5IO::writeArrayReplace(target.c_str(), "payload", payload) != 0)
				{
					failure.store(1);
					return;
				}
			}
		});
		std::thread reader([&target, iterations, &failure]()
		{
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				Hdf5IO::setAuditOperationId(0x200000000000ULL | static_cast<uint64_t>(iteration + 1));
				if (!readUniformPayload(target.c_str(), 1, 0, 300000 + iterations - 1))
				{
					failure.store(1);
					return;
				}
			}
		});
		writer.join();
		reader.join();
		return failure.load() == 0 && readUniformPayload(target.c_str(), 1, 300000, 300000 + iterations - 1) ? 0 : 1;
	}

	int runCopyContention(int iterations)
	{
		const std::string sourceA = temporaryPath("copy_source_a", 0);
		const std::string sourceB = temporaryPath("copy_source_b", 0);
		const std::string target = temporaryPath("copy_target", 0);
		if (sourceA.empty() || sourceB.empty() || target.empty() ||
			createPayload(sourceA.c_str(), 400000, 0x030000000000ULL) != 0 ||
			createPayload(sourceB.c_str(), 500000, 0x040000000000ULL) != 0 ||
			createPayload(target.c_str(), 0, 0x050000000000ULL) != 0) return 1;

		std::atomic<int> failure{ 0 };
		auto copier = [iterations, &failure, &target](const std::string& source, uint64_t operationBase)
		{
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				Hdf5IO::setAuditOperationId(operationBase | static_cast<uint64_t>(iteration + 1));
				if (Hdf5IO::copyDatasetIfPresent(source.c_str(), target.c_str(), "payload", true) != 0)
				{
					failure.store(1);
					return;
				}
			}
		};
		std::thread first(copier, std::cref(sourceA), 0x300000000000ULL);
		std::thread second(copier, std::cref(sourceB), 0x400000000000ULL);
		first.join();
		second.join();
		return failure.load() == 0 && readUniformPayload(target.c_str(), 1, 400000, 500000) ? 0 : 1;
	}

	int runReverseCopyContention(int iterations)
	{
		const std::string firstPath = temporaryPath("reverse_copy_a", 0);
		const std::string secondPath = temporaryPath("reverse_copy_b", 0);
		if (firstPath.empty() || secondPath.empty() ||
			createPayload(firstPath.c_str(), 600000, 0x060000000000ULL) != 0 ||
			createPayload(secondPath.c_str(), 700000, 0x070000000000ULL) != 0) return 1;

		std::atomic<int> failure{ 0 };
		auto copier = [iterations, &failure](const std::string& source, const std::string& destination, uint64_t operationBase)
		{
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				Hdf5IO::setAuditOperationId(operationBase | static_cast<uint64_t>(iteration + 1));
				if (Hdf5IO::copyDatasetIfPresent(source.c_str(), destination.c_str(), "payload", true) != 0)
				{
					failure.store(1);
					return;
				}
			}
		};
		std::thread forward(copier, std::cref(firstPath), std::cref(secondPath), 0x500000000000ULL);
		std::thread reverse(copier, std::cref(secondPath), std::cref(firstPath), 0x600000000000ULL);
		forward.join();
		reverse.join();
		return failure.load() == 0 && readUniformPayload(firstPath.c_str(), 1, 600000, 700000) &&
			readUniformPayload(secondPath.c_str(), 1, 600000, 700000) ? 0 : 1;
	}

	int runContentionScenarios()
	{
		if (Hdf5IO::enableAuditDiagnostics(65536) != 0) return 2;
		const int iterations = 100;
		const int result = runSameTargetWriteContention(iterations) == 0 &&
			runSameFileReadWriteContention(iterations) == 0 && runCopyContention(iterations) == 0 &&
			runReverseCopyContention(iterations) == 0 && verifyAuditEvents() ? 0 : 1;
		Hdf5IO::disableAuditDiagnostics();
		return result;
	}

	int runWorker(const char* path, int worker, int iterations)
	{
		Hdf5IO::Hdf5RuntimeInfo runtime = {};
		char modulePath[MAX_PATH] = {};
		if (Hdf5IO::getRuntimeInfo(&runtime) != 0 || Hdf5IO::getRuntimeModulePath(modulePath, sizeof(modulePath)) != 0 ||
			Hdf5IO::enableAuditDiagnostics(65536) != 0) return 2;

		int result = 0;
		for (int iteration = 0; iteration < iterations; ++iteration)
		{
			Hdf5IO::setAuditOperationId((static_cast<uint64_t>(worker) + 1) << 32 | static_cast<uint64_t>(iteration + 1));
			cv::Mat row(1, kColumns, CV_32S, cv::Scalar(worker * 100000 + iteration));
			if (Hdf5IO::writeSubarray(path, "payload", row, worker, 0) != 0)
			{
				result = 1;
				break;
			}
		}
		const bool auditValid = verifyAuditEvents();
		std::printf("worker=%d hdf5=%u.%u.%u threadSafe=%d module=%s audit=%s\n", worker,
			runtime.majorVersion, runtime.minorVersion, runtime.releaseVersion, runtime.libraryThreadSafe,
			modulePath, auditValid ? "complete" : "incomplete");
		Hdf5IO::disableAuditDiagnostics();
		return result == 0 && auditValid ? 0 : 1;
	}

	int runSingleProcess(int workerCount, int iterations)
	{
		Hdf5IO::Hdf5RuntimeInfo runtime = {};
		char modulePath[MAX_PATH] = {};
		if (Hdf5IO::getRuntimeInfo(&runtime) != 0 || Hdf5IO::getRuntimeModulePath(modulePath, sizeof(modulePath)) != 0 ||
			Hdf5IO::enableAuditDiagnostics(65536) != 0) return 2;

		std::atomic<int> failure{ 0 };
		std::vector<std::thread> workers;
		for (int worker = 0; worker < workerCount; ++worker)
		{
			const std::string path = temporaryPath("single", worker);
			workers.emplace_back([path, worker, iterations, &failure]()
			{
				if (path.empty() || exercisePath(path.c_str(), (static_cast<uint64_t>(worker) + 1) << 32, iterations) != 0)
					failure.store(1);
			});
		}
		for (std::thread& worker : workers) worker.join();

		const bool auditValid = verifyAuditEvents();
		std::printf("single-process hdf5=%u.%u.%u threadSafe=%d module=%s audit=%s\n",
			runtime.majorVersion, runtime.minorVersion, runtime.releaseVersion, runtime.libraryThreadSafe,
			modulePath, auditValid ? "complete" : "incomplete");
		Hdf5IO::disableAuditDiagnostics();
		return failure.load() == 0 && auditValid ? 0 : 1;
	}

	int runChildScenario(const char* executable, const char* mode)
	{
		char commandLine[2 * MAX_PATH] = {};
		sprintf_s(commandLine, "\"%s\" %s", executable, mode);
		STARTUPINFOA startup = {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION child = {};
		if (!CreateProcessA(nullptr, commandLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) return 1;
		const DWORD waitResult = WaitForSingleObject(child.hProcess, kScenarioTimeoutMilliseconds);
		if (waitResult == WAIT_TIMEOUT)
		{
			TerminateProcess(child.hProcess, 1);
			WaitForSingleObject(child.hProcess, INFINITE);
		}
		DWORD exitCode = 1;
		const BOOL exitResult = GetExitCodeProcess(child.hProcess, &exitCode);
		CloseHandle(child.hThread);
		CloseHandle(child.hProcess);
		return waitResult == WAIT_OBJECT_0 && exitResult && exitCode == 0 ? 0 : 1;
	}

	int runCrossProcess(const char* executable, int processCount, int iterations)
	{
		const std::string path = temporaryPath("cross", 0);
		if (path.empty() || Hdf5IO::createFile(path.c_str()) != 0 ||
			Hdf5IO::createZeroDataset(path.c_str(), "payload", processCount, kColumns, CV_32S) != 0) return 1;

		std::vector<PROCESS_INFORMATION> children;
		for (int worker = 0; worker < processCount; ++worker)
		{
			char commandLine[2 * MAX_PATH] = {};
			sprintf_s(commandLine, "\"%s\" --worker \"%s\" %d %d", executable, path.c_str(), worker, iterations);
			STARTUPINFOA startup = {};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION child = {};
			if (!CreateProcessA(nullptr, commandLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) return 1;
			children.push_back(child);
		}

		int result = 0;
		for (PROCESS_INFORMATION& child : children)
		{
			const DWORD waitResult = WaitForSingleObject(child.hProcess, kScenarioTimeoutMilliseconds);
			if (waitResult == WAIT_TIMEOUT)
			{
				TerminateProcess(child.hProcess, 1);
				WaitForSingleObject(child.hProcess, INFINITE);
				result = 1;
			}
			DWORD exitCode = 1;
			if (waitResult != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &exitCode) || exitCode != 0) result = 1;
			CloseHandle(child.hThread);
			CloseHandle(child.hProcess);
		}
		cv::Mat payload;
		if (Hdf5IO::readArray(path.c_str(), "payload", payload) != 0 || payload.rows != processCount ||
			payload.cols != kColumns || payload.type() != CV_32S) return 1;
		for (int worker = 0; worker < processCount; ++worker)
		{
			for (int column = 0; column < kColumns; ++column)
			{
				if (payload.at<int>(worker, column) != worker * 100000 + iterations - 1) return 1;
			}
		}
		return result;
	}

	// Creates an HDF5 file that carries a string dataset, an integer array
	// dataset, and a rank-3 float dataset so the diagnostic read paths can be
	// exercised against controlled wrong-type/wrong-shape fixtures.
	int createDiagnosticFixture(const char* path)
	{
		Hdf5IO::setAuditOperationId(0x0B0000000001ULL);
		if (Hdf5IO::createFile(path) != 0) return 1;
		Hdf5IO::WriteSession* writer = Hdf5IO::openWriteSession(path);
		if (!writer) return 1;
		cv::Mat array(1, kColumns, CV_32S, cv::Scalar(11));
		const int arrayStatus = Hdf5IO::writeArray(writer, "payload", array);
		const int stringStatus = Hdf5IO::createString(writer, "label", "diagnostic-fixture");
		Hdf5IO::closeWriteSession(writer);
		if (arrayStatus != 0 || stringStatus != 0) return 1;

		// Rank-3 dataset created directly with the HDF5 C API; readArrayDiagnosed
		// must reject it at the RANK stage before any output allocation.
		Hdf5IO::setAuditOperationId(0x0B0000000002ULL);
		const hsize_t dimensions[3] = { 2, 3, 4 };
		std::vector<float> cube(24, 1.0f);
		hid_t file = H5Fopen(path, H5F_ACC_RDWR, H5P_DEFAULT);
		if (file < 0) return 1;
		hid_t space = H5Screate_simple(3, dimensions, nullptr);
		hid_t memorySpace = H5Screate_simple(3, dimensions, nullptr);
		hid_t dataset = H5Dcreate2(file, "cube", H5T_NATIVE_FLOAT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
		const herr_t writeStatus = dataset >= 0 ? H5Dwrite(dataset, H5T_NATIVE_FLOAT, memorySpace, space,
			H5P_DEFAULT, cube.data()) : -1;
		if (dataset >= 0) H5Dclose(dataset);
		if (memorySpace >= 0) H5Sclose(memorySpace);
		if (space >= 0) H5Sclose(space);
		if (H5Fclose(file) < 0) return 1;
		if (writeStatus < 0) return 1;
		return 0;
	}

	bool diagnoseArray(const char* label, const char* path, const char* datasetName,
		int expectedStage, int expectedReturn)
	{
		Hdf5IO::setAuditOperationId(0x0B0000001000ULL | static_cast<uint64_t>(expectedStage));
		cv::Mat output;
		Hdf5IO::Hdf5ReadDiagnostic diagnostic = {};
		const int status = Hdf5IO::readArrayDiagnosed(path, datasetName, output, &diagnostic);
		const bool stageMatches = diagnostic.stage == expectedStage;
		const bool stackAvailable = diagnostic.errorStack[0] != '\0';
		const bool returnMatches = status != 0 && (expectedReturn == 0 || status == expectedReturn);
		std::printf("diagnose-array %-24s rc=%d stage=%d(%s) h5Status=%d stack=%s\n", label, status,
			diagnostic.stage, stageMatches ? "ok" : "BAD", diagnostic.hdf5Status,
			stackAvailable ? "non-empty" : "EMPTY");
		return stageMatches && stackAvailable && returnMatches;
	}

	bool diagnoseString(const char* label, const char* path, const char* datasetName,
		int expectedStage, int expectedReturn)
	{
		Hdf5IO::setAuditOperationId(0x0B0000002000ULL | static_cast<uint64_t>(expectedStage));
		std::string value;
		Hdf5IO::Hdf5ReadDiagnostic diagnostic = {};
		const int status = Hdf5IO::readStringDiagnosed(path, datasetName, value, &diagnostic);
		const bool stageMatches = diagnostic.stage == expectedStage;
		const bool stackAvailable = diagnostic.errorStack[0] != '\0';
		const bool returnMatches = status != 0 && (expectedReturn == 0 || status == expectedReturn);
		std::printf("diagnose-string %-22s rc=%d stage=%d(%s) h5Status=%d stack=%s\n", label, status,
			diagnostic.stage, stageMatches ? "ok" : "BAD", diagnostic.hdf5Status,
			stackAvailable ? "non-empty" : "EMPTY");
		return stageMatches && stackAvailable && returnMatches;
	}

	int runDiagnosedReadPaths()
	{
		if (Hdf5IO::enableAuditDiagnostics(65536) != 0) return 2;
		const std::string fixture = temporaryPath("diagnosed", 0);
		const std::string missing = temporaryPath("diagnosed_missing", 0);
		if (fixture.empty() || missing.empty() || createDiagnosticFixture(fixture.c_str()) != 0)
		{
			Hdf5IO::disableAuditDiagnostics();
			return 1;
		}

		bool ok = true;
		// Nonexistent file reaches the OPEN_FILE stage with a clear non-HDF5 reason.
		ok &= diagnoseArray("missing file", missing.c_str(), "payload",
			Hdf5IO::HDF5_READ_STAGE_OPEN_FILE, 0);
		ok &= diagnoseString("missing file", missing.c_str(), "label",
			Hdf5IO::HDF5_READ_STAGE_OPEN_FILE, 0);
		// Nonexistent dataset reaches OPEN_DATASET.
		ok &= diagnoseArray("missing dataset", fixture.c_str(), "absent",
			Hdf5IO::HDF5_READ_STAGE_OPEN_DATASET, 0);
		ok &= diagnoseString("missing dataset", fixture.c_str(), "absent",
			Hdf5IO::HDF5_READ_STAGE_OPEN_DATASET, 0);
		// Wrong type: string dataset read as an array.
		ok &= diagnoseArray("wrong type", fixture.c_str(), "label",
			Hdf5IO::HDF5_READ_STAGE_DATA_TYPE, 0);
		// Wrong type: integer array dataset read as a string.
		ok &= diagnoseString("wrong type", fixture.c_str(), "payload",
			Hdf5IO::HDF5_READ_STAGE_DATA_TYPE, 0);
		// Wrong shape: rank-3 dataset read as an array.
		ok &= diagnoseArray("wrong shape", fixture.c_str(), "cube",
			Hdf5IO::HDF5_READ_STAGE_RANK, 0);

		const bool auditValid = verifyAuditEvents();
		std::printf("diagnosed-read audit=%s\n", auditValid ? "complete" : "incomplete");
		Hdf5IO::disableAuditDiagnostics();
		return ok && auditValid ? 0 : 1;
	}

	struct DemEventCollector
	{
		std::vector<std::string> callIds;
		std::vector<std::string> stages;
		int errorCount = 0;
	};

	void __stdcall demDiagnosticCallback(const DemDiagnosticEvent* event, void* userData)
	{
		DemEventCollector* collector = static_cast<DemEventCollector*>(userData);
		if (!collector || !event) return;
		collector->callIds.emplace_back(event->callId ? event->callId : "");
		collector->stages.emplace_back(event->stage ? event->stage : "");
		if (event->level >= DEM_LOG_ERROR) ++collector->errorCount;
	}

	// Redirects the process stdout/stderr into the supplied files so the dem
	// _ex call can be audited for accidental console output. Returns 0 on
	// success; callers must call restoreConsole with the saved descriptors.
	int redirectConsoleTo(const char* stdoutPath, const char* stderrPath, int* savedStdout, int* savedStderr)
	{
		if (!stdoutPath || !stderrPath || !savedStdout || !savedStderr) return 1;
		fflush(stdout);
		fflush(stderr);
		*savedStdout = _dup(_fileno(stdout));
		*savedStderr = _dup(_fileno(stderr));
		if (*savedStdout < 0 || *savedStderr < 0) return 1;
		FILE* stdoutFile = _fsopen(stdoutPath, "w", _SH_DENYNO);
		FILE* stderrFile = _fsopen(stderrPath, "w", _SH_DENYNO);
		if (!stdoutFile || !stderrFile)
		{
			if (stdoutFile) fclose(stdoutFile);
			if (stderrFile) fclose(stderrFile);
			_close(*savedStdout);
			_close(*savedStderr);
			return 1;
		}
		if (_dup2(_fileno(stdoutFile), _fileno(stdout)) < 0 ||
			_dup2(_fileno(stderrFile), _fileno(stderr)) < 0)
		{
			fclose(stdoutFile);
			fclose(stderrFile);
			_close(*savedStdout);
			_close(*savedStderr);
			return 1;
		}
		fclose(stdoutFile);
		fclose(stderrFile);
		return 0;
	}

	void restoreConsole(int savedStdout, int savedStderr)
	{
		fflush(stdout);
		fflush(stderr);
		if (savedStdout >= 0) _dup2(savedStdout, _fileno(stdout));
		if (savedStderr >= 0) _dup2(savedStderr, _fileno(stderr));
		if (savedStdout >= 0) _close(savedStdout);
		if (savedStderr >= 0) _close(savedStderr);
	}

	long long fileSize(const char* path)
	{
		WIN32_FILE_ATTRIBUTE_DATA attributes = {};
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) return -1;
		return (static_cast<long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
	}

	int runDemNewtonIterEx()
	{
		const std::string fixture = temporaryPath("dem_ex", 0);
		const std::string stdoutPath = temporaryPath("dem_ex_stdout", 0);
		const std::string stderrPath = temporaryPath("dem_ex_stderr", 0);
		if (fixture.empty() || stdoutPath.empty() || stderrPath.empty()) return 1;

		Hdf5IO::setAuditOperationId(0x0C0000000001ULL);
		if (Hdf5IO::createFile(fixture.c_str()) != 0) return 1;
		Hdf5IO::WriteSession* writer = Hdf5IO::openWriteSession(fixture.c_str());
		if (!writer) return 1;
		cv::Mat phase(2, 2, CV_64F, cv::Scalar(0.25));
		const int phaseStatus = Hdf5IO::writeArray(writer, "phase", phase);
		cv::Mat flatCoefficient(1, 6, CV_64F);
		for (int column = 0; column < 6; ++column) flatCoefficient.at<double>(0, column) = 0.1 * (column + 1);
		const int flatStatus = Hdf5IO::writeArray(writer, "flat_phase_coefficient", flatCoefficient);
		Hdf5IO::closeWriteSession(writer);
		if (phaseStatus != 0 || flatStatus != 0) return 1;

		DemEventCollector collector;
		DemDiagnosticOptions options = {};
		options.structSize = sizeof(DemDiagnosticOptions);
		options.version = DEM_DIAGNOSTIC_OPTIONS_VERSION;
		options.callId = "audit-harness-dem-ex";
		options.logLevel = DEM_LOG_DEBUG;
		options.callback = demDiagnosticCallback;
		options.userData = &collector;

		int savedStdout = -1;
		int savedStderr = -1;
		if (redirectConsoleTo(stdoutPath.c_str(), stderrPath.c_str(), &savedStdout, &savedStderr) != 0) return 1;
		Dem dem;
		cv::Mat demOutput;
		Hdf5IO::setAuditOperationId(0x0C0000000002ULL);
		const int result = dem.dem_newton_iter_ex(fixture.c_str(), demOutput, fixture.c_str(), 3,
			TR_MODE_SINGLE_TX_SINGLE_RX, &options);
		restoreConsole(savedStdout, savedStderr);

		bool callIdConsistent = !collector.callIds.empty();
		for (const std::string& callId : collector.callIds)
		{
			if (callId != options.callId) callIdConsistent = false;
		}
		bool sawPhaseStage = false;
		bool sawEntryStage = false;
		for (const std::string& stage : collector.stages)
		{
			if (stage == "input.phase") sawPhaseStage = true;
			if (stage == "entry") sawEntryStage = true;
		}
		const long long stdoutSize = fileSize(stdoutPath.c_str());
		const long long stderrSize = fileSize(stderrPath.c_str());
		const bool silent = stdoutSize == 0 && stderrSize == 0;
		const bool ok = callIdConsistent && sawPhaseStage && sawEntryStage && silent;

		std::printf("dem-newton-ex result=%d callId=%s stages=%zu errors=%d input.phase=%d entry=%d "
			"stdout=%lld stderr=%lld silence=%s\n", result, callIdConsistent ? "consistent" : "INCONSISTENT",
			collector.stages.size(), collector.errorCount, sawPhaseStage ? 1 : 0, sawEntryStage ? 1 : 0,
			stdoutSize, stderrSize, silent ? "ok" : "BAD");
		return ok ? 0 : 1;
	}
}

int main(int argc, char* argv[])
{
	if (argc == 5 && strcmp(argv[1], "--worker") == 0)
		return runWorker(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 2 && strcmp(argv[1], "--contention") == 0)
		return runContentionScenarios();
	if (argc == 2 && strcmp(argv[1], "--diagnosed") == 0)
		return runDiagnosedReadPaths();
	if (argc == 2 && strcmp(argv[1], "--dem-ex") == 0)
		return runDemNewtonIterEx();

	char executable[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, executable, MAX_PATH) == 0) return 2;
	const int singleResult = runSingleProcess(8, 100);
	const int contentionResult = runChildScenario(executable, "--contention");
	const int crossResult = runCrossProcess(executable, 4, 100);
	const int diagnosedResult = runDiagnosedReadPaths();
	const int demExResult = runDemNewtonIterEx();
	return singleResult == 0 && contentionResult == 0 && crossResult == 0 &&
		diagnosedResult == 0 && demExResult == 0 ? 0 : 1;
}
