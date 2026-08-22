#pragma once

#include <opencv2/core.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

#if defined(HDF5IO_EXPORTS)
#define HDF5IO_API __declspec(dllexport)
#else
#define HDF5IO_API __declspec(dllimport)
#endif

	namespace Hdf5IO
{
	struct ReadSession;
	struct WriteSession;
	struct BatchLock;

	// All public operations acquire the same named mutex before entering HDF5.
	// A distinct negative return code denotes an abandoned mutex and callers must
	// treat it as a failed transaction rather than continuing with stale state.
	constexpr int kLockAbandoned = -100;
	// Indicates invalid API input. In particular, copy operations must reject a
	// source and destination that resolve to the same physical file.
	constexpr int kInvalidArgument = -101;
	constexpr int kAuditDisabled = -102;

	// Diagnostic details are caller-owned and valid only for the duration of a
	// single readArrayDiagnosed call. The fixed buffers keep this C-compatible
	// structure independent of cross-DLL allocation.
	enum Hdf5ReadStage
	{
		HDF5_READ_STAGE_NONE = 0,
		HDF5_READ_STAGE_INVALID_ARGUMENT,
		HDF5_READ_STAGE_OPEN_FILE,
		HDF5_READ_STAGE_DATASET_PATH,
		HDF5_READ_STAGE_OPEN_DATASET,
		HDF5_READ_STAGE_DATA_SPACE,
		HDF5_READ_STAGE_DATA_TYPE,
		HDF5_READ_STAGE_RANK,
		HDF5_READ_STAGE_DIMENSIONS,
		HDF5_READ_STAGE_OUTPUT_ALLOCATION,
		HDF5_READ_STAGE_DATA_READ
	};

	struct Hdf5ReadDiagnostic
	{
		int stage;
		int hdf5Status;
		int rows;
		int columns;
		int cvType;
		char hdf5Type[96];
		char errorStack[4096];
	};

	enum Hdf5AuditEventKind { HDF5_AUDIT_OPEN = 1, HDF5_AUDIT_CLOSE = 2 };
	struct Hdf5AuditEvent
	{
		uint64_t operationId;
		uint64_t fileHandleId;
		uint64_t monotonicMilliseconds;
		unsigned long threadId;
		int kind;
		int success;
		int hdf5Status;
		unsigned long volumeSerialNumber;
		unsigned long fileIndexHigh;
		unsigned long fileIndexLow;
		unsigned int hdf5MajorVersion;
		unsigned int hdf5MinorVersion;
		unsigned int hdf5ReleaseVersion;
		char path[512];
		char mode[16];
	};
	// A non-zero droppedEventCount or a zero evidenceComplete value invalidates
	// the current diagnostic evidence range. The first/last fields locate the
	// overflow interval without retaining unbounded event history.
	struct Hdf5AuditStatus
	{
		int enabled;
		int evidenceComplete;
		unsigned long long droppedEventCount;
		uint64_t firstDroppedOperationId;
		uint64_t lastDroppedOperationId;
		uint64_t firstDroppedMonotonicMilliseconds;
		uint64_t lastDroppedMonotonicMilliseconds;
	};

	// Values are obtained from the HDF5 runtime linked by this DLL. A non-zero
	// libraryThreadSafe value only reports HDF5 API thread-safety; it is not a
	// claim that operations execute in parallel or that DLL-level state is safe.
	struct Hdf5RuntimeInfo
	{
		unsigned int majorVersion;
		unsigned int minorVersion;
		unsigned int releaseVersion;
		int libraryThreadSafe;
	};

	// Reads the version and thread-safety result from the HDF5 runtime used by
	// this DLL. A zero return value requires both underlying HDF5 calls to pass.
	HDF5IO_API int getRuntimeInfo(Hdf5RuntimeInfo* info);
	// Copies the path of the module containing the HDF5 runtime symbol into the
	// caller buffer. When HDF5 is statically linked, this can be Hdf5IO itself.
	HDF5IO_API int getRuntimeModulePath(char* buffer, size_t bufferSize);
	HDF5IO_API int enableAuditDiagnostics(size_t queueCapacity);
	HDF5IO_API void disableAuditDiagnostics();
	// Audit-enabled file lifecycle work requires a non-zero ID set by the caller
	// before its first Hdf5IO operation. A zero-ID event marks evidence incomplete.
	HDF5IO_API void setAuditOperationId(uint64_t operationId);
	HDF5IO_API int getAuditStatus(Hdf5AuditStatus* status);
	HDF5IO_API int pollAuditEvent(Hdf5AuditEvent* event);

	// Opens both existing paths using normal Windows path resolution and compares
	// their volume serial number and file index. This covers aliases such as hard
	// links; path text alone is never used as final identity evidence.
	HDF5IO_API int areSameExistingFile(const char* firstFilename, const char* secondFilename, int* sameFile);
	// Validates a source/output pair before the first output mutation. A missing
	// output path is acceptable; an existing output that identifies the same
	// physical file as the source returns kInvalidArgument.
	HDF5IO_API int validateDistinctFilePaths(const char* sourceFilename, const char* outputFilename);

	// Holds this DLL's named HDF5 mutex across a caller-defined group of Hdf5IO
	// calls. The handle is opaque so mutex ownership remains in this DLL.
	HDF5IO_API BatchLock* acquireBatchLock();
	HDF5IO_API int getBatchLockStatus(const BatchLock* lock);
	HDF5IO_API void releaseBatchLock(BatchLock* lock);

	// Creates or truncates an HDF5 file. Array creation APIs below intentionally
	// fail when the target dataset already exists, matching the legacy writer.
	HDF5IO_API int createFile(const char* filename);
	// Reports whether a dataset link exists. Returns 0 for a completed probe
	// (with exists set to 0 or 1) and a negative value for file/HDF5 failures.
	HDF5IO_API int datasetExists(const char* filename, const char* datasetName, int* exists);
	HDF5IO_API int getDatasetDims(const char* filename, const char* datasetName, int* rows, int* columns);
	HDF5IO_API int readArray(const char* filename, const char* datasetName, cv::Mat& output);
	// Performs the same conversion as readArray while preserving the precise
	// failing HDF5 operation and its current-thread error stack on failure.
	HDF5IO_API int readArrayDiagnosed(const char* filename, const char* datasetName,
		cv::Mat& output, Hdf5ReadDiagnostic* diagnostic);
	// Holds the HDF5 file handle inside Hdf5IO so callers never exchange hid_t
	// values across DLL boundaries. Every session operation acquires the shared lock.
	HDF5IO_API ReadSession* openReadSession(const char* filename);
	HDF5IO_API void closeReadSession(ReadSession* session);
	HDF5IO_API int readArray(ReadSession* session, const char* datasetName, cv::Mat& output);
	HDF5IO_API int readArray(ReadSession* session, const char* datasetName, int outputCvType, cv::Mat& output);
	HDF5IO_API int readSubarray(ReadSession* session, const char* datasetName, int offsetRow, int offsetColumn,
		int rows, int columns, cv::Mat& output);
	// Reads a rank-3 float dataset whose last dimension contains real/imaginary
	// samples. The session retains every HDF5 handle.
	HDF5IO_API int readInterleavedComplexFloat(ReadSession* session, const char* datasetName,
		cv::Mat& real, cv::Mat& imaginary);
	// A null or empty objectPath addresses file-root attributes; otherwise the
	// path identifies an HDF5 object such as "/S01/IMG".
	HDF5IO_API int readStringAttribute(ReadSession* session, const char* objectPath,
		const char* attributeName, std::string& value);
	HDF5IO_API int readArrayAttribute(ReadSession* session, const char* objectPath,
		const char* attributeName, cv::Mat& value);
	// Keeps a writable file handle inside Hdf5IO for a batch of create-only
	// writes. The overloads retain the legacy failure-on-existing-dataset rule.
	HDF5IO_API WriteSession* openWriteSession(const char* filename);
	HDF5IO_API void closeWriteSession(WriteSession* session);
	HDF5IO_API int writeArray(WriteSession* session, const char* datasetName, const cv::Mat& input);
	HDF5IO_API int writeArrayReplace(WriteSession* session, const char* datasetName, const cv::Mat& input);
	HDF5IO_API int createString(WriteSession* session, const char* datasetName, const char* value);
	// Creates a dataset and fails if it already exists.
	HDF5IO_API int writeArray(const char* filename, const char* datasetName, const cv::Mat& input);
	// Replaces an existing dataset with the supplied matrix.
	HDF5IO_API int writeArrayReplace(const char* filename, const char* datasetName, const cv::Mat& input);
	// Creates a chunked 2-D dataset without materializing data. Zero datasets
	// retain the legacy logical fill-value behaviour.
	HDF5IO_API int createEmptyDataset(const char* filename, const char* datasetName, int rows, int columns,
		int cvType, int chunkRows = 256, int chunkColumns = 256);
	HDF5IO_API int createZeroDataset(const char* filename, const char* datasetName, int rows, int columns,
		int cvType, int chunkRows = 256, int chunkColumns = 256);
	HDF5IO_API int writeSubarray(
		const char* filename,
		const char* datasetName,
		const cv::Mat& input,
		int offsetRow,
		int offsetColumn);
	HDF5IO_API int writeDouble(const char* filename, const char* datasetName, double value);
	HDF5IO_API int writeInt(const char* filename, const char* datasetName, int value);
	HDF5IO_API int readDouble(const char* filename, const char* datasetName, double* value);
	HDF5IO_API int readInt(const char* filename, const char* datasetName, int* value);
	HDF5IO_API int writeString(const char* filename, const char* datasetName, const char* value);
	// Creates a fixed-length legacy-compatible string dataset and fails when it exists.
	HDF5IO_API int createString(const char* filename, const char* datasetName, const char* value);
	// Removes a dataset. Returns 0 when removed, 1 when absent, and a negative
	// value on HDF5 failure.
	HDF5IO_API int removeDatasetIfPresent(const char* filename, const char* datasetName);
	// Copies a dataset when it exists in the source. Returns 0 when copied, 1
	// when the source is absent or the destination exists and replacement is off,
	// and a negative value on HDF5 failure.
	HDF5IO_API int copyDatasetIfPresent(const char* sourceFilename, const char* destinationFilename,
		const char* datasetName, bool replaceExisting);
	// Batch form of copyDatasetIfPresent. Missing source datasets are skipped;
	// invalid names and HDF5 delete/copy failures are returned to the caller.
	HDF5IO_API int copyDatasetsIfPresent(const char* sourceFilename, const char* destinationFilename,
		const char* const* datasetNames, int datasetCount, bool replaceExisting);
	HDF5IO_API int readString(const char* filename, const char* datasetName, std::string& value);
	HDF5IO_API int readStringDiagnosed(const char* filename, const char* datasetName,
		std::string& value, Hdf5ReadDiagnostic* diagnostic);
	HDF5IO_API int readSubarray(
		const char* filename,
		const char* datasetName,
		int offsetRow,
		int offsetColumn,
		int rows,
		int columns,
		cv::Mat& output);
}
