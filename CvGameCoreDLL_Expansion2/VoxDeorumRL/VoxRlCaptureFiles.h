// Vox Deorum: recording capture file utilities. Win32-backed output files and
// path helpers for the recording tree described in plans/stage-1/8-capture.md.
#ifndef VOX_RL_CAPTURE_FILES_H
#define VOX_RL_CAPTURE_FILES_H

#include "VoxDeorumRL/schema/VoxRlFrame.h"

#include <string>

// Converts a UTF-8 path to a wide path for Win32 file APIs.
bool VoxRlUtf8ToWide(const char* utf8Path, std::wstring& widePath);

// Creates every missing directory along a UTF-8 path. Returns true when the
// full path exists on return.
bool VoxRlCreateDirectories(const char* utf8Path);

// Returns true when a regular file exists at the UTF-8 path.
bool VoxRlFileExists(const char* utf8Path);

// Removes the final path component of a folder path that ends in a separator
// or names one. The capture root derives Civ V's per-user game folder this way
// from the engine-provided cache folder path.
bool VoxRlStripLastPathComponent(const std::string& path, std::string& out);

// Formats the canonical UUID text of a game UUID, matching the recording
// reader's VoxRrFormatGameUuid exactly: 8-4-4-4-12 lowercase hex digits.
void VoxRlFormatGameUuidText(const VoxRlGameUuid& game, char out[37]);

// An append-oriented Win32 output file. Writes are unbuffered through the
// operating system; Flush pushes bytes to disk at publication markers only.
class VoxRlOutputFile
{
public:
	VoxRlOutputFile();
	~VoxRlOutputFile();

	// Creates or truncates the file and positions at offset zero.
	bool OpenNew(const char* utf8Path);
	// Opens or creates the file for appending and records its current size.
	bool OpenAppend(const char* utf8Path);
	// Appends bytes at the current end.
	bool Write(const void* bytes, unsigned int length);
	// Flushes the file to disk; the complete flushed commit line is the
	// logical publication marker for the recording index.
	bool Flush();
	void Close();

	bool IsOpen() const;
	// Total bytes this file has accepted since Open, plus any preexisting
	// appended prefix. Valid while open.
	unsigned __int64 ByteLength() const;

private:
	VoxRlOutputFile(const VoxRlOutputFile&);
	VoxRlOutputFile& operator=(const VoxRlOutputFile&);

	void* m_hFile;
	unsigned __int64 m_byteLength;
};

#endif
