// Installed from simulator/capture/VoxRlCapture.cpp. Edit that source, then reinstall.

// Vox Deorum: recording capture manager. See VoxRlCapture.h.
#include "CvGameCoreDLLPCH.h"
#include "VoxDeorumRL/VoxRlCapture.h"
#include "VoxDeorumRL/VoxRlCaptureBuilders.h"
#include "schema/VoxRlCollectors.generated.h"
#include "VoxDeorumRL/VoxRlCaptureFiles.h"
#include "VoxDeorumRL/schema/VoxRlBlockMetadata.h"
#include "VoxDeorumRL/schema/VoxRlBlockStorage.h"
#include "VoxDeorumRL/schema/VoxRlBlockWriter.h"

#include "../commit_id.inc"
#include "CvConnectionService.h"
#include "CvDangerPlots.h"
#include "CvTacticalAI.h"
#include "CvTacticalAnalysisMap.h"

#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <sstream>

namespace
{
	// Reads the opt-in once at DLL load, before any game lifecycle hooks run.
	bool ReadCaptureEnabled()
	{
		const char* enabled = getenv("VOX_RL_CAPTURE");
		return enabled != NULL && enabled[0] == '1';
	}

	// Compares zero-initialized portable rows to omit unchanged zone tables.
	template <typename Record>
	bool SameRows(const std::vector<Record>& left, const std::vector<Record>& right)
	{
		return left.size() == right.size() && (left.empty() ||
			std::memcmp(&left[0], &right[0], left.size() * sizeof(Record)) == 0);
	}

	// Returns a high-resolution monotonic timestamp in nanoseconds for the
	// opt-in producer timings.
	unsigned __int64 TimingNanoseconds()
	{
		static LONGLONG frequency = 0;
		if (frequency == 0)
		{
			LARGE_INTEGER period;
			if (!QueryPerformanceFrequency(&period)) return 0;
			frequency = period.QuadPart;
		}
		LARGE_INTEGER now;
		if (!QueryPerformanceCounter(&now)) return 0;
		// Seconds and remainder are converted separately so the multiply cannot
		// overflow the 64-bit total at any uptime.
		return static_cast<unsigned __int64>(now.QuadPart / frequency) * 1000000000ULL +
			(static_cast<unsigned __int64>(now.QuadPart % frequency) * 1000000000ULL) /
			static_cast<unsigned __int64>(frequency);
	}

	// Adds one scoped interval to a nanosecond total while timings are enabled.
	class ScopedTiming
	{
	public:
		ScopedTiming(bool enabled, unsigned __int64& total)
			: m_enabled(enabled), m_total(total), m_start(enabled ? TimingNanoseconds() : 0)
		{
		}
		~ScopedTiming()
		{
			if (m_enabled) m_total += TimingNanoseconds() - m_start;
		}
	private:
		bool m_enabled;
		unsigned __int64& m_total;
		unsigned __int64 m_start;
	};
}

const bool gVoxRlCaptureEnabled = ReadCaptureEnabled();

// Per-segment recording state. One segment is active at a time and spans
// one player turn between the pre-danger checkpoint and the next checkpoint.
struct VoxRlCapture::Segment
{
	PlayerTypes player;
	int turn;
	unsigned int staticGeneration;
	unsigned int worldGeneration;
	unsigned int campaignGeneration;

	std::string relativeSegmentDir;
	std::string streamPath;
	std::string indexPath;
	std::string staticRelPath;
	std::string campaignRelPath;
	std::string worldRelPath;
	unsigned int staticFramedLength;
	unsigned int campaignFramedLength;
	unsigned int worldFramedLength;

	unsigned int nextDeltaSequence;
	bool published;
	bool failed;
	bool closed;
	char closureReason[40];

	// Pending frames collected before a compatible CAMPAIGN permits
	// publication. Bounded by the pending byte budget.
	std::vector<VoxRlFrameEntry> frameTable;
	std::vector<unsigned char> pendingBytes;
	// The checkpoint WORLD stays in its completed owned storage until the
	// campaign seam permits publication, so unpublished observers leave no
	// WORLD file behind and no duplicate bytes are held. Directories follow
	// the same rule: they are created only when a file is about to be written.
	VoxRlOwnedBlockStorage worldStorage;
	VoxRlZoneSnapshot zoneSnapshot;
	unsigned int pendingCount;
	unsigned int committedFrameCount;
	unsigned int committedDecisionCount;
	unsigned int committedCoverageCount;
	unsigned int commitId;
	unsigned int firstRequestSequence;
	unsigned int lastRequestSequence;
	bool hasRequests;

	struct DecisionRow
	{
		unsigned int decisionId;
		int attemptIndex;
		int caller;
		int retryOutcome;
		unsigned int requestFrameIndex;
		unsigned int resultFrameIndex;
		bool hasPrevious;
		int previousAttemptIndex;
		unsigned int previousWorldGeneration;
	};
	std::vector<DecisionRow> decisions;
	std::vector<std::string> coverageLines;

	// Dirty sets filled by setter notifications.
	std::set<VoxRlEntityKey> dirtyUnits;
	std::set<VoxRlEntityKey> removedUnits;
	std::set<VoxRlEntityKey> dirtyCities;
	std::set<VoxRlEntityKey> removedCities;
	std::set<int> dirtyPlots;
	std::map<VoxRlVisibilityKey, unsigned char> visibilityFlips;
	std::set<int> visibilityResets;                                    // known-visibility reset teams
	std::map<VoxRlEntityKey, unsigned char> revealedOverrideUpserts;  // team-major key, 1 = present
	std::set<VoxRlEntityKey> removedRevealedOverrides;                // (team, plot)
	std::set<int> dirtyInterceptors;
	std::map<VoxRlEntityKey, unsigned char> dirtyRelations;           // (team, otherTeam)
	std::vector<RequestDangerEventRecord> pendingDangerEvents;
	// Sparse family dirty entity sets: creation, promotion, and counter hooks mark
	// only the affected entities, and removals drop their keys from collection work.
	std::set<VoxRlEntityKey> dirtyUnitModifiers;
	std::set<VoxRlEntityKey> dirtyUnitPlagues;
	std::set<VoxRlEntityKey> dirtyUnitBlockedPromotions;
	std::set<VoxRlEntityKey> dirtyUnitAttackCounts;
	std::set<int> dirtyPlayerResistances;
	std::set<VoxRlEntityKey> dirtyCityAttackCounts;

	// Last emitted plot and city records for the omit-unchanged comparison.
	// Only entities emitted as deltas in this segment are retained.
	std::map<VoxRlEntityKey, RequestDeltaPlotCoreRecord> lastPlotRows;
	std::map<VoxRlEntityKey, RequestDeltaCityRecord> lastCityRows;
	std::map<int, TeamPassabilityRecord> lastTeamPassabilityRows;

	// The zone-id-to-row decode table of the currently accepted zone snapshot;
	// rebuilt only when a replacement accepts a new table.
	std::map<int, unsigned int> zoneRowByZoneId;

	// Iteration index bookkeeping across deltas.
	std::map<VoxRlEntityKey, unsigned int> unitIteration;
	std::map<VoxRlEntityKey, unsigned int> cityIteration;
	std::map<int, unsigned int> nextUnitIteration;
	std::map<int, unsigned int> nextCityIteration;

	// Staged synchronization request during a refresh or discovery window.
	bool stagingRequest;
	VoxRlRequestData staged;

	// Opt-in timing accumulators for this segment, reported at closure. All
	// time members are nanoseconds from the high-resolution performance
	// counter; the byte and count members carry the measured volumes.
	struct Timings
	{
		unsigned __int64 worldBuildNs;
		VoxRlWorldBuildTimings worldPhases;
		unsigned __int64 worldWriteFlushNs;
		unsigned __int64 campaignConstructNs;
		unsigned __int64 campaignWriteFlushNs;
		unsigned __int64 deltaCollectNs;
		unsigned __int64 requestBuildNs;
		unsigned __int64 resultBuildNs;
		unsigned __int64 pendingBufferNs;
		unsigned __int64 pendingPublishWriteNs;
		unsigned __int64 streamWriteNs;
		unsigned __int64 commitNs;
		unsigned __int64 streamFlushNs;
		unsigned __int64 indexFlushNs;
		unsigned __int64 worldBuildBytes;
		unsigned __int64 campaignBuildBytes;
		unsigned __int64 requestBytes;
		unsigned __int64 resultBytes;
		unsigned __int64 pendingBufferBytes;
		unsigned __int64 pendingPublishWriteBytes;
		unsigned __int64 streamWriteBytes;
		unsigned __int64 indexBytes;
		unsigned __int64 deltaRows;
		unsigned int deltaCollectCount;
		unsigned int requestCount;
		unsigned int resultCount;
		unsigned int commitCount;
		Timings()
			: worldBuildNs(0), worldWriteFlushNs(0), campaignConstructNs(0), campaignWriteFlushNs(0),
			deltaCollectNs(0), requestBuildNs(0), resultBuildNs(0), pendingBufferNs(0),
			pendingPublishWriteNs(0), streamWriteNs(0), commitNs(0), streamFlushNs(0), indexFlushNs(0),
			worldBuildBytes(0), campaignBuildBytes(0), requestBytes(0), resultBytes(0),
			pendingBufferBytes(0), pendingPublishWriteBytes(0), streamWriteBytes(0), indexBytes(0), deltaRows(0),
			deltaCollectCount(0), requestCount(0), resultCount(0), commitCount(0)
		{
		}
	};
	Timings timings;

	Segment()
		: player(NO_PLAYER), turn(-1), staticGeneration(0), worldGeneration(0), campaignGeneration(0),
		staticFramedLength(0), campaignFramedLength(0), worldFramedLength(0),
		nextDeltaSequence(0), published(false), failed(false), closed(false),
		pendingCount(0), committedFrameCount(0), committedDecisionCount(0), committedCoverageCount(0),
		commitId(0), firstRequestSequence(0), lastRequestSequence(0), hasRequests(false),
		stagingRequest(false)
	{
		closureReason[0] = '\0';
	}
};

// One caller engagement: all attempts of one FindAndExecuteBestUnitAssignments
// or PerformRangedOpportunityAttack invocation share its decision id.
struct VoxRlCapture::Engagement
{
	bool active;
	int callerType;
	PlayerTypes player;
	unsigned int decisionId;
	int attemptIndex;
	int lastOutcome;
	unsigned int lastAttemptWorldGeneration;
	bool lastSearchEmpty;
	Engagement()
		: active(false), callerType(0), player(NO_PLAYER), decisionId(0), attemptIndex(0),
		lastOutcome(0), lastAttemptWorldGeneration(0), lastSearchEmpty(false)
	{
	}
};

VoxRlCaptureConfig::VoxRlCaptureConfig()
	: enabled(false), filterPlayers(false), filterTurns(false), timings(false)
{
}

namespace
{
	// Memory ceiling for frames waiting for publication. Exceeding it
	// closes the affected segment at its last commit.
	const unsigned int kVoxRlPendingBudgetBytes = 8U * 1024U * 1024U;

	void AppendNumber(std::string& out, unsigned __int64 value)
	{
		char buffer[24];
		sprintf_s(buffer, 24, "%I64u", value);
		out += buffer;
	}

	void AppendInt(std::string& out, int value)
	{
		char buffer[16];
		sprintf_s(buffer, 16, "%d", value);
		out += buffer;
	}

	// Minimal JSON string escaping for controlled producer text.
	void AppendJsonString(std::string& out, const char* text)
	{
		out += '"';
		for (const char* cursor = text; *cursor != '\0'; ++cursor)
		{
			if (*cursor == '"' || *cursor == '\\')
			{
				out += '\\';
				out += *cursor;
			}
			else if (*cursor == '\n')
			{
				out += "\\n";
			}
			else
			{
				out += *cursor;
			}
		}
		out += '"';
	}

	// The manifest hash text: eight 32-bit words in order.
	std::string ManifestHashText()
	{
		std::string text;
		char buffer[16];
		for (int word = 0; word < 8; ++word)
		{
			sprintf_s(buffer, 16, "%08x", static_cast<unsigned int>(kVoxRlManifestHashWords[word]));
			text += buffer;
		}
		return text;
	}

	// Counts the rows one collected request carries across every section.
	unsigned __int64 CountRequestRows(const VoxRlRequestData& data)
	{
		return data.requestDangerEvents.size() + data.requestTeamRelations.size() +
			data.requestDeltaUnits.size() + data.requestDeltaPlots.size() +
			data.requestDeltaPlotZones.size() + data.requestDeltaPlotZonesWide.size() +
			data.requestZoneReplacements.size() + data.requestZoneNeighbors.size() +
			data.requestDeltaCities.size() + data.requestRemovedUnits.size() +
			data.requestRemovedCities.size() + data.requestTeamPassability.size() +
			data.requestVisibilityFlips.size() + data.requestVisibilityResets.size() + data.requestRevealedOverrideUpserts.size() +
			data.requestRemovedRevealedOverrides.size() + data.requestKnownAttackers.size() +
			data.requestInterceptorReplacements.size() + data.requestInterceptorEntries.size() +
			data.requestParticipants.size() + data.requestDroppedUnits.size() +
			data.requestUnitModifierReplacements.size() + data.requestUnitModifierRows.size() +
			data.requestUnitPlagueReplacements.size() + data.requestUnitPlagueRows.size() +
			data.requestUnitBlockedPromotionReplacements.size() + data.requestUnitBlockedPromotionRows.size() +
			data.requestUnitAttackCountReplacements.size() + data.requestUnitAttackCountRows.size() +
			data.requestPlayerResistanceReplacements.size() + data.requestPlayerResistanceRows.size() +
			data.requestCityAttackCountReplacements.size() + data.requestCityAttackCountRows.size() +
			data.requestDeltaUnitMovementCounts.size();
	}
}

VoxRlCapture& VoxRlCapture::GetInstance()
{
	static VoxRlCapture instance;
	return instance;
}

VoxRlCapture::VoxRlCapture()
	: m_configResolved(false),
	m_rootResolved(false),
	m_gameUuidKnown(false),
	m_staticGeneration(0),
	m_staticGenerationWritten(false),
	m_topologyInvalidated(false),
	m_worldReplacementPending(false),
	m_decisionIdCounter(0),
	m_identityPendingLogged(false),
	m_staticConstructNs(0),
	m_staticWriteFlushNs(0),
	m_staticBuildBytes(0),
	m_zoneSnapshotDirty(false),
	m_teamPassabilityDirty(false),
	m_campaignTurnPlayer(0xFFFFFFFF),
	m_campaignTurn(-1),
	m_campaignStaticGeneration(0),
	m_campaignGeneration(0),
	m_campaignFramedLength(0),
	m_segmentStreamBytes(0),
	m_segment(NULL),
	m_engagement(NULL),
	m_searchActive(false),
	m_shuttingDown(false),
	m_concluded(false),
	m_worldReplacementLogged(false)
{
	std::memset(&m_gameUuid, 0, sizeof(m_gameUuid));
	std::memset(m_gameUuidText, 0, sizeof(m_gameUuidText));
}

VoxRlCapture::~VoxRlCapture()
{
	delete m_segment;
	m_segment = NULL;
	delete m_engagement;
	m_engagement = NULL;
}

bool VoxRlCapture::IsObserving(PlayerTypes ePlayer) const
{
	return m_segment != NULL && !m_segment->failed && !m_segment->closed &&
		m_segment->player == ePlayer && m_gameUuidKnown;
}

bool VoxRlCapture::DiscoverGameUuid()
{
	if (m_gameUuidKnown)
	{
		return true;
	}
	std::string uuidText;
	if (!CvConnectionService::GetInstance().TryReadGameUuid(uuidText))
	{
		return false;
	}
	// Canonical UUID text: 36 lowercase hex digits with hyphens at the
	// canonical positions. Reject anything else without arming capture.
	if (uuidText.size() != 36 || uuidText[8] != '-' || uuidText[13] != '-' ||
		uuidText[18] != '-' || uuidText[23] != '-')
	{
		return false;
	}
	for (size_t index = 0; index < uuidText.size(); ++index)
	{
		const char c = uuidText[index];
		const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (!digit && c != '-')
		{
			return false;
		}
	}
	unsigned int word = 0;
	int digitsInWord = 0;
	int wordIndex = 0;
	for (size_t index = 0; index < uuidText.size(); ++index)
	{
		if (uuidText[index] == '-') continue;
		const char c = uuidText[index];
		unsigned int digit = 0;
		if (c >= '0' && c <= '9') digit = static_cast<unsigned int>(c - '0');
		else digit = static_cast<unsigned int>(c - 'a' + 10);
		word = (word << 4) | digit;
		if (++digitsInWord == 8)
		{
			m_gameUuid.words[wordIndex] = word;
			++wordIndex;
			word = 0;
			digitsInWord = 0;
		}
	}
	if (wordIndex != 4)
	{
		return false;
	}
	m_gameUuidKnown = true;
	VoxRlFormatGameUuidText(m_gameUuid, m_gameUuidText);
	return true;
}

bool VoxRlCapture::ReadGameUuidThroughLua(std::string& uuidText)
{
	return CvConnectionService::GetInstance().TryReadGameUuid(uuidText);
}

bool VoxRlCapture::ResolveConfiguration()
{
	if (m_configResolved)
	{
		return m_config.enabled;
	}
	m_configResolved = true;
	m_config.enabled = gVoxRlCaptureEnabled;
	if (!m_config.enabled)
	{
		return false;
	}
	const char* root = getenv("VOX_RL_CAPTURE_ROOT");
	if (root != NULL && root[0] != '\0')
	{
		m_config.rootOverride = root;
	}
	const char* players = getenv("VOX_RL_CAPTURE_PLAYERS");
	if (players != NULL && players[0] != '\0')
	{
		m_config.filterPlayers = true;
		std::stringstream stream(players);
		std::string entry;
		while (std::getline(stream, entry, ','))
		{
			if (entry.empty()) continue;
			m_config.players.insert(atoi(entry.c_str()));
		}
	}
	// The opt-in producer timings accumulate in memory and report at segment
	// closure, so an armed game pays one branch per timing point.
	const char* timings = getenv("VOX_RL_CAPTURE_TIMINGS");
	m_config.timings = timings != NULL && timings[0] == '1';
	return true;
}

bool VoxRlCapture::ResolveCaptureRoot()
{
	if (m_rootResolved)
	{
		return !m_captureRoot.empty();
	}
	m_rootResolved = true;
	if (!m_config.rootOverride.empty())
	{
		m_captureRoot = m_config.rootOverride;
	}
	else
	{
		// The engine exposes the per-user cache folder, which always sits
		// inside Civ V's per-user game folder. Derive that folder rather
		// than resolving Documents independently in the DLL.
		const char* cachePath = gDLL->GetCacheFolderPath();
		if (cachePath == NULL || cachePath[0] == '\0')
		{
			return false;
		}
		std::string gameFolder;
		if (!VoxRlStripLastPathComponent(cachePath, gameFolder))
		{
			return false;
		}
		m_captureRoot = gameFolder;
		m_captureRoot += "/VoxDeorumRL";
	}
	// The root is only resolved here; the directory chain is created when
	// the first file is written, so an attachment that never publishes
	// leaves no folders behind.
	return true;
}

bool VoxRlCapture::AdmitsPlayer(PlayerTypes ePlayer) const
{
	// The barbarian slot can be included in the explicit environment filter,
	// but it never reaches the campaign seam and therefore cannot publish.
	if (ePlayer == BARBARIAN_PLAYER) return false;
	// Campaign capture is driven by CvMilitaryAI::UpdateOperations, which
	// only runs for major civilizations. Keep minor observers out of the
	// recording lifecycle while WORLD data still includes their entities.
	if (!GET_PLAYER(ePlayer).isMajorCiv()) return false;
	if (!m_config.filterPlayers) return true;
	return m_config.players.count(static_cast<int>(ePlayer)) != 0;
}

unsigned int VoxRlCapture::ReserveGeneration(const std::string& directory, const char* prefix)
{
	// Generation numbers are unique within the game directory: the next
	// free file name is reserved without overwriting prior or unfinished
	// files, which also covers process restarts.
	unsigned int candidate = m_staticGeneration + 1;
	if (candidate < 1) candidate = 1;
	while (true)
	{
		char suffix[32];
		sprintf_s(suffix, 32, "/%s-%u.bin", prefix, candidate);
		const std::string path = directory + suffix;
		if (!VoxRlFileExists(path.c_str()))
		{
			return candidate;
		}
		++candidate;
		if (candidate == 0) return 0;
	}
}

void VoxRlCapture::OnGameStartOrLoad()
{
	// A load or new game ends any engagement and unfinished segment; the
	// identity is re-read from the loaded save at the next checkpoint.
	EndEngagement();
	CloseSegment("gameLoad");
	m_gameUuidKnown = false;
	std::memset(&m_gameUuid, 0, sizeof(m_gameUuid));
	std::memset(m_gameUuidText, 0, sizeof(m_gameUuidText));
	m_staticGeneration = 0;
	m_staticGenerationWritten = false;
	m_campaignTurnPlayer = 0xFFFFFFFF;
	m_campaignTurn = -1;
	m_campaignStaticGeneration = 0;
	m_campaignGeneration = 0;
	m_campaignRelPath.clear();
	m_campaignFramedLength = 0;
	m_topologyInvalidated = false;
	m_worldReplacementPending = false;
	m_decisionIdCounter = 0;
	m_identityPendingLogged = false;
	m_staticConstructNs = 0;
	m_staticWriteFlushNs = 0;
	m_staticBuildBytes = 0;
	m_zoneSnapshotDirty = false;
	m_teamPassabilityDirty = false;
	m_shuttingDown = false;
	m_concluded = false;
	m_config = VoxRlCaptureConfig();
	m_configResolved = false;
	m_rootResolved = false;
	m_captureRoot.clear();
	m_gameDirectory.clear();
}

void VoxRlCapture::Shutdown()
{
	m_shuttingDown = true;
	EndEngagement();
	CloseSegment("gameTeardown");
}

void VoxRlCapture::OnGameConcluded()
{
	if (m_concluded)
	{
		return;
	}
	// The game ended: commit the open segment, release the output file
	// handles, and ignore capture hooks until a new game starts. The
	// victory archive can then move the recording directory.
	m_concluded = true;
	EndEngagement();
	CloseSegment("gameConcluded");
}

void VoxRlCapture::CloseSegment(const char* closureReason)
{
	if (m_segment == NULL)
	{
		return;
	}
	if (m_segment->stagingRequest)
	{
		// A refresh window straddled the boundary; discard the staged rows
		// and record the gap.
		m_segment->stagingRequest = false;
		AddCoverageOmission("interruptedRefresh");
	}
	if (!m_segment->failed && m_segment->published)
	{
		// Closure owns the durability sequence: the stream becomes durable
		// before the closing index commit is appended, and the index becomes
		// durable before its handle closes. Ordinary event commits perform no
		// synchronization, so this is the only point where the segment's
		// committed prefix is guaranteed to survive power loss.
		bool flushed = false;
		{
			ScopedTiming timing(m_config.timings, m_segment->timings.streamFlushNs);
			flushed = m_segmentStream.Flush();
		}
		if (!flushed)
		{
			FailSegment("captureFailure");
			return;
		}
		// A closure-only batch commits without adding binary frames.
		CommitBatch(true, closureReason);
		if (m_segment == NULL || m_segment->failed)
		{
			return;
		}
		bool indexFlushed = false;
		{
			ScopedTiming timing(m_config.timings, m_segment->timings.indexFlushNs);
			indexFlushed = m_segmentIndex.Flush();
		}
		if (!indexFlushed)
		{
			FailSegment("captureFailure");
			return;
		}
	}
	WriteTimingSummary(closureReason);
	delete m_segment;
	m_segment = NULL;
	m_segmentStream.Close();
	m_segmentIndex.Close();
}

void VoxRlCapture::FailSegment(const char* closureReason)
{
	if (m_segment == NULL || m_segment->failed)
	{
		return;
	}
	m_segment->failed = true;
	FILogFile* log = LOGFILEMGR.GetLog("VoxRlCapture.log", FILogFile::kDontTimeStamp);
	if (log != NULL)
	{
		log->Msg("Capture segment failed: stage=%s player=%d turn=%d.\n", closureReason,
			static_cast<int>(m_segment->player), m_segment->turn);
	}
	WriteTimingSummary(closureReason);
	// Retain the prior committed prefix exactly as it is on disk. No
	// further index lines are appended after a damaged tail; capture
	// restarts at a fresh supported checkpoint.
	delete m_segment;
	m_segment = NULL;
	m_segmentStream.Close();
	m_segmentIndex.Close();
}

bool VoxRlCapture::AppendIndexLine(const char* line)
{
	if (!m_segmentIndex.IsOpen())
	{
		return false;
	}
	return m_segmentIndex.Write(line, static_cast<unsigned int>(std::strlen(line)));
}

void VoxRlCapture::AddCoverageOmission(const char* reason)
{
	if (m_segment == NULL)
	{
		return;
	}
	std::string line = "{\"type\":\"coverage\",\"omissions\":[{\"reason\":";
	AppendJsonString(line, reason);
	line += "}]}\n";
	m_segment->coverageLines.push_back(line);
}

// Writes one segment's accumulated timing summary to VoxRlCapture.csv. The
// timings are opt-in, accumulate in memory, and publish only at this segment
// boundary, so ordinary events never pay a log write.
void VoxRlCapture::WriteTimingSummary(const char* closureReason)
{
	if (m_segment == NULL || !m_config.timings)
	{
		return;
	}
	const Segment::Timings& t = m_segment->timings;
	FILogFile* log = LOGFILEMGR.GetLog("VoxRlCapture.csv", FILogFile::kDontTimeStamp);
	if (log == NULL)
	{
		m_staticConstructNs = 0;
		m_staticWriteFlushNs = 0;
		m_staticBuildBytes = 0;
		return;
	}
	static bool headerWritten = false;
	if (!headerWritten)
	{
		log->Msg("game_uuid,player,turn,world_generation,closure,failed,published,frames,commits,"
			"plots,units,cities,alive_players,alive_teams,static_construct_ns,static_write_flush_ns,static_bytes,"
			"world_construct_ns,world_owner_iteration_ns,world_player_citadel_ns,world_danger_sparse_relations_ns,"
			"world_zone_ns,world_plot_unit_ns,world_visibility_ns,world_entity_relation_ns,world_serialize_ns,"
			"world_write_flush_ns,world_bytes,campaign_construct_ns,campaign_write_flush_ns,campaign_bytes,"
			"delta_collections,delta_collect_ns,delta_rows,requests,request_build_ns,request_bytes,results,"
			"result_build_ns,result_bytes,pending_buffer_ns,pending_buffer_bytes,pending_publish_write_ns,"
			"pending_publish_write_bytes,stream_write_ns,stream_write_bytes,commit_ns,index_bytes,"
			"stream_flush_ns,index_flush_ns\n");
		headerWritten = true;
	}
	log->Msg("%s,%d,%d,%u,%s,%d,%d,%u,%u,%u,%u,%u,%u,%u,"
		"%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,"
		"%I64u,%I64u,%I64u,%u,%I64u,%I64u,%u,%I64u,%I64u,%u,%I64u,%I64u,%I64u,%I64u,%I64u,"
		"%I64u,%I64u,%I64u,%I64u,%I64u,%I64u,%I64u\n",
		m_gameUuidText, static_cast<int>(m_segment->player), m_segment->turn, m_segment->worldGeneration,
		closureReason != NULL ? closureReason : "unknown", m_segment->failed ? 1 : 0,
		m_segment->published ? 1 : 0, static_cast<unsigned int>(m_segment->frameTable.size()), t.commitCount,
		t.worldPhases.plotCount, t.worldPhases.unitCount, t.worldPhases.cityCount,
		t.worldPhases.alivePlayerCount, t.worldPhases.aliveTeamCount,
		m_staticConstructNs, m_staticWriteFlushNs, m_staticBuildBytes,
		t.worldBuildNs, t.worldPhases.ownerIterationNs, t.worldPhases.playerCitadelNs,
		t.worldPhases.dangerSparseRelationsNs, t.worldPhases.zoneNs, t.worldPhases.plotUnitNs,
		t.worldPhases.visibilityNs, t.worldPhases.entityRelationNs, t.worldPhases.serializeNs,
		t.worldWriteFlushNs, t.worldBuildBytes, t.campaignConstructNs, t.campaignWriteFlushNs,
		t.campaignBuildBytes, t.deltaCollectCount, t.deltaCollectNs, t.deltaRows,
		t.requestCount, t.requestBuildNs, t.requestBytes, t.resultCount, t.resultBuildNs, t.resultBytes,
		t.pendingBufferNs, t.pendingBufferBytes, t.pendingPublishWriteNs, t.pendingPublishWriteBytes,
		t.streamWriteNs, t.streamWriteBytes, t.commitNs, t.indexBytes, t.streamFlushNs, t.indexFlushNs);
	m_staticConstructNs = 0;
	m_staticWriteFlushNs = 0;
	m_staticBuildBytes = 0;
}

bool VoxRlCapture::BuildAndWriteStatic()
{
	VoxRlBlockIdentity identity;
	identity.session = m_gameUuid;
	identity.turn = kVoxRlAbsentTurn;
	identity.player = kVoxRlAbsentPlayer;
	identity.staticGeneration = m_staticGeneration;
	identity.generation = m_staticGeneration;

	VoxRlOwnedBlockStorage storage;
	unsigned int length = 0;
	char fileName[32];
	{
		ScopedTiming timing(m_config.timings, m_staticConstructNs);
		if (!VoxRlBuildStaticBlock(identity, storage, length))
		{
			return false;
		}
	}
	{
		ScopedTiming timing(m_config.timings, m_staticWriteFlushNs);
		const std::string directory = m_gameDirectory + "/baselines/static";
		sprintf_s(fileName, 32, "static-%u.bin", m_staticGeneration);
		const std::string path = directory + "/" + fileName;
		VoxRlOutputFile file;
		if (!file.OpenNew(path.c_str()) || !file.Write(storage.Bytes(), length) || !file.Flush())
		{
			return false;
		}
		m_staticBuildBytes += length;
	}
	m_staticRelPath = "baselines/static/" + std::string(fileName);
	m_staticFramedLength = length;
	m_staticGenerationWritten = true;
	m_topologyInvalidated = false;
	return true;
}

bool VoxRlCapture::BuildWorldBaseline(PlayerTypes ePlayer, int iTurn)
{
	Segment& segment = *m_segment;
	char folder[64];
	sprintf_s(folder, 64, "baselines/world/player-%d/turn-%d", static_cast<int>(ePlayer), iTurn);
	const std::string directory = m_gameDirectory + "/" + folder;
	segment.worldGeneration = ReserveGeneration(directory, "world");
	if (segment.worldGeneration == 0)
	{
		return false;
	}
	VoxRlBlockIdentity identity;
	identity.session = m_gameUuid;
	identity.turn = iTurn;
	identity.player = static_cast<int>(ePlayer);
	identity.generation = segment.worldGeneration;
	identity.staticGeneration = segment.staticGeneration;
	identity.worldGeneration = segment.worldGeneration;

	VoxRlOwnedBlockStorage storage;
	unsigned int length = 0;
	std::vector<TeamPassabilityRecord> teamPassability;
	{
		ScopedTiming timing(m_config.timings, m_segment->timings.worldBuildNs);
		VoxRlWorldBuildTimings* phases = m_config.timings ? &segment.timings.worldPhases : NULL;
		if (!VoxRlBuildWorldBlock(identity, ePlayer, storage, length, segment.zoneSnapshot,
			teamPassability, phases))
		{
			return false;
		}
	}
	segment.timings.worldBuildBytes += length;
	segment.lastTeamPassabilityRows.clear();
	for (size_t index = 0; index < teamPassability.size(); ++index)
		segment.lastTeamPassabilityRows[static_cast<int>(teamPassability[index].team)] = teamPassability[index];
	// The WORLD build accepts a fresh zone snapshot and the collected team
	// passability table, so both dirty markers reset here.
	m_zoneSnapshotDirty = false;
	m_teamPassabilityDirty = false;
	segment.zoneRowByZoneId.clear();
	for (size_t zoneIndex = 0; zoneIndex < segment.zoneSnapshot.zones.size(); ++zoneIndex)
		segment.zoneRowByZoneId[segment.zoneSnapshot.zones[zoneIndex].zoneId] = static_cast<unsigned int>(zoneIndex + 1);
	char fileName[32];
	sprintf_s(fileName, 32, "world-%u.bin", segment.worldGeneration);
	segment.worldRelPath = folder + std::string("/") + fileName;
	segment.worldFramedLength = length;
	// The completed WORLD keeps its built storage; the swap transfers the
	// allocation without copying the bytes.
	segment.worldStorage.Swap(&storage);
	SeedWorldIterationIndices();
	return true;
}

bool VoxRlCapture::WriteWorldBaseline()
{
	Segment& segment = *m_segment;
	if (segment.worldStorage.ByteLength() == 0 || segment.worldRelPath.empty())
	{
		return false;
	}
	const std::string path = m_gameDirectory + "/" + segment.worldRelPath;
	VoxRlOutputFile file;
	bool written = false;
	{
		ScopedTiming timing(m_config.timings, segment.timings.worldWriteFlushNs);
		written = file.OpenNew(path.c_str()) &&
			file.Write(segment.worldStorage.Bytes(), segment.worldFramedLength) && file.Flush();
	}
	if (!written)
	{
		return false;
	}
	segment.worldStorage.Clear();
	return true;
}

bool VoxRlCapture::BuildAndWriteCampaign(PlayerTypes ePlayer, int iTurn)
{
	Segment& segment = *m_segment;
	char folder[80];
	sprintf_s(folder, 80, "baselines/campaign/player-%d/turn-%d", static_cast<int>(ePlayer), iTurn);
	const std::string directory = m_gameDirectory + "/" + folder;
	const unsigned int campaignGeneration = ReserveGeneration(directory, "campaign");
	if (campaignGeneration == 0)
	{
		return false;
	}
	VoxRlBlockIdentity identity;
	identity.session = m_gameUuid;
	identity.turn = iTurn;
	identity.player = static_cast<int>(ePlayer);
	identity.generation = campaignGeneration;
	identity.staticGeneration = segment.staticGeneration;
	identity.campaignGeneration = campaignGeneration;

	VoxRlOwnedBlockStorage storage;
	unsigned int length = 0;
	char fileName[36];
	{
		ScopedTiming timing(m_config.timings, segment.timings.campaignConstructNs);
		if (!VoxRlBuildCampaignBlock(identity, ePlayer, storage, length))
		{
			return false;
		}
	}
	{
		ScopedTiming timing(m_config.timings, segment.timings.campaignWriteFlushNs);
		sprintf_s(fileName, 36, "campaign-%u.bin", campaignGeneration);
		const std::string path = directory + "/" + fileName;
		VoxRlOutputFile file;
		if (!file.OpenNew(path.c_str()) || !file.Write(storage.Bytes(), length) || !file.Flush())
		{
			return false;
		}
		segment.timings.campaignBuildBytes += length;
	}
	m_campaignTurnPlayer = static_cast<unsigned int>(ePlayer);
	m_campaignTurn = iTurn;
	m_campaignStaticGeneration = segment.staticGeneration;
	m_campaignGeneration = campaignGeneration;
	m_campaignRelPath = folder + std::string("/") + fileName;
	m_campaignFramedLength = length;
	segment.campaignGeneration = campaignGeneration;
	segment.campaignRelPath = m_campaignRelPath;
	segment.campaignFramedLength = length;
	return true;
}

void VoxRlCapture::StartSegment(PlayerTypes ePlayer, int iTurn)
{
	m_segment = new Segment();
	Segment& segment = *m_segment;
	segment.player = ePlayer;
	segment.turn = iTurn;
	segment.staticGeneration = m_staticGeneration;
	if (!BuildWorldBaseline(ePlayer, iTurn))
	{
		FailSegment("worldBuild");
		return;
	}
	char folder[80];
	sprintf_s(folder, 80, "segments/player-%d/turn-%d/world-%u",
		static_cast<int>(ePlayer), iTurn, segment.worldGeneration);
	segment.relativeSegmentDir = folder;
	const std::string segmentDir = m_gameDirectory + "/" + folder;
	segment.streamPath = segmentDir + "/stream.bin";
	segment.indexPath = segmentDir + "/index.jsonl";
	segment.staticRelPath = m_staticRelPath;
	segment.staticFramedLength = m_staticFramedLength;
	// A same-turn WORLD replacement reuses the compatible CAMPAIGN.
	if (m_campaignTurnPlayer == static_cast<unsigned int>(ePlayer) && m_campaignTurn == iTurn &&
		m_campaignStaticGeneration == segment.staticGeneration && m_campaignGeneration != 0)
	{
		segment.campaignGeneration = m_campaignGeneration;
		segment.campaignRelPath = m_campaignRelPath;
		segment.campaignFramedLength = m_campaignFramedLength;
	}
	m_segmentStreamBytes = 0;
}

void VoxRlCapture::OnPreDangerCheckpoint(PlayerTypes ePlayer)
{
	if (m_concluded)
	{
		// The game concluded; post-victory play is not recorded.
		return;
	}
	if (!ResolveConfiguration())
	{
		return;
	}
	if (gDLL == NULL || !gDLL->IsGameCoreThread())
	{
		return;
	}
	const int iTurn = GC.getGame().getGameTurn();
	if (m_segment != NULL && !m_segment->closed &&
		(m_segment->player != ePlayer || m_segment->turn != iTurn))
	{
		// The previous segment's turn ended: another player's checkpoint or
		// a repeated checkpoint for the same turn starts a fresh segment.
		CloseSegment("turnComplete");
	}
	if (m_segment != NULL && !m_segment->closed)
	{
		// The checkpoint is idempotent within one player turn.
		return;
	}
	if (!AdmitsPlayer(ePlayer))
	{
		if (m_segment != NULL && !m_segment->closed)
		{
			CloseSegment("filterBoundary");
		}
		return;
	}
	if (!DiscoverGameUuid())
	{
		// The MCP synchronization has not established the game ID yet.
		// Capture stays unarmed and retries at the next checkpoint.
		if (!m_identityPendingLogged)
		{
			m_identityPendingLogged = true;
			if (GC.getLogging())
			{
				FILogFile* log = LOGFILEMGR.GetLog("VoxRlCapture.log", FILogFile::kDontTimeStamp);
				if (log != NULL)
				{
					log->Msg("Capture waits for the game identity; the next checkpoint retries.\n");
				}
			}
		}
		return;
	}
	if (!ResolveCaptureRoot())
	{
		return;
	}
	m_gameDirectory = m_captureRoot + "/" + m_gameUuidText;
	// STATIC is reused until a topology invalidation replaces it.
	if (!m_staticGenerationWritten || m_topologyInvalidated)
	{
		const std::string directory = m_gameDirectory + "/baselines/static";
		const unsigned int generation = ReserveGeneration(directory, "static");
		if (generation == 0)
		{
			return;
		}
		m_staticGeneration = generation;
		if (!BuildAndWriteStatic())
		{
			m_staticGenerationWritten = false;
			return;
		}
	}
	StartSegment(ePlayer, iTurn);
}

void VoxRlCapture::OnCampaignSeam(PlayerTypes ePlayer)
{
	if (!IsObserving(ePlayer))
	{
		return;
	}
	if (gDLL == NULL || !gDLL->IsGameCoreThread())
	{
		return;
	}
	Segment& segment = *m_segment;
	if (segment.campaignGeneration != 0)
	{
		// Already bound through same-turn reuse.
		if (!segment.published)
		{
			PublishPendingFrames();
		}
		return;
	}
	if (!BuildAndWriteCampaign(ePlayer, segment.turn))
	{
		FailSegment("captureFailure");
		return;
	}
	PublishPendingFrames();
}

void VoxRlCapture::PublishPendingFrames()
{
	Segment& segment = *m_segment;
	if (segment.published || segment.failed)
	{
		return;
	}
	// The WORLD snapshot was captured at the pre-danger checkpoint. Write it
	// only now, after the campaign seam makes this segment publishable.
	if (!WriteWorldBaseline())
	{
		FailSegment("worldWrite");
		return;
	}
	// Dependencies must exist before the index publishes them.
	if (!VoxRlFileExists((m_gameDirectory + "/" + segment.staticRelPath).c_str()) ||
		!VoxRlFileExists((m_gameDirectory + "/" + segment.campaignRelPath).c_str()) ||
		!VoxRlFileExists((m_gameDirectory + "/" + segment.worldRelPath).c_str()))
	{
		FailSegment("captureFailure");
		return;
	}
	// Opening the stream and index creates the segment directory; until
	// this point the segment existed only in memory.
	if (!m_segmentStream.OpenNew(segment.streamPath.c_str()))
	{
		FailSegment("captureFailure");
		return;
	}
	if (!m_segmentIndex.OpenNew(segment.indexPath.c_str()))
	{
		FailSegment("captureFailure");
		return;
	}
	const std::string streamRelPath = segment.relativeSegmentDir + "/stream.bin";
	std::string line = "{\"type\":\"segment\",\"recordingVersion\":0,\"schemaVersion\":";
	{
		char number[32];
		sprintf_s(number, 32, "%u", static_cast<unsigned int>(VoxRlSchemaVersion));
		line += number;
	}
	line += ",\"manifestHash\":\"" + ManifestHashText() + "\"";
	line += ",\"producer\":{\"buildIdentifier\":";
	AppendJsonString(line, CURRENT_GAMECORE_VERSION);
	line += ",\"compiler\":\"msvc\",\"configuration\":\"dll\",\"localModifications\":\"untracked\"}";
	line += ",\"modConfig\":";
	{
		std::string modConfig = CURRENT_GAMECORE_VERSION;
		modConfig += ";IPC_CHANNEL=1";
		AppendJsonString(line, modConfig.c_str());
	}
	line += ",\"gameId\":\"" + std::string(m_gameUuidText) + "\"";
	{
		char number[32];
		sprintf_s(number, 32, "%d", static_cast<int>(segment.player));
		line += ",\"player\":" + std::string(number);
		sprintf_s(number, 32, "%d", segment.turn);
		line += ",\"turn\":" + std::string(number);
		sprintf_s(number, 32, "%u", segment.worldGeneration);
		line += ",\"worldGeneration\":" + std::string(number);
		sprintf_s(number, 32, "%u", segment.staticGeneration);
		line += ",\"staticGeneration\":" + std::string(number);
		sprintf_s(number, 32, "%u", segment.campaignGeneration);
		line += ",\"campaignGeneration\":" + std::string(number);
	}
	line += ",\"dependencies\":{";
	line += "\"static\":{\"path\":\"" + segment.staticRelPath + "\",\"blockKind\":1,\"generation\":" +
		NumberText(segment.staticGeneration) + ",\"framedLength\":\"" + NumberText(segment.staticFramedLength) + "\"}";
	line += ",\"campaign\":{\"path\":\"" + segment.campaignRelPath + "\",\"blockKind\":2,\"generation\":" +
		NumberText(segment.campaignGeneration) + ",\"framedLength\":\"" + NumberText(segment.campaignFramedLength) + "\"}";
	line += ",\"world\":{\"path\":\"" + segment.worldRelPath + "\",\"blockKind\":3,\"generation\":" +
		NumberText(segment.worldGeneration) + ",\"framedLength\":\"" + NumberText(segment.worldFramedLength) + "\"}";
	line += "},\"stream\":{\"path\":\"" + streamRelPath + "\"}";
	line += ",\"filter\":{\"enabled\":true,\"players\":";
	line += m_config.filterPlayers ? "\"filtered\"" : "\"all\"";
	line += ",\"turns\":";
	line += m_config.filterTurns ? "\"filtered\"" : "\"all\"";
	line += "}";
	line += ",\"checkpoint\":{\"kind\":\"postDiplomacy\",\"warReplacement\":false}}\n";
	if (!AppendIndexLine(line.c_str()))
	{
		FailSegment("captureFailure");
		return;
	}
	segment.published = true;
	// Move the pending frames into the stream, then publish them as the
	// first batch together with the segment record's dependencies.
	const unsigned int pendingBytes = static_cast<unsigned int>(segment.pendingBytes.size());
	bool pendingWritten = false;
	{
		ScopedTiming timing(m_config.timings, segment.timings.pendingPublishWriteNs);
		pendingWritten = m_segmentStream.Write(
			segment.pendingBytes.empty() ? NULL : &segment.pendingBytes[0], pendingBytes);
	}
	if (!pendingWritten)
	{
		FailSegment("captureFailure");
		return;
	}
	if (m_config.timings) segment.timings.pendingPublishWriteBytes += pendingBytes;
	m_segmentStreamBytes = pendingBytes;
	segment.pendingBytes.clear();
	segment.pendingCount = 0;
	CommitBatch(false, NULL);
}

std::string VoxRlCapture::NumberText(unsigned __int64 value)
{
	char buffer[24];
	sprintf_s(buffer, 24, "%I64u", value);
	return std::string(buffer);
}

void VoxRlCapture::CommitBatch(bool closureOnly, const char* closureReason)
{
	Segment& segment = *m_segment;
	if (!segment.published || segment.failed)
	{
		return;
	}
	// Frame lines for every frame appended since the previous commit.
	const unsigned int firstFrame = segment.committedFrameCount;
	std::string batch;
	for (unsigned int index = firstFrame; index < segment.frameTable.size(); ++index)
	{
		const VoxRlFrameEntry& frame = segment.frameTable[index];
		batch += "{\"type\":\"frame\",\"offset\":\"" + NumberText(frame.offset) +
			"\",\"framedLength\":\"" + NumberText(frame.length) +
			"\",\"blockKind\":" + NumberText(static_cast<unsigned int>(frame.blockKind)) +
			",\"requestSequence\":" + NumberText(frame.requestSequence) + ",\"decisionId\":";
		if (frame.decisionId == kVoxRlSyncOnlyDecisionId)
		{
			batch += "null";
		}
		else
		{
			batch += NumberText(frame.decisionId) + ",\"attemptIndex\":" + NumberText(static_cast<unsigned int>(frame.attemptIndex));
		}
		batch += "}\n";
	}
	// Decision lines reference request and result frames in this batch.
	const unsigned int firstDecision = segment.committedDecisionCount;
	for (unsigned int index = firstDecision; index < segment.decisions.size(); ++index)
	{
		const Segment::DecisionRow& row = segment.decisions[index];
		batch += "{\"type\":\"decision\",\"decisionId\":" + NumberText(row.decisionId) +
			",\"attemptIndex\":" + NumberText(static_cast<unsigned int>(row.attemptIndex)) +
			",\"caller\":" + NumberText(static_cast<unsigned int>(row.caller)) +
			",\"retryOutcome\":" + NumberText(static_cast<unsigned int>(row.retryOutcome)) +
			",\"worldGeneration\":" + NumberText(segment.worldGeneration) +
			",\"requestFrame\":" + NumberText(row.requestFrameIndex) +
			",\"resultFrame\":" + NumberText(row.resultFrameIndex) + ",\"previousAttempt\":";
		if (row.hasPrevious)
		{
			batch += "{\"attemptIndex\":" + NumberText(static_cast<unsigned int>(row.previousAttemptIndex)) +
				",\"worldGeneration\":" + NumberText(row.previousWorldGeneration) + "}";
		}
		else
		{
			batch += "null";
		}
		batch += "}\n";
	}
	// Coverage lines describe newly known omissions and are never rewritten.
	for (size_t index = 0; index < segment.coverageLines.size(); ++index)
	{
		batch += segment.coverageLines[index];
	}
	const unsigned int coverageCount = segment.committedCoverageCount +
		static_cast<unsigned int>(segment.coverageLines.size());
	segment.coverageLines.clear();

	std::string commit = "{\"type\":\"commit\",\"commitId\":" + NumberText(segment.commitId + 1) +
		",\"frameCount\":" + NumberText(segment.frameTable.size()) +
		",\"decisionCount\":" + NumberText(segment.decisions.size()) +
		",\"coverageCount\":" + NumberText(coverageCount) +
		",\"streamCommittedLength\":\"" + NumberText(m_segmentStreamBytes) + "\"" +
		",\"firstRequestSequence\":";
	if (segment.hasRequests)
	{
		commit += NumberText(segment.firstRequestSequence);
	}
	else
	{
		commit += "null";
	}
	commit += ",\"lastRequestSequence\":";
	if (segment.hasRequests)
	{
		commit += NumberText(segment.lastRequestSequence);
	}
	else
	{
		commit += "null";
	}
	commit += ",\"state\":\"";
	commit += closureOnly ? "closed" : "open";
	commit += "\",\"closureReason\":";
	if (closureOnly)
	{
		AppendJsonString(commit, closureReason != NULL ? closureReason : "turnComplete");
	}
	else
	{
		commit += "null";
	}
	commit += "}\n";

	bool appended = false;
	{
		ScopedTiming timing(m_config.timings, segment.timings.commitNs);
		// Ordinary commits append the batch and its commit line without disk
		// synchronization: completed writes stay readable through the operating
		// system's file cache, and durability is established only by the closure
		// sequence in CloseSegment.
		appended = AppendIndexLine(batch.c_str()) && AppendIndexLine(commit.c_str());
	}
	if (!appended)
	{
		FailSegment("captureFailure");
		return;
	}
	if (m_config.timings)
	{
		segment.timings.commitCount += 1;
		segment.timings.indexBytes += batch.size() + commit.size();
	}
	segment.commitId += 1;
	segment.committedFrameCount = static_cast<unsigned int>(segment.frameTable.size());
	segment.committedDecisionCount = static_cast<unsigned int>(segment.decisions.size());
	segment.committedCoverageCount = coverageCount;
	if (closureOnly)
	{
		segment.closed = true;
	}
}

bool VoxRlCapture::EnqueueFrame(const void* bytes, unsigned int length, int blockKind,
	unsigned int requestSequence, unsigned int decisionId, int attemptIndex)
{
	Segment& segment = *m_segment;
	if (segment.failed)
	{
		return false;
	}
	if (segment.published)
	{
		VoxRlFrameEntry entry;
		entry.offset = m_segmentStreamBytes;
		entry.length = length;
		entry.blockKind = blockKind;
		entry.requestSequence = requestSequence;
		entry.decisionId = decisionId;
		entry.attemptIndex = attemptIndex;
		bool written = false;
		{
			ScopedTiming timing(m_config.timings, segment.timings.streamWriteNs);
			written = m_segmentStream.Write(bytes, length);
		}
		if (!written)
		{
			FailSegment("captureFailure");
			return false;
		}
		if (m_config.timings) segment.timings.streamWriteBytes += length;
		m_segmentStreamBytes += length;
		segment.frameTable.push_back(entry);
		return true;
	}
	// Pre-publication frames wait in bounded memory until a compatible
	// CAMPAIGN permits publication.
	if (segment.pendingBytes.size() + length > kVoxRlPendingBudgetBytes)
	{
		FailSegment("budgetExceeded");
		return false;
	}
	VoxRlFrameEntry entry;
	entry.offset = static_cast<unsigned __int64>(segment.pendingBytes.size());
	entry.length = length;
	entry.blockKind = blockKind;
	entry.requestSequence = requestSequence;
	entry.decisionId = decisionId;
	entry.attemptIndex = attemptIndex;
	const unsigned char* cursor = static_cast<const unsigned char*>(bytes);
	{
		ScopedTiming timing(m_config.timings, segment.timings.pendingBufferNs);
		segment.pendingBytes.insert(segment.pendingBytes.end(), cursor, cursor + length);
	}
	if (m_config.timings) segment.timings.pendingBufferBytes += length;
	segment.pendingCount += 1;
	segment.frameTable.push_back(entry);
	return true;
}

void VoxRlCapture::ClearDirtyState()
{
	Segment& segment = *m_segment;
	segment.dirtyUnits.clear();
	segment.removedUnits.clear();
	segment.dirtyCities.clear();
	segment.removedCities.clear();
	segment.dirtyPlots.clear();
	segment.visibilityFlips.clear();
	segment.visibilityResets.clear();
	segment.revealedOverrideUpserts.clear();
	segment.removedRevealedOverrides.clear();
	segment.dirtyInterceptors.clear();
	segment.dirtyRelations.clear();
	segment.pendingDangerEvents.clear();
	segment.dirtyUnitModifiers.clear();
	segment.dirtyUnitPlagues.clear();
	segment.dirtyUnitBlockedPromotions.clear();
	segment.dirtyUnitAttackCounts.clear();
	segment.dirtyPlayerResistances.clear();
	segment.dirtyCityAttackCounts.clear();
}

unsigned int VoxRlCapture::UnitIterationIndex(PlayerTypes eOwner, int iUnitId)
{
	Segment& segment = *m_segment;
	const VoxRlEntityKey key(static_cast<int>(eOwner), iUnitId);
	std::map<VoxRlEntityKey, unsigned int>::const_iterator found = segment.unitIteration.find(key);
	if (found != segment.unitIteration.end())
	{
		return (*found).second;
	}
	// Assign a fresh owner-relative index for units created after the WORLD
	// build; uniqueness within the owner is what the loader enforces.
	unsigned int& next = segment.nextUnitIteration[static_cast<int>(eOwner)];
	const unsigned int index = next;
	next += 1;
	segment.unitIteration[key] = index;
	return index;
}

unsigned int VoxRlCapture::CityIterationIndex(PlayerTypes eOwner, int iCityId)
{
	Segment& segment = *m_segment;
	const VoxRlEntityKey key(static_cast<int>(eOwner), iCityId);
	std::map<VoxRlEntityKey, unsigned int>::const_iterator found = segment.cityIteration.find(key);
	if (found != segment.cityIteration.end())
	{
		return (*found).second;
	}
	unsigned int& next = segment.nextCityIteration[static_cast<int>(eOwner)];
	const unsigned int index = next;
	next += 1;
	segment.cityIteration[key] = index;
	return index;
}

void VoxRlCapture::SeedWorldIterationIndices()
{
	Segment& segment = *m_segment;
	segment.unitIteration.clear();
	segment.cityIteration.clear();
	segment.nextUnitIteration.clear();
	segment.nextCityIteration.clear();
	// These loops match the WORLD builder's owner-relative first/next order.
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		unsigned int iteration = 0;
		int loop = 0;
		for (CvUnit* unit = owner.firstUnit(&loop); unit != NULL; unit = owner.nextUnit(&loop))
		{
			if (unit->isDelayedDeath() || unit->plot() == NULL) continue;
			segment.unitIteration[VoxRlEntityKey(player, unit->GetID())] = iteration;
			++iteration;
		}
		segment.nextUnitIteration[player] = iteration;
		iteration = 0;
		loop = 0;
		for (CvCity* city = owner.firstCity(&loop); city != NULL; city = owner.nextCity(&loop))
		{
			segment.cityIteration[VoxRlEntityKey(player, city->GetID())] = iteration;
			++iteration;
		}
		segment.nextCityIteration[player] = iteration;
	}
}

bool VoxRlCapture::CollectDelta(VoxRlRequestData& data)
{
	Segment& segment = *m_segment;
	ScopedTiming timing(m_config.timings, segment.timings.deltaCollectNs);
	const PlayerTypes capturingPlayer = segment.player;
	CvPlayerAI& capturing = GET_PLAYER(capturingPlayer);
	const TeamTypes capturingTeam = capturing.getTeam();
	CvTacticalAnalysisMap* zoneMap = capturing.GetTacticalAI()->GetTacticalAnalysisMap();
	CvMap& map = GC.getMap();
	const int plotCount = map.numPlots();
	// The zone table is recollected only after a native rebuild marked it dirty.
	// The comparison stays because the native rebuild does not identify changed
	// plots: it accepts the rebuilt table, carries a replacement when the rows
	// differ, and records each plot whose assignment changed. While the marker
	// is clean, the table and every assignment are known unchanged, so the
	// whole pass is skipped.
	std::vector<int> zoneChangedPlots;
	if (m_zoneSnapshotDirty)
	{
		VoxRlZoneSnapshot zones;
		if (!VoxRlCollectZones(zoneMap, zones) || zones.plotZones.size() != segment.zoneSnapshot.plotZones.size())
			return false;
		if (!SameRows(zones.zones, segment.zoneSnapshot.zones) ||
			!SameRows(zones.neighbors, segment.zoneSnapshot.neighbors))
		{
			data.requestHeader.hasZoneReplacement = 1;
			VoxRlMirrorSparseRows(zones.zones, data.requestZoneReplacements);
			VoxRlMirrorSparseRows(zones.neighbors, data.requestZoneNeighbors);
			// The decode table is rebuilt only when a new table is accepted.
			segment.zoneRowByZoneId.clear();
			for (size_t zoneIndex = 0; zoneIndex < zones.zones.size(); ++zoneIndex)
				segment.zoneRowByZoneId[zones.zones[zoneIndex].zoneId] = static_cast<unsigned int>(zoneIndex + 1);
		}
		for (int plot = 0; plot < plotCount; ++plot)
		{
			if (zones.plotZones[plot] != segment.zoneSnapshot.plotZones[plot])
			{
				segment.dirtyPlots.insert(plot);
				zoneChangedPlots.push_back(plot);
			}
		}
		segment.zoneSnapshot.zones.swap(zones.zones);
		segment.zoneSnapshot.neighbors.swap(zones.neighbors);
		segment.zoneSnapshot.plotZones.swap(zones.plotZones);
		m_zoneSnapshotDirty = false;
	}
	// The retained table is exactly the table this request's assignments decode
	// against: the replacement when one is carried, and otherwise the previously
	// accepted table. One-based indices address its ordered rows, and the 16-bit
	// form covers up to 65,535 rows before the wide form takes over.
	const bool wideZoneIndices = segment.zoneSnapshot.zones.size() > 65535U;

	// Removed units come first so an upsert of the same key is refused.
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.removedUnits.begin();
		key != segment.removedUnits.end(); ++key)
	{
		RequestRemovedUnitRecord row;
		std::memset(&row, 0, sizeof(row));
		row.owner = static_cast<i8>((*key).owner);
		row.unitId = static_cast<i32>((*key).id);
		data.requestRemovedUnits.push_back(row);
	}
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.removedCities.begin();
		key != segment.removedCities.end(); ++key)
	{
		RequestRemovedCityRecord row;
		std::memset(&row, 0, sizeof(row));
		row.owner = static_cast<i8>((*key).owner);
		row.cityId = static_cast<i32>((*key).id);
		data.requestRemovedCities.push_back(row);
	}

	// The dirty set deduplicates notifications, so each live dirty unit
	// emits one complete record per flush.
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyUnits.begin();
		key != segment.dirtyUnits.end(); ++key)
	{
		CvUnit* pUnit = GET_PLAYER(static_cast<PlayerTypes>((*key).owner)).getUnit((*key).id);
		if (pUnit == NULL)
		{
			// The unit died between the mark and the flush; its removal is
			// tracked separately, so nothing upserts here.
			continue;
		}
		if (pUnit->plot() == NULL || pUnit->isDelayedDeath())
		{
			continue;
		}
		UnitRecord record;
		std::memset(&record, 0, sizeof(record));
		std::vector<UnitMovementCountRecord> movementCounts;
		if (!VoxRlCollectUnitRecord(*pUnit, capturingTeam, record, movementCounts))
		{
			return false;
		}
		record.iterationIndex = UnitIterationIndex(static_cast<PlayerTypes>((*key).owner), (*key).id);
		RequestDeltaUnitRecord delta;
		std::memcpy(&delta, &record, sizeof(record));
		// Each delta carries an independent range into the request-local section; the
		// mirrored entries follow delta-row order and an empty range clears both tables.
		std::vector<RequestUnitMovementCountRecord> requestCounts;
		VoxRlMirrorSparseRows(movementCounts, requestCounts);
		if (!AppendRequestDeltaUnitRecordMovementCountRange(&delta, &data, requestCounts)) return false;
		data.requestDeltaUnits.push_back(delta);
	}

	// Physical plot deltas carry only the dense core fields; zone membership travels
	// in its own assignment rows driven by the native zone comparison. The dedupe map
	// retains the last emitted core row.
	for (std::set<int>::const_iterator plotIndex = segment.dirtyPlots.begin();
		plotIndex != segment.dirtyPlots.end(); ++plotIndex)
	{
		if (*plotIndex < 0 || *plotIndex >= plotCount) continue;
		CvPlot* plot = map.plotByIndex(*plotIndex);
		if (plot == NULL) continue;
		PlotCoreRecord record;
		std::memset(&record, 0, sizeof(record));
		if (!CollectPlotCoreRecord(*plot, record))
		{
			return false;
		}
		RequestDeltaPlotCoreRecord delta;
		std::memset(&delta, 0, sizeof(delta));
		delta.plotIndex = static_cast<i32>(*plotIndex);
		delta.effectiveOwningCityId = record.effectiveOwningCityId;
		delta.improvementType = record.improvementType;
		delta.resourceType = record.resourceType;
		delta.reconCount = record.reconCount;
		delta.extraMovePathCost = record.extraMovePathCost;
		delta.unitIncrement = record.unitIncrement;
		delta.owningCityId = record.owningCityId;
		delta.effectiveOwningCityOwner = record.effectiveOwningCityOwner;
		delta.beingWorked = record.beingWorked;
		delta.owner = record.owner;
		delta.featureType = record.featureType;
		delta.improvementPillaged = record.improvementPillaged;
		delta.improvementPassable = record.improvementPassable;
		delta.routeType = record.routeType;
		delta.routePillaged = record.routePillaged;
		delta.restoreMoves = record.restoreMoves;
		delta.freeMoveAcross = record.freeMoveAcross;
		delta.owningCityOwner = record.owningCityOwner;
		const VoxRlEntityKey plotKey(-2, *plotIndex);
		std::map<VoxRlEntityKey, RequestDeltaPlotCoreRecord>::const_iterator last =
			segment.lastPlotRows.find(plotKey);
		if (last != segment.lastPlotRows.end() && std::memcmp(&last->second, &delta, sizeof(delta)) == 0)
		{
			continue;
		}
		segment.lastPlotRows[plotKey] = delta;
		data.requestDeltaPlots.push_back(delta);
	}

	// Zone assignment deltas carry every plot whose native zone id changed, encoded
	// against the applicable table. Both widths stay present; exactly one carries rows.
	for (std::vector<int>::const_iterator plotIndex = zoneChangedPlots.begin();
		plotIndex != zoneChangedPlots.end(); ++plotIndex)
	{
		if (*plotIndex < 0 || *plotIndex >= plotCount) return false;
		const i32 plotZone = segment.zoneSnapshot.plotZones[*plotIndex];
		unsigned int zoneTableIndex = 0;
		if (plotZone != -1)
		{
			std::map<int, unsigned int>::const_iterator zoneRow = segment.zoneRowByZoneId.find(plotZone);
			if (zoneRow == segment.zoneRowByZoneId.end()) return false;
			zoneTableIndex = zoneRow->second;
		}
		if (wideZoneIndices)
		{
			RequestDeltaPlotZoneWideRecord row;
			std::memset(&row, 0, sizeof(row));
			row.plotIndex = static_cast<i32>(*plotIndex);
			row.zoneTableIndex = zoneTableIndex;
			data.requestDeltaPlotZonesWide.push_back(row);
		}
		else
		{
			RequestDeltaPlotZoneRecord row;
			std::memset(&row, 0, sizeof(row));
			row.plotIndex = static_cast<i32>(*plotIndex);
			row.zoneTableIndex = static_cast<u16>(zoneTableIndex);
			data.requestDeltaPlotZones.push_back(row);
		}
	}

	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyCities.begin();
		key != segment.dirtyCities.end(); ++key)
	{
		CvCity* pCity = GET_PLAYER(static_cast<PlayerTypes>((*key).owner)).getCity((*key).id);
		if (pCity == NULL)
		{
			continue;
		}
		CityRecord record;
		std::memset(&record, 0, sizeof(record));
		if (!VoxRlCollectCityRecord(*pCity, capturingPlayer, record))
		{
			return false;
		}
		record.iterationIndex = CityIterationIndex(static_cast<PlayerTypes>((*key).owner), (*key).id);
		RequestDeltaCityRecord delta;
		std::memcpy(&delta, &record, sizeof(record));
		std::map<VoxRlEntityKey, RequestDeltaCityRecord>::const_iterator last =
			segment.lastCityRows.find(*key);
		if (last != segment.lastCityRows.end() && std::memcmp(&last->second, &delta, sizeof(delta)) == 0)
		{
			continue;
		}
		segment.lastCityRows[*key] = delta;
		data.requestDeltaCities.push_back(delta);
	}

	// The small team passability table is recollected only after a technology
	// ownership change marked it dirty; WORLD builds collect it in full.
	if (m_teamPassabilityDirty)
	{
		std::vector<TeamPassabilityRecord> teamPassability;
		if (!VoxRlCollectTeamPassabilityRows(teamPassability)) return false;
		for (size_t index = 0; index < teamPassability.size(); ++index)
		{
			const TeamPassabilityRecord& record = teamPassability[index];
			std::map<int, TeamPassabilityRecord>::const_iterator last =
				segment.lastTeamPassabilityRows.find(static_cast<int>(record.team));
			if (last != segment.lastTeamPassabilityRows.end() &&
				std::memcmp(&last->second, &record, sizeof(record)) == 0) continue;
			RequestTeamPassabilityRecord row;
			std::memcpy(&row, &record, sizeof(row));
			segment.lastTeamPassabilityRows[static_cast<int>(record.team)] = record;
			data.requestTeamPassability.push_back(row);
		}
		m_teamPassabilityDirty = false;
	}

	// Known-visibility resets travel before the ordinary flips of the same
	// request; the reader clears each named team's complete bitset first.
	for (std::set<int>::const_iterator team = segment.visibilityResets.begin();
		team != segment.visibilityResets.end(); ++team)
	{
		RequestVisibilityResetRecord row;
		std::memset(&row, 0, sizeof(row));
		row.team = static_cast<i8>(*team);
		data.requestVisibilityResets.push_back(row);
	}
	for (std::map<VoxRlVisibilityKey, unsigned char>::const_iterator flip = segment.visibilityFlips.begin();
		flip != segment.visibilityFlips.end(); ++flip)
	{
		RequestVisibilityFlipRecord row;
		std::memset(&row, 0, sizeof(row));
		row.team = static_cast<i8>((*flip).first.team);
		row.plotIndex = static_cast<i32>((*flip).first.plotIndex);
		row.bitsetKind = static_cast<u8>((*flip).first.kind);
		row.value = (*flip).second != 0 ? 1 : 0;
		data.requestVisibilityFlips.push_back(row);
	}

	for (std::map<VoxRlEntityKey, unsigned char>::const_iterator upsert = segment.revealedOverrideUpserts.begin();
		upsert != segment.revealedOverrideUpserts.end(); ++upsert)
	{
		if ((*upsert).second == 0) continue;
		if ((*upsert).first.id < 0 || (*upsert).first.id >= plotCount) continue;
		CvPlot* plot = map.plotByIndex((*upsert).first.id);
		if (plot == NULL) continue;
		RevealedOverrideRecord record;
		std::memset(&record, 0, sizeof(record));
		CollectRevealedOverrideRecord(*plot, static_cast<TeamTypes>((*upsert).first.owner), record);
		RequestRevealedOverrideUpsertRecord row;
		std::memcpy(&row, &record, sizeof(record));
		row.plotIndex = static_cast<i32>((*upsert).first.id);
		row.team = static_cast<i8>((*upsert).first.owner);
		data.requestRevealedOverrideUpserts.push_back(row);
	}
	for (std::set<VoxRlEntityKey>::const_iterator removal = segment.removedRevealedOverrides.begin();
		removal != segment.removedRevealedOverrides.end(); ++removal)
	{
		RequestRemovedRevealedOverrideRecord row;
		std::memset(&row, 0, sizeof(row));
		row.team = static_cast<i8>((*removal).owner);
		row.plotIndex = static_cast<i32>((*removal).id);
		data.requestRemovedRevealedOverrides.push_back(row);
	}

	// Interceptor replacements carry each named player's complete cache in
	// native vector order.
	unsigned int entryCursor = 0;
	for (std::set<int>::const_iterator player = segment.dirtyInterceptors.begin();
		player != segment.dirtyInterceptors.end(); ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(*player));
		const std::vector<std::pair<int, int> >& interceptors = owner.GetPossibleInterceptors();
		RequestInterceptorReplacementRecord replacement;
		std::memset(&replacement, 0, sizeof(replacement));
		replacement.player = static_cast<i8>(*player);
		data.requestInterceptorReplacements.push_back(replacement);
		for (size_t index = 0; index < interceptors.size(); ++index)
		{
			RequestInterceptorEntryRecord row;
			std::memset(&row, 0, sizeof(row));
			row.owner = static_cast<i8>(*player);
			row.unitId = static_cast<i32>(interceptors[index].first);
			row.plotIndex = static_cast<i32>(interceptors[index].second);
			data.requestInterceptorEntries.push_back(row);
		}
		entryCursor += static_cast<unsigned int>(interceptors.size());
	}

	// Directed relation replacements from the complete captured matrix.
	for (std::map<VoxRlEntityKey, unsigned char>::const_iterator relation = segment.dirtyRelations.begin();
		relation != segment.dirtyRelations.end(); ++relation)
	{
		if ((*relation).second == 0) continue;
		TeamRelationRecord record;
		std::memset(&record, 0, sizeof(record));
		CollectTeamRelationRecord(GET_TEAM(static_cast<TeamTypes>((*relation).first.owner)),
			static_cast<TeamTypes>((*relation).first.id), capturingPlayer, record);
		RequestTeamRelationRecord row;
		std::memcpy(&row, &record, sizeof(record));
		row.team = static_cast<i8>((*relation).first.owner);
		row.otherTeam = static_cast<i8>((*relation).first.id);
		data.requestTeamRelations.push_back(row);
	}

	// Sparse family replacements are entity-keyed: each replacement names one
	// entity and carries that entity's complete current rows for its family.
	// Only dirty entities are recollected; an absent replacement leaves that
	// entity's rows unchanged.
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyUnitModifiers.begin();
		key != segment.dirtyUnitModifiers.end(); ++key)
	{
		CvUnit* pUnit = GET_PLAYER(static_cast<PlayerTypes>((*key).owner)).getUnit((*key).id);
		if (pUnit == NULL || pUnit->plot() == NULL || pUnit->isDelayedDeath())
		{
			// The unit died between the mark and the flush; its removal row
			// removes the associated sparse rows as well.
			continue;
		}
		std::vector<UnitModifierRecord> rows;
		if (!VoxRlCollectUnitModifierRows(static_cast<PlayerTypes>((*key).owner), (*key).id, pUnit, rows))
		{
			return false;
		}
		RequestUnitModifierReplacementRecord replacement;
		std::memset(&replacement, 0, sizeof(replacement));
		replacement.owner = static_cast<i8>((*key).owner);
		replacement.unitId = static_cast<i32>((*key).id);
		std::vector<RequestUnitModifierRowRecord> requestRows;
		requestRows.resize(rows.size());
		for (size_t index = 0; index < rows.size(); ++index)
		{
			requestRows[index].family = rows[index].family;
			requestRows[index].index = rows[index].index;
			requestRows[index].value = rows[index].value;
		}
		if (!AppendRequestUnitModifierReplacementRecordRowRange(&replacement, &data, requestRows)) return false;
		data.requestUnitModifierReplacements.push_back(replacement);
	}
	// Plagues and blocked promotions come from one native pass per unit, so the two
	// families share a union walk and each emits only the replacements it marked.
	std::set<VoxRlEntityKey> plagueOrBlocked;
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyUnitPlagues.begin();
		key != segment.dirtyUnitPlagues.end(); ++key) plagueOrBlocked.insert(*key);
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyUnitBlockedPromotions.begin();
		key != segment.dirtyUnitBlockedPromotions.end(); ++key) plagueOrBlocked.insert(*key);
	for (std::set<VoxRlEntityKey>::const_iterator key = plagueOrBlocked.begin();
		key != plagueOrBlocked.end(); ++key)
	{
		CvUnit* pUnit = GET_PLAYER(static_cast<PlayerTypes>((*key).owner)).getUnit((*key).id);
		if (pUnit == NULL || pUnit->plot() == NULL || pUnit->isDelayedDeath()) continue;
		std::vector<UnitPlagueRecord> plagues;
		std::vector<UnitBlockedPromotionRecord> blocked;
		VoxRlCollectUnitPlagueRows(static_cast<PlayerTypes>((*key).owner), (*key).id, pUnit, plagues, blocked);
		if (segment.dirtyUnitPlagues.count(*key) != 0)
		{
			RequestUnitPlagueReplacementRecord replacement;
			std::memset(&replacement, 0, sizeof(replacement));
			replacement.owner = static_cast<i8>((*key).owner);
			replacement.unitId = static_cast<i32>((*key).id);
			std::vector<RequestUnitPlagueRowRecord> requestRows;
			requestRows.resize(plagues.size());
			for (size_t index = 0; index < plagues.size(); ++index)
			{
				requestRows[index].plague = plagues[index].plague;
				requestRows[index].domain = plagues[index].domain;
				requestRows[index].applyOnAttack = plagues[index].applyOnAttack;
				requestRows[index].applyOnDefense = plagues[index].applyOnDefense;
				requestRows[index].applyChance = plagues[index].applyChance;
			}
			if (!AppendRequestUnitPlagueReplacementRecordRowRange(&replacement, &data, requestRows)) return false;
			data.requestUnitPlagueReplacements.push_back(replacement);
		}
		if (segment.dirtyUnitBlockedPromotions.count(*key) != 0)
		{
			RequestUnitBlockedPromotionReplacementRecord replacement;
			std::memset(&replacement, 0, sizeof(replacement));
			replacement.owner = static_cast<i8>((*key).owner);
			replacement.unitId = static_cast<i32>((*key).id);
			std::vector<RequestUnitBlockedPromotionRowRecord> requestRows;
			requestRows.resize(blocked.size());
			for (size_t index = 0; index < blocked.size(); ++index)
			{
				requestRows[index].promotion = blocked[index].promotion;
			}
			if (!AppendRequestUnitBlockedPromotionReplacementRecordRowRange(&replacement, &data, requestRows)) return false;
			data.requestUnitBlockedPromotionReplacements.push_back(replacement);
		}
	}
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyUnitAttackCounts.begin();
		key != segment.dirtyUnitAttackCounts.end(); ++key)
	{
		CvUnit* pUnit = GET_PLAYER(static_cast<PlayerTypes>((*key).owner)).getUnit((*key).id);
		if (pUnit == NULL || pUnit->plot() == NULL || pUnit->isDelayedDeath()) continue;
		std::vector<UnitAttackCountRecord> rows;
		VoxRlCollectUnitAttackCountRows(static_cast<PlayerTypes>((*key).owner), (*key).id, pUnit, rows);
		RequestUnitAttackCountReplacementRecord replacement;
		std::memset(&replacement, 0, sizeof(replacement));
		replacement.owner = static_cast<i8>((*key).owner);
		replacement.unitId = static_cast<i32>((*key).id);
		std::vector<RequestUnitAttackCountRowRecord> requestRows;
		requestRows.resize(rows.size());
		for (size_t index = 0; index < rows.size(); ++index)
		{
			requestRows[index].attackingPlayer = rows[index].attackingPlayer;
			requestRows[index].count = rows[index].count;
		}
		if (!AppendRequestUnitAttackCountReplacementRecordRowRange(&replacement, &data, requestRows)) return false;
		data.requestUnitAttackCountReplacements.push_back(replacement);
	}
	for (std::set<int>::const_iterator player = segment.dirtyPlayerResistances.begin();
		player != segment.dirtyPlayerResistances.end(); ++player)
	{
		std::vector<PlayerResistanceRecord> rows;
		VoxRlCollectPlayerResistanceRows(&GET_PLAYER(static_cast<PlayerTypes>(*player)), rows);
		RequestPlayerResistanceReplacementRecord replacement;
		std::memset(&replacement, 0, sizeof(replacement));
		replacement.player = static_cast<i8>(*player);
		std::vector<RequestPlayerResistanceRowRecord> requestRows;
		requestRows.resize(rows.size());
		for (size_t index = 0; index < rows.size(); ++index)
		{
			requestRows[index].opponent = rows[index].opponent;
			requestRows[index].dominationResistance = rows[index].dominationResistance;
		}
		if (!AppendRequestPlayerResistanceReplacementRecordRowRange(&replacement, &data, requestRows)) return false;
		data.requestPlayerResistanceReplacements.push_back(replacement);
	}
	for (std::set<VoxRlEntityKey>::const_iterator key = segment.dirtyCityAttackCounts.begin();
		key != segment.dirtyCityAttackCounts.end(); ++key)
	{
		CvCity* pCity = GET_PLAYER(static_cast<PlayerTypes>((*key).owner)).getCity((*key).id);
		if (pCity == NULL) continue;
		std::vector<CityAttackCountRecord> rows;
		VoxRlCollectCityAttackCountRows(pCity, rows);
		RequestCityAttackCountReplacementRecord replacement;
		std::memset(&replacement, 0, sizeof(replacement));
		replacement.cityOwner = static_cast<i8>((*key).owner);
		replacement.cityId = static_cast<i32>((*key).id);
		std::vector<RequestCityAttackCountRowRecord> requestRows;
		requestRows.resize(rows.size());
		for (size_t index = 0; index < rows.size(); ++index)
		{
			requestRows[index].attackingPlayer = rows[index].attackingPlayer;
			requestRows[index].count = rows[index].count;
		}
		if (!AppendRequestCityAttackCountReplacementRecordRowRange(&replacement, &data, requestRows)) return false;
		data.requestCityAttackCountReplacements.push_back(replacement);
	}

	// Pending danger events carry their event-time board in this request.
	data.requestDangerEvents = segment.pendingDangerEvents;
	if (m_config.timings)
	{
		segment.timings.deltaCollectCount += 1;
		segment.timings.deltaRows += CountRequestRows(data);
	}
	return true;
}

bool VoxRlCapture::EmitStagedRequest()
{
	Segment& segment = *m_segment;
	if (segment.failed)
	{
		return false;
	}
	VoxRlRequestData& data = segment.staged;
	VoxRlBlockIdentity identity;
	identity.session = m_gameUuid;
	identity.turn = segment.turn;
	identity.player = static_cast<int>(segment.player);
	identity.staticGeneration = segment.staticGeneration;
	identity.worldGeneration = segment.worldGeneration;
	identity.campaignGeneration = segment.campaignGeneration;
	identity.deltaSequence = segment.nextDeltaSequence;
	identity.decisionId = kVoxRlSyncOnlyDecisionId;

	VoxRlOwnedBlockStorage storage;
	unsigned int length = 0;
	bool built = false;
	{
		ScopedTiming timing(m_config.timings, segment.timings.requestBuildNs);
		built = VoxRlBuildRequestBlock(identity, data, storage, length);
	}
	if (!built)
	{
		FailSegment("captureFailure");
		return false;
	}
	if (m_config.timings)
	{
		segment.timings.requestCount += 1;
		segment.timings.requestBytes += length;
	}
	if (!EnqueueFrame(storage.Bytes(), length, VOX_RL_BLOCK_REQUEST,
		segment.nextDeltaSequence, kVoxRlSyncOnlyDecisionId, -1))
	{
		return false;
	}
	if (!segment.hasRequests)
	{
		segment.hasRequests = true;
		segment.firstRequestSequence = segment.nextDeltaSequence;
	}
	segment.lastRequestSequence = segment.nextDeltaSequence;
	segment.nextDeltaSequence += 1;
	ClearDirtyState();
	segment.staged = VoxRlRequestData();
	segment.stagingRequest = false;
	if (segment.published)
	{
		CommitBatch(false, NULL);
	}
	return true;
}

// Stages the board before the complete native danger refresh.
void VoxRlCapture::OnDangerRefreshBegin(const CvDangerPlots& danger)
{
	if (m_searchActive)
	{
		// A decision request/result pair is in flight. Replay reexecutes its
		// native search from the recorded input, so do not publish a separate
		// synchronization request or replace the segment here.
		return;
	}
	if (m_segment == NULL || m_segment->failed)
	{
		return;
	}
	if (gDLL == NULL || !gDLL->IsGameCoreThread())
	{
		return;
	}
	if (danger.GetObserver() != m_segment->player)
	{
		return;
	}
	// The load path rebuilds from a save on the UI thread; ignore it.
	if (danger.GetTurnBuilt() == -1)
	{
		return;
	}
	if (m_segment->stagingRequest)
	{
		return;
	}
	// A pending war replacement builds the new WORLD at this actual
	// pre-refresh checkpoint, before the refresh consumes the board.
	if (m_worldReplacementPending)
	{
		m_worldReplacementPending = false;
		const PlayerTypes player = m_segment->player;
		const int turn = m_segment->turn;
		CloseSegment("worldReplacement");
		StartSegment(player, turn);
		if (m_segment == NULL || m_segment->failed)
		{
			return;
		}
		AddCoverageOmission("warReplacement");
	}
	// Stage the event-time board with the pending dirty event, then add
	// this refresh event. The delta collects directly into the segment's
	// staged request, so the staged vectors are transferred, not copied.
	if (!CollectDelta(m_segment->staged))
	{
		FailSegment("captureFailure");
		return;
	}
	RequestDangerEventRecord event;
	std::memset(&event, 0, sizeof(event));
	event.kind = 2;
	event.observer = static_cast<i8>(m_segment->player);
	event.owner = static_cast<i8>(NO_PLAYER);
	event.unitId = -1;
	event.turn = 0;
	m_segment->staged.requestDangerEvents.push_back(event);
	m_segment->stagingRequest = true;
}

// Publishes the staged inputs after all native refresh passes finish.
void VoxRlCapture::OnDangerRefreshComplete(const CvDangerPlots& danger)
{
	if (m_searchActive)
	{
		return;
	}
	if (m_segment == NULL || m_segment->failed || !m_segment->stagingRequest)
	{
		return;
	}
	if (gDLL == NULL || !gDLL->IsGameCoreThread())
	{
		return;
	}
	if (danger.GetObserver() != m_segment->player)
	{
		return;
	}
	// The refresh is complete; publish its synchronization request.
	EmitStagedRequest();
}

// Stages a discovery after native eligibility and duplicate checks pass.
void VoxRlCapture::OnDangerDiscoveryBegin(const CvDangerPlots& danger, const CvUnit* pUnit)
{
	if (m_segment == NULL || m_segment->failed || m_segment->stagingRequest)
	{
		return;
	}
	if (gDLL == NULL || !gDLL->IsGameCoreThread())
	{
		return;
	}
	if (danger.GetObserver() != m_segment->player)
	{
		return;
	}
	// Stage the event-time board the discovery consumes. The delta collects
	// directly into the segment's staged request, so the staged vectors are
	// transferred, not copied.
	if (!CollectDelta(m_segment->staged))
	{
		FailSegment("captureFailure");
		return;
	}
	RequestKnownAttackerRecord row;
	std::memset(&row, 0, sizeof(row));
	row.observer = static_cast<i8>(m_segment->player);
	row.owner = static_cast<i8>(pUnit->getOwner());
	row.unitId = static_cast<i32>(pUnit->GetID());
	m_segment->staged.requestKnownAttackers.push_back(row);
	m_segment->stagingRequest = true;
}

// Publishes inputs for the successful native discovery.
void VoxRlCapture::OnDangerDiscoveryEnd(const CvDangerPlots& danger)
{
	if (m_segment == NULL || m_segment->failed || !m_segment->stagingRequest)
	{
		return;
	}
	if (gDLL == NULL || !gDLL->IsGameCoreThread())
	{
		return;
	}
	if (danger.GetObserver() != m_segment->player)
	{
		return;
	}
	EmitStagedRequest();
}

void VoxRlCapture::NoteDangerDirty(PlayerTypes eObserver)
{
	if (!IsObserving(eObserver))
	{
		return;
	}
	Segment& segment = *m_segment;
	RequestDangerEventRecord event;
	std::memset(&event, 0, sizeof(event));
	event.kind = 1;
	event.observer = static_cast<i8>(eObserver);
	event.owner = static_cast<i8>(NO_PLAYER);
	event.unitId = -1;
	event.turn = 0;
	segment.pendingDangerEvents.push_back(event);
}

void VoxRlCapture::BeginEngagement(int callerType, PlayerTypes ePlayer)
{
	if (m_concluded)
	{
		return;
	}
	if (!ResolveConfiguration())
	{
		return;
	}
	if (m_engagement == NULL)
	{
		m_engagement = new Engagement();
	}
	if (m_engagement->active)
	{
		return;
	}
	m_engagement->active = true;
	m_engagement->callerType = callerType;
	m_engagement->player = ePlayer;
	// Decision identifiers are monotonic within a game attachment across
	// both caller paths, starting at one; zero stays synchronization-only.
	m_decisionIdCounter += 1;
	m_engagement->decisionId = m_decisionIdCounter;
	m_engagement->attemptIndex = 0;
	m_engagement->lastOutcome = VOX_RL_RETRY_FIRST;
	m_engagement->lastAttemptWorldGeneration = kVoxRlAbsentGeneration;
	m_engagement->lastSearchEmpty = false;
}

void VoxRlCapture::EndEngagement()
{
	if (m_engagement != NULL)
	{
		m_engagement->active = false;
	}
}

void VoxRlCapture::NoteExecutionResult(bool bSuccess)
{
	if (m_engagement == NULL || !m_engagement->active)
	{
		return;
	}
	if (!m_engagement->lastSearchEmpty && !bSuccess)
	{
		m_engagement->lastOutcome = VOX_RL_RETRY_PREVIOUS_EXECUTION_FAILED;
	}
}

std::vector<STacticalAssignment> VoxRlCapture::RunNativeSearch(const std::vector<CvUnit*>& vUnits,
	CvPlot* pTarget, int eAggression, std::set<int>& unuseableUnits,
	bool bTargetDistanceRelevant, bool bReturnToStartPositions, int iSaveMovement)
{
	return TacticalAIHelpers::FindBestUnitAssignments(vUnits, pTarget,
		static_cast<eAggressionLevel>(eAggression), unuseableUnits, bTargetDistanceRelevant,
		bReturnToStartPositions, iSaveMovement);
}

std::vector<STacticalAssignment> VoxRlCapture::SearchAssignments(int callerType, PlayerTypes ePlayer,
	const std::vector<CvUnit*>& vUnits, CvPlot* pTarget, int eAggression,
	std::set<int>& unuseableUnits, bool bTargetDistanceRelevant,
	bool bReturnToStartPositions, int iSaveMovement)
{
	std::vector<STacticalAssignment> results;
	const bool ephemeral = m_engagement == NULL || !m_engagement->active || m_engagement->player != ePlayer;
	if (ephemeral)
	{
		// The opportunity caller runs single-attempt engagements of its own.
		BeginEngagement(callerType, ePlayer);
	}
	const bool capture = !m_shuttingDown && !m_concluded && IsObserving(ePlayer) && pTarget != NULL &&
		m_engagement != NULL && m_engagement->active && m_engagement->player == ePlayer;
	if (capture)
	{
		RunCapturedSearch(callerType, ePlayer, vUnits, pTarget, eAggression, unuseableUnits,
			bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement, results);
	}
	else
	{
		results = RunNativeSearch(vUnits, pTarget, eAggression, unuseableUnits,
			bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement);
	}
	if (ephemeral)
	{
		EndEngagement();
	}
	return results;
}

void VoxRlCapture::RunCapturedSearch(int callerType, PlayerTypes ePlayer,
	const std::vector<CvUnit*>& vUnits, CvPlot* pTarget, int eAggression,
	std::set<int>& unuseableUnits, bool bTargetDistanceRelevant,
	bool bReturnToStartPositions, int iSaveMovement,
	std::vector<STacticalAssignment>& results)
{
	Segment& segment = *m_segment;
	Engagement& engagement = *m_engagement;
	// 1. Carry pending board changes and danger dirty events in the decision
	// request alongside its caller context. Refreshes and discoveries publish
	// their event-time inputs at their own native boundaries.
	VoxRlRequestData data;
	if (!CollectDelta(data))
	{
		FailSegment("captureFailure");
		results = RunNativeSearch(vUnits, pTarget, eAggression, unuseableUnits,
			bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement);
		return;
	}
	data.requestHeader.callerType = static_cast<i32>(callerType);
	data.requestHeader.attemptIndex = static_cast<i32>(engagement.attemptIndex);
	data.requestHeader.retryOutcome = static_cast<i32>(engagement.lastOutcome);
	// The resolved intent field stays absent until a later policy bridge
	// defines the mapping.
	data.requestHeader.resolvedIntent = 0;
	data.requestHeader.previousAttemptWorldGeneration = engagement.attemptIndex == 0
		? kVoxRlAbsentGeneration : engagement.lastAttemptWorldGeneration;
	data.requestHeader.targetPlotIndex = pTarget != NULL ? static_cast<i32>(pTarget->GetPlotIndex()) : static_cast<i32>(kVoxRlAbsentPlotIndex);
	data.requestHeader.saveMovement = static_cast<i32>(iSaveMovement);
	data.requestHeader.aggressionLevel = static_cast<u8>(eAggression);
	data.requestHeader.targetDistanceRelevant = bTargetDistanceRelevant ? 1 : 0;
	data.requestHeader.returnToStartPositions = bReturnToStartPositions ? 1 : 0;
	for (size_t index = 0; index < vUnits.size(); ++index)
	{
		RequestParticipantRecord row;
		std::memset(&row, 0, sizeof(row));
		row.owner = static_cast<i8>(ePlayer);
		if (vUnits[index] != NULL)
		{
			row.unitId = static_cast<i32>(vUnits[index]->GetID());
		}
		else
		{
			row.unitId = -1;
		}
		data.requestParticipants.push_back(row);
	}
	for (std::set<int>::const_iterator unit = unuseableUnits.begin(); unit != unuseableUnits.end(); ++unit)
	{
		RequestDroppedUnitRecord row;
		std::memset(&row, 0, sizeof(row));
		row.owner = static_cast<i8>(ePlayer);
		row.unitId = static_cast<i32>(*unit);
		data.requestDroppedUnits.push_back(row);
	}

	VoxRlBlockIdentity identity;
	identity.session = m_gameUuid;
	identity.turn = segment.turn;
	identity.player = static_cast<int>(segment.player);
	identity.staticGeneration = segment.staticGeneration;
	identity.worldGeneration = segment.worldGeneration;
	identity.campaignGeneration = segment.campaignGeneration;
	identity.deltaSequence = segment.nextDeltaSequence;
	identity.decisionId = engagement.decisionId;

	VoxRlOwnedBlockStorage requestStorage;
	unsigned int requestLength = 0;
	bool requestBuilt = false;
	{
		ScopedTiming timing(m_config.timings, segment.timings.requestBuildNs);
		requestBuilt = VoxRlBuildRequestBlock(identity, data, requestStorage, requestLength);
	}
	if (!requestBuilt)
	{
		FailSegment("captureFailure");
		results = RunNativeSearch(vUnits, pTarget, eAggression, unuseableUnits,
			bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement);
		return;
	}
	if (m_config.timings)
	{
		segment.timings.requestCount += 1;
		segment.timings.requestBytes += requestLength;
	}
	if (!EnqueueFrame(requestStorage.Bytes(), requestLength, VOX_RL_BLOCK_REQUEST,
		segment.nextDeltaSequence, engagement.decisionId, engagement.attemptIndex))
	{
		results = RunNativeSearch(vUnits, pTarget, eAggression, unuseableUnits,
			bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement);
		return;
	}
	const unsigned int requestFrameIndex = static_cast<unsigned int>(segment.frameTable.size() - 1);
	if (!segment.hasRequests)
	{
		segment.hasRequests = true;
		segment.firstRequestSequence = segment.nextDeltaSequence;
	}
	segment.lastRequestSequence = segment.nextDeltaSequence;
	segment.nextDeltaSequence += 1;
	ClearDirtyState();

	// 2. Invoke native search exactly once, keeping nested refreshes in this decision.
	m_searchActive = true;
	results = RunNativeSearch(vUnits, pTarget, eAggression, unuseableUnits,
		bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement);
	m_searchActive = false;

	// 3. Snapshot assignments into the result block.
	VoxRlResultData assignments;
	if (!VoxRlCollectAssignmentRows(ePlayer, results, assignments))
	{
		FailSegment("captureFailure");
		return;
	}

	VoxRlOwnedBlockStorage resultStorage;
	unsigned int resultLength = 0;
	bool resultBuilt = false;
	{
		ScopedTiming timing(m_config.timings, segment.timings.resultBuildNs);
		resultBuilt = assignments.Write(identity, resultStorage, resultLength);
	}
	if (!resultBuilt)
	{
		FailSegment("captureFailure");
		return;
	}
	if (m_config.timings)
	{
		segment.timings.resultCount += 1;
		segment.timings.resultBytes += resultLength;
	}
	if (!EnqueueFrame(resultStorage.Bytes(), resultLength, VOX_RL_BLOCK_RESULT,
		segment.lastRequestSequence, engagement.decisionId, engagement.attemptIndex))
	{
		return;
	}
	const unsigned int resultFrameIndex = static_cast<unsigned int>(segment.frameTable.size() - 1);

	// 4. Record the decision row and publish the pair as one batch.
	Segment::DecisionRow decision;
	decision.decisionId = engagement.decisionId;
	decision.attemptIndex = engagement.attemptIndex;
	decision.caller = callerType;
	decision.retryOutcome = engagement.lastOutcome;
	decision.requestFrameIndex = requestFrameIndex;
	decision.resultFrameIndex = resultFrameIndex;
	decision.hasPrevious = engagement.attemptIndex > 0;
	decision.previousAttemptIndex = engagement.attemptIndex - 1;
	decision.previousWorldGeneration = engagement.lastAttemptWorldGeneration;
	segment.decisions.push_back(decision);
	const unsigned int completedWorldGeneration = segment.worldGeneration;
	if (segment.published)
	{
		CommitBatch(false, NULL);
		if (m_segment == NULL || m_segment->failed)
		{
			return;
		}
	}

	// 5. Update the engagement for a possible retry. An empty result sets
	// the retry outcome directly; an execution failure is reported by the
	// caller loop before the next attempt runs.
	engagement.lastSearchEmpty = results.empty();
	if (results.empty())
	{
		engagement.lastOutcome = VOX_RL_RETRY_PREVIOUS_EMPTY;
	}
	engagement.lastAttemptWorldGeneration = completedWorldGeneration;
	engagement.attemptIndex += 1;
}

void VoxRlCapture::NoteUnitChanged(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyUnits.insert(VoxRlEntityKey(static_cast<int>(eOwner), iUnitId));
	m_segment->removedUnits.erase(VoxRlEntityKey(static_cast<int>(eOwner), iUnitId));
}

void VoxRlCapture::NoteUnitPromotionsChanged(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	NoteUnitChanged(eOwner, iUnitId);
	// Promotions change the sparse modifier, plague, and blocked-promotion rows
	// of this unit only; other entities are not recollected.
	const VoxRlEntityKey key(static_cast<int>(eOwner), iUnitId);
	m_segment->dirtyUnitModifiers.insert(key);
	m_segment->dirtyUnitPlagues.insert(key);
	m_segment->dirtyUnitBlockedPromotions.insert(key);
}

void VoxRlCapture::NoteUnitCreated(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	NoteUnitChanged(eOwner, iUnitId);
	const VoxRlEntityKey key(static_cast<int>(eOwner), iUnitId);
	m_segment->dirtyUnitModifiers.insert(key);
	m_segment->dirtyUnitPlagues.insert(key);
	m_segment->dirtyUnitBlockedPromotions.insert(key);
	m_segment->dirtyUnitAttackCounts.insert(key);
}

void VoxRlCapture::NoteUnitRemoved(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	const VoxRlEntityKey key(static_cast<int>(eOwner), iUnitId);
	m_segment->dirtyUnits.erase(key);
	m_segment->removedUnits.insert(key);
	// Deletion removes the unit's associated sparse rows, so no family
	// tombstone or collection work remains for it.
	m_segment->dirtyUnitModifiers.erase(key);
	m_segment->dirtyUnitPlagues.erase(key);
	m_segment->dirtyUnitBlockedPromotions.erase(key);
	m_segment->dirtyUnitAttackCounts.erase(key);
}

void VoxRlCapture::NoteCityChanged(PlayerTypes eOwner, int iCityId)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyCities.insert(VoxRlEntityKey(static_cast<int>(eOwner), iCityId));
	m_segment->removedCities.erase(VoxRlEntityKey(static_cast<int>(eOwner), iCityId));
}

void VoxRlCapture::NoteCityCreated(PlayerTypes eOwner, int iCityId)
{
	if (m_segment == NULL || m_segment->failed) return;
	NoteCityChanged(eOwner, iCityId);
	m_segment->dirtyCityAttackCounts.insert(VoxRlEntityKey(static_cast<int>(eOwner), iCityId));
}

void VoxRlCapture::NoteCityRemoved(PlayerTypes eOwner, int iCityId)
{
	if (m_segment == NULL || m_segment->failed) return;
	const VoxRlEntityKey key(static_cast<int>(eOwner), iCityId);
	m_segment->dirtyCities.erase(key);
	m_segment->removedCities.insert(key);
	m_segment->lastCityRows.erase(key);
	// Deletion removes the city's associated sparse rows, so no family
	// tombstone or collection work remains for it.
	m_segment->dirtyCityAttackCounts.erase(key);
}

void VoxRlCapture::NotePlotChanged(int iPlotIndex)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyPlots.insert(iPlotIndex);
}

void VoxRlCapture::NoteVisibilityChanged(TeamTypes eTeam, int iPlotIndex, int iBitsetKind, bool bValue)
{
	if (m_segment == NULL || m_segment->failed) return;
	if (eTeam == NO_TEAM) return;
	const VoxRlVisibilityKey key(static_cast<int>(eTeam), iPlotIndex, iBitsetKind);
	m_segment->visibilityFlips[key] = bValue ? 1 : 0;
}

void VoxRlCapture::NoteKnownVisibilityReset(TeamTypes eTeam)
{
	if (m_segment == NULL || m_segment->failed) return;
	if (eTeam == NO_TEAM) return;
	Segment& segment = *m_segment;
	segment.visibilityResets.insert(static_cast<int>(eTeam));
	// The reset supersedes this team's pending known-visible entries. The map
	// is keyed team-major, so the team's range is contiguous and the scan never
	// touches other teams' entries; other bitset kinds are preserved.
	std::map<VoxRlVisibilityKey, unsigned char>::iterator flip =
		segment.visibilityFlips.lower_bound(VoxRlVisibilityKey(static_cast<int>(eTeam), INT_MIN, 0));
	while (flip != segment.visibilityFlips.end() && (*flip).first.team == static_cast<int>(eTeam))
	{
		if ((*flip).first.kind == VOX_RL_BITSET_KNOWN_VISIBLE)
		{
			segment.visibilityFlips.erase(flip++);
		}
		else
		{
			++flip;
		}
	}
}

void VoxRlCapture::NoteTacticalZonesRebuilt()
{
	// The native dominance zone table was rebuilt; the next collection compares
	// it against the accepted snapshot and re-accepts what changed.
	m_zoneSnapshotDirty = true;
}

void VoxRlCapture::NoteTeamTechsChanged()
{
	// A technology ownership change can flip terrain and feature passability;
	// the next collection recollects the small team table.
	m_teamPassabilityDirty = true;
}

void VoxRlCapture::NoteRevealedOverrideChanged(TeamTypes eTeam, int iPlotIndex, bool bRemoved)
{
	if (m_segment == NULL || m_segment->failed) return;
	if (eTeam == NO_TEAM) return;
	const VoxRlEntityKey key(static_cast<int>(eTeam), iPlotIndex);
	if (bRemoved)
	{
		m_segment->revealedOverrideUpserts.erase(key);
		m_segment->removedRevealedOverrides.insert(key);
	}
	else
	{
		m_segment->removedRevealedOverrides.erase(key);
		m_segment->revealedOverrideUpserts[key] = 1;
	}
}

void VoxRlCapture::NoteInterceptorCacheChanged(PlayerTypes ePlayer)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyInterceptors.insert(static_cast<int>(ePlayer));
}

void VoxRlCapture::NoteTeamRelationChanged(TeamTypes eTeam, TeamTypes eOtherTeam)
{
	if (m_segment == NULL || m_segment->failed) return;
	if (eTeam == NO_TEAM || eOtherTeam == NO_TEAM) return;
	m_segment->dirtyRelations[VoxRlEntityKey(static_cast<int>(eTeam), static_cast<int>(eOtherTeam))] = 1;
}

void VoxRlCapture::NoteWarStateChanged(TeamTypes eTeam, TeamTypes eOtherTeam)
{
	NoteTeamRelationChanged(eTeam, eOtherTeam);
	NoteTeamRelationChanged(eOtherTeam, eTeam);
	if (m_segment == NULL || m_segment->failed) return;
	// War state changes dirty every member's danger cache and mark the next
	// pre-refresh moment as a WORLD replacement checkpoint.
	const int teamA = static_cast<int>(eTeam);
	const int teamB = static_cast<int>(eOtherTeam);
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		const PlayerTypes member = static_cast<PlayerTypes>(player);
		const int memberTeam = static_cast<int>(GET_PLAYER(member).getTeam());
		if (memberTeam == teamA || memberTeam == teamB)
		{
			if (IsObserving(member))
			{
				NoteDangerDirty(member);
			}
		}
	}
	m_worldReplacementPending = true;
}

void VoxRlCapture::NoteUnitAttackCountChanged(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyUnitAttackCounts.insert(VoxRlEntityKey(static_cast<int>(eOwner), iUnitId));
}

void VoxRlCapture::NoteCityAttackCountChanged(PlayerTypes eOwner, int iCityId)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyCityAttackCounts.insert(VoxRlEntityKey(static_cast<int>(eOwner), iCityId));
}

void VoxRlCapture::NotePlayerResistanceChanged(PlayerTypes ePlayer, PlayerTypes eOpponent)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyPlayerResistances.insert(static_cast<int>(ePlayer));
}

void VoxRlCapture::NoteTopologyInvalidated()
{
	if (m_segment == NULL || m_segment->failed) return;
	m_topologyInvalidated = true;
}
