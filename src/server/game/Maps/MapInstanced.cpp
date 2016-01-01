/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MapInstanced.h"
#include "ObjectMgr.h"
#include "MapManager.h"
#include "Battleground.h"
#include "VMapFactory.h"
#include "MMapFactory.h"
#include "InstanceSaveMgr.h"
#include "World.h"
#include "Group.h"
#include "Player.h"

MapInstanced::MapInstanced(uint32 id, time_t expiry) : Map(id, expiry, 0, DUNGEON_DIFFICULTY_NORMAL)
{
    // initialize instanced maps list
    m_InstancedMaps.clear();
    // fill with zero
    memset(&GridMapReference, 0, MAX_NUMBER_OF_GRIDS*MAX_NUMBER_OF_GRIDS*sizeof(uint16));
}

void MapInstanced::InitVisibilityDistance()
{
    if (m_InstancedMaps.empty())
        return;
    //initialize visibility distances for all instance copies
    for (InstancedMaps::iterator i = m_InstancedMaps.begin(); i != m_InstancedMaps.end(); ++i)
    {
        (*i).second->InitVisibilityDistance();
    }
}

void MapInstanced::Update(const uint32 t)
{
    // take care of loaded GridMaps (when unused, unload it!)
    Map::Update(t);

    // update the instanced maps
    InstancedMaps::iterator i = m_InstancedMaps.begin();

    while (i != m_InstancedMaps.end())
    {
        if (i->second->CanUnload(t))
        {
            if (!DestroyInstance(i))                             // iterator incremented
            {
                //m_unloadTimer
            }
        }
        else
        {
            // update only here, because it may schedule some bad things before delete
            if (sMapMgr->GetMapUpdater()->activated())
                sMapMgr->GetMapUpdater()->schedule_update(*i->second, t);
            else
                i->second->Update(t);
            ++i;
        }
    }
}

void MapInstanced::DelayedUpdate(const uint32 diff)
{
    for (InstancedMaps::iterator i = m_InstancedMaps.begin(); i != m_InstancedMaps.end(); ++i)
        i->second->DelayedUpdate(diff);

    Map::DelayedUpdate(diff); // this may be removed
}

/*
void MapInstanced::RelocationNotify()
{
    for (InstancedMaps::iterator i = m_InstancedMaps.begin(); i != m_InstancedMaps.end(); ++i)
        i->second->RelocationNotify();
}
*/

void MapInstanced::UnloadAll()
{
    // Unload instanced maps
    for (InstancedMaps::iterator i = m_InstancedMaps.begin(); i != m_InstancedMaps.end(); ++i)
        i->second->UnloadAll();

    // Delete the maps only after everything is unloaded to prevent crashes
    for (InstancedMaps::iterator i = m_InstancedMaps.begin(); i != m_InstancedMaps.end(); ++i)
        delete i->second;

    m_InstancedMaps.clear();

    // Unload own grids (just dummy(placeholder) grids, neccesary to unload GridMaps!)
    Map::UnloadAll();
}

/*
- return the right instance for the object, based on its InstanceId
- create the instance if it's not created already
- the player is not actually added to the instance (only in InstanceMap::Add)
*/
Map* MapInstanced::CreateInstanceForPlayer(const uint32 mapId, Player* player)
{
    if (GetId() != mapId || !player)
        return NULL;

    Map* map = NULL;
    uint32 newInstanceId = 0;                       // instanceId of the resulting map

    if (IsBattlegroundOrArena())
    {
        // instantiate or find existing bg map for player
        // the instance id is set in battlegroundid
        newInstanceId = player->GetBattlegroundId();
        if (!newInstanceId)
            return NULL;

        map = sMapMgr->FindMap(mapId, newInstanceId);
        if (!map)
        {
            if (Battleground* bg = player->GetBattleground())
                map = CreateBattleground(newInstanceId, bg);
            else
            {
                player->TeleportToBGEntryPoint();
                return NULL;
            }
        }
    }
    else
    {
        InstancePlayerBind* pBind = player->GetBoundInstance(GetId(), player->GetDifficulty(IsRaid()), player->GetGroup() ? player->GetGroup()->isLFGGroup() : false);
        InstanceSave* pSave = pBind ? pBind->save : NULL;

        // the player's permanent player bind is taken into consideration first
        // then the player's group bind and finally the solo bind.
        if (!pBind || !pBind->perm)
        {
            InstanceGroupBind* groupBind = NULL;
            Group* group = player->GetGroup();
            // use the player's difficulty setting (it may not be the same as the group's)
            if (group)
            {
                groupBind = group->GetBoundInstance(this, group->isLFGGroup());
                if (groupBind)
                    pSave = groupBind->save;
            }
        }
        if (pSave)
        {
            // solo/perm/group
            newInstanceId = pSave->GetInstanceId();
            map = FindInstanceMap(newInstanceId);

            // Shared locks: create an instance to match the current player raid difficulty, if the save and player difficulties don't match.
            // We must check for save difficulty going original diff -> new one, and map spawn mode going new -> original, to make sure all cases are handled.
            // Although Heroic 10 / 25 Man also theoretically share a cooldown, if you kill a boss on 10 / 25 Heroic you cannot enter any other Heroic size version of the raid (cannot switch).
            // Heroic size switching is already handled with no checks needed. The map is created on the save difficulty and you can only switch difficulty dynamically, from inside.
            if (IsRaid() && (pSave->GetDifficulty() == RAID_DIFFICULTY_10MAN_NORMAL || pSave->GetDifficulty() == RAID_DIFFICULTY_25MAN_NORMAL))
            {
                // Normal. The map is created on the player difficulty.
                if (player->GetDifficulty(IsRaid()) != pSave->GetDifficulty() || map && map->GetSpawnMode() != player->GetDifficulty(IsRaid()))
                    map = CreateInstance(newInstanceId, pSave, player->GetDifficulty(IsRaid()));
            }

            // it is possible that the save exists but the map doesn't
            if (!map)
                map = CreateInstance(newInstanceId, pSave, pSave->GetDifficulty());
        }
        else
        {
            // If no instanceId via group members or instance saves is found, the instance will be created for the first time.
            newInstanceId = sMapMgr->GenerateInstanceId();

            Difficulty diff = player->GetGroup() ? player->GetGroup()->GetDifficulty(IsRaid()) : player->GetDifficulty(IsRaid());

            map = FindInstanceMap(newInstanceId);
            if (!map)
                map = CreateInstance(newInstanceId, NULL, diff);
        }
    }

    return map;
}

InstanceMap* MapInstanced::CreateInstance(uint32 InstanceId, InstanceSave* save, Difficulty difficulty)
{
    // load/create a map
    TRINITY_GUARD(ACE_Thread_Mutex, Lock);

    // make sure we have a valid map id
    const MapEntry* entry = sMapStore.LookupEntry(GetId());
    if (!entry)
    {
        sLog->outError(LOG_FILTER_MAPS, "CreateInstance: no entry for map %d", GetId());
        ASSERT(false);
    }
    const InstanceTemplate* iTemplate = sObjectMgr->GetInstanceTemplate(GetId());
    if (!iTemplate)
    {
        sLog->outError(LOG_FILTER_MAPS, "CreateInstance: no instance template for map %d", GetId());
        ASSERT(false);
    }

    // some instances only have one difficulty
    GetDownscaledMapDifficultyData(GetId(), difficulty);

    sLog->outDebug(LOG_FILTER_MAPS, "MapInstanced::CreateInstance: %s map instance %d for %d created with difficulty %s", save?"":"new ", InstanceId, GetId(), difficulty?"heroic":"normal");

    InstanceMap* map = new InstanceMap(GetId(), GetGridExpiry(), InstanceId, difficulty, this);
    ASSERT(map->IsDungeon());

    map->LoadRespawnTimes();

    bool load_data = save != NULL;
    map->CreateInstanceData(load_data);

    // memory LEAK here !!!
    m_InstancedMaps[InstanceId] = map;
    return map;
}

BattlegroundMap* MapInstanced::CreateBattleground(uint32 InstanceId, Battleground* bg)
{
    // load/create a map
    TRINITY_GUARD(ACE_Thread_Mutex, Lock);

    sLog->outDebug(LOG_FILTER_MAPS, "MapInstanced::CreateBattleground: map bg %d for %d created.", InstanceId, GetId());

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), bg->GetMinLevel());

    uint8 spawnMode;

    if (bracketEntry)
        spawnMode = bracketEntry->difficulty;
    else
        spawnMode = REGULAR_DIFFICULTY;

    BattlegroundMap* map = new BattlegroundMap(GetId(), GetGridExpiry(), InstanceId, this, spawnMode);
    ASSERT(map->IsBattlegroundOrArena());
    map->SetBG(bg);
    bg->SetBgMap(map);

    m_InstancedMaps[InstanceId] = map;
    return map;
}

// increments the iterator after erase
bool MapInstanced::DestroyInstance(InstancedMaps::iterator &itr)
{
    itr->second->RemoveAllPlayers();
    if (itr->second->HavePlayers())
    {
        ++itr;
        return false;
    }

    itr->second->UnloadAll();
    // should only unload VMaps if this is the last instance and grid unloading is enabled
    if (m_InstancedMaps.size() <= 1 && sWorld->getBoolConfig(CONFIG_GRID_UNLOAD))
    {
        VMAP::VMapFactory::createOrGetVMapManager()->unloadMap(itr->second->GetId());
        MMAP::MMapFactory::createOrGetMMapManager()->unloadMap(itr->second->GetId());
        // in that case, unload grids of the base map, too
        // so in the next map creation, (EnsureGridCreated actually) VMaps will be reloaded
        Map::UnloadAll();
    }

    // Free up the instance id and allow it to be reused for bgs and arenas (other instances are handled in the InstanceSaveMgr)
    if (itr->second->IsBattlegroundOrArena())
        sMapMgr->FreeInstanceId(itr->second->GetInstanceId());

    // erase map
    delete itr->second;
    m_InstancedMaps.erase(itr++);

    return true;
}

bool MapInstanced::CanEnter(Player* /*player*/)
{
    //ASSERT(false);
    return true;
}
