// Installed from simulator/schema/shared/VoxRlFrame.h. Edit that source, then reinstall.

#ifndef VOX_RL_FRAME_H
#define VOX_RL_FRAME_H

#include "VoxRlTypes.h"

enum {
    kVoxRlFrameMagic = 0x52445856U,
    kVoxRlProtocolVersion = 0
};

// Canonical sentinel values for image identity fields an owning block kind does not carry.
enum {
    kVoxRlAbsentTurn = -1,        // STATIC carries no turn; WORLD establishes the active turn.
    kVoxRlAbsentPlayer = -1,      // STATIC carries no player; WORLD establishes the active player.
    kVoxRlAbsentPlotIndex = -1,   // Optional plot arguments use this sentinel.
    kVoxRlAbsentGeneration = 0,   // Generations start at one, so zero marks an absent generation.
    kVoxRlSyncOnlyDecisionId = 0  // Zero marks a synchronization-only request with no decision.
};

// Stores the 128-bit game UUID that identifies one recorded game, created by the existing
// MCP synchronization path and persisted in the save. The four words hold the canonical
// UUID text's 32 hex digits in order: words[0] is digits 0-7, words[1] is digits 8-15,
// words[2] is digits 16-23, and words[3] is digits 24-31. Each word's numeric value equals
// the big-endian reading of its four UUID bytes, and every word is serialized little-endian
// like the rest of the image. The UUID is never truncated or hashed into a second identity.
struct VoxRlGameUuid {
    u32 words[4];
};

// Stores the fixed 16-byte decision transport frame declared in specs.md.
struct VoxRlFrameHeader {
    u32 magic;
    u16 protocolVersion;
    u8 messageType;
    u8 flags;
    u32 decisionId;
    u32 payloadLength;
};
typedef char VoxRlAssert_FrameHeaderSize[(sizeof(VoxRlFrameHeader) == 16) ? 1 : -1];

// Stores one complete portable image after its transport frame.
struct VoxRlImageHeader {
    u32 schemaVersion;
    u32 manifestHash[8];
    u16 blockKind;
    u16 sectionCount;
    u32 imageSize;
    VoxRlGameUuid session;
    i32 turn;
    i32 player;
    u32 generation;
    u32 staticGeneration;
    u32 worldGeneration;
    u32 campaignGeneration;
    u32 deltaSequence;
};
typedef char VoxRlAssert_ImageHeaderSize[(sizeof(VoxRlImageHeader) == 88) ? 1 : -1];
typedef char VoxRlAssert_GameUuidSize[(sizeof(VoxRlGameUuid) == 16) ? 1 : -1];

// Stores the self-describing directory entry for one image section.
struct VoxRlSectionDirectoryEntry {
    u16 kind;
    u16 reserved;
    u32 offset;
    u32 count;
    u32 byteLength;
};
typedef char VoxRlAssert_SectionDirectoryEntrySize[(sizeof(VoxRlSectionDirectoryEntry) == 16) ? 1 : -1];

#endif
