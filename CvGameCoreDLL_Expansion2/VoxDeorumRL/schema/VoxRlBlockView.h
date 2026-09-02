// Installed from simulator/schema/shared/VoxRlBlockView.h. Edit that source, then reinstall.

#ifndef VOX_RL_BLOCK_VIEW_H
#define VOX_RL_BLOCK_VIEW_H

#include "VoxRlFrame.h"
#include "VoxRlSchema.generated.h"

// Identifies the first validation failure encountered while opening a block.
enum VoxRlBlockStatus {
    VOX_RL_BLOCK_OK = 0,
    VOX_RL_BLOCK_NULL_BYTES,
    VOX_RL_BLOCK_UNALIGNED_BYTES,
    VOX_RL_BLOCK_TRUNCATED_FRAME,
    VOX_RL_BLOCK_BAD_MAGIC,
    VOX_RL_BLOCK_BAD_PROTOCOL_VERSION,
    VOX_RL_BLOCK_BAD_PAYLOAD_LENGTH,
    VOX_RL_BLOCK_TRUNCATED_IMAGE,
    VOX_RL_BLOCK_BAD_SCHEMA_VERSION,
    VOX_RL_BLOCK_BAD_MANIFEST_HASH,
    VOX_RL_BLOCK_BAD_KIND,
    VOX_RL_BLOCK_MESSAGE_KIND_MISMATCH,
    VOX_RL_BLOCK_BAD_IMAGE_LENGTH,
    VOX_RL_BLOCK_BAD_DIRECTORY,
    VOX_RL_BLOCK_UNKNOWN_SECTION,
    VOX_RL_BLOCK_SECTION_KIND_MISMATCH,
    VOX_RL_BLOCK_SECTION_ORDER,
    VOX_RL_BLOCK_SECTION_ALIGNMENT,
    VOX_RL_BLOCK_SECTION_LENGTH,
    VOX_RL_BLOCK_SECTION_COUNT,
    VOX_RL_BLOCK_DUPLICATE_SECTION,
    VOX_RL_BLOCK_REQUIRED_SECTION_MISSING,
    VOX_RL_BLOCK_BAD_PORTABLE_BOOL,
    VOX_RL_BLOCK_BAD_RANGE,
    VOX_RL_BLOCK_BAD_SESSION,
    VOX_RL_BLOCK_BAD_TURN,
    VOX_RL_BLOCK_BAD_PLAYER,
    VOX_RL_BLOCK_BAD_GENERATION,
    VOX_RL_BLOCK_BAD_STATIC_GENERATION,
    VOX_RL_BLOCK_BAD_WORLD_GENERATION,
    VOX_RL_BLOCK_BAD_CAMPAIGN_GENERATION,
    VOX_RL_BLOCK_BAD_DELTA_SEQUENCE,
    VOX_RL_BLOCK_EXPECTED_DECISION_ID_MISMATCH,
    VOX_RL_BLOCK_FRAME_RECORD_DECISION_ID_MISMATCH,
    VOX_RL_BLOCK_BAD_STABLE_ID,
    VOX_RL_BLOCK_BAD_LAYOUT,
    VOX_RL_BLOCK_STATUS_COUNT
};

// Selects the optional retained identity values a caller wants a view to enforce.
struct VoxRlBlockValidationOptions {
    bool checkSession;
    bool checkTurn;
    bool checkPlayer;
    bool checkGeneration;
    bool checkStaticGeneration;
    bool checkWorldGeneration;
    bool checkCampaignGeneration;
    bool checkDeltaSequence;
    bool checkDecisionId;
    bool checkPlotCount;
    bool checkAliveTeamCount;
    u32 sessionLow;
    u32 sessionHigh;
    i32 turn;
    i32 player;
    u32 generation;
    u32 staticGeneration;
    u32 worldGeneration;
    u32 campaignGeneration;
    u32 deltaSequence;
    u32 decisionId;
    u32 plotCount;
    u32 aliveTeamCount;
};

// Gives read-only access to one validated framed portable image.
class VoxRlBlockView {
public:
    // Creates an empty view that exposes no bytes until Open succeeds.
    VoxRlBlockView();

    // Validates the structure needed for safe access and retains a read-only borrowed pointer on success.
    bool Open(const void* bytes, u32 availableBytes, const VoxRlBlockValidationOptions* options = 0);

    // Adds the expensive semantic checks intended for tests and debug readers.
    bool OpenStrict(const void* bytes, u32 availableBytes, const VoxRlBlockValidationOptions* options = 0);

    // Clears the borrowed pointer after its owner releases the underlying storage.
    void Reset();

    // Returns the status from the most recent Open attempt.
    VoxRlBlockStatus Status() const;

    // Returns whether Open has completed successfully.
    bool IsValid() const;

    // Returns the complete framed bytes after validation.
    const u8* Bytes() const;

    // Returns the complete framed byte count after validation.
    u32 ByteLength() const;

    // Returns the validated transport header.
    const VoxRlFrameHeader* FrameHeader() const;

    // Returns the validated payload image header.
    const VoxRlImageHeader* ImageHeader() const;

    // Returns the validated directory entry for a known section, or zero when absent.
    const VoxRlSectionDirectoryEntry* FindSection(u16 sectionKind) const;

    // Returns read-only bytes for a validated section, or zero when the section is absent.
    const u8* SectionBytes(u16 sectionKind) const;

private:
    // Opens one block with the selected structural or strict validation level.
    bool OpenInternal(
        const void* bytes,
        u32 availableBytes,
        const VoxRlBlockValidationOptions* options,
        bool strictValidation);

    // Records the status and clears all externally visible state before Open returns false.
    bool Fail(VoxRlBlockStatus status);

    const u8* bytes_;
    u32 byteLength_;
    const VoxRlFrameHeader* frameHeader_;
    const VoxRlImageHeader* imageHeader_;
    const VoxRlSectionDirectoryEntry* directory_;
    VoxRlBlockStatus status_;
};

// Returns a stable short description for logging a validation status.
const char* VoxRlBlockStatusName(VoxRlBlockStatus status);

#endif
