// Installed from simulator/schema/shared/VoxRlBlockWriter.h. Edit that source, then reinstall.

#ifndef VOX_RL_BLOCK_WRITER_H
#define VOX_RL_BLOCK_WRITER_H

#include "VoxRlBlockView.h"

// Builds one deterministic portable block into caller-owned aligned storage.
class VoxRlBlockWriter {
public:
    // Wraps bounded caller-provided storage without taking ownership of it.
    VoxRlBlockWriter(void* bytes, u32 capacity);

    // Starts a block with a fixed directory size and its complete identity metadata.
    bool Begin(
        u16 blockKind,
        u16 sectionCount,
        u32 decisionId,
        u32 sessionLow,
        u32 sessionHigh,
        i32 turn,
        i32 player,
        u32 generation,
        u32 staticGeneration,
        u32 worldGeneration,
        u32 campaignGeneration,
        u32 deltaSequence,
        u8 flags = 0);

    // Reserves and clears the next canonical section before returning writable section bytes.
    bool BeginSection(u16 sectionKind, u32 count, u32 byteLength, void** sectionBytes);

    // Self-validates the constructed block and returns its exact framed byte length.
    bool Finish(u32* byteLength);

    // Abandons the current construction without changing caller-owned memory.
    void Reset();

    // Returns the failure status from the most recent writer operation.
    VoxRlBlockStatus Status() const;

private:
    // Records the writer failure status without exposing an incomplete block.
    bool Fail(VoxRlBlockStatus status);

    u8* bytes_;
    u32 capacity_;
    u32 nextOffset_;
    u16 blockKind_;
    u16 expectedSectionCount_;
    u16 writtenSectionCount_;
    u16 lastSectionOrder_;
    bool started_;
    bool finished_;
    VoxRlBlockStatus status_;
};

#endif
