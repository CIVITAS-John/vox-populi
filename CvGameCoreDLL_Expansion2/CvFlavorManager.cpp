/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvFlavorManager.h"
#include "CvMinorCivAI.h"
#include "CvEnumMapSerialization.h"

// must be included after all other headers
#include "LintFree.h"

/// Constructor
CvFlavorRecipient::CvFlavorRecipient()
{

}

/// Destructor
CvFlavorRecipient::~CvFlavorRecipient()
{

}

/// Initialize data
void CvFlavorRecipient::Init()
{
//	m_piLatestFlavorValues = vector<int>(GC.getNumFlavorTypes(), 0);
	m_piLatestFlavorValues.init(0);
}

/// Deallocate memory created in initialize
void CvFlavorRecipient::Uninit()
{
//	m_piLatestFlavorValues.clear();
	m_piLatestFlavorValues.uninit();
}

/// Returns whether or not this Recipient is a City
bool CvFlavorRecipient::IsCity() const
{
	return m_bIsCity;
}

/// Public function that other classes can call to set/reset this recipient's flavors
void CvFlavorRecipient::SetFlavors(const CvEnumMap<FlavorTypes, int>& piUpdatedFlavorValues, const char* reason)
{
	ASSERT(piUpdatedFlavorValues.valid(), "Invalid map of flavor deltas passed to flavor recipient");

	if(!piUpdatedFlavorValues.valid()) return;

	int iNumFlavors = GC.getNumFlavorTypes();
	for (int iI = 0; iI < iNumFlavors; iI++)
	{
		LogFlavorChange((FlavorTypes)iI, piUpdatedFlavorValues[iI]-m_piLatestFlavorValues[iI], reason, true);
		m_piLatestFlavorValues[iI] = piUpdatedFlavorValues[iI];
	}

	FlavorUpdate();
}

/// Public function that other classes can call to change this recipient's flavors
void CvFlavorRecipient::ChangeFlavors(const CvEnumMap<FlavorTypes, int>& piDeltaFlavorValues, const char* reason, bool effectstart)
{
	ASSERT(piDeltaFlavorValues.valid(), "Invalid map of flavor deltas passed to flavor recipient");

	if(!piDeltaFlavorValues.valid()) return;

	int iNumFlavors = GC.getNumFlavorTypes();
	for(int iI = 0; iI < iNumFlavors; iI++)
	{
		if(piDeltaFlavorValues[iI] != 0)
		{
			LogFlavorChange((FlavorTypes)iI, piDeltaFlavorValues[iI], reason, effectstart);
			//don't enforce a certain range here, this will permanently skew the flavor if a temporary effect is cancelled!
			m_piLatestFlavorValues[iI] += piDeltaFlavorValues[iI];
		}
	}

	FlavorUpdate();
}

/// LatestFlavorValue Accessor Function
int CvFlavorRecipient::GetLatestFlavorValue(FlavorTypes eFlavor, bool bAllowNegative)
{
	PRECONDITION(eFlavor > -1, "Out of bounds.");
	PRECONDITION(eFlavor < GC.getNumFlavorTypes(), "Out of bounds.");

	if(m_piLatestFlavorValues[eFlavor] < 0 && !bAllowNegative)
	{
		return 0;
	}

	return m_piLatestFlavorValues[eFlavor];
}

/// Constructor
CvFlavorManager::CvFlavorManager(void)
{
	// Vox Deorum: Initialize custom flavor fields
	m_iCustomFlavorSetTurn = -1;
	m_bHasCustomFlavors = false;
}

/// Destructor
CvFlavorManager::~CvFlavorManager(void)
{
	Uninit();
}

/// Initialize
void CvFlavorManager::Init(CvPlayer* pPlayer)
{
	// Copy off inputs
	m_pPlayer = pPlayer;

	// Allocate memory
	m_piPersonalityFlavor.init();
	m_piActiveFlavor.init();
	m_CustomFlavors.init(50); // Vox Deorum: Initialize custom flavors to 50
	m_FlavorTargetList.reserve((3*64)+100);

	// Clear variables
	Reset();

	// If this is a live player, go ahead and set up his flavor preferences
	PlayerTypes p = pPlayer->GetID();
	if (p != NO_PLAYER)
	{
		SlotStatus s = CvPreGame::slotStatus(p);
		if ((s == SS_TAKEN || s == SS_COMPUTER) && !pPlayer->isBarbarian())
		{
			// Copy over leaderhead defaults unless human
			if (!pPlayer->isHuman(ISHUMAN_AI_DIPLOMACY))
			{
				LeaderHeadTypes leader = pPlayer->getPersonalityType();
				if (leader != NO_LEADER)
				{
					CvLeaderHeadInfo* pkLeaderHeadInfo = GC.getLeaderHeadInfo(leader);
					if (pkLeaderHeadInfo)
					{
						int iNumFlavorTypes = GC.getNumFlavorTypes();

						for (int iI = 0; iI < iNumFlavorTypes; iI++)
						{
							// Majors and Barbarians use Leader XML Flavors
							if (!pPlayer->isMinorCiv())
							{
								m_piPersonalityFlavor[iI] = pkLeaderHeadInfo->getFlavorValue(iI);
							}
							// Minors use Minor XML Flavors
							else
							{
								m_piPersonalityFlavor[iI] = GC.getMinorCivInfo(pPlayer->GetMinorCivAI()->GetMinorCivType())->getFlavorValue(iI);
							}
						}
					}
				}

				// Tweak from default values
				RandomizeWeights();
			}
			// Human player, just set all flavors to average (5)
			else
			{
				int iDefaultFlavorValue = /*5*/ GD_INT_GET(DEFAULT_FLAVOR_VALUE);
				int iNumFlavors = GC.getNumFlavorTypes();
				for (int iI = 0; iI < iNumFlavors; iI++)
				{
					m_piPersonalityFlavor[iI] = iDefaultFlavorValue;
				}
			}

			// Send out updated values to all recipients
			BroadcastBaseFlavors();

			//make the personality active and broadcast again
			ResetToBasePersonality();
		}
	}
}

/// Deallocate memory created in initialize
void CvFlavorManager::Uninit()
{
	m_piPersonalityFlavor.uninit();
	m_piActiveFlavor.uninit();
	m_CustomFlavors.uninit(); // Vox Deorum
	m_FlavorTargetList.clear();
}

/// Reset member variables
void CvFlavorManager::Reset()
{
	m_piPersonalityFlavor.assign(0);
	m_piActiveFlavor.assign(0);
	m_CustomFlavors.assign(50); // Vox Deorum
	m_iCustomFlavorSetTurn = -1; // Vox Deorum
	m_bHasCustomFlavors = false; // Vox Deorum
}

template<typename FlavorManager, typename Visitor>
void CvFlavorManager::Serialize(FlavorManager& flavorManager, Visitor& visitor)
{
	visitor(flavorManager.m_piPersonalityFlavor);
	visitor(flavorManager.m_piActiveFlavor);

	// Vox Deorum: Serialize custom flavor fields - will activate later
	visitor(flavorManager.m_bHasCustomFlavors);
	visitor(flavorManager.m_iCustomFlavorSetTurn);
	visitor(flavorManager.m_CustomFlavors);
}

/// Serialization read
void CvFlavorManager::Read(FDataStream& kStream)
{
	CvStreamLoadVisitor serialVisitor(kStream);
	Serialize(*this, serialVisitor);
}

/// Serialization write
void CvFlavorManager::Write(FDataStream& kStream) const
{
	CvStreamSaveVisitor serialVisitor(kStream);
	Serialize(*this, serialVisitor);
}

FDataStream& operator>>(FDataStream& loadFrom, CvFlavorManager& writeTo)
{
	writeTo.Read(loadFrom);
	return loadFrom;
}
FDataStream& operator<<(FDataStream& saveTo, const CvFlavorManager& readFrom)
{
	readFrom.Write(saveTo);
	return saveTo;
}

/// Register a new recipient of the player's global flavors
void CvFlavorManager::AddFlavorRecipient(CvFlavorRecipient* pTargetObject, bool bPropegateFlavorValues)
{
	// Add this one to our list
	m_FlavorTargetList.push_back(pTargetObject);

	// If we've already been initialized, then go ahead and send out current values
	if(m_piPersonalityFlavor.valid() && bPropegateFlavorValues)
	{
		pTargetObject->SetFlavors(m_piPersonalityFlavor, "ADDED_RECIPIENT");
	}
}

/// Remove the recipient
void CvFlavorManager::RemoveFlavorRecipient(CvFlavorRecipient* pTargetObject)
{
	Flavor_List::iterator iter = m_FlavorTargetList.begin();
	Flavor_List::iterator end  = m_FlavorTargetList.end();

	while(iter != end)
	{
		if(*iter == pTargetObject)
		{
			m_FlavorTargetList.erase(iter);
			return;
		}
		++iter;
	}
}

void CvFlavorManager::ChangeLeader(LeaderHeadTypes eOldLeader, LeaderHeadTypes eNewLeader)
{
	CvLeaderHeadInfo* pkOldLeaderHeadInfo = GC.getLeaderHeadInfo(eOldLeader);
	CvLeaderHeadInfo* pkNewLeaderHeadInfo = GC.getLeaderHeadInfo(eNewLeader);
	
	if(pkOldLeaderHeadInfo && pkNewLeaderHeadInfo)
	{
		CvEnumMap<FlavorTypes, int> aiTempFlavors;
		aiTempFlavors.init();
		
		for(std::size_t iI = 0; iI < aiTempFlavors.size(); iI++)
		{
			aiTempFlavors[iI] = pkNewLeaderHeadInfo->getFlavorValue(iI) - pkOldLeaderHeadInfo->getFlavorValue(iI);
		}
		
		ChangeActivePersonalityFlavors(aiTempFlavors, "NEW_LEADER", true);

		aiTempFlavors.uninit();
	}
}

/// Update to a new set of flavors
void CvFlavorManager::ChangeActivePersonalityFlavors(const CvEnumMap<FlavorTypes, int>& piDeltaFlavorValues, const char* reason, bool effectstart)
{
	ASSERT(piDeltaFlavorValues.valid(), "Invalid map of flavor deltas passed to flavor manager");

	if(!piDeltaFlavorValues.valid()) return;

	int iNumFlavors = GC.getNumFlavorTypes();
	for(int iI = 0; iI < iNumFlavors; iI++)
	{
		//don't enforce a certain range here, this will permanently skew the flavor if a temporary effect is cancelled!
		if(piDeltaFlavorValues[iI] != 0)
		{
			LogActivePersonalityChange((FlavorTypes)iI, piDeltaFlavorValues[iI], reason, effectstart);
			m_piActiveFlavor[iI] += piDeltaFlavorValues[iI];
		}
	}

	for (Flavor_List::iterator it = m_FlavorTargetList.begin(); it != m_FlavorTargetList.end(); ++it)
	{
		if ((*it)->IsCity())
			continue;

		(*it)->ChangeFlavors(piDeltaFlavorValues, reason, effectstart);
	}
}

void CvFlavorManager::ChangeCityFlavors(const CvEnumMap<FlavorTypes, int>& piDeltaFlavorValues, const char* reason, bool effectstart)
{
	for (Flavor_List::iterator it = m_FlavorTargetList.begin(); it != m_FlavorTargetList.end(); ++it)
	{
		if ((*it)->IsCity())
			(*it)->ChangeFlavors(piDeltaFlavorValues, reason, effectstart);
	}
}


/// Resets active settings to player's base personality
void CvFlavorManager::ResetToBasePersonality()
{
	for(int iI = 0; iI < GC.getNumFlavorTypes(); iI++)
	{
		LogActivePersonalityChange((FlavorTypes)iI, m_piPersonalityFlavor[iI] - m_piActiveFlavor[iI], "Reset_To_Base", true);
		m_piActiveFlavor[iI] = m_piPersonalityFlavor[iI];
	}

	BroadcastBaseFlavors();
}

/// Make some adjustments to flavors based on the map we're on
void CvFlavorManager::AdjustWeightsForMap()
{
	int iTotalLandTiles = GC.getMap().getLandPlots();
	int iNumPlayers = GC.getGame().GetNumMajorCivsAlive();

	if (iNumPlayers > 0)
	{
		int iNumFlavorTypes = GC.getNumFlavorTypes();
		// Find tiles per player
		float fTilesPerPlayer = iTotalLandTiles / (float)iNumPlayers;

		// Compute +/- addition
		//
		// We want this to be logarithmic, since that is the curve between lots of players on a duel map
		// and a few player on a huge map.  "FLAVOR_STANDARD_LOG10_TILES_PER_PLAYER" is the typical log10 of
		// tiles per player.  We go up and down from this point (multiplying by a coefficient) from here
		float fAdjust = log10(fTilesPerPlayer) - /*2.1f*/ GD_FLOAT_GET(FLAVOR_STANDARD_LOG10_TILES_PER_PLAYER);
		int iAdjust = (int)(fAdjust * /*8*/ GD_INT_GET(FLAVOR_EXPANDGROW_COEFFICIENT));

		int iMax = range(/*10*/ GD_INT_GET(PERSONALITY_FLAVOR_MAX_VALUE), 1, 100);
		int iMin = range(/*0*/ GD_INT_GET(PERSONALITY_FLAVOR_MIN_VALUE), 1, iMax);

		int iExpansionIndex = GC.getInfoTypeForString("FLAVOR_EXPANSION");
		int iGrowthIndex = GC.getInfoTypeForString("FLAVOR_GROWTH");

		PRECONDITION(iExpansionIndex >= 0 && iExpansionIndex < iNumFlavorTypes && iGrowthIndex >= 0 && iGrowthIndex < iNumFlavorTypes);

		// Boost expansion
		m_piPersonalityFlavor[iExpansionIndex] += iAdjust;
		if (m_piPersonalityFlavor[iExpansionIndex] > iMax)
			m_piPersonalityFlavor[iExpansionIndex] = iMax;

		// Reduce growth
		m_piPersonalityFlavor[iGrowthIndex] -= iAdjust;
		if (m_piPersonalityFlavor[iGrowthIndex] < iMin)
			m_piPersonalityFlavor[iGrowthIndex] = iMin;

		// Save these off as our core personality and broadcast updates
		ResetToBasePersonality();
	}
}

/// Retrieve the value of one Personality flavor, typically in the range [0,10]
int CvFlavorManager::GetPersonalityIndividualFlavor(FlavorTypes eType)
{
	if ((int)eType < 0 || (int)eType >= GC.getNumFlavorTypes()) return 0;

	// Vox Deorum: Override with custom flavor if active
	if (m_bHasCustomFlavors)
	{
		// Convert MCP value (0-100) to the configured personality flavor range
		int mcpValue = m_CustomFlavors[eType];
		int iMax = range(GD_INT_GET(PERSONALITY_FLAVOR_MAX_VALUE), 1, 100);
		int iValue = (mcpValue * iMax + 50) / 100; // Round
		return range(iValue, 0, iMax);
	}

	return m_piPersonalityFlavor[eType];
}

/// Retrieve the value of all Personality flavors
CvEnumMap<FlavorTypes, int>& CvFlavorManager::GetAllPersonalityFlavors()
{
	return m_piPersonalityFlavor;
}

/// Retrieve the value of one Personality flavor, modified for the diplomacy AI
int CvFlavorManager::GetPersonalityFlavorForDiplomacy(FlavorTypes eType)
{
	if ((int)eType < 0 || (int)eType >= GC.getNumFlavorTypes()) return 0;

	// Vox Deorum: Override with custom flavor if active
	if (m_bHasCustomFlavors)
	{
		// Convert MCP value (0-100) to 1-10 range
		int mcpValue = m_CustomFlavors[eType];
		int iValue = (int)(mcpValue / 10.0f + 0.5f); // Round to 0-10
		return range(iValue, 1, 10); // Clamp to 1-10 for diplomacy
	}

	int iMax = range(GD_INT_GET(PERSONALITY_FLAVOR_MAX_VALUE), 1, 100);
	int iRawValue = GetPersonalityIndividualFlavor(eType);

	// If the flavor was zeroed out, always return the minimum value
	if (iRawValue == 0)
		return 1;

	// If Max is 10, the flavor scale corresponds with the diplomacy AI's 1-10, so no issue here
	if (iMax == 10)
		return iRawValue;

	// If a modder has set max to 1, it's either 10 (for non-zero values) or 1 (for zero values)
	if (iMax == 1)
		return 10;

	// If Max isn't 10, map [1..iMax] -> [1..10] with rounding to nearest
	int iScaled = ((iRawValue - 1) * 9 + (iMax - 1) / 2) / (iMax - 1) + 1;
	return range(iScaled, 1, 10);
}

// PRIVATE METHODS

/// Make a random adjustment to each flavor value for this leader so they don't play exactly the same
void CvFlavorManager::RandomizeWeights()
{
	int iMax = range(/*10*/ GD_INT_GET(PERSONALITY_FLAVOR_MAX_VALUE), 1, 100);
	int iMin = range(/*0*/ GD_INT_GET(PERSONALITY_FLAVOR_MIN_VALUE), 1, iMax);
	int iPlusMinus = max(/*2*/ GD_INT_GET(FLAVOR_RANDOMIZATION_RANGE), 0);

	for (int iI = 0; iI < GC.getNumFlavorTypes(); iI++)
	{
		// Don't modify it if it's zero-ed out in the XML
		if (m_piPersonalityFlavor[iI] != 0)
			m_piPersonalityFlavor[iI] = range(GC.getGame().randRangeInclusive(m_piPersonalityFlavor[iI] - iPlusMinus, m_piPersonalityFlavor[iI] + iPlusMinus, CvSeeder::fromRaw(0xe655df8f).mix(m_pPlayer->GetID()).mix(iI)), iMin, iMax);
	}
}

// Vox Deorum: Vox Deorum's custom flavor conversion from the 0..100 scale to a signed game-scale delta:
// sign(x) * round(300 * (e^(3|x|) - 1) / 19.09) with x = (value - 50) / 50, evaluated once in single
// precision. The table keeps the conversion free of exp and identical in the DLL and the simulator.
const short FLAVOR_GAME_SCALE[101] =
{
	-300, -282, -264, -248, -233, -218, -205, -192, -180, -168,
	-158, -147, -138, -129, -121, -113, -105,  -98,  -91,  -85,
	 -79,  -74,  -69,  -64,  -59,  -55,  -51,  -47,  -43,  -40,
	 -36,  -33,  -31,  -28,  -25,  -23,  -21,  -19,  -17,  -15,
	 -13,  -11,  -10,   -8,   -7,   -5,   -4,   -3,   -2,   -1,
	   0,
	   1,    2,    3,    4,    5,    7,    8,   10,   11,   13,
	  15,   17,   19,   21,   23,   25,   28,   31,   33,   36,
	  40,   43,   47,   51,   55,   59,   64,   69,   74,   79,
	  85,   91,   98,  105,  113,  121,  129,  138,  147,  158,
	 168,  180,  192,  205,  218,  233,  248,  264,  282,  300
};

// Vox Deorum: converts a 0..100 flavor to Vox Deorum's game-scale delta, clamping out-of-range input.
int CvFlavorManager::FlavorToGameScale(int iValue)
{
	return FLAVOR_GAME_SCALE[range(iValue, 0, 100)];
}

// Vox Deorum: converts a game-scale delta back to the nearest 0..100 flavor, with ties going toward 50.
int CvFlavorManager::FlavorFromGameScale(int iGame)
{
	if (iGame <= FLAVOR_GAME_SCALE[0])
		return 0;
	if (iGame >= FLAVOR_GAME_SCALE[100])
		return 100;

	// find the first entry at or above the game value
	int iLow = 0;
	int iHigh = 100;
	while (iLow < iHigh)
	{
		int iMiddle = (iLow + iHigh) / 2;
		if (FLAVOR_GAME_SCALE[iMiddle] < iGame)
			iLow = iMiddle + 1;
		else
			iHigh = iMiddle;
	}
	if (FLAVOR_GAME_SCALE[iLow] == iGame)
		return iLow;

	int iAbove = FLAVOR_GAME_SCALE[iLow] - iGame;
	int iBelow = iGame - FLAVOR_GAME_SCALE[iLow - 1];
	if (iAbove != iBelow)
		return iAbove < iBelow ? iLow : iLow - 1;
	return iLow - 1 >= 50 ? iLow - 1 : iLow;
}

// Vox Deorum: shifts a 0..100 flavor by a game-scale delta, the way a strategy row or modifier moves it.
int CvFlavorManager::ShiftFlavor(int iValue, int iGameDelta)
{
	if (iGameDelta == 0)
		return range(iValue, 0, 100);
	return FlavorFromGameScale(FlavorToGameScale(iValue) + iGameDelta);
}

// Vox Deorum: Set custom flavors that auto-expire after 10 turns
// Accepts MCP range (0-100), stores raw values, converts to game range (-300 to 300) for strategies
void CvFlavorManager::SetCustomFlavors(const CvEnumMap<FlavorTypes, int>& flavors)
{
	// If custom flavors already exist, unset them first
	if (m_bHasCustomFlavors)
	{
		UnsetCustomFlavors();
	}

	// Store the raw MCP values (0-100) and convert to game range (-300 to 300) for strategies
	CvEnumMap<FlavorTypes, int> gameFlavors;
	gameFlavors.init(0);

	int iNumFlavors = GC.getNumFlavorTypes();
	for (int i = 0; i < iNumFlavors; i++)
	{
		int mcpValue = flavors[i];

		// Store raw MCP value (0-100)
		m_CustomFlavors[i] = mcpValue;

		// Convert to game range (-300 to 300) with the flavor scale table
		// (MCP 50 = game 0, gentle middle, steep extremes)
		gameFlavors[i] = FlavorToGameScale(mcpValue);
	}

	// Apply them using BOTH ChangeActivePersonalityFlavors and ChangeCityFlavors with reason "VoxDeorum"
	ChangeActivePersonalityFlavors(gameFlavors, "VoxDeorum", true);
	ChangeCityFlavors(gameFlavors, "VoxDeorum", true);

	// Set the turn when custom flavors were applied
	m_iCustomFlavorSetTurn = GC.getGame().getGameTurn();
	m_bHasCustomFlavors = true;
}

// Vox Deorum: Unset custom flavors
void CvFlavorManager::UnsetCustomFlavors()
{
	// If no custom flavors, return
	if (!m_bHasCustomFlavors)
	{
		return;
	}

	// Convert stored MCP values to game range deltas for removal
	CvEnumMap<FlavorTypes, int> negativeFlavors;
	negativeFlavors.init();

	int iNumFlavors = GC.getNumFlavorTypes();
	for (int i = 0; i < iNumFlavors; i++)
	{
		int mcpValue = m_CustomFlavors[i];

		// Convert MCP to game range, then negate for removal
		negativeFlavors[i] = -FlavorToGameScale(mcpValue);
	}

	// Apply the negative deltas to both player and city flavors with reason "Custom"
	ChangeActivePersonalityFlavors(negativeFlavors, "Custom", false);
	ChangeCityFlavors(negativeFlavors, "Custom", false);

	// Clear m_CustomFlavors (set all to 50)
	m_CustomFlavors.assign(50);
	m_bHasCustomFlavors = false;
}

// Vox Deorum: Get custom flavors (returns raw MCP range 0-100)
void CvFlavorManager::GetCustomFlavors(CvEnumMap<FlavorTypes, int>& out) const
{
	// Copy raw MCP values (0-100) from m_CustomFlavors
	int iNumFlavors = GC.getNumFlavorTypes();
	for (int i = 0; i < iNumFlavors; i++)
	{
		out[i] = m_CustomFlavors[i];
	}
}

// Vox Deorum: Check if custom flavors should expire
void CvFlavorManager::CheckCustomFlavorExpiration()
{
	// If has custom flavors and current turn >= m_iCustomFlavorSetTurn + 10, call UnsetCustomFlavors()
	if (m_bHasCustomFlavors && GC.getGame().getGameTurn() >= m_iCustomFlavorSetTurn + 10)
	{
		UnsetCustomFlavors();
	}
}

// Vox Deorum: Check if custom flavor is lower than threshold (0-100 MCP range)
// Returns false if no custom flavor is set
bool CvFlavorManager::IsCustomFlavorLowerThan(FlavorTypes eFlavor, int iThreshold) const
{
	if (!m_bHasCustomFlavors)
		return false;

	if ((int)eFlavor < 0 || (int)eFlavor >= GC.getNumFlavorTypes())
		return false;

	return m_CustomFlavors[eFlavor] <= iThreshold;
}

// Vox Deorum: Check if custom flavor is higher than threshold (0-100 MCP range)
// Returns false if no custom flavor is set
bool CvFlavorManager::IsCustomFlavorHigherThan(FlavorTypes eFlavor, int iThreshold) const
{
	if (!m_bHasCustomFlavors)
		return false;

	if ((int)eFlavor < 0 || (int)eFlavor >= GC.getNumFlavorTypes())
		return false;

	return m_CustomFlavors[eFlavor] >= iThreshold;
}

/// Sends base personality flavor settings to all recipients
void CvFlavorManager::BroadcastBaseFlavors()
{
	for(Flavor_List::iterator it = m_FlavorTargetList.begin(); it != m_FlavorTargetList.end(); ++it)
	{
		(*it)->SetFlavors(m_piPersonalityFlavor, "BASEFLAVOR");
	}
}

void CvFlavorManager::LogActivePersonalityChange(FlavorTypes eFlavor, int change, const char* reason, bool start)
{
	CvString strOutBuf;
	CvString strBaseString;
	CvString strTemp;
	CvString playerName;
	CvString strDesc;
	CvString strLogName;

	if(GC.getLogging() && GC.getAILogging())
	{
		// Find the name of this civ
		playerName = m_pPlayer->getCivilizationShortDescription();

		// Open the log file
		if(GC.getPlayerAndCityAILogSplit())
		{
			strLogName = "FlavorAILog_" + playerName + ".csv";
		}
		else
		{
			strLogName = "FlavorAILog.csv";
		}

		FILogFile* pLog = LOGFILEMGR.GetLog(strLogName, FILogFile::kDontTimeStamp);

		// Get the leading info for this line
		strBaseString.Format("%03d, ", GC.getGame().getElapsedGameTurns());
		strBaseString += playerName + ", ";

		// Dump out the setting for each flavor
		if(eFlavor == NO_FLAVOR)
		{
			for(int iI = 0; iI < GC.getNumFlavorTypes(); iI++)
			{
				strTemp.Format("%s, %d, %d, %s, %s", GC.getFlavorTypes((FlavorTypes)iI).GetCString(), m_piActiveFlavor[iI], change, reason ? reason : "unknown", start ? "start" : "end");
				strOutBuf = strBaseString + strTemp;
				pLog->Msg(strOutBuf);
			}
		}
		else
		{
			strTemp.Format("%s, %d, %d, %s, %s", GC.getFlavorTypes((FlavorTypes)eFlavor).GetCString(), m_piActiveFlavor[eFlavor], change, reason ? reason : "unknown", start ? "start" : "end");
			strOutBuf = strBaseString + strTemp;
			pLog->Msg(strOutBuf);
		}
	}
}
