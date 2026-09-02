// Installed from simulator/schema/shared/VoxRlBlockMetadata.h. Edit that source, then reinstall.

#ifndef VOX_RL_BLOCK_METADATA_H
#define VOX_RL_BLOCK_METADATA_H

#include "VoxRlFrame.h"
#include "VoxRlSchema.generated.h"

// Finds the generated metadata for one block kind.
static inline const VoxRlBlockDefinition* VoxRlFindBlockDefinition(u16 blockKind)
{
    for (u16 index = 0; index < kVoxRlBlockDefinitionCount; ++index)
    {
        if (kVoxRlBlockDefinitions[index].kind == blockKind)
        {
            return &kVoxRlBlockDefinitions[index];
        }
    }
    return 0;
}

// Finds the generated metadata for one section kind.
static inline const VoxRlSectionDefinition* VoxRlFindSectionDefinition(u16 sectionKind)
{
    for (u16 index = 0; index < kVoxRlSectionDefinitionCount; ++index)
    {
        if (kVoxRlSectionDefinitions[index].kind == sectionKind)
        {
            return &kVoxRlSectionDefinitions[index];
        }
    }
    return 0;
}

// Counts all generated sections that belong to a block kind.
static inline u16 VoxRlBlockSectionCount(u16 blockKind)
{
    u16 count = 0;
    for (u16 index = 0; index < kVoxRlSectionDefinitionCount; ++index)
    {
        if (kVoxRlSectionDefinitions[index].blockKind == blockKind)
        {
            ++count;
        }
    }
    return count;
}

// Counts required generated sections that belong to a block kind.
static inline u16 VoxRlRequiredBlockSectionCount(u16 blockKind)
{
    u16 count = 0;
    for (u16 index = 0; index < kVoxRlSectionDefinitionCount; ++index)
    {
        if (kVoxRlSectionDefinitions[index].blockKind == blockKind && kVoxRlSectionDefinitions[index].required != 0)
        {
            ++count;
        }
    }
    return count;
}

// Returns whether the block is transported through a pipe payload.
static inline bool VoxRlBlockUsesPipeTransport(const VoxRlBlockDefinition& definition)
{
    return definition.transport == VOX_RL_BLOCK_TRANSPORT_PIPE;
}

#endif
