//===--------------------------------------------------------------------------------*- C++ -*-===//
//                          _
//                         | |
//                       __| | __ ___      ___ ___
//                      / _` |/ _` \ \ /\ / / '_  |
//                     | (_| | (_| |\ V  V /| | | |
//                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#include "dawn/Optimizer/Replacing.h"
#include "dawn/IIR/ASTStmt.h"
#include "dawn/IIR/ASTUtil.h"
#include "dawn/IIR/ASTVisitor.h"
#include "dawn/IIR/InstantiationHelper.h"
#include "dawn/IIR/StencilInstantiation.h"
#include <unordered_map>

namespace dawn {

namespace {

/// @brief Get all field and variable accesses identifier by `AccessID`
class GetFieldAndVarAccesses : public iir::ASTVisitorForwarding {
  const iir::StencilMetaInformation& metadata_;
  int AccessID_;

  std::vector<std::shared_ptr<iir::FieldAccessExpr>> fieldAccessExprToReplace_;
  std::vector<std::shared_ptr<iir::VarAccessExpr>> varAccessesToReplace_;

public:
  GetFieldAndVarAccesses(iir::StencilMetaInformation& metadata, int AccessID)
      : metadata_(metadata), AccessID_(AccessID) {}

  void visit(const std::shared_ptr<iir::VarAccessExpr>& expr) override {
    if(metadata_.getAccessIDFromExpr(expr) == AccessID_)
      varAccessesToReplace_.emplace_back(expr);
  }

  void visit(const std::shared_ptr<iir::FieldAccessExpr>& expr) override {
    if(metadata_.getAccessIDFromExpr(expr) == AccessID_)
      fieldAccessExprToReplace_.emplace_back(expr);
  }

  std::vector<std::shared_ptr<iir::VarAccessExpr>>& getVarAccessesToReplace() {
    return varAccessesToReplace_;
  }

  std::vector<std::shared_ptr<iir::FieldAccessExpr>>& getFieldAccessExprToReplace() {
    return fieldAccessExprToReplace_;
  }

  void reset() {
    fieldAccessExprToReplace_.clear();
    varAccessesToReplace_.clear();
  }
};

} // anonymous namespace

void replaceFieldWithVarAccessInStmts(
    iir::StencilMetaInformation& metadata, iir::Stencil* stencil, int AccessID,
    const std::string& varname,
    ArrayRef<std::unique_ptr<iir::StatementAccessesPair>> statementAccessesPairs) {

  GetFieldAndVarAccesses visitor(metadata, AccessID);
  for(const auto& statementAccessesPair : statementAccessesPairs) {
    visitor.reset();

    const auto& stmt = statementAccessesPair->getStatement();
    stmt->accept(visitor);

    for(auto& oldExpr : visitor.getFieldAccessExprToReplace()) {
      auto newExpr = std::make_shared<iir::VarAccessExpr>(varname);

      iir::replaceOldExprWithNewExprInStmt(stmt, oldExpr, newExpr);

      metadata.insertExprToAccessID(newExpr, AccessID);
      metadata.eraseExprToAccessID(oldExpr);
    }
  }
}

void replaceVarWithFieldAccessInStmts(
    iir::StencilMetaInformation& metadata, iir::Stencil* stencil, int AccessID,
    const std::string& fieldname,
    ArrayRef<std::unique_ptr<iir::StatementAccessesPair>> statementAccessesPairs) {

  GetFieldAndVarAccesses visitor(metadata, AccessID);
  for(const auto& statementAccessesPair : statementAccessesPairs) {
    visitor.reset();

    const auto& stmt = statementAccessesPair->getStatement();
    stmt->accept(visitor);

    for(auto& oldExpr : visitor.getVarAccessesToReplace()) {
      auto newExpr = std::make_shared<iir::FieldAccessExpr>(fieldname);

      iir::replaceOldExprWithNewExprInStmt(stmt, oldExpr, newExpr);

      metadata.insertExprToAccessID(newExpr, AccessID);
      metadata.eraseExprToAccessID(oldExpr);
    }
  }
}

namespace {

/// @brief Get all field and variable accesses identifier by `AccessID`
class GetStencilCalls : public iir::ASTVisitorForwarding {
  const std::shared_ptr<iir::StencilInstantiation>& instantiation_;
  int StencilID_;

  std::vector<std::shared_ptr<iir::StencilCallDeclStmt>> stencilCallsToReplace_;

public:
  GetStencilCalls(const std::shared_ptr<iir::StencilInstantiation>& instantiation, int StencilID)
      : instantiation_(instantiation), StencilID_(StencilID) {}

  void visit(const std::shared_ptr<iir::StencilCallDeclStmt>& stmt) override {
    if(instantiation_->getMetaData().getStencilIDFromStencilCallStmt(stmt) == StencilID_)
      stencilCallsToReplace_.emplace_back(stmt);
  }

  std::vector<std::shared_ptr<iir::StencilCallDeclStmt>>& getStencilCallsToReplace() {
    return stencilCallsToReplace_;
  }

  void reset() { stencilCallsToReplace_.clear(); }
};

} // anonymous namespace

void replaceStencilCalls(const std::shared_ptr<iir::StencilInstantiation>& instantiation,
                         int oldStencilID, const std::vector<int>& newStencilIDs) {
  GetStencilCalls visitor(instantiation, oldStencilID);

  for(auto& stmt : instantiation->getIIR()->getControlFlowDescriptor().getStatements()) {
    visitor.reset();

    stmt->accept(visitor);
    for(auto& oldStencilCall : visitor.getStencilCallsToReplace()) {

      // Create the new stencils
      std::vector<std::shared_ptr<iir::StencilCallDeclStmt>> newStencilCalls;
      for(int StencilID : newStencilIDs) {
        auto placeholderStencil = std::make_shared<ast::StencilCall>(
            iir::InstantiationHelper::makeStencilCallCodeGenName(StencilID));
        newStencilCalls.push_back(iir::makeStencilCallDeclStmt(placeholderStencil));
      }

      // Bundle all the statements in a block statements
      auto newBlockStmt = iir::makeBlockStmt();
      std::copy(newStencilCalls.begin(), newStencilCalls.end(),
                std::back_inserter(newBlockStmt->getStatements()));

      if(oldStencilCall == stmt) {
        // Replace the the statement directly
        DAWN_ASSERT(visitor.getStencilCallsToReplace().size() == 1);
        stmt = newBlockStmt;
      } else {
        // Recursively replace the statement
        iir::replaceOldStmtWithNewStmtInStmt(stmt, oldStencilCall, newBlockStmt);
      }

      auto& metadata = instantiation->getMetaData();
      metadata.eraseStencilCallStmt(oldStencilCall);
      for(std::size_t i = 0; i < newStencilIDs.size(); ++i) {
        metadata.addStencilCallStmt(newStencilCalls[i], newStencilIDs[i]);
      }
    }
  }
}

} // namespace dawn
