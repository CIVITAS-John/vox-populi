// Installed from simulator/schema/shared/VoxRlBlockStorage.h. Edit that source, then reinstall.

#ifndef VOX_RL_BLOCK_STORAGE_H
#define VOX_RL_BLOCK_STORAGE_H

#include "VoxRlTypes.h"

#include <vector>

// Owns a mutable byte allocation used for copied fixture and pipe blocks.
class VoxRlOwnedBlockStorage {
public:
    // Creates empty owned storage.
    VoxRlOwnedBlockStorage();

    // Resizes the owned byte allocation and clears the new block deterministically.
    bool Allocate(u32 byteLength);

    // Clears the owned bytes.
    void Clear();

    // Exchanges owned bytes with another storage instance without copying either allocation.
    void Swap(VoxRlOwnedBlockStorage* other);

    // Returns writable owned bytes, or zero for an empty allocation.
    u8* MutableBytes();

    // Returns read-only owned bytes, or zero for an empty allocation.
    const u8* Bytes() const;

    // Returns the owned byte count.
    u32 ByteLength() const;

private:
    // Disallows accidental copies of storage that owns a mutable byte allocation.
    VoxRlOwnedBlockStorage(const VoxRlOwnedBlockStorage& other);

    // Disallows accidental assignment of storage that owns a mutable byte allocation.
    VoxRlOwnedBlockStorage& operator=(const VoxRlOwnedBlockStorage& other);

    std::vector<u8> bytes_;
};

#ifdef VOX_RL_TESTING
// Configures a finite number of owned-storage allocation failures for harness coverage.
void VoxRlFailOwnedStorageAllocationsForTesting(u32 count);
#endif

#endif
