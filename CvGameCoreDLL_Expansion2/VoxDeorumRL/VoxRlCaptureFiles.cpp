// Installed from simulator/capture/VoxRlCaptureFiles.cpp. Edit that source, then reinstall.

// Vox Deorum: recording capture file utilities. See VoxRlCaptureFiles.h.
#include "CvGameCoreDLLPCH.h"
#include "VoxDeorumRL/VoxRlCaptureFiles.h"

#include <windows.h>

namespace
{
	// Creates the directory chain holding one file. Output opens go through
	// this so directories appear only when a file is about to be written and
	// a never-publishing capture leaves no empty folders behind.
	bool CreateDirectoriesForFile(const char* utf8Path)
	{
		std::string parent;
		if (!VoxRlStripLastPathComponent(utf8Path, parent))
		{
			// A bare file name has no parent directory to create.
			return true;
		}
		return VoxRlCreateDirectories(parent.c_str());
	}
}

bool VoxRlUtf8ToWide(const char* utf8Path, std::wstring& widePath)
{
	if (utf8Path == NULL || utf8Path[0] == '\0')
	{
		return false;
	}
	int needed = MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, NULL, 0);
	if (needed <= 0)
	{
		return false;
	}
	widePath.resize(static_cast<size_t>(needed));
	int written = MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, &widePath[0], needed);
	if (written != needed)
	{
		return false;
	}
	// MultiByteToWideChar writes the terminating zero into the string.
	widePath.resize(static_cast<size_t>(needed - 1));
	return true;
}

bool VoxRlCreateDirectories(const char* utf8Path)
{
	std::wstring widePath;
	if (!VoxRlUtf8ToWide(utf8Path, widePath))
	{
		return false;
	}
	if (widePath.size() == 0)
	{
		return false;
	}
	if (GetFileAttributesW(widePath.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		return true;
	}
	// Walk the path and create each missing directory from the root down.
	for (size_t position = 0; position <= widePath.size(); ++position)
	{
		const bool atSeparator = position < widePath.size() && (widePath[position] == L'\\' || widePath[position] == L'/');
		const bool atEnd = position == widePath.size();
		if (!atSeparator && !atEnd)
		{
			continue;
		}
		// Skip drive roots and leading separators.
		if (position == 0)
		{
			continue;
		}
		if (widePath[position - 1] == L':' || widePath[position - 1] == L'\\' || widePath[position - 1] == L'/')
		{
			continue;
		}
		std::wstring prefix = widePath.substr(0, position);
		if (GetFileAttributesW(prefix.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			continue;
		}
		if (!CreateDirectoryW(prefix.c_str(), NULL))
		{
			const DWORD error = GetLastError();
			if (error != ERROR_ALREADY_EXISTS && GetFileAttributesW(prefix.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				return false;
			}
		}
	}
	return GetFileAttributesW(widePath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool VoxRlFileExists(const char* utf8Path)
{
	std::wstring widePath;
	if (!VoxRlUtf8ToWide(utf8Path, widePath))
	{
		return false;
	}
	const DWORD attributes = GetFileAttributesW(widePath.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// The engine only exposes the per-user cache folder, which always sits inside
// Civ V's per-user game folder. Stripping the final component of the
// engine-provided cache path yields that folder without resolving Documents
// independently in the DLL.
bool VoxRlStripLastPathComponent(const std::string& path, std::string& out)
{
	size_t end = path.size();
	while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/'))
	{
		--end;
	}
	size_t separator = end;
	while (separator > 0 && path[separator - 1] != '\\' && path[separator - 1] != '/')
	{
		--separator;
	}
	if (separator == 0 || separator >= end)
	{
		return false;
	}
	out.assign(path, 0, separator - 1);
	return out.size() > 0;
}

void VoxRlFormatGameUuidText(const VoxRlGameUuid& game, char out[37])
{
	sprintf_s(out, 37, "%08x-%04x-%04x-%04x-%04x%08x",
		static_cast<unsigned int>(game.words[0]),
		static_cast<unsigned int>((game.words[1] >> 16) & 0xFFFFU),
		static_cast<unsigned int>(game.words[1] & 0xFFFFU),
		static_cast<unsigned int>((game.words[2] >> 16) & 0xFFFFU),
		static_cast<unsigned int>(game.words[2] & 0xFFFFU),
		static_cast<unsigned int>(game.words[3]));
}

VoxRlOutputFile::VoxRlOutputFile()
	: m_hFile(INVALID_HANDLE_VALUE),
	m_byteLength(0)
{
}

VoxRlOutputFile::~VoxRlOutputFile()
{
	Close();
}

bool VoxRlOutputFile::OpenNew(const char* utf8Path)
{
	Close();
	if (!CreateDirectoriesForFile(utf8Path))
	{
		return false;
	}
	std::wstring widePath;
	if (!VoxRlUtf8ToWide(utf8Path, widePath))
	{
		return false;
	}
	HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	m_hFile = handle;
	m_byteLength = 0;
	return true;
}

bool VoxRlOutputFile::OpenAppend(const char* utf8Path)
{
	Close();
	if (!CreateDirectoriesForFile(utf8Path))
	{
		return false;
	}
	std::wstring widePath;
	if (!VoxRlUtf8ToWide(utf8Path, widePath))
	{
		return false;
	}
	HANDLE handle = CreateFileW(widePath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ, NULL,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	LARGE_INTEGER size;
	size.QuadPart = 0;
	if (!GetFileSizeEx(handle, &size))
	{
		CloseHandle(handle);
		return false;
	}
	m_hFile = handle;
	m_byteLength = static_cast<unsigned __int64>(size.QuadPart);
	return true;
}

bool VoxRlOutputFile::Write(const void* bytes, unsigned int length)
{
	if (m_hFile == INVALID_HANDLE_VALUE || (bytes == NULL && length != 0))
	{
		return false;
	}
	if (length == 0)
	{
		return true;
	}
	const unsigned char* cursor = static_cast<const unsigned char*>(bytes);
	while (length != 0)
	{
		const DWORD chunk = static_cast<DWORD>(length > 0x40000000U ? 0x40000000U : length);
		DWORD written = 0;
		if (!WriteFile(m_hFile, cursor, chunk, &written, NULL) || written != chunk)
		{
			return false;
		}
		cursor += chunk;
		m_byteLength += static_cast<unsigned __int64>(written);
		length -= static_cast<unsigned int>(written);
	}
	return true;
}

bool VoxRlOutputFile::Flush()
{
	if (m_hFile == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	return FlushFileBuffers(m_hFile) != FALSE;
}

void VoxRlOutputFile::Close()
{
	if (m_hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_hFile);
		m_hFile = INVALID_HANDLE_VALUE;
	}
	m_byteLength = 0;
}

bool VoxRlOutputFile::IsOpen() const
{
	return m_hFile != INVALID_HANDLE_VALUE;
}

unsigned __int64 VoxRlOutputFile::ByteLength() const
{
	return m_byteLength;
}
