/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#ifndef VALIDATOR_GOVALIDATOR_H_
#define VALIDATOR_GOVALIDATOR_H_

#include "planner/plan/Query.h"
#include "validator/TraversalValidator.h"

namespace nebula {
namespace graph {
class GoValidator final : public TraversalValidator {
public:
    using VertexProp = nebula::storage::cpp2::VertexProp;
    using EdgeProp = nebula::storage::cpp2::EdgeProp;
    GoValidator(Sentence* sentence, QueryContext* context)
        : TraversalValidator(sentence, context) {}

private:
    Status validateImpl() override;

    Status toPlan() override;

    Status validateWhere(WhereClause* where);

    Status validateYield(YieldClause* yield);

    void extractPropExprs(const Expression* expr);

    Expression* rewrite2VarProp(const Expression* expr);

    Status buildColumns();

    Status buildOneStepPlan();

    Status buildNStepsPlan();

    Status buildMToNPlan();

    Status oneStep(PlanNode* dependencyForGn,
                   const std::string& inputVarNameForGN,
                   PlanNode* projectFromJoin);

    std::vector<std::string> buildDstVertexColNames();

    std::unique_ptr<std::vector<VertexProp>> buildSrcVertexProps();

    std::unique_ptr<std::vector<VertexProp>> buildDstVertexProps();

    std::unique_ptr<std::vector<EdgeProp>> buildEdgeProps();

    std::unique_ptr<std::vector<EdgeProp>> buildEdgeDst();

    void buildEdgeProps(std::unique_ptr<std::vector<EdgeProp>>& edgeProps, bool isInEdge);

    PlanNode* buildLeftVarForTraceJoin(PlanNode* dedupStartVid);

    PlanNode* traceToStartVid(PlanNode* projectLeftVarForJoin, PlanNode* dedupDstVids);

    PlanNode* buildJoinPipeOrVariableInput(PlanNode* projectFromJoin,
                                           PlanNode* dependencyForJoinInput);

    PlanNode* buildProjectSrcEdgePropsForGN(std::string gnVar, PlanNode* dependency);

    PlanNode* buildJoinDstProps(PlanNode* projectSrcDstProps);

    PlanNode* projectSrcDstVidsFromGN(PlanNode* dep, PlanNode* gn);

private:
    YieldColumns* yields() const {
        return newYieldCols_ ? newYieldCols_ : yields_;
    }

    Expression* filter() const {
        return newFilter_ ? newFilter_ : filter_;
    }

    Over over_;
    Expression* filter_{nullptr};
    YieldColumns* yields_{nullptr};
    bool distinct_{false};

    // Generated by validator if needed, and the lifecycle of raw pinters would
    // be managed by object pool
    YieldColumns* srcAndEdgePropCols_{nullptr};
    YieldColumns* dstPropCols_{nullptr};
    YieldColumns* inputPropCols_{nullptr};
    std::unordered_map<std::string, YieldColumn*> propExprColMap_;
    Expression* newFilter_{nullptr};
    YieldColumns* newYieldCols_{nullptr};
    // Used for n steps to trace the path
    std::string dstVidColName_;
    // Used for get dst props
    std::string joinDstVidColName_;
};
}   // namespace graph
}   // namespace nebula
#endif
