/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "planner/match/MatchSolver.h"

#include "common/expression/UnaryExpression.h"
#include "context/ast/AstContext.h"
#include "context/ast/CypherAstContext.h"
#include "planner/Planner.h"
#include "planner/plan/Query.h"
#include "util/ExpressionUtils.h"
#include "util/SchemaUtil.h"
#include "visitor/RewriteVisitor.h"

namespace nebula {
namespace graph {
Expression* MatchSolver::rewriteLabel2Vertex(const Expression* expr) {
    auto matcher = [](const Expression* e) -> bool {
        return e->kind() == Expression::Kind::kLabel ||
               e->kind() == Expression::Kind::kLabelAttribute;
    };
    auto rewriter = [](const Expression* e) -> Expression* {
        DCHECK(e->kind() == Expression::Kind::kLabelAttribute ||
               e->kind() == Expression::Kind::kLabel);
        if (e->kind() == Expression::Kind::kLabelAttribute) {
            auto la = static_cast<const LabelAttributeExpression*>(e);
            return new AttributeExpression(new VertexExpression(), la->right()->clone().release());
        }
        return new VertexExpression();
    };

    return RewriteVisitor::transform(expr, std::move(matcher), std::move(rewriter));
}

Expression* MatchSolver::rewriteLabel2Edge(const Expression* expr) {
    auto matcher = [](const Expression* e) -> bool {
        return e->kind() == Expression::Kind::kLabel ||
               e->kind() == Expression::Kind::kLabelAttribute;
    };
    auto rewriter = [](const Expression* e) -> Expression* {
        DCHECK(e->kind() == Expression::Kind::kLabelAttribute ||
               e->kind() == Expression::Kind::kLabel);
        if (e->kind() == Expression::Kind::kLabelAttribute) {
            auto la = static_cast<const LabelAttributeExpression*>(e);
            return new AttributeExpression(new EdgeExpression(), la->right()->clone().release());
        }
        return new EdgeExpression();
    };

    return RewriteVisitor::transform(expr, std::move(matcher), std::move(rewriter));
}

Expression* MatchSolver::rewriteLabel2VarProp(const Expression* expr) {
    auto matcher = [](const Expression* e) -> bool {
        return e->kind() == Expression::Kind::kLabel ||
               e->kind() == Expression::Kind::kLabelAttribute;
    };
    auto rewriter = [](const Expression* e) -> Expression* {
        DCHECK(e->kind() == Expression::Kind::kLabelAttribute ||
               e->kind() == Expression::Kind::kLabel);
        if (e->kind() == Expression::Kind::kLabelAttribute) {
            auto* la = static_cast<const LabelAttributeExpression*>(e);
            auto* var = new VariablePropertyExpression("", la->left()->name());
            return new AttributeExpression(var, new ConstantExpression(la->right()->value()));
        }
        auto label = static_cast<const LabelExpression*>(e);
        return new VariablePropertyExpression("", label->name());
    };

    return RewriteVisitor::transform(expr, std::move(matcher), std::move(rewriter));
}

Expression* MatchSolver::doRewrite(const std::unordered_map<std::string, AliasType>& aliases,
                                   const Expression* expr) {
    if (expr->kind() == Expression::Kind::kLabel) {
        auto* labelExpr = static_cast<const LabelExpression*>(expr);
        auto alias = aliases.find(labelExpr->name());
        DCHECK(alias != aliases.end());
    }

    return rewriteLabel2VarProp(expr);
}

Expression* MatchSolver::makeIndexFilter(const std::string& label,
                                         const MapExpression* map,
                                         QueryContext* qctx,
                                         bool isEdgeProperties) {
    auto makePropExpr = [=, &label](const std::string& prop) -> Expression* {
        if (isEdgeProperties) {
            return new EdgePropertyExpression(label, prop);
        }
        return new TagPropertyExpression(label, prop);
    };

    auto root = qctx->objPool()->makeAndAdd<LogicalExpression>(Expression::Kind::kLogicalAnd);
    std::vector<std::unique_ptr<Expression>> operands;
    operands.reserve(map->size());
    for (const auto& item : map->items()) {
        operands.emplace_back(new RelationalExpression(
            Expression::Kind::kRelEQ, makePropExpr(item.first), item.second->clone().release()));
    }
    root->setOperands(std::move(operands));
    return root;
}

Expression* MatchSolver::makeIndexFilter(const std::string& label,
                                         const std::string& alias,
                                         Expression* filter,
                                         QueryContext* qctx,
                                         bool isEdgeProperties) {
    static const std::unordered_set<Expression::Kind> kinds = {
        Expression::Kind::kRelEQ,
        Expression::Kind::kRelLT,
        Expression::Kind::kRelLE,
        Expression::Kind::kRelGT,
        Expression::Kind::kRelGE,
    };

    std::vector<const Expression*> ands;
    auto kind = filter->kind();
    if (kinds.count(kind) == 1) {
        ands.emplace_back(filter);
    } else if (kind == Expression::Kind::kLogicalAnd) {
        auto* logic = static_cast<LogicalExpression*>(filter);
        ExpressionUtils::pullAnds(logic);
        for (auto& operand : logic->operands()) {
            ands.emplace_back(operand.get());
        }
    } else {
        return nullptr;
    }

    std::vector<Expression*> relationals;
    for (auto* item : ands) {
        if (kinds.count(item->kind()) != 1) {
            continue;
        }

        auto* binary = static_cast<const BinaryExpression*>(item);
        auto* left = binary->left();
        auto* right = binary->right();
        const LabelAttributeExpression* la = nullptr;
        const ConstantExpression* constant = nullptr;
        // TODO(aiee) extract the logic that apllies to both match and lookup
        if (left->kind() == Expression::Kind::kLabelAttribute &&
            right->kind() == Expression::Kind::kConstant) {
            la = static_cast<const LabelAttributeExpression*>(left);
            constant = static_cast<const ConstantExpression*>(right);
        } else if (right->kind() == Expression::Kind::kLabelAttribute &&
                   left->kind() == Expression::Kind::kConstant) {
            la = static_cast<const LabelAttributeExpression*>(right);
            constant = static_cast<const ConstantExpression*>(left);
        } else {
            continue;
        }

        if (la->left()->name() != alias) {
            continue;
        }

        const auto &value = la->right()->value();
        auto* tpExpr =
            isEdgeProperties
                ? static_cast<Expression*>(new EdgePropertyExpression(label, value.getStr()))
                : static_cast<Expression*>(new TagPropertyExpression(label, value.getStr()));
        auto *newConstant = constant->clone().release();
        if (left->kind() == Expression::Kind::kLabelAttribute) {
            auto* rel = new RelationalExpression(item->kind(), tpExpr, newConstant);
            relationals.emplace_back(rel);
        } else {
            auto* rel = new RelationalExpression(item->kind(), newConstant, tpExpr);
            relationals.emplace_back(rel);
        }
    }

    if (relationals.empty()) {
        return nullptr;
    }

    auto* root = relationals[0];
    for (auto i = 1u; i < relationals.size(); i++) {
        auto* left = root;
        root = new LogicalExpression(Expression::Kind::kLogicalAnd, left, relationals[i]);
    }

    return qctx->objPool()->add(root);
}

void MatchSolver::extractAndDedupVidColumn(QueryContext* qctx,
                                           Expression* initialExpr,
                                           PlanNode* dep,
                                           const std::string& inputVar,
                                           SubPlan& plan) {
    auto columns = qctx->objPool()->add(new YieldColumns);
    auto* var = qctx->symTable()->getVar(inputVar);
    Expression* vidExpr = initialExprOrEdgeDstExpr(initialExpr, var->colNames.back());
    columns->addColumn(new YieldColumn(vidExpr));
    auto project = Project::make(qctx, dep, columns);
    project->setInputVar(inputVar);
    project->setColNames({kVid});
    auto dedup = Dedup::make(qctx, project);
    dedup->setColNames({kVid});

    plan.root = dedup;
}

Expression* MatchSolver::initialExprOrEdgeDstExpr(Expression* initialExpr,
                                                  const std::string& vidCol) {
    if (initialExpr != nullptr) {
        return initialExpr;
    } else {
        return getEndVidInPath(vidCol);
    }
}

Expression* MatchSolver::getEndVidInPath(const std::string& colName) {
    // expr: __Project_2[-1] => path
    auto columnExpr = ExpressionUtils::inputPropExpr(colName);
    // expr: endNode(path) => vn
    auto args = std::make_unique<ArgumentList>();
    args->addArgument(std::move(columnExpr));
    auto endNode = std::make_unique<FunctionCallExpression>("endNode", args.release());
    // expr: en[_dst] => dst vid
    auto vidExpr = std::make_unique<ConstantExpression>(kVid);
    return new AttributeExpression(endNode.release(), vidExpr.release());
}

Expression* MatchSolver::getStartVidInPath(const std::string& colName) {
    // expr: __Project_2[0] => path
    auto columnExpr = ExpressionUtils::inputPropExpr(colName);
    // expr: startNode(path) => v1
    auto args = std::make_unique<ArgumentList>();
    args->addArgument(std::move(columnExpr));
    auto firstVertexExpr = std::make_unique<FunctionCallExpression>("startNode", args.release());
    // expr: v1[_vid] => vid
    return new AttributeExpression(firstVertexExpr.release(), new ConstantExpression(kVid));
}

PlanNode* MatchSolver::filtPathHasSameEdge(PlanNode* input,
                                           const std::string& column,
                                           QueryContext* qctx) {
    auto args = std::make_unique<ArgumentList>();
    args->addArgument(ExpressionUtils::inputPropExpr(column));
    auto fnCall = std::make_unique<FunctionCallExpression>("hasSameEdgeInPath", args.release());
    auto pool = qctx->objPool();
    auto cond = pool->makeAndAdd<UnaryExpression>(Expression::Kind::kUnaryNot, fnCall.release());
    auto filter = Filter::make(qctx, input, cond);
    filter->setColNames(input->colNames());
    return filter;
}

Status MatchSolver::appendFetchVertexPlan(const Expression* nodeFilter,
                                          const SpaceInfo& space,
                                          QueryContext* qctx,
                                          Expression* initialExpr,
                                          SubPlan& plan) {
    return appendFetchVertexPlan(
        nodeFilter, space, qctx, initialExpr, plan.root->outputVar(), plan);
}

Status MatchSolver::appendFetchVertexPlan(const Expression* nodeFilter,
                                          const SpaceInfo& space,
                                          QueryContext* qctx,
                                          Expression* initialExpr,
                                          std::string inputVar,
                                          SubPlan& plan) {
    // [Project && Dedup]
    extractAndDedupVidColumn(qctx, initialExpr, plan.root, inputVar, plan);
    auto srcExpr = ExpressionUtils::inputPropExpr(kVid);
    // [Get vertices]
    auto props = SchemaUtil::getAllVertexProp(qctx, space, true);
    NG_RETURN_IF_ERROR(props);
    auto gv = GetVertices::make(qctx,
                                plan.root,
                                space.id,
                                qctx->objPool()->add(srcExpr.release()),
                                std::move(props).value(),
                                {});

    PlanNode* root = gv;
    if (nodeFilter != nullptr) {
        auto* newFilter = MatchSolver::rewriteLabel2Vertex(nodeFilter);
        qctx->objPool()->add(newFilter);
        root = Filter::make(qctx, root, newFilter);
    }

    // Normalize all columns to one
    auto columns = qctx->objPool()->add(new YieldColumns);
    auto pathExpr = std::make_unique<PathBuildExpression>();
    pathExpr->add(std::make_unique<VertexExpression>());
    columns->addColumn(new YieldColumn(pathExpr.release()));
    plan.root = Project::make(qctx, root, columns);
    plan.root->setColNames({kPathStr});
    return Status::OK();
}

}   // namespace graph
}   // namespace nebula
