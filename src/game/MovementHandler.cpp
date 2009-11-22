/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "Corpse.h"
#include "Player.h"
#include "MapManager.h"
#include "Transports.h"
#include "BattleGround.h"
#include "WaypointMovementGenerator.h"
#include "InstanceSaveMgr.h"
#include "ObjectMgr.h"
#include "World.h"

/*Movement anticheat DEBUG defines */
//#define MOVEMENT_ANTICHEAT_DEBUG true
/*end Movement anticheate defines*/
void WorldSession::HandleMoveWorldportAckOpcode( WorldPacket & /*recv_data*/ )
{
    sLog.outDebug( "WORLD: got MSG_MOVE_WORLDPORT_ACK." );
    HandleMoveWorldportAckOpcode();
}

void WorldSession::HandleMoveWorldportAckOpcode()
{
    // ignore unexpected far teleports
    if(!GetPlayer()->IsBeingTeleportedFar())
        return;

    // get the teleport destination
    WorldLocation &loc = GetPlayer()->GetTeleportDest();

    // possible errors in the coordinate validity check
    if(!MapManager::IsValidMapCoord(loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation))
    {
        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: player got's teleported far to a not valid location. (map:%u, x:%f, y:%f, z:%f) We log him out and don't save him..", loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);
        // stop teleportation else we would try this again in the beginning of WorldSession::LogoutPlayer...
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // player don't gets saved - so his coords will stay at the point where
        // he was last saved
        LogoutPlayer(false);
        return;
    }
    //reset falltimer at teleport
    GetPlayer()->m_anti_justteleported = 1;
    // get the destination map entry, not the current one, this will fix homebind and reset greeting
    MapEntry const* mEntry = sMapStore.LookupEntry(loc.mapid);
    InstanceTemplate const* mInstance = ObjectMgr::GetInstanceTemplate(loc.mapid);

    // reset instance validity, except if going to an instance inside an instance
    if(GetPlayer()->m_InstanceValid == false && !mInstance)
        GetPlayer()->m_InstanceValid = true;

    GetPlayer()->SetSemaphoreTeleportFar(false);

    // relocate the player to the teleport destination
    GetPlayer()->SetMapId(loc.mapid);
    GetPlayer()->Relocate(loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation);

    // since the MapId is set before the GetInstance call, the InstanceId must be set to 0
    // to let GetInstance() determine the proper InstanceId based on the player's binds
    GetPlayer()->SetInstanceId(0);

    // check this before Map::Add(player), because that will create the instance save!
    bool reset_notify = (GetPlayer()->GetBoundInstance(GetPlayer()->GetMapId(), GetPlayer()->GetDifficulty()) == NULL);

    GetPlayer()->SendInitialPacketsBeforeAddToMap();
    // the CanEnter checks are done in TeleporTo but conditions may change
    // while the player is in transit, for example the map may get full
    if(!GetPlayer()->GetMap()->Add(GetPlayer()))
    {
        sLog.outDebug("WORLD: teleport of player %s (%d) to location %d, %f, %f, %f, %f failed", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation);
        // teleport the player home
        if(!GetPlayer()->TeleportTo(GetPlayer()->m_homebindMapId, GetPlayer()->m_homebindX, GetPlayer()->m_homebindY, GetPlayer()->m_homebindZ, GetPlayer()->GetOrientation()))
        {
            // the player must always be able to teleport home
            sLog.outError("WORLD: failed to teleport player %s (%d) to homebind location %d, %f, %f, %f, %f!", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow(), GetPlayer()->m_homebindMapId, GetPlayer()->m_homebindX, GetPlayer()->m_homebindY, GetPlayer()->m_homebindZ, GetPlayer()->GetOrientation());
            assert(false);
        }
        return;
    }
    GetPlayer()->SendInitialPacketsAfterAddToMap();

    // flight fast teleport case
    if(GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
    {
        if(!_player->InBattleGround())
        {
            // short preparations to continue flight
            FlightPathMovementGenerator* flight = (FlightPathMovementGenerator*)(GetPlayer()->GetMotionMaster()->top());
            flight->Initialize(*GetPlayer());
            return;
        }

        // battleground state prepare, stop flight
        GetPlayer()->GetMotionMaster()->MovementExpired();
        GetPlayer()->m_taxi.ClearTaxiDestinations();
    }

    // resurrect character at enter into instance where his corpse exist after add to map
    Corpse *corpse = GetPlayer()->GetCorpse();
    if (corpse && corpse->GetType() != CORPSE_BONES && corpse->GetMapId() == GetPlayer()->GetMapId())
    {
        if( mEntry->IsDungeon() )
        {
            GetPlayer()->ResurrectPlayer(0.5f);
            GetPlayer()->SpawnCorpseBones();
        }
    }

    if(mEntry->IsRaid() && mInstance)
    {
        if(reset_notify)
        {
            uint32 timeleft = sInstanceSaveMgr.GetResetTimeFor(GetPlayer()->GetMapId()) - time(NULL);
            GetPlayer()->SendInstanceResetWarning(GetPlayer()->GetMapId(), timeleft); // greeting at the entrance of the resort raid instance
        }
    }

    // mount allow check
    if(!mEntry->IsMountAllowed())
        _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // battleground state prepare
    // only add to bg group and object, if the player was invited (else he entered through command)
    if(_player->InBattleGround() && _player->IsInvitedForBattleGroundInstance(_player->GetBattleGroundId()))
    {
        BattleGround *bg = _player->GetBattleGround();
        if(bg)
        {
            bg->AddPlayer(_player);
            if(bg->GetMapId() == _player->GetMapId())       // we teleported to bg
            {
                // get the team this way, because arenas might 'override' the teams.
                uint32 team = bg->GetPlayerTeam(_player->GetGUID());
                if(!team)
                    team = _player->GetTeam();
                if(!bg->GetBgRaid(team))      // first player joined
                {
                    Group *group = new Group;
                    bg->SetBgRaid(team, group);
                    group->Create(_player->GetGUIDLow(), _player->GetName());
                }
                else                                        // raid already exist
                {
                    bg->GetBgRaid(team)->AddMember(_player->GetGUID(), _player->GetName());
                }
            }
        }
    }

    // honorless target
    if(GetPlayer()->pvpInfo.inHostileArea)
        GetPlayer()->CastSpell(GetPlayer(), 2479, true);

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMoveTeleportAck(WorldPacket& recv_data)
{
    sLog.outDebug("MSG_MOVE_TELEPORT_ACK");
    uint64 guid;
    uint32 flags, time;

    recv_data >> guid;
    recv_data >> flags >> time;
    DEBUG_LOG("Guid " UI64FMTD, guid);
    DEBUG_LOG("Flags %u, time %u", flags, time/IN_MILISECONDS);

    Player* plMover = GetPlayer();

    if(!plMover || !plMover->IsBeingTeleportedNear())
        return;

    if(guid != plMover->GetGUID())
        return;

    plMover->SetSemaphoreTeleportNear(false);

    uint32 old_zone = plMover->GetZoneId();

    WorldLocation const& dest = plMover->GetTeleportDest();

    plMover->SetPosition(dest.coord_x, dest.coord_y, dest.coord_z, dest.orientation, true);

    uint32 newzone, newarea;
    plMover->GetZoneAndAreaId(newzone, newarea);
    plMover->UpdateZone(newzone, newarea);

    // new zone
    if(old_zone != newzone)
    {
        // honorless target
        if(plMover->pvpInfo.inHostileArea)
            plMover->CastSpell(plMover, 2479, true);
    }

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMovementOpcodes( WorldPacket & recv_data )
{
    uint32 opcode = recv_data.GetOpcode();
    sLog.outDebug("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if(GetPlayer()->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        GetPlayer()->m_anti_justteleported = 1;
        return;
    }

    /* extract packet */
    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);
    /*----------------*/

    if(recv_data.size() != recv_data.rpos())
    {
        sLog.outError("MovementHandler: player %s (guid %d, account %u) sent a packet (opcode %u) that is " SIZEFMTD " bytes larger than it should be. Kicked as cheater.", _player->GetName(), _player->GetGUIDLow(), _player->GetSession()->GetAccountId(), recv_data.GetOpcode(), recv_data.size() - recv_data.rpos());
        KickPlayer();
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    if (!MaNGOS::IsValidMapCoord(movementInfo.x, movementInfo.y, movementInfo.z, movementInfo.o))
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    /* handle special cases */
    if (movementInfo.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
    {
        // transports size limited
        // (also received at zeppelin leave by some reason with t_* as absolute in continent coordinates, can be safely skipped)
        if( movementInfo.t_x > 60 || movementInfo.t_y > 60 || movementInfo.t_x < -60 ||  movementInfo.t_y < -60 )
        {
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
        }

        if( !MaNGOS::IsValidMapCoord(movementInfo.x+movementInfo.t_x, movementInfo.y+movementInfo.t_y,
            movementInfo.z+movementInfo.t_z, movementInfo.o + movementInfo.t_o) )
        {
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
        }

        if (GetPlayer()->m_anti_transportGUID == 0 && (movementInfo.t_guid !=0))
        {
            // if we boarded a transport, add us to it
            if (!GetPlayer()->m_transport)
            {
                // elevators also cause the client to send MOVEMENTFLAG_ONTRANSPORT - just unmount if the guid can be found in the transport list
                for (MapManager::TransportSet::const_iterator iter = sMapMgr.m_Transports.begin(); iter != sMapMgr.m_Transports.end(); ++iter)
                {
                    // unmount before boarding
                    _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

                    if ((*iter)->GetGUID() == movementInfo.t_guid)
                    {
                        GetPlayer()->m_transport = (*iter);
                        (*iter)->AddPassenger(GetPlayer());
                        break;
                    }
                }
            }
            //movement anticheat;
            //Correct finding GO guid in DB (thanks to GriffonHeart)
            GameObject *obj = GetPlayer()->GetMap()->GetGameObject(movementInfo.t_guid);
            if(obj)
                GetPlayer()->m_anti_transportGUID = obj->GetDBTableGUIDLow();
            else
                GetPlayer()->m_anti_transportGUID = GUID_LOPART(movementInfo.t_guid);
            // end movement anticheat
        }
    } else if (GetPlayer()->m_anti_transportGUID != 0){
        if (GetPlayer()->m_transport)               // if we were on a transport, leave
        {
            GetPlayer()->m_transport->RemovePassenger(GetPlayer());
            GetPlayer()->m_transport = NULL;
        }
        movementInfo.t_x = 0.0f;
        movementInfo.t_y = 0.0f;
        movementInfo.t_z = 0.0f;
        movementInfo.t_o = 0.0f;
        movementInfo.t_time = 0;
        GetPlayer()->m_anti_transportGUID = 0;
    }

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == MSG_MOVE_FALL_LAND && !GetPlayer()->isInFlight())
    {
        //movement anticheat
        GetPlayer()->m_anti_justjumped = 0;
        GetPlayer()->m_anti_jumpbase = 0;
        //end movement anticheat
        GetPlayer()->HandleFall(movementInfo);
    }


    if (movementInfo.HasMovementFlag(MOVEMENTFLAG_SWIMMING) != GetPlayer()->IsInWater())
    {
        // now client not include swimming flag in case jumping under water
        GetPlayer()->SetInWater( !GetPlayer()->IsInWater() || GetPlayer()->GetBaseMap()->IsUnderWater(movementInfo.x, movementInfo.y, movementInfo.z) );
    }

    #ifdef MOVEMENT_ANTICHEAT_DEBUG
    sLog.outBasic("%s newcoord: tm:%d ftm:%d | %f,%f,%fo(%f) [%X][%s]| transport: %f,%f,%fo(%f)",GetPlayer()->GetName(),movementInfo.time,movementInfo.fallTime,movementInfo.x,movementInfo.y,movementInfo.z,movementInfo.o,MovementFlags, LookupOpcodeName(opcode),movementInfo.t_x,movementInfo.t_y,movementInfo.t_z,movementInfo.t_o);
    sLog.outBasic("Transport: %d |  tguid: %d - %d", GetPlayer()->m_anti_transportGUID, GUID_LOPART(movementInfo.t_guid), GUID_HIPART(movementInfo.t_guid));
    #endif
    /*----------------------*/
    //---- anti-cheat features -->>>
    bool check_passed = true;

    //calc time deltas
    int32 timedelta = 1500;
    if (GetPlayer()->m_anti_lastmovetime !=0){
        timedelta = movementInfo.time - GetPlayer()->m_anti_lastmovetime;
        GetPlayer()->m_anti_deltamovetime += timedelta;
        GetPlayer()->m_anti_lastmovetime = movementInfo.time;
    } else {
        GetPlayer()->m_anti_lastmovetime = movementInfo.time;
    }

    uint32 CurrentMStime=getMSTime();
    uint32 CurrentMStimeDelta = 1500;
    if (GetPlayer()->m_anti_lastMStime != 0){
        CurrentMStimeDelta = CurrentMStime - GetPlayer()->m_anti_lastMStime;
        GetPlayer()->m_anti_deltaMStime += CurrentMStimeDelta;
        GetPlayer()->m_anti_lastMStime = CurrentMStime;
    } else {
        GetPlayer()->m_anti_lastMStime = CurrentMStime;
    }

    //resync times on client login (first 15 sec for heavy areas)
    if (GetPlayer()->m_anti_deltaMStime < 15000 && GetPlayer()->m_anti_deltamovetime < 15000)
        GetPlayer()->m_anti_deltamovetime = GetPlayer()->m_anti_deltaMStime;

    int32 sync_time = GetPlayer()->m_anti_deltamovetime - GetPlayer()->m_anti_deltaMStime;

    #ifdef MOVEMENT_ANTICHEAT_DEBUG
    sLog.outBasic("dtime: %d, stime: %d || dMS: %d - dMV: %d || dt: %d", timedelta, CurTime, GetPlayer()->m_anti_deltaMStime,  GetPlayer()->m_anti_deltamovetime, sync_time);
    #endif

    uint32 curDest = GetPlayer()->m_taxi.GetTaxiDestination(); //check taxi flight
    if ((GetPlayer()->m_anti_transportGUID == 0) && World::GetEnableMvAnticheat() && !curDest)
    {
        UnitMoveType move_type;

        if (movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING)) move_type = movementInfo.HasMovementFlag(MOVEMENTFLAG_BACKWARD) ? MOVE_FLIGHT_BACK : MOVE_FLIGHT;
        else if (movementInfo.HasMovementFlag(MOVEMENTFLAG_SWIMMING)) move_type = movementInfo.HasMovementFlag(MOVEMENTFLAG_BACKWARD) ? MOVE_SWIM_BACK : MOVE_SWIM;
        else if (movementInfo.HasMovementFlag(MOVEMENTFLAG_WALK_MODE)) move_type = MOVE_WALK;
        //hmm... in first time after login player has MOVE_SWIMBACK instead MOVE_WALKBACK
        else move_type = movementInfo.HasMovementFlag(MOVEMENTFLAG_BACKWARD) ? MOVE_SWIM_BACK : MOVE_RUN;

        float allowed_delta= 0;
        float current_speed = GetPlayer()->GetSpeed(move_type);
        float delta_x = GetPlayer()->GetPositionX() - movementInfo.x;
        float delta_y = GetPlayer()->GetPositionY() - movementInfo.y;
        float delta_z = GetPlayer()->GetPositionZ() - movementInfo.z;
        float real_delta = delta_x * delta_x + delta_y * delta_y;
        float tg_z = -99999; //tangens

        int32 gmd = World::GetMistimingDelta();
        if (sync_time > gmd || sync_time < -gmd){
            timedelta = CurrentMStimeDelta;
            GetPlayer()->m_anti_mistiming_count++;

            //sLog.outError("Movement anticheat: %s is mistaming exception. Exception count: %d, mistiming: %d ms ", GetPlayer()->GetName(), GetPlayer()->m_anti_mistiming_count, sync_time);
            //if (GetPlayer()->m_anti_mistiming_count > World::GetMistimingAlarms())
            //{
                //GetPlayer()->GetSession()->KickPlayer();
                ///sLog.outError("Movement anticheat: %s is mistaming exception. Exception count: %d, mistiming: %d ms ", GetPlayer()->GetName(), GetPlayer()->m_anti_mistiming_count, sync_time);
            //}
            check_passed = false;
        }

        if (timedelta < 0) {timedelta = 0;}

        float time_delta = (timedelta < 1500) ? (float)timedelta/1000 : 1.5f; //normalize time - 1.5 second allowed for heavy loaded server

        if (!(movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING) || movementInfo.HasMovementFlag(MOVEMENTFLAG_SWIMMING)))
          tg_z = (real_delta !=0) ? (delta_z*delta_z / real_delta) : -99999;

        if (current_speed < GetPlayer()->m_anti_last_hspeed)
        {
            allowed_delta = GetPlayer()->m_anti_last_hspeed;
            if (GetPlayer()->m_anti_lastspeed_changetime == 0 )
                GetPlayer()->m_anti_lastspeed_changetime = movementInfo.time + (uint32)floor(((GetPlayer()->m_anti_last_hspeed / current_speed) * 1000)) + 100; //100ms above for random fluctuating =)))
        } else allowed_delta = current_speed;
        allowed_delta = allowed_delta * time_delta;
        allowed_delta = allowed_delta * allowed_delta + 2;

       // static char const* move_type_name[MAX_MOVE_TYPE] = {  "Walk", "Run", "Walkback", "Swim", "Swimback", "Turn", "Fly", "Flyback" };
       // sLog.outBasic("%s newcoord: tm:%d ftm:%d | %f,%f,%fo(%f) [%X][%s]$%s",GetPlayer()->GetName(),movementInfo.time,movementInfo.fallTime,movementInfo.x,movementInfo.y,movementInfo.z,movementInfo.o,MovementFlags, LookupOpcodeName(opcode),move_type_name[move_type]);
       // sLog.outBasic("%f",tg_z);
        //AntiGravitation (thanks to Meekro)
        float JumpHeight = GetPlayer()->m_anti_jumpbase - movementInfo.z;
        if ((GetPlayer()->m_anti_jumpbase != 0) 
                    && !(movementInfo.HasMovementFlag(MOVEMENTFLAG_SWIMMING) || movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING)
                         || movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING2))
                    && (JumpHeight < GetPlayer()->m_anti_last_vspeed)){
            #ifdef MOVEMENT_ANTICHEAT_DEBUG
            sLog.outError("Movement anticheat: %s is graviJump exception. dz=%f",GetPlayer()->GetName(), movementInfo.z - GetPlayer()->m_anti_jumpbase);
            #endif
            check_passed = false;
        }

        if (opcode == MSG_MOVE_JUMP && !GetPlayer()->IsInWater()){
            if (GetPlayer()->m_anti_justjumped >= 1){
                ///GetPlayer()->m_anti_justjumped = 0;
                check_passed = false; //don't process new jump packet
            } else {
                GetPlayer()->m_anti_justjumped += 1;
                GetPlayer()->m_anti_jumpbase = movementInfo.z;
            }
        } else if (GetPlayer()->IsInWater()) {
             GetPlayer()->m_anti_justjumped = 0;
        }
        if ((real_delta > allowed_delta)) //&& (delta_z < 1)
        {
            #ifdef MOVEMENT_ANTICHEAT_DEBUG
            sLog.outError("Movement anticheat: %s is speed exception. {real_delta=%f allowed_delta=%f | current_speed=%f preview_speed=%f time=%f}(%f %f %f %d)[%s]",GetPlayer()->GetName(),real_delta, allowed_delta, current_speed, GetPlayer()->m_anti_last_hspeed,time_delta,GetPlayer()->GetPositionX(),GetPlayer()->GetPositionY(),GetPlayer()->GetPositionZ(), GetPlayer()->GetMapId(),LookupOpcodeName(opcode));
            #endif
            check_passed = false;
        }
        if ((real_delta>4900.0f) && !(real_delta < allowed_delta))
        {
            #ifdef MOVEMENT_ANTICHEAT_DEBUG
            sLog.outError("Movement anticheat: %s is teleport exception. {real_delta=%f allowed_delta=%f | current_speed=%f preview_speed=%f time=%f}(%f %f %f %d)",GetPlayer()->GetName(),real_delta, allowed_delta, current_speed, GetPlayer()->m_anti_last_hspeed,time_delta,GetPlayer()->GetPositionX(),GetPlayer()->GetPositionY(),GetPlayer()->GetPositionZ(), GetPlayer()->GetMapId());
            #endif
            check_passed = false;
        }
        if (movementInfo.time>GetPlayer()->m_anti_lastspeed_changetime)
        {
            GetPlayer()->m_anti_last_hspeed = current_speed; // store current speed
            GetPlayer()->m_anti_last_vspeed = -2.3f;
            if (GetPlayer()->m_anti_lastspeed_changetime != 0) GetPlayer()->m_anti_lastspeed_changetime = 0;
        }

        if ((tg_z > 2.371f) && (delta_z < GetPlayer()->m_anti_last_vspeed) && opcode!=MSG_MOVE_HEARTBEAT)
        {
            #ifdef MOVEMENT_ANTICHEAT_DEBUG
            sLog.outError("Movement anticheat: %s is mountain exception. {tg_z=%f} (%f %f %f %d)",GetPlayer()->GetName(),tg_z, GetPlayer()->GetPositionX(),GetPlayer()->GetPositionY(),GetPlayer()->GetPositionZ(), GetPlayer()->GetMapId());
            #endif
            check_passed = false;
        }

       // if (((MovementFlags & MOVEMENTFLAG_LEVITATING) != 0) & !GetPlayer()->isGameMaster() && !(GetPlayer()->HasAuraType(SPELL_AURA_FLY))){
       // }

        if ( (movementInfo.HasMovementFlag(MOVEMENTFLAG_CAN_FLY) || movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING)
                    || movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING2))
              && !GetPlayer()->isGameMaster()
              && !(GetPlayer()->HasAuraType(SPELL_AURA_FLY) || GetPlayer()->HasAuraType(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED)) )
        {
            #ifdef MOVEMENT_ANTICHEAT_DEBUG
            sLog.outError("Movement anticheat: %s is fly cheater. {SPELL_AURA_FLY=[%X]} {SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED=[%X]} {SPELL_AURA_MOD_SPEED_FLIGHT=[%X]} {SPELL_AURA_MOD_FLIGHT_SPEED_ALWAYS=[%X]} {SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK=[%X]}",
               GetPlayer()->GetName(),
               GetPlayer()->HasAuraType(SPELL_AURA_FLY), GetPlayer()->HasAuraType(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED),
               GetPlayer()->HasAuraType(SPELL_AURA_MOD_SPEED_FLIGHT), GetPlayer()->HasAuraType(SPELL_AURA_MOD_FLIGHT_SPEED_ALWAYS),
               GetPlayer()->HasAuraType(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK));
            #endif
            check_passed = false;
        }
        if ( movementInfo.HasMovementFlag(MOVEMENTFLAG_WATERWALKING)
             && !GetPlayer()->isGameMaster()
             && !(GetPlayer()->HasAuraType(SPELL_AURA_WATER_WALK) || GetPlayer()->HasAuraType(SPELL_AURA_GHOST)) )
        {
            #ifdef MOVEMENT_ANTICHEAT_DEBUG
            sLog.outError("Movement anticheat: %s is water-walk exception. [%X]{SPELL_AURA_WATER_WALK=[%X]}", GetPlayer()->GetName(), MovementFlags, GetPlayer()->HasAuraType(SPELL_AURA_WATER_WALK));
            #endif
            check_passed = false;
        }
        if( movementInfo.z < 0.0001f && movementInfo.z > -0.0001f
            && ( movementInfo.HasMovementFlag(MOVEMENTFLAG_SWIMMING) || movementInfo.HasMovementFlag(MOVEMENTFLAG_CAN_FLY)
                 ||movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING) || movementInfo.HasMovementFlag(MOVEMENTFLAG_FLYING2) )
            && !GetPlayer()->isGameMaster() )
        {
            // Prevent using TeleportToPlan.
            Map *map = GetPlayer()->GetMap();
            if (map){
                float plane_z = map->GetHeight(movementInfo.x, movementInfo.y, MAX_HEIGHT) - movementInfo.z;
                plane_z = (plane_z < -500.0f) ? 0 : plane_z; //check holes in heigth map
                if(plane_z > 0.1f || plane_z < -0.1f)
                {
                    GetPlayer()->m_anti_teletoplane_count++;
                    check_passed = false;
                    #ifdef MOVEMENT_ANTICHEAT_DEBUG
                    sLog.outError("Movement anticheat: %s is teleport to plan exception. plane_z: %f ", GetPlayer()->GetName(), plane_z);
                    #endif
                    if (GetPlayer()->m_anti_teletoplane_count > World::GetTeleportToPlaneAlarms())
                    {
                        GetPlayer()->GetSession()->KickPlayer();
                        sLog.outError("Movement anticheat: %s is teleport to plan exception. Exception count: %d ", GetPlayer()->GetName(), GetPlayer()->m_anti_teletoplane_count);
                    }
                }
            }
        } else {
            if (GetPlayer()->m_anti_teletoplane_count !=0)
                GetPlayer()->m_anti_teletoplane_count = 0;
        }
    } else if ((movementInfo.flags & MOVEMENTFLAG_ONTRANSPORT)   && (GetPlayer()->GetMapId() !=554)) {//liftmechanar
            //antiwrap =)
        if (GetPlayer()->m_transport)
        {
            float trans_rad = movementInfo.t_x*movementInfo.t_x + movementInfo.t_y*movementInfo.t_y + movementInfo.t_z*movementInfo.t_z;
            if (trans_rad > 3600.0f && opcode!=MSG_MOVE_HEARTBEAT)
                check_passed = false;
        } else {
            if (GameObjectData const* go_data = sObjectMgr.GetGOData(GetPlayer()->m_anti_transportGUID))
            {
                float delta_gox = go_data->posX - movementInfo.x;
                float delta_goy = go_data->posY - movementInfo.y;
                float delta_goz = go_data->posZ - movementInfo.z;
                int mapid = go_data->mapid;
                #ifdef MOVEMENT_ANTICHEAT_DEBUG
                sLog.outError("Movement anticheat: %s on some transport. xyzo: %f,%f,%f", GetPlayer()->GetName(), go_data->posX,go_data->posY,go_data->posZ);
                #endif
                if (GetPlayer()->GetMapId() != mapid){
                    check_passed = false;
                } else if (mapid !=369) {
                    float delta_go = delta_gox*delta_gox + delta_goy*delta_goy;
                    if (delta_go > 3600.0f)
                        check_passed = false;
                }

            } else {
                #ifdef MOVEMENT_ANTICHEAT_DEBUG
                sLog.outError("Movement anticheat: %s on undefined transport.", GetPlayer()->GetName());
                #endif
                check_passed = false;
            }
        }
        if (!check_passed){
            if (GetPlayer()->m_transport)
                {
                    GetPlayer()->m_transport->RemovePassenger(GetPlayer());
                    GetPlayer()->m_transport = NULL;
                }
                movementInfo.t_x = 0.0f;
                movementInfo.t_y = 0.0f;
                movementInfo.t_z = 0.0f;
                movementInfo.t_o = 0.0f;
                movementInfo.t_time = 0;
                GetPlayer()->m_anti_transportGUID = 0;
        }
    }
    /* process position-change */
    if (check_passed)
    {
        recv_data.put<uint32>(5, getMSTime());                  // offset flags(4) + unk(1)
        WorldPacket data(recv_data.GetOpcode(), (GetPlayer()->GetPackGUID().size()+recv_data.size()));
        data.append(GetPlayer()->GetPackGUID());
        data.append(recv_data.contents(), recv_data.size());
        GetPlayer()->SendMessageToSet(&data, false);

    GetPlayer()->m_movementInfo = movementInfo;

   GetPlayer()->SetPosition(movementInfo.x, movementInfo.y, movementInfo.z, movementInfo.o);

    GetPlayer()->UpdateFallInformationIfNeed(movementInfo,recv_data.GetOpcode());

        if(GetPlayer()->isMovingOrTurning())
            GetPlayer()->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    if(movementInfo.z < -500.0f)
    {
        if(GetPlayer()->InBattleGround()
            && GetPlayer()->GetBattleGround()
            && GetPlayer()->GetBattleGround()->HandlePlayerUnderMap(_player))
        {
            // do nothing, the handle already did if returned true
        }
        else
        {
            // NOTE: this is actually called many times while falling
            // even after the player has been teleported away
            // TODO: discard movement packets after the player is rooted
            if(GetPlayer()->isAlive())
            {
                GetPlayer()->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, GetPlayer()->GetMaxHealth());
                // pl can be alive if GM/etc
                if(!GetPlayer()->isAlive())
                {
                    // change the death state to CORPSE to prevent the death timer from
                    // starting in the next player update
                    GetPlayer()->KillPlayer();
                    GetPlayer()->BuildPlayerRepop();
                }
            }

            // cancel the death timer here if started
            GetPlayer()->RepopAtGraveyard();
        }
    }

        if (GetPlayer()->m_anti_alarmcount > 0){
            sLog.outError("Movement anticheat: %s produce %d anticheat alarms",GetPlayer()->GetName(),GetPlayer()->m_anti_alarmcount);
            GetPlayer()->m_anti_alarmcount = 0;
        }
    } else {
        GetPlayer()->m_anti_alarmcount++;
        WorldPacket data;
//        GetPlayer()->SetUnitMovementFlags(0);
        GetPlayer()->BuildTeleportAckMsg(&data, GetPlayer()->GetPositionX(), GetPlayer()->GetPositionY(), GetPlayer()->GetPositionZ(), GetPlayer()->GetOrientation());
        GetPlayer()->GetSession()->SendPacket(&data);
        GetPlayer()->BuildHeartBeatMsg(&data);
        GetPlayer()->SendMessageToSet(&data, true);
    }
}

void WorldSession::HandleForceSpeedChangeAck(WorldPacket &recv_data)
{
    /* extract packet */
    uint64 guid;
    uint32 unk1;
    float  newspeed;

    recv_data >> guid;

    // now can skip not our packet
    if(_player->GetGUID() != guid)
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    // continue parse packet

    recv_data >> unk1;

    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);

    recv_data >> newspeed;
    /*----------------*/

    // client ACK send one packet for mounted/run case and need skip all except last from its
    // in other cases anti-cheat check can be fail in false case
    UnitMoveType move_type;
    UnitMoveType force_move_type;

    static char const* move_type_name[MAX_MOVE_TYPE] = {  "Walk", "Run", "RunBack", "Swim", "SwimBack", "TurnRate", "Flight", "FlightBack" };

    uint16 opcode = recv_data.GetOpcode();
    switch(opcode)
    {
        case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:          move_type = MOVE_WALK;          force_move_type = MOVE_WALK;        break;
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:           move_type = MOVE_RUN;           force_move_type = MOVE_RUN;         break;
        case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:      move_type = MOVE_RUN_BACK;      force_move_type = MOVE_RUN_BACK;    break;
        case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:          move_type = MOVE_SWIM;          force_move_type = MOVE_SWIM;        break;
        case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:     move_type = MOVE_SWIM_BACK;     force_move_type = MOVE_SWIM_BACK;   break;
        case CMSG_FORCE_TURN_RATE_CHANGE_ACK:           move_type = MOVE_TURN_RATE;     force_move_type = MOVE_TURN_RATE;   break;
        case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:        move_type = MOVE_FLIGHT;        force_move_type = MOVE_FLIGHT;      break;
        case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:   move_type = MOVE_FLIGHT_BACK;   force_move_type = MOVE_FLIGHT_BACK; break;
        default:
            sLog.outError("WorldSession::HandleForceSpeedChangeAck: Unknown move type opcode: %u", opcode);
            return;
    }

    // skip all forced speed changes except last and unexpected
    // in run/mounted case used one ACK and it must be skipped.m_forced_speed_changes[MOVE_RUN} store both.
    if(_player->m_forced_speed_changes[force_move_type] > 0)
    {
        --_player->m_forced_speed_changes[force_move_type];
        if(_player->m_forced_speed_changes[force_move_type] > 0)
            return;
    }

    if (!_player->GetTransport() && fabs(_player->GetSpeed(move_type) - newspeed) > 0.01f)
    {
        if(_player->GetSpeed(move_type) > newspeed)         // must be greater - just correct
        {
            sLog.outError("%sSpeedChange player %s is NOT correct (must be %f instead %f), force set to correct value",
                move_type_name[move_type], _player->GetName(), _player->GetSpeed(move_type), newspeed);
            _player->SetSpeed(move_type,_player->GetSpeedRate(move_type),true);
        }
        else                                                // must be lesser - cheating
        {
            sLog.outBasic("Player %s from account id %u kicked for incorrect speed (must be %f instead %f)",
                _player->GetName(),_player->GetSession()->GetAccountId(),_player->GetSpeed(move_type), newspeed);
            _player->GetSession()->KickPlayer();
        }
    }
}

void WorldSession::HandleSetActiveMoverOpcode(WorldPacket &recv_data)
{
    sLog.outDebug("WORLD: Recvd CMSG_SET_ACTIVE_MOVER");

    uint64 guid;
    recv_data >> guid;

    WorldPacket data(SMSG_TIME_SYNC_REQ, 4);                // new 2.0.x, enable movement
    data << uint32(0x00000000);                             // on blizz it increments periodically
    SendPacket(&data);
}

void WorldSession::HandleMountSpecialAnimOpcode(WorldPacket& /*recvdata*/)
{
    //sLog.outDebug("WORLD: Recvd CMSG_MOUNTSPECIAL_ANIM");

    WorldPacket data(SMSG_MOUNTSPECIAL_ANIM, 8);
    data << uint64(GetPlayer()->GetGUID());

    GetPlayer()->SendMessageToSet(&data, false);
}

void WorldSession::HandleMoveKnockBackAck( WorldPacket & recv_data )
{
    sLog.outDebug("CMSG_MOVE_KNOCK_BACK_ACK");

    recv_data.read_skip<uint64>();                          // guid
    recv_data.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    uint32 unk1,unk2,unk3;
    recv_data >> unk1 >> unk2 >> unk3;
    ReadMovementInfo(recv_data, &movementInfo);
    //Save movement flags
    _player->m_movementInfo.SetMovementFlags(MovementFlags(movementInfo.flags));
    #ifdef MOVEMENT_ANTICHEAT_DEBUG
    sLog.outBasic("%s CMSG_MOVE_KNOCK_BACK_ACK: tm:%d ftm:%d | %f,%f,%fo(%f) [%X]",GetPlayer()->GetName(),movementInfo.time,movementInfo.fallTime,movementInfo.x,movementInfo.y,movementInfo.z,movementInfo.o,movementInfo.flags);
    sLog.outBasic("%s CMSG_MOVE_KNOCK_BACK_ACK additional: vspeed:%f, hspeed:%f",GetPlayer()->GetName(), movementInfo.j_unk, movementInfo.j_xyspeed);
    #endif

    _player->m_movementInfo = movementInfo;
    _player->m_anti_last_hspeed = movementInfo.j_xyspeed;
    _player->m_anti_last_vspeed = movementInfo.j_unk < 3.2f ? movementInfo.j_unk - 1.0f : 3.2f;
    uint32 dt = (_player->m_anti_last_vspeed < 0) ? (int)(ceil(_player->m_anti_last_vspeed/-25)*1000) : (int)(ceil(_player->m_anti_last_vspeed/25)*1000);
    _player->m_anti_lastspeed_changetime = movementInfo.time + dt + 1000;
}

void WorldSession::HandleMoveHoverAck( WorldPacket& recv_data )
{
    sLog.outDebug("CMSG_MOVE_HOVER_ACK");

    recv_data.read_skip<uint64>();                          // guid
    recv_data.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);

    recv_data.read_skip<uint32>();                          // unk2
}

void WorldSession::HandleMoveWaterWalkAck(WorldPacket& recv_data)
{
    sLog.outDebug("CMSG_MOVE_WATER_WALK_ACK");

    recv_data.read_skip<uint64>();                          // guid
    recv_data.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);

    recv_data.read_skip<uint32>();                          // unk2
}

void WorldSession::HandleSummonResponseOpcode(WorldPacket& recv_data)
{
    if(!_player->isAlive() || _player->isInCombat() )
        return;

    uint64 summoner_guid;
    bool agree;
    recv_data >> summoner_guid;
    recv_data >> agree;

    _player->SummonIfPossible(agree);
}
