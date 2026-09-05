// Installed from simulator/schema/shared/VoxRlBlockStorage.cpp. Edit that source, then reinstall.

#include <windows.h>

#include "VoxRlBlockStorage.h"

#ifdef VOX_RL_TESTING
namespace {

// Counts pending harness-requested allocation failures.
u32 ownedStorageAllocationFailures = 0U;

} // namespace
#endif

// Creates empty borrowed storage.
VoxRlBorrowedBlockStorage::VoxRlBorrowedBlockStorage() : bytes_(0), byteLength_(0)
{
}

// Borrows aligned bytes without changing their ownership or lifetime.
bool VoxRlBorrowedBlockStorage::Reset(const void* bytes, u32 byteLength)
{
    if (bytes == 0 || byteLength == 0 || (reinterpret_cast<size_t>(bytes) & (kVoxRlAlignment - 1)) != 0)
    {
        Clear();
        return false;
    }
    bytes_ = static_cast<const u8*>(bytes);
    byteLength_ = byteLength;
    return true;
}

// Clears borrowed storage before its external owner goes away.
void VoxRlBorrowedBlockStorage::Clear()
{
    bytes_ = 0;
    byteLength_ = 0;
}

// Returns the borrowed byte pointer.
const u8* VoxRlBorrowedBlockStorage::Bytes() const
{
    return bytes_;
}

// Returns the borrowed byte count.
u32 VoxRlBorrowedBlockStorage::ByteLength() const
{
    return byteLength_;
}

// Creates empty owned storage.
VoxRlOwnedBlockStorage::VoxRlOwnedBlockStorage()
{
}

// Allocates deterministically cleared owned storage.
bool VoxRlOwnedBlockStorage::Allocate(u32 byteLength)
{
#ifdef VOX_RL_TESTING
    if (ownedStorageAllocationFailures != 0U)
    {
        --ownedStorageAllocationFailures;
        return false;
    }
#endif
    try
    {
        bytes_.assign(byteLength, 0);
    }
    catch (...)
    {
        bytes_.clear();
        return false;
    }
    return true;
}

// Releases all owned bytes.
void VoxRlOwnedBlockStorage::Clear()
{
    bytes_.clear();
}

// Exchanges owned bytes with another storage instance without copying either allocation.
void VoxRlOwnedBlockStorage::Swap(VoxRlOwnedBlockStorage* other)
{
    if (other != 0)
    {
        bytes_.swap(other->bytes_);
    }
}

#ifdef VOX_RL_TESTING
// Configures a finite number of owned-storage allocation failures for harness coverage.
void VoxRlFailOwnedStorageAllocationsForTesting(u32 count)
{
    ownedStorageAllocationFailures = count;
}
#endif

// Returns writable owned storage when it is non-empty.
u8* VoxRlOwnedBlockStorage::MutableBytes()
{
    return bytes_.empty() ? 0 : &bytes_[0];
}

// Returns read-only owned storage when it is non-empty.
const u8* VoxRlOwnedBlockStorage::Bytes() const
{
    return bytes_.empty() ? 0 : &bytes_[0];
}

// Returns the exact owned byte length after guarding the vector conversion.
u32 VoxRlOwnedBlockStorage::ByteLength() const
{
    return bytes_.size() > 0xffffffffU ? 0 : static_cast<u32>(bytes_.size());
}

// Creates an unopened fixture-file mapping.
VoxRlFixtureFileMapping::VoxRlFixtureFileMapping()
    : fileHandle_(0), mappingHandle_(0), bytes_(0), byteLength_(0)
{
}

// Closes a fixture mapping before its object is destroyed.
VoxRlFixtureFileMapping::~VoxRlFixtureFileMapping()
{
    Close();
}

// Opens a complete fixture file as a read-only mapping without copying its payload.
bool VoxRlFixtureFileMapping::Open(const wchar_t* path)
{
    Close();
    if (path == 0 || *path == 0)
    {
        return false;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD high = 0;
    const DWORD low = GetFileSize(file, &high);
    if ((low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) || high != 0 || low == 0)
    {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, 0, PAGE_READONLY, 0, 0, 0);
    if (mapping == 0)
    {
        CloseHandle(file);
        return false;
    }
    const void* bytes = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (bytes == 0)
    {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    fileHandle_ = file;
    mappingHandle_ = mapping;
    bytes_ = static_cast<const u8*>(bytes);
    byteLength_ = static_cast<u32>(low);
    return true;
}

// Closes the mapped view before its backing handles.
void VoxRlFixtureFileMapping::Close()
{
    if (bytes_ != 0)
    {
        UnmapViewOfFile(bytes_);
    }
    if (mappingHandle_ != 0)
    {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
    }
    if (fileHandle_ != 0)
    {
        CloseHandle(static_cast<HANDLE>(fileHandle_));
    }
    fileHandle_ = 0;
    mappingHandle_ = 0;
    bytes_ = 0;
    byteLength_ = 0;
}

// Returns the read-only mapped fixture bytes.
const u8* VoxRlFixtureFileMapping::Bytes() const
{
    return bytes_;
}

// Returns the exact mapped fixture byte count.
u32 VoxRlFixtureFileMapping::ByteLength() const
{
    return byteLength_;
}
