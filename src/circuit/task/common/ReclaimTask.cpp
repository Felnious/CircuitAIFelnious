/*
 * ReclaimTask.cpp
 *
 *  Created on: Sep 4, 2016
 *      Author: rlcevg
 */

#include "task/common/ReclaimTask.h"
#include "module/TaskModule.h"
#include "terrain/TerrainManager.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "spring/SpringCallback.h"

#include "AISCommands.h"

namespace circuit {

using namespace springai;

// Nearby static assist turrets (nanotowers) pitch in on a reclaim within their own build range
static void AssistNanosInRange(CCircuitAI* circuit, CCircuitUnit* unit, const AIFloat3& pos, float radius, CCircuitUnit* target)
{
	const int frame = circuit->GetLastFrame();
	const auto& friendlies = circuit->GetCallback()->GetFriendlyUnitIdsIn(pos, radius + 400.f, false);
	for (const int fId : friendlies) {
		if (fId == unit->GetId()) {
			continue;
		}
		CCircuitUnit* nano = circuit->GetTeamUnit(fId);
		if (nano == nullptr) {
			continue;
		}
		CCircuitDef* cdef = nano->GetCircuitDef();
		if ((cdef == nullptr) || cdef->IsMobile() || !cdef->IsAssist()) {
			continue;
		}
		if (pos.SqDistance2D(nano->GetPos(frame)) > SQUARE(cdef->GetBuildDistance() + radius)) {
			continue;
		}
		TRY_UNIT(circuit, nano,
			if (target != nullptr) {
				nano->CmdReclaimUnit(target, UNIT_CMD_OPTION, frame + FRAMES_PER_SEC * 60);
			} else {
				nano->CmdReclaimInArea(pos, radius, UNIT_COMMAND_OPTION_CONTROL_KEY, frame + FRAMES_PER_SEC * 60);
			}
		)
	}
}

IReclaimTask::IReclaimTask(ITaskModule* mgr, Priority priority, Type type,
						   const AIFloat3& position,
						   SResource cost, int timeout, float radius, bool isMetal)
		: IBuilderTask(mgr, priority, nullptr, position, type, BuildType::RECLAIM, cost, 0.f, timeout)
		, radius(radius)
		, isMetal(isMetal)
{
}

IReclaimTask::IReclaimTask(ITaskModule* mgr, Priority priority, Type type,
						   CCircuitUnit* target,
						   int timeout)
		: IBuilderTask(mgr, priority, nullptr, -RgtVector, type, BuildType::RECLAIM, {1000.f, 0.f}, 0.f, timeout)
		, radius(0.f)
		, isMetal(false)
{
	SetTarget(target);
}

IReclaimTask::IReclaimTask(ITaskModule* mgr, Type type)
		: IBuilderTask(mgr, type, BuildType::RECLAIM)
		, radius(0.f)
		, isMetal(false)
{
}

IReclaimTask::~IReclaimTask()
{
}

bool IReclaimTask::CanAssignTo(CCircuitUnit* unit) const
{
	return unit->GetCircuitDef()->IsAbleToReclaim() && (cost.metal > buildPower.metal * MAX_BUILD_SEC);
}

void IReclaimTask::RemoveAssignee(CCircuitUnit* unit)
{
	IBuilderTask::RemoveAssignee(unit);
	if (units.empty()) {
		manager->AbortTask(this);
	}
}

void IReclaimTask::Finish()
{
}

void IReclaimTask::Cancel()
{
}

bool IReclaimTask::Execute(CCircuitUnit* unit)
{
	executors.insert(unit);

	CCircuitAI* circuit = manager->GetCircuit();
	TRY_UNIT(circuit, unit,
		unit->CmdPriority(ClampPriority());
	)

	const int frame = circuit->GetLastFrame();
	if (target != nullptr) {
		TRY_UNIT(circuit, unit,
			unit->CmdReclaimUnit(target, UNIT_CMD_OPTION, frame + FRAMES_PER_SEC * 60);
		)
		AssistNanosInRange(circuit, unit, target->GetPos(frame), 0.f, target);
		return true;
	}

	AIFloat3 pos;
	float reclRadius;
	if ((radius == .0f) || !geom::is_valid(position)) {
		pos = circuit->GetTerrainManager()->GetTerrainCenter();
		reclRadius = pos.Length2D();
	} else {
		pos = position;
		reclRadius = radius;
	}
	TRY_UNIT(circuit, unit,
		// NOTE: CONTROL_KEY enables special mode that ignores autoreclaimable value
		unit->CmdReclaimInArea(pos, reclRadius, UNIT_COMMAND_OPTION_CONTROL_KEY, frame + FRAMES_PER_SEC * 60);
	)
	AssistNanosInRange(circuit, unit, pos, reclRadius, nullptr);
	return true;
}

void IReclaimTask::OnUnitIdle(CCircuitUnit* unit)
{
	manager->AbortTask(this);
}

void IReclaimTask::SetTarget(CCircuitUnit* unit)
{
	target = unit;
	buildPos = (unit != nullptr) ? unit->GetPos(manager->GetCircuit()->GetLastFrame()) : AIFloat3(-RgtVector);
}

bool IReclaimTask::IsInRange(const AIFloat3& pos, float range) const
{
	return position.SqDistance2D(pos) <= SQUARE(radius + range);
}

#define SERIALIZE(stream, func)	\
	utils::binary_##func(stream, radius);		\
	utils::binary_##func(stream, isMetal);

bool IReclaimTask::Load(std::istream& is)
{
	IBuilderTask::Load(is);
	SERIALIZE(is, read)
#ifdef DEBUG_SAVELOAD
	manager->GetCircuit()->LOG("%s | radius=%f | isMetal=%i", __PRETTY_FUNCTION__, radius, isMetal);
#endif
	return true;
}

void IReclaimTask::Save(std::ostream& os) const
{
	IBuilderTask::Save(os);
	SERIALIZE(os, write)
#ifdef DEBUG_SAVELOAD
	manager->GetCircuit()->LOG("%s | radius=%f | isMetal=%i", __PRETTY_FUNCTION__, radius, isMetal);
#endif
}

} // namespace circuit
