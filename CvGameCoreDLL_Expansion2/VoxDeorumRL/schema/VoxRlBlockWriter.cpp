// Installed from simulator/schema/shared/VoxRlBlockWriter.cpp. Edit that source, then reinstall.

#include "VoxRlBlockWriter.h"
#include "VoxRlBlockMetadata.h"

// Checks writer-provided counts that are fully defined by the schema metadata.
static bool VoxRlWriterHasValidSectionCount(const VoxRlSectionDefinition& definition, u32 count, u32 byteLength)
{
    if (definition.countRule == VOX_RL_SECTION_COUNT_SINGLE && count != 1)
    {
        return false;
    }
    if (definition.countRule == VOX_RL_SECTION_COUNT_BYTES && count != byteLength)
    {
        return false;
    }
    return true;
}

// Wraps caller-owned memory and records invalid input before Begin is called.
VoxRlBlockWriter::VoxRlBlockWriter(void* bytes, u32 capacity)
    : bytes_(static_cast<u8*>(bytes)), capacity_(capacity), nextOffset_(0), blockKind_(0), expectedSectionCount_(0),
      writtenSectionCount_(0), lastSectionOrder_(0), started_(false), finished_(false), status_(VOX_RL_BLOCK_OK)
{
    if (bytes_ == 0)
    {
        status_ = VOX_RL_BLOCK_NULL_BYTES;
    }
    else if ((reinterpret_cast<size_t>(bytes_) & (kVoxRlAlignment - 1)) != 0)
    {
        status_ = VOX_RL_BLOCK_UNALIGNED_BYTES;
    }
}

// Records a construction failure and prevents an incomplete image from being finished.
bool VoxRlBlockWriter::Fail(VoxRlBlockStatus status)
{
    status_ = status;
    started_ = false;
    finished_ = false;
    return false;
}

// Starts deterministic block construction after reserving its complete directory.
bool VoxRlBlockWriter::Begin(
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
    u8 flags)
{
    if (bytes_ == 0)
    {
        return Fail(VOX_RL_BLOCK_NULL_BYTES);
    }
    if ((reinterpret_cast<size_t>(bytes_) & (kVoxRlAlignment - 1)) != 0)
    {
        return Fail(VOX_RL_BLOCK_UNALIGNED_BYTES);
    }
    const VoxRlBlockDefinition* blockDefinition = VoxRlFindBlockDefinition(blockKind);
    if (blockDefinition == 0)
    {
        return Fail(VOX_RL_BLOCK_BAD_KIND);
    }
    const u16 maximumSectionCount = VoxRlBlockSectionCount(blockKind);
    if (sectionCount < VoxRlRequiredBlockSectionCount(blockKind) || sectionCount > maximumSectionCount)
    {
        return Fail(VOX_RL_BLOCK_BAD_DIRECTORY);
    }
    u32 directoryLength = 0;
    u32 imagePrefixLength = 0;
    u32 firstSectionOffset = 0;
    if (!VoxRlMultiplyU32(sectionCount, sizeof(VoxRlSectionDirectoryEntry), directoryLength) ||
        !VoxRlAddU32(sizeof(VoxRlImageHeader), directoryLength, imagePrefixLength) ||
        !VoxRlAlignU32(imagePrefixLength, firstSectionOffset))
    {
        return Fail(VOX_RL_BLOCK_BAD_DIRECTORY);
    }
    u32 requiredCapacity = 0;
    if (!VoxRlAddU32(sizeof(VoxRlFrameHeader), firstSectionOffset, requiredCapacity) || requiredCapacity > capacity_)
    {
        return Fail(VOX_RL_BLOCK_TRUNCATED_IMAGE);
    }
    VoxRlZeroBytes(bytes_, requiredCapacity);
    VoxRlFrameHeader* frame = reinterpret_cast<VoxRlFrameHeader*>(bytes_);
    frame->magic = kVoxRlFrameMagic;
    frame->protocolVersion = kVoxRlProtocolVersion;
    frame->messageType = static_cast<u8>(blockKind);
    frame->flags = flags;
    frame->decisionId = decisionId;
    frame->payloadLength = 0;
    VoxRlImageHeader* image = reinterpret_cast<VoxRlImageHeader*>(bytes_ + sizeof(VoxRlFrameHeader));
    image->schemaVersion = VoxRlSchemaVersion;
    for (u16 index = 0; index < 8; ++index)
    {
        image->manifestHash[index] = kVoxRlManifestHashWords[index];
    }
    image->blockKind = blockKind;
    image->sectionCount = sectionCount;
    image->imageSize = 0;
    image->sessionLow = sessionLow;
    image->sessionHigh = sessionHigh;
    image->turn = turn;
    image->player = player;
    image->generation = generation;
    image->staticGeneration = staticGeneration;
    image->worldGeneration = worldGeneration;
    image->campaignGeneration = campaignGeneration;
    image->deltaSequence = deltaSequence;
    nextOffset_ = firstSectionOffset;
    blockKind_ = blockKind;
    expectedSectionCount_ = sectionCount;
    writtenSectionCount_ = 0;
    lastSectionOrder_ = 0;
    started_ = true;
    finished_ = false;
    status_ = VOX_RL_BLOCK_OK;
    return true;
}

// Reserves one complete canonical section and clears its bytes before caller writes records.
bool VoxRlBlockWriter::BeginSection(u16 sectionKind, u32 count, u32 byteLength, void** sectionBytes)
{
    if (sectionBytes == 0 || !started_ || finished_ || writtenSectionCount_ >= expectedSectionCount_)
    {
        return Fail(VOX_RL_BLOCK_BAD_DIRECTORY);
    }
    const VoxRlSectionDefinition* definition = VoxRlFindSectionDefinition(sectionKind);
    if (definition == 0)
    {
        return Fail(VOX_RL_BLOCK_UNKNOWN_SECTION);
    }
    if (definition->blockKind != blockKind_)
    {
        return Fail(VOX_RL_BLOCK_SECTION_KIND_MISMATCH);
    }
    if (definition->canonicalOrder <= lastSectionOrder_)
    {
        return Fail(VOX_RL_BLOCK_SECTION_ORDER);
    }
    if (!VoxRlWriterHasValidSectionCount(*definition, count, byteLength))
    {
        return Fail(VOX_RL_BLOCK_SECTION_COUNT);
    }
    if (definition->recordSize != 0)
    {
        u32 expectedLength = 0;
        if (!VoxRlMultiplyU32(count, definition->recordSize, expectedLength) || expectedLength != byteLength)
        {
            return Fail(VOX_RL_BLOCK_SECTION_LENGTH);
        }
    }
    u32 sectionOffset = 0;
    u32 sectionEnd = 0;
    const VoxRlBlockDefinition* blockDefinition = VoxRlFindBlockDefinition(blockKind_);
    if (blockDefinition == 0 || !VoxRlAlignU32(nextOffset_, sectionOffset) ||
        !VoxRlAddU32(sectionOffset, byteLength, sectionEnd))
    {
        return Fail(VOX_RL_BLOCK_SECTION_LENGTH);
    }
    if (sectionEnd > blockDefinition->maximumPayloadBytes)
    {
        return Fail(VoxRlBlockUsesPipeTransport(*blockDefinition) ? VOX_RL_BLOCK_BAD_PAYLOAD_LENGTH : VOX_RL_BLOCK_BAD_IMAGE_LENGTH);
    }
    u32 blockEnd = 0;
    if (!VoxRlAddU32(sizeof(VoxRlFrameHeader), sectionEnd, blockEnd) || blockEnd > capacity_)
    {
        return Fail(VOX_RL_BLOCK_TRUNCATED_IMAGE);
    }
    VoxRlZeroBytes(bytes_ + sizeof(VoxRlFrameHeader) + nextOffset_, sectionOffset - nextOffset_);
    VoxRlZeroBytes(bytes_ + sizeof(VoxRlFrameHeader) + sectionOffset, byteLength);
    VoxRlSectionDirectoryEntry* directory = reinterpret_cast<VoxRlSectionDirectoryEntry*>(
        bytes_ + sizeof(VoxRlFrameHeader) + sizeof(VoxRlImageHeader));
    VoxRlSectionDirectoryEntry& entry = directory[writtenSectionCount_];
    entry.kind = sectionKind;
    entry.reserved = 0;
    entry.offset = sectionOffset;
    entry.count = count;
    entry.byteLength = byteLength;
    *sectionBytes = bytes_ + sizeof(VoxRlFrameHeader) + sectionOffset;
    nextOffset_ = sectionEnd;
    lastSectionOrder_ = definition->canonicalOrder;
    ++writtenSectionCount_;
    return true;
}

// Validates and publishes the exact constructed byte length to the caller.
bool VoxRlBlockWriter::Finish(u32* byteLength)
{
    if (byteLength == 0 || !started_ || finished_ || writtenSectionCount_ != expectedSectionCount_)
    {
        return Fail(VOX_RL_BLOCK_BAD_DIRECTORY);
    }
    const VoxRlBlockDefinition* blockDefinition = VoxRlFindBlockDefinition(blockKind_);
    if (blockDefinition == 0)
    {
        return Fail(VOX_RL_BLOCK_BAD_KIND);
    }
    if (nextOffset_ > blockDefinition->maximumPayloadBytes)
    {
        return Fail(VoxRlBlockUsesPipeTransport(*blockDefinition) ? VOX_RL_BLOCK_BAD_PAYLOAD_LENGTH : VOX_RL_BLOCK_BAD_IMAGE_LENGTH);
    }
    VoxRlFrameHeader* frame = reinterpret_cast<VoxRlFrameHeader*>(bytes_);
    VoxRlImageHeader* image = reinterpret_cast<VoxRlImageHeader*>(bytes_ + sizeof(VoxRlFrameHeader));
    frame->payloadLength = nextOffset_;
    image->imageSize = nextOffset_;
    u32 completeLength = 0;
    if (!VoxRlAddU32(sizeof(VoxRlFrameHeader), nextOffset_, completeLength))
    {
        return Fail(VOX_RL_BLOCK_BAD_IMAGE_LENGTH);
    }
    VoxRlBlockView view;
    if (!view.Open(bytes_, completeLength))
    {
        return Fail(view.Status());
    }
    *byteLength = completeLength;
    finished_ = true;
    status_ = VOX_RL_BLOCK_OK;
    return true;
}

// Clears construction state while leaving caller-owned memory untouched.
void VoxRlBlockWriter::Reset()
{
    nextOffset_ = 0;
    blockKind_ = 0;
    expectedSectionCount_ = 0;
    writtenSectionCount_ = 0;
    lastSectionOrder_ = 0;
    started_ = false;
    finished_ = false;
    status_ = VOX_RL_BLOCK_OK;
}

// Returns the status from the latest writer operation.
VoxRlBlockStatus VoxRlBlockWriter::Status() const
{
    return status_;
}
