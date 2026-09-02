// Installed from simulator/schema/shared/VoxRlFrame.h. Edit that source, then reinstall.

#ifndef VOX_RL_FRAME_H
#define VOX_RL_FRAME_H

#include "VoxRlTypes.h"

enum {
    kVoxRlFrameMagic = 0x52445856U,
    kVoxRlProtocolVersion = 0
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
    u32 sessionLow;
    u32 sessionHigh;
    i32 turn;
    i32 player;
    u32 generation;
    u32 staticGeneration;
    u32 worldGeneration;
    u32 campaignGeneration;
    u32 deltaSequence;
};
typedef char VoxRlAssert_ImageHeaderSize[(sizeof(VoxRlImageHeader) == 80) ? 1 : -1];

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
