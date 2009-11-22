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
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Opcodes.h"
#include "Log.h"
#include "UpdateMask.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "Spell.h"
#include "DynamicObject.h"
#include "Group.h"
#include "UpdateData.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CellImpl.h"
#include "Policies/SingletonImp.h"
#include "SharedDefines.h"
#include "LootMgr.h"
#include "VMapFactory.h"
#include "BattleGround.h"
#include "Util.h"

#define SPELL_CHANNEL_UPDATE_INTERVAL (1 * IN_MILISECONDS)

extern pEffect SpellEffects[TOTAL_SPELL_EFFECTS];

bool IsQuestTameSpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto) return false;

    return spellproto->Effect[0] == SPELL_EFFECT_THREAT
        && spellproto->Effect[1] == SPELL_EFFECT_APPLY_AURA && spellproto->EffectApplyAuraName[1] == SPELL_AURA_DUMMY;
}

SpellCastTargets::SpellCastTargets()
{
    m_unitTarget = NULL;
    m_itemTarget = NULL;
    m_GOTarget   = NULL;

    m_unitTargetGUID   = 0;
    m_GOTargetGUID     = 0;
    m_CorpseTargetGUID = 0;
    m_itemTargetGUID   = 0;
    m_itemTargetEntry  = 0;

    m_srcX = m_srcY = m_srcZ = m_destX = m_destY = m_destZ = 0;
    m_strTarget = "";
    m_targetMask = 0;
}

SpellCastTargets::~SpellCastTargets()
{
}

void SpellCastTargets::setUnitTarget(Unit *target)
{
    if (!target)
        return;

    m_destX = target->GetPositionX();
    m_destY = target->GetPositionY();
    m_destZ = target->GetPositionZ();
    m_unitTarget = target;
    m_unitTargetGUID = target->GetGUID();
    m_targetMask |= TARGET_FLAG_UNIT;
}

void SpellCastTargets::setDestination(float x, float y, float z)
{
    m_destX = x;
    m_destY = y;
    m_destZ = z;
    m_targetMask |= TARGET_FLAG_DEST_LOCATION;
}

void SpellCastTargets::setSource(float x, float y, float z)
{
    m_srcX = x;
    m_srcY = y;
    m_srcZ = z;
    m_targetMask |= TARGET_FLAG_SOURCE_LOCATION;
}

void SpellCastTargets::setGOTarget(GameObject *target)
{
    m_GOTarget = target;
    m_GOTargetGUID = target->GetGUID();
    //    m_targetMask |= TARGET_FLAG_OBJECT;
}

void SpellCastTargets::setItemTarget(Item* item)
{
    if(!item)
        return;

    m_itemTarget = item;
    m_itemTargetGUID = item->GetGUID();
    m_itemTargetEntry = item->GetEntry();
    m_targetMask |= TARGET_FLAG_ITEM;
}

void SpellCastTargets::setCorpseTarget(Corpse* corpse)
{
    m_CorpseTargetGUID = corpse->GetGUID();
}

void SpellCastTargets::Update(Unit* caster)
{
    m_GOTarget   = m_GOTargetGUID ? caster->GetMap()->GetGameObject(m_GOTargetGUID) : NULL;
    m_unitTarget = m_unitTargetGUID ?
        ( m_unitTargetGUID == caster->GetGUID() ? caster : ObjectAccessor::GetUnit(*caster, m_unitTargetGUID) ) :
    NULL;

    m_itemTarget = NULL;
    if(caster->GetTypeId() == TYPEID_PLAYER)
    {
        if(m_targetMask & TARGET_FLAG_ITEM)
            m_itemTarget = ((Player*)caster)->GetItemByGuid(m_itemTargetGUID);
        else
        {
            Player* pTrader = ((Player*)caster)->GetTrader();
            if(pTrader && m_itemTargetGUID < TRADE_SLOT_COUNT)
                m_itemTarget = pTrader->GetItemByPos(pTrader->GetItemPosByTradeSlot(m_itemTargetGUID));
        }
        if(m_itemTarget)
            m_itemTargetEntry = m_itemTarget->GetEntry();
    }
}

bool SpellCastTargets::read ( WorldPacket * data, Unit *caster )
{
    if(data->rpos() + 4 > data->size())
        return false;

    *data >> m_targetMask;

    if(m_targetMask == TARGET_FLAG_SELF)
    {
        m_destX = caster->GetPositionX();
        m_destY = caster->GetPositionY();
        m_destZ = caster->GetPositionZ();
        m_unitTarget = caster;
        m_unitTargetGUID = caster->GetGUID();
        return true;
    }

    // TARGET_FLAG_UNK2 is used for non-combat pets, maybe other?
    if( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_UNK2 ))
        if(!data->readPackGUID(m_unitTargetGUID))
            return false;

    if( m_targetMask & ( TARGET_FLAG_OBJECT | TARGET_FLAG_OBJECT_UNK ))
        if(!data->readPackGUID(m_GOTargetGUID))
            return false;

    if(( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM )) && caster->GetTypeId() == TYPEID_PLAYER)
        if(!data->readPackGUID(m_itemTargetGUID))
            return false;

    if( m_targetMask & TARGET_FLAG_SOURCE_LOCATION )
    {
        if(data->rpos() + 4 + 4 + 4 > data->size())
            return false;

        *data >> m_srcX >> m_srcY >> m_srcZ;
        if(!MaNGOS::IsValidMapCoord(m_srcX, m_srcY, m_srcZ))
            return false;
    }

    if( m_targetMask & TARGET_FLAG_DEST_LOCATION )
    {
        if(data->rpos() + 4 + 4 + 4 > data->size())
            return false;

        *data >> m_destX >> m_destY >> m_destZ;
        if(!MaNGOS::IsValidMapCoord(m_destX, m_destY, m_destZ))
            return false;
    }

    if( m_targetMask & TARGET_FLAG_STRING )
    {
        if(data->rpos() + 1 > data->size())
            return false;

        *data >> m_strTarget;
    }

    if( m_targetMask & (TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
        if(!data->readPackGUID(m_CorpseTargetGUID))
            return false;

    // find real units/GOs
    Update(caster);
    return true;
}

void SpellCastTargets::write ( WorldPacket * data )
{
    *data << uint32(m_targetMask);

    if( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_PVP_CORPSE | TARGET_FLAG_OBJECT | TARGET_FLAG_CORPSE | TARGET_FLAG_UNK2 ) )
    {
        if(m_targetMask & TARGET_FLAG_UNIT)
        {
            if(m_unitTarget)
                data->append(m_unitTarget->GetPackGUID());
            else
                *data << uint8(0);
        }
        else if( m_targetMask & ( TARGET_FLAG_OBJECT | TARGET_FLAG_OBJECT_UNK ) )
        {
            if(m_GOTarget)
                data->append(m_GOTarget->GetPackGUID());
            else
                *data << uint8(0);
        }
        else if( m_targetMask & ( TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
            data->appendPackGUID(m_CorpseTargetGUID);
        else
            *data << uint8(0);
    }

    if( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM ) )
    {
        if(m_itemTarget)
            data->append(m_itemTarget->GetPackGUID());
        else
            *data << uint8(0);
    }

    if( m_targetMask & TARGET_FLAG_SOURCE_LOCATION )
        *data << m_srcX << m_srcY << m_srcZ;

    if( m_targetMask & TARGET_FLAG_DEST_LOCATION )
        *data << m_destX << m_destY << m_destZ;

    if( m_targetMask & TARGET_FLAG_STRING )
        *data << m_strTarget;
}

Spell::Spell( Unit* Caster, SpellEntry const *info, bool triggered, uint64 originalCasterGUID, Spell** triggeringContainer )
{
    ASSERT( Caster != NULL && info != NULL );
    ASSERT( info == sSpellStore.LookupEntry( info->Id ) && "`info` must be pointer to sSpellStore element");

    m_spellInfo = info;
    m_caster = Caster;
    m_selfContainer = NULL;
    m_triggeringContainer = triggeringContainer;
    m_referencedFromCurrentSpell = false;
    m_executedCurrently = false;
    m_delayStart = 0;
    m_delayAtDamageCount = 0;

    m_applyMultiplierMask = 0;

    // Get data for type of attack
    switch (m_spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            if (m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_REQ_OFFHAND)
                m_attackType = OFF_ATTACK;
            else
                m_attackType = BASE_ATTACK;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            m_attackType = RANGED_ATTACK;
            break;
        default:
                                                            // Wands
            if (m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_REQ_WAND)
                m_attackType = RANGED_ATTACK;
            else
                m_attackType = BASE_ATTACK;
            break;
    }

    m_spellSchoolMask = GetSpellSchoolMask(info);           // Can be override for some spell (wand shoot for example)

    if(m_attackType == RANGED_ATTACK)
    {
        // wand case
        if((m_caster->getClassMask() & CLASSMASK_WAND_USERS) != 0 && m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            if(Item* pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK))
                m_spellSchoolMask = SpellSchoolMask(1 << pItem->GetProto()->Damage[0].DamageType);
        }
    }
    // Set health leech amount to zero
    m_healthLeech = 0;

    if(originalCasterGUID)
        m_originalCasterGUID = originalCasterGUID;
    else
        m_originalCasterGUID = m_caster->GetGUID();

    if(m_originalCasterGUID == m_caster->GetGUID())
        m_originalCaster = m_caster;
    else
    {
        m_originalCaster = ObjectAccessor::GetUnit(*m_caster, m_originalCasterGUID);
        if(m_originalCaster && !m_originalCaster->IsInWorld()) m_originalCaster = NULL;
    }

    for(int i=0; i <3; ++i)
        m_currentBasePoints[i] = m_spellInfo->EffectBasePoints[i];

    m_spellState = SPELL_STATE_NULL;

    m_castPositionX = m_castPositionY = m_castPositionZ = 0;
    m_TriggerSpells.clear();
    m_preCastSpells.clear();
    m_IsTriggeredSpell = triggered;
    //m_AreaAura = false;
    m_CastItem = NULL;

    unitTarget = NULL;
    itemTarget = NULL;
    gameObjTarget = NULL;
    focusObject = NULL;
    m_cast_count = 0;
    m_triggeredByAuraSpell  = NULL;

    //Auto Shot & Shoot
    if( m_spellInfo->AttributesEx2 == 0x000020 && !triggered )
        m_autoRepeat = true;
    else
        m_autoRepeat = false;

    m_powerCost = 0;                                        // setup to correct value in Spell::prepare, don't must be used before.
    m_casttime = 0;                                         // setup to correct value in Spell::prepare, don't must be used before.
    m_timer = 0;                                            // will set to castime in prepare

    m_needAliveTargetMask = 0;

    // determine reflection
    m_canReflect = false;

    if(m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_CANT_REFLECTED)==0)
    {
        for(int j = 0;j < 3; ++j)
        {
            if (m_spellInfo->Effect[j] == 0)
                continue;

            if(!IsPositiveTarget(m_spellInfo->EffectImplicitTargetA[j], m_spellInfo->EffectImplicitTargetB[j]))
                m_canReflect = true;
            else
                m_canReflect = (m_spellInfo->AttributesEx & (1<<7)) ? true : false;

            if(m_canReflect)
                continue;
            else
                break;
        }
    }

    CleanupTargetList();
}

Spell::~Spell()
{
}

template<typename T>
WorldObject* Spell::FindCorpseUsing()
{
    // non-standard target selection
    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
    float max_range = GetSpellMaxRange(srange);

    CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    WorldObject* result = NULL;

    T u_check(m_caster, max_range);
    MaNGOS::WorldObjectSearcher<T> searcher(result, u_check);

    TypeContainerVisitor<MaNGOS::WorldObjectSearcher<T>, GridTypeMapContainer > grid_searcher(searcher);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, grid_searcher, *m_caster->GetMap(), *m_caster, max_range);

    if (!result)
    {
        TypeContainerVisitor<MaNGOS::WorldObjectSearcher<T>, WorldTypeMapContainer > world_searcher(searcher);
        cell_lock->Visit(cell_lock, world_searcher, *m_caster->GetMap(), *m_caster, max_range);
    }

    return result;
}

void Spell::FillTargetMap()
{
    // TODO: ADD the correct target FILLS!!!!!!

    for(uint32 i = 0; i < 3; ++i)
    {
        // not call for empty effect.
        // Also some spells use not used effect targets for store targets for dummy effect in triggered spells
        if(m_spellInfo->Effect[i] == 0)
            continue;

        // targets for TARGET_SCRIPT_COORDINATES (A) and TARGET_SCRIPT  filled in Spell::CheckCast call
        if( m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT_COORDINATES ||
            m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT ||
            m_spellInfo->EffectImplicitTargetB[i] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[i] != TARGET_SELF )
            continue;

        // TODO: find a way so this is not needed?
        // for area auras always add caster as target (needed for totems for example)
        if(IsAreaAuraEffect(m_spellInfo->Effect[i]))
            AddUnitTarget(m_caster, i);

        std::list<Unit*> tmpUnitMap;

        // TargetA/TargetB dependent from each other, we not switch to full support this dependences
        // but need it support in some know cases
        switch(m_spellInfo->EffectImplicitTargetA[i])
        {
            case 0:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                        SetTargetMap(i, TARGET_EFFECT_SELECT, tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        break;
                }
                break;
            case TARGET_EFFECT_SELECT:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                    case TARGET_EFFECT_SELECT:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        break;
                    case TARGET_INNKEEPER_COORDINATES:
                    case TARGET_TABLE_X_Y_Z_COORDINATES:
                    case TARGET_CASTER_COORDINATES:
                    case TARGET_SCRIPT_COORDINATES:
                    case TARGET_CURRENT_ENEMY_COORDINATES:
                    case TARGET_DUELVSPLAYER_COORDINATES:
                    case TARGET_DYNAMIC_OBJECT_COORDINATES:
                    case TARGET_POINT_AT_NORTH:
                    case TARGET_POINT_AT_SOUTH:
                    case TARGET_POINT_AT_EAST:
                    case TARGET_POINT_AT_WEST:
                    case TARGET_POINT_AT_NE:
                    case TARGET_POINT_AT_NW:
                    case TARGET_POINT_AT_SE:
                    case TARGET_POINT_AT_SW:
                        // need some target for proccesing
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        break;
                }
                break;
            case TARGET_SELF:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                    case TARGET_EFFECT_SELECT:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        break;
                    case TARGET_AREAEFFECT_INSTANT:         // use B case that not dependent from from A in fact
                        if((m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)==0)
                            m_targets.setDestination(m_caster->GetPositionX(),m_caster->GetPositionY(),m_caster->GetPositionZ());
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                    case TARGET_BEHIND_VICTIM:              // use B case that not dependent from from A in fact
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                }
                break;
            case TARGET_CASTER_COORDINATES:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case TARGET_ALL_ENEMY_IN_AREA:
                        // Note: this hack with search required until GO casting not implemented
                        // environment damage spells already have around enemies targeting but this not help in case not existed GO casting support
                        // currently each enemy selected explicitly and self cast damage
                        if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE)
                        {
                            if(m_targets.getUnitTarget())
                                tmpUnitMap.push_back(m_targets.getUnitTarget());
                        }
                        else
                        {
                            SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                            SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        }
                        break;
                    default:
                        {
                            SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                            SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        }
                        break;
                }
                break;
            case TARGET_TABLE_X_Y_Z_COORDINATES:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);

                        // need some target for proccesing
                        SetTargetMap(i, TARGET_EFFECT_SELECT, tmpUnitMap);
                        break;
                    case TARGET_AREAEFFECT_INSTANT:         // All 17/7 pairs used for dest teleportation, A processed in effect code
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        break;
                }
                break;
            default:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                    case TARGET_EFFECT_SELECT:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        break;
                    case TARGET_SCRIPT_COORDINATES:         // B case filled in CheckCast but we need fill unit list base at A case
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                        SetTargetMap(i, m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                        break;
                }
                break;
        }

        if(m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            Player *me = (Player*)m_caster;
            for (std::list<Unit*>::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                Unit *owner = (*itr)->GetOwner();
                Unit *u = owner ? owner : (*itr);
                if(u!=m_caster && u->IsPvP() && (!me->duel || me->duel->opponent != u))
                {
                    me->UpdatePvP(true);
                    me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
                    break;
                }
            }
        }

        for (std::list<Unit*>::iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end();)
        {
            if (!CheckTarget (*itr, i))
            {
                itr = tmpUnitMap.erase(itr);
                continue;
            }
            else
                ++itr;
        }

        for(std::list<Unit*>::const_iterator iunit = tmpUnitMap.begin(); iunit != tmpUnitMap.end(); ++iunit)
            AddUnitTarget((*iunit), i);
    }
}

void Spell::prepareDataForTriggerSystem()
{
    //==========================================================================================
    // Now fill data for trigger system, need know:
    // an spell trigger another or not ( m_canTrigger )
    // Create base triggers flags for Attacker and Victim ( m_procAttacker and  m_procVictim)
    //==========================================================================================
    // Fill flag can spell trigger or not
    // TODO: possible exist spell attribute for this
    m_canTrigger = false;

    if (m_CastItem)
        m_canTrigger = false;         // Do not trigger from item cast spell
    else if (!m_IsTriggeredSpell)
        m_canTrigger = true;          // Normal cast - can trigger
    else if (!m_triggeredByAuraSpell)
        m_canTrigger = true;          // Triggered from SPELL_EFFECT_TRIGGER_SPELL - can trigger

    if (!m_canTrigger)                // Exceptions (some periodic triggers)
    {
        switch (m_spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_MAGE:
                // Arcane Missles / Blizzard triggers need do it
                if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000000200080))
                    m_canTrigger = true;
                // Clearcasting trigger need do it
                else if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000200000000))
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_WARLOCK: // For Hellfire Effect / Rain of Fire / Seed of Corruption triggers need do it
                if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000800000000060))
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_PRIEST:  // For Penance heal/damage triggers need do it
                if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0001800000000000))
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_ROGUE:   // For poisons need do it
                if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x000000101001E000))
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_HUNTER:  // Hunter Rapid Killing/Explosive Trap Effect/Immolation Trap Effect/Frost Trap Aura/Snake Trap Effect/Explosive Shot
                if ((m_spellInfo->SpellFamilyFlags & UI64LIT(0x0100200000000284)))
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_PALADIN: // Holy Shock triggers need do it
                if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0001000000200000))
                    m_canTrigger = true;
                break;
            default:
                break;
        }
    }
    if (m_CastItem) m_canTrigger = false;
    // Get data for type of attack and fill base info for trigger
    switch (m_spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            m_procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_SPELL_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_MELEE_SPELL_HIT;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            // Auto attack
            if (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG)
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            }
            else // Ranged spell attack
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_SPELL_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_SPELL_HIT;
            }
            break;
        default:
            if (IsPositiveSpell(m_spellInfo->Id))                                 // Check for positive spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL;
                m_procVictim   = PROC_FLAG_TAKEN_POSITIVE_SPELL;
            }
            else if (m_spellInfo->Id == 5019) // Wands auto attack
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            }
            else                                           // Negative spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;
            }
            break;
    }
    // Hunter traps spells (for Entrapment trigger)
    // Gives your Immolation Trap, Frost Trap, Explosive Trap, and Snake Trap ....
    if (m_spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER && (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000200000000084)))
        m_procAttacker |= PROC_FLAG_ON_TRAP_ACTIVATION;
}

void Spell::CleanupTargetList()
{
    m_UniqueTargetInfo.clear();
    m_UniqueGOTargetInfo.clear();
    m_UniqueItemInfo.clear();
    m_countOfHit = 0;
    m_countOfMiss = 0;
    m_delayMoment = 0;
}

void Spell::AddUnitTarget(Unit* pVictim, uint32 effIndex)
{
    if( m_spellInfo->Effect[effIndex] == 0 )
        return;

    uint64 targetGUID = pVictim->GetGUID();

    // Lookup target in already in list
    for(std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= 1<<effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him

    // Get spell hit result on target
    TargetInfo target;
    target.targetGUID = targetGUID;                         // Store target GUID
    target.effectMask = 1<<effIndex;                        // Store index of effect
    target.processed  = false;                              // Effects not apply on target

    // Calculate hit result
    target.missCondition = m_caster->SpellHitResult(pVictim, m_spellInfo, m_canReflect);
    if (target.missCondition == SPELL_MISS_NONE)
        ++m_countOfHit;
    else
        ++m_countOfMiss;

    // Spell have speed - need calculate incoming time
    if (m_spellInfo->speed > 0.0f)
    {
        // calculate spell incoming interval
        float dist = m_caster->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f) dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / m_spellInfo->speed * 1000.0f);

        // Calculate minimum incoming time
        if (m_delayMoment == 0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = UI64LIT(0);

    // If target reflect spell back to caster
    if (target.missCondition == SPELL_MISS_REFLECT)
    {
        // Calculate reflected spell result on caster
        target.reflectResult =  m_caster->SpellHitResult(m_caster, m_spellInfo, m_canReflect);

        if (target.reflectResult == SPELL_MISS_REFLECT)     // Impossible reflect again, so simply deflect spell
            target.reflectResult = SPELL_MISS_PARRY;

        // Increase time interval for reflected spells by 1.5
        target.timeDelay += target.timeDelay >> 1;
    }
    else
        target.reflectResult = SPELL_MISS_NONE;

    // Add target to list
    m_UniqueTargetInfo.push_back(target);
}

void Spell::AddUnitTarget(uint64 unitGUID, uint32 effIndex)
{
    Unit* unit = m_caster->GetGUID() == unitGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, unitGUID);
    if (unit)
        AddUnitTarget(unit, effIndex);
}

void Spell::AddGOTarget(GameObject* pVictim, uint32 effIndex)
{
    if( m_spellInfo->Effect[effIndex] == 0 )
        return;

    uint64 targetGUID = pVictim->GetGUID();

    // Lookup target in already in list
    for(std::list<GOTargetInfo>::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= 1 << effIndex;              // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him

    GOTargetInfo target;
    target.targetGUID = targetGUID;
    target.effectMask = 1 << effIndex;
    target.processed  = false;                              // Effects not apply on target

    // Spell have speed - need calculate incoming time
    if (m_spellInfo->speed > 0.0f)
    {
        // calculate spell incoming interval
        float dist = m_caster->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f) dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / m_spellInfo->speed * 1000.0f);
        if (m_delayMoment==0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = UI64LIT(0);

    ++m_countOfHit;

    // Add target to list
    m_UniqueGOTargetInfo.push_back(target);
}

void Spell::AddGOTarget(uint64 goGUID, uint32 effIndex)
{
    GameObject* go = m_caster->GetMap()->GetGameObject(goGUID);
    if (go)
        AddGOTarget(go, effIndex);
}

void Spell::AddItemTarget(Item* pitem, uint32 effIndex)
{
    if( m_spellInfo->Effect[effIndex] == 0 )
        return;

    // Lookup target in already in list
    for(std::list<ItemTargetInfo>::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
    {
        if (pitem == ihit->item)                            // Found in list
        {
            ihit->effectMask |= 1<<effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target add data

    ItemTargetInfo target;
    target.item       = pitem;
    target.effectMask = 1 << effIndex;
    m_UniqueItemInfo.push_back(target);
}

/*
void Spell::doTriggers(SpellMissInfo missInfo, uint32 damage, SpellSchoolMask damageSchoolMask, uint32 block, uint32 absorb, bool crit)
{
    // Do triggers depends from hit result (triggers on hit do in effects)
    // Set aura states depends from hit result
    if (missInfo!=SPELL_MISS_NONE)
    {
        // Miss/dodge/parry/block only for melee based spells
        // Resist only for magic based spells
        switch (missInfo)
        {
            case SPELL_MISS_MISS:
                if(m_caster->GetTypeId()== TYPEID_PLAYER)
                    ((Player*)m_caster)->UpdateWeaponSkill(BASE_ATTACK);

                m_caster->CastMeleeProcDamageAndSpell(unitTarget, 0, damageSchoolMask, m_attackType, MELEE_HIT_MISS, m_spellInfo, m_IsTriggeredSpell);
                break;
            case SPELL_MISS_RESIST:
                m_caster->ProcDamageAndSpell(unitTarget, PROC_FLAG_TARGET_RESISTS, PROC_FLAG_RESIST_SPELL, 0, damageSchoolMask, m_spellInfo, m_IsTriggeredSpell);
                break;
            case SPELL_MISS_DODGE:
                if(unitTarget->GetTypeId() == TYPEID_PLAYER)
                    ((Player*)unitTarget)->UpdateDefense();

                // Overpower
                if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->getClass() == CLASS_WARRIOR)
                {
                    ((Player*) m_caster)->AddComboPoints(unitTarget, 1);
                    m_caster->StartReactiveTimer( REACTIVE_OVERPOWER );
                }

                // Riposte
                if (unitTarget->getClass() != CLASS_ROGUE)
                {
                    unitTarget->ModifyAuraState(AURA_STATE_DEFENSE, true);
                    unitTarget->StartReactiveTimer( REACTIVE_DEFENSE );
                }

                m_caster->CastMeleeProcDamageAndSpell(unitTarget, 0, damageSchoolMask, m_attackType, MELEE_HIT_DODGE, m_spellInfo, m_IsTriggeredSpell);
                break;
            case SPELL_MISS_PARRY:
                // Update victim defense ?
                if(unitTarget->GetTypeId() == TYPEID_PLAYER)
                    ((Player*)unitTarget)->UpdateDefense();
                // Mongoose bite - set only Counterattack here
                if (unitTarget->getClass() == CLASS_HUNTER)
                {
                    unitTarget->ModifyAuraState(AURA_STATE_HUNTER_PARRY,true);
                    unitTarget->StartReactiveTimer( REACTIVE_HUNTER_PARRY );
                }
                else
                {
                    unitTarget->ModifyAuraState(AURA_STATE_DEFENSE, true);
                    unitTarget->StartReactiveTimer( REACTIVE_DEFENSE );
                }
                m_caster->CastMeleeProcDamageAndSpell(unitTarget, 0, damageSchoolMask, m_attackType, MELEE_HIT_PARRY, m_spellInfo, m_IsTriggeredSpell);
                break;
            case SPELL_MISS_BLOCK:
                unitTarget->ModifyAuraState(AURA_STATE_DEFENSE, true);
                unitTarget->StartReactiveTimer( REACTIVE_DEFENSE );

                m_caster->CastMeleeProcDamageAndSpell(unitTarget, 0, damageSchoolMask, m_attackType, MELEE_HIT_BLOCK, m_spellInfo, m_IsTriggeredSpell);
                break;
                // Trigger from this events not supported
            case SPELL_MISS_EVADE:
            case SPELL_MISS_IMMUNE:
            case SPELL_MISS_IMMUNE2:
            case SPELL_MISS_DEFLECT:
            case SPELL_MISS_ABSORB:
                // Trigger from reflects need do after get reflect result
            case SPELL_MISS_REFLECT:
                break;
            default:
                break;
        }
    }
}
*/

void Spell::DoAllEffectOnTarget(TargetInfo *target)
{
    if (!target)                                  // Check target
        return;
    if (target->processed)                                  // Check target
        return;
    target->processed = true;                               // Target checked in apply effects procedure

    // Get mask of effects for target
    uint32 mask = target->effectMask;
    if (mask == 0)                                          // No effects
        return;

    Unit* unit = m_caster->GetGUID()==target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster,target->targetGUID);
    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *caster = m_originalCaster ? m_originalCaster : m_caster;

    // Skip if m_originalCaster not available
    if (!caster)
        return;

    SpellMissInfo missInfo = target->missCondition;
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    m_damage = 0;
    m_healing = 0;

    // Fill base trigger info
    uint32 procAttacker = m_procAttacker;
    uint32 procVictim   = m_procVictim;
    uint32 procEx       = PROC_EX_NONE;

    if (missInfo==SPELL_MISS_NONE)                          // In case spell hit target, do all effect on that target
        DoSpellHitOnUnit(unit, mask);
    else if (missInfo == SPELL_MISS_REFLECT)                // In case spell reflect from target, do all effect on caster (if hit)
    {
        if (target->reflectResult == SPELL_MISS_NONE)       // If reflected spell hit caster -> do all effect on him
            DoSpellHitOnUnit(m_caster, mask);
    }

    // All calculated do it!
    // Do healing and triggers
    if (m_healing)
    {
        bool crit = caster->isSpellCrit(NULL, m_spellInfo, m_spellSchoolMask);
        uint32 addhealth = m_healing;
        if (crit)
        {
            procEx |= PROC_EX_CRITICAL_HIT;
            addhealth = caster->SpellCriticalHealingBonus(m_spellInfo, addhealth, NULL);
        }
        else
            procEx |= PROC_EX_NORMAL_HIT;
//caster->SendHealSpellLog(unitTarget, m_spellInfo->Id, addhealth, crit);
        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unitTarget, procAttacker, procVictim, procEx, addhealth, m_attackType, m_spellInfo);
    }
    // Do damage and triggers
    else if (m_damage)
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, m_spellSchoolMask);

        // Add bonuses and fill damageInfo struct
        caster->CalculateSpellDamage(&damageInfo, m_damage, m_spellInfo);
        caster->DealDamageMods(damageInfo.target, damageInfo.damage, &damageInfo.absorb);

        // Send log damage message to client
        caster->SendSpellNonMeleeDamageLog(&damageInfo);

        procEx = createProcExtendMask(&damageInfo, missInfo);
        procVictim |= PROC_FLAG_TAKEN_ANY_DAMAGE;

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unitTarget, procAttacker, procVictim, procEx, damageInfo.damage, m_attackType, m_spellInfo);

        caster->DealSpellDamage(&damageInfo, true);

        // Judgement of Blood
        if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN && (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000800000000)) && m_spellInfo->SpellIconID==153)
        {
            int32 damagePoint  = damageInfo.damage * 33 / 100;
            m_caster->CastCustomSpell(m_caster, 32220, &damagePoint, NULL, NULL, true);
        }
        // Shadow Word: Death - deals damage equal to damage done to caster if victim is not killed
        else if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PRIEST && m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000200000000) &&
            caster != unitTarget && unitTarget->isAlive())
        {
            // Redirect damage to caster if victim Alive
            damageInfo.target = caster;
            damageInfo.absorb = 0;
            damageInfo.resist = 0;
            damageInfo.blocked = 0;
            // Send log damage message to client
            caster->SendSpellNonMeleeDamageLog(&damageInfo);
            caster->DealSpellDamage(&damageInfo, true);
        }
        // Bloodthirst
        else if (m_spellInfo->SpellFamilyName == SPELLFAMILY_WARRIOR && m_spellInfo->SpellFamilyFlags & UI64LIT(0x40000000000))
        {
            uint32 BTAura = 0;
            switch(m_spellInfo->Id)
            {
                case 23881: BTAura = 23885; break;
                case 23892: BTAura = 23886; break;
                case 23893: BTAura = 23887; break;
                case 23894: BTAura = 23888; break;
                case 25251: BTAura = 25252; break;
                case 30335: BTAura = 30339; break;
                default:
                    sLog.outError("Spell::EffectSchoolDMG: Spell %u not handled in BTAura",m_spellInfo->Id);
                    break;
            }
            if (BTAura)
                m_caster->CastSpell(m_caster,BTAura,true);
        }
    }
    // Passive spell hits/misses or active spells only misses (only triggers)
    else
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, m_spellSchoolMask);
        procEx = createProcExtendMask(&damageInfo, missInfo);
        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unit, procAttacker, procVictim, procEx, 0, m_attackType, m_spellInfo);
    }

    // Call scripted function for AI if this spell is casted upon a creature
    if(unit->GetTypeId()==TYPEID_UNIT)
    {
        // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
        // ignore pets or autorepeat/melee casts for speed (not exist quest for spells (hm... )
        if( !((Creature*)unit)->isPet() && !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive() )
        {
            if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
                p->CastedCreatureOrGO(unit->GetEntry(), unit->GetGUID(), m_spellInfo->Id);
        }

        if(((Creature*)unit)->AI())
            ((Creature*)unit)->AI()->SpellHit(m_caster ,m_spellInfo);
    }

    // Call scripted function for AI if this spell is casted by a creature
    if(m_caster->GetTypeId()==TYPEID_UNIT && ((Creature*)m_caster)->AI())
        ((Creature*)m_caster)->AI()->SpellHitTarget(unit, m_spellInfo);
}

void Spell::DoSpellHitOnUnit(Unit *unit, const uint32 effectMask)
{
    if (m_caster->hasUnitState(UNIT_STAT_DIED)) 
        return;

    if(!unit || !effectMask)
        return;

    // Recheck immune (only for delayed spells)
    if( m_spellInfo->speed && (
        unit->IsImmunedToDamage(GetSpellSchoolMask(m_spellInfo),true) ||
        unit->IsImmunedToSpell(m_spellInfo,true) ))
    {
        m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_IMMUNE);
        return;
    }

    if( m_caster != unit )
    {
        // Recheck  UNIT_FLAG_NON_ATTACKABLE for delayed spells
        if (m_spellInfo->speed > 0.0f &&
            unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) &&
            unit->GetCharmerOrOwnerGUID() != m_caster->GetGUID())
        {
            m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
            return;
        }

        if( !m_caster->IsFriendlyTo(unit) )
        {
            // for delayed spells ignore not visible explicit target
            if (m_spellInfo->speed > 0.0f && unit == m_targets.getUnitTarget() &&
                !unit->isVisibleForOrDetect(m_caster,m_caster,false))
            {
                m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                return;
            }

            // not break stealth by cast targeting
            if (!(m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_BREAK_STEALTH))
                unit->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

            // can cause back attack (if detected), stealth removed at Spell::cast if spell break it
            if (!(m_spellInfo->AttributesEx & SPELL_ATTR_EX_NO_INITIAL_AGGRO) && !IsPositiveSpell(m_spellInfo->Id) &&
                m_caster->isVisibleForOrDetect(unit,unit,false))
            {
                // use speedup check to avoid re-remove after above lines
                if (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_BREAK_STEALTH)
                    unit->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

                // caster can be detected but have stealth aura
                m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

                if (!unit->IsStandState() && !unit->hasUnitState(UNIT_STAT_STUNNED))
                    unit->SetStandState(UNIT_STAND_STATE_STAND);

                if (!unit->isInCombat() && unit->GetTypeId() != TYPEID_PLAYER && ((Creature*)unit)->AI())
                    ((Creature*)unit)->AI()->AttackedBy(m_caster);

                unit->AddThreat(m_caster);
                unit->SetInCombatWith(m_caster);
                m_caster->SetInCombatWith(unit);

                if(Player *attackedPlayer = unit->GetCharmerOrOwnerPlayerOrPlayerItself())
                    m_caster->SetContestedPvP(attackedPlayer);
            }
        }
        else
        {
            // for delayed spells ignore negative spells (after duel end) for friendly targets
            if(m_spellInfo->speed > 0.0f && !IsPositiveSpell(m_spellInfo->Id))
            {
                m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                return;
            }

            // assisting case, healing and resurrection
            if(unit->hasUnitState(UNIT_STAT_ATTACK_PLAYER))
                m_caster->SetContestedPvP();
            if( unit->isInCombat() && !(m_spellInfo->AttributesEx & SPELL_ATTR_EX_NO_INITIAL_AGGRO) )
            {
                m_caster->SetInCombatState(unit->GetCombatTimer() > 0);
                unit->getHostileRefManager().threatAssist(m_caster, 0.0f, m_spellInfo);
            }
        }
    }

    // Get Data Needed for Diminishing Returns, some effects may have multiple auras, so this must be done on spell hit, not aura add
    m_diminishGroup = GetDiminishingReturnsGroupForSpell(m_spellInfo,m_triggeredByAuraSpell);
    m_diminishLevel = unit->GetDiminishing(m_diminishGroup);
    // Increase Diminishing on unit, current informations for actually casts will use values above
    if((GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_PLAYER && unit->GetTypeId() == TYPEID_PLAYER) || GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_ALL)
        unit->IncrDiminishing(m_diminishGroup);

    // Apply additional spell effects to target
    CastPreCastSpells(unit);

    for(uint32 effectNumber = 0; effectNumber < 3; ++effectNumber)
    {
        if (effectMask & (1 << effectNumber))
        {
            HandleEffects(unit, NULL, NULL, effectNumber, m_damageMultipliers[effectNumber]);
            if ( m_applyMultiplierMask & (1 << effectNumber) )
            {
                // Get multiplier
                float multiplier = m_spellInfo->DmgMultiplier[effectNumber];
                // Apply multiplier mods
                if(Player* modOwner = m_originalCaster->GetSpellModOwner())
                    modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier, this);
                m_damageMultipliers[effectNumber] *= multiplier;
            }
        }
    }
}

void Spell::DoAllEffectOnTarget(GOTargetInfo *target)
{
    if (!target)                                  // Check target
        return;
    if (target->processed)                                  // Check target
        return;
    target->processed = true;                               // Target checked in apply effects procedure

    uint32 effectMask = target->effectMask;
    if(!effectMask)
        return;

    GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
    if(!go)
        return;

    for(uint32 effectNumber = 0; effectNumber < 3; ++effectNumber)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, NULL, go, effectNumber);

    // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
    // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
    if( !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive() )
    {
        if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
            p->CastedCreatureOrGO(go->GetEntry(), go->GetGUID(), m_spellInfo->Id);
    }
}

void Spell::DoAllEffectOnTarget(ItemTargetInfo *target)
{
    if (!target)                                  // Check target
        return;
    uint32 effectMask = target->effectMask;
    if(!target->item || !effectMask)
        return;

    for(uint32 effectNumber = 0; effectNumber < 3; ++effectNumber)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, target->item, NULL, effectNumber);
}

bool Spell::IsAliveUnitPresentInTargetList()
{
    // Not need check return true
    if (m_needAliveTargetMask == 0)
        return true;

    uint8 needAliveTargetMask = m_needAliveTargetMask;

    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if( ihit->missCondition == SPELL_MISS_NONE && (needAliveTargetMask & ihit->effectMask) )
        {
            Unit *unit = m_caster->GetGUID() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);

            // either unit is alive and normal spell, or unit dead and deathonly-spell
            if (unit && (unit->isAlive() != IsDeathOnlySpell(m_spellInfo)))
                needAliveTargetMask &= ~ihit->effectMask;   // remove from need alive mask effect that have alive target
        }
    }

    // is all effects from m_needAliveTargetMask have alive targets
    return needAliveTargetMask == 0;
}

// Helper for Chain Healing
// Spell target first
// Raidmates then descending by injury suffered (MaxHealth - Health)
// Other players/mobs then descending by injury suffered (MaxHealth - Health)
struct ChainHealingOrder : public std::binary_function<const Unit*, const Unit*, bool>
{
    const Unit* MainTarget;
    ChainHealingOrder(Unit const* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(Unit const* _Left, Unit const* _Right) const
    {
        return (ChainHealingHash(_Left) < ChainHealingHash(_Right));
    }
    int32 ChainHealingHash(Unit const* Target) const
    {
        if (Target == MainTarget)
            return 0;
        else if (Target->GetTypeId() == TYPEID_PLAYER && MainTarget->GetTypeId() == TYPEID_PLAYER &&
            ((Player const*)Target)->IsInSameRaidWith((Player const*)MainTarget))
        {
            if (Target->GetHealth() == Target->GetMaxHealth())
                return 40000;
            else
                return 20000 - Target->GetMaxHealth() + Target->GetHealth();
        }
        else
            return 40000 - Target->GetMaxHealth() + Target->GetHealth();
    }
};

class ChainHealingFullHealth: std::unary_function<const Unit*, bool>
{
    public:
        const Unit* MainTarget;
        ChainHealingFullHealth(const Unit* Target) : MainTarget(Target) {};

        bool operator()(const Unit* Target)
        {
            return (Target != MainTarget && Target->GetHealth() == Target->GetMaxHealth());
        }
};

// Helper for targets nearest to the spell target
// The spell target is always first unless there is a target at _completely_ the same position (unbelievable case)
struct TargetDistanceOrder : public std::binary_function<const Unit, const Unit, bool>
{
    const Unit* MainTarget;
    TargetDistanceOrder(const Unit* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(const Unit* _Left, const Unit* _Right) const
    {
        return MainTarget->GetDistanceOrder(_Left,_Right);
    }
};

void Spell::SetTargetMap(uint32 effIndex,uint32 targetMode,UnitList& TagUnitMap)
{
    float radius;
    if (m_spellInfo->EffectRadiusIndex[effIndex])
        radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[effIndex]));
    else
        radius = GetSpellMaxRange(sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex));

    uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[effIndex];

    if(m_originalCaster)
    {
        if(Player* modOwner = m_originalCaster->GetSpellModOwner())
        {
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RADIUS, radius, this);
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget, this);
        }
    }

    uint32 unMaxTargets = m_spellInfo->MaxAffectedTargets;

    switch(targetMode)
    {
        case TARGET_RANDOM_NEARBY_LOC:
            radius *= sqrt(rand_norm()); // Get a random point in circle. Use sqrt(rand) to correct distribution when converting polar to Cartesian coordinates.
                                         // no 'break' expected since we use code in case TARGET_RANDOM_CIRCUMFERENCE_POINT!!!
        case TARGET_RANDOM_CIRCUMFERENCE_POINT:
        {
            float angle = 2.0 * M_PI * rand_norm();
            float dest_x, dest_y, dest_z;
            m_caster->GetClosePoint(dest_x, dest_y, dest_z, 0.0f, radius, angle);
            m_targets.setDestination(dest_x, dest_y, dest_z);

            TagUnitMap.push_back(m_caster);
            break;
        }
        case TARGET_RANDOM_NEARBY_DEST:
        {
            radius *= sqrt(rand_norm()); // Get a random point in circle. Use sqrt(rand) to correct distribution when converting polar to Cartesian coordinates.
            float angle = 2.0 * M_PI * rand_norm();
            float dest_x = m_targets.m_destX + cos(angle) * radius;
            float dest_y = m_targets.m_destY + sin(angle) * radius;
            float dest_z = m_caster->GetPositionZ();
            m_caster->UpdateGroundPositionZ(dest_x, dest_y, dest_z);
            m_targets.setDestination(dest_x, dest_y, dest_z);

            if (radius > 0.0f)
            {
                // caster included here?
                FillAreaTargets(TagUnitMap, dest_x, dest_y, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            }
            else
                TagUnitMap.push_back(m_caster);

            break;
        }

        case TARGET_TOTEM_EARTH:
        case TARGET_TOTEM_WATER:
        case TARGET_TOTEM_AIR:
        case TARGET_TOTEM_FIRE:
        case TARGET_SELF:
        case TARGET_SELF2:
        case TARGET_AREAEFFECT_CUSTOM:
        case TARGET_AREAEFFECT_CUSTOM_2:
        case TARGET_SUMMON:
            TagUnitMap.push_back(m_caster);
            break;
        case TARGET_RANDOM_ENEMY_CHAIN_IN_AREA:
        {
            m_targets.m_targetMask = 0;
            unMaxTargets = EffectChainTarget;
            float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
            Cell cell(p);
            cell.data.Part.reserved = ALL_DISTRICT;
            cell.SetNoCreate();

            std::list<Unit *> tempUnitMap;

            {
                MaNGOS::AnyAoETargetUnitInObjectRangeCheck u_check(m_caster, m_caster, max_range);
                MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck> searcher(tempUnitMap, u_check);

                TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, WorldTypeMapContainer > world_unit_searcher(searcher);
                TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, GridTypeMapContainer >  grid_unit_searcher(searcher);

                CellLock<GridReadGuard> cell_lock(cell, p);
                cell_lock->Visit(cell_lock, world_unit_searcher, *m_caster->GetMap(), *m_caster, max_range);
                cell_lock->Visit(cell_lock, grid_unit_searcher, *m_caster->GetMap(), *m_caster, max_range);
            }

            if(tempUnitMap.empty())
                break;

            tempUnitMap.sort(TargetDistanceOrder(m_caster));

            //Now to get us a random target that's in the initial range of the spell
            uint32 t = 0;
            std::list<Unit *>::iterator itr = tempUnitMap.begin();
            while(itr!= tempUnitMap.end() && (*itr)->IsWithinDist(m_caster,radius))
                ++t, ++itr;

            if(!t)
                break;

            itr = tempUnitMap.begin();
            std::advance(itr, rand()%t);
            Unit *pUnitTarget = *itr;
            TagUnitMap.push_back(pUnitTarget);

            tempUnitMap.erase(itr);

            tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

            t = unMaxTargets - 1;
            Unit *prev = pUnitTarget;
            std::list<Unit*>::iterator next = tempUnitMap.begin();

            while(t && next != tempUnitMap.end() )
            {
                if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if(!prev->IsWithinLOSInMap(*next))
                {
                    ++next;
                    continue;
                }

                prev = *next;
                TagUnitMap.push_back(prev);
                tempUnitMap.erase(next);
                tempUnitMap.sort(TargetDistanceOrder(prev));
                next = tempUnitMap.begin();

                --t;
            }
            break;
        }
        case TARGET_PET:
        {
            Pet* tmpUnit = m_caster->GetPet();
            if (!tmpUnit) break;
            TagUnitMap.push_back(tmpUnit);
            break;
        }
        case TARGET_CHAIN_DAMAGE:
        {
            if (EffectChainTarget <= 1)
            {
                Unit* pUnitTarget = SelectMagnetTarget();
                if(pUnitTarget)
                    TagUnitMap.push_back(pUnitTarget);
            }
            else
            {
                Unit* pUnitTarget = m_targets.getUnitTarget();
                if(!pUnitTarget)
                    break;

                unMaxTargets = EffectChainTarget;

                float max_range;
                if(m_spellInfo->DmgClass==SPELL_DAMAGE_CLASS_MELEE)
                    max_range = radius;                     //
                else
                    //FIXME: This very like horrible hack and wrong for most spells
                    max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

                CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
                Cell cell(p);
                cell.data.Part.reserved = ALL_DISTRICT;
                cell.SetNoCreate();

                Unit* originalCaster = GetOriginalCaster();
                if(originalCaster)
                {
                    std::list<Unit *> tempUnitMap;

                    {
                        MaNGOS::AnyAoETargetUnitInObjectRangeCheck u_check(pUnitTarget, originalCaster, max_range, false);
                        MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck> searcher(tempUnitMap, u_check);

                        TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, WorldTypeMapContainer > world_unit_searcher(searcher);
                        TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, GridTypeMapContainer >  grid_unit_searcher(searcher);

                        CellLock<GridReadGuard> cell_lock(cell, p);
						cell_lock->Visit(cell_lock, world_unit_searcher, *m_caster->GetMap(), *m_caster, max_range);
						cell_lock->Visit(cell_lock, grid_unit_searcher, *m_caster->GetMap(), *m_caster, max_range);
                    }

                    tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

                    if(tempUnitMap.empty())
                        break;

                    if(*tempUnitMap.begin() == pUnitTarget)
                        tempUnitMap.erase(tempUnitMap.begin());

                    TagUnitMap.push_back(pUnitTarget);
                    uint32 t = unMaxTargets - 1;
                    Unit *prev = pUnitTarget;
                    std::list<Unit*>::iterator next = tempUnitMap.begin();

                    while(t && next != tempUnitMap.end() )
                    {
                        if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                            break;

                        if(!prev->IsWithinLOSInMap(*next))
                        {
                            ++next;
                            continue;
                        }

                        prev = *next;
                        TagUnitMap.push_back(prev);
                        tempUnitMap.erase(next);
                        tempUnitMap.sort(TargetDistanceOrder(prev));
                        next = tempUnitMap.begin();

                        --t;
                    }
                }
            }
            break;
        }
        case TARGET_ALL_ENEMY_IN_AREA:
            FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_DEST_CENTER,SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_AREAEFFECT_INSTANT:
        {
            SpellTargets targetB = SPELL_TARGETS_AOE_DAMAGE;
            // Select friendly targets for positive effect
            if (IsPositiveEffect(m_spellInfo->Id, effIndex))
                targetB = SPELL_TARGETS_FRIENDLY;

            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),radius, PUSH_DEST_CENTER, targetB);

            // exclude caster
            TagUnitMap.remove(m_caster);
            break;
        }
        case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
        {
            // targets the ground, not the units in the area
            if (m_spellInfo->Effect[effIndex]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
            {
                FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_DEST_CENTER,SPELL_TARGETS_AOE_DAMAGE);

                // exclude caster (this can be important if this not original caster)
                TagUnitMap.remove(m_caster);
            }
            break;
        }
        case TARGET_DUELVSPLAYER_COORDINATES:
        {
            if(Unit* currentTarget = m_targets.getUnitTarget())
            {
                m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
                TagUnitMap.push_back(currentTarget);
            }
            break;
        }
        case TARGET_ALL_PARTY_AROUND_CASTER:
        case TARGET_ALL_PARTY_AROUND_CASTER_2:
        case TARGET_ALL_PARTY:
            FillRaidOrPartyTargets(TagUnitMap,m_caster,radius,false,true,true);
            break;
        case TARGET_RANDOM_RAID_MEMBER:
            if (m_caster->GetTypeId() == TYPEID_PLAYER)
                if(Player* target = ((Player*)m_caster)->GetNextRandomRaidMember(radius))
                    TagUnitMap.push_back(target);
            break;
        case TARGET_SINGLE_FRIEND:
        case TARGET_SINGLE_FRIEND_2:
            if(m_targets.getUnitTarget())
                TagUnitMap.push_back(m_targets.getUnitTarget());
            break;
        case TARGET_NONCOMBAT_PET:
            if(Unit* target = m_targets.getUnitTarget())
                if( target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->isPet() && ((Pet*)target)->getPetType() == MINI_PET)
                    TagUnitMap.push_back(target);
            break;
        case TARGET_CASTER_COORDINATES:
        {
            // Check original caster is GO - set its coordinates as dst cast
            WorldObject *caster = NULL;
            if (IS_GAMEOBJECT_GUID(m_originalCasterGUID))
                caster = m_caster->GetMap()->GetGameObject(m_originalCasterGUID);
            if (!caster)
                caster = m_caster;
            // Set dest for targets
            m_targets.setDestination(caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ());
            break;
        }
        case TARGET_ALL_HOSTILE_UNITS_AROUND_CASTER:
            FillAreaTargets(TagUnitMap, m_targets.m_destX, m_targets.m_destY, radius, PUSH_SELF_CENTER, SPELL_TARGETS_HOSTILE);
            break;
        case TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER:
            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),radius,PUSH_SELF_CENTER,SPELL_TARGETS_FRIENDLY);
            break;
        case TARGET_ALL_FRIENDLY_UNITS_IN_AREA:
            FillAreaTargets(TagUnitMap, m_targets.m_destX, m_targets.m_destY, radius, PUSH_DEST_CENTER, SPELL_TARGETS_FRIENDLY);
            break;
        // TARGET_SINGLE_PARTY means that the spells can only be casted on a party member and not on the caster (some seals, fire shield from imp, etc..)
        case TARGET_SINGLE_PARTY:
        {
            Unit *target = m_targets.getUnitTarget();
            // Thoses spells apparently can't be casted on the caster.
            if( target && target != m_caster)
            {
                // Can only be casted on group's members or its pets
                Group  *pGroup = NULL;

                Unit* owner = m_caster->GetCharmerOrOwner();
                Unit *targetOwner = target->GetCharmerOrOwner();
                if(owner)
                {
                    if(owner->GetTypeId() == TYPEID_PLAYER)
                    {
                        if( target == owner )
                        {
                            TagUnitMap.push_back(target);
                            break;
                        }
                        pGroup = ((Player*)owner)->GetGroup();
                    }
                }
                else if (m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    if( targetOwner == m_caster && target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->isPet())
                    {
                        TagUnitMap.push_back(target);
                        break;
                    }
                    pGroup = ((Player*)m_caster)->GetGroup();
                }

                if(pGroup)
                {
                    // Our target can also be a player's pet who's grouped with us or our pet. But can't be controlled player
                    if(targetOwner)
                    {
                        if( targetOwner->GetTypeId() == TYPEID_PLAYER &&
                            target->GetTypeId() == TYPEID_UNIT && (((Creature*)target)->isPet()) &&
                            target->GetOwnerGUID() == targetOwner->GetGUID() &&
                            pGroup->IsMember(((Player*)targetOwner)->GetGUID()))
                        {
                            TagUnitMap.push_back(target);
                        }
                    }
                    // 1Our target can be a player who is on our group
                    else if (target->GetTypeId() == TYPEID_PLAYER && pGroup->IsMember(((Player*)target)->GetGUID()))
                    {
                        TagUnitMap.push_back(target);
                    }
                }
            }
            break;
        }
        case TARGET_GAMEOBJECT:
            if(m_targets.getGOTarget())
                AddGOTarget(m_targets.getGOTarget(), effIndex);
            break;
        case TARGET_IN_FRONT_OF_CASTER:
        {
            bool inFront = m_spellInfo->SpellVisual != 3879;
            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),radius,inFront ? PUSH_IN_FRONT : PUSH_IN_BACK,SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case TARGET_LARGE_FRONTAL_CONE:
            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(), radius, PUSH_IN_FRONT_90, SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_NARROW_FRONTAL_CONE:
            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(), radius, PUSH_IN_FRONT_15, SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_DUELVSPLAYER:
        {
            Unit *target = m_targets.getUnitTarget();
            if(target)
            {
                if(m_caster->IsFriendlyTo(target))
                {
                    TagUnitMap.push_back(target);
                }
                else
                {
                    Unit* pUnitTarget = SelectMagnetTarget();
                    if(pUnitTarget)
                        TagUnitMap.push_back(pUnitTarget);
                }
            }
            break;
        }
        case TARGET_GAMEOBJECT_ITEM:
            if(m_targets.getGOTargetGUID())
                AddGOTarget(m_targets.getGOTarget(), effIndex);
            else if(m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), effIndex);
            break;
        case TARGET_MASTER:
            if(Unit* owner = m_caster->GetCharmerOrOwner())
                TagUnitMap.push_back(owner);
            break;
        case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
            // targets the ground, not the units in the area
            if (m_spellInfo->Effect[effIndex]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),radius,PUSH_DEST_CENTER,SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_MINION:
            if(m_spellInfo->Effect[effIndex] != SPELL_EFFECT_DUEL)
                TagUnitMap.push_back(m_caster);
            break;
        case TARGET_SINGLE_ENEMY:
        {
            Unit* pUnitTarget = SelectMagnetTarget();
            if(pUnitTarget)
                TagUnitMap.push_back(pUnitTarget);
            break;
        }
        case TARGET_AREAEFFECT_PARTY:
        {
            Unit* owner = m_caster->GetCharmerOrOwner();
            Player *pTarget = NULL;

            if(owner)
            {
                TagUnitMap.push_back(m_caster);
                if(owner->GetTypeId() == TYPEID_PLAYER)
                    pTarget = (Player*)owner;
            }
            else if (m_caster->GetTypeId() == TYPEID_PLAYER)
            {
                if(Unit* target = m_targets.getUnitTarget())
                {
                    if( target->GetTypeId() != TYPEID_PLAYER)
                    {
                        if(((Creature*)target)->isPet())
                        {
                            Unit *targetOwner = target->GetOwner();
                            if(targetOwner->GetTypeId() == TYPEID_PLAYER)
                                pTarget = (Player*)targetOwner;
                        }
                    }
                    else
                        pTarget = (Player*)target;
                }
            }

            Group* pGroup = pTarget ? pTarget->GetGroup() : NULL;

            if(pGroup)
            {
                uint8 subgroup = pTarget->GetSubGroup();

                for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if(Target && Target->GetSubGroup()==subgroup && !m_caster->IsHostileTo(Target))
                    {
                        if(Target->getLevel()+10 >= m_spellInfo->spellLevel && pTarget->IsWithinDistInMap(Target, radius))
                            TagUnitMap.push_back(Target);

                        if(Pet* pet = Target->GetPet())
                            if(pet->getLevel()+10 >= m_spellInfo->spellLevel && pTarget->IsWithinDistInMap(pet, radius) )
                                TagUnitMap.push_back(pet);
                    }
                }
            }
            else if (owner)
            {
                if(m_caster->IsWithinDistInMap(owner, radius))
                    TagUnitMap.push_back(owner);
            }
            else if(pTarget)
            {
                TagUnitMap.push_back(pTarget);

                if(Pet* pet = pTarget->GetPet())
                    if(pet->getLevel()+10 >= m_spellInfo->spellLevel && m_caster->IsWithinDistInMap(pet, radius) )
                        TagUnitMap.push_back(pet);
            }
            break;
        }
        case TARGET_SCRIPT:
        {
            if(m_targets.getUnitTarget())
                TagUnitMap.push_back(m_targets.getUnitTarget());
            if(m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), effIndex);
            break;
        }
        case TARGET_SELF_FISHING:
            TagUnitMap.push_back(m_caster);
            break;
        case TARGET_CHAIN_HEAL:
        {
            Unit* pUnitTarget = m_targets.getUnitTarget();
            if(!pUnitTarget)
                break;

            if (EffectChainTarget <= 1)
                TagUnitMap.push_back(pUnitTarget);
            else
            {
                unMaxTargets = EffectChainTarget;
                float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

                UnitList tempUnitMap;

                FillAreaTargets(tempUnitMap, m_caster->GetPositionX(), m_caster->GetPositionY(), max_range, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);

                if(m_caster != pUnitTarget && std::find(tempUnitMap.begin(),tempUnitMap.end(), m_caster) == tempUnitMap.end() )
                    tempUnitMap.push_front(m_caster);

                tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

                if(tempUnitMap.empty())
                    break;

                if(*tempUnitMap.begin() == pUnitTarget)
                    tempUnitMap.erase(tempUnitMap.begin());

                TagUnitMap.push_back(pUnitTarget);
                uint32 t = unMaxTargets - 1;
                Unit *prev = pUnitTarget;
                std::list<Unit*>::iterator next = tempUnitMap.begin();

                while(t && next != tempUnitMap.end() )
                {
                    if(!prev->IsWithinDist(*next, CHAIN_SPELL_JUMP_RADIUS))
                        break;

                    if(!prev->IsWithinLOSInMap(*next))
                    {
                        ++next;
                        continue;
                    }

                    if((*next)->GetHealth() == (*next)->GetMaxHealth())
                    {
                        next = tempUnitMap.erase(next);
                        continue;
                    }

                    prev = *next;
                    TagUnitMap.push_back(prev);
                    tempUnitMap.erase(next);
                    tempUnitMap.sort(TargetDistanceOrder(prev));
                    next = tempUnitMap.begin();

                    --t;
                }
            }
            break;
        }
        case TARGET_CURRENT_ENEMY_COORDINATES:
        {
            Unit* currentTarget = m_targets.getUnitTarget();
            if(currentTarget)
            {
                TagUnitMap.push_back(currentTarget);
                m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
                if(m_spellInfo->EffectImplicitTargetB[effIndex]==TARGET_ALL_ENEMY_IN_AREA_INSTANT)
                    FillAreaTargets(TagUnitMap, currentTarget->GetPositionX(), currentTarget->GetPositionY(), radius, PUSH_TARGET_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            }
            break;
        }
        case TARGET_AREAEFFECT_PARTY_AND_CLASS:
        {
            Player* targetPlayer = m_targets.getUnitTarget() && m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER
                ? (Player*)m_targets.getUnitTarget() : NULL;

            Group* pGroup = targetPlayer ? targetPlayer->GetGroup() : NULL;
            if(pGroup)
            {
                for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if( Target && Target->getLevel()+10 >= m_spellInfo->spellLevel && targetPlayer->getClass() == Target->getClass() && !m_caster->IsHostileTo(Target) && targetPlayer->IsWithinDistInMap(Target, radius) )
                    {
                        TagUnitMap.push_back(Target);
                    }
                }
            }
            else if(m_targets.getUnitTarget())
                TagUnitMap.push_back(m_targets.getUnitTarget());
            break;
        }
        case TARGET_TABLE_X_Y_Z_COORDINATES:
        {
            SpellTargetPosition const* st = sSpellMgr.GetSpellTargetPosition(m_spellInfo->Id);
            if(st)
            {
                if (st->target_mapId == m_caster->GetMapId())
                    m_targets.setDestination(st->target_X, st->target_Y, st->target_Z);
                else
                    sLog.outError( "SPELL: wrong map (%u instead %u) target coordinates for spell ID %u", st->target_mapId, m_caster->GetMapId(), m_spellInfo->Id );
            }
            else
                sLog.outError( "SPELL: unknown target coordinates for spell ID %u", m_spellInfo->Id );
            break;
        }
        case TARGET_INFRONT_OF_VICTIM:
        case TARGET_BEHIND_VICTIM:
        case TARGET_RIGHT_FROM_VICTIM:
        case TARGET_LEFT_FROM_VICTIM:
        {
            Unit *pTarget = NULL;

            // explicit cast data from client or server-side cast
            // some spell at client send caster
            if(m_targets.getUnitTarget() && m_targets.getUnitTarget()!=m_caster)
                pTarget = m_targets.getUnitTarget();
            else if(m_caster->getVictim())
                pTarget = m_caster->getVictim();
            else if(m_caster->GetTypeId() == TYPEID_PLAYER)
                pTarget = ObjectAccessor::GetUnit(*m_caster, ((Player*)m_caster)->GetSelection());

            if(pTarget)
            {
                float angle = 0.0f;
                float dist = (radius && targetMode != TARGET_BEHIND_VICTIM) ? radius : CONTACT_DISTANCE;

                switch(targetMode)
                {
                    case TARGET_INFRONT_OF_VICTIM:                   break;
                    case TARGET_BEHIND_VICTIM:      angle = M_PI;    break;
                    case TARGET_RIGHT_FROM_VICTIM:  angle = -M_PI/2;  break;
                    case TARGET_LEFT_FROM_VICTIM:   angle = M_PI/2; break;
                }

                float _target_x, _target_y, _target_z;
                pTarget->GetClosePoint(_target_x, _target_y, _target_z, pTarget->GetObjectSize(), dist, angle);
                if(pTarget->IsWithinLOS(_target_x, _target_y, _target_z))
                {
                    TagUnitMap.push_back(m_caster);
                    m_targets.setDestination(_target_x, _target_y, _target_z);
                }
            }
            break;
        }
        case TARGET_DYNAMIC_OBJECT_COORDINATES:
            // if parent spell create dynamic object extract area from it
            if(DynamicObject* dynObj = m_caster->GetDynObject(m_triggeredByAuraSpell ? m_triggeredByAuraSpell->Id : m_spellInfo->Id))
                m_targets.setDestination(dynObj->GetPositionX(), dynObj->GetPositionY(), dynObj->GetPositionZ());
            break;

        case TARGET_DYNAMIC_OBJECT_FRONT:
        case TARGET_DYNAMIC_OBJECT_LEFT_SIDE:
        case TARGET_DYNAMIC_OBJECT_RIGHT_SIDE:
            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
            {
                float angle = m_caster->GetOrientation();
                switch(targetMode)
                {
                    case TARGET_DYNAMIC_OBJECT_FRONT:                         break;
                    case TARGET_DYNAMIC_OBJECT_LEFT_SIDE:  angle -= 3*M_PI/4; break;
                    case TARGET_DYNAMIC_OBJECT_RIGHT_SIDE: angle += 3*M_PI/4; break;
                }

                float x,y;
                m_caster->GetNearPoint2D(x,y,radius,angle);
                m_targets.setDestination(x,y,m_caster->GetPositionZ());
            }

            TagUnitMap.push_back(m_caster);
            break;

        case TARGET_POINT_AT_NORTH:
        case TARGET_POINT_AT_SOUTH:
        case TARGET_POINT_AT_EAST:
        case TARGET_POINT_AT_WEST:
        case TARGET_POINT_AT_NE:
        case TARGET_POINT_AT_NW:
        case TARGET_POINT_AT_SE:
        case TARGET_POINT_AT_SW:
        {

            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
            {
                Unit* currentTarget = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;
                float angle = currentTarget != m_caster ? currentTarget->GetAngle(m_caster) : m_caster->GetOrientation();

                switch(targetMode)
                {
                    case TARGET_POINT_AT_NORTH:                    break;
                    case TARGET_POINT_AT_SOUTH: angle +=   M_PI;   break;
                    case TARGET_POINT_AT_EAST:  angle -=   M_PI/2; break;
                    case TARGET_POINT_AT_WEST:  angle +=   M_PI/2; break;
                    case TARGET_POINT_AT_NE:    angle -=   M_PI/4; break;
                    case TARGET_POINT_AT_NW:    angle +=   M_PI/4; break;
                    case TARGET_POINT_AT_SE:    angle -= 3*M_PI/4; break;
                    case TARGET_POINT_AT_SW:    angle += 3*M_PI/4; break;
                }

                float x,y;
                currentTarget->GetNearPoint2D(x,y,radius,angle);
                m_targets.setDestination(x,y,currentTarget->GetPositionZ());
            }
            break;
        }
        case TARGET_EFFECT_SELECT:
        {
            // add here custom effects that need default target.
            // FOR EVERY TARGET TYPE THERE IS A DIFFERENT FILL!!
            switch(m_spellInfo->Effect[effIndex])
            {
                case SPELL_EFFECT_DUMMY:
                {
                    switch(m_spellInfo->Id)
                    {
                        case 20577:                         // Cannibalize
                        {
                            WorldObject* result = FindCorpseUsing<MaNGOS::CannibalizeObjectCheck> ();

                            if(result)
                            {
                                switch(result->GetTypeId())
                                {
                                    case TYPEID_UNIT:
                                    case TYPEID_PLAYER:
                                        TagUnitMap.push_back((Unit*)result);
                                        break;
                                    case TYPEID_CORPSE:
                                        m_targets.setCorpseTarget((Corpse*)result);
                                        if(Player* owner = ObjectAccessor::FindPlayer(((Corpse*)result)->GetOwnerGUID()))
                                            TagUnitMap.push_back(owner);
                                        break;
                                }
                            }
                            else
                            {
                                // clear cooldown at fail
                                if(m_caster->GetTypeId() == TYPEID_PLAYER)
                                    ((Player*)m_caster)->RemoveSpellCooldown(m_spellInfo->Id, true);
                                SendCastResult(SPELL_FAILED_NO_EDIBLE_CORPSES);
                                finish(false);
                            }
                            break;
                        }
                        default:
                            if(m_targets.getUnitTarget())
                                TagUnitMap.push_back(m_targets.getUnitTarget());
                            break;
                    }
                    break;
                }
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_PARRY:
                case SPELL_EFFECT_BLOCK:
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_TRIGGER_SPELL:
                case SPELL_EFFECT_TRIGGER_MISSILE:
                case SPELL_EFFECT_LEARN_SPELL:
                case SPELL_EFFECT_SKILL_STEP:
                case SPELL_EFFECT_PROFICIENCY:
                case SPELL_EFFECT_SUMMON_POSSESSED:
                case SPELL_EFFECT_SUMMON_OBJECT_WILD:
                case SPELL_EFFECT_SELF_RESURRECT:
                case SPELL_EFFECT_REPUTATION:
                case SPELL_EFFECT_SEND_TAXI:
                    if(m_targets.getUnitTarget())
                        TagUnitMap.push_back(m_targets.getUnitTarget());
                    break;
                case SPELL_EFFECT_SUMMON_PLAYER:
                    if(m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->GetSelection() && !(m_caster->GetMap()->IsDungeon()))
                    {
                        Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelection());
                        if(target)
                            TagUnitMap.push_back(target);
                    }
                    break;
                case SPELL_EFFECT_RESURRECT_NEW:
                    if(m_targets.getUnitTarget())
                        TagUnitMap.push_back(m_targets.getUnitTarget());
                    if(m_targets.getCorpseTargetGUID())
                    {
                        Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster, m_targets.getCorpseTargetGUID());
                        if(corpse)
                        {
                            Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID());
                            if(owner)
                                TagUnitMap.push_back(owner);
                        }
                    }
                    break;
                case SPELL_EFFECT_SUMMON:
                    if (m_spellInfo->EffectMiscValueB[effIndex] == SUMMON_TYPE_POSESSED ||
                        m_spellInfo->EffectMiscValueB[effIndex] == SUMMON_TYPE_POSESSED2)
                    {
                        if(m_targets.getUnitTarget())
                            TagUnitMap.push_back(m_targets.getUnitTarget());
                    }
                    else
                        TagUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_SUMMON_CHANGE_ITEM:
                case SPELL_EFFECT_SUMMON_WILD:
                case SPELL_EFFECT_SUMMON_GUARDIAN:
                case SPELL_EFFECT_TRANS_DOOR:
                case SPELL_EFFECT_ADD_FARSIGHT:
                case SPELL_EFFECT_STUCK:
                case SPELL_EFFECT_DESTROY_ALL_TOTEMS:
                case SPELL_EFFECT_SUMMON_DEMON:
                case SPELL_EFFECT_SKILL:
                    TagUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_LEARN_PET_SPELL:
                    if(Pet* pet = m_caster->GetPet())
                        TagUnitMap.push_back(pet);
                    break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                case SPELL_EFFECT_DISENCHANT:
                case SPELL_EFFECT_FEED_PET:
                case SPELL_EFFECT_PROSPECTING:
                    if(m_targets.getItemTarget())
                        AddItemTarget(m_targets.getItemTarget(), effIndex);
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                    switch(m_spellInfo->EffectApplyAuraName[effIndex])
                    {
                        case SPELL_AURA_ADD_FLAT_MODIFIER:  // some spell mods auras have 0 target modes instead expected TARGET_SELF(1) (and present for other ranks for same spell for example)
                        case SPELL_AURA_ADD_PCT_MODIFIER:
                            TagUnitMap.push_back(m_caster);
                            break;
                        default:                            // apply to target in other case
                            if(m_targets.getUnitTarget())
                                TagUnitMap.push_back(m_targets.getUnitTarget());
                            break;
                    }
                    break;
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    // AreaAura
                    if(m_spellInfo->Attributes == 0x9050000 || m_spellInfo->Attributes == 0x10000)
                        SetTargetMap(effIndex, TARGET_AREAEFFECT_PARTY, TagUnitMap);
                    break;
                case SPELL_EFFECT_SKIN_PLAYER_CORPSE:
                    if(m_targets.getUnitTarget())
                    {
                        TagUnitMap.push_back(m_targets.getUnitTarget());
                    }
                    else if (m_targets.getCorpseTargetGUID())
                    {
                        if (Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster,m_targets.getCorpseTargetGUID()))
                            if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID()))
                                TagUnitMap.push_back(owner);
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            //sLog.outError( "SPELL: Unknown implicit target (%u) for spell ID %u", targetMode, m_spellInfo->Id );
            break;
    }

    if (unMaxTargets && TagUnitMap.size() > unMaxTargets)
    {
        // make sure one unit is always removed per iteration
        uint32 removed_utarget = 0;
        for (UnitList::iterator itr = TagUnitMap.begin(), next; itr != TagUnitMap.end(); itr = next)
        {
            next = itr;
            ++next;
            if (!*itr) continue;
            if ((*itr) == m_targets.getUnitTarget())
            {
                TagUnitMap.erase(itr);
                removed_utarget = 1;
                //        break;
            }
        }
        // remove random units from the map
        while (TagUnitMap.size() > unMaxTargets - removed_utarget)
        {
            uint32 poz = urand(0, TagUnitMap.size()-1);
            for (UnitList::iterator itr = TagUnitMap.begin(); itr != TagUnitMap.end(); ++itr, --poz)
            {
                if (!*itr) continue;

                if (!poz)
                {
                    TagUnitMap.erase(itr);
                    break;
                }
            }
        }
        // the player's target will always be added to the map
        if (removed_utarget && m_targets.getUnitTarget())
            TagUnitMap.push_back(m_targets.getUnitTarget());
    }
}

void Spell::prepare(SpellCastTargets * targets, Aura* triggeredByAura)
{
    m_targets = *targets;

    m_spellState = SPELL_STATE_PREPARING;

    m_castPositionX = m_caster->GetPositionX();
    m_castPositionY = m_caster->GetPositionY();
    m_castPositionZ = m_caster->GetPositionZ();
    m_castOrientation = m_caster->GetOrientation();

    if(triggeredByAura)
        m_triggeredByAuraSpell  = triggeredByAura->GetSpellProto();

    // create and add update event for this spell
    SpellEvent* Event = new SpellEvent(this);
    m_caster->m_Events.AddEvent(Event, m_caster->m_Events.CalculateTime(1));

    //Prevent casting at cast another spell (ServerSide check)
    if(m_caster->IsNonMeleeSpellCasted(false, true) && m_cast_count)
    {
        SendCastResult(SPELL_FAILED_SPELL_IN_PROGRESS);
        finish(false);
        return;
    }

	if(uint8 result = sObjectMgr.IsSpellDisabled(m_spellInfo->Id))
	{
		if(m_caster->GetTypeId() == TYPEID_PLAYER)
		{
			sLog.outDebug("Player %s cast a spell %u which was disabled by server administrator",   m_caster->GetName(), m_spellInfo->Id);
			if(result == 2)
			sLog.outChar("Player %s cast a spell %u which was disabled by server administrator and marked as CheatSpell",   m_caster->GetName(), m_spellInfo->Id);
		}
		SendCastResult(SPELL_FAILED_SPELL_UNAVAILABLE);
        finish(false);
        return;
	}

    // Fill cost data
    m_powerCost = CalculatePowerCost();

    SpellCastResult result = CheckCast(true);
    if(result != SPELL_CAST_OK && !IsAutoRepeat())          //always cast autorepeat dummy for triggering
    {
        if(triggeredByAura)
        {
            SendChannelUpdate(0);
            triggeredByAura->SetAuraDuration(0);
        }
        SendCastResult(result);
        finish(false);
        return;
    }

    // Prepare data for triggers
    prepareDataForTriggerSystem();

    // calculate cast time (calculated after first CheckCast check to prevent charge counting for first CheckCast fail)
    m_casttime = GetSpellCastTime(m_spellInfo, this);

    // set timer base at cast time
    ReSetTimer();

    // stealth must be removed at cast starting (at show channel bar)
    // skip triggered spell (item equip spell casting and other not explicit character casts/item uses)
    if ( !m_IsTriggeredSpell && isSpellBreakStealth(m_spellInfo) )
    {
        m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);
        m_caster->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
    }

    if(m_IsTriggeredSpell)
        cast(true);
    else
    {
        m_caster->SetCurrentCastedSpell( this );
        SendSpellStart();
    }
}

void Spell::cancel()
{
    if(m_spellState == SPELL_STATE_FINISHED)
        return;

    m_autoRepeat = false;
    switch (m_spellState)
    {
        case SPELL_STATE_PREPARING:
        case SPELL_STATE_DELAYED:
        {
            SendInterrupted(0);
            SendCastResult(SPELL_FAILED_INTERRUPTED);
        } break;

        case SPELL_STATE_CASTING:
        {
            for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            {
                if( ihit->missCondition == SPELL_MISS_NONE )
                {
                    Unit* unit = m_caster->GetGUID()==(*ihit).targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                    if( unit && unit->isAlive() )
                        unit->RemoveAurasDueToSpell(m_spellInfo->Id);
                }
            }

            m_caster->RemoveAurasDueToSpell(m_spellInfo->Id);
            SendChannelUpdate(0);
            SendInterrupted(0);
            SendCastResult(SPELL_FAILED_INTERRUPTED);
        } break;

        default:
        {
        } break;
    }

    finish(false);
    m_caster->RemoveDynObject(m_spellInfo->Id);
    m_caster->RemoveGameObject(m_spellInfo->Id,true);
}

void Spell::cast(bool skipCheck)
{
    SetExecutedCurrently(true);

    // update pointers base at GUIDs to prevent access to non-existed already object
    UpdatePointers();

    // cancel at lost main target unit
    if(!m_targets.getUnitTarget() && m_targets.getUnitTargetGUID() && m_targets.getUnitTargetGUID() != m_caster->GetGUID())
    {
        cancel();
        SetExecutedCurrently(false);
        return;
    }

    if(m_caster->GetTypeId() != TYPEID_PLAYER && m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
        m_caster->SetInFront(m_targets.getUnitTarget());

    SpellCastResult castResult = CheckPower();
    if(castResult != SPELL_CAST_OK)
    {
        SendCastResult(castResult);
        finish(false);
        SetExecutedCurrently(false);
        return;
    }

    // triggered cast called from Spell::prepare where it was already checked
    if(!skipCheck)
    {
        castResult = CheckCast(false);
        if(castResult != SPELL_CAST_OK)
        {
            SendCastResult(castResult);
            finish(false);
            SetExecutedCurrently(false);
            return;
        }
    }

    // different triggred (for caster) and precast (casted before apply effect to target) cases
    switch(m_spellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            if (m_spellInfo->Mechanic == MECHANIC_BANDAGE)  // Bandages
                AddPrecastSpell(11196);                     // Recently Bandaged
            else if(m_spellInfo->SpellIconID == 1662 && m_spellInfo->AttributesEx & 0x20)
                                                            // Blood Fury (Racial)
                AddPrecastSpell(23230);                     // Blood Fury - Healing Reduction
            break;
        }
        case SPELLFAMILY_MAGE:
        {
            // Ice Block
            if(m_spellInfo->CasterAuraStateNot==AURA_STATE_HYPOTHERMIA)
                AddPrecastSpell(41425);                     // Hypothermia
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            // Power Word: Shield
            if(m_spellInfo->CasterAuraStateNot==AURA_STATE_WEAKENED_SOUL || m_spellInfo->TargetAuraStateNot==AURA_STATE_WEAKENED_SOUL)
                AddPrecastSpell(6788);                      // Weakened Soul
            // Prayer of Mending (jump animation), we need formal caster instead original for correct animation
            else if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000002000000000))
                AddTriggeredSpell(41637);

            switch(m_spellInfo->Id)
            {
                case 15237: AddTriggeredSpell(23455); break;// Holy Nova, rank 1
                case 15430: AddTriggeredSpell(23458); break;// Holy Nova, rank 2
                case 15431: AddTriggeredSpell(23459); break;// Holy Nova, rank 3
                case 27799: AddTriggeredSpell(27803); break;// Holy Nova, rank 4
                case 27800: AddTriggeredSpell(27804); break;// Holy Nova, rank 5
                case 27801: AddTriggeredSpell(27805); break;// Holy Nova, rank 6
                case 25331: AddTriggeredSpell(25329); break;// Holy Nova, rank 7
                default:break;
            }
            break;      
        }
        case SPELLFAMILY_PALADIN:
        {
            // Divine Shield, Divine Protection, Blessing of Protection or Avenging Wrath
            if(m_spellInfo->CasterAuraStateNot==AURA_STATE_FORBEARANCE || m_spellInfo->TargetAuraStateNot==AURA_STATE_FORBEARANCE)
                AddPrecastSpell(25771);                     // Forbearance         
          break;
        }
        default:
            break;
    }

    // traded items have trade slot instead of guid in m_itemTargetGUID
    // set to real guid to be sent later to the client
    m_targets.updateTradeSlotItem();

    FillTargetMap();

    if(m_spellState == SPELL_STATE_FINISHED)                // stop cast if spell marked as finish somewhere in FillTargetMap
    {
        SetExecutedCurrently(false);
        return;
    }

    // CAST SPELL
    SendSpellCooldown();

    TakePower();
    TakeReagents();                                         // we must remove reagents before HandleEffects to allow place crafted item in same slot

    SendCastResult(castResult);
    SendSpellGo();                                          // we must send smsg_spell_go packet before m_castItem delete in TakeCastItem()...


    // Okay, everything is prepared. Now we need to distinguish between immediate and evented delayed spells
    if (m_spellInfo->speed > 0.0f)
    {

        // Remove used for cast item if need (it can be already NULL after TakeReagents call
        // in case delayed spell remove item at cast delay start
        TakeCastItem();

        // Okay, maps created, now prepare flags
        m_immediateHandled = false;
        m_spellState = SPELL_STATE_DELAYED;
        SetDelayStart(0);
    }
    else
    {
        // Immediate spell, no big deal
        handle_immediate();
    }

    SetExecutedCurrently(false);
}

void Spell::handle_immediate()
{
    // start channeling if applicable
    if(IsChanneledSpell(m_spellInfo))
    {
        m_spellState = SPELL_STATE_CASTING;
        SendChannelStart(GetSpellDuration(m_spellInfo));
    }

    // process immediate effects (items, ground, etc.) also initialize some variables
    _handle_immediate_phase();

    for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
        DoAllEffectOnTarget(&(*ihit));

    for(std::list<GOTargetInfo>::iterator ihit= m_UniqueGOTargetInfo.begin();ihit != m_UniqueGOTargetInfo.end();++ihit)
        DoAllEffectOnTarget(&(*ihit));

    // spell is finished, perform some last features of the spell here
    _handle_finish_phase();

    // Remove used for cast item if need (it can be already NULL after TakeReagents call
    TakeCastItem();

    if(m_spellState != SPELL_STATE_CASTING)
        finish(true);                                       // successfully finish spell cast (not last in case autorepeat or channel spell)
}

uint64 Spell::handle_delayed(uint64 t_offset)
{
    uint64 next_time = 0;

    if (!m_immediateHandled)
    {
        _handle_immediate_phase();
        m_immediateHandled = true;
    }

    // now recheck units targeting correctness (need before any effects apply to prevent adding immunity at first effect not allow apply second spell effect and similar cases)
    for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if (ihit->processed == false)
        {
            if ( ihit->timeDelay <= t_offset )
                DoAllEffectOnTarget(&(*ihit));
            else if( next_time == 0 || ihit->timeDelay < next_time )
                next_time = ihit->timeDelay;
        }
    }

    // now recheck gameobject targeting correctness
    for(std::list<GOTargetInfo>::iterator ighit= m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end();++ighit)
    {
        if (ighit->processed == false)
        {
            if ( ighit->timeDelay <= t_offset )
                DoAllEffectOnTarget(&(*ighit));
            else if( next_time == 0 || ighit->timeDelay < next_time )
                next_time = ighit->timeDelay;
        }
    }
    // All targets passed - need finish phase
    if (next_time == 0)
    {
        // spell is finished, perform some last features of the spell here
        _handle_finish_phase();

        finish(true);                                       // successfully finish spell cast

        // return zero, spell is finished now
        return 0;
    }
    else
    {
        // spell is unfinished, return next execution time
        return next_time;
    }
}

void Spell::_handle_immediate_phase()
{
    // handle some immediate features of the spell here
    HandleThreatSpells(m_spellInfo->Id);

    m_needSpellLog = IsNeedSendToClient();
    for(uint32 j = 0; j < 3; ++j)
    {
        if(m_spellInfo->Effect[j] == 0)
            continue;

        // apply Send Event effect to ground in case empty target lists
        if( m_spellInfo->Effect[j] == SPELL_EFFECT_SEND_EVENT && !HaveTargetsForEffect(j) )
        {
            HandleEffects(NULL, NULL, NULL, j);
            continue;
        }

        // Don't do spell log, if is school damage spell
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_SCHOOL_DAMAGE || m_spellInfo->Effect[j] == 0)
            m_needSpellLog = false;

        uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[j];
        if(m_originalCaster)
            if(Player* modOwner = m_originalCaster->GetSpellModOwner())
                modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget, this);

        // initialize multipliers
        m_damageMultipliers[j] = 1.0f;
        if( (m_spellInfo->EffectImplicitTargetA[j] == TARGET_CHAIN_DAMAGE || m_spellInfo->EffectImplicitTargetA[j] == TARGET_CHAIN_HEAL) &&
            (EffectChainTarget > 1) )
            m_applyMultiplierMask |= 1 << j;
    }

    // initialize Diminishing Returns Data
    m_diminishLevel = DIMINISHING_LEVEL_1;
    m_diminishGroup = DIMINISHING_NONE;

    // process items
    for(std::list<ItemTargetInfo>::iterator ihit= m_UniqueItemInfo.begin();ihit != m_UniqueItemInfo.end();++ihit)
        DoAllEffectOnTarget(&(*ihit));

    // process ground
    for(uint32 j = 0; j < 3; ++j)
    {
        // persistent area auras target only the ground
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            HandleEffects(NULL, NULL, NULL, j);
    }
}

void Spell::_handle_finish_phase()
{
    // spell log
    if(m_needSpellLog)
        SendLogExecute();
}

void Spell::SendSpellCooldown()
{
    if(m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player* _player = (Player*)m_caster;

    // (1) have infinity cooldown but set at aura apply, (2) passive cooldown at triggering
    if(m_spellInfo->Attributes & (SPELL_ATTR_DISABLED_WHILE_ACTIVE | SPELL_ATTR_PASSIVE))
        return;

    _player->AddSpellAndCategoryCooldowns(m_spellInfo,m_CastItem ? m_CastItem->GetEntry() : 0, this);
}

void Spell::update(uint32 difftime)
{
    // update pointers based at it's GUIDs
    UpdatePointers();

    if(m_targets.getUnitTargetGUID() && !m_targets.getUnitTarget())
    {
        cancel();
        return;
    }

    // check if the player caster has moved before the spell finished
    if ((m_caster->GetTypeId() == TYPEID_PLAYER && m_timer != 0) &&
        (m_castPositionX != m_caster->GetPositionX() || m_castPositionY != m_caster->GetPositionY() || m_castPositionZ != m_caster->GetPositionZ()) &&
        (m_spellInfo->Effect[0] != SPELL_EFFECT_STUCK || !((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING)))
    {
        // always cancel for channeled spells
        if( m_spellState == SPELL_STATE_CASTING )
            cancel();
        // don't cancel for melee, autorepeat, triggered and instant spells
        else if(!IsNextMeleeSwingSpell() && !IsAutoRepeat() && !m_IsTriggeredSpell && (m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
            cancel();
    }

    switch(m_spellState)
    {
        case SPELL_STATE_PREPARING:
        {
            if(m_timer)
            {
                if(difftime >= m_timer)
                    m_timer = 0;
                else
                    m_timer -= difftime;
            }

            if(m_timer == 0 && !IsNextMeleeSwingSpell() && !IsAutoRepeat())
                cast();
        } break;
        case SPELL_STATE_CASTING:
        {
            if(m_timer > 0)
            {
                if( m_caster->GetTypeId() == TYPEID_PLAYER )
                {
                    // check if player has jumped before the channeling finished
                    if(((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_JUMPING))
                        cancel();

                    // check for incapacitating player states
                    if( m_caster->hasUnitState(UNIT_STAT_STUNNED | UNIT_STAT_CONFUSED))
                        cancel();

                    // check if player has turned if flag is set
                    if( m_spellInfo->ChannelInterruptFlags & CHANNEL_FLAG_TURNING && m_castOrientation != m_caster->GetOrientation() )
                        cancel();
                }

                // check if there are alive targets left
                if (!IsAliveUnitPresentInTargetList())
                {
                    SendChannelUpdate(0);
                    finish();
                }

                if(difftime >= m_timer)
                    m_timer = 0;
                else
                    m_timer -= difftime;
            }

            if(m_timer == 0)
            {
                SendChannelUpdate(0);

                // channeled spell processed independently for quest targeting
                // cast at creature (or GO) quest objectives update at successful cast channel finished
                // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
                if( !IsAutoRepeat() && !IsNextMeleeSwingSpell() )
                {
                    if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
                    {
                        for(std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                        {
                            TargetInfo* target = &*ihit;
                            if(!IS_CREATURE_GUID(target->targetGUID))
                                continue;

                            Unit* unit = m_caster->GetGUID() == target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, target->targetGUID);
                            if (unit == NULL)
                                continue;

                            p->CastedCreatureOrGO(unit->GetEntry(), unit->GetGUID(), m_spellInfo->Id);
                        }

                        for(std::list<GOTargetInfo>::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
                        {
                            GOTargetInfo* target = &*ihit;

                            GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
                            if(!go)
                                continue;

                            p->CastedCreatureOrGO(go->GetEntry(), go->GetGUID(), m_spellInfo->Id);
                        }
                    }
                }

                finish();
            }
        } break;
        default:
        {
        }break;
    }
}

void Spell::finish(bool ok)
{
    if(!m_caster)
        return;

    if(m_spellState == SPELL_STATE_FINISHED)
        return;

    m_spellState = SPELL_STATE_FINISHED;

    // other code related only to successfully finished spells
    if(!ok)
        return;

    //remove spell mods
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->RemoveSpellMods(this);

    //handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    Unit::AuraList const& targetTriggers = m_caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
    for(Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
    {
        SpellEntry const *auraSpellInfo = (*i)->GetSpellProto();
        uint32 auraSpellIdx = (*i)->GetEffIndex();
        if (IsAffectedBy(auraSpellInfo, auraSpellIdx))
        {
            for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
                if( ihit->effectMask & (1<<auraSpellIdx) )
            {
                // check m_caster->GetGUID() let load auras at login and speedup most often case
                Unit *unit = m_caster->GetGUID()== ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                if (unit && unit->isAlive())
                {
                    // Calculate chance at that moment (can be depend for example from combo points)
                    int32 chance = m_caster->CalculateSpellDamage(auraSpellInfo, auraSpellIdx, (*i)->GetBasePoints(),unit);

                    if(roll_chance_i(chance))
                        m_caster->CastSpell(unit, auraSpellInfo->EffectTriggerSpell[auraSpellIdx], true, NULL, (*i));
                }
            }
        }
    }

    // Heal caster for all health leech from all targets
    if (m_healthLeech)
    {
        m_caster->DealHeal(m_caster, uint32(m_healthLeech), m_spellInfo);
        m_caster->SendHealSpellLog(m_caster, m_spellInfo->Id, uint32(m_healthLeech));
    }

    if (IsMeleeAttackResetSpell())
    {
        m_caster->resetAttackTimer(BASE_ATTACK);
        if(m_caster->haveOffhandWeapon())
            m_caster->resetAttackTimer(OFF_ATTACK);
    }

    /*if (IsRangedAttackResetSpell())
        m_caster->resetAttackTimer(RANGED_ATTACK);*/

    // Clear combo at finish state
    if(m_caster->GetTypeId() == TYPEID_PLAYER && NeedsComboPoints(m_spellInfo))
    {
        // Not drop combopoints if negative spell and if any miss on enemy exist
        bool needDrop = true;
        if (!IsPositiveSpell(m_spellInfo->Id))
        {
            for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            {
                if (ihit->missCondition != SPELL_MISS_NONE && ihit->targetGUID!=m_caster->GetGUID())
                {
                    needDrop = false;
                    break;
                }
            }
        }
        if (needDrop)
            ((Player*)m_caster)->ClearComboPoints();
    }

    // call triggered spell only at successful cast (after clear combo points -> for add some if need)
    if(!m_TriggerSpells.empty())
        CastTriggerSpells();

    // Stop Attack for some spells
    if( m_spellInfo->Attributes & SPELL_ATTR_STOP_ATTACK_TARGET )
        m_caster->AttackStop();
}

void Spell::SendCastResult(SpellCastResult result)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if(((Player*)m_caster)->GetSession()->PlayerLoading())  // don't send cast results at loading time
        return;

    SendCastResult((Player*)m_caster,m_spellInfo,m_cast_count,result);
}

void Spell::SendCastResult(Player* caster, SpellEntry const* spellInfo, uint8 cast_count, SpellCastResult result)
{
    if(result == SPELL_CAST_OK)
    {
        WorldPacket data(SMSG_CLEAR_EXTRA_AURA_INFO, (8+4));
        data.append(caster->GetPackGUID());
        data << uint32(spellInfo->Id);
        caster->GetSession()->SendPacket(&data);
        return;
    }

    WorldPacket data(SMSG_CAST_FAILED, (4+1+1));
    data << uint32(spellInfo->Id);
    data << uint8(result);                                  // problem
    data << uint8(cast_count);                              // single cast or multi 2.3 (0/1)
    switch (result)
    {
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            data << uint32(spellInfo->RequiresSpellFocus);
            break;
        case SPELL_FAILED_REQUIRES_AREA:
            // hardcode areas limitation case
            switch(spellInfo->Id)
            {
                case 41617:                                 // Cenarion Mana Salve
                case 41619:                                 // Cenarion Healing Salve
                    data << uint32(3905);
                    break;
                case 41618:                                 // Bottled Nethergon Energy
                case 41620:                                 // Bottled Nethergon Vapor
                    data << uint32(3842);
                    break;
                case 45373:                                 // Bloodberry Elixir
                    data << uint32(4075);
                    break;
                default:                                    // default case
                    data << uint32(spellInfo->AreaId);
                    break;
            }
            break;
        case SPELL_FAILED_TOTEMS:
            if(spellInfo->Totem[0])
                data << uint32(spellInfo->Totem[0]);
            if(spellInfo->Totem[1])
                data << uint32(spellInfo->Totem[1]);
            break;
        case SPELL_FAILED_TOTEM_CATEGORY:
            if(spellInfo->TotemCategory[0])
                data << uint32(spellInfo->TotemCategory[0]);
            if(spellInfo->TotemCategory[1])
                data << uint32(spellInfo->TotemCategory[1]);
            break;
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
            data << uint32(spellInfo->EquippedItemClass);
            data << uint32(spellInfo->EquippedItemSubClassMask);
            data << uint32(spellInfo->EquippedItemInventoryTypeMask);
            break;
        default:
            break;
    }
    caster->GetSession()->SendPacket(&data);
}

void Spell::SendSpellStart()
{
    if(!IsNeedSendToClient())
        return;

    sLog.outDebug("Sending SMSG_SPELL_START id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN1;
    if(IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;


    WorldPacket data(SMSG_SPELL_START, (8+8+4+4+2));
    if(m_CastItem)
        data.append(m_CastItem->GetPackGUID());
    else
        data.append(m_caster->GetPackGUID());

    data.append(m_caster->GetPackGUID());
    data << uint32(m_spellInfo->Id);
    data << uint8(m_cast_count);                            // single cast or multi 2.3 (0/1)
    data << uint16(castFlags);
    data << uint32(m_timer);

    m_targets.write(&data);

    if( castFlags & CAST_FLAG_AMMO )
        WriteAmmoToPacket(&data);

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendSpellGo()
{
    // not send invisible spell casting
    if(!IsNeedSendToClient())
        return;

    sLog.outDebug("Sending SMSG_SPELL_GO id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN3;
    if(IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;                        // arrows/bullets visual

    WorldPacket data(SMSG_SPELL_GO, 50);                    // guess size

    if(m_CastItem)
        data.append(m_CastItem->GetPackGUID());
    else
        data.append(m_caster->GetPackGUID());

    data.append(m_caster->GetPackGUID());
    data << uint32(m_spellInfo->Id);
    data << uint16(castFlags);
    data << uint32(getMSTime());                            // timestamp

    WriteSpellGoTargets(&data);

    m_targets.write(&data);

    if( castFlags & CAST_FLAG_AMMO )
        WriteAmmoToPacket(&data);

    m_caster->SendMessageToSet(&data, true);
}

void Spell::WriteAmmoToPacket( WorldPacket * data )
{
    uint32 ammoInventoryType = 0;
    uint32 ammoDisplayID = 0;

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Item *pItem = ((Player*)m_caster)->GetWeaponForAttack( RANGED_ATTACK );
        if(pItem)
        {
            ammoInventoryType = pItem->GetProto()->InventoryType;
            if( ammoInventoryType == INVTYPE_THROWN )
                ammoDisplayID = pItem->GetProto()->DisplayInfoID;
            else
            {
                uint32 ammoID = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                if(ammoID)
                {
                    ItemPrototype const *pProto = ObjectMgr::GetItemPrototype( ammoID );
                    if(pProto)
                    {
                        ammoDisplayID = pProto->DisplayInfoID;
                        ammoInventoryType = pProto->InventoryType;
                    }
                }
                else if(m_caster->GetDummyAura(46699))      // Requires No Ammo
                {
                    ammoDisplayID = 5996;                   // normal arrow
                    ammoInventoryType = INVTYPE_AMMO;
                }
            }
        }
    }
    // TODO: implement selection ammo data based at ranged weapon stored in equipmodel/equipinfo/equipslot fields

    *data << uint32(ammoDisplayID);
    *data << uint32(ammoInventoryType);
}

void Spell::WriteSpellGoTargets( WorldPacket * data )
{
    *data << (uint8)m_countOfHit;
    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
        if ((*ihit).missCondition == SPELL_MISS_NONE)       // Add only hits
            *data << uint64(ihit->targetGUID);

    for(std::list<GOTargetInfo>::const_iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
        *data << uint64(ighit->targetGUID);                 // Always hits

    *data << (uint8)m_countOfMiss;
    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if( ihit->missCondition != SPELL_MISS_NONE )        // Add only miss
        {
            *data << uint64(ihit->targetGUID);
            *data << uint8(ihit->missCondition);
            if( ihit->missCondition == SPELL_MISS_REFLECT )
                *data << uint8(ihit->reflectResult);
        }
    }
}

void Spell::SendLogExecute()
{
    Unit *target = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;

    WorldPacket data(SMSG_SPELLLOGEXECUTE, (8+4+4+4+4+8));

    if(m_caster->GetTypeId() == TYPEID_PLAYER)
        data.append(m_caster->GetPackGUID());
    else
        data.append(target->GetPackGUID());

    data << uint32(m_spellInfo->Id);
    uint32 count1 = 1;
    data << uint32(count1);                                 // count1 (effect count?)
    for(uint32 i = 0; i < count1; ++i)
    {
        data << uint32(m_spellInfo->Effect[0]);             // spell effect
        uint32 count2 = 1;
        data << uint32(count2);                             // count2 (target count?)
        for(uint32 j = 0; j < count2; ++j)
        {
            switch(m_spellInfo->Effect[0])
            {
                case SPELL_EFFECT_POWER_DRAIN:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);
                    data << uint32(0);
                    data << float(0);
                    break;
                case SPELL_EFFECT_ADD_EXTRA_ATTACKS:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);                      // count?
                    break;
                case SPELL_EFFECT_INTERRUPT_CAST:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);                      // spellid
                    break;
                case SPELL_EFFECT_DURABILITY_DAMAGE:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);
                    data << uint32(0);
                    break;
                case SPELL_EFFECT_OPEN_LOCK:
                case SPELL_EFFECT_OPEN_LOCK_ITEM:
                    if(Item *item = m_targets.getItemTarget())
                        data.append(item->GetPackGUID());
                    else
                        data << uint8(0);
                    break;
                case SPELL_EFFECT_CREATE_ITEM:
                    data << uint32(m_spellInfo->EffectItemType[0]);
                    break;
                case SPELL_EFFECT_SUMMON:
                case SPELL_EFFECT_SUMMON_WILD:
                case SPELL_EFFECT_SUMMON_GUARDIAN:
                case SPELL_EFFECT_TRANS_DOOR:
                case SPELL_EFFECT_SUMMON_PET:
                case SPELL_EFFECT_SUMMON_POSSESSED:
                case SPELL_EFFECT_SUMMON_TOTEM:
                case SPELL_EFFECT_SUMMON_OBJECT_WILD:
                case SPELL_EFFECT_CREATE_HOUSE:
                case SPELL_EFFECT_DUEL:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT1:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT2:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT3:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT4:
                case SPELL_EFFECT_SUMMON_PHANTASM:
                case SPELL_EFFECT_SUMMON_CRITTER:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
                case SPELL_EFFECT_SUMMON_DEMON:
                case SPELL_EFFECT_150:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else if(m_targets.getItemTargetGUID())
                        data.appendPackGUID(m_targets.getItemTargetGUID());
                    else if(GameObject *go = m_targets.getGOTarget())
                        data.append(go->GetPackGUID());
                    else
                        data << uint8(0);                   // guid
                    break;
                case SPELL_EFFECT_FEED_PET:
                    data << uint32(m_targets.getItemTargetEntry());
                    break;
                case SPELL_EFFECT_DISMISS_PET:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    break;
                default:
                    return;
            }
        }
    }

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendInterrupted(uint8 result)
{
    WorldPacket data(SMSG_SPELL_FAILURE, (8+4+1));
    data.append(m_caster->GetPackGUID());
    data << m_spellInfo->Id;
    data << result;
    m_caster->SendMessageToSet(&data, true);

    data.Initialize(SMSG_SPELL_FAILED_OTHER, (8+4));
    data.append(m_caster->GetPackGUID());
    data << m_spellInfo->Id;
    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendChannelUpdate(uint32 time)
{
    if(time == 0)
    {
        m_caster->SetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT, 0);
        m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
    }

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data( MSG_CHANNEL_UPDATE, 8+4 );
    data.append(m_caster->GetPackGUID());
    data << uint32(time);

    ((Player*)m_caster)->GetSession()->SendPacket( &data );
}

void Spell::SendChannelStart(uint32 duration)
{
    WorldObject* target = NULL;

    // select first not resisted target from target list for _0_ effect
    if(!m_UniqueTargetInfo.empty())
    {
        for(std::list<TargetInfo>::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        {
            if( (itr->effectMask & (1 << 0)) && itr->reflectResult == SPELL_MISS_NONE && itr->targetGUID != m_caster->GetGUID())
            {
                target = ObjectAccessor::GetUnit(*m_caster, itr->targetGUID);
                break;
            }
        }
    }
    else if(!m_UniqueGOTargetInfo.empty())
    {
        for(std::list<GOTargetInfo>::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        {
            if(itr->effectMask & (1 << 0) )
            {
                target = m_caster->GetMap()->GetGameObject(itr->targetGUID);
                break;
            }
        }
    }

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        WorldPacket data( MSG_CHANNEL_START, (8+4+4) );
        data.append(m_caster->GetPackGUID());
        data << uint32(m_spellInfo->Id);
        data << uint32(duration);

        ((Player*)m_caster)->GetSession()->SendPacket( &data );
    }

    m_timer = duration;
    if(target)
        m_caster->SetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT, target->GetGUID());
    m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, m_spellInfo->Id);
}

void Spell::SendResurrectRequest(Player* target)
{
    // Both players and NPCs can resurrect using spells - have a look at creature 28487 for example
    // However, the packet structure differs slightly

    const char* sentName = m_caster->GetTypeId() == TYPEID_PLAYER ? "" : m_caster->GetNameForLocaleIdx(target->GetSession()->GetSessionDbLocaleIndex());

    WorldPacket data(SMSG_RESURRECT_REQUEST, (8+4+strlen(sentName)+1+1+1));
    data << uint64(m_caster->GetGUID());
    data << uint32(strlen(sentName) + 1);

    data << sentName;
    data << uint8(0);

    data << uint8(m_caster->GetTypeId() == TYPEID_PLAYER ? 0 : 1);
    target->GetSession()->SendPacket(&data);
}

void Spell::SendPlaySpellVisual(uint32 SpellID)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 8 + 4);
    data << uint64(m_caster->GetGUID());
    data << uint32(SpellID);                                // spell visual id?
    ((Player*)m_caster)->GetSession()->SendPacket(&data);
}

void Spell::TakeCastItem()
{
    if(!m_CastItem || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    // not remove cast item at triggered spell (equipping, weapon damage, etc)
    if(m_IsTriggeredSpell)
        return;

    ItemPrototype const *proto = m_CastItem->GetProto();

    if(!proto)
    {
        // This code is to avoid a crash
        // I'm not sure, if this is really an error, but I guess every item needs a prototype
        sLog.outError("Cast item has no item prototype highId=%d, lowId=%d",m_CastItem->GetGUIDHigh(), m_CastItem->GetGUIDLow());
        return;
    }

    bool expendable = false;
    bool withoutCharges = false;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId)
        {
            // item has limited charges
            if (proto->Spells[i].SpellCharges)
            {
                if (proto->Spells[i].SpellCharges < 0)
                    expendable = true;

                int32 charges = m_CastItem->GetSpellCharges(i);

                // item has charges left
                if (charges)
                {
                    (charges > 0) ? --charges : ++charges;  // abs(charges) less at 1 after use
                    if (proto->Stackable < 2)
                        m_CastItem->SetSpellCharges(i, charges);
                    m_CastItem->SetState(ITEM_CHANGED, (Player*)m_caster);
                }

                // all charges used
                withoutCharges = (charges == 0);
            }
        }
    }

    if (expendable && withoutCharges)
    {
        uint32 count = 1;
        ((Player*)m_caster)->DestroyItemCount(m_CastItem, count, true);

        // prevent crash at access to deleted m_targets.getItemTarget
        if(m_CastItem==m_targets.getItemTarget())
            m_targets.setItemTarget(NULL);

        m_CastItem = NULL;
    }
}

void Spell::TakePower()
{
    if(m_CastItem || m_triggeredByAuraSpell)
        return;

    // health as power used
    if(m_spellInfo->powerType == POWER_HEALTH)
    {
        m_caster->ModifyHealth( -(int32)m_powerCost );
        return;
    }

    if(m_spellInfo->powerType >= MAX_POWERS)
    {
        sLog.outError("Spell::TakePower: Unknown power type '%d'", m_spellInfo->powerType);
        return;
    }

    Powers powerType = Powers(m_spellInfo->powerType);

    m_caster->ModifyPower(powerType, -(int32)m_powerCost);

    // Set the five second timer
    if (powerType == POWER_MANA && m_powerCost > 0)
        m_caster->SetLastManaUse();
}

void Spell::TakeReagents()
{
    if(m_IsTriggeredSpell)                                  // reagents used in triggered spell removed by original spell or don't must be removed.
        return;

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player* p_caster = (Player*)m_caster;
    if (p_caster->CanNoReagentCast(m_spellInfo))
        return;

    for(uint32 x = 0; x < 8; ++x)
    {
        if(m_spellInfo->Reagent[x] <= 0)
            continue;

        uint32 itemid = m_spellInfo->Reagent[x];
        uint32 itemcount = m_spellInfo->ReagentCount[x];

        // if CastItem is also spell reagent
        if (m_CastItem)
        {
            ItemPrototype const *proto = m_CastItem->GetProto();
            if( proto && proto->ItemId == itemid )
            {
                for(int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }

                m_CastItem = NULL;
            }
        }

        // if getItemTarget is also spell reagent
        if (m_targets.getItemTargetEntry() == itemid)
            m_targets.setItemTarget(NULL);

        p_caster->DestroyItemCount(itemid, itemcount, true);
    }
}

void Spell::HandleThreatSpells(uint32 spellId)
{
    if(!m_targets.getUnitTarget() || !spellId)
        return;

    if(!m_targets.getUnitTarget()->CanHaveThreatList())
        return;

    uint16 threat = sSpellMgr.GetSpellThreat(spellId);

    if(!threat)
        return;

    m_targets.getUnitTarget()->AddThreat(m_caster, float(threat), false, GetSpellSchoolMask(m_spellInfo), m_spellInfo);

    DEBUG_LOG("Spell %u, rank %u, added an additional %i threat", spellId, sSpellMgr.GetSpellRank(spellId), threat);
}

void Spell::HandleEffects(Unit *pUnitTarget,Item *pItemTarget,GameObject *pGOTarget,uint32 i, float DamageMultiplier)
{
    unitTarget = pUnitTarget;
    itemTarget = pItemTarget;
    gameObjTarget = pGOTarget;

    uint8 eff = m_spellInfo->Effect[i];

    damage = int32(CalculateDamage((uint8)i, unitTarget) * DamageMultiplier);

    sLog.outDebug( "Spell: Effect : %u", eff);

    //Simply return. Do not display "immune" in red text on client
    if(unitTarget && unitTarget->IsImmunedToSpellEffect(m_spellInfo, i))
        return;

    if(eff < TOTAL_SPELL_EFFECTS)
    {
        //sLog.outDebug( "WORLD: Spell FX %d < TOTAL_SPELL_EFFECTS ", eff);
        (*this.*SpellEffects[eff])(i);
    }
    /*
    else
    {
        sLog.outDebug( "WORLD: Spell FX %d > TOTAL_SPELL_EFFECTS ", eff);
        if (m_CastItem)
            EffectEnchantItemTmp(i);
        else
        {
            sLog.outError("SPELL: unknown effect %u spell id %u",
                eff, m_spellInfo->Id);
        }
    }
    */
}


void Spell::AddTriggeredSpell( uint32 spellId )
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId );

    if(!spellInfo)
    {
        sLog.outError("Spell::AddTriggeredSpell: unknown spell id %u used as triggred spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }

    m_TriggerSpells.push_back(spellInfo);
}

void Spell::AddPrecastSpell( uint32 spellId )
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId );

    if(!spellInfo)
    {
        sLog.outError("Spell::AddPrecastSpell: unknown spell id %u used as pre-cast spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }

    m_preCastSpells.push_back(spellInfo);
}

void Spell::CastTriggerSpells()
{
    for(SpellInfoList::const_iterator si=m_TriggerSpells.begin(); si!=m_TriggerSpells.end(); ++si)
    {
        Spell* spell = new Spell(m_caster, (*si), true, m_originalCasterGUID, m_selfContainer);
        spell->prepare(&m_targets);                         // use original spell original targets
    }
}

void Spell::CastPreCastSpells(Unit* target)
{
    for(SpellInfoList::const_iterator si=m_preCastSpells.begin(); si!=m_preCastSpells.end(); ++si)
        m_caster->CastSpell(target, (*si), true, m_CastItem);
}

SpellCastResult Spell::CheckCast(bool strict)
{
    // check cooldowns to prevent cheating (ignore passive spells, that client side visual only)
    if (m_caster->GetTypeId()==TYPEID_PLAYER && !(m_spellInfo->Attributes & SPELL_ATTR_PASSIVE) &&
        ((Player*)m_caster)->HasSpellCooldown(m_spellInfo->Id))
    {
        if(m_triggeredByAuraSpell)
            return SPELL_FAILED_DONT_REPORT;
        else
            return SPELL_FAILED_NOT_READY;
    }

    // only allow triggered spells if at an ended battleground
    if( !m_IsTriggeredSpell && m_caster->GetTypeId() == TYPEID_PLAYER)
        if(BattleGround * bg = ((Player*)m_caster)->GetBattleGround())
            if(bg->GetStatus() == STATUS_WAIT_LEAVE)
                return SPELL_FAILED_DONT_REPORT;

    // only check at first call, Stealth auras are already removed at second call
    // for now, ignore triggered spells
    if( strict && !m_IsTriggeredSpell)
    {
        // Cannot be used in this stance/form
        SpellCastResult shapeError = GetErrorAtShapeshiftedCast(m_spellInfo, m_caster->m_form);
        if(shapeError != SPELL_CAST_OK)
            return shapeError;

        if ((m_spellInfo->Attributes & SPELL_ATTR_ONLY_STEALTHED) && !(m_caster->HasStealthAura()))
            return SPELL_FAILED_ONLY_STEALTHED;
    }

    // caster state requirements
    if(m_spellInfo->CasterAuraState && !m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraState)))
        return SPELL_FAILED_CASTER_AURASTATE;
    if(m_spellInfo->CasterAuraStateNot && m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraStateNot)))
        return SPELL_FAILED_CASTER_AURASTATE;

    // cancel autorepeat spells if cast start when moving
    // (not wand currently autorepeat cast delayed to moving stop anyway in spell update code)
    if( m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->isMoving() )
    {
        // skip stuck spell to allow use it in falling case and apply spell limitations at movement
        if( (!((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING) || m_spellInfo->Effect[0] != SPELL_EFFECT_STUCK) &&
            (IsAutoRepeat() || (m_spellInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED) != 0) )
            return SPELL_FAILED_MOVING;
    }

    if(Unit *target = m_targets.getUnitTarget())
    {
        // target state requirements (not allowed state), apply to self also
        if(m_spellInfo->TargetAuraStateNot && target->HasAuraState(AuraState(m_spellInfo->TargetAuraStateNot)))
            return SPELL_FAILED_TARGET_AURASTATE;

        if (!m_IsTriggeredSpell && IsDeathOnlySpell(m_spellInfo) && target->isAlive())
            return SPELL_FAILED_TARGET_NOT_DEAD;

        bool non_caster_target = target != m_caster && !IsSpellWithCasterSourceTargetsOnly(m_spellInfo);

        if(non_caster_target)
        {
            // target state requirements (apply to non-self only), to allow cast affects to self like Dirty Deeds
            if(m_spellInfo->TargetAuraState && !target->HasAuraStateForCaster(AuraState(m_spellInfo->TargetAuraState),m_caster->GetGUID()))
                return SPELL_FAILED_TARGET_AURASTATE;

            // Not allow casting on flying player
            if (target->isInFlight())
                return SPELL_FAILED_BAD_TARGETS;

            if(!m_IsTriggeredSpell && VMAP::VMapFactory::checkSpellForLoS(m_spellInfo->Id) && !m_caster->IsWithinLOSInMap(target))
                return SPELL_FAILED_LINE_OF_SIGHT;

            // auto selection spell rank implemented in WorldSession::HandleCastSpellOpcode
            // this case can be triggered if rank not found (too low-level target for first rank)
            if (m_caster->GetTypeId() == TYPEID_PLAYER && !IsPassiveSpell(m_spellInfo->Id) && !m_CastItem)
            {
                for(int i=0;i<3;++i)
                {
                    // check only spell that apply positive auras
                    if (IsPositiveEffect(m_spellInfo->Id, i) && m_spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
                        // at not self target
                        !IsCasterSourceTarget(m_spellInfo->EffectImplicitTargetA[i]) &&
                        // and target low level
                        target->getLevel() + 10 < m_spellInfo->spellLevel)
                    {
                        return SPELL_FAILED_LOWLEVEL;
                    }
                }
            }
        }

        // check pet presents
        for(int j = 0; j < 3; ++j)
        {
            if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_PET)
            {
                target = m_caster->GetPet();
                if(!target)
                {
                    if(m_triggeredByAuraSpell)              // not report pet not existence for triggered spells
                        return SPELL_FAILED_DONT_REPORT;
                    else
                        return SPELL_FAILED_NO_PET;
                }
                break;
            }
        }

        //check creature type
        //ignore self casts (including area casts when caster selected as target)
        if(non_caster_target)
        {
            if(!CheckTargetCreatureType(target))
            {
                if(target->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_TARGET_IS_PLAYER;
                else
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        // TODO: this check can be applied and for player to prevent cheating when IsPositiveSpell will return always correct result.
        // check target for pet/charmed casts (not self targeted), self targeted cast used for area effects and etc
        if(non_caster_target && m_caster->GetTypeId() == TYPEID_UNIT && m_caster->GetCharmerOrOwnerGUID())
        {
            // check correctness positive/negative cast target (pet cast real check and cheating check)
            if(IsPositiveSpell(m_spellInfo->Id))
            {
                if(m_caster->IsHostileTo(target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else
            {
                if(m_caster->IsFriendlyTo(target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        if(IsPositiveSpell(m_spellInfo->Id))
            if(target->IsImmunedToSpell(m_spellInfo,false))
                return SPELL_FAILED_TARGET_AURASTATE;

        //Must be behind the target.
        if( m_spellInfo->AttributesEx2 == 0x100000 && (m_spellInfo->AttributesEx & 0x200) == 0x200 && target->HasInArc(M_PI, m_caster) )
        {
            //Exclusion for Pounce: Facing Limitation was removed in 2.0.1, but it still uses the same, old Ex-Flags
            if (m_spellInfo->SpellFamilyName != SPELLFAMILY_DRUID || (m_spellInfo->SpellFamilyFlags != UI64LIT(0x0000000000020000)))
            {
                SendInterrupted(2);
                return SPELL_FAILED_NOT_BEHIND;
            }
        }

        //Target must be facing you.
        if((m_spellInfo->Attributes == 0x150010) && !target->HasInArc(M_PI, m_caster) )
        {
            SendInterrupted(2);
            return SPELL_FAILED_NOT_INFRONT;
        }

        // check if target is in combat
        if (non_caster_target && (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_IN_COMBAT_TARGET) && target->isInCombat())
            return SPELL_FAILED_TARGET_AFFECTING_COMBAT;
    }
    // zone check
    uint32 zone, area;
    m_caster->GetZoneAndAreaId(zone, area);

    SpellCastResult locRes= sSpellMgr.GetSpellAllowedInLocationError(m_spellInfo,m_caster->GetMapId(),zone,area,
        m_caster->GetCharmerOrOwnerPlayerOrPlayerItself());
    if (locRes != SPELL_CAST_OK)
        return locRes;

    // not let players cast spells at mount (and let do it to creatures)
    if (m_caster->IsMounted() && m_caster->GetTypeId()==TYPEID_PLAYER && !m_IsTriggeredSpell &&
        !IsPassiveSpell(m_spellInfo->Id) && !(m_spellInfo->Attributes & SPELL_ATTR_CASTABLE_WHILE_MOUNTED))
    {
        if (m_caster->isInFlight())
            return SPELL_FAILED_NOT_ON_TAXI;
        else
            return SPELL_FAILED_NOT_MOUNTED;
    }

    // always (except passive spells) check items (focus object can be required for any type casts)
    if (!IsPassiveSpell(m_spellInfo->Id))
    {
        SpellCastResult castResult = CheckItems();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    //ImpliciteTargetA-B = 38, If fact there is 0 Spell with  ImpliciteTargetB=38
    if (m_UniqueTargetInfo.empty())                         // skip second CheckCast apply (for delayed spells for example)
    {
        for(uint8 j = 0; j < 3; ++j)
        {
            if( m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
                m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[j] != TARGET_SELF ||
                m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES )
            {
                SpellScriptTargetBounds bounds = sSpellMgr.GetSpellScriptTargetBounds(m_spellInfo->Id);
                if(bounds.first==bounds.second)
                    sLog.outErrorDb("Spell (ID: %u) has effect EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT or TARGET_SCRIPT_COORDINATES, but does not have record in `spell_script_target`",m_spellInfo->Id);

                SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
                float range = GetSpellMaxRange(srange);

                Creature* creatureScriptTarget = NULL;
                GameObject* goScriptTarget = NULL;

                for(SpellScriptTarget::const_iterator i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                {
                    switch(i_spellST->second.type)
                    {
                        case SPELL_TARGET_TYPE_GAMEOBJECT:
                        {
                            GameObject* p_GameObject = NULL;

                            if (i_spellST->second.targetEntry)
                            {
                                CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
                                Cell cell(p);
                                cell.data.Part.reserved = ALL_DISTRICT;

                                MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster,i_spellST->second.targetEntry,range);
                                MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(p_GameObject,go_check);

                                TypeContainerVisitor<MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck>, GridTypeMapContainer > object_checker(checker);
                                CellLock<GridReadGuard> cell_lock(cell, p);
                                cell_lock->Visit(cell_lock, object_checker, *m_caster->GetMap(), *m_caster, range);

                                if (p_GameObject)
                                {
                                    // remember found target and range, next attempt will find more near target with another entry
                                    creatureScriptTarget = NULL;
                                    goScriptTarget = p_GameObject;
                                    range = go_check.GetLastRange();
                                }
                            }
                            else if (focusObject)           // Focus Object
                            {
                                float frange = m_caster->GetDistance(focusObject);
                                if (range >= frange)
                                {
                                    creatureScriptTarget = NULL;
                                    goScriptTarget = focusObject;
                                    range = frange;
                                }
                            }
                            break;
                        }
                        case SPELL_TARGET_TYPE_CREATURE:
                        case SPELL_TARGET_TYPE_DEAD:
                        default:
                        {
                            Creature *p_Creature = NULL;

                            CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
                            Cell cell(p);
                            cell.data.Part.reserved = ALL_DISTRICT;
                            cell.SetNoCreate();             // Really don't know what is that???

                            MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*m_caster,i_spellST->second.targetEntry,i_spellST->second.type!=SPELL_TARGET_TYPE_DEAD,range);
                            MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(p_Creature, u_check);

                            TypeContainerVisitor<MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck>, GridTypeMapContainer >  grid_creature_searcher(searcher);

                            CellLock<GridReadGuard> cell_lock(cell, p);
                            cell_lock->Visit(cell_lock, grid_creature_searcher, *m_caster->GetMap(), *m_caster, range);

                            if(p_Creature )
                            {
                                creatureScriptTarget = p_Creature;
                                goScriptTarget = NULL;
                                range = u_check.GetLastRange();
                            }
                            break;
                        }
                    }
                }

                if(creatureScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES )
                    {
                        m_targets.setDestination(creatureScriptTarget->GetPositionX(),creatureScriptTarget->GetPositionY(),creatureScriptTarget->GetPositionZ());

                        if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->EffectImplicitTargetB[j] == 0 && m_spellInfo->Effect[j]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddUnitTarget(creatureScriptTarget, j);
                    }
                    // store explicit target for TARGET_SCRIPT
                    else
                        AddUnitTarget(creatureScriptTarget, j);
                }
                else if(goScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES )
                    {
                        m_targets.setDestination(goScriptTarget->GetPositionX(),goScriptTarget->GetPositionY(),goScriptTarget->GetPositionZ());

                        if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->EffectImplicitTargetB[j] == 0 && m_spellInfo->Effect[j]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddGOTarget(goScriptTarget, j);
                    }
                    // store explicit target for TARGET_SCRIPT
                    else
                        AddGOTarget(goScriptTarget, j);
                }
                //Missing DB Entry or targets for this spellEffect.
                else
                {
                    // not report target not existence for triggered spells
                    if(m_triggeredByAuraSpell || m_IsTriggeredSpell)
                        return SPELL_FAILED_DONT_REPORT;
                    else
                        return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }
    }

    if(!m_IsTriggeredSpell)
    {
        SpellCastResult castResult = CheckRange(strict);
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    {
        SpellCastResult castResult = CheckPower();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    if(!m_IsTriggeredSpell)                                 // triggered spell not affected by stun/etc
    {
        SpellCastResult castResult = CheckCasterAuras();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    for (int i = 0; i < 3; ++i)
    {
        // for effects of spells that have only one target
        switch(m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_DUMMY:
            {
                if(m_spellInfo->SpellIconID == 1648)        // Execute
                {
                    if(!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else if (m_spellInfo->Id == 51582)          // Rocket Boots Engaged
                {
                    if(m_caster->IsInWater())
                        return SPELL_FAILED_ONLY_ABOVEWATER;
                }
                else if(m_spellInfo->SpellIconID == 156)    // Holy Shock
                {
                    // spell different for friends and enemies
                    // hart version required facing
                    if(m_targets.getUnitTarget() && !m_caster->IsFriendlyTo(m_targets.getUnitTarget()) && !m_caster->HasInArc( M_PI, m_targets.getUnitTarget() ))
                        return SPELL_FAILED_UNIT_NOT_INFRONT;
                }
                break;
            }
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            {
                // Hammer of Wrath
                if(m_spellInfo->SpellVisual == 7250)
                {
                    if (!m_targets.getUnitTarget())
                        return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                    if(m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                break;
            }
            case SPELL_EFFECT_TAMECREATURE:
            {
                // Spell can be triggered, we need to check original caster prior to caster
                Unit* caster = m_originalCaster ? m_originalCaster : m_caster;

                if (caster->GetTypeId() != TYPEID_PLAYER ||
                    !m_targets.getUnitTarget() ||
                    m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                Player* plrCaster = (Player*)caster;

                if(plrCaster->getClass() != CLASS_HUNTER)
                {
                    plrCaster->SendPetTameFailure(PETTAME_UNITSCANTTAME);
                    return SPELL_FAILED_DONT_REPORT;
                }

                Creature* target = (Creature*)m_targets.getUnitTarget();

                if(target->isPet() || target->isCharmed())
                {
                    plrCaster->SendPetTameFailure(PETTAME_CREATUREALREADYOWNED);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (target->getLevel() > plrCaster->getLevel())
                {
                    plrCaster->SendPetTameFailure(PETTAME_TOOHIGHLEVEL);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (!target->GetCreatureInfo()->isTameable())
                {
                    plrCaster->SendPetTameFailure(PETTAME_NOTTAMEABLE);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if(plrCaster->GetPetGUID() || plrCaster->GetCharmGUID())
                {
                    plrCaster->SendPetTameFailure(PETTAME_ANOTHERSUMMONACTIVE);
                    return SPELL_FAILED_DONT_REPORT;
                }

                break;
            }
            case SPELL_EFFECT_LEARN_SPELL:
            {
                if(m_spellInfo->EffectImplicitTargetA[i] != TARGET_PET)
                    break;

                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

                if(!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if(!pet->CanTakeMoreActiveSpells(learn_spellproto->Id))
                    return SPELL_FAILED_TOO_MANY_SKILLS;

                if(m_spellInfo->spellLevel > pet->getLevel())
                    return SPELL_FAILED_LOWLEVEL;

                if(!pet->HasTPForSpell(learn_spellproto->Id))
                    return SPELL_FAILED_TRAINING_POINTS;

                break;
            }
            case SPELL_EFFECT_LEARN_PET_SPELL:
            {
                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

                if(!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if(!pet->CanTakeMoreActiveSpells(learn_spellproto->Id))
                    return SPELL_FAILED_TOO_MANY_SKILLS;

                if(m_spellInfo->spellLevel > pet->getLevel())
                    return SPELL_FAILED_LOWLEVEL;

                if(!pet->HasTPForSpell(learn_spellproto->Id))
                    return SPELL_FAILED_TRAINING_POINTS;

                break;
            }
            case SPELL_EFFECT_FEED_PET:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                Item* foodItem = m_targets.getItemTarget();
                if(!foodItem)
                    return SPELL_FAILED_BAD_TARGETS;

                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if(!pet->HaveInDiet(foodItem->GetProto()))
                    return SPELL_FAILED_WRONG_PET_FOOD;

                if(!pet->GetCurrentFoodBenefitLevel(foodItem->GetProto()->ItemLevel))
                    return SPELL_FAILED_FOOD_LOWLEVEL;

                if(m_caster->isInCombat() || pet->isInCombat())
                    return SPELL_FAILED_AFFECTING_COMBAT;

                break;
            }
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_POWER_DRAIN:
            {
                // Can be area effect, Check only for players and not check if target - caster (spell can have multiply drain/burn effects)
                if(m_caster->GetTypeId() == TYPEID_PLAYER)
                    if(Unit* target = m_targets.getUnitTarget())
                        if(target != m_caster && target->getPowerType() != m_spellInfo->EffectMiscValue[i])
                            return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            case SPELL_EFFECT_CHARGE:
            {
                if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                    return SPELL_FAILED_ROOTED;

                break;
            }
            case SPELL_EFFECT_SKINNING:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || !m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() != TYPEID_UNIT)
                    return SPELL_FAILED_BAD_TARGETS;

                if( !(m_targets.getUnitTarget()->GetUInt32Value(UNIT_FIELD_FLAGS) & UNIT_FLAG_SKINNABLE) )
                    return SPELL_FAILED_TARGET_UNSKINNABLE;

                Creature* creature = (Creature*)m_targets.getUnitTarget();
                if ( creature->GetCreatureType() != CREATURE_TYPE_CRITTER && ( !creature->lootForBody || !creature->loot.empty() ) )
                {
                    return SPELL_FAILED_TARGET_NOT_LOOTED;
                }

                uint32 skill = creature->GetCreatureInfo()->GetRequiredLootSkill();

                int32 skillValue = ((Player*)m_caster)->GetSkillValue(skill);
                int32 TargetLevel = m_targets.getUnitTarget()->getLevel();
                int32 ReqValue = (skillValue < 100 ? (TargetLevel-10) * 10 : TargetLevel * 5);
                if (ReqValue > skillValue)
                    return SPELL_FAILED_LOW_CASTLEVEL;

                // chance for fail at orange skinning attempt
                if( (m_selfContainer && (*m_selfContainer) == this) &&
                    skillValue < sWorld.GetConfigMaxSkillValue() &&
                    (ReqValue < 0 ? 0 : ReqValue) > irand(skillValue - 25, skillValue + 37) )
                    return SPELL_FAILED_TRY_AGAIN;

                break;
            }
            case SPELL_EFFECT_OPEN_LOCK_ITEM:
            case SPELL_EFFECT_OPEN_LOCK:
            {
                if( m_spellInfo->EffectImplicitTargetA[i] != TARGET_GAMEOBJECT &&
                    m_spellInfo->EffectImplicitTargetA[i] != TARGET_GAMEOBJECT_ITEM )
                    break;

                if( m_caster->GetTypeId() != TYPEID_PLAYER  // only players can open locks, gather etc.
                    // we need a go target in case of TARGET_GAMEOBJECT
                    || m_spellInfo->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT && !m_targets.getGOTarget()
                    // we need a go target, or an openable item target in case of TARGET_GAMEOBJECT_ITEM
                    || m_spellInfo->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT_ITEM && !m_targets.getGOTarget() &&
                    (!m_targets.getItemTarget() || !m_targets.getItemTarget()->GetProto()->LockID || m_targets.getItemTarget()->GetOwner() != m_caster ) )
                    return SPELL_FAILED_BAD_TARGETS;

                // In BattleGround players can use only flags and banners
                if( ((Player*)m_caster)->InBattleGround() &&
                    !((Player*)m_caster)->CanUseBattleGroundObject() )
                    return SPELL_FAILED_TRY_AGAIN;

                // get the lock entry
                uint32 lockId = 0;
                if (GameObject* go = m_targets.getGOTarget())
                {
                    lockId = go->GetGOInfo()->GetLockId();
                    if (!lockId)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else if(Item* itm = m_targets.getItemTarget())
                    lockId = itm->GetProto()->LockID;

                SkillType skillId = SKILL_NONE;
                int32 reqSkillValue = 0;
                int32 skillValue = 0;

                // check lock compatibility
                SpellCastResult res = CanOpenLock(i, lockId, skillId, reqSkillValue, skillValue);
                if(res != SPELL_CAST_OK)
                    return res;

                // chance for fail at orange mining/herb/LockPicking gathering attempt
                // second check prevent fail at rechecks
                if(skillId != SKILL_NONE && (!m_selfContainer || ((*m_selfContainer) != this)))
                {
                    bool canFailAtMax = skillId != SKILL_HERBALISM && skillId != SKILL_MINING;

                    // chance for failure in orange gather / lockpick (gathering skill can't fail at maxskill)
                    if((canFailAtMax || skillValue < sWorld.GetConfigMaxSkillValue()) && reqSkillValue > irand(skillValue - 25, skillValue + 37))
                        return SPELL_FAILED_TRY_AGAIN;
                }
                break;
            }
            case SPELL_EFFECT_SUMMON_DEAD_PET:
            {
                Creature *pet = m_caster->GetPet();
                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if(pet->isAlive())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                break;
            }
            // This is generic summon effect now and don't make this check for summon types similar
            // SPELL_EFFECT_SUMMON_CRITTER, SPELL_EFFECT_SUMMON_WILD or SPELL_EFFECT_SUMMON_GUARDIAN.
            // These won't show up in m_caster->GetPetGUID()
            case SPELL_EFFECT_SUMMON:
            {
                switch(m_spellInfo->EffectMiscValueB[i])
                {
                    case SUMMON_TYPE_POSESSED:
                    case SUMMON_TYPE_POSESSED2:
                    case SUMMON_TYPE_DEMON:
                    case SUMMON_TYPE_SUMMON:
                    {
                        if(m_caster->GetPetGUID())
                            return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                        if(m_caster->GetCharmGUID())
                            return SPELL_FAILED_ALREADY_HAVE_CHARM;
                        break;
                    }
                }
                break;
            }
            // Don't make this check for SPELL_EFFECT_SUMMON_CRITTER, SPELL_EFFECT_SUMMON_WILD or SPELL_EFFECT_SUMMON_GUARDIAN.
            // These won't show up in m_caster->GetPetGUID()
            case SPELL_EFFECT_SUMMON_POSSESSED:
            case SPELL_EFFECT_SUMMON_PHANTASM:
            case SPELL_EFFECT_SUMMON_DEMON:
            {
                if(m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_WILD:
            {
                // fire bomb trigger, can only be used in halaa opvp when flying on a path from a wyvern roost
                // yeah, hacky, I know, but neither item flags, nor spell attributes contained any useable data (or I was unable to find it)
                if(m_spellInfo->EffectMiscValue[i] == 18225 && m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    // if not in halaa or not in flight, cannot be used
                    if(m_caster->GetAreaId() != 3628 || !m_caster->isInFlight())
                        return SPELL_FAILED_NOT_HERE;

                    // if not on one of the specific taxi paths, then cannot be used
                    uint32 src_node = ((Player*)m_caster)->m_taxi.GetTaxiSource();
                    if( src_node != 103 &&
                        src_node != 105 &&
                        src_node != 107 &&
                        src_node != 109 )
                        return SPELL_FAILED_NOT_HERE;
                }
                break;
            }
            case SPELL_EFFECT_SUMMON_PET:
            {
                if(m_caster->GetPetGUID())                  //let warlock do a replacement summon
                {

                    Pet* pet = ((Player*)m_caster)->GetPet();

                    if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->getClass() == CLASS_WARLOCK)
                    {
                        if (strict)                         //starting cast, trigger pet stun (cast by pet so it doesn't attack player)
                            pet->CastSpell(pet, 32752, true, NULL, NULL, pet->GetGUID());
                    }
                    else
                        return SPELL_FAILED_ALREADY_HAVE_SUMMON;
                }

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_PLAYER:
            {
                if(m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;
                if(!((Player*)m_caster)->GetSelection())
                    return SPELL_FAILED_BAD_TARGETS;

                Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelection());
                if( !target || ((Player*)m_caster) == target || !target->IsInSameRaidWith((Player*)m_caster) )
                    return SPELL_FAILED_BAD_TARGETS;

                // check if our map is dungeon
                if( sMapStore.LookupEntry(m_caster->GetMapId())->IsDungeon() )
                {
                    InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(m_caster->GetMapId());
                    if(!instance)
                        return SPELL_FAILED_TARGET_NOT_IN_INSTANCE;
                    if ( instance->levelMin > target->getLevel() )
                        return SPELL_FAILED_LOWLEVEL;
                    if ( instance->levelMax && instance->levelMax < target->getLevel() )
                        return SPELL_FAILED_HIGHLEVEL;
                }
                break;
            }
            case SPELL_EFFECT_LEAP:
            case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
            {
                // not allow use this effect at battleground until battleground start
                if(m_caster->GetTypeId() == TYPEID_PLAYER)
                    if(BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                        if(bg->GetStatus() != STATUS_IN_PROGRESS)
                            return SPELL_FAILED_TRY_AGAIN;
                break;
            }
            case SPELL_EFFECT_STEAL_BENEFICIAL_BUFF:
            {
                if (m_targets.getUnitTarget() == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            default:break;
        }
    }

    for (int i = 0; i < 3; ++i)
    {
        switch(m_spellInfo->EffectApplyAuraName[i])
        {
            case SPELL_AURA_MOD_POSSESS:
            {
                if(m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_UNKNOWN;

                if(m_targets.getUnitTarget() == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;

                if(m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if(m_caster->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if(!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if(m_targets.getUnitTarget()->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if(int32(m_targets.getUnitTarget()->getLevel()) > CalculateDamage(i,m_targets.getUnitTarget()))
                    return SPELL_FAILED_HIGHLEVEL;

                break;
            }
            case SPELL_AURA_MOD_CHARM:
            {
                if(m_targets.getUnitTarget() == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;

                if(m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if(m_caster->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if(!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if(m_targets.getUnitTarget()->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if(int32(m_targets.getUnitTarget()->getLevel()) > CalculateDamage(i,m_targets.getUnitTarget()))
                    return SPELL_FAILED_HIGHLEVEL;

                break;
            }
            case SPELL_AURA_MOD_POSSESS_PET:
            {
                if(m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_UNKNOWN;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if(m_caster->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                Pet* pet = m_caster->GetPet();
                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if(pet->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                break;
            }
            case SPELL_AURA_MOUNTED:
            {
                if (m_caster->IsInWater())
                    return SPELL_FAILED_ONLY_ABOVEWATER;

                if (m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->GetTransport())
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                // Ignore map check if spell have AreaId. AreaId already checked and this prevent special mount spells
                if (m_caster->GetTypeId()==TYPEID_PLAYER && !sMapStore.LookupEntry(m_caster->GetMapId())->IsMountAllowed() && !m_IsTriggeredSpell && !m_spellInfo->AreaId)
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                if (m_caster->GetAreaId()==35)
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                ShapeshiftForm form = m_caster->m_form;
                if( form == FORM_CAT          || form == FORM_TREE      || form == FORM_TRAVEL   ||
                    form == FORM_AQUA         || form == FORM_BEAR      || form == FORM_DIREBEAR ||
                    form == FORM_CREATUREBEAR || form == FORM_GHOSTWOLF || form == FORM_FLIGHT   ||
                    form == FORM_FLIGHT_EPIC  || form == FORM_MOONKIN )
                    return SPELL_FAILED_NOT_SHAPESHIFT;

                break;
            }
            case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
            {
                if(!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                // can be casted at non-friendly unit or own pet/charm
                if(m_caster->IsFriendlyTo(m_targets.getUnitTarget()))
                    return SPELL_FAILED_TARGET_FRIENDLY;

                break;
            }
            case SPELL_AURA_PERIODIC_MANA_LEECH:
            {
                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (m_caster->GetTypeId() != TYPEID_PLAYER || m_CastItem)
                    break;

                if(m_targets.getUnitTarget()->getPowerType() != POWER_MANA)
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            default:
                break;
        }
    }

    // all ok
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckPetCast(Unit* target)
{
    if(!m_caster->isAlive())
        return SPELL_FAILED_CASTER_DEAD;

    if(m_caster->IsNonMeleeSpellCasted(false))              //prevent spellcast interruption by another spellcast
        return SPELL_FAILED_SPELL_IN_PROGRESS;
    if(m_caster->isInCombat() && IsNonCombatSpell(m_spellInfo))
        return SPELL_FAILED_AFFECTING_COMBAT;

    if(m_caster->GetTypeId()==TYPEID_UNIT && (((Creature*)m_caster)->isPet() || m_caster->isCharmed()))
    {
                                                            //dead owner (pets still alive when owners ressed?)
        if(m_caster->GetCharmerOrOwner() && !m_caster->GetCharmerOrOwner()->isAlive())
            return SPELL_FAILED_CASTER_DEAD;

        if(!target && m_targets.getUnitTarget())
            target = m_targets.getUnitTarget();

        bool need = false;
        for(uint32 i = 0; i < 3; ++i)
        {
            if(m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_DAMAGE || m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_FRIEND || m_spellInfo->EffectImplicitTargetA[i] == TARGET_DUELVSPLAYER || m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_PARTY || m_spellInfo->EffectImplicitTargetA[i] == TARGET_CURRENT_ENEMY_COORDINATES)
            {
                need = true;
                if(!target)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;
                break;
            }
        }
        if(need)
            m_targets.setUnitTarget(target);

        Unit* _target = m_targets.getUnitTarget();

        if(_target)                                         //for target dead/target not valid
        {
            if(!_target->isAlive())
                return SPELL_FAILED_BAD_TARGETS;

            if(IsPositiveSpell(m_spellInfo->Id))
            {
                if(m_caster->IsHostileTo(_target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else
            {
                bool duelvsplayertar = false;
                for(int j = 0; j < 3; ++j)
                {
                                                            //TARGET_DUELVSPLAYER is positive AND negative
                    duelvsplayertar |= (m_spellInfo->EffectImplicitTargetA[j] == TARGET_DUELVSPLAYER);
                }
                if(m_caster->IsFriendlyTo(target) && !duelvsplayertar)
                {
                    return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }
                                                            //cooldown
        if(((Creature*)m_caster)->HasSpellCooldown(m_spellInfo->Id))
            return SPELL_FAILED_NOT_READY;
    }

    return CheckCast(true);
}

SpellCastResult Spell::CheckCasterAuras() const
{
    // Flag drop spells totally immuned to caster auras
    // FIXME: find more nice check for all totally immuned spells
    switch(m_spellInfo->Id)
    {
        case 23336:     // Alliance Flag Drop
        case 23334:     // Horde Flag Drop
        case 34991:     // Summon Netherstorm Flag
        case 22812:     // Barkskin
            return SPELL_CAST_OK;
        default:
            break;
    }

    uint8 school_immune = 0;
    uint32 mechanic_immune = 0;
    uint32 dispel_immune = 0;

    // Check if the spell grants school or mechanic immunity.
    // We use bitmasks so the loop is done only once and not on every aura check below.
    if ( m_spellInfo->AttributesEx & SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY )
    {
        for(int i = 0; i < 3; ++i)
        {
            if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_IMMUNITY)
                school_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY)
                mechanic_immune |= 1 << uint32(m_spellInfo->EffectMiscValue[i]-1);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY_MASK)
                mechanic_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_DISPEL_IMMUNITY)
                dispel_immune |= GetDispellMask(DispelType(m_spellInfo->EffectMiscValue[i]));
        }
        // immune movement impairment and loss of control
        if (m_spellInfo->Id == 42292)
            mechanic_immune = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK;
    }

    // Check whether the cast should be prevented by any state you might have.
    SpellCastResult prevented_reason = SPELL_CAST_OK;
    // Have to check if there is a stun aura. Otherwise will have problems with ghost aura apply while logging out
    uint32 unitflag = m_caster->GetUInt32Value(UNIT_FIELD_FLAGS);     // Get unit state
    if (unitflag & UNIT_FLAG_STUNNED && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
        prevented_reason = SPELL_FAILED_STUNNED;
    else if (unitflag & UNIT_FLAG_CONFUSED && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
        prevented_reason = SPELL_FAILED_CONFUSED;
    else if (unitflag & UNIT_FLAG_FLEEING && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
        prevented_reason = SPELL_FAILED_FLEEING;
    else if (unitflag & UNIT_FLAG_SILENCED && m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        prevented_reason = SPELL_FAILED_SILENCED;
    else if (unitflag & UNIT_FLAG_PACIFIED && m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
        prevented_reason = SPELL_FAILED_PACIFIED;

    // Attr must make flag drop spell totally immune from all effects
    if (prevented_reason != SPELL_CAST_OK)
    {
        if (school_immune || mechanic_immune || dispel_immune)
        {
            //Checking auras is needed now, because you are prevented by some state but the spell grants immunity.
            Unit::AuraMap const& auras = m_caster->GetAuras();
            for(Unit::AuraMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
            {
                if (itr->second)
                {
                    if (GetSpellMechanicMask(itr->second->GetSpellProto(), itr->second->GetEffIndex()) & mechanic_immune)
                        continue;
                    if (GetSpellSchoolMask(itr->second->GetSpellProto()) & school_immune)
                        continue;
                    if ((1<<(itr->second->GetSpellProto()->Dispel)) & dispel_immune)
                        continue;

                    // Make a second check for spell failed so the right SPELL_FAILED message is returned.
                    // That is needed when your casting is prevented by multiple states and you are only immune to some of them.
                    switch(itr->second->GetModifier()->m_auraname)
                    {
                        case SPELL_AURA_MOD_STUN:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
                                return SPELL_FAILED_STUNNED;
                            break;
                        case SPELL_AURA_MOD_CONFUSE:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
                                return SPELL_FAILED_CONFUSED;
                            break;
                        case SPELL_AURA_MOD_FEAR:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
                                return SPELL_FAILED_FLEEING;
                            break;
                        case SPELL_AURA_MOD_SILENCE:
                        case SPELL_AURA_MOD_PACIFY:
                        case SPELL_AURA_MOD_PACIFY_SILENCE:
                            if( m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
                                return SPELL_FAILED_PACIFIED;
                            else if ( m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                                return SPELL_FAILED_SILENCED;
                            break;
                    }
                }
            }
        }
        // You are prevented from casting and the spell casted does not grant immunity. Return a failed error.
        else
            return prevented_reason;
    }
    return SPELL_CAST_OK;
}

bool Spell::CanAutoCast(Unit* target)
{
    uint64 targetguid = target->GetGUID();

    for(uint32 j = 0; j < 3; ++j)
    {
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_APPLY_AURA)
        {
            if( m_spellInfo->StackAmount <= 1)
            {
                if( target->HasAura(m_spellInfo->Id, j) )
                    return false;
            }
            else
            {
                if( target->GetAuras().count(Unit::spellEffectPair(m_spellInfo->Id, j)) >= m_spellInfo->StackAmount)
                    return false;
            }
        }
        else if ( IsAreaAuraEffect( m_spellInfo->Effect[j] ))
        {
                if( target->HasAura(m_spellInfo->Id, j) )
                    return false;
        }
    }

    SpellCastResult result = CheckPetCast(target);

    if(result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
    {
        FillTargetMap();
        //check if among target units, our WANTED target is as well (->only self cast spells return false)
        for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            if( ihit->targetGUID == targetguid )
                return true;
    }
    return false;                                           //target invalid
}

SpellCastResult Spell::CheckRange(bool strict)
{
    float range_mod;

    // self cast doesn't need range checking -- also for Starshards fix
    if (m_spellInfo->rangeIndex == 1)
        return SPELL_CAST_OK;

    if (strict)                                             //add radius of caster
        range_mod = 1.25;
    else                                                    //add radius of caster and ~5 yds "give"
        range_mod = 6.25;

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
    float max_range = GetSpellMaxRange(srange) + range_mod;
    float min_range = GetSpellMinRange(srange);

    if(Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RANGE, max_range, this);

    Unit *target = m_targets.getUnitTarget();

    if(target && target != m_caster)
    {
        // distance from target in checks
        float dist = m_caster->GetCombatDistance(target);

        if(dist > max_range)
            return SPELL_FAILED_OUT_OF_RANGE;               //0x5A;
        if(min_range && dist < min_range)
            return SPELL_FAILED_TOO_CLOSE;
        if( m_caster->GetTypeId() == TYPEID_PLAYER &&
            (m_spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc( M_PI, target ) )
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }

    if(m_targets.m_targetMask == TARGET_FLAG_DEST_LOCATION && m_targets.m_destX != 0 && m_targets.m_destY != 0 && m_targets.m_destZ != 0)
    {
        if(!m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, max_range))
            return SPELL_FAILED_OUT_OF_RANGE;
        if(min_range && m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, min_range))
            return SPELL_FAILED_TOO_CLOSE;
    }

    return SPELL_CAST_OK;
}

int32 Spell::CalculatePowerCost()
{
    // item cast not used power
    if(m_CastItem)
        return 0;

    // Spell drain all exist power on cast (Only paladin lay of Hands)
    if (m_spellInfo->AttributesEx & SPELL_ATTR_EX_DRAIN_ALL_POWER)
    {
        // If power type - health drain all
        if (m_spellInfo->powerType == POWER_HEALTH)
            return m_caster->GetHealth();
        // Else drain all power
        if (m_spellInfo->powerType < MAX_POWERS)
            return m_caster->GetPower(Powers(m_spellInfo->powerType));
        sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", m_spellInfo->powerType, m_spellInfo->Id);
        return 0;
    }

    // Base powerCost
    int32 powerCost = m_spellInfo->manaCost;
    // PCT cost from total amount
    if (m_spellInfo->ManaCostPercentage)
    {
        switch (m_spellInfo->powerType)
        {
            // health as power used
            case POWER_HEALTH:
                powerCost += m_spellInfo->ManaCostPercentage * m_caster->GetCreateHealth() / 100;
                break;
            case POWER_MANA:
                powerCost += m_spellInfo->ManaCostPercentage * m_caster->GetCreateMana() / 100;
                break;
            case POWER_RAGE:
            case POWER_FOCUS:
            case POWER_ENERGY:
            case POWER_HAPPINESS:
                //            case POWER_RUNES:
                powerCost += m_spellInfo->ManaCostPercentage * m_caster->GetMaxPower(Powers(m_spellInfo->powerType)) / 100;
                break;
            default:
                sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", m_spellInfo->powerType, m_spellInfo->Id);
                return 0;
        }
    }
    SpellSchools school = GetFirstSchoolInMask(m_spellSchoolMask);
    // Flat mod from caster auras by spell school
    powerCost += m_caster->GetInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + school);
    // Shiv - costs 20 + weaponSpeed*10 energy (apply only to non-triggered spell with energy cost)
    if ( m_spellInfo->AttributesEx4 & SPELL_ATTR_EX4_SPELL_VS_EXTEND_COST )
        powerCost += m_caster->GetAttackTime(OFF_ATTACK)/100;
    // Apply cost mod by spell
    if(Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_COST, powerCost, this);

    if(m_spellInfo->Attributes & SPELL_ATTR_LEVEL_DAMAGE_CALCULATION)
        powerCost = int32(powerCost/ (1.117f* m_spellInfo->spellLevel / m_caster->getLevel() -0.1327f));

    // PCT mod from user auras by school
    powerCost = int32(powerCost * (1.0f+m_caster->GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER+school)));
    if (powerCost < 0)
        powerCost = 0;
    return powerCost;
}

SpellCastResult Spell::CheckPower()
{
    // item cast not used power
    if(m_CastItem)
        return SPELL_CAST_OK;

    // health as power used - need check health amount
    if(m_spellInfo->powerType == POWER_HEALTH)
    {
        if(m_caster->GetHealth() <= m_powerCost)
            return SPELL_FAILED_CASTER_AURASTATE;
        return SPELL_CAST_OK;
    }

    // Check valid power type
    if( m_spellInfo->powerType >= MAX_POWERS )
    {
        sLog.outError("Spell::CheckMana: Unknown power type '%d'", m_spellInfo->powerType);
        return SPELL_FAILED_UNKNOWN;
    }

    // Check power amount
    Powers powerType = Powers(m_spellInfo->powerType);
    if(m_caster->GetPower(powerType) < m_powerCost)
        return SPELL_FAILED_NO_POWER;
    else
        return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckItems()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player* p_caster = (Player*)m_caster;

    // cast item checks
    if(m_CastItem)
    {
        uint32 itemid = m_CastItem->GetEntry();
        if( !p_caster->HasItemCount(itemid, 1) )
            return SPELL_FAILED_ITEM_NOT_READY;

        ItemPrototype const *proto = m_CastItem->GetProto();
        if(!proto)
            return SPELL_FAILED_ITEM_NOT_READY;

        for (int i = 0; i < 5; ++i)
            if (proto->Spells[i].SpellCharges)
                if(m_CastItem->GetSpellCharges(i) == 0)
                    return SPELL_FAILED_NO_CHARGES_REMAIN;

        // consumable cast item checks
        if (proto->Class == ITEM_CLASS_CONSUMABLE && m_targets.getUnitTarget())
        {
            // such items should only fail if there is no suitable effect at all - see Rejuvenation Potions for example
            SpellCastResult failReason = SPELL_CAST_OK;
            for (int i = 0; i < 3; ++i)
            {
                // skip check, pet not required like checks, and for TARGET_PET m_targets.getUnitTarget() is not the real target but the caster
                if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_PET)
                    continue;

                if (m_spellInfo->Effect[i] == SPELL_EFFECT_HEAL)
                {
                    if (m_targets.getUnitTarget()->GetHealth() == m_targets.getUnitTarget()->GetMaxHealth())
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_HEALTH;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }

                // Mana Potion, Rage Potion, Thistle Tea(Rogue), ...
                if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENERGIZE)
                {
                    if(m_spellInfo->EffectMiscValue[i] < 0 || m_spellInfo->EffectMiscValue[i] >= MAX_POWERS)
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }

                    Powers power = Powers(m_spellInfo->EffectMiscValue[i]);
                    if (m_targets.getUnitTarget()->GetPower(power) == m_targets.getUnitTarget()->GetMaxPower(power))
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }
            }
            if (failReason != SPELL_CAST_OK)
                return failReason;
        }
    }

    // check target item
    if(m_targets.getItemTargetGUID())
    {
        if(m_caster->GetTypeId() != TYPEID_PLAYER)
            return SPELL_FAILED_BAD_TARGETS;

        if(!m_targets.getItemTarget())
            return SPELL_FAILED_ITEM_GONE;

        if(!m_targets.getItemTarget()->IsFitToSpellRequirements(m_spellInfo))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }
    // if not item target then required item must be equipped
    else
    {
        if(m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->HasItemFitToSpellReqirements(m_spellInfo))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }

    // check spell focus object
    if(m_spellInfo->RequiresSpellFocus)
    {
        CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
        Cell cell(p);
        cell.data.Part.reserved = ALL_DISTRICT;

        GameObject* ok = NULL;
        MaNGOS::GameObjectFocusCheck go_check(m_caster,m_spellInfo->RequiresSpellFocus);
        MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck> checker(ok,go_check);

        TypeContainerVisitor<MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck>, GridTypeMapContainer > object_checker(checker);
        CellLock<GridReadGuard> cell_lock(cell, p);
        Map& map = *m_caster->GetMap();
        cell_lock->Visit(cell_lock, object_checker, map, *m_caster, map.GetVisibilityDistance());

        if(!ok)
            return SPELL_FAILED_REQUIRES_SPELL_FOCUS;

        focusObject = ok;                                   // game object found in range
    }

    // check reagents (ignore triggered spells with reagents processed by original spell) and special reagent ignore case.
    if (!m_IsTriggeredSpell && !p_caster->CanNoReagentCast(m_spellInfo))
    {
        for(uint32 i = 0; i < 8; ++i)
        {
            if(m_spellInfo->Reagent[i] <= 0)
                continue;

            uint32 itemid    = m_spellInfo->Reagent[i];
            uint32 itemcount = m_spellInfo->ReagentCount[i];

            // if CastItem is also spell reagent
            if( m_CastItem && m_CastItem->GetEntry() == itemid )
            {
                ItemPrototype const *proto = m_CastItem->GetProto();
                if(!proto)
                    return SPELL_FAILED_ITEM_NOT_READY;
                for(int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }
            }
            if( !p_caster->HasItemCount(itemid, itemcount) )
                return SPELL_FAILED_ITEM_NOT_READY;         //0x54
        }
    }

    // check totem-item requirements (items presence in inventory)
    uint32 totems = 2;
    for(int i = 0; i < 2 ; ++i)
    {
        if(m_spellInfo->Totem[i] != 0)
        {
            if( p_caster->HasItemCount(m_spellInfo->Totem[i], 1) )
            {
                totems -= 1;
                continue;
            }
        }else
        totems -= 1;
    }
    if(totems != 0)
        return SPELL_FAILED_TOTEMS;                         //0x7C

    // Check items for TotemCategory  (items presence in inventory)
    uint32 TotemCategory = 2;
    for(int i= 0; i < 2; ++i)
    {
        if(m_spellInfo->TotemCategory[i] != 0)
        {
            if( p_caster->HasItemTotemCategory(m_spellInfo->TotemCategory[i]) )
            {
                TotemCategory -= 1;
                continue;
            }
        }
        else
            TotemCategory -= 1;
    }
    if(TotemCategory != 0)
        return SPELL_FAILED_TOTEM_CATEGORY;                 //0x7B

    // special checks for spell effects
    for(int i = 0; i < 3; ++i)
    {
        switch (m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_CREATE_ITEM:
            {
                if (!m_IsTriggeredSpell && m_spellInfo->EffectItemType[i])
                {
                    ItemPosCountVec dest;
                    uint8 msg = p_caster->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m_spellInfo->EffectItemType[i], 1 );
                    if (msg != EQUIP_ERR_OK )
                    {
                        p_caster->SendEquipError( msg, NULL, NULL );
                        return SPELL_FAILED_DONT_REPORT;
                    }
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM:
            {
                Item* targetItem = m_targets.getItemTarget();
                if(!targetItem)
                    return SPELL_FAILED_ITEM_NOT_FOUND;

                if( targetItem->GetProto()->ItemLevel < m_spellInfo->baseLevel )
                    return SPELL_FAILED_LOWLEVEL;
                // Not allow enchant in trade slot for some enchant type
                if( targetItem->GetOwner() != m_caster )
                {
                    uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                    SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                    if(!pEnchant)
                        return SPELL_FAILED_ERROR;
                    if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                        return SPELL_FAILED_NOT_TRADEABLE;
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            {
                Item *item = m_targets.getItemTarget();
                if(!item)
                    return SPELL_FAILED_ITEM_NOT_FOUND;
                // Not allow enchant in trade slot for some enchant type
                if( item->GetOwner() != m_caster )
                {
                    uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                    SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                    if(!pEnchant)
                        return SPELL_FAILED_ERROR;
                    if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                        return SPELL_FAILED_NOT_TRADEABLE;
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
                // check item existence in effect code (not output errors at offhand hold item effect to main hand for example
                break;
            case SPELL_EFFECT_DISENCHANT:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                // prevent disenchanting in trade slot
                if( m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID() )
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                ItemPrototype const* itemProto = m_targets.getItemTarget()->GetProto();
                if(!itemProto)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                uint32 item_quality = itemProto->Quality;
                // 2.0.x addon: Check player enchanting level against the item disenchanting requirements
                uint32 item_disenchantskilllevel = itemProto->RequiredDisenchantSkill;
                if (item_disenchantskilllevel == uint32(-1))
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (item_disenchantskilllevel > p_caster->GetSkillValue(SKILL_ENCHANTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                if(item_quality > 4 || item_quality < 2)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if(itemProto->Class != ITEM_CLASS_WEAPON && itemProto->Class != ITEM_CLASS_ARMOR)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (!itemProto->DisenchantID)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                break;
            }
            case SPELL_EFFECT_PROSPECTING:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //ensure item is a prospectable ore
                if(!(m_targets.getItemTarget()->GetProto()->BagFamily & BAG_FAMILY_MASK_MINING_SUPP) || m_targets.getItemTarget()->GetProto()->Class != ITEM_CLASS_TRADE_GOODS)
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //prevent prospecting in trade slot
                if( m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID() )
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //Check for enough skill in jewelcrafting
                uint32 item_prospectingskilllevel = m_targets.getItemTarget()->GetProto()->RequiredSkillRank;
                if(item_prospectingskilllevel >p_caster->GetSkillValue(SKILL_JEWELCRAFTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                //make sure the player has the required ores in inventory
                if(m_targets.getItemTarget()->GetCount() < 5)
                    return SPELL_FAILED_PROSPECT_NEED_MORE;

                if(!LootTemplates_Prospecting.HaveLootFor(m_targets.getItemTargetEntry()))
                    return SPELL_FAILED_CANT_BE_PROSPECTED;

                break;
            }
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            {
                if(m_caster->GetTypeId() != TYPEID_PLAYER) return SPELL_FAILED_TARGET_NOT_PLAYER;
                if( m_attackType != RANGED_ATTACK )
                    break;
                Item *pItem = ((Player*)m_caster)->GetWeaponForAttack(m_attackType);
                if(!pItem || pItem->IsBroken())
                    return SPELL_FAILED_EQUIPPED_ITEM;

                switch(pItem->GetProto()->SubClass)
                {
                    case ITEM_SUBCLASS_WEAPON_THROWN:
                    {
                        uint32 ammo = pItem->GetEntry();
                        if( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_GUN:
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                    {
                        uint32 ammo = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                        if(!ammo)
                        {
                            // Requires No Ammo
                            if(m_caster->GetDummyAura(46699))
                                break;                      // skip other checks

                            return SPELL_FAILED_NO_AMMO;
                        }

                        ItemPrototype const *ammoProto = ObjectMgr::GetItemPrototype( ammo );
                        if(!ammoProto)
                            return SPELL_FAILED_NO_AMMO;

                        if(ammoProto->Class != ITEM_CLASS_PROJECTILE)
                            return SPELL_FAILED_NO_AMMO;

                        // check ammo ws. weapon compatibility
                        switch(pItem->GetProto()->SubClass)
                        {
                            case ITEM_SUBCLASS_WEAPON_BOW:
                            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                                if(ammoProto->SubClass != ITEM_SUBCLASS_ARROW)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            case ITEM_SUBCLASS_WEAPON_GUN:
                                if(ammoProto->SubClass != ITEM_SUBCLASS_BULLET)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            default:
                                return SPELL_FAILED_NO_AMMO;
                        }

                        if( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_WAND:
                        break;
                    default:
                        break;
                }
                break;
            }
            default:break;
        }
    }

    return SPELL_CAST_OK;
}

void Spell::Delayed()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (m_spellState == SPELL_STATE_DELAYED)
        return;                                             // spell is active and can't be time-backed

    // spells not loosing casting time ( slam, dynamites, bombs.. )
    if(!(m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_DAMAGE))
        return;

    //check resist chance
    int32 resistChance = 100;                               //must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id,SPELLMOD_NOT_LOSE_CASTING_TIME,resistChance, this);
    resistChance += m_caster->GetTotalAuraModifier(SPELL_AURA_RESIST_PUSHBACK) - 100;
    if (roll_chance_i(resistChance))
        return;

    int32 delaytime = GetNextDelayAtDamageMsTime();

    if(int32(m_timer) + delaytime > m_casttime)
    {
        delaytime = m_casttime - m_timer;
        m_timer = m_casttime;
    }
    else
        m_timer += delaytime;

    sLog.outDetail("Spell %u partially interrupted for (%d) ms at damage", m_spellInfo->Id, delaytime);

    WorldPacket data(SMSG_SPELL_DELAYED, 8+4);
    data.append(m_caster->GetPackGUID());
    data << uint32(delaytime);

    m_caster->SendMessageToSet(&data, true);
}

void Spell::DelayedChannel()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER || getState() != SPELL_STATE_CASTING)
        return;

    //check resist chance
    int32 resistChance = 100;                               //must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id,SPELLMOD_NOT_LOSE_CASTING_TIME,resistChance, this);
    resistChance += m_caster->GetTotalAuraModifier(SPELL_AURA_RESIST_PUSHBACK) - 100;
    if (roll_chance_i(resistChance))
        return;

    int32 delaytime = GetNextDelayAtDamageMsTime();

    if(int32(m_timer) < delaytime)
    {
        delaytime = m_timer;
        m_timer = 0;
    }
    else
        m_timer -= delaytime;

    sLog.outDebug("Spell %u partially interrupted for %i ms, new duration: %u ms", m_spellInfo->Id, delaytime, m_timer);

    for(std::list<TargetInfo>::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if ((*ihit).missCondition == SPELL_MISS_NONE)
        {
            Unit* unit = m_caster->GetGUID() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
            if (unit)
            {
                for (int j = 0; j < 3; ++j)
                    if( ihit->effectMask & (1 << j) )
                        unit->DelayAura(m_spellInfo->Id, j, delaytime);
            }

        }
    }

    for(int j = 0; j < 3; ++j)
    {
        // partially interrupt persistent area auras
        DynamicObject* dynObj = m_caster->GetDynObject(m_spellInfo->Id, j);
        if(dynObj)
            dynObj->Delay(delaytime);
    }

    SendChannelUpdate(m_timer);
}

void Spell::UpdatePointers()
{
    if(m_originalCasterGUID==m_caster->GetGUID())
        m_originalCaster = m_caster;
    else
    {
        m_originalCaster = ObjectAccessor::GetUnit(*m_caster,m_originalCasterGUID);
        if(m_originalCaster && !m_originalCaster->IsInWorld()) m_originalCaster = NULL;
    }

    m_targets.Update(m_caster);
}

bool Spell::IsAffectedBy(SpellEntry const *spellInfo, uint32 effectId)
{
    return sSpellMgr.IsAffectedBySpell(m_spellInfo,spellInfo->Id,effectId,spellInfo->EffectItemType[effectId]);
}

bool Spell::CheckTargetCreatureType(Unit* target) const
{
    uint32 spellCreatureTargetMask = m_spellInfo->TargetCreatureType;

    // Curse of Doom : not find another way to fix spell target check :/
    if(m_spellInfo->SpellFamilyName==SPELLFAMILY_WARLOCK && m_spellInfo->SpellFamilyFlags == UI64LIT(0x0200000000))
    {
        // not allow cast at player
        if(target->GetTypeId()==TYPEID_PLAYER)
            return false;

        spellCreatureTargetMask = 0x7FF;
    }

    // Dismiss Pet and Taming Lesson skipped
    if(m_spellInfo->Id == 2641 || m_spellInfo->Id == 23356)
        spellCreatureTargetMask =  0;

    if (spellCreatureTargetMask)
    {
        uint32 TargetCreatureType = target->GetCreatureTypeMask();

        return !TargetCreatureType || (spellCreatureTargetMask & TargetCreatureType);
    }
    return true;
}

CurrentSpellTypes Spell::GetCurrentContainer()
{
    if (IsNextMeleeSwingSpell())
        return(CURRENT_MELEE_SPELL);
    else if (IsAutoRepeat())
        return(CURRENT_AUTOREPEAT_SPELL);
    else if (IsChanneledSpell(m_spellInfo))
        return(CURRENT_CHANNELED_SPELL);
    else
        return(CURRENT_GENERIC_SPELL);
}

bool Spell::CheckTarget( Unit* target, uint32 eff )
{
    // Check targets for creature type mask and remove not appropriate (skip explicit self target case, maybe need other explicit targets)
    if(m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SELF )
    {
        if (!CheckTargetCreatureType(target))
            return false;
    }

    // Check targets for not_selectable unit flag and remove
    // A player can cast spells on his pet (or other controlled unit) though in any state
    if (target != m_caster && target->GetCharmerOrOwnerGUID() != m_caster->GetGUID())
    {
        // any unattackable target skipped
        if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE))
            return false;

        // unselectable targets skipped in all cases except TARGET_SCRIPT targeting
        // in case TARGET_SCRIPT target selected by server always and can't be cheated
        if( target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE) &&
            m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SCRIPT &&
            m_spellInfo->EffectImplicitTargetB[eff] != TARGET_SCRIPT )
            return false;
    }

    //Check player targets and remove if in GM mode or GM invisibility (for not self casting case)
    if( target != m_caster && target->GetTypeId() == TYPEID_PLAYER)
    {
        if(((Player*)target)->GetVisibility() == VISIBILITY_OFF)
            return false;

        if(((Player*)target)->isGameMaster() && !IsPositiveSpell(m_spellInfo->Id))
            return false;
    }

    //Check targets for LOS visibility (except spells without range limitations )
    switch(m_spellInfo->Effect[eff])
    {
        case SPELL_EFFECT_SUMMON_PLAYER:                    // from anywhere
            break;
        case SPELL_EFFECT_DUMMY:
            if(m_spellInfo->Id != 20577)                    // Cannibalize
                break;
            //fall through
        case SPELL_EFFECT_RESURRECT_NEW:
            // player far away, maybe his corpse near?
            if(target != m_caster && !target->IsWithinLOSInMap(m_caster))
            {
                if(!m_targets.getCorpseTargetGUID())
                    return false;

                Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster, m_targets.getCorpseTargetGUID());
                if(!corpse)
                    return false;

                if(target->GetGUID() != corpse->GetOwnerGUID())
                    return false;

                if(!corpse->IsWithinLOSInMap(m_caster))
                    return false;
            }

            // all ok by some way or another, skip normal check
            break;
        default:                                            // normal case
            // Get GO cast coordinates if original caster -> GO
            WorldObject *caster = NULL;
            if (m_originalCasterGUID)
                caster = m_caster->GetMap()->GetGameObject(m_originalCasterGUID);
            if (!caster)
                caster = m_caster;
            if(target!=m_caster && !target->IsWithinLOSInMap(caster))
                return false;
            break;
    }

    return true;
}

Unit* Spell::SelectMagnetTarget()
{
    Unit* target = m_targets.getUnitTarget();

    if(target && target->HasAuraType(SPELL_AURA_SPELL_MAGNET) && !(m_spellInfo->Attributes & 0x10))
    {
        Unit::AuraList const& magnetAuras = target->GetAurasByType(SPELL_AURA_SPELL_MAGNET);
        for(Unit::AuraList::const_iterator itr = magnetAuras.begin(); itr != magnetAuras.end(); ++itr)
        {
            if(Unit* magnet = (*itr)->GetCaster())
            {
                if(magnet->IsWithinLOSInMap(m_caster))
                {
                    target = magnet;
                    m_targets.setUnitTarget(target);
                    break;
                }
            }
        }
    }

    return target;
}

bool Spell::IsNeedSendToClient() const
{
    return m_spellInfo->SpellVisual!=0 || IsChanneledSpell(m_spellInfo) ||
        m_spellInfo->speed > 0.0f || !m_triggeredByAuraSpell && !m_IsTriggeredSpell;
}

bool Spell::HaveTargetsForEffect( uint8 effect ) const
{
    for(std::list<TargetInfo>::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        if(itr->effectMask & (1 << effect))
            return true;

    for(std::list<GOTargetInfo>::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        if(itr->effectMask & (1 << effect))
            return true;

    for(std::list<ItemTargetInfo>::const_iterator itr = m_UniqueItemInfo.begin(); itr != m_UniqueItemInfo.end(); ++itr)
        if(itr->effectMask & (1 << effect))
            return true;

    return false;
}

SpellEvent::SpellEvent(Spell* spell) : BasicEvent()
{
    m_Spell = spell;
}

SpellEvent::~SpellEvent()
{
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel();

    if (m_Spell->IsDeletable())
    {
        delete m_Spell;
    }
    else
    {
        sLog.outError("~SpellEvent: %s %u tried to delete non-deletable spell %u. Was not deleted, causes memory leak.",
            (m_Spell->GetCaster()->GetTypeId() == TYPEID_PLAYER ? "Player" : "Creature"), m_Spell->GetCaster()->GetGUIDLow(), m_Spell->m_spellInfo->Id);
    }
}

bool SpellEvent::Execute(uint64 e_time, uint32 p_time)
{
    // update spell if it is not finished
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->update(p_time);

    // check spell state to process
    switch (m_Spell->getState())
    {
        case SPELL_STATE_FINISHED:
        {
            // spell was finished, check deletable state
            if (m_Spell->IsDeletable())
            {
                // check, if we do have unfinished triggered spells
                return true;                                // spell is deletable, finish event
            }
            // event will be re-added automatically at the end of routine)
        } break;

        case SPELL_STATE_CASTING:
        {
            // this spell is in channeled state, process it on the next update
            // event will be re-added automatically at the end of routine)
        } break;

        case SPELL_STATE_DELAYED:
        {
            // first, check, if we have just started
            if (m_Spell->GetDelayStart() != 0)
            {
                // no, we aren't, do the typical update
                // check, if we have channeled spell on our hands
                if (IsChanneledSpell(m_Spell->m_spellInfo))
                {
                    // evented channeled spell is processed separately, casted once after delay, and not destroyed till finish
                    // check, if we have casting anything else except this channeled spell and autorepeat
                    if (m_Spell->GetCaster()->IsNonMeleeSpellCasted(false, true, true))
                    {
                        // another non-melee non-delayed spell is casted now, abort
                        m_Spell->cancel();
                    }
                    else
                    {
                        // do the action (pass spell to channeling state)
                        m_Spell->handle_immediate();
                    }
                    // event will be re-added automatically at the end of routine)
                }
                else
                {
                    // run the spell handler and think about what we can do next
                    uint64 t_offset = e_time - m_Spell->GetDelayStart();
                    uint64 n_offset = m_Spell->handle_delayed(t_offset);
                    if (n_offset)
                    {
                        // re-add us to the queue
                        m_Spell->GetCaster()->m_Events.AddEvent(this, m_Spell->GetDelayStart() + n_offset, false);
                        return false;                       // event not complete
                    }
                    // event complete
                    // finish update event will be re-added automatically at the end of routine)
                }
            }
            else
            {
                // delaying had just started, record the moment
                m_Spell->SetDelayStart(e_time);
                // re-plan the event for the delay moment
                m_Spell->GetCaster()->m_Events.AddEvent(this, e_time + m_Spell->GetDelayMoment(), false);
                return false;                               // event not complete
            }
        } break;

        default:
        {
            // all other states
            // event will be re-added automatically at the end of routine)
        } break;
    }

    // spell processing not complete, plan event on the next update interval
    m_Spell->GetCaster()->m_Events.AddEvent(this, e_time + 1, false);
    return false;                                           // event not complete
}

void SpellEvent::Abort(uint64 /*e_time*/)
{
    // oops, the spell we try to do is aborted
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel();
}

bool SpellEvent::IsDeletable() const
{
    return m_Spell->IsDeletable();
}

SpellCastResult Spell::CanOpenLock(uint32 effIndex, uint32 lockId, SkillType& skillId, int32& reqSkillValue, int32& skillValue)
{
    if(!lockId)                                             // possible case for GO and maybe for items.
        return SPELL_CAST_OK;

    // Get LockInfo
    LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);

    if (!lockInfo)
        return SPELL_FAILED_BAD_TARGETS;

    bool reqKey = false;                                    // some locks not have reqs

    for(int j = 0; j < 8; ++j)
    {
        switch(lockInfo->Type[j])
        {
            // check key item (many fit cases can be)
            case LOCK_KEY_ITEM:
                if(lockInfo->Index[j] && m_CastItem && m_CastItem->GetEntry()==lockInfo->Index[j])
                    return SPELL_CAST_OK;
                reqKey = true;
                break;
                // check key skill (only single first fit case can be)
            case LOCK_KEY_SKILL:
            {
                reqKey = true;

                // wrong locktype, skip
                if(uint32(m_spellInfo->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                    continue;

                skillId = SkillByLockType(LockType(lockInfo->Index[j]));

                if ( skillId != SKILL_NONE )
                {
                    // skill bonus provided by casting spell (mostly item spells)
                    // add the damage modifier from the spell casted (cheat lock / skeleton key etc.) (use m_currentBasePoints, CalculateDamage returns wrong value)
                    uint32 spellSkillBonus = uint32(m_currentBasePoints[effIndex]+1);
                    reqSkillValue = lockInfo->Skill[j];

                    // castitem check: rogue using skeleton keys. the skill values should not be added in this case.
                    skillValue = m_CastItem || m_caster->GetTypeId()!= TYPEID_PLAYER ?
                        0 : ((Player*)m_caster)->GetSkillValue(skillId);

                    skillValue += spellSkillBonus;

                    if (skillValue < reqSkillValue)
                        return SPELL_FAILED_LOW_CASTLEVEL;
                }

                return SPELL_CAST_OK;
            }
        }
    }

    if(reqKey)
        return SPELL_FAILED_BAD_TARGETS;

    return SPELL_CAST_OK;
}

void Spell::FillAreaTargets( UnitList& TagUnitMap, float x, float y, float radius, SpellNotifyPushType pushType, SpellTargets spellTargets )
{
    CellPair p(MaNGOS::ComputeCellPair(x, y));
    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();
    MaNGOS::SpellNotifierCreatureAndPlayer notifier(*this, TagUnitMap, radius, pushType, spellTargets);
    TypeContainerVisitor<MaNGOS::SpellNotifierCreatureAndPlayer, WorldTypeMapContainer > world_notifier(notifier);
    TypeContainerVisitor<MaNGOS::SpellNotifierCreatureAndPlayer, GridTypeMapContainer > grid_notifier(notifier);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, world_notifier, *m_caster->GetMap(), *m_caster, radius);
    cell_lock->Visit(cell_lock, grid_notifier, *m_caster->GetMap(), *m_caster, radius);
}

void Spell::FillRaidOrPartyTargets( UnitList &TagUnitMap, Unit* target, float radius, bool raid, bool withPets, bool withcaster )
{
    Player *pTarget = target->GetCharmerOrOwnerPlayerOrPlayerItself();
    Group *pGroup = pTarget ? pTarget->GetGroup() : NULL;

    if (pGroup)
    {
        uint8 subgroup = pTarget->GetSubGroup();

        for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* Target = itr->getSource();

            // IsHostileTo check duel and controlled by enemy
            if (Target && (raid || subgroup==Target->GetSubGroup())
                && !m_caster->IsHostileTo(Target))
            {
                if (Target==m_caster && withcaster ||
                    Target!=m_caster && m_caster->IsWithinDistInMap(Target, radius))
                    TagUnitMap.push_back(Target);

                if (withPets)
                    if (Pet* pet = Target->GetPet())
                        if (pet==m_caster && withcaster ||
                            pet!=m_caster && m_caster->IsWithinDistInMap(pet, radius))
                            TagUnitMap.push_back(pet);
            }
        }
    }
    else
    {
        Unit* ownerOrSelf = pTarget ? pTarget : target->GetCharmerOrOwnerOrSelf();
        if (ownerOrSelf==m_caster && withcaster ||
            ownerOrSelf!=m_caster && m_caster->IsWithinDistInMap(ownerOrSelf, radius))
            TagUnitMap.push_back(ownerOrSelf);

        if (withPets)
            if (Pet* pet = ownerOrSelf->GetPet())
                if (pet==m_caster && withcaster ||
                    pet!=m_caster && m_caster->IsWithinDistInMap(pet, radius))
                    TagUnitMap.push_back(pet);
    }
}