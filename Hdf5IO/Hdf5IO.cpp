#include "../include/Hdf5IO.h"

#define NOMINMAX
#include <windows.h>
#include <hdf5.h>

#include <mutex>
#include <algorithm>
#include <limits>
#include <new>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr wchar_t kHdf5MutexName[] = L"Local\\InSAR.Hdf5IO.v1";

	class ScopedHdf5Lock
	{
	public:
		ScopedHdf5Lock() : mutex_(CreateMutexW(nullptr, FALSE, kHdf5MutexName)), result_(-1)
		{
			if (!mutex_)
				return;

			const DWORD waitResult = WaitForSingleObject(mutex_, INFINITE);
			if (waitResult == WAIT_OBJECT_0)
				result_ = 0;
			else if (waitResult == WAIT_ABANDONED)
				result_ = Hdf5IO::kLockAbandoned;
		}

		~ScopedHdf5Lock()
		{
			if (mutex_)
			{
				if (result_ == 0 || result_ == Hdf5IO::kLockAbandoned)
					ReleaseMutex(mutex_);
				CloseHandle(mutex_);
			}
		}

		int result() const noexcept { return result_; }

	private:
		HANDLE mutex_;
		int result_;
	};

	class ScopedH5Id
	{
	public:
		explicit ScopedH5Id(hid_t id = -1, herr_t(*closer)(hid_t) = nullptr) : id_(id), closer_(closer) {}
		~ScopedH5Id() { if (id_ >= 0 && closer_) closer_(id_); }
		ScopedH5Id(const ScopedH5Id&) = delete;
		ScopedH5Id& operator=(const ScopedH5Id&) = delete;
		ScopedH5Id(ScopedH5Id&& other) noexcept : id_(other.id_), closer_(other.closer_)
		{
			other.id_ = -1;
			other.closer_ = nullptr;
		}
		ScopedH5Id& operator=(ScopedH5Id&& other) noexcept
		{
			if (this != &other)
			{
				if (id_ >= 0 && closer_) closer_(id_);
				id_ = other.id_;
				closer_ = other.closer_;
				other.id_ = -1;
				other.closer_ = nullptr;
			}
			return *this;
		}
		operator hid_t() const noexcept { return id_; }
		bool valid() const noexcept { return id_ >= 0; }

	private:
		hid_t id_;
		herr_t(*closer_)(hid_t);
	};

	int h5TypeToCvType(hid_t type)
	{
		const H5T_class_t typeClass = H5Tget_class(type);
		const size_t typeSize = H5Tget_size(type);
		if (typeClass == H5T_FLOAT)
			return typeSize == sizeof(float) ? CV_32F : (typeSize == sizeof(double) ? CV_64F : -1);
		if (typeClass == H5T_INTEGER)
		{
			if (typeSize == sizeof(unsigned char) && H5Tget_sign(type) == H5T_SGN_NONE) return CV_8U;
			return typeSize == sizeof(short) ? CV_16S : (typeSize == sizeof(int) ? CV_32S : -1);
		}
		return -1;
	}

	hid_t cvTypeToNativeH5Type(int cvType)
	{
		switch (cvType)
		{
		case CV_16S: return H5T_NATIVE_SHORT;
		case CV_32S: return H5T_NATIVE_INT;
		case CV_8U: return H5T_NATIVE_UCHAR;
		case CV_32F: return H5T_NATIVE_FLOAT;
		case CV_64F: return H5T_NATIVE_DOUBLE;
		default: return -1;
		}
	}

	std::string datasetPath(const char* datasetName)
	{
		if (!datasetName || !*datasetName) return std::string();
		return datasetName[0] == '/' ? std::string(datasetName) : std::string("/") + datasetName;
	}

	bool isRegularFile(const char* filename)
	{
		if (!filename || !*filename) return false;
		const DWORD attributes = GetFileAttributesA(filename);
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	int openDataset(const char* filename, const char* datasetName, ScopedH5Id& file, ScopedH5Id& dataset)
	{
		if (!isRegularFile(filename)) return -1;
		file = ScopedH5Id(H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
		if (!file.valid()) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		if (H5Lexists(file, path.c_str(), H5P_DEFAULT) <= 0) return -1;
		dataset = ScopedH5Id(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
		return dataset.valid() ? 0 : -1;
	}

	int openWritableFile(const char* filename, ScopedH5Id& file)
	{
		if (!isRegularFile(filename)) return -1;
		file = ScopedH5Id(H5Fopen(filename, H5F_ACC_RDWR, H5P_DEFAULT), H5Fclose);
		return file.valid() ? 0 : -1;
	}

	int openOrCreateWritableFile(const char* filename, ScopedH5Id& file)
	{
		if (!filename) return -1;
		if (openWritableFile(filename, file) == 0) return 0;
		file = ScopedH5Id(H5Fcreate(filename, H5F_ACC_EXCL, H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
		return file.valid() ? 0 : -1;
	}

	int createChunkedDataset(
		const char* filename, const char* datasetName, int rows, int columns, int cvType,
		int chunkRows, int chunkColumns, bool zeroFill)
	{
		if (!filename || !datasetName || rows < 1 || columns < 1 || chunkRows < 1 || chunkColumns < 1) return -1;
		const hid_t type = cvTypeToNativeH5Type(cvType);
		const std::string path = datasetPath(datasetName);
		if (type < 0 || path.empty()) return -1;
		ScopedH5Id file;
		if (openOrCreateWritableFile(filename, file) != 0 || H5Lexists(file, path.c_str(), H5P_DEFAULT) > 0) return -1;
		const hsize_t dimensions[2] = { static_cast<hsize_t>(rows), static_cast<hsize_t>(columns) };
		const hsize_t chunkDimensions[2] = {
			static_cast<hsize_t>(std::min(rows, chunkRows)), static_cast<hsize_t>(std::min(columns, chunkColumns)) };
		ScopedH5Id space(H5Screate_simple(2, dimensions, nullptr), H5Sclose);
		ScopedH5Id properties(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
		if (!space.valid() || !properties.valid() || H5Pset_chunk(properties, 2, chunkDimensions) < 0 ||
			H5Pset_alloc_time(properties, H5D_ALLOC_TIME_INCR) < 0 ||
			H5Pset_fill_time(properties, zeroFill ? H5D_FILL_TIME_IFSET : H5D_FILL_TIME_NEVER) < 0) return -1;
		double fillValue = 0.0;
		if (zeroFill && H5Pset_fill_value(properties, type, &fillValue) < 0) return -1;
		ScopedH5Id dataset(H5Dcreate2(file, path.c_str(), type, space, H5P_DEFAULT, properties, H5P_DEFAULT), H5Dclose);
		return dataset.valid() ? 0 : -1;
	}

	int writeArrayLocked(hid_t file, const char* datasetName, const cv::Mat& input, bool replaceExisting)
	{
		if (!datasetName || input.empty() || input.channels() != 1) return -1;
		const hid_t type = cvTypeToNativeH5Type(input.type());
		const std::string path = datasetPath(datasetName);
		if (type < 0 || path.empty()) return -1;
		if (H5Lexists(file, path.c_str(), H5P_DEFAULT) > 0)
		{
			if (!replaceExisting || H5Ldelete(file, path.c_str(), H5P_DEFAULT) < 0) return -1;
		}
		const hsize_t dimensions[2] = { static_cast<hsize_t>(input.rows), static_cast<hsize_t>(input.cols) };
		ScopedH5Id space(H5Screate_simple(2, dimensions, nullptr), H5Sclose);
		ScopedH5Id dataset(H5Dcreate2(file, path.c_str(), type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
		if (!space.valid() || !dataset.valid()) return -1;
		return H5Dwrite(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, input.data) < 0 ? -1 : 0;
	}

	int replaceScalarDataset(hid_t file, const char* datasetName, hid_t type, const void* value, bool matrixLayout)
	{
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		if (H5Lexists(file, path.c_str(), H5P_DEFAULT) > 0 && H5Ldelete(file, path.c_str(), H5P_DEFAULT) < 0)
			return -1;
		const hsize_t matrixDimensions[2] = { 1, 1 };
		const hsize_t scalarDimensions[1] = { 1 };
		ScopedH5Id space(
			matrixLayout ? H5Screate_simple(2, matrixDimensions, nullptr) : H5Screate_simple(1, scalarDimensions, nullptr),
			H5Sclose);
		ScopedH5Id dataset(H5Dcreate2(file, path.c_str(), type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
		if (!space.valid() || !dataset.valid()) return -1;
		return H5Dwrite(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, value) < 0 ? -1 : 0;
	}

	int createStringLocked(hid_t file, const char* datasetName, const char* value)
	{
		if (file < 0 || !datasetName || !value || !*value) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty() || H5Lexists(file, path.c_str(), H5P_DEFAULT) > 0) return -1;
		const hsize_t dimensions[1] = { 1 };
		ScopedH5Id space(H5Screate_simple(1, dimensions, nullptr), H5Sclose);
		ScopedH5Id fileType(H5Tcopy(H5T_FORTRAN_S1), H5Tclose);
		ScopedH5Id memoryType(H5Tcopy(H5T_C_S1), H5Tclose);
		const size_t length = strlen(value);
		if (!space.valid() || !fileType.valid() || !memoryType.valid() || H5Tset_size(fileType, length) < 0 ||
			H5Tset_size(memoryType, length) < 0) return -1;
		ScopedH5Id dataset(H5Dcreate2(file, path.c_str(), fileType, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
		if (!dataset.valid()) return -1;
		return H5Dwrite(dataset, memoryType, H5S_ALL, H5S_ALL, H5P_DEFAULT, value) < 0 ? -1 : 0;
	}

	int readSubarrayLocked(hid_t file, const char* datasetName, int offsetRow, int offsetColumn,
		int rows, int columns, cv::Mat& output)
	{
		if (file < 0 || !datasetName || offsetRow < 0 || offsetColumn < 0 || rows < 1 || columns < 1)
			return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty() || H5Lexists(file, path.c_str(), H5P_DEFAULT) <= 0) return -1;
		ScopedH5Id dataset(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
		ScopedH5Id space(dataset.valid() ? H5Dget_space(dataset) : -1, H5Sclose);
		ScopedH5Id type(dataset.valid() ? H5Dget_type(dataset) : -1, H5Tclose);
		if (!space.valid() || !type.valid() || H5Sget_simple_extent_ndims(space) != 2) return -1;
		hsize_t dimensions[2] = {};
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 ||
			static_cast<hsize_t>(offsetRow) + rows > dimensions[0] ||
			static_cast<hsize_t>(offsetColumn) + columns > dimensions[1]) return -1;
		const int cvType = h5TypeToCvType(type);
		const hid_t nativeType = cvTypeToNativeH5Type(cvType);
		if (nativeType < 0) return -1;

		output.create(rows, columns, cvType);
		const hsize_t offset[2] = { static_cast<hsize_t>(offsetRow), static_cast<hsize_t>(offsetColumn) };
		const hsize_t count[2] = { static_cast<hsize_t>(rows), static_cast<hsize_t>(columns) };
		ScopedH5Id memory(H5Screate_simple(2, count, nullptr), H5Sclose);
		if (!memory.valid() || H5Sselect_hyperslab(space, H5S_SELECT_SET, offset, nullptr, count, nullptr) < 0)
			return -1;
		return H5Dread(dataset, nativeType, memory, space, H5P_DEFAULT, output.data) < 0 ? -1 : 0;
	}
}

namespace Hdf5IO
{
	struct ReadSession
	{
		hid_t file;
	};

	struct WriteSession
	{
		hid_t file;
	};

	struct BatchLock
	{
		ScopedHdf5Lock lock;
	};

	BatchLock* acquireBatchLock()
	{
		return new (std::nothrow) BatchLock();
	}

	int getBatchLockStatus(const BatchLock* lock)
	{
		return lock ? lock->lock.result() : -1;
	}

	void releaseBatchLock(BatchLock* lock)
	{
		delete lock;
	}

	static hid_t outputCvTypeToH5Type(int cvType)
	{
		switch (cvType)
		{
		case CV_8U: return H5T_NATIVE_INT8; // Preserves legacy quality-flag bit representation.
		case CV_16S: return H5T_NATIVE_INT16;
		case CV_32S: return H5T_NATIVE_INT;
		case CV_32F: return H5T_NATIVE_FLOAT;
		case CV_64F: return H5T_NATIVE_DOUBLE;
		default: return -1;
		}
	}

	int createFile(const char* filename)
	{
		if (!filename) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file(H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
		return file.valid() ? 0 : -1;
	}

	int getDatasetDims(const char* filename, const char* datasetName, int* rows, int* columns)
	{
		if (!filename || !datasetName || !rows || !columns) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		ScopedH5Id file;
		ScopedH5Id dataset;
		if (openDataset(filename, datasetName, file, dataset) != 0) return -1;
		ScopedH5Id space(H5Dget_space(dataset), H5Sclose);
		const int rank = space.valid() ? H5Sget_simple_extent_ndims(space) : -1;
		if (rank < 1) return -1;
		std::vector<hsize_t> dimensions(rank);
		if (H5Sget_simple_extent_dims(space, dimensions.data(), nullptr) < 0 ||
			dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			(rank > 1 && dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max()))) return -1;
		*rows = static_cast<int>(dimensions[0]);
		*columns = rank == 1 ? 1 : static_cast<int>(dimensions[1]);
		return 0;
	}

	int readArray(const char* filename, const char* datasetName, cv::Mat& output)
	{
		if (!filename || !datasetName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		ScopedH5Id dataset;
		if (openDataset(filename, datasetName, file, dataset) != 0) return -1;
		ScopedH5Id space(H5Dget_space(dataset), H5Sclose);
		ScopedH5Id type(H5Dget_type(dataset), H5Tclose);
		const int rank = space.valid() ? H5Sget_simple_extent_ndims(space) : -1;
		const int cvType = type.valid() ? h5TypeToCvType(type) : -1;
		if (rank < 1 || rank > 2 || cvType < 0) return -1;
		hsize_t dimensions[2] = { 1, 1 };
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 ||
			dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			(rank == 2 && dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max()))) return -1;
		const int rows = static_cast<int>(dimensions[0]);
		const int columns = rank == 1 ? 1 : static_cast<int>(dimensions[1]);
		output.create(rows, columns, cvType);
		const hid_t nativeType = cvTypeToNativeH5Type(cvType);
		return H5Dread(dataset, nativeType, H5S_ALL, H5S_ALL, H5P_DEFAULT, output.data) < 0 ? -1 : 0;
	}

	ReadSession* openReadSession(const char* filename)
	{
		if (!filename) return nullptr;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return nullptr;
		const hid_t file = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
		if (file < 0) return nullptr;
		ReadSession* session = new (std::nothrow) ReadSession{ file };
		if (!session) H5Fclose(file);
		return session;
	}

	void closeReadSession(ReadSession* session)
	{
		if (!session) return;
		ScopedHdf5Lock lock;
		if (lock.result() == 0 && session->file >= 0) H5Fclose(session->file);
		delete session;
	}

	WriteSession* openWriteSession(const char* filename)
	{
		if (!filename) return nullptr;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return nullptr;
		const hid_t file = H5Fopen(filename, H5F_ACC_RDWR, H5P_DEFAULT);
		if (file < 0) return nullptr;
		WriteSession* session = new (std::nothrow) WriteSession{ file };
		if (!session) H5Fclose(file);
		return session;
	}

	void closeWriteSession(WriteSession* session)
	{
		if (!session) return;
		ScopedHdf5Lock lock;
		if (lock.result() == 0 && session->file >= 0) H5Fclose(session->file);
		delete session;
	}

	int writeArray(WriteSession* session, const char* datasetName, const cv::Mat& input)
	{
		if (!session || session->file < 0 || !datasetName || input.empty() || input.channels() != 1) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return writeArrayLocked(session->file, datasetName, input, false);
	}

	int writeArrayReplace(WriteSession* session, const char* datasetName, const cv::Mat& input)
	{
		if (!session || session->file < 0 || !datasetName || input.empty() || input.channels() != 1) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return writeArrayLocked(session->file, datasetName, input, true);
	}

	int createString(WriteSession* session, const char* datasetName, const char* value)
	{
		if (!session || session->file < 0 || !datasetName || !value || !*value) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return createStringLocked(session->file, datasetName, value);
	}

	int readArray(ReadSession* session, const char* datasetName, cv::Mat& output)
	{
		if (!session || session->file < 0 || !datasetName) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id dataset(H5Dopen2(session->file, path.c_str(), H5P_DEFAULT), H5Dclose);
		ScopedH5Id space(dataset.valid() ? H5Dget_space(dataset) : -1, H5Sclose);
		ScopedH5Id type(dataset.valid() ? H5Dget_type(dataset) : -1, H5Tclose);
		const int rank = space.valid() ? H5Sget_simple_extent_ndims(space) : -1;
		const int cvType = type.valid() ? h5TypeToCvType(type) : -1;
		if (!dataset.valid() || !space.valid() || !type.valid() || rank < 1 || rank > 2 || cvType < 0) return -1;
		hsize_t dimensions[2] = { 1, 1 };
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 ||
			dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			(rank == 2 && dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max()))) return -1;
		output.create(static_cast<int>(dimensions[0]), rank == 1 ? 1 : static_cast<int>(dimensions[1]), cvType);
		return H5Dread(dataset, cvTypeToNativeH5Type(cvType), H5S_ALL, H5S_ALL, H5P_DEFAULT, output.data) < 0 ? -1 : 0;
	}

	int readArray(ReadSession* session, const char* datasetName, int outputCvType, cv::Mat& output)
	{
		if (!session || session->file < 0 || !datasetName) return -1;
		const std::string path = datasetPath(datasetName);
		const hid_t outputType = outputCvTypeToH5Type(outputCvType);
		if (path.empty() || outputType < 0) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		if (H5Lexists(session->file, path.c_str(), H5P_DEFAULT) <= 0) return -1;
		ScopedH5Id dataset(H5Dopen2(session->file, path.c_str(), H5P_DEFAULT), H5Dclose);
		ScopedH5Id space(dataset.valid() ? H5Dget_space(dataset) : -1, H5Sclose);
		const int rank = space.valid() ? H5Sget_simple_extent_ndims(space) : -1;
		if (!dataset.valid() || rank < 1 || rank > 2) return -1;
		hsize_t dimensions[2] = { 1, 1 };
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 ||
			dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			(rank == 2 && dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max()))) return -1;
		output.create(static_cast<int>(dimensions[0]), rank == 1 ? 1 : static_cast<int>(dimensions[1]), outputCvType);
		return H5Dread(dataset, outputType, H5S_ALL, H5S_ALL, H5P_DEFAULT, output.data) < 0 ? -1 : 0;
	}

	int readSubarray(ReadSession* session, const char* datasetName, int offsetRow, int offsetColumn,
		int rows, int columns, cv::Mat& output)
	{
		if (!session || session->file < 0) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return readSubarrayLocked(session->file, datasetName, offsetRow, offsetColumn, rows, columns, output);
	}

	int readInterleavedComplexFloat(ReadSession* session, const char* datasetName,
		cv::Mat& real, cv::Mat& imaginary)
	{
		if (!session || session->file < 0 || !datasetName) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id dataset(H5Dopen2(session->file, path.c_str(), H5P_DEFAULT), H5Dclose);
		ScopedH5Id space(dataset.valid() ? H5Dget_space(dataset) : -1, H5Sclose);
		if (!dataset.valid() || !space.valid() || H5Sget_simple_extent_ndims(space) != 3) return -1;
		hsize_t dimensions[3] = { 0, 0, 0 };
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 || dimensions[2] != 2 ||
			dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max())) return -1;
		const size_t rows = static_cast<size_t>(dimensions[0]);
		const size_t columns = static_cast<size_t>(dimensions[1]);
		if (rows == 0 || columns == 0 || rows > std::numeric_limits<size_t>::max() / columns / 2) return -1;
		std::vector<float> interleaved;
		try
		{
			interleaved.resize(rows * columns * 2);
		}
		catch (const std::bad_alloc&)
		{
			return -1;
		}
		if (H5Dread(dataset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, interleaved.data()) < 0) return -1;
		real.create(static_cast<int>(rows), static_cast<int>(columns), CV_32F);
		imaginary.create(static_cast<int>(rows), static_cast<int>(columns), CV_32F);
		for (int row = 0; row < real.rows; ++row)
		{
			const float* source = interleaved.data() + static_cast<size_t>(row) * columns * 2;
			float* realRow = real.ptr<float>(row);
			float* imaginaryRow = imaginary.ptr<float>(row);
			for (int column = 0; column < real.cols; ++column)
			{
				realRow[column] = source[2 * column];
				imaginaryRow[column] = source[2 * column + 1];
			}
		}
		return 0;
	}

	int readStringAttribute(ReadSession* session, const char* objectPath, const char* attributeName, std::string& value)
	{
		if (!session || session->file < 0 || !attributeName || !*attributeName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id object;
		hid_t objectId = session->file;
		if (objectPath && *objectPath)
		{
			const std::string path = datasetPath(objectPath);
			object = ScopedH5Id(H5Oopen(session->file, path.c_str(), H5P_DEFAULT), H5Oclose);
			if (!object.valid()) return -1;
			objectId = object;
		}
		ScopedH5Id attribute(H5Aopen(objectId, attributeName, H5P_DEFAULT), H5Aclose);
		ScopedH5Id fileType(attribute.valid() ? H5Aget_type(attribute) : -1, H5Tclose);
		ScopedH5Id space(attribute.valid() ? H5Aget_space(attribute) : -1, H5Sclose);
		if (!attribute.valid() || !fileType.valid() || H5Tget_class(fileType) != H5T_STRING || H5Tis_variable_str(fileType) > 0) return -1;
		const size_t stringSize = H5Tget_size(fileType);
		const hssize_t pointCount = space.valid() ? H5Sget_simple_extent_npoints(space) : -1;
		if (stringSize == 0 || stringSize == std::numeric_limits<size_t>::max() || pointCount < 1 ||
			static_cast<size_t>(pointCount) > std::numeric_limits<size_t>::max() / (stringSize + 1)) return -1;
		ScopedH5Id memoryType(H5Tcopy(H5T_C_S1), H5Tclose);
		if (!memoryType.valid() || H5Tset_size(memoryType, stringSize + 1) < 0) return -1;
		std::vector<char> buffer(static_cast<size_t>(pointCount) * (stringSize + 1), '\0');
		if (H5Aread(attribute, memoryType, buffer.data()) < 0) return -1;
		value.assign(buffer.data());
		return 0;
	}

	int readArrayAttribute(ReadSession* session, const char* objectPath, const char* attributeName, cv::Mat& value)
	{
		if (!session || session->file < 0 || !attributeName || !*attributeName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id object;
		hid_t objectId = session->file;
		if (objectPath && *objectPath)
		{
			const std::string path = datasetPath(objectPath);
			object = ScopedH5Id(H5Oopen(session->file, path.c_str(), H5P_DEFAULT), H5Oclose);
			if (!object.valid()) return -1;
			objectId = object;
		}
		ScopedH5Id attribute(H5Aopen(objectId, attributeName, H5P_DEFAULT), H5Aclose);
		ScopedH5Id fileType(attribute.valid() ? H5Aget_type(attribute) : -1, H5Tclose);
		ScopedH5Id space(attribute.valid() ? H5Aget_space(attribute) : -1, H5Sclose);
		const int cvType = fileType.valid() ? h5TypeToCvType(fileType) : -1;
		const int rank = space.valid() ? H5Sget_simple_extent_ndims(space) : -1;
		if (!attribute.valid() || !fileType.valid() || !space.valid() || rank < 0 || rank > 2 ||
			(cvType != CV_16S && cvType != CV_32S && cvType != CV_32F && cvType != CV_64F)) return -1;
		hsize_t dimensions[2] = { 1, 1 };
		if (rank > 0 && H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0) return -1;
		if (dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max())) return -1;
		value.create(static_cast<int>(dimensions[0]), static_cast<int>(dimensions[1]), cvType);
		return H5Aread(attribute, cvTypeToNativeH5Type(cvType), value.data) < 0 ? -1 : 0;
	}

	int writeArray(const char* filename, const char* datasetName, const cv::Mat& input)
	{
		if (!filename || !datasetName || input.empty() || input.channels() != 1) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		return writeArrayLocked(file, datasetName, input, false);
	}

	int writeArrayReplace(const char* filename, const char* datasetName, const cv::Mat& input)
	{
		if (!filename || !datasetName || input.empty() || input.channels() != 1) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		return writeArrayLocked(file, datasetName, input, true);
	}

	int createEmptyDataset(const char* filename, const char* datasetName, int rows, int columns,
		int cvType, int chunkRows, int chunkColumns)
	{
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return createChunkedDataset(filename, datasetName, rows, columns, cvType, chunkRows, chunkColumns, false);
	}

	int createZeroDataset(const char* filename, const char* datasetName, int rows, int columns,
		int cvType, int chunkRows, int chunkColumns)
	{
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return createChunkedDataset(filename, datasetName, rows, columns, cvType, chunkRows, chunkColumns, true);
	}

	int writeSubarray(const char* filename, const char* datasetName, const cv::Mat& input, int offsetRow, int offsetColumn)
	{
		if (!filename || !datasetName || input.empty() || offsetRow < 0 || offsetColumn < 0) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		ScopedH5Id dataset(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
		ScopedH5Id space(dataset.valid() ? H5Dget_space(dataset) : -1, H5Sclose);
		ScopedH5Id type(dataset.valid() ? H5Dget_type(dataset) : -1, H5Tclose);
		if (!dataset.valid() || !space.valid() || !type.valid() || H5Sget_simple_extent_ndims(space) != 2) return -1;
		hsize_t dimensions[2] = {};
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 ||
			static_cast<hsize_t>(offsetRow) + input.rows > dimensions[0] ||
			static_cast<hsize_t>(offsetColumn) + input.cols > dimensions[1]) return -1;
		const int expectedCvType = h5TypeToCvType(type);
		if (expectedCvType < 0 || input.type() != expectedCvType) return -1;
		const hid_t nativeType = cvTypeToNativeH5Type(expectedCvType);
		const hsize_t offset[2] = { static_cast<hsize_t>(offsetRow), static_cast<hsize_t>(offsetColumn) };
		const hsize_t count[2] = { static_cast<hsize_t>(input.rows), static_cast<hsize_t>(input.cols) };
		ScopedH5Id memory(H5Screate_simple(2, count, nullptr), H5Sclose);
		if (!memory.valid() || H5Sselect_hyperslab(space, H5S_SELECT_SET, offset, nullptr, count, nullptr) < 0) return -1;
		return H5Dwrite(dataset, nativeType, memory, space, H5P_DEFAULT, input.data) < 0 ? -1 : 0;
	}

	int writeDouble(const char* filename, const char* datasetName, double value)
	{
		if (!filename || !datasetName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		return replaceScalarDataset(file, datasetName, H5T_NATIVE_DOUBLE, &value, true);
	}

	int writeInt(const char* filename, const char* datasetName, int value)
	{
		if (!filename || !datasetName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		return replaceScalarDataset(file, datasetName, H5T_NATIVE_INT, &value, true);
	}

	int readDouble(const char* filename, const char* datasetName, double* value)
	{
		if (!filename || !datasetName || !value) return -1;
		cv::Mat data;
		if (readArray(filename, datasetName, data) != 0 || data.total() != 1) return -1;
		data.convertTo(data, CV_64F);
		*value = data.at<double>(0, 0);
		return 0;
	}

	int readInt(const char* filename, const char* datasetName, int* value)
	{
		if (!filename || !datasetName || !value) return -1;
		cv::Mat data;
		if (readArray(filename, datasetName, data) != 0 || data.total() != 1) return -1;
		data.convertTo(data, CV_32S);
		*value = data.at<int>(0, 0);
		return 0;
	}

	int writeString(const char* filename, const char* datasetName, const char* value)
	{
		if (!filename || !datasetName || !value) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		ScopedH5Id type(H5Tcopy(H5T_C_S1), H5Tclose);
		if (!type.valid() || H5Tset_size(type, strlen(value) + 1) < 0 || H5Tset_strpad(type, H5T_STR_NULLTERM) < 0) return -1;
		return replaceScalarDataset(file, datasetName, type, value, false);
	}

	int createString(const char* filename, const char* datasetName, const char* value)
	{
		if (!filename) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		return createStringLocked(file, datasetName, value);
	}

	int removeDatasetIfPresent(const char* filename, const char* datasetName)
	{
		if (!filename || !datasetName) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		if (openWritableFile(filename, file) != 0) return -1;
		if (H5Lexists(file, path.c_str(), H5P_DEFAULT) <= 0) return 1;
		return H5Ldelete(file, path.c_str(), H5P_DEFAULT) < 0 ? -1 : 0;
	}

	int copyDatasetIfPresent(const char* sourceFilename, const char* destinationFilename,
		const char* datasetName, bool replaceExisting)
	{
		if (!sourceFilename || !destinationFilename || !datasetName) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id source(H5Fopen(sourceFilename, H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
		ScopedH5Id destination(H5Fopen(destinationFilename, H5F_ACC_RDWR, H5P_DEFAULT), H5Fclose);
		if (!source.valid() || !destination.valid()) return -1;
		if (H5Lexists(source, path.c_str(), H5P_DEFAULT) <= 0) return 1;
		if (H5Lexists(destination, path.c_str(), H5P_DEFAULT) > 0)
		{
			if (!replaceExisting) return 1;
			if (H5Ldelete(destination, path.c_str(), H5P_DEFAULT) < 0) return -1;
		}
		return H5Ocopy(source, path.c_str(), destination, path.c_str(), H5P_DEFAULT, H5P_DEFAULT) < 0 ? -1 : 0;
	}

	int copyDatasetsIfPresent(const char* sourceFilename, const char* destinationFilename,
		const char* const* datasetNames, int datasetCount, bool replaceExisting)
	{
		if (!sourceFilename || !destinationFilename || !datasetNames || datasetCount < 0) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id source(H5Fopen(sourceFilename, H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
		ScopedH5Id destination(H5Fopen(destinationFilename, H5F_ACC_RDWR, H5P_DEFAULT), H5Fclose);
		if (!source.valid() || !destination.valid()) return -1;
		for (int index = 0; index < datasetCount; ++index)
		{
			const std::string path = datasetPath(datasetNames[index]);
			if (path.empty() || H5Lexists(source, path.c_str(), H5P_DEFAULT) <= 0) continue;
			if (H5Lexists(destination, path.c_str(), H5P_DEFAULT) > 0)
			{
				if (!replaceExisting) continue;
				if (H5Ldelete(destination, path.c_str(), H5P_DEFAULT) < 0) continue;
			}
			H5Ocopy(source, path.c_str(), destination, path.c_str(), H5P_DEFAULT, H5P_DEFAULT);
		}
		return 0;
	}

	int readString(const char* filename, const char* datasetName, std::string& value)
	{
		if (!filename || !datasetName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedH5Id file;
		ScopedH5Id dataset;
		if (openDataset(filename, datasetName, file, dataset) != 0) return -1;
		ScopedH5Id type(H5Dget_type(dataset), H5Tclose);
		if (!type.valid() || H5Tget_class(type) != H5T_STRING || H5Tis_variable_str(type) > 0) return -1;
		const size_t length = H5Tget_size(type);
		if (length == 0 || length > 65536) return -1;
		std::vector<char> buffer(length + 1, '\0');
		if (H5Dread(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data()) < 0) return -1;
		value.assign(buffer.data());
		return 0;
	}

	int readSubarray(const char* filename, const char* datasetName, int offsetRow, int offsetColumn,
		int rows, int columns, cv::Mat& output)
	{
		if (!filename || !datasetName || offsetRow < 0 || offsetColumn < 0 || rows < 1 || columns < 1)
			return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		ScopedH5Id file(H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
		return file.valid() ? readSubarrayLocked(file, datasetName, offsetRow, offsetColumn, rows, columns, output) : -1;
	}
}
