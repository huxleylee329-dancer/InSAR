#include "stdafx.h"
#include "..\include\Utils.h"
#include "..\include\Hdf5IO.h"

#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")

namespace
{
	void setError(PathResolver::Error value, PathResolver::Error* output)
	{
		if (output) *output = value;
	}

	bool isDriveRelative(const std::wstring& path)
	{
		return path.size() >= 2 &&
			((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
			path[1] == L':' && (path.size() == 2 || (path[2] != L'\\' && path[2] != L'/'));
	}

	bool isRootRelative(const std::wstring& path)
	{
		// A leading single separator is rooted on the current drive, while a
		// double separator begins a UNC path and is handled as an absolute path.
		return path.size() >= 1 && (path[0] == L'\\' || path[0] == L'/') &&
			(path.size() == 1 || (path[1] != L'\\' && path[1] != L'/'));
	}

	bool normalizeAbsolute(const std::wstring& value, std::wstring& output)
	{
		// Extended-length paths are already unambiguous.  Keeping their prefix is
		// required for callers that intentionally exceed MAX_PATH.
		if (value.rfind(L"\\\\?\\", 0) == 0)
		{
			output = value;
			return true;
		}
		const DWORD length = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
		if (length == 0) return false;
		std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
		const DWORD written = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
		if (written == 0 || written >= buffer.size()) return false;
		output.assign(buffer.data(), written);
		return true;
	}

	std::wstring joinPath(const std::wstring& root, const std::wstring& relative)
	{
		if (root.empty() || root.back() == L'\\' || root.back() == L'/') return root + relative;
		return root + L"\\" + relative;
	}
}

namespace PathResolver
{
	const char* errorMessage(Error error)
	{
		switch (error)
		{
		case Error::None: return "success";
		case Error::InvalidUtf8: return "path is not valid UTF-8";
		case Error::EmbeddedNul: return "path contains an embedded NUL";
		case Error::DriveRelative: return "drive-relative paths such as C:foo are not supported";
		case Error::RootRelative: return "root-relative paths such as \\foo are not supported";
		case Error::InvalidProjectRoot: return "project root is invalid";
		case Error::Hdf5ReadFailure: return "source path datasets could not be read";
		case Error::MetadataMismatch: return "source path metadata is incomplete";
		case Error::UnsupportedMetadata: return "source path metadata is unsupported";
		default: return "unknown path error";
		}
	}

	bool utf8ToWide(const std::string& utf8, std::wstring& wide, Error* error)
	{
		wide.clear();
		if (utf8.find('\0') != std::string::npos)
		{
			setError(Error::EmbeddedNul, error);
			return false;
		}
		if (utf8.empty())
		{
			setError(Error::None, error);
			return true;
		}
		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
			static_cast<int>(utf8.size()), nullptr, 0);
		if (length <= 0)
		{
			setError(Error::InvalidUtf8, error);
			return false;
		}
		wide.resize(length);
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
			&wide[0], length) != length)
		{
			wide.clear();
			setError(Error::InvalidUtf8, error);
			return false;
		}
		setError(Error::None, error);
		return true;
	}

	bool wideToUtf8(const std::wstring& wide, std::string& utf8, Error* error)
	{
		utf8.clear();
		if (wide.find(L'\0') != std::wstring::npos)
		{
			setError(Error::EmbeddedNul, error);
			return false;
		}
		if (wide.empty())
		{
			setError(Error::None, error);
			return true;
		}
		const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
			static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		if (length <= 0)
		{
			setError(Error::InvalidUtf8, error);
			return false;
		}
		utf8.resize(length);
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
			&utf8[0], length, nullptr, nullptr) != length)
		{
			utf8.clear();
			setError(Error::InvalidUtf8, error);
			return false;
		}
		setError(Error::None, error);
		return true;
	}

	bool resolve(const std::string& utf8Path, const std::string& projectRoot, Resolution& result, Error* error)
	{
		result = {};
		std::wstring path;
		if (!utf8ToWide(utf8Path, path, error)) return false;
		if (isDriveRelative(path))
		{
			setError(Error::DriveRelative, error);
			return false;
		}
		if (isRootRelative(path))
		{
			setError(Error::RootRelative, error);
			return false;
		}

		std::wstring root;
		if (!utf8ToWide(projectRoot, root, error) || root.empty() || isDriveRelative(root) ||
			isRootRelative(root) || !normalizeAbsolute(root, root))
		{
			setError(Error::InvalidProjectRoot, error);
			return false;
		}

		result.projectRelative = PathIsRelativeW(path.c_str()) != FALSE;
		const std::wstring candidate = result.projectRelative ? joinPath(root, path) : path;
		if (!normalizeAbsolute(candidate, result.wide) || !wideToUtf8(result.wide, result.utf8, error)) return false;
		setError(Error::None, error);
		return true;
	}

	bool readSourcePathPair(const char* h5File, const char* projectRoot, SourcePathPair& result,
		Error* error, std::string* detail)
	{
		result = {};
		if (detail) detail->clear();
		if (!h5File || !projectRoot)
		{
			setError(Error::Hdf5ReadFailure, error);
			return false;
		}
		std::string source1;
		std::string source2;
		if (Hdf5IO::readString(h5File, "source_1", source1) != 0 ||
			Hdf5IO::readString(h5File, "source_2", source2) != 0)
		{
			setError(Error::Hdf5ReadFailure, error);
			if (detail) *detail = "source_1 and source_2 are required";
			return false;
		}

		std::string encoding;
		std::string version;
		const int encodingStatus = Hdf5IO::readString(h5File, "source_path_encoding", encoding);
		const int versionStatus = Hdf5IO::readString(h5File, "source_path_format_version", version);
		if (encodingStatus != 0 || versionStatus != 0)
		{
			setError(Error::MetadataMismatch, error);
			if (detail) *detail = "source_path_encoding and source_path_format_version are both required";
			return false;
		}
		if (encoding != "UTF-8" || version != "2")
		{
			setError(Error::UnsupportedMetadata, error);
			if (detail) *detail = "expected source_path_encoding=UTF-8 and source_path_format_version=2";
			return false;
		}

		if (!resolve(source1, projectRoot, result.source1, error) || !resolve(source2, projectRoot, result.source2, error))
		{
			if (detail) *detail = errorMessage(error ? *error : Error::InvalidUtf8);
			return false;
		}
		setError(Error::None, error);
		return true;
	}
}
