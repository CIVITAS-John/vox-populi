// Installed from simulator/schema/shared/VoxRlBlockStorage.cpp. Edit that source, then reinstall.

#include "VoxRlBlockStorage.h"

#ifdef VOX_RL_TESTING
namespace {

// Counts pending harness-requested allocation failures.
u32 ownedStorageAllocationFailures = 0U;

} // namespace
#endif

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
