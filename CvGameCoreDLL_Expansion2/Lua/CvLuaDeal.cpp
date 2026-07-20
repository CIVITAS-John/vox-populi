/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "../CvGameCoreDLLPCH.h"
#include "../CustomMods.h"
#include "../CvDealAI.h"
#include "../CvDiplomacyAI.h"
#include "CvLuaSupport.h"
#include "CvLuaDeal.h"

#pragma warning(disable:4800 ) //forcing value to bool 'true' or 'false'

	//Utility macro for registering methods
#define Method(Name)			\
	lua_pushcclosure(L, l##Name, 0);	\
	lua_setfield(L, t, #Name);


using namespace CvLuaArgs;
TradedItemList::iterator CvLuaDeal::m_iterator;

//------------------------------------------------------------------------------
void CvLuaDeal::PushMethods(lua_State* L, int t)
{
	Method(ClearItems);
	Method(GetNumItems);

	Method(GetStartTurn);
	Method(GetEndTurn);
	Method(GetDuration);

	Method(GetOtherPlayer);
	Method(GetFromPlayer);
	Method(GetToPlayer);
	Method(SetFromPlayer);
	Method(SetToPlayer);

	Method(GetSurrenderingPlayer);
	Method(SetSurrenderingPlayer);
	Method(GetDemandingPlayer);
	Method(SetDemandingPlayer);
	Method(GetRequestingPlayer);
	Method(SetRequestingPlayer);

	Method(ResetIterator);
	Method(GetNextItem);

	Method(GetGoldAvailable);

	Method(IsPossibleToTradeItem);
	Method(GetReasonsItemUntradeable);
	Method(GetTradeItemValue);
	Method(Enact); // Vox Deorum: enact an agreed agent deal for real (interactive-diplomacy stage 6)
#if defined(MOD_ACTIVE_DIPLOMACY)
	Method(AreAllTradeItemsValid); // Vox Deorum: expose guarded final validation to the stage 7 deal editor.
#endif
	Method(BlockTemporaryForPermanentTrade);

	Method(GetRenewDealMessage);
	Method(AddGoldTrade);
	Method(AddGoldPerTurnTrade);
	Method(AddMapTrade);
	Method(AddResourceTrade);
	Method(AddCityTrade);
	Method(AddAllowEmbassy);
	Method(AddOpenBorders);
	Method(AddDefensivePact);
	Method(AddResearchAgreement);
	Method(AddPeaceTreaty);
	Method(AddThirdPartyPeace);
	Method(AddThirdPartyWar);
	Method(AddDeclarationOfFriendship);
	Method(AddVoteCommitment);

	Method(RemoveByType);
	Method(RemoveResourceTrade);
	Method(RemoveCityTrade);
	Method(RemoveThirdPartyPeace);
	Method(RemoveThirdPartyWar);
	Method(RemoveVoteCommitment);

	Method(ChangeGoldTrade);
	Method(ChangeGoldPerTurnTrade);
	Method(ChangeResourceTrade);
	Method(ChangeThirdPartyWarDuration);
	Method(ChangeThirdPartyPeaceDuration);

	Method(AddTechTrade);
	Method(AddVassalageTrade);
	Method(AddRevokeVassalageTrade);
	Method(RemoveTechTrade);

	// DEPRECATED
	Method(AddUnitTrade);
	Method(AddTradeAgreement);
	Method(AddPermamentAlliance);
	Method(AddSurrender);
	Method(AddTruce);
	Method(AddThirdPartyEmbargo);
	Method(RemoveUnitTrade);
	Method(RemoveThirdPartyEmbargo);
	Method(ChangeThirdPartyEmbargoDuration);
}

//------------------------------------------------------------------------------
int CvLuaDeal::lRemoveByType(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	int args = lua_gettop(L);

	if(args == 1)
		pkDeal->RemoveByType((TradeableItems) lua_tointeger(L, 2), NO_PLAYER);
	else
		pkDeal->RemoveByType((TradeableItems) lua_tointeger(L, 2), (PlayerTypes) lua_tointeger(L, 3));

	return 0;
}


//------------------------------------------------------------------------------
void CvLuaDeal::HandleMissingInstance(lua_State* L)
{
	DefaultHandleMissingInstance(L);
}
//------------------------------------------------------------------------------
const char* CvLuaDeal::GetTypeName()
{
	return "Deal";
}
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Lua Methods
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
int CvLuaDeal::lIsPossibleToTradeItem(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes) lua_tointeger(L, 2);
	const PlayerTypes eToPlayer = (PlayerTypes) lua_tointeger(L, 3);
	const TradeableItems eItem = (TradeableItems) lua_tointeger(L, 4);
	const int iData1 = lua_tointeger(L, 5);
	const int iData2 = lua_tointeger(L, 6);
	const int iData3 = lua_tointeger(L, 7);
	const bool bFlag1 = lua_toboolean(L, 8);
	// Vox Deorum: optional human-to-human override (default false preserves stock behavior).
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 9, false);

	const bool bResult = pkDeal->IsPossibleToTradeItem(eFromPlayer, eToPlayer, eItem, iData1, iData2, iData3, bFlag1, /*bFinalizing*/ false, bTreatAsHumanToHuman);
	lua_pushboolean(L, bResult);
	return 1;
}

int CvLuaDeal::lGetReasonsItemUntradeable(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes) lua_tointeger(L, 2);
	const PlayerTypes eToPlayer = (PlayerTypes) lua_tointeger(L, 3);
	const TradeableItems eItem = (TradeableItems) lua_tointeger(L, 4);
	const int iData1 = lua_tointeger(L, 5);
	const int iData2 = lua_tointeger(L, 6);
	const int iData3 = lua_tointeger(L, 7);
	const bool bFlag1 = lua_toboolean(L, 8);
	// Vox Deorum: optional human-to-human override (default false preserves stock behavior).
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 9, false);

	CvString sResult = pkDeal->GetReasonsItemUntradeable(eFromPlayer, eToPlayer, eItem, iData1, iData2, iData3, bFlag1, bTreatAsHumanToHuman);
	lua_pushstring(L, sResult);
	return 1;
}

#if defined(MOD_ACTIVE_DIPLOMACY)
//------------------------------------------------------------------------------
// Vox Deorum: validate the complete scratch deal with an optional human-to-human override.
int CvLuaDeal::lAreAllTradeItemsValid(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 2, false);
	const bool bResult = pkDeal->AreAllTradeItemsValid(bTreatAsHumanToHuman);

	lua_pushboolean(L, bResult);
	return 1;
}
#endif

//------------------------------------------------------------------------------
// Vox Deorum: read-only per-item AI value estimate, computed BOTH directions.
// Wraps CvDealAI::GetTradeItemValue without ever touching the acceptance/enact path.
// Returns two integers: the value to the giver of parting with the item (bFromMe = true,
// from eFromPlayer's perspective) and the value to the receiver of gaining it
// (bFromMe = false, from eToPlayer's perspective). The valuation-layer anti-exploit
// INT_MAX sentinels (last strategic resource, last luxury while unhappy) surface here
// as estimates but gate nothing — acceptance is decided by the negotiation. (specs.md §4)
// Args: deal:GetTradeItemValue(eFromPlayer, eToPlayer, eItem, iData1, iData2, iData3, bFlag1, iDuration)
int CvLuaDeal::lGetTradeItemValue(lua_State* L)
{
	const PlayerTypes eFromPlayer = (PlayerTypes) lua_tointeger(L, 2);
	const PlayerTypes eToPlayer = (PlayerTypes) lua_tointeger(L, 3);
	const TradeableItems eItem = (TradeableItems) lua_tointeger(L, 4);
	const int iData1 = luaL_optint(L, 5, -1);
	const int iData2 = luaL_optint(L, 6, -1);
	const int iData3 = luaL_optint(L, 7, -1);
	const bool bFlag1 = luaL_optbool(L, 8, false);
	const int iDuration = luaL_optint(L, 9, -1);

	// Value to the giver of giving the item away (from eFromPlayer's perspective).
	const int iValueToGiver = GET_PLAYER(eFromPlayer).GetDealAI()->GetTradeItemValue(
		eItem, /*bFromMe*/ true, eToPlayer, iData1, iData2, iData3, bFlag1, iDuration, /*bIsAIOffer*/ false, /*bEqualize*/ true);
	// Value to the receiver of gaining the item (from eToPlayer's perspective).
	const int iValueToReceiver = GET_PLAYER(eToPlayer).GetDealAI()->GetTradeItemValue(
		eItem, /*bFromMe*/ false, eFromPlayer, iData1, iData2, iData3, bFlag1, iDuration, /*bIsAIOffer*/ false, /*bEqualize*/ true);

	lua_pushinteger(L, iValueToGiver);
	lua_pushinteger(L, iValueToReceiver);
	return 2;
}

//------------------------------------------------------------------------------
// Vox Deorum: enact this (scratch) deal for real — the agent-diplomacy write path
// (interactive-diplomacy stage 6). A thin wrapper over the existing FinalizeMPDeal chain:
// acceptance is PRE-DECIDED (bAccepted = true), so CvDealAI is NEVER consulted, and the
// human-to-human override (bTreatAsHumanToHuman = true) is threaded through so AI-only
// structural restrictions don't gate the deal while structural legality is still honored.
// The renew-deal ID for the pair is pre-cleared so ActivateDeal's renewal diff can't clobber
// an unrelated pending renewal. FinalizeMPDeal takes a copy of the deal (the scratch deal is
// cleared by the caller afterward). Returns the enactment success bool.
// Usage: local ok = deal:Enact()
int CvLuaDeal::lEnact(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = pkDeal->GetFromPlayer();
	const PlayerTypes eToPlayer = pkDeal->GetToPlayer();

	// Pre-clear any pending renewal for this pair so it is not clobbered by ActivateDeal.
	GC.getGame().GetGameDeals().SetRenewDealID(eFromPlayer, eToPlayer, -1);

	const bool bResult = GC.getGame().GetGameDeals().FinalizeMPDeal(*pkDeal, /*bAccepted*/ true, /*bTreatAsHumanToHuman*/ true);

	lua_pushboolean(L, bResult);
	return 1;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lBlockTemporaryForPermanentTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const TradeableItems eItem = (TradeableItems) lua_tointeger(L, 2);
	const PlayerTypes eFromPlayer = (PlayerTypes) lua_tointeger(L, 3);
	const PlayerTypes eToPlayer = (PlayerTypes) lua_tointeger(L, 4);

	const bool bResult = pkDeal->BlockTemporaryForPermanentTrade(eItem, eFromPlayer, eToPlayer);
	lua_pushboolean(L, bResult);
	return 1;
}


//------------------------------------------------------------------------------
int CvLuaDeal::lResetIterator(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	m_iterator = pkDeal->m_TradedItems.begin();
	return 0;
}

//------------------------------------------------------------------------------
int CvLuaDeal::lGetNextItem(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);

	if(m_iterator == pkDeal->m_TradedItems.end())
		return 0;

	const CvTradedItem& item = (*m_iterator);
	lua_pushinteger(L, item.m_eItemType);
	lua_pushinteger(L, item.m_iDuration);
	lua_pushinteger(L, item.m_iFinalTurn);
	lua_pushinteger(L, item.m_iData1);
	lua_pushinteger(L, item.m_iData2);
	lua_pushinteger(L, item.m_iData3);
	lua_pushboolean(L, item.m_bFlag1);
	lua_pushinteger(L, item.m_eFromPlayer);

	m_iterator++;

	return 8;
}

//------------------------------------------------------------------------------
int CvLuaDeal::lGetRenewDealMessage(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const PlayerTypes eOtherPlayer = (PlayerTypes)lua_tointeger(L, 3);

	int iDealValueToMe = 0;
	bool bCantMatchOffer = false;
	bool bDealAcceptable = GET_PLAYER(eFromPlayer).GetDealAI()->IsDealWithHumanAcceptable(pkDeal, eOtherPlayer, iDealValueToMe, &bCantMatchOffer, false);
	DiploMessageTypes eMessage = bDealAcceptable ? DIPLO_MESSAGE_RENEW_DEAL : DIPLO_MESSAGE_WANT_MORE_RENEW_DEAL;
	lua_pushstring(L, GET_PLAYER(eFromPlayer).GetDiplomacyAI()->GetDiploStringForMessage(eMessage));
	return 1;
}
//------------------------------------------------------------------------------
// Vox Deorum: change gold with an optional human-to-human legality override.
int CvLuaDeal::lChangeGoldTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iAmount = lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);
	const bool bResult = pkDeal->ChangeGoldTrade(eFromPlayer, iAmount, bTreatAsHumanToHuman);

	lua_pushboolean(L, bResult);
	return 1;
}
//------------------------------------------------------------------------------
// Vox Deorum: change gold per turn with an optional human-to-human legality override.
int CvLuaDeal::lChangeGoldPerTurnTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iAmount = lua_tointeger(L, 3);
	const int iDuration = lua_tointeger(L, 4);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 5, false);
	const bool bResult = pkDeal->ChangeGoldPerTurnTrade(eFromPlayer, iAmount, iDuration, bTreatAsHumanToHuman);

	lua_pushboolean(L, bResult);
	return 1;
}
//------------------------------------------------------------------------------
// Vox Deorum: change a resource amount with an optional human-to-human legality override.
int CvLuaDeal::lChangeResourceTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const ResourceTypes eResource = (ResourceTypes)lua_tointeger(L, 3);
	const int iAmount = lua_tointeger(L, 4);
	const int iDuration = lua_tointeger(L, 5);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 6, false);
	const bool bResult = pkDeal->ChangeResourceTrade(eFromPlayer, eResource, iAmount, iDuration, bTreatAsHumanToHuman);

	lua_pushboolean(L, bResult);
	return 1;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddGoldTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iAmount = lua_tointeger(L, 3);
	// Vox Deorum: optional human-to-human override (default false = stock). The game's own
	// TradeLogic.lua callers pass fewer args and keep stock behavior; the enact path passes true.
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddGoldTrade(eFromPlayer, iAmount, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddGoldPerTurnTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iAmount = lua_tointeger(L, 3);
	const int iDuration = lua_tointeger(L, 4);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 5, false);

	pkDeal->AddGoldPerTurnTrade(eFromPlayer, iAmount, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddMapTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 3, false);

	pkDeal->AddMapTrade(eFromPlayer, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddResourceTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const ResourceTypes eResource = (ResourceTypes)lua_tointeger(L, 3);
	const int iAmount = lua_tointeger(L, 4);
	const int iDuration = lua_tointeger(L, 5);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 6, false);

	pkDeal->AddResourceTrade(eFromPlayer, eResource, iAmount, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddCityTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iCityID = lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddCityTrade(eFromPlayer, iCityID, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddAllowEmbassy(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 3, false);

	pkDeal->AddAllowEmbassy(eFromPlayer, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddOpenBorders(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iDuration = lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddOpenBorders(eFromPlayer, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddDefensivePact(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iDuration = lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddDefensivePact(eFromPlayer, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddResearchAgreement(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iDuration = lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddResearchAgreement(eFromPlayer, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddPeaceTreaty(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const int iDuration = lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddPeaceTreaty(eFromPlayer, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddThirdPartyPeace(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const TeamTypes eThirdPartyTeam = (TeamTypes)lua_tointeger(L, 3);
	const int iDuration = lua_tointeger(L, 4);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 5, false);

	pkDeal->AddThirdPartyPeace(eFromPlayer, eThirdPartyTeam, iDuration, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddThirdPartyWar(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const TeamTypes eThirdPartyTeam = (TeamTypes)lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddThirdPartyWar(eFromPlayer, eThirdPartyTeam, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddDeclarationOfFriendship(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 3, false);

	pkDeal->AddDeclarationOfFriendship(eFromPlayer, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddVoteCommitment(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes) lua_tointeger(L, 2);
	const int iResolutionID = lua_tointeger(L, 3);
	const int iVoteChoice = lua_tointeger(L, 4);
	const int iNumVotes = lua_tointeger(L, 5);
	const bool bRepeal = lua_toboolean(L, 6);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 7, false);

	pkDeal->AddVoteCommitment(eFromPlayer, iResolutionID, iVoteChoice, iNumVotes, bRepeal, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddTechTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const TechTypes eTech = (TechTypes)lua_tointeger(L, 3);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 4, false);

	pkDeal->AddTechTrade(eFromPlayer, eTech, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddVassalageTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 3, false);

	pkDeal->AddVassalageTrade(eFromPlayer, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}
//------------------------------------------------------------------------------
int CvLuaDeal::lAddRevokeVassalageTrade(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes)lua_tointeger(L, 2);
	const bool bTreatAsHumanToHuman = luaL_optbool(L, 3, false);

	pkDeal->AddRevokeVassalageTrade(eFromPlayer, /*bDoNotRemove*/ true, bTreatAsHumanToHuman);
	return 0;
}

//------------------------------------------------------------------------------
int CvLuaDeal::lRemoveVoteCommitment(lua_State* L)
{
	CvDeal* pkDeal = GetInstance(L);
	const PlayerTypes eFromPlayer = (PlayerTypes) lua_tointeger(L, 2);
	const int iResolutionID = lua_tointeger(L, 3);
	const int iVoteChoice = lua_tointeger(L, 4);
	const int iNumVotes = lua_tointeger(L, 5);
	const bool bRepeal = lua_toboolean(L, 6);

	pkDeal->RemoveVoteCommitment(eFromPlayer, iResolutionID, iVoteChoice, iNumVotes, bRepeal);
	return 0;
}

//------------------------------------------------------------------------------
// DEPRECATED
int CvLuaDeal::lAddUnitTrade(lua_State* L)
{
	luaL_error(L, "AddUnitTrade - function is deprecated");
}
int CvLuaDeal::lAddTradeAgreement(lua_State* L)
{
	luaL_error(L, "AddTradeAgreement - function is deprecated");
}
int CvLuaDeal::lAddPermamentAlliance(lua_State* L)
{
	luaL_error(L, "AddPermamentAlliance - function is deprecated");
}
int CvLuaDeal::lAddSurrender(lua_State* L)
{
	luaL_error(L, "AddSurrender - function is deprecated");
}
int CvLuaDeal::lAddTruce(lua_State* L)
{
	luaL_error(L, "AddTruce - function is deprecated");
}
int CvLuaDeal::lAddThirdPartyEmbargo(lua_State* L)
{
	luaL_error(L, "AddThirdPartyEmbargo - function is deprecated");
}
int CvLuaDeal::lRemoveUnitTrade(lua_State* L)
{
	luaL_error(L, "RemoveUnitTrade - function is deprecated");
}
int CvLuaDeal::lRemoveThirdPartyEmbargo(lua_State* L)
{
	luaL_error(L, "RemoveThirdPartyEmbargo - function is deprecated");
}
int CvLuaDeal::lChangeThirdPartyEmbargoDuration(lua_State* L)
{
	luaL_error(L, "ChangeThirdPartyEmbargoDuration - function is deprecated");
}
