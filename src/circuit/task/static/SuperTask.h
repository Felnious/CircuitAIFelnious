/*
 * SuperTask.h
 *
 *  Created on: Aug 12, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_STATIC_SUPERTASK_H_
#define SRC_CIRCUIT_TASK_STATIC_SUPERTASK_H_

#include "task/fighter/FighterTask.h"

namespace circuit {

class CCircuitDef;
class CEnemyInfo;
class CEnemyManager;

class CSuperTask final: public IFighterTask {
public:
	CSuperTask(ITaskModule* mgr);
	virtual ~CSuperTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Start(CCircuitUnit* unit) override;
	virtual void Update() override;

	// Script hooks
	void SetTargetPos(const springai::AIFloat3& pos);

private:
	void ExecuteAttack(CCircuitUnit* unit);
	// Random target near the front; returns nullptr and fills outPos when no live target is visible
	CEnemyInfo* FindJunoTarget(CCircuitUnit* unit, CCircuitDef* cdef, int frame, springai::AIFloat3& outPos);
	// Highest-cost known group ("biggest metal clump"); returns nullptr and fills outPos otherwise.
	// outMetal receives the total metal cost of the targeted group (0 if none).
	CEnemyInfo* FindEmpTarget(CCircuitUnit* unit, CCircuitDef* cdef, int frame, springai::AIFloat3& outPos, float& outMetal);
	// Cortron/Legperdition: Afus/Fusion reactors > Labs > other static structures > everything else
	CEnemyInfo* FindPriorityEmpTarget(CCircuitUnit* unit, CCircuitDef* cdef, int frame, springai::AIFloat3& outPos);

	int targetFrame;
	springai::AIFloat3 targetPos;
	bool isTargetOverride;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_STATIC_SUPERTASK_H_
