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

#include "GenerateInMemoryStencils.h"

#include "dawn/Compiler/DawnCompiler.h"
#include "dawn/IIR/ASTStmt.h"
#include "dawn/IIR/ASTUtil.h"
#include "dawn/IIR/FieldAccessMetadata.h"
#include "dawn/IIR/IIR.h"
#include "dawn/IIR/IIRNodeIterator.h"
#include "dawn/IIR/InstantiationHelper.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/IIR/StencilMetaInformation.h"
#include "dawn/Optimizer/AccessComputation.h"
#include "dawn/Optimizer/OptimizerContext.h"
#include "dawn/Optimizer/PassComputeStageExtents.h"
#include "dawn/Optimizer/PassSetStageName.h"
#include "dawn/Optimizer/PassTemporaryType.h"
#include "dawn/Optimizer/StatementMapper.h"
#include "dawn/SIR/ASTFwd.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Serialization/IIRSerializer.h"
#include "dawn/Support/Logging.h"
#include "dawn/Support/STLExtras.h"

using namespace dawn;

std::shared_ptr<iir::StencilInstantiation>
createCopyStencilIIRInMemory(OptimizerContext& optimizer) {
  auto target = std::make_shared<iir::StencilInstantiation>(*optimizer.getSIR()->GlobalVariableMap,
                                                            optimizer.getSIR()->StencilFunctions);

  ///////////////// Generation of the IIR
  sir::Attr attributes;
  int stencilID = target->nextUID();
  target->getIIR()->insertChild(
      std::make_unique<iir::Stencil>(target->getMetaData(), attributes, stencilID),
      target->getIIR());
  const auto& IIRStencil = target->getIIR()->getChild(0);
  // One Multistage with a parallel looporder
  IIRStencil->insertChild(
      std::make_unique<iir::MultiStage>(target->getMetaData(), iir::LoopOrderKind::LK_Parallel));
  const auto& IIRMSS = (IIRStencil)->getChild(0);
  IIRMSS->setID(target->nextUID());

  // Create one stage inside the MSS
  IIRMSS->insertChild(std::make_unique<iir::Stage>(target->getMetaData(), target->nextUID()));
  const auto& IIRStage = IIRMSS->getChild(0);

  // Create one doMethod inside the Stage that spans the full domain
  IIRStage->insertChild(std::make_unique<iir::DoMethod>(
      iir::Interval(sir::Interval{sir::Interval::Start, sir::Interval::End}),
      target->getMetaData()));
  const auto& IIRDoMethod = IIRStage->getChild(0);
  IIRDoMethod->setID(target->nextUID());

  // create the StmtAccessPair
  auto sirInField = std::make_shared<sir::Field>("in_field");
  sirInField->IsTemporary = false;
  sirInField->fieldDimensions = Array3i{1, 1, 1};
  auto sirOutField = std::make_shared<sir::Field>("out_field");
  sirOutField->IsTemporary = false;
  sirOutField->fieldDimensions = Array3i{1, 1, 1};

  auto lhs = std::make_shared<ast::FieldAccessExpr>(sirOutField->Name);
  lhs->setID(target->nextUID());
  auto rhs = std::make_shared<ast::FieldAccessExpr>(sirInField->Name);
  rhs->setID(target->nextUID());

  int in_fieldID = target->getMetaData().addField(iir::FieldAccessType::FAT_APIField,
                                                  sirInField->Name, sirInField->fieldDimensions);
  int out_fieldID = target->getMetaData().addField(iir::FieldAccessType::FAT_APIField,
                                                   sirOutField->Name, sirOutField->fieldDimensions);

  auto expr = std::make_shared<ast::AssignmentExpr>(lhs, rhs);
  expr->setID(target->nextUID());
  auto stmt = iir::makeExprStmt(expr);
  stmt->setID(target->nextUID());
  auto insertee = std::make_unique<iir::StatementAccessesPair>(stmt);

  // Add the accesses to the Pair:
  std::shared_ptr<iir::Accesses> callerAccesses = std::make_shared<iir::Accesses>();
  callerAccesses->addWriteExtent(out_fieldID, iir::Extents{0, 0, 0, 0, 0, 0});
  callerAccesses->addReadExtent(in_fieldID, iir::Extents{0, 0, 0, 0, 0, 0});
  insertee->setCallerAccesses(callerAccesses);
  // And add the StmtAccesspair to it
  IIRDoMethod->insertChild(std::move(insertee));
  IIRDoMethod->updateLevel();

  // Add the control flow descriptor to the IIR
  auto stencilCall = std::make_shared<ast::StencilCall>("generatedDriver");
  stencilCall->Args.push_back(sirInField->Name);
  stencilCall->Args.push_back(sirOutField->Name);
  auto placeholderStencil = std::make_shared<ast::StencilCall>(
      iir::InstantiationHelper::makeStencilCallCodeGenName(stencilID));
  auto stencilCallDeclStmt = iir::makeStencilCallDeclStmt(placeholderStencil);
  // Register the call and set it as a replacement for the next vertical region
  target->getMetaData().addStencilCallStmt(stencilCallDeclStmt, stencilID);
  target->getIIR()->getControlFlowDescriptor().insertStmt(stencilCallDeclStmt);

  ///////////////// Generation of the Metadata

  target->getMetaData().insertExprToAccessID(lhs, out_fieldID);
  target->getMetaData().insertExprToAccessID(rhs, in_fieldID);
  target->getMetaData().setStencilname("generated");

  for(const auto& MS : iterateIIROver<iir::MultiStage>(*(target->getIIR()))) {
    MS->update(iir::NodeUpdateType::levelAndTreeAbove);
  }
  // Iterate all statements (top -> bottom)
  for(const auto& stagePtr : iterateIIROver<iir::Stage>(*(target->getIIR()))) {
    iir::Stage& stage = *stagePtr;
    for(const auto& doMethod : stage.getChildren()) {
      doMethod->update(iir::NodeUpdateType::level);
    }
    stage.update(iir::NodeUpdateType::level);
  }
  for(const auto& MSPtr : iterateIIROver<iir::Stage>(*(target->getIIR()))) {
    MSPtr->update(iir::NodeUpdateType::levelAndTreeAbove);
  }

  return target;
}

std::shared_ptr<iir::StencilInstantiation>
createLapStencilIIRInMemory(OptimizerContext& optimizer) {
  auto target = std::make_shared<iir::StencilInstantiation>(*optimizer.getSIR()->GlobalVariableMap,
                                                            optimizer.getSIR()->StencilFunctions);

  ///////////////// Generation of the IIR
  sir::Attr attributes;
  int stencilID = target->nextUID();
  target->getIIR()->insertChild(
      std::make_unique<iir::Stencil>(target->getMetaData(), attributes, stencilID),
      target->getIIR());
  const auto& IIRStencil = target->getIIR()->getChild(0);
  // One Multistage with a parallel looporder
  IIRStencil->insertChild(
      std::make_unique<iir::MultiStage>(target->getMetaData(), iir::LoopOrderKind::LK_Parallel));
  const auto& IIRMSS = (IIRStencil)->getChild(0);
  IIRMSS->setID(target->nextUID());

  auto IIRStage1 = std::make_unique<iir::Stage>(target->getMetaData(), target->nextUID());
  auto IIRStage2 = std::make_unique<iir::Stage>(target->getMetaData(), target->nextUID());

  IIRStage1->setExtents(iir::Extents(-1, +1, -1, +1, 0, 0));

  // Create one doMethod inside the Stage that spans the full domain
  IIRStage1->insertChild(std::make_unique<iir::DoMethod>(
      iir::Interval(sir::Interval{sir::Interval::Start, sir::Interval::End}),
      target->getMetaData()));
  const auto& IIRDoMethod1 = IIRStage1->getChild(0);
  IIRDoMethod1->setID(target->nextUID());

  IIRStage2->insertChild(std::make_unique<iir::DoMethod>(
      iir::Interval(sir::Interval{sir::Interval::Start, sir::Interval::End}),
      target->getMetaData()));
  const auto& IIRDoMethod2 = IIRStage2->getChild(0);
  IIRDoMethod2->setID(target->nextUID());

  // Create two stages inside the MSS
  IIRMSS->insertChild(std::move(IIRStage1));
  IIRMSS->insertChild(std::move(IIRStage2));

  // create the StmtAccessPair
  auto sirInField = std::make_shared<sir::Field>("in");
  sirInField->IsTemporary = false;
  sirInField->fieldDimensions = Array3i{1, 1, 1};
  auto sirOutField = std::make_shared<sir::Field>("out");
  sirOutField->IsTemporary = false;
  sirOutField->fieldDimensions = Array3i{1, 1, 1};
  auto sirTmpField = std::make_shared<sir::Field>("tmp");
  sirOutField->IsTemporary = true;
  sirOutField->fieldDimensions = Array3i{1, 1, 1};

  auto lhsTmp = std::make_shared<ast::FieldAccessExpr>(sirTmpField->Name);
  lhsTmp->setID(target->nextUID());

  auto rhsInT1 = std::make_shared<ast::FieldAccessExpr>(sirInField->Name, Array3i{0, -2, 0});
  auto rhsInT2 = std::make_shared<ast::FieldAccessExpr>(sirInField->Name, Array3i{0, +2, 0});
  auto rhsInT3 = std::make_shared<ast::FieldAccessExpr>(sirInField->Name, Array3i{-2, 0, 0});
  auto rhsInT4 = std::make_shared<ast::FieldAccessExpr>(sirInField->Name, Array3i{+2, 0, 0});

  rhsInT1->setID(target->nextUID());
  rhsInT2->setID(target->nextUID());
  rhsInT3->setID(target->nextUID());
  rhsInT4->setID(target->nextUID());

  auto lhsOut = std::make_shared<ast::FieldAccessExpr>(sirOutField->Name);
  lhsOut->setID(target->nextUID());

  auto rhsTmpT1 = std::make_shared<ast::FieldAccessExpr>(sirTmpField->Name, Array3i{0, -1, 0});
  auto rhsTmpT2 = std::make_shared<ast::FieldAccessExpr>(sirTmpField->Name, Array3i{0, +1, 0});
  auto rhsTmpT3 = std::make_shared<ast::FieldAccessExpr>(sirTmpField->Name, Array3i{-1, 0, 0});
  auto rhsTmpT4 = std::make_shared<ast::FieldAccessExpr>(sirTmpField->Name, Array3i{+1, 0, 0});

  rhsTmpT1->setID(target->nextUID());
  rhsTmpT2->setID(target->nextUID());
  rhsTmpT3->setID(target->nextUID());
  rhsTmpT4->setID(target->nextUID());

  int inFieldID = target->getMetaData().addField(iir::FieldAccessType::FAT_APIField,
                                                 sirInField->Name, sirInField->fieldDimensions);
  int tmpFieldID = target->getMetaData().addField(iir::FieldAccessType::FAT_StencilTemporary,
                                                  sirTmpField->Name, sirTmpField->fieldDimensions);
  int outFieldID = target->getMetaData().addField(iir::FieldAccessType::FAT_APIField,
                                                  sirOutField->Name, sirOutField->fieldDimensions);

  auto plusIn1 = std::make_shared<ast::BinaryOperator>(rhsInT1, std::string("+"), rhsInT2);
  auto plusIn2 = std::make_shared<ast::BinaryOperator>(rhsInT3, std::string("+"), rhsInT4);
  auto plusIn3 = std::make_shared<ast::BinaryOperator>(plusIn1, std::string("+"), plusIn2);

  plusIn1->setID(target->nextUID());
  plusIn2->setID(target->nextUID());
  plusIn3->setID(target->nextUID());

  auto assignmentTmpIn = std::make_shared<ast::AssignmentExpr>(lhsTmp, plusIn3);
  assignmentTmpIn->setID(target->nextUID());

  auto stmt1 = iir::makeExprStmt(assignmentTmpIn);
  stmt1->setID(target->nextUID());
  auto insertee1 = std::make_unique<iir::StatementAccessesPair>(stmt1);

  // Add the accesses to the Pair:
  std::shared_ptr<iir::Accesses> callerAccesses1 = std::make_shared<iir::Accesses>();
  callerAccesses1->addWriteExtent(tmpFieldID, iir::Extents{0, 0, 0, 0, 0, 0});
  callerAccesses1->addReadExtent(inFieldID, iir::Extents{-2, 2, -2, 2, 0, 0});
  insertee1->setCallerAccesses(callerAccesses1);
  // And add the StmtAccesspair to it
  IIRDoMethod1->insertChild(std::move(insertee1));
  IIRDoMethod1->updateLevel();

  auto plusTmp1 = std::make_shared<ast::BinaryOperator>(rhsTmpT1, std::string("+"), rhsTmpT2);
  auto plusTmp2 = std::make_shared<ast::BinaryOperator>(rhsTmpT3, std::string("+"), rhsTmpT4);
  auto plusTmp3 = std::make_shared<ast::BinaryOperator>(plusTmp1, std::string("+"), plusTmp2);

  plusTmp1->setID(target->nextUID());
  plusTmp2->setID(target->nextUID());
  plusTmp3->setID(target->nextUID());

  auto assignmentOutTmp = std::make_shared<ast::AssignmentExpr>(lhsOut, plusTmp3);
  assignmentOutTmp->setID(target->nextUID());

  auto stmt2 = iir::makeExprStmt(assignmentOutTmp);
  stmt2->setID(target->nextUID());
  auto insertee2 = std::make_unique<iir::StatementAccessesPair>(stmt2);

  // Add the accesses to the Pair:
  std::shared_ptr<iir::Accesses> callerAccesses2 = std::make_shared<iir::Accesses>();
  callerAccesses2->addWriteExtent(outFieldID, iir::Extents{0, 0, 0, 0, 0, 0});
  callerAccesses2->addReadExtent(tmpFieldID, iir::Extents{-1, 1, -1, 1, 0, 0});
  insertee2->setCallerAccesses(callerAccesses2);
  // And add the StmtAccesspair to it
  IIRDoMethod2->insertChild(std::move(insertee2));
  IIRDoMethod2->updateLevel();

  // Add the control flow descriptor to the IIR
  auto stencilCall = std::make_shared<ast::StencilCall>("generatedDriver");
  stencilCall->Args.push_back(sirInField->Name);
  // stencilCall->Args.push_back(sirTmpField->Name);
  stencilCall->Args.push_back(sirOutField->Name);
  auto placeholderStencil = std::make_shared<ast::StencilCall>(
      iir::InstantiationHelper::makeStencilCallCodeGenName(stencilID));
  auto stencilCallDeclStmt = iir::makeStencilCallDeclStmt(placeholderStencil);
  // Register the call and set it as a replacement for the next vertical region
  target->getMetaData().addStencilCallStmt(stencilCallDeclStmt, stencilID);
  target->getIIR()->getControlFlowDescriptor().insertStmt(stencilCallDeclStmt);

  ///////////////// Generation of the Metadata

  target->getMetaData().insertExprToAccessID(lhsTmp, tmpFieldID);
  target->getMetaData().insertExprToAccessID(rhsInT1, inFieldID);
  target->getMetaData().insertExprToAccessID(rhsInT2, inFieldID);
  target->getMetaData().insertExprToAccessID(rhsInT3, inFieldID);
  target->getMetaData().insertExprToAccessID(rhsInT4, inFieldID);

  target->getMetaData().insertExprToAccessID(lhsOut, outFieldID);
  target->getMetaData().insertExprToAccessID(rhsTmpT1, tmpFieldID);
  target->getMetaData().insertExprToAccessID(rhsTmpT2, tmpFieldID);
  target->getMetaData().insertExprToAccessID(rhsTmpT3, tmpFieldID);
  target->getMetaData().insertExprToAccessID(rhsTmpT4, tmpFieldID);

  target->getMetaData().setStencilname("generated");

  for(const auto& MS : iterateIIROver<iir::MultiStage>(*(target->getIIR()))) {
    MS->update(iir::NodeUpdateType::levelAndTreeAbove);
  }
  // Iterate all statements (top -> bottom)
  for(const auto& stagePtr : iterateIIROver<iir::Stage>(*(target->getIIR()))) {
    iir::Stage& stage = *stagePtr;
    for(const auto& doMethod : stage.getChildren()) {
      doMethod->update(iir::NodeUpdateType::level);
    }
    stage.update(iir::NodeUpdateType::level);
  }
  for(const auto& MSPtr : iterateIIROver<iir::Stage>(*(target->getIIR()))) {
    MSPtr->update(iir::NodeUpdateType::levelAndTreeAbove);
  }

  return target;
}
