#define NOMINMAX
#include <windows.h>

#include "../include/Hdf5IO.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

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
}

int main(int argc, char* argv[])
{
	if (argc == 5 && strcmp(argv[1], "--worker") == 0)
		return runWorker(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 2 && strcmp(argv[1], "--contention") == 0)
		return runContentionScenarios();

	char executable[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, executable, MAX_PATH) == 0) return 2;
	const int singleResult = runSingleProcess(8, 100);
	const int contentionResult = runChildScenario(executable, "--contention");
	const int crossResult = runCrossProcess(executable, 4, 100);
	return singleResult == 0 && contentionResult == 0 && crossResult == 0 ? 0 : 1;
}
