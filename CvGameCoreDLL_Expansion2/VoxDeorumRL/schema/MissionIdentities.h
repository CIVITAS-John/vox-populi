// Installed from simulator/schema/shared/MissionIdentities.h. Edit that source, then reinstall.

#ifndef VOX_RL_MISSION_IDENTITIES_H
#define VOX_RL_MISSION_IDENTITIES_H

// Lists every native CvTypes mission getter in CvTypes.h declaration order. STATIC stores
// the captured identity at each entry's position, so capture and the simulator facade both
// expand this one list: X(name) names CvTypes::getMISSION_<name>.
#define VOX_RL_MISSION_IDENTITIES(X) \
    X(MOVE_TO) \
    X(ROUTE_TO) \
    X(MOVE_TO_UNIT) \
    X(SWAP_UNITS) \
    X(SKIP) \
    X(SLEEP) \
    X(ALERT) \
    X(FORTIFY) \
    X(GARRISON) \
    X(SET_UP_FOR_RANGED_ATTACK) \
    X(EMBARK) \
    X(DISEMBARK) \
    X(AIRPATROL) \
    X(HEAL) \
    X(AIRLIFT) \
    X(NUKE) \
    X(PARADROP) \
    X(AIR_SWEEP) \
    X(REBASE) \
    X(RANGE_ATTACK) \
    X(PILLAGE) \
    X(FOUND) \
    X(JOIN) \
    X(CONSTRUCT) \
    X(DISCOVER) \
    X(HURRY) \
    X(TRADE) \
    X(BUY_CITY_STATE) \
    X(REPAIR_FLEET) \
    X(SPACESHIP) \
    X(CULTURE_BOMB) \
    X(FOUND_RELIGION) \
    X(GOLDEN_AGE) \
    X(BUILD) \
    X(LEAD) \
    X(DIE_ANIMATION) \
    X(BEGIN_COMBAT) \
    X(END_COMBAT) \
    X(AIRSTRIKE) \
    X(SURRENDER) \
    X(CAPTURED) \
    X(IDLE) \
    X(DIE) \
    X(DAMAGE) \
    X(MULTI_SELECT) \
    X(MULTI_DESELECT) \
    X(WAIT_FOR) \
    X(SPREAD_RELIGION) \
    X(ENHANCE_RELIGION) \
    X(REMOVE_HERESY) \
    X(ESTABLISH_TRADE_ROUTE) \
    X(PLUNDER_TRADE_ROUTE) \
    X(GREAT_WORK) \
    X(CHANGE_TRADE_UNIT_HOME_CITY) \
    X(SELL_EXOTIC_GOODS) \
    X(GIVE_POLICIES) \
    X(ONE_SHOT_TOURISM) \
    X(CHANGE_ADMIRAL_PORT) \
    X(FREE_LUXURY)

// Names each list position as a STATIC missionTypes slot.
#define VOX_RL_MISSION_IDENTITY_SLOT(name) kMissionIdentity_##name,
enum MissionIdentitySlot
{
    VOX_RL_MISSION_IDENTITIES(VOX_RL_MISSION_IDENTITY_SLOT)
    kMissionIdentityCount
};
#undef VOX_RL_MISSION_IDENTITY_SLOT

#endif
