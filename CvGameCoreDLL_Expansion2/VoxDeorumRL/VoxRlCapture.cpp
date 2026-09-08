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

#include <cstdio>
#include <cstdlib>
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
	std::set<int> dirtyPassability;
	std::map<VoxRlVisibilityKey, unsigned char> visibilityFlips;
	std::map<VoxRlEntityKey, unsigned char> revealedOverrideUpserts;  // team-major key, 1 = present
	std::set<VoxRlEntityKey> removedRevealedOverrides;                // (team, plot)
	std::set<int> dirtyInterceptors;
	std::map<VoxRlEntityKey, unsigned char> dirtyRelations;           // (team, otherTeam)
	std::vector<RequestDangerEventRecord> pendingDangerEvents;
	bool sparseDirty[6];

	// Last emitted plot and city records for the omit-unchanged comparison.
	// Only entities emitted as deltas in this segment are retained.
	std::map<VoxRlEntityKey, RequestDeltaPlotRecord> lastPlotRows;
	std::map<VoxRlEntityKey, RequestDeltaCityRecord> lastCityRows;

	// Iteration index bookkeeping across deltas.
	std::map<VoxRlEntityKey, unsigned int> unitIteration;
	std::map<VoxRlEntityKey, unsigned int> cityIteration;
	std::map<int, unsigned int> nextUnitIteration;
	std::map<int, unsigned int> nextCityIteration;

	// Staged synchronization request during a refresh or discovery window.
	bool stagingRequest;
	VoxRlRequestData staged;

	Segment()
		: player(NO_PLAYER), turn(-1), staticGeneration(0), worldGeneration(0), campaignGeneration(0),
		staticFramedLength(0), campaignFramedLength(0), worldFramedLength(0),
		nextDeltaSequence(0), published(false), failed(false), closed(false),
		pendingCount(0), committedFrameCount(0), committedDecisionCount(0), committedCoverageCount(0),
		commitId(0), firstRequestSequence(0), lastRequestSequence(0), hasRequests(false),
		stagingRequest(false)
	{
		closureReason[0] = '\0';
		for (int index = 0; index < 6; ++index) sparseDirty[index] = false;
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
	: enabled(false), filterPlayers(false), filterTurns(false)
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
	if (!VoxRlCreateDirectories(m_captureRoot.c_str()))
	{
		m_captureRoot.clear();
		return false;
	}
	return true;
}

bool VoxRlCapture::AdmitsPlayer(PlayerTypes ePlayer) const
{
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
		// A closure-only batch commits without adding binary frames.
		CommitBatch(true, closureReason);
	}
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
	if (!VoxRlBuildStaticBlock(identity, storage, length))
	{
		return false;
	}
	const std::string directory = m_gameDirectory + "/baselines/static";
	if (!VoxRlCreateDirectories(directory.c_str()))
	{
		return false;
	}
	char fileName[32];
	sprintf_s(fileName, 32, "static-%u.bin", m_staticGeneration);
	const std::string path = directory + "/" + fileName;
	VoxRlOutputFile file;
	if (!file.OpenNew(path.c_str()) || !file.Write(storage.Bytes(), length) || !file.Flush())
	{
		return false;
	}
	m_staticRelPath = "baselines/static/" + std::string(fileName);
	m_staticFramedLength = length;
	m_staticGenerationWritten = true;
	m_topologyInvalidated = false;
	return true;
}

bool VoxRlCapture::BuildAndWriteWorld(PlayerTypes ePlayer, int iTurn)
{
	Segment& segment = *m_segment;
	char folder[64];
	sprintf_s(folder, 64, "baselines/world/player-%d/turn-%d", static_cast<int>(ePlayer), iTurn);
	const std::string directory = m_gameDirectory + "/" + folder;
	if (!VoxRlCreateDirectories(directory.c_str()))
	{
		return false;
	}
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
	if (!VoxRlBuildWorldBlock(identity, ePlayer, storage, length))
	{
		return false;
	}
	char fileName[32];
	sprintf_s(fileName, 32, "world-%u.bin", segment.worldGeneration);
	const std::string path = directory + "/" + fileName;
	VoxRlOutputFile file;
	if (!file.OpenNew(path.c_str()) || !file.Write(storage.Bytes(), length) || !file.Flush())
	{
		return false;
	}
	segment.worldRelPath = folder + std::string("/") + fileName;
	segment.worldFramedLength = length;
	SeedWorldIterationIndices();
	return true;
}

bool VoxRlCapture::BuildAndWriteCampaign(PlayerTypes ePlayer, int iTurn)
{
	Segment& segment = *m_segment;
	char folder[80];
	sprintf_s(folder, 80, "baselines/campaign/player-%d/turn-%d", static_cast<int>(ePlayer), iTurn);
	const std::string directory = m_gameDirectory + "/" + folder;
	if (!VoxRlCreateDirectories(directory.c_str()))
	{
		return false;
	}
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
	if (!VoxRlBuildCampaignBlock(identity, ePlayer, storage, length))
	{
		return false;
	}
	char fileName[36];
	sprintf_s(fileName, 36, "campaign-%u.bin", campaignGeneration);
	const std::string path = directory + "/" + fileName;
	VoxRlOutputFile file;
	if (!file.OpenNew(path.c_str()) || !file.Write(storage.Bytes(), length) || !file.Flush())
	{
		return false;
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
	if (!BuildAndWriteWorld(ePlayer, iTurn))
	{
		FailSegment("captureFailure");
		return;
	}
	char folder[80];
	sprintf_s(folder, 80, "segments/player-%d/turn-%d/world-%u",
		static_cast<int>(ePlayer), iTurn, segment.worldGeneration);
	segment.relativeSegmentDir = folder;
	const std::string segmentDir = m_gameDirectory + "/" + folder;
	if (!VoxRlCreateDirectories(segmentDir.c_str()))
	{
		FailSegment("captureFailure");
		return;
	}
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
	if (!VoxRlCreateDirectories((m_gameDirectory + "/baselines/static").c_str()) ||
		!VoxRlCreateDirectories((m_gameDirectory + "/segments").c_str()))
	{
		return;
	}
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
	// Dependencies must exist before the index publishes them.
	if (!VoxRlFileExists((m_gameDirectory + "/" + segment.staticRelPath).c_str()) ||
		!VoxRlFileExists((m_gameDirectory + "/" + segment.campaignRelPath).c_str()) ||
		!VoxRlFileExists((m_gameDirectory + "/" + segment.worldRelPath).c_str()))
	{
		FailSegment("captureFailure");
		return;
	}
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
	if (!m_segmentStream.Write(
		segment.pendingBytes.empty() ? NULL : &segment.pendingBytes[0],
		static_cast<unsigned int>(segment.pendingBytes.size())))
	{
		FailSegment("captureFailure");
		return;
	}
	m_segmentStreamBytes = segment.pendingBytes.size();
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

	// The stream must be durable before its index commit makes its bytes
	// visible to a reader after a process or system crash.
	if (!m_segmentStream.Flush() || !AppendIndexLine(batch.c_str()) ||
		!AppendIndexLine(commit.c_str()) || !m_segmentIndex.Flush())
	{
		FailSegment("captureFailure");
		return;
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
		if (!m_segmentStream.Write(bytes, length))
		{
			FailSegment("captureFailure");
			return false;
		}
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
	segment.pendingBytes.insert(segment.pendingBytes.end(), cursor, cursor + length);
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
	segment.dirtyPassability.clear();
	segment.visibilityFlips.clear();
	segment.revealedOverrideUpserts.clear();
	segment.removedRevealedOverrides.clear();
	segment.dirtyInterceptors.clear();
	segment.dirtyRelations.clear();
	segment.pendingDangerEvents.clear();
	for (int index = 0; index < 6; ++index) segment.sparseDirty[index] = false;
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
	const PlayerTypes capturingPlayer = segment.player;
	CvPlayerAI& capturing = GET_PLAYER(capturingPlayer);
	const TeamTypes capturingTeam = capturing.getTeam();
	CvTacticalAnalysisMap* zoneMap = capturing.GetTacticalAI()->GetTacticalAnalysisMap();
	CvMap& map = GC.getMap();
	const int plotCount = map.numPlots();

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
		if (!VoxRlCollectUnitRecord(*pUnit, capturingTeam, record))
		{
			return false;
		}
		record.iterationIndex = UnitIterationIndex(static_cast<PlayerTypes>((*key).owner), (*key).id);
		RequestDeltaUnitRecord delta;
		std::memcpy(&delta, &record, sizeof(record));
		data.requestDeltaUnits.push_back(delta);
	}

	for (std::set<int>::const_iterator plotIndex = segment.dirtyPlots.begin();
		plotIndex != segment.dirtyPlots.end(); ++plotIndex)
	{
		if (*plotIndex < 0 || *plotIndex >= plotCount) continue;
		CvPlot* plot = map.plotByIndex(*plotIndex);
		if (plot == NULL) continue;
		PlotDynamicRecord record;
		std::memset(&record, 0, sizeof(record));
		if (!VoxRlCollectPlotDynamicRecord(*plot, zoneMap, record))
		{
			return false;
		}
		// The packed unit range belongs to the WORLD builder; a delta plot
		// row carries no unit range and the loader merges the rest.
		RequestDeltaPlotRecord delta;
		std::memset(&delta, 0, sizeof(delta));
		delta.plotIndex = static_cast<i32>(*plotIndex);
		delta.effectiveOwningCityId = record.effectiveOwningCityId;
		delta.improvementType = record.improvementType;
		delta.resourceType = record.resourceType;
		delta.reconCount = record.reconCount;
		delta.extraMovePathCost = record.extraMovePathCost;
		delta.unitIncrement = record.unitIncrement;
		delta.zoneId = record.zoneId;
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
		std::map<VoxRlEntityKey, RequestDeltaPlotRecord>::const_iterator last =
			segment.lastPlotRows.find(plotKey);
		if (last != segment.lastPlotRows.end() && std::memcmp(&last->second, &delta, sizeof(delta)) == 0)
		{
			continue;
		}
		segment.lastPlotRows[plotKey] = delta;
		data.requestDeltaPlots.push_back(delta);
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

	for (std::set<int>::const_iterator plotIndex = segment.dirtyPassability.begin();
		plotIndex != segment.dirtyPassability.end(); ++plotIndex)
	{
		if (*plotIndex < 0 || *plotIndex >= plotCount) continue;
		CvPlot* plot = map.plotByIndex(*plotIndex);
		if (plot == NULL) continue;
		PlotPassabilityRecord record;
		std::memset(&record, 0, sizeof(record));
		if (!CollectPlotPassabilityRecord(*plot, record))
		{
			return false;
		}
		RequestPlotPassabilityRecord row;
		std::memset(&row, 0, sizeof(row));
		row.plotIndex = static_cast<i32>(*plotIndex);
		row.baseImpassable = record.baseImpassable;
		std::memcpy(row.teamImpassable, record.teamImpassable, sizeof(row.teamImpassable));
		data.requestPlotPassability.push_back(row);
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

	// Sparse family replacements publish the complete family. The helpers
	// collect the WORLD record shapes, which the request sections mirror
	// field for field.
	if (segment.sparseDirty[VOX_RL_SPARSE_UNIT_MODIFIERS])
	{
		std::vector<UnitModifierRecord> rows;
		if (!VoxRlCollectAllUnitModifierRows(rows)) return false;
		VoxRlMirrorSparseRows(rows, data.requestUnitModifiers);
	}
	if (segment.sparseDirty[VOX_RL_SPARSE_UNIT_PLAGUES] || segment.sparseDirty[VOX_RL_SPARSE_UNIT_BLOCKED_PROMOTIONS])
	{
		std::vector<UnitPlagueRecord> plagues;
		std::vector<UnitBlockedPromotionRecord> blocked;
		VoxRlCollectAllUnitPlagueRows(plagues, blocked);
		if (segment.sparseDirty[VOX_RL_SPARSE_UNIT_PLAGUES])
		{
			VoxRlMirrorSparseRows(plagues, data.requestUnitPlagues);
		}
		if (segment.sparseDirty[VOX_RL_SPARSE_UNIT_BLOCKED_PROMOTIONS])
		{
			VoxRlMirrorSparseRows(blocked, data.requestUnitBlockedPromotions);
		}
	}
	if (segment.sparseDirty[VOX_RL_SPARSE_UNIT_ATTACK_COUNTS])
	{
		std::vector<UnitAttackCountRecord> rows;
		VoxRlCollectAllUnitAttackCountRows(rows);
		VoxRlMirrorSparseRows(rows, data.requestUnitAttackCounts);
	}
	if (segment.sparseDirty[VOX_RL_SPARSE_PLAYER_RESISTANCES])
	{
		std::vector<PlayerResistanceRecord> rows;
		VoxRlCollectAllPlayerResistanceRows(rows);
		VoxRlMirrorSparseRows(rows, data.requestPlayerResistances);
	}
	if (segment.sparseDirty[VOX_RL_SPARSE_CITY_ATTACK_COUNTS])
	{
		std::vector<CityAttackCountRecord> rows;
		VoxRlCollectAllCityAttackCountRows(rows);
		VoxRlMirrorSparseRows(rows, data.requestCityAttackCounts);
	}
	unsigned char mask = 0;
	for (int bit = 0; bit < 6; ++bit)
	{
		if (segment.sparseDirty[bit]) mask = static_cast<unsigned char>(mask | (1u << bit));
	}
	data.requestHeader.sparseCombatReplacementMask = mask;

	// Pending danger events carry their event-time board in this request.
	data.requestDangerEvents = segment.pendingDangerEvents;
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
	if (!VoxRlBuildRequestBlock(identity, data, storage, length))
	{
		FailSegment("captureFailure");
		return false;
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
	// this refresh event.
	VoxRlRequestData data;
	if (!CollectDelta(data))
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
	data.requestDangerEvents.push_back(event);
	m_segment->staged = data;
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
	// Stage the event-time board the discovery consumes.
	VoxRlRequestData data;
	if (!CollectDelta(data))
	{
		FailSegment("captureFailure");
		return;
	}
	RequestKnownAttackerRecord row;
	std::memset(&row, 0, sizeof(row));
	row.observer = static_cast<i8>(m_segment->player);
	row.owner = static_cast<i8>(pUnit->getOwner());
	row.unitId = static_cast<i32>(pUnit->GetID());
	data.requestKnownAttackers.push_back(row);
	m_segment->staged = data;
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
	if (!VoxRlBuildRequestBlock(identity, data, requestStorage, requestLength))
	{
		FailSegment("captureFailure");
		results = RunNativeSearch(vUnits, pTarget, eAggression, unuseableUnits,
			bTargetDistanceRelevant, bReturnToStartPositions, iSaveMovement);
		return;
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
	if (!assignments.Write(identity, resultStorage, resultLength))
	{
		FailSegment("captureFailure");
		return;
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
	// Promotions change the sparse modifier, plague, and blocked-promotion
	// families, which are published as complete replacements.
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_MODIFIERS] = true;
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_PLAGUES] = true;
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_BLOCKED_PROMOTIONS] = true;
}

void VoxRlCapture::NoteUnitCreated(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	NoteUnitChanged(eOwner, iUnitId);
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_MODIFIERS] = true;
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_PLAGUES] = true;
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_BLOCKED_PROMOTIONS] = true;
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_ATTACK_COUNTS] = true;
}

void VoxRlCapture::NoteUnitRemoved(PlayerTypes eOwner, int iUnitId)
{
	if (m_segment == NULL || m_segment->failed) return;
	const VoxRlEntityKey key(static_cast<int>(eOwner), iUnitId);
	m_segment->dirtyUnits.erase(key);
	m_segment->removedUnits.insert(key);
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
	m_segment->sparseDirty[VOX_RL_SPARSE_CITY_ATTACK_COUNTS] = true;
}

void VoxRlCapture::NoteCityRemoved(PlayerTypes eOwner, int iCityId)
{
	if (m_segment == NULL || m_segment->failed) return;
	const VoxRlEntityKey key(static_cast<int>(eOwner), iCityId);
	m_segment->dirtyCities.erase(key);
	m_segment->removedCities.insert(key);
	m_segment->lastCityRows.erase(key);
}

void VoxRlCapture::NotePlotChanged(int iPlotIndex)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyPlots.insert(iPlotIndex);
}

void VoxRlCapture::NotePlotPassabilityChanged(int iPlotIndex)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->dirtyPassability.insert(iPlotIndex);
}

void VoxRlCapture::NoteVisibilityChanged(TeamTypes eTeam, int iPlotIndex, int iBitsetKind, bool bValue)
{
	if (m_segment == NULL || m_segment->failed) return;
	if (eTeam == NO_TEAM) return;
	const VoxRlVisibilityKey key(static_cast<int>(eTeam), iPlotIndex, iBitsetKind);
	m_segment->visibilityFlips[key] = bValue ? 1 : 0;
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
	m_segment->sparseDirty[VOX_RL_SPARSE_UNIT_ATTACK_COUNTS] = true;
}

void VoxRlCapture::NoteCityAttackCountChanged(PlayerTypes eOwner, int iCityId)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->sparseDirty[VOX_RL_SPARSE_CITY_ATTACK_COUNTS] = true;
}

void VoxRlCapture::NotePlayerResistanceChanged(PlayerTypes ePlayer, PlayerTypes eOpponent)
{
	if (m_segment == NULL || m_segment->failed) return;
	m_segment->sparseDirty[VOX_RL_SPARSE_PLAYER_RESISTANCES] = true;
}

void VoxRlCapture::NoteTopologyInvalidated()
{
	if (m_segment == NULL || m_segment->failed) return;
	m_topologyInvalidated = true;
}
