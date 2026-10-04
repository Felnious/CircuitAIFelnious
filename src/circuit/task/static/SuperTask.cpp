/*
 * SuperTask.cpp
 *
 *  Created on: Aug 12, 2016
 *      Author: rlcevg
 */

#include "task/static/SuperTask.h"
#include "task/fighter/SquadTask.h"
#include "map/InfluenceMap.h"
#include "module/MilitaryManager.h"
#include "setup/SetupManager.h"
#include "terrain/TerrainManager.h"
#include "unit/enemy/EnemyUnit.h"
#include "unit/CircuitUnit.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"
#include "spring/SpringMap.h"

#include "AISCommands.h"
#include "Lua.h"
#include "Log.h"

#include <cctype>
#include <format>

namespace circuit {

using namespace springai;

#define TARGET_DELAY	(FRAMES_PER_SEC * 10)

CSuperTask::CSuperTask(ITaskModule* mgr)
		: IFighterTask(mgr, IFighterTask::FightType::SUPER, 1.f)
		, targetFrame(0)
		, targetPos(-RgtVector)
		, isTargetOverride(false)
{
}

CSuperTask::~CSuperTask()
{
}

bool CSuperTask::CanAssignTo(CCircuitUnit* unit) const
{
	return false;
}

void CSuperTask::RemoveAssignee(CCircuitUnit* unit)
{
	IFighterTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}
}

void CSuperTask::Start(CCircuitUnit* unit)
{
	const int frame = manager->GetCircuit()->GetLastFrame();
	targetFrame = frame - TARGET_DELAY;
	position = unit->GetPos(frame);
	if (unit->GetCircuitDef()->IsAttrJuno()) {
		manager->GetCircuit()->LOG("%s: CSuperTask::Start", unit->GetCircuitDef()->GetDef()->GetName());
	}
}

void CSuperTask::Update()
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();
	CCircuitUnit* unit = *units.begin();

	if (unit->GetCircuitDef()->IsAttrJuno()) {
		circuit->LOG("%s: CSuperTask::Update enter, blocker=%d", unit->GetCircuitDef()->GetDef()->GetName(), unit->Blocker() != nullptr);
	}

	if (unit->Blocker() != nullptr) {
		return;  // Do not interrupt current action
	}

	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsHoldFire()) {
		if (targetFrame + (cdef->GetReloadTime() + TARGET_DELAY) > frame) {
			if ((State::ENGAGE == state) && (targetFrame + TARGET_DELAY <= frame)) {
				TRY_UNIT(circuit, unit,
					unit->CmdStop();
				)
				state = State::ROAM;
			}
			return;
		}
	} else if (targetFrame + TARGET_DELAY > frame) {
		return;
	}

	if (isTargetOverride) {
		ExecuteAttack(unit);
		return;
	}

	if (cdef->IsAttrJuno() || cdef->IsAttrEmp()) {
		// Force-fire the instant a rocket is ready: always resolve *some* target/position,
		// bypassing the ally-influence/avoid-overlap gating used by the default group logic below
		// (that gating could reject every candidate and leave the silo stockpiling forever).
		AIFloat3 firePos = -RgtVector;
		const std::string defName = cdef->GetDef()->GetName();
		if (cdef->IsAttrJuno()) {
			// Diagnostic: this branch is only reached once per TARGET_DELAY/reload window, so
			// logging here is cheap and shows the real ammo state (actual firing needs the
			// engine's stockpile count > 0, which is independent of our targeting logic).
			circuit->LOG("%s: stockpile=%d queued=%d fireState=%d holdFire=%d",
					defName.c_str(), unit->GetUnit()->GetStockpile(), unit->GetUnit()->GetStockpileQueued(),
					cdef->GetFireState(), (int)cdef->IsHoldFire());
		}
		const bool isPriorityEmp = cdef->IsAttrEmp()
				&& ((defName == "cortron") || (defName == "legperdition"));
		float empMetal = 0.f;
		CEnemyInfo* bestTarget;
		if (cdef->IsAttrJuno()) {
			bestTarget = FindJunoTarget(unit, cdef, frame, firePos);
		} else if (isPriorityEmp) {
			bestTarget = FindPriorityEmpTarget(unit, cdef, frame, firePos);
		} else {
			bestTarget = FindEmpTarget(unit, cdef, frame, firePos, empMetal);
		}
		if ((bestTarget == nullptr) && !geom::is_valid(firePos)) {
			TRY_UNIT(circuit, unit,
				unit->CmdStop();
			)
			SetTarget(nullptr);
			targetFrame = frame;
			return;
		}
		if (cdef->IsAttrEmp() && !isPriorityEmp && (empMetal > 0.f)) {
			std::string label = defName;
			if (!label.empty()) {
				label[0] = std::toupper((unsigned char)label[0]);
			}
			circuit->LOG("%s'd %.0f Metal", label.c_str(), empMetal);
		}
		SetTarget(bestTarget);
		targetPos = (bestTarget != nullptr) ? bestTarget->GetPos() : firePos;
		targetPos.y = circuit->GetMap()->GetElevationAt(targetPos.x, targetPos.z);
		ExecuteAttack(unit);
		return;
	}

	CInfluenceMap* inflMap = circuit->GetInflMap();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	const float maxSqRange = SQUARE(cdef->GetMaxRange());
	const float sqAoe = SQUARE(cdef->GetAoe() * 1.25f);
	float cost = 0.f;
	int groupIdx = -1;
	const std::array<const std::set<IFighterTask*>*, 3> avoidTasks = {  // NOTE: ISquadTask only
		&militaryMgr->GetTasks(IFighterTask::FightType::ATTACK),
		&militaryMgr->GetTasks(IFighterTask::FightType::AH),
		&militaryMgr->GetTasks(IFighterTask::FightType::AA),
	};
	auto isTargetValid = [&avoidTasks, frame, sqAoe, inflMap, circuit](const CEnemyManager::SEnemyGroup& group) {
		// Ally influence and own tasks avoidance
		if (inflMap->GetInfluenceAt(group.pos) > -INFL_EPS) {
			return false;
		}
		for (const std::set<IFighterTask*>* tasks : avoidTasks) {
			for (const IFighterTask* task : *tasks) {
				const AIFloat3& leaderPos = static_cast<const ISquadTask*>(task)->GetLeaderPos(frame);
				if (leaderPos.SqDistance2D(group.pos) < sqAoe) {
					return false;
				}
			}
		}
		for (const ICoreUnit::Id eId : group.units) {
			CEnemyInfo* enemy = circuit->GetEnemyInfo(eId);
			if (enemy == nullptr) {
				continue;
			}
			CCircuitDef* edef = enemy->GetCircuitDef();
			// NOTE: groups are created by leader, ignore flags could be different
			if ((edef == nullptr) || !circuit->GetCircuitDef(edef->GetId())->IsIgnore()) {
				return true;
			}
		}
		return false;
	};

	const std::vector<CEnemyManager::SEnemyGroup>& groups = circuit->GetEnemyManager()->GetEnemyGroups();
	if (cdef->IsHoldFire() || (State::ROAM == state)) {
		for (unsigned i = 0; i < groups.size(); ++i) {
			const CEnemyManager::SEnemyGroup& group = groups[i];
			if ((cost >= group.cost) || (position.SqDistance2D(group.pos) >= maxSqRange)) {
				continue;
			}
			if (isTargetValid(group)) {
				cost = group.cost;
				groupIdx = i;
			}
		}
	} else {
		// TODO: Use WeaponDef::GetTurnRate() for turn-delay weight
		const AIFloat3& targetVec = (targetPos - position).Normalize2D();
		for (unsigned i = 0; i < groups.size(); ++i) {
			const CEnemyManager::SEnemyGroup& group = groups[i];
			if (position.SqDistance2D(group.pos) >= maxSqRange) {
				continue;
			}
			const AIFloat3& newVec = (group.pos - position).Normalize2D();
			const float angleMod = M_PI / (2.f * (std::acos(targetVec.dot2D(newVec)) + 1e-2f));
			if (cost >= group.cost * angleMod) {
				continue;
			}
			if (isTargetValid(group)) {
				cost = group.cost;
				groupIdx = i;
			}
		}
	}
	const float maxCost = cdef->IsAttrStock() ? cdef->GetWeaponDef()->GetCostM() : cdef->GetCostM() * 0.01f;

	if ((groupIdx < 0) || (cost < maxCost)) {
		TRY_UNIT(circuit, unit,
			unit->CmdStop();
		)
		SetTarget(nullptr);
		targetFrame = frame;
		return;
	}

	const AIFloat3& grPos = groups[groupIdx].pos;
	CEnemyInfo* bestTarget = nullptr;
	if (cdef->IsAttrStock()) {
		float minSqDist = std::numeric_limits<float>::max();
		for (const ICoreUnit::Id eId : groups[groupIdx].units) {
			CEnemyInfo* enemy = circuit->GetEnemyInfo(eId);
			if (enemy == nullptr) {
				continue;
			}
			CCircuitDef* edef = enemy->GetCircuitDef();
			// NOTE: groups are created by leader, ignore flags could be different
			if ((edef != nullptr) && circuit->GetCircuitDef(edef->GetId())->IsIgnore()) {
				continue;
			}
			const float sqDist = grPos.SqDistance2D(enemy->GetPos());
			if ((minSqDist > sqDist) && (position.SqDistance2D(enemy->GetPos()) < maxSqRange)) {
				minSqDist = sqDist;
				bestTarget = enemy;
			}
		}
	} else {
		float maxCost = 0.f;
		for (const ICoreUnit::Id eId : groups[groupIdx].units) {
			CEnemyInfo* enemy = circuit->GetEnemyInfo(eId);
			if (enemy == nullptr) {
				continue;
			}
			CCircuitDef* edef = enemy->GetCircuitDef();
			// NOTE: groups are created by leader, ignore flags could be different
			if ((edef != nullptr) && circuit->GetCircuitDef(edef->GetId())->IsIgnore()) {
				continue;
			}
			if ((maxCost < enemy->GetCost()) && (position.SqDistance2D(enemy->GetPos()) < maxSqRange)) {
				maxCost = enemy->GetCost();
				bestTarget = enemy;
			}
		}
	}
	SetTarget(bestTarget);
	if (GetTarget() != nullptr) {
		targetPos = GetTarget()->GetPos();
		targetPos.y = circuit->GetMap()->GetElevationAt(targetPos.x, targetPos.z);

		ExecuteAttack(unit);
	}
}

CEnemyInfo* CSuperTask::FindJunoTarget(CCircuitUnit* unit, CCircuitDef* cdef, int frame, AIFloat3& outPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = unit->GetPos(frame);
	const float range = cdef->GetMaxRange();

	// Force-fire Juno at a visible enemy to keep harassing the front line; Labs and Defenses
	// (immobile builders / immobile attackers) take priority over everything else in range.
	auto& enemyIds = circuit->GetCallback()->GetEnemyUnitIdsIn(pos, range);
	std::vector<CEnemyInfo*> labs;
	std::vector<CEnemyInfo*> defenses;
	std::vector<CEnemyInfo*> candidates;
	for (int eId : enemyIds) {
		CEnemyInfo* enemy = circuit->GetEnemyInfo(eId);
		if ((enemy == nullptr) || !enemy->IsInRadarOrLOS()) {
			continue;
		}
		candidates.push_back(enemy);
		CCircuitDef* edef = enemy->GetCircuitDef();
		if ((edef != nullptr) && !edef->IsMobile()) {
			if (edef->IsBuilder()) {
				labs.push_back(enemy);
			} else if (edef->IsAttacker()) {
				defenses.push_back(enemy);
			}
		}
	}
	if (!labs.empty()) {
		return labs[rand() % labs.size()];
	}
	if (!defenses.empty()) {
		return defenses[rand() % defenses.size()];
	}
	if (!candidates.empty()) {
		return candidates[rand() % candidates.size()];
	}

	// No visible enemy: aim at a random known contact, or blind at the front line
	const std::vector<CEnemyManager::SEnemyGroup>& groups = circuit->GetEnemyManager()->GetEnemyGroups();
	std::vector<const CEnemyManager::SEnemyGroup*> nearGroups;
	for (const CEnemyManager::SEnemyGroup& group : groups) {
		if (pos.SqDistance2D(group.pos) < SQUARE(range)) {
			nearGroups.push_back(&group);
		}
	}
	if (!nearGroups.empty()) {
		outPos = nearGroups[rand() % nearGroups.size()]->pos;
		return nullptr;
	}

	const AIFloat3& lanePos = circuit->GetSetupManager()->GetLanePos();
	const AIFloat3& anchor = geom::is_valid(lanePos) ? lanePos : pos;  // always resolve to something
	const float jitter = std::min(range * 0.5f, 1000.f);
	outPos = anchor + AIFloat3((float)(rand() % 2001 - 1000) * 0.001f * jitter, 0.f,
								(float)(rand() % 2001 - 1000) * 0.001f * jitter);
	CTerrainManager::CorrectPosition(outPos);
	return nullptr;
}

CEnemyInfo* CSuperTask::FindEmpTarget(CCircuitUnit* unit, CCircuitDef* cdef, int frame, AIFloat3& outPos, float& outMetal)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = unit->GetPos(frame);
	const float range = cdef->GetMaxRange();

	outMetal = 0.f;

	// Highest total-cost known group in range ("biggest metal clump") - no ally-safety gating
	const std::vector<CEnemyManager::SEnemyGroup>& groups = circuit->GetEnemyManager()->GetEnemyGroups();
	const CEnemyManager::SEnemyGroup* bestGroup = nullptr;
	float bestCost = 0.f;
	for (const CEnemyManager::SEnemyGroup& group : groups) {
		if (pos.SqDistance2D(group.pos) >= SQUARE(range)) {
			continue;
		}
		if (bestCost < group.cost) {
			bestCost = group.cost;
			bestGroup = &group;
		}
	}
	if (bestGroup != nullptr) {
		outMetal = bestGroup->cost;
		float minSqDist = std::numeric_limits<float>::max();
		CEnemyInfo* bestTarget = nullptr;
		for (const ICoreUnit::Id eId : bestGroup->units) {
			CEnemyInfo* enemy = circuit->GetEnemyInfo(eId);
			if (enemy == nullptr) {
				continue;
			}
			const float sqDist = bestGroup->pos.SqDistance2D(enemy->GetPos());
			if (minSqDist > sqDist) {
				minSqDist = sqDist;
				bestTarget = enemy;
			}
		}
		if (bestTarget != nullptr) {
			return bestTarget;
		}
		outPos = bestGroup->pos;
		return nullptr;
	}

	// Nothing in range yet: aim at any other known group, else blind at the front line
	if (!groups.empty()) {
		outPos = groups[rand() % groups.size()].pos;
		return nullptr;
	}

	const AIFloat3& lanePos = circuit->GetSetupManager()->GetLanePos();
	outPos = geom::is_valid(lanePos) ? lanePos : pos;  // always resolve to something
	CTerrainManager::CorrectPosition(outPos);
	return nullptr;
}

CEnemyInfo* CSuperTask::FindPriorityEmpTarget(CCircuitUnit* unit, CCircuitDef* cdef, int frame, AIFloat3& outPos)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const AIFloat3& pos = unit->GetPos(frame);
	const float range = cdef->GetMaxRange();

	// Cortron/Legperdition: Afus/Fusion reactors > Labs (immobile builders) > other static
	// structures > everything else visible in range.
	auto& enemyIds = circuit->GetCallback()->GetEnemyUnitIdsIn(pos, range);
	std::vector<CEnemyInfo*> fusions;
	std::vector<CEnemyInfo*> labs;
	std::vector<CEnemyInfo*> statics;
	std::vector<CEnemyInfo*> candidates;
	for (int eId : enemyIds) {
		CEnemyInfo* enemy = circuit->GetEnemyInfo(eId);
		if ((enemy == nullptr) || !enemy->IsInRadarOrLOS()) {
			continue;
		}
		candidates.push_back(enemy);
		CCircuitDef* edef = enemy->GetCircuitDef();
		if (edef == nullptr) {
			continue;
		}
		if (std::string(edef->GetDef()->GetName()).find("fus") != std::string::npos) {
			fusions.push_back(enemy);
		} else if (!edef->IsMobile() && edef->IsBuilder()) {
			labs.push_back(enemy);
		} else if (!edef->IsMobile()) {
			statics.push_back(enemy);
		}
	}
	if (!fusions.empty()) {
		return fusions[rand() % fusions.size()];
	}
	if (!labs.empty()) {
		return labs[rand() % labs.size()];
	}
	if (!statics.empty()) {
		return statics[rand() % statics.size()];
	}
	if (!candidates.empty()) {
		return candidates[rand() % candidates.size()];
	}

	// Nothing visible: aim at any known group, else blind at the front line
	const std::vector<CEnemyManager::SEnemyGroup>& groups = circuit->GetEnemyManager()->GetEnemyGroups();
	std::vector<const CEnemyManager::SEnemyGroup*> nearGroups;
	for (const CEnemyManager::SEnemyGroup& group : groups) {
		if (pos.SqDistance2D(group.pos) < SQUARE(range)) {
			nearGroups.push_back(&group);
		}
	}
	if (!nearGroups.empty()) {
		outPos = nearGroups[rand() % nearGroups.size()]->pos;
		return nullptr;
	}

	const AIFloat3& lanePos = circuit->GetSetupManager()->GetLanePos();
	outPos = geom::is_valid(lanePos) ? lanePos : pos;  // always resolve to something
	CTerrainManager::CorrectPosition(outPos);
	return nullptr;
}

void CSuperTask::SetTargetPos(const AIFloat3& pos)
{
	SetTarget(nullptr);
	targetFrame = 0;
	isTargetOverride = geom::is_valid(pos);
	if (isTargetOverride) {
		targetPos = pos;
		targetPos.y = manager->GetCircuit()->GetMap()->GetElevationAt(pos.x, pos.z);
	}
}

void CSuperTask::ExecuteAttack(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	const int frame = circuit->GetLastFrame();

	bool isFiring = !unit->GetCircuitDef()->IsAttrStock() || (unit->GetUnit()->GetStockpile() > 0);
	std::string cmd = isFiring ? "ai_super_fire:" : "ai_super_intention:";
	cmd += std::format("{}/{}/{}", unit->GetId(), int(targetPos.x), int(targetPos.z));
	circuit->GetLua()->CallRules(cmd.c_str(), cmd.size());

	TRY_UNIT(circuit, unit,
		if (!isTargetOverride && (GetTarget() != nullptr) && GetTarget()->IsInRadarOrLOS() && !circuit->IsCheating()) {
			unit->GetUnit()->Attack(GetTarget()->GetUnit(), UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		} else {
			unit->CmdAttackGround(targetPos, UNIT_COMMAND_OPTION_RIGHT_MOUSE_KEY, frame + FRAMES_PER_SEC * 60);
		}
	)
	targetFrame = frame;
	state = State::ENGAGE;
}

} // namespace circuit
