#pragma once

#include <opencv2/core.hpp>
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

	// Holds the process-wide HDF5 mutex across a caller-defined group of Hdf5IO
	// calls. The handle is opaque so mutex ownership remains in this DLL.
	HDF5IO_API BatchLock* acquireBatchLock();
	HDF5IO_API int getBatchLockStatus(const BatchLock* lock);
	HDF5IO_API void releaseBatchLock(BatchLock* lock);

	// Creates or truncates an HDF5 file. Array creation APIs below intentionally
	// fail when the target dataset already exists, matching the legacy writer.
	HDF5IO_API int createFile(const char* filename);
	HDF5IO_API int getDatasetDims(const char* filename, const char* datasetName, int* rows, int* columns);
	HDF5IO_API int readArray(const char* filename, const char* datasetName, cv::Mat& output);
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
	// Batch form of copyDatasetIfPresent. Missing source datasets are skipped.
	HDF5IO_API int copyDatasetsIfPresent(const char* sourceFilename, const char* destinationFilename,
		const char* const* datasetNames, int datasetCount, bool replaceExisting);
	HDF5IO_API int readString(const char* filename, const char* datasetName, std::string& value);
	HDF5IO_API int readSubarray(
		const char* filename,
		const char* datasetName,
		int offsetRow,
		int offsetColumn,
		int rows,
		int columns,
		cv::Mat& output);
}
