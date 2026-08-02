#include "../include/Hdf5IO.h"

#define NOMINMAX
#include <windows.h>
#include <hdf5.h>

#include <mutex>
#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <cstring>
#include <clocale>
#include <deque>
#include <string>
#include <vector>

namespace
{
	bool isValidUtf8Path(const char* path)
	{
		if (!path || !*path) return false;
		return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0) > 0;
	}

	// HDF5 1.14's Windows VFD is backed by the C runtime's narrow I/O. Keep
	// its filename conversion on UTF-8 for the duration of the H5F call, rather
	// than inheriting the process ANSI code page.
	class ScopedUtf8FileLocale
	{
	public:
		bool activate()
		{
			const char* locale = setlocale(LC_CTYPE, nullptr);
			if (!locale) return false;
			previousLocale_ = locale;
			previousThreadMode_ = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
			if (previousThreadMode_ == -1) return false;
			threadLocaleEnabled_ = true;
			if (setlocale(LC_CTYPE, ".UTF8")) return true;
			_configthreadlocale(previousThreadMode_);
			threadLocaleEnabled_ = false;
			return false;
		}

		~ScopedUtf8FileLocale()
		{
			if (!threadLocaleEnabled_) return;
			setlocale(LC_CTYPE, previousLocale_.c_str());
			_configthreadlocale(previousThreadMode_);
		}

	private:
		std::string previousLocale_;
		int previousThreadMode_ = -1;
		bool threadLocaleEnabled_ = false;
	};

	HANDLE openFileAttributesUtf8(const char* path)
	{
		if (!path || !*path) return INVALID_HANDLE_VALUE;
		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
		if (length <= 0) return INVALID_HANDLE_VALUE;
		std::vector<wchar_t> widePath(static_cast<size_t>(length));
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath.data(), length) != length)
			return INVALID_HANDLE_VALUE;
		return CreateFileW(widePath.data(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	// Local names are shared by processes in the same Windows logon session.
	// This is a conservative implementation detail, not a documented
	// cross-process HDF5 coordination protocol.
	constexpr wchar_t kHdf5MutexName[] = L"Local\\InSAR.Hdf5IO.v1";

#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
	struct FileIdentity
	{
		unsigned long volumeSerialNumber = 0;
		unsigned long fileIndexHigh = 0;
		unsigned long fileIndexLow = 0;
		bool available = false;
	};

	struct AuditSnapshot
	{
		uint64_t generation = 0;
		unsigned int majorVersion = 0;
		unsigned int minorVersion = 0;
		unsigned int releaseVersion = 0;
		bool enabled = false;
	};

	struct AuditState
	{
		std::mutex mutex;
		std::deque<Hdf5IO::Hdf5AuditEvent> events;
		size_t capacity = 0;
		unsigned long long dropped = 0;
		uint64_t firstDroppedOperationId = 0;
		uint64_t lastDroppedOperationId = 0;
		uint64_t firstDroppedMonotonicMilliseconds = 0;
		uint64_t lastDroppedMonotonicMilliseconds = 0;
		std::atomic<uint64_t> nextFileHandleId{ 1 };
		bool enabled = false;
		bool evidenceComplete = true;
		unsigned int majorVersion = 0;
		unsigned int minorVersion = 0;
		unsigned int releaseVersion = 0;
		uint64_t generation = 0;
	};
	AuditState g_audit;
	thread_local uint64_t g_auditOperationId = 0;

	AuditSnapshot getAuditSnapshot()
	{
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		AuditSnapshot snapshot;
		snapshot.generation = g_audit.generation;
		snapshot.majorVersion = g_audit.majorVersion;
		snapshot.minorVersion = g_audit.minorVersion;
		snapshot.releaseVersion = g_audit.releaseVersion;
		snapshot.enabled = g_audit.enabled;
		return snapshot;
	}

	uint64_t reserveAuditFileHandleId(const AuditSnapshot& snapshot)
	{
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		if (!snapshot.enabled || !g_audit.enabled || snapshot.generation != g_audit.generation) return 0;
		return g_audit.nextFileHandleId.fetch_add(1);
	}

	void markAuditEvidenceIncomplete(const AuditSnapshot& snapshot)
	{
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		if (snapshot.enabled && g_audit.enabled && snapshot.generation == g_audit.generation)
			g_audit.evidenceComplete = false;
	}

	void appendAuditEvent(const AuditSnapshot& snapshot, const Hdf5IO::Hdf5AuditEvent& event)
	{
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		if (!snapshot.enabled || !g_audit.enabled || snapshot.generation != g_audit.generation) return;
		if (g_audit.events.size() >= g_audit.capacity)
		{
			if (g_audit.dropped == 0)
			{
				g_audit.firstDroppedOperationId = event.operationId;
				g_audit.firstDroppedMonotonicMilliseconds = event.monotonicMilliseconds;
			}
			++g_audit.dropped;
			g_audit.lastDroppedOperationId = event.operationId;
			g_audit.lastDroppedMonotonicMilliseconds = event.monotonicMilliseconds;
			g_audit.evidenceComplete = false;
			return;
		}
		g_audit.events.push_back(event);
	}

	bool getFileIdentity(const char* path, FileIdentity& identity)
	{
		// This opens the requested path after HDF5 has opened its own file. It is
		// useful event correlation only: an uncoordinated replacement can rebind
		// the path between those operations, so it cannot identify HDF5's object.
		identity = {};
		if (!path || !*path) return false;
		const HANDLE handle = openFileAttributesUtf8(path);
		if (handle == INVALID_HANDLE_VALUE) return false;
		BY_HANDLE_FILE_INFORMATION information = {};
		const BOOL result = GetFileInformationByHandle(handle, &information);
		CloseHandle(handle);
		if (!result) return false;
		identity.volumeSerialNumber = information.dwVolumeSerialNumber;
		identity.fileIndexHigh = information.nFileIndexHigh;
		identity.fileIndexLow = information.nFileIndexLow;
		identity.available = true;
		return true;
	}

	void recordFileEvent(const AuditSnapshot& snapshot, uint64_t operationId, int kind, uint64_t fileHandleId,
		const char* path, const char* mode, herr_t status, const FileIdentity& identity)
	{
		if (operationId == 0) markAuditEvidenceIncomplete(snapshot);
		Hdf5IO::Hdf5AuditEvent event = {};
		event.operationId = operationId;
		event.fileHandleId = fileHandleId;
		event.monotonicMilliseconds = GetTickCount64();
		event.threadId = GetCurrentThreadId();
		event.kind = kind;
		event.success = status >= 0 ? 1 : 0;
		event.hdf5Status = static_cast<int>(status);
		event.volumeSerialNumber = identity.volumeSerialNumber;
		event.fileIndexHigh = identity.fileIndexHigh;
		event.fileIndexLow = identity.fileIndexLow;
		event.hdf5MajorVersion = snapshot.majorVersion;
		event.hdf5MinorVersion = snapshot.minorVersion;
		event.hdf5ReleaseVersion = snapshot.releaseVersion;
		if (path) strncpy_s(event.path, path, _TRUNCATE);
		if (mode) strncpy_s(event.mode, mode, _TRUNCATE);
		appendAuditEvent(snapshot, event);
	}
#endif

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

	// The only owner for path-created HDF5 file IDs. It preserves the global
	// lock contract supplied by its callers while making every actual H5F open,
	// create, and close observable by the opt-in audit channel.
	class ScopedAuditedH5File
	{
	public:
		ScopedAuditedH5File() = default;
		~ScopedAuditedH5File() { close(); }
		ScopedAuditedH5File(const ScopedAuditedH5File&) = delete;
		ScopedAuditedH5File& operator=(const ScopedAuditedH5File&) = delete;

		int open(const char* path, unsigned flags, const char* mode)
		{
			if (file_ >= 0 || !isValidUtf8Path(path) || !mode) return -1;
			ScopedUtf8FileLocale utf8Locale;
			if (!utf8Locale.activate()) return -1;
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
			auditSnapshot_ = getAuditSnapshot();
			auditOperationId_ = g_auditOperationId;
#endif
			file_ = H5Fopen(path, flags, H5P_DEFAULT);
			if (file_ < 0)
			{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
				recordFileEvent(auditSnapshot_, auditOperationId_, Hdf5IO::HDF5_AUDIT_OPEN, 0, path, mode, -1, identity_);
#endif
				return -1;
			}
			recordSuccessfulOpen(path, mode);
			return 0;
		}

		int create(const char* path, unsigned flags, const char* mode)
		{
			if (file_ >= 0 || !isValidUtf8Path(path) || !mode) return -1;
			ScopedUtf8FileLocale utf8Locale;
			if (!utf8Locale.activate()) return -1;
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
			auditSnapshot_ = getAuditSnapshot();
			auditOperationId_ = g_auditOperationId;
#endif
			file_ = H5Fcreate(path, flags, H5P_DEFAULT, H5P_DEFAULT);
			if (file_ < 0)
			{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
				recordFileEvent(auditSnapshot_, auditOperationId_, Hdf5IO::HDF5_AUDIT_OPEN, 0, path, mode, -1, identity_);
#endif
				return -1;
			}
			recordSuccessfulOpen(path, mode);
			return 0;
		}

		herr_t close()
		{
			if (file_ < 0) return 0;
			const hid_t file = file_;
			file_ = -1;
			const herr_t status = H5Fclose(file);
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
			if (auditHandleId_ != 0)
				recordFileEvent(auditSnapshot_, auditOperationId_, Hdf5IO::HDF5_AUDIT_CLOSE, auditHandleId_, path_, mode_, status, identity_);
			auditHandleId_ = 0;
#endif
			return status;
		}

		// A failed/abandoned mutex acquisition must not issue an unprotected HDF5
		// close. Keep the legacy leak-on-lock-failure behavior, but flag any
		// active diagnostic run as incomplete instead of reporting a false pair.
		void abandonWithoutClose()
		{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
			if (file_ >= 0 && auditHandleId_ != 0) markAuditEvidenceIncomplete(auditSnapshot_);
			auditHandleId_ = 0;
#endif
			file_ = -1;
		}

		operator hid_t() const noexcept { return file_; }
		bool valid() const noexcept { return file_ >= 0; }

	private:
		void recordSuccessfulOpen(const char* path, const char* mode)
		{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
			strncpy_s(path_, path, _TRUNCATE);
			strncpy_s(mode_, mode, _TRUNCATE);
			auditHandleId_ = reserveAuditFileHandleId(auditSnapshot_);
			if (auditHandleId_ == 0) return;
			if (!getFileIdentity(path, identity_)) markAuditEvidenceIncomplete(auditSnapshot_);
			recordFileEvent(auditSnapshot_, auditOperationId_, Hdf5IO::HDF5_AUDIT_OPEN, auditHandleId_, path_, mode_, 0, identity_);
#else
			(void)path;
			(void)mode;
#endif
		}

		hid_t file_ = -1;
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
		uint64_t auditHandleId_ = 0;
		uint64_t auditOperationId_ = 0;
		char path_[512] = {};
		char mode_[16] = {};
		FileIdentity identity_;
		AuditSnapshot auditSnapshot_;
#endif
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
		const HANDLE file = openFileAttributesUtf8(filename);
		if (file == INVALID_HANDLE_VALUE) return false;
		BY_HANDLE_FILE_INFORMATION information = {};
		const BOOL result = GetFileInformationByHandle(file, &information);
		CloseHandle(file);
		return result && (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	int sameExistingFile(const char* firstFilename, const char* secondFilename, int* sameFile)
	{
		if (!firstFilename || !*firstFilename || !secondFilename || !*secondFilename || !sameFile) return -1;
		*sameFile = 0;

		// OPEN_EXISTING follows reparse points by default. FILE_SHARE_DELETE lets
		// this read-only identity probe coexist with the normal HDF5 open path.
		const HANDLE first = openFileAttributesUtf8(firstFilename);
		if (first == INVALID_HANDLE_VALUE) return -1;
		const HANDLE second = openFileAttributesUtf8(secondFilename);
		if (second == INVALID_HANDLE_VALUE)
		{
			CloseHandle(first);
			return -1;
		}

		BY_HANDLE_FILE_INFORMATION firstInfo = {};
		BY_HANDLE_FILE_INFORMATION secondInfo = {};
		const BOOL firstResult = GetFileInformationByHandle(first, &firstInfo);
		const BOOL secondResult = GetFileInformationByHandle(second, &secondInfo);
		CloseHandle(second);
		CloseHandle(first);
		if (!firstResult || !secondResult) return -1;

		*sameFile = firstInfo.dwVolumeSerialNumber == secondInfo.dwVolumeSerialNumber &&
			firstInfo.nFileIndexHigh == secondInfo.nFileIndexHigh &&
			firstInfo.nFileIndexLow == secondInfo.nFileIndexLow;
		return 0;
	}

	int openDataset(const char* filename, const char* datasetName, ScopedAuditedH5File& file, ScopedH5Id& dataset)
	{
		if (!isRegularFile(filename)) return -1;
		if (file.open(filename, H5F_ACC_RDONLY, "read") != 0) return -1;
		const std::string path = datasetPath(datasetName);
		if (path.empty()) return -1;
		if (H5Lexists(file, path.c_str(), H5P_DEFAULT) <= 0) return -1;
		dataset = ScopedH5Id(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
		return dataset.valid() ? 0 : -1;
	}

	struct Hdf5ErrorStackCapture
	{
		char* buffer;
		size_t capacity;
		size_t length;
	};

	herr_t appendHdf5Error(unsigned, const H5E_error2_t* error, void* data)
	{
		Hdf5ErrorStackCapture* capture = static_cast<Hdf5ErrorStackCapture*>(data);
		if (!capture || !capture->buffer || capture->length >= capture->capacity) return 0;
		const int written = _snprintf_s(capture->buffer + capture->length,
			capture->capacity - capture->length, _TRUNCATE,
			"%s:%u %s: %s\n", error && error->file_name ? error->file_name : "?",
			error ? error->line : 0, error && error->func_name ? error->func_name : "?",
			error && error->desc ? error->desc : "?");
		if (written > 0) capture->length += static_cast<size_t>(written);
		else capture->length = capture->capacity;
		return 0;
	}

	void captureHdf5ErrorStack(Hdf5IO::Hdf5ReadDiagnostic& diagnostic)
	{
		diagnostic.errorStack[0] = '\0';
		Hdf5ErrorStackCapture capture = { diagnostic.errorStack, sizeof(diagnostic.errorStack), 0 };
		if (H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, appendHdf5Error, &capture) < 0 || capture.length == 0)
			strcpy_s(diagnostic.errorStack, "No HDF5 error stack was available.");
	}

	class ScopedHdf5AutoErrorSilencer
	{
	public:
		ScopedHdf5AutoErrorSilencer() : callback_(nullptr), userData_(nullptr), active_(false)
		{
			if (H5Eget_auto2(H5E_DEFAULT, &callback_, &userData_) == 0 &&
				H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr) == 0) active_ = true;
		}

		~ScopedHdf5AutoErrorSilencer()
		{
			if (active_) H5Eset_auto2(H5E_DEFAULT, callback_, userData_);
		}

	private:
		H5E_auto2_t callback_;
		void* userData_;
		bool active_;
	};

	void describeHdf5Type(hid_t type, char* buffer, size_t bufferSize)
	{
		if (!buffer || bufferSize == 0) return;
		buffer[0] = '\0';
		const H5T_class_t typeClass = H5Tget_class(type);
		const size_t typeSize = H5Tget_size(type);
		const char* className = typeClass == H5T_FLOAT ? "float" :
			typeClass == H5T_INTEGER ? "integer" : "other";
		sprintf_s(buffer, bufferSize, "%s(%zu bytes)", className, typeSize);
	}

	int openWritableFile(const char* filename, ScopedAuditedH5File& file)
	{
		if (!isRegularFile(filename)) return -1;
		return file.open(filename, H5F_ACC_RDWR, "read-write");
	}

	int openOrCreateWritableFile(const char* filename, ScopedAuditedH5File& file)
	{
		if (!filename) return -1;
		if (openWritableFile(filename, file) == 0) return 0;
		return file.create(filename, H5F_ACC_EXCL, "create-excl");
	}

	int createChunkedDataset(
		const char* filename, const char* datasetName, int rows, int columns, int cvType,
		int chunkRows, int chunkColumns, bool zeroFill)
	{
		if (!filename || !datasetName || rows < 1 || columns < 1 || chunkRows < 1 || chunkColumns < 1) return -1;
		const hid_t type = cvTypeToNativeH5Type(cvType);
		const std::string path = datasetPath(datasetName);
		if (type < 0 || path.empty()) return -1;
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
	};

	struct WriteSession
	{
		ScopedAuditedH5File file;
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

	int getRuntimeInfo(Hdf5RuntimeInfo* info)
	{
		if (!info) return kInvalidArgument;
		*info = {};
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		unsigned int majorVersion = 0;
		unsigned int minorVersion = 0;
		unsigned int releaseVersion = 0;
		hbool_t isThreadSafe = 0;
		const herr_t versionStatus = H5get_libversion(&majorVersion, &minorVersion, &releaseVersion);
		const herr_t threadSafetyStatus = H5is_library_threadsafe(&isThreadSafe);
		if (versionStatus < 0 || threadSafetyStatus < 0) return -1;

		info->majorVersion = majorVersion;
		info->minorVersion = minorVersion;
		info->releaseVersion = releaseVersion;
		info->libraryThreadSafe = isThreadSafe != 0 ? 1 : 0;
		return 0;
	}

	int getRuntimeModulePath(char* buffer, size_t bufferSize)
	{
		if (!buffer || bufferSize < 2 || bufferSize > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
			return kInvalidArgument;
		buffer[0] = '\0';
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		HMODULE module = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(H5get_libversion), &module)) return -1;
		const DWORD pathLength = GetModuleFileNameA(module, buffer, static_cast<DWORD>(bufferSize));
		if (pathLength == 0 || pathLength >= bufferSize)
		{
			buffer[bufferSize - 1] = '\0';
			return -1;
		}
		return 0;
	}

	int enableAuditDiagnostics(size_t queueCapacity)
	{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
		if (queueCapacity == 0) return kInvalidArgument;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		unsigned int majorVersion = 0, minorVersion = 0, releaseVersion = 0;
		if (H5get_libversion(&majorVersion, &minorVersion, &releaseVersion) < 0) return -1;
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		g_audit.events.clear();
		g_audit.capacity = queueCapacity;
		g_audit.dropped = 0;
		g_audit.firstDroppedOperationId = 0;
		g_audit.lastDroppedOperationId = 0;
		g_audit.firstDroppedMonotonicMilliseconds = 0;
		g_audit.lastDroppedMonotonicMilliseconds = 0;
		g_audit.evidenceComplete = true;
		g_audit.majorVersion = majorVersion;
		g_audit.minorVersion = minorVersion;
		g_audit.releaseVersion = releaseVersion;
		++g_audit.generation;
		g_audit.enabled = true;
		return 0;
#else
		(void)queueCapacity;
		return kAuditDisabled;
#endif
	}

	void disableAuditDiagnostics()
	{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		g_audit.enabled = false;
		++g_audit.generation;
#endif
	}

	void setAuditOperationId(uint64_t operationId)
	{
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
		g_auditOperationId = operationId;
#else
		(void)operationId;
#endif
	}

	int getAuditStatus(Hdf5AuditStatus* status)
	{
		if (!status) return kInvalidArgument;
		*status = {};
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		status->enabled = g_audit.enabled ? 1 : 0;
		status->evidenceComplete = g_audit.evidenceComplete ? 1 : 0;
		status->droppedEventCount = g_audit.dropped;
		status->firstDroppedOperationId = g_audit.firstDroppedOperationId;
		status->lastDroppedOperationId = g_audit.lastDroppedOperationId;
		status->firstDroppedMonotonicMilliseconds = g_audit.firstDroppedMonotonicMilliseconds;
		status->lastDroppedMonotonicMilliseconds = g_audit.lastDroppedMonotonicMilliseconds;
		return 0;
#else
		return kAuditDisabled;
#endif
	}

	int pollAuditEvent(Hdf5AuditEvent* event)
	{
		if (!event) return kInvalidArgument;
#if defined(HDF5IO_ENABLE_AUDIT_DIAGNOSTICS)
		std::lock_guard<std::mutex> guard(g_audit.mutex);
		if (!g_audit.enabled) return kAuditDisabled;
		if (g_audit.events.empty()) return 1;
		*event = g_audit.events.front();
		g_audit.events.pop_front();
		return 0;
#else
		return kAuditDisabled;
#endif
	}

	int areSameExistingFile(const char* firstFilename, const char* secondFilename, int* sameFile)
	{
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		return sameExistingFile(firstFilename, secondFilename, sameFile);
	}

	int validateDistinctFilePaths(const char* sourceFilename, const char* outputFilename)
	{
		if (!sourceFilename || !*sourceFilename || !outputFilename || !*outputFilename) return kInvalidArgument;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		const HANDLE source = openFileAttributesUtf8(sourceFilename);
		if (source == INVALID_HANDLE_VALUE) return -1;
		const HANDLE output = openFileAttributesUtf8(outputFilename);
		if (output == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			CloseHandle(source);
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
		}

		BY_HANDLE_FILE_INFORMATION sourceInfo = {};
		BY_HANDLE_FILE_INFORMATION outputInfo = {};
		const BOOL sourceResult = GetFileInformationByHandle(source, &sourceInfo);
		const BOOL outputResult = GetFileInformationByHandle(output, &outputInfo);
		CloseHandle(output);
		CloseHandle(source);
		if (!sourceResult || !outputResult) return -1;
		return sourceInfo.dwVolumeSerialNumber == outputInfo.dwVolumeSerialNumber &&
			sourceInfo.nFileIndexHigh == outputInfo.nFileIndexHigh &&
			sourceInfo.nFileIndexLow == outputInfo.nFileIndexLow ? kInvalidArgument : 0;
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
		ScopedAuditedH5File file;
		return file.create(filename, H5F_ACC_TRUNC, "create-trunc");
	}

	int getDatasetDims(const char* filename, const char* datasetName, int* rows, int* columns)
	{
		if (!filename || !datasetName || !rows || !columns) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
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

	int readArrayDiagnosed(const char* filename, const char* datasetName, cv::Mat& output,
		Hdf5ReadDiagnostic* diagnostic)
	{
		Hdf5ReadDiagnostic localDiagnostic = {};
		Hdf5ReadDiagnostic& result = diagnostic ? *diagnostic : localDiagnostic;
		result = {};
		if (!filename || !*filename || !datasetName || !*datasetName)
		{
			result.stage = HDF5_READ_STAGE_INVALID_ARGUMENT;
			result.hdf5Status = kInvalidArgument;
			strcpy_s(result.errorStack, "Filename and dataset name are required.");
			return kInvalidArgument;
		}

		ScopedHdf5Lock lock;
		if (lock.result() != 0)
		{
			result.stage = HDF5_READ_STAGE_OPEN_FILE;
			result.hdf5Status = lock.result();
			strcpy_s(result.errorStack, "Unable to acquire the HDF5 operation lock.");
			return lock.result();
		}
		ScopedHdf5AutoErrorSilencer errorSilencer;
		auto fail = [&](int stage, int status) {
			result.stage = stage;
			result.hdf5Status = status;
			captureHdf5ErrorStack(result);
			return status;
		};

		if (!isRegularFile(filename))
		{
			result.stage = HDF5_READ_STAGE_OPEN_FILE;
			result.hdf5Status = -1;
			strcpy_s(result.errorStack, "The HDF5 path does not identify an existing regular file.");
			return -1;
		}

		H5Eclear2(H5E_DEFAULT);
		ScopedAuditedH5File file;
		if (file.open(filename, H5F_ACC_RDONLY, "read") != 0) return fail(HDF5_READ_STAGE_OPEN_FILE, -1);
		const std::string path = datasetPath(datasetName);
		if (path.empty())
		{
			result.stage = HDF5_READ_STAGE_DATASET_PATH;
			result.hdf5Status = kInvalidArgument;
			strcpy_s(result.errorStack, "The HDF5 dataset path is invalid.");
			return kInvalidArgument;
		}

		H5Eclear2(H5E_DEFAULT);
		const htri_t exists = H5Lexists(file, path.c_str(), H5P_DEFAULT);
		if (exists < 0) return fail(HDF5_READ_STAGE_OPEN_DATASET, static_cast<int>(exists));
		if (exists == 0)
		{
			result.stage = HDF5_READ_STAGE_OPEN_DATASET;
			result.hdf5Status = 0;
			strcpy_s(result.errorStack, "The requested dataset does not exist.");
			return -1;
		}

		H5Eclear2(H5E_DEFAULT);
		ScopedH5Id dataset(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
		if (!dataset.valid()) return fail(HDF5_READ_STAGE_OPEN_DATASET, -1);
		H5Eclear2(H5E_DEFAULT);
		ScopedH5Id space(H5Dget_space(dataset), H5Sclose);
		if (!space.valid()) return fail(HDF5_READ_STAGE_DATA_SPACE, -1);
		H5Eclear2(H5E_DEFAULT);
		ScopedH5Id type(H5Dget_type(dataset), H5Tclose);
		if (!type.valid()) return fail(HDF5_READ_STAGE_DATA_TYPE, -1);

		H5Eclear2(H5E_DEFAULT);
		const int rank = H5Sget_simple_extent_ndims(space);
		if (rank < 1 || rank > 2) return fail(HDF5_READ_STAGE_RANK, rank);
		H5Eclear2(H5E_DEFAULT);
		const int cvType = h5TypeToCvType(type);
		if (cvType < 0) return fail(HDF5_READ_STAGE_DATA_TYPE, cvType);
		describeHdf5Type(type, result.hdf5Type, sizeof(result.hdf5Type));

		hsize_t dimensions[2] = { 1, 1 };
		H5Eclear2(H5E_DEFAULT);
		if (H5Sget_simple_extent_dims(space, dimensions, nullptr) < 0 ||
			dimensions[0] > static_cast<hsize_t>(std::numeric_limits<int>::max()) ||
			(rank == 2 && dimensions[1] > static_cast<hsize_t>(std::numeric_limits<int>::max())))
			return fail(HDF5_READ_STAGE_DIMENSIONS, -1);
		result.rows = static_cast<int>(dimensions[0]);
		result.columns = rank == 1 ? 1 : static_cast<int>(dimensions[1]);
		result.cvType = cvType;
		try
		{
			output.create(result.rows, result.columns, cvType);
		}
		catch (const cv::Exception&)
		{
			result.stage = HDF5_READ_STAGE_OUTPUT_ALLOCATION;
			result.hdf5Status = -1;
			strcpy_s(result.errorStack, "OpenCV could not allocate the output matrix.");
			return -1;
		}

		H5Eclear2(H5E_DEFAULT);
		const herr_t readStatus = H5Dread(dataset, cvTypeToNativeH5Type(cvType), H5S_ALL, H5S_ALL,
			H5P_DEFAULT, output.data);
		if (readStatus < 0) return fail(HDF5_READ_STAGE_DATA_READ, static_cast<int>(readStatus));
		return 0;
	}

	ReadSession* openReadSession(const char* filename)
	{
		if (!filename) return nullptr;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return nullptr;
		ReadSession* session = new (std::nothrow) ReadSession();
		if (!session) return nullptr;
		if (session->file.open(filename, H5F_ACC_RDONLY, "read") != 0)
		{
			delete session;
			return nullptr;
		}
		return session;
	}

	void closeReadSession(ReadSession* session)
	{
		if (!session) return;
		ScopedHdf5Lock lock;
		if (lock.result() == 0) session->file.close();
		else session->file.abandonWithoutClose();
		delete session;
	}

	WriteSession* openWriteSession(const char* filename)
	{
		if (!filename) return nullptr;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return nullptr;
		WriteSession* session = new (std::nothrow) WriteSession();
		if (!session) return nullptr;
		if (session->file.open(filename, H5F_ACC_RDWR, "read-write") != 0)
		{
			delete session;
			return nullptr;
		}
		return session;
	}

	void closeWriteSession(WriteSession* session)
	{
		if (!session) return;
		ScopedHdf5Lock lock;
		if (lock.result() == 0) session->file.close();
		else session->file.abandonWithoutClose();
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
		ScopedAuditedH5File file;
		if (openWritableFile(filename, file) != 0) return -1;
		return writeArrayLocked(file, datasetName, input, false);
	}

	int writeArrayReplace(const char* filename, const char* datasetName, const cv::Mat& input)
	{
		if (!filename || !datasetName || input.empty() || input.channels() != 1) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
		if (openWritableFile(filename, file) != 0) return -1;
		return replaceScalarDataset(file, datasetName, H5T_NATIVE_DOUBLE, &value, true);
	}

	int writeInt(const char* filename, const char* datasetName, int value)
	{
		if (!filename || !datasetName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File file;
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
		ScopedAuditedH5File source;
		ScopedAuditedH5File destination;
		if (source.open(sourceFilename, H5F_ACC_RDONLY, "read") != 0 ||
			destination.open(destinationFilename, H5F_ACC_RDWR, "read-write") != 0) return -1;
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
		ScopedAuditedH5File source;
		ScopedAuditedH5File destination;
		if (source.open(sourceFilename, H5F_ACC_RDONLY, "read") != 0 ||
			destination.open(destinationFilename, H5F_ACC_RDWR, "read-write") != 0) return -1;
		for (int index = 0; index < datasetCount; ++index)
		{
			if (!datasetNames[index]) return kInvalidArgument;
			const std::string path = datasetPath(datasetNames[index]);
			if (path.empty()) return kInvalidArgument;
			if (H5Lexists(source, path.c_str(), H5P_DEFAULT) <= 0) continue;
			if (H5Lexists(destination, path.c_str(), H5P_DEFAULT) > 0)
			{
				if (!replaceExisting) continue;
				if (H5Ldelete(destination, path.c_str(), H5P_DEFAULT) < 0) return -1;
			}
			if (H5Ocopy(source, path.c_str(), destination, path.c_str(), H5P_DEFAULT, H5P_DEFAULT) < 0) return -1;
		}
		return 0;
	}

	int readString(const char* filename, const char* datasetName, std::string& value)
	{
		if (!filename || !datasetName) return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();
		ScopedAuditedH5File file;
		ScopedH5Id dataset;
		if (openDataset(filename, datasetName, file, dataset) != 0) return -1;
		ScopedH5Id type(H5Dget_type(dataset), H5Tclose);
		if (!type.valid() || H5Tget_class(type) != H5T_STRING || H5Tis_variable_str(type) > 0) return -1;
		const size_t length = H5Tget_size(type);
		if (length == 0 || length > 65536) return -1;
		std::vector<char> buffer(length + 1, '\0');
		if (H5Dread(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data()) < 0) return -1;
		const std::vector<char>::iterator terminator = std::find(buffer.begin(), buffer.begin() + length, '\0');
		if (terminator != buffer.begin() + length)
		{
			// A fixed-width HDF5 string may contain zero padding after its normal
			// terminator, but non-zero bytes after it are an embedded NUL.
			if (std::find_if(terminator + 1, buffer.begin() + length,
				[](char value) { return value != '\0'; }) != buffer.begin() + length) return -1;
			value.assign(buffer.data(), static_cast<size_t>(terminator - buffer.begin()));
		}
		else value.assign(buffer.data(), length);
		return 0;
	}

	int readStringDiagnosed(const char* filename, const char* datasetName, std::string& value,
		Hdf5ReadDiagnostic* diagnostic)
	{
		Hdf5ReadDiagnostic localDiagnostic = {};
		Hdf5ReadDiagnostic& result = diagnostic ? *diagnostic : localDiagnostic;
		result = {};
		if (!filename || !*filename || !datasetName || !*datasetName)
		{
			result.stage = HDF5_READ_STAGE_INVALID_ARGUMENT;
			result.hdf5Status = kInvalidArgument;
			strcpy_s(result.errorStack, "Filename and dataset name are required.");
			return kInvalidArgument;
		}
		ScopedHdf5Lock lock;
		if (lock.result() != 0)
		{
			result.stage = HDF5_READ_STAGE_OPEN_FILE;
			result.hdf5Status = lock.result();
			strcpy_s(result.errorStack, "Unable to acquire the HDF5 operation lock.");
			return lock.result();
		}
		ScopedHdf5AutoErrorSilencer errorSilencer;
		auto fail = [&](int stage, int status) {
			result.stage = stage;
			result.hdf5Status = status;
			captureHdf5ErrorStack(result);
			return status;
		};
		if (!isRegularFile(filename))
		{
			result.stage = HDF5_READ_STAGE_OPEN_FILE;
			result.hdf5Status = -1;
			strcpy_s(result.errorStack, "The HDF5 path does not identify an existing regular file.");
			return -1;
		}
		H5Eclear2(H5E_DEFAULT);
		ScopedAuditedH5File file;
		if (file.open(filename, H5F_ACC_RDONLY, "read") != 0) return fail(HDF5_READ_STAGE_OPEN_FILE, -1);
		const std::string path = datasetPath(datasetName);
		if (path.empty())
		{
			result.stage = HDF5_READ_STAGE_DATASET_PATH;
			result.hdf5Status = kInvalidArgument;
			strcpy_s(result.errorStack, "The HDF5 dataset path is invalid.");
			return kInvalidArgument;
		}
		H5Eclear2(H5E_DEFAULT);
		const htri_t exists = H5Lexists(file, path.c_str(), H5P_DEFAULT);
		if (exists < 0) return fail(HDF5_READ_STAGE_OPEN_DATASET, static_cast<int>(exists));
		if (exists == 0)
		{
			result.stage = HDF5_READ_STAGE_OPEN_DATASET;
			result.hdf5Status = 0;
			strcpy_s(result.errorStack, "The requested dataset does not exist.");
			return -1;
		}
		H5Eclear2(H5E_DEFAULT);
		ScopedH5Id dataset(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
		if (!dataset.valid()) return fail(HDF5_READ_STAGE_OPEN_DATASET, -1);
		H5Eclear2(H5E_DEFAULT);
		ScopedH5Id type(H5Dget_type(dataset), H5Tclose);
		if (!type.valid()) return fail(HDF5_READ_STAGE_DATA_TYPE, -1);
		H5Eclear2(H5E_DEFAULT);
		if (H5Tget_class(type) != H5T_STRING) return fail(HDF5_READ_STAGE_DATA_TYPE, -1);
		H5Eclear2(H5E_DEFAULT);
		if (H5Tis_variable_str(type) > 0)
		{
			result.stage = HDF5_READ_STAGE_DATA_TYPE;
			result.hdf5Status = -1;
			strcpy_s(result.errorStack, "Variable-length string datasets are unsupported.");
			return -1;
		}
		H5Eclear2(H5E_DEFAULT);
		const size_t length = H5Tget_size(type);
		if (length == 0 || length > 65536)
		{
			result.stage = HDF5_READ_STAGE_DATA_TYPE;
			result.hdf5Status = -1;
			strcpy_s(result.errorStack, "The fixed string length is invalid.");
			return -1;
		}
		strcpy_s(result.hdf5Type, "fixed-string");
		try
		{
			std::vector<char> buffer(length + 1, '\0');
			H5Eclear2(H5E_DEFAULT);
			const herr_t readStatus = H5Dread(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());
			if (readStatus < 0) return fail(HDF5_READ_STAGE_DATA_READ, static_cast<int>(readStatus));
			value.assign(buffer.data());
		}
		catch (const std::bad_alloc&)
		{
			result.stage = HDF5_READ_STAGE_OUTPUT_ALLOCATION;
			result.hdf5Status = -1;
			strcpy_s(result.errorStack, "Unable to allocate the string read buffer.");
			return -1;
		}
		return 0;
	}

	int readSubarray(const char* filename, const char* datasetName, int offsetRow, int offsetColumn,
		int rows, int columns, cv::Mat& output)
	{
		if (!filename || !datasetName || offsetRow < 0 || offsetColumn < 0 || rows < 1 || columns < 1)
			return -1;
		ScopedHdf5Lock lock;
		if (lock.result() != 0) return lock.result();

		ScopedAuditedH5File file;
		return file.open(filename, H5F_ACC_RDONLY, "read") == 0 ?
			readSubarrayLocked(file, datasetName, offsetRow, offsetColumn, rows, columns, output) : -1;
	}
}
