// Installed from simulator/capture/VoxRlCapture.h. Edit that source, then reinstall.

// Vox Deorum: recording capture manager. Owns capture lifecycle, segment
// state, dirty sets, and publication for the recording tree described in
// plans/stage-1/8-capture.md. Every entry point is called only after the
// caller has checked MOD_IPC_CHANNEL and gVoxRlCaptureEnabled. Hooks skip
// recording when no segment is armed.
#ifndef VOX_RL_CAPTURE_H
#define VOX_RL_CAPTURE_H

#include "VoxDeorumRL/schema/VoxRlFrame.h"
#include "VoxDeorumRL/schema/VoxRlBlockStorage.h"
#include "VoxDeorumRL/VoxRlCaptureFiles.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class CvCity;
class CvDangerPlots;
class CvPlot;
class CvTeam;
class CvUnit;
struct VoxRlRequestData;
struct STacticalAssignment;

// Cached at DLL load so disabled capture skips hooks before accessing the manager.
extern const bool gVoxRlCaptureEnabled;

// Request caller types recorded in request headers and result metadata.
enum
{
	VOX_RL_CALLER_FULL = 0,
	VOX_RL_CALLER_OPPORTUNITY = 1
};

// Retry outcomes describing what the previous attempt of the same engagement
// produced. First attempts carry VOX_RL_RETRY_FIRST.
enum
{
	VOX_RL_RETRY_FIRST = 0,
	VOX_RL_RETRY_PREVIOUS_EMPTY = 1,
	VOX_RL_RETRY_PREVIOUS_EXECUTION_FAILED = 2
};

// Visibility bitset kinds for flip records.
enum
{
	VOX_RL_BITSET_REVEALED = 0,
	VOX_RL_BITSET_VISIBLE = 1,
	VOX_RL_BITSET_KNOWN_VISIBLE = 2,
	VOX_RL_BITSET_INVISIBLE_VISIBLE = 3
};

// One owner-qualified board identity used by the dirty sets.
struct VoxRlEntityKey
{
	int owner;
	int id;
	VoxRlEntityKey() : owner(-1), id(-1) {}
	VoxRlEntityKey(int iOwner, int iId) : owner(iOwner), id(iId) {}
	bool operator<(const VoxRlEntityKey& other) const
	{
		if (owner != other.owner) return owner < other.owner;
		return id < other.id;
	}
};

// One visibility flip candidate. The dirty set keeps the latest value for
// each team, plot, and bitset kind.
struct VoxRlVisibilityKey
{
	int team;
	int plotIndex;
	int kind;
	VoxRlVisibilityKey() : team(-1), plotIndex(-1), kind(-1) {}
	VoxRlVisibilityKey(int iTeam, int iPlot, int iKind) : team(iTeam), plotIndex(iPlot), kind(iKind) {}
	bool operator<(const VoxRlVisibilityKey& other) const
	{
		if (team != other.team) return team < other.team;
		if (plotIndex != other.plotIndex) return plotIndex < other.plotIndex;
		return kind < other.kind;
	}
};

// One grouped visibility word: the team, kind, and thirty-two-plot word position.
struct VoxRlVisibilityWordKey
{
	int team;
	int kind;
	int wordIndex;
	VoxRlVisibilityWordKey() : team(-1), kind(-1), wordIndex(-1) {}
	VoxRlVisibilityWordKey(int iTeam, int iKind, int iWord) : team(iTeam), kind(iKind), wordIndex(iWord) {}
	bool operator<(const VoxRlVisibilityWordKey& other) const
	{
		if (team != other.team) return team < other.team;
		if (kind != other.kind) return kind < other.kind;
		return wordIndex < other.wordIndex;
	}
};

// One grouped visibility word's accumulated payload: the changed bits and their values.
struct VoxRlVisibilityWordValue
{
	unsigned int mask;
	unsigned int bits;
	VoxRlVisibilityWordValue() : mask(0), bits(0) {}
};

// One published or pending frame in a segment stream.
struct VoxRlFrameEntry
{
	unsigned __int64 offset;
	unsigned int length;
	int blockKind;
	unsigned int requestSequence;
	unsigned int decisionId;
	int attemptIndex;
	VoxRlFrameEntry() : offset(0), length(0), blockKind(0), requestSequence(0), decisionId(0), attemptIndex(-1) {}
};

// Filters and paths resolve once per enabled VD game attachment. The enable
// flag is cached at DLL load and defaults to off.
struct VoxRlCaptureConfig
{
	bool enabled;
	std::string rootOverride;    // VOX_RL_CAPTURE_ROOT
	bool filterPlayers;
	std::set<int> players;       // VOX_RL_CAPTURE_PLAYERS
	bool filterTurns;
	bool timings;                // VOX_RL_CAPTURE_TIMINGS
	VoxRlCaptureConfig();
};

// The recording capture manager. One instance per process; all work happens
// on the game core thread.
class VoxRlCapture
{
public:
	static VoxRlCapture& GetInstance();

	// Identity discovery uses the existing save-data table through the
	// connection service's Lua state. Returns false while the game UUID has
	// not been established yet.
	bool DiscoverGameUuid();

	// Lifecycle hooks.
	void OnGameStartOrLoad();
	void Shutdown();
	// Finalizes the recording at the victory announcement: commits the
	// open segment, releases the output handles, and ignores capture
	// hooks until a new game starts. The victory archive can then move
	// the recording directory.
	void OnGameConcluded();

	// Checkpoint hooks.
	void OnPreDangerCheckpoint(PlayerTypes ePlayer);
	void OnCampaignSeam(PlayerTypes ePlayer);

	// Engagement and attempt hooks around the native search entry.
	void BeginEngagement(int callerType, PlayerTypes ePlayer);
	void EndEngagement();
	void NoteExecutionResult(bool bSuccess);
	// Wraps one native FindBestUnitAssignments call: builds the decision
	// request with pending changes, invokes the native search once,
	// and snapshots the result. The native search itself is unchanged.
	std::vector<STacticalAssignment> SearchAssignments(int callerType, PlayerTypes ePlayer,
		const std::vector<CvUnit*>& vUnits, CvPlot* pTarget, int eAggression,
		std::set<int>& unuseableUnits, bool bTargetDistanceRelevant,
		bool bReturnToStartPositions, int iSaveMovement);

	// Danger lifecycle hooks.
	void OnDangerRefreshBegin(const CvDangerPlots& danger);
	void OnDangerRefreshComplete(const CvDangerPlots& danger);
	void OnDangerDiscoveryBegin(const CvDangerPlots& danger, const CvUnit* pUnit);
	void OnDangerDiscoveryEnd(const CvDangerPlots& danger);
	void NoteDangerDirty(PlayerTypes eObserver);

	// Dirty notifications from audited setters. They only mark keys; safe
	// boundaries collect complete records after the mutation finishes.
	void NoteUnitChanged(PlayerTypes eOwner, int iUnitId);
	void NoteUnitPromotionsChanged(PlayerTypes eOwner, int iUnitId);
	void NoteUnitCreated(PlayerTypes eOwner, int iUnitId);
	void NoteUnitRemoved(PlayerTypes eOwner, int iUnitId);
	void NoteCityChanged(PlayerTypes eOwner, int iCityId);
	void NoteCityCreated(PlayerTypes eOwner, int iCityId);
	void NoteCityRemoved(PlayerTypes eOwner, int iCityId);
	void NotePlotChanged(int iPlotIndex);
	void NoteVisibilityChanged(TeamTypes eTeam, int iPlotIndex, int iBitsetKind, bool bValue);
	// Records one team's known-visibility reset: the reader clears that team's
	// complete known-visible bitset before the same request's ordinary flips.
	void NoteKnownVisibilityReset(TeamTypes eTeam);
	// Marks the captured zone snapshot dirty after a native dominance zone
	// rebuild; the next collection compares and re-accepts the table.
	void NoteTacticalZonesRebuilt();
	// Marks the captured team passability table dirty after a technology
	// ownership change; the next collection recollects the small team table.
	void NoteTeamTechsChanged();
	void NoteRevealedOverrideChanged(TeamTypes eTeam, int iPlotIndex, bool bRemoved);
	void NoteInterceptorCacheChanged(PlayerTypes ePlayer);
	void NoteTeamRelationChanged(TeamTypes eTeam, TeamTypes eOtherTeam);
	void NoteWarStateChanged(TeamTypes eTeam, TeamTypes eOtherTeam);
	void NoteUnitAttackCountChanged(PlayerTypes eOwner, int iUnitId);
	void NoteCityAttackCountChanged(PlayerTypes eOwner, int iCityId);
	void NotePlayerResistanceChanged(PlayerTypes ePlayer, PlayerTypes eOpponent);
	void NoteTopologyInvalidated();

	// True when a segment is armed for the given observer.
	bool IsObserving(PlayerTypes ePlayer) const;

private:
	VoxRlCapture();
	~VoxRlCapture();
	VoxRlCapture(const VoxRlCapture&);
	VoxRlCapture& operator=(const VoxRlCapture&);

	struct Segment;
	class PendingRequest;
	struct Engagement;

	// Per-attachment turn-scoped campaign reuse: one CAMPAIGN block serves
	// compatible same-turn WORLD replacements. The built bytes stay in
	// capture-level storage until the first segment that publishes writes
	// them, so a suppressed first segment leaves the block available for the
	// next same-turn segment.
	unsigned int m_campaignTurnPlayer;
	int m_campaignTurn;
	unsigned int m_campaignStaticGeneration;
	unsigned int m_campaignGeneration;
	std::string m_campaignRelPath;
	unsigned int m_campaignFramedLength;
	VoxRlOwnedBlockStorage m_campaignStorage;

	// Open segment streams and index. The stream, the index, and the
	// segment directory itself are created only when the segment publishes;
	// pending frames wait in bounded memory.
	VoxRlOutputFile m_segmentStream;
	VoxRlOutputFile m_segmentIndex;
	unsigned __int64 m_segmentStreamBytes;

	// Resolves configuration and the capture root once per attachment.
	bool ResolveConfiguration();
	bool ResolveCaptureRoot();
	bool AdmitsPlayer(PlayerTypes ePlayer) const;
	// Reserves the next unused generation number for a baseline file.
	unsigned int ReserveGeneration(const std::string& directory, const char* prefix);

	// Segment lifecycle helpers.
	void CloseSegment(const char* closureReason);
	void FailSegment(const char* closureReason);
	void StartSegment(PlayerTypes ePlayer, int iTurn);
	bool BuildAndWriteStatic();
	// Captures the checkpoint WORLD in segment-owned memory so it can be
	// published only after the compatible CAMPAIGN exists.
	bool BuildWorldBaseline(PlayerTypes ePlayer, int iTurn);
	// Writes the checkpoint WORLD retained by BuildWorldBaseline.
	bool WriteWorldBaseline();
	// Builds the CAMPAIGN block into capture-level storage and binds this
	// segment to it. The file write happens at publication.
	bool BuildAndBindCampaign(PlayerTypes ePlayer, int iTurn);
	// Writes the capture-level CAMPAIGN bytes to their reserved path.
	bool WriteCampaignBaseline();
	// Appends one suppressed-segment line to suppressed.jsonl at the
	// recording root, carrying the omissions that would otherwise never
	// reach disk.
	void AppendSuppressedLine(const char* closureReason);
	// Publishes the segment when a compatible CAMPAIGN is bound and a
	// decision request has been enqueued. The budget overflow path may pass
	// allowWithoutDecision to flush a large synchronization-only backlog
	// instead of failing the segment.
	void PublishPendingFrames(bool allowWithoutDecision = false);
	bool AppendIndexLine(const char* line);
	void AddCoverageOmission(const char* reason);
	// Writes one segment's accumulated timing summary to VoxRlCapture.csv when
	// the opt-in timing configuration is active.
	void WriteTimingSummary(const char* closureReason);
	// Publishes every frame appended since the previous commit as one
	// batch. A closure-only batch commits without new binary frames.
	void CommitBatch(bool closureOnly, const char* closureReason);
	// Collects pending changes; successful publication clears the dirty sets.
	bool CollectDelta(VoxRlRequestData& data);
	void ClearDirtyState();
	// Appends one finished block to the segment's pending frames or stream
	// and records its frame index entry.
	bool EnqueueFrame(const void* bytes, unsigned int length, int blockKind,
		unsigned int requestSequence, unsigned int decisionId, int attemptIndex);
	// Emits one staged request block and consumes its sequence.
	bool EmitStagedRequest();
	// Invokes the unmodified native search once for every caller path.
	std::vector<STacticalAssignment> RunNativeSearch(const std::vector<CvUnit*>& vUnits,
		class CvPlot* pTarget, int eAggression, std::set<int>& unuseableUnits,
		bool bTargetDistanceRelevant, bool bReturnToStartPositions, int iSaveMovement);
	// Builds and closes a decision request/result pair around one native
	// search invocation.
	void RunCapturedSearch(int callerType, PlayerTypes ePlayer,
		const std::vector<CvUnit*>& vUnits, CvPlot* pTarget, int eAggression,
		std::set<int>& unuseableUnits, bool bTargetDistanceRelevant,
		bool bReturnToStartPositions, int iSaveMovement,
		std::vector<STacticalAssignment>& results);
	// Resolves a unit's owner-qualified iteration index, assigning a new
	// one for units created after the WORLD build.
	unsigned int UnitIterationIndex(PlayerTypes eOwner, int iUnitId);
	unsigned int CityIterationIndex(PlayerTypes eOwner, int iCityId);
	// Seeds delta iteration indices from the exact unit and city order used
	// by the WORLD block.
	void SeedWorldIterationIndices();

	// Attempts to read the game UUID from the Deorum save-data table.
	bool ReadGameUuidThroughLua(std::string& uuidText);

	// Formats an unsigned 64-bit value as exact decimal text for the index.
	static std::string NumberText(unsigned __int64 value);

	VoxRlCaptureConfig m_config;
	bool m_configResolved;
	bool m_rootResolved;
	std::string m_captureRoot;      // .../VoxDeorumRL
	std::string m_gameDirectory;    // <root>/<game-id>
	VoxRlGameUuid m_gameUuid;
	bool m_gameUuidKnown;
	char m_gameUuidText[40];
	unsigned int m_staticGeneration;
	bool m_staticGenerationWritten;
	std::string m_staticRelPath;
	unsigned int m_staticFramedLength;
	bool m_topologyInvalidated;
	// A war change marks the next pre-refresh moment as a WORLD
	// replacement checkpoint.
	bool m_worldReplacementPending;
	unsigned int m_decisionIdCounter;
	bool m_identityPendingLogged;
	// Pending STATIC construction time and bytes accrued since the previous
	// timing summary; folded into the next segment summary.
	unsigned __int64 m_staticConstructNs;
	unsigned __int64 m_staticWriteFlushNs;
	unsigned __int64 m_staticBuildBytes;
	// True when the native tactical zone table was rebuilt since the last
	// accepted snapshot; collection compares and re-accepts when set.
	bool m_zoneSnapshotDirty;
	// True when technology ownership changed since the last collected team
	// passability table.
	bool m_teamPassabilityDirty;
	Segment* m_segment;
	Engagement* m_engagement;
	// Nested danger refreshes belong to the active search request.
	bool m_searchActive;
	bool m_shuttingDown;
	bool m_concluded;
	bool m_worldReplacementLogged;
};

#endif
