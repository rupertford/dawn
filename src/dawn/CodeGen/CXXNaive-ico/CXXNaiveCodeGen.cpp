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

#include "dawn/CodeGen/CXXNaive-ico/CXXNaiveCodeGen.h"
#include "dawn/CodeGen/CXXNaive-ico/ASTStencilBody.h"
#include "dawn/CodeGen/CXXNaive-ico/ASTStencilDesc.h"
#include "dawn/CodeGen/CXXUtil.h"
#include "dawn/CodeGen/CodeGenProperties.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/Assert.h"
#include "dawn/Support/Logging.h"
#include "dawn/Support/StringUtil.h"
#include <algorithm>
#include <vector>

namespace dawn {
namespace codegen {
namespace cxxnaiveico {

// static std::string makeLoopImpl(const iir::Extent extent, const std::string& dim,
// const std::string& lower, const std::string& upper,
// const std::string& comparison, const std::string& increment) {
// return Twine("for(int " + dim + " = " + lower + "+" + std::to_string(extent.Minus) + "; " + dim +
// " " + comparison + " " + upper + "+" + std::to_string(extent.Plus) + "; " +
// increment + dim + ")")
// .str();
// }

// static std::string makeIntervalBound(const std::string dom, iir::Interval const& interval,
// iir::Interval::Bound bound) {
// return interval.levelIsEnd(bound)
// ? "( " + dom + ".ksize() == 0 ? 0 : (" + dom + ".ksize() - " + dom +
// ".kplus() - 1)) + " + std::to_string(interval.offset(bound))
// : std::to_string(interval.bound(bound));
// }

// static std::string makeKLoop(const std::string dom, bool isBackward,
// iir::Interval const& interval) {

// const std::string lower = makeIntervalBound(dom, interval, iir::Interval::Bound::lower);
// const std::string upper = makeIntervalBound(dom, interval, iir::Interval::Bound::upper);

// return isBackward ? makeLoopImpl(iir::Extent{}, "k", upper, lower, ">=", "--")
// : makeLoopImpl(iir::Extent{}, "k", lower, upper, "<=", "++");
// }

CXXNaiveIcoCodeGen::CXXNaiveIcoCodeGen(stencilInstantiationContext& ctx, DiagnosticsEngine& engine,
                                       int maxHaloPoint)
    : CodeGen(ctx, engine, maxHaloPoint) {}

CXXNaiveIcoCodeGen::~CXXNaiveIcoCodeGen() {}

std::string CXXNaiveIcoCodeGen::generateStencilInstantiation(
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation) {
  using namespace codegen;

  std::stringstream ssSW;

  Namespace dawnNamespace("dawn_generated", ssSW);
  Namespace cxxnaiveNamespace("cxxnaiveico", ssSW);

  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  Class StencilWrapperClass(stencilInstantiation->getName(), ssSW);
  StencilWrapperClass.changeAccessibility("private");

  CodeGenProperties codeGenProperties = computeCodeGenProperties(stencilInstantiation.get());

  // generateStencilFunctions(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  generateStencilClasses(stencilInstantiation, StencilWrapperClass, codeGenProperties);

  generateStencilWrapperMembers(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  generateStencilWrapperCtr(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  generateGlobalsAPI(*stencilInstantiation, StencilWrapperClass, globalsMap, codeGenProperties);

  generateStencilWrapperRun(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  StencilWrapperClass.commit();

  cxxnaiveNamespace.commit();
  dawnNamespace.commit();

  return ssSW.str();
}

void CXXNaiveIcoCodeGen::generateStencilWrapperRun(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    const CodeGenProperties& codeGenProperties) const {

  // Generate the run method by generate code for the stencil description AST
  MemberFunction RunMethod = stencilWrapperClass.addMemberFunction("void", "run", "");

  RunMethod.finishArgs();

  // generate the control flow code executing each inner stencil
  ASTStencilDesc stencilDescCGVisitor(stencilInstantiation->getMetaData(), codeGenProperties);
  stencilDescCGVisitor.setIndent(RunMethod.getIndent());
  for(const auto& statement :
      stencilInstantiation->getIIR()->getControlFlowDescriptor().getStatements()) {
    statement->accept(stencilDescCGVisitor);
    RunMethod.addStatement(stencilDescCGVisitor.getCodeAndResetStream());
  }

  RunMethod.commit();
}
void CXXNaiveIcoCodeGen::generateStencilWrapperCtr(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    const CodeGenProperties& codeGenProperties) const {

  const auto& stencils = stencilInstantiation->getStencils();
  const auto& metadata = stencilInstantiation->getMetaData();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  // Generate stencil wrapper constructor
  const auto& APIFields = metadata.getAccessesOfType<iir::FieldAccessType::FAT_APIField>();
  auto StencilWrapperConstructor = stencilWrapperClass.addConstructor();

  StencilWrapperConstructor.addArg("const Mesh& mesh");

  std::string ctrArgs("(dom");
  for(auto APIfieldID : APIFields) {
    StencilWrapperConstructor.addArg("Field<double>& " +
                                     metadata.getFieldNameFromAccessID(APIfieldID));
    ctrArgs += "," + metadata.getFieldNameFromAccessID(APIfieldID);
  }

  // add the ctr initialization of each stencil
  for(const auto& stencilPtr : stencils) {
    iir::Stencil& stencil = *stencilPtr;
    if(stencil.isEmpty())
      continue;

    const auto stencilFields = support::orderMap(stencil.getFields());

    const std::string stencilName =
        codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil.getStencilID());

    std::string initCtr = "m_" + stencilName;

    initCtr += "(mesh";
    if(!globalsMap.empty()) {
      initCtr += ",m_globals";
    }
    for(const auto& fieldInfoPair : stencilFields) {
      const auto& fieldInfo = fieldInfoPair.second;
      if(fieldInfo.IsTemporary)
        continue;
      initCtr += "," + (metadata.isAccessType(iir::FieldAccessType::FAT_InterStencilTemporary,
                                              fieldInfo.field.getAccessID())
                            ? ("m_" + fieldInfo.Name)
                            : (fieldInfo.Name));
    }
    initCtr += ")";
    StencilWrapperConstructor.addInit(initCtr);
  }

  if(metadata.hasAccessesOfType<iir::FieldAccessType::FAT_InterStencilTemporary>()) {
    std::vector<std::string> tempFields;
    for(auto accessID :
        metadata.getAccessesOfType<iir::FieldAccessType::FAT_InterStencilTemporary>()) {
      tempFields.push_back(metadata.getFieldNameFromAccessID(accessID));
    }
    addTmpStorageInitStencilWrapperCtr(StencilWrapperConstructor, stencils, tempFields);
  }

  StencilWrapperConstructor.commit();
}
void CXXNaiveIcoCodeGen::generateStencilWrapperMembers(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    CodeGenProperties& codeGenProperties) const {

  const auto& metadata = stencilInstantiation->getMetaData();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  stencilWrapperClass.addMember("static constexpr const char* s_name =",
                                Twine("\"") + stencilWrapperClass.getName() + Twine("\""));

  if(!globalsMap.empty()) {
    stencilWrapperClass.addMember("globals", "m_globals");
  }

  for(auto stencilPropertiesPair :
      codeGenProperties.stencilProperties(StencilContext::SC_Stencil)) {
    stencilWrapperClass.addMember(stencilPropertiesPair.second->name_,
                                  "m_" + stencilPropertiesPair.second->name_);
  }

  stencilWrapperClass.changeAccessibility("public");
  stencilWrapperClass.addCopyConstructor(Class::Deleted);

  stencilWrapperClass.addComment("Members");
  //
  // Members
  //
  // Define allocated memebers if necessary
  if(metadata.hasAccessesOfType<iir::FieldAccessType::FAT_InterStencilTemporary>()) {
    stencilWrapperClass.addMember(c_gtc() + "meta_data_t", "m_meta_data");

    for(int AccessID :
        metadata.getAccessesOfType<iir::FieldAccessType::FAT_InterStencilTemporary>())
      stencilWrapperClass.addMember(c_gtc() + "storage_t",
                                    "m_" + metadata.getFieldNameFromAccessID(AccessID));
  }
}
void CXXNaiveIcoCodeGen::generateStencilClasses(
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    Class& stencilWrapperClass, const CodeGenProperties& codeGenProperties) const {

  const auto& stencils = stencilInstantiation->getStencils();
  // const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  // Stencil members:
  // generate the code for each of the stencils
  for(const auto& stencil : stencils) {

    std::string stencilName =
        codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil->getStencilID());

    if(stencil->isEmpty())
      continue;

    // fields used in the stencil
    const auto stencilFields = support::orderMap(stencil->getFields());

    auto nonTempFields = makeRange(
        stencilFields, std::function<bool(std::pair<int, iir::Stencil::FieldInfo> const&)>(
                           [](std::pair<int, iir::Stencil::FieldInfo> const& p) {
                             return !p.second.IsTemporary;
                           }));
    auto tempFields = makeRange(
        stencilFields,
        std::function<bool(std::pair<int, iir::Stencil::FieldInfo> const&)>(
            [](std::pair<int, iir::Stencil::FieldInfo> const& p) { return p.second.IsTemporary; }));

    Structure StencilClass = stencilWrapperClass.addStruct(stencilName);

    ASTStencilBody stencilBodyCXXVisitor(stencilInstantiation->getMetaData(),
                                         StencilContext::SC_Stencil);

    // StencilClass.addComment("Temporary storages");
    // addTempStorageTypedef(StencilClass, *stencil);

    // StencilClass.addMember("const " + c_gtc() + "domain&", "m_dom");

    // if(!globalsMap.empty()) {
    //   StencilClass.addMember("const globals&", "m_globals");
    // }

    StencilClass.addMember("Mesh const&", "m_mesh");
    for(auto fieldIt : nonTempFields) {
      StencilClass.addMember("Field<double>&", "m_" + fieldIt.second.Name);
    }

    // addTmpStorageDeclaration(StencilClass, tempFields);

    StencilClass.changeAccessibility("public");

    auto stencilClassCtr = StencilClass.addConstructor();

    stencilClassCtr.addArg("Mesh const& mesh");
    for(auto fieldIt : nonTempFields) {
      stencilClassCtr.addArg("Field<double>&" + fieldIt.second.Name);
    }

    // stencilClassCtr.addInit("m_dom(dom_)");
    // if(!globalsMap.empty()) {
    //   stencilClassCtr.addArg("m_globals(globals_)");
    // }

    stencilClassCtr.addInit("m_mesh(mesh)");
    for(auto fieldIt : nonTempFields) {
      stencilClassCtr.addInit("m_" + fieldIt.second.Name + "(" + fieldIt.second.Name + ")");
    }

    // addTmpStorageInit(stencilClassCtr, *stencil, tempFields);
    stencilClassCtr.commit();

    // virtual dtor
    MemberFunction stencilClassDtr = StencilClass.addDestructor(false);
    stencilClassDtr.startBody();
    stencilClassDtr.commit();

    // synchronize storages method
    MemberFunction syncStoragesMethod = StencilClass.addMemberFunction("void", "sync_storages", "");
    syncStoragesMethod.startBody();

    // for(auto fieldIt : nonTempFields) {
    //   syncStoragesMethod.addStatement("m_" + fieldIt.second.Name + ".sync()");
    // }

    syncStoragesMethod.commit();

    //
    // Run-Method
    //
    MemberFunction StencilRunMethod = StencilClass.addMemberFunction("void", "run", "");
    StencilRunMethod.startBody();

    // StencilRunMethod.addStatement("sync_storages()");
    for(const auto& multiStagePtr : stencil->getChildren()) {

      StencilRunMethod.ss() << "{\n";

      const iir::MultiStage& multiStage = *multiStagePtr;

      // create all the data views
      const auto& usedFields = multiStage.getFields();
      for(const auto& usedField : usedFields) {
        auto field = stencilFields.at(usedField.first);
        // auto storageName = field.IsTemporary ? "tmp_storage_t" :
        // StencilTemplates[usedField.first]; StencilRunMethod.addStatement(c_gt() + "data_view<" +
        // storageName + "> " + field.Name +
        //                               "= " + c_gt() + "make_host_view(m_" + field.Name + ")");
        // StencilRunMethod.addStatement("std::array<int,3> " + field.Name + "_offsets{0,0,0}");
      }
      auto intervals_set = multiStage.getIntervals();
      std::vector<iir::Interval> intervals_v;
      std::copy(intervals_set.begin(), intervals_set.end(), std::back_inserter(intervals_v));

      // compute the partition of the intervals
      auto partitionIntervals = iir::Interval::computePartition(intervals_v);
      if((multiStage.getLoopOrder() == iir::LoopOrderKind::LK_Backward))
        std::reverse(partitionIntervals.begin(), partitionIntervals.end());

      for(auto interval : partitionIntervals) {

        // for each interval, we generate naive nested loops
        for(const auto& stagePtr : multiStage.getChildren()) {
          const iir::Stage& stage = *stagePtr;

          StencilRunMethod.addBlockStatement("for (auto const& t : getTriangles(m_mesh))", [&]() {
            // Generate Do-Method
            for(const auto& doMethodPtr : stage.getChildren()) {
              const iir::DoMethod& doMethod = *doMethodPtr;
              if(!doMethod.getInterval().overlaps(interval))
                continue;
              for(const auto& statementAccessesPair : doMethod.getChildren()) {
                statementAccessesPair->getStatement()->accept(stencilBodyCXXVisitor);
                StencilRunMethod << stencilBodyCXXVisitor.getCodeAndResetStream();
              }
            }
          });
        }
      }
      StencilRunMethod.ss() << "}";
    }
    StencilRunMethod.addStatement("sync_storages()");
    StencilRunMethod.commit();
  }
}

void CXXNaiveIcoCodeGen::generateStencilFunctions(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    const CodeGenProperties& codeGenProperties) const {

  const auto& metadata = stencilInstantiation->getMetaData();
  // stencil functions
  //
  // Generate stencil functions code for stencils instantiated by this stencil
  //
  std::unordered_set<std::string> generatedStencilFun;
  size_t idx = 0;
  for(const auto& stencilFun : metadata.getStencilFunctionInstantiations()) {
    std::string stencilFunName = iir::StencilFunctionInstantiation::makeCodeGenName(*stencilFun);
    if(generatedStencilFun.emplace(stencilFunName).second) {

      // Field declaration
      const auto& fields = stencilFun->getCalleeFields();

      if(fields.empty()) {
        DiagnosticsBuilder diag(DiagnosticsKind::Error,
                                stencilInstantiation->getMetaData().getStencilLocation());
        diag << "no storages referenced in stencil '" << stencilInstantiation->getName()
             << "', this would result in invalid gridtools code";
        diagEngine.report(diag);
        return;
      }

      MemberFunction stencilFunMethod = stencilWrapperClass.addMemberFunction(
          std::string("static ") + (stencilFun->hasReturn() ? "double" : "void"), stencilFunName);

      if(fields.empty() && !stencilFun->hasReturn()) {
        DiagnosticsBuilder diag(DiagnosticsKind::Error, stencilFun->getStencilFunction()->Loc);
        diag << "no storages referenced in stencil function '" << stencilFun->getName()
             << "', this would result in invalid gridtools code";
        diagEngine.report(diag);
        return;
      }

      // Each stencil function call will pass the (i,j,k) position
      stencilFunMethod.addArg("const int i");
      stencilFunMethod.addArg("const int j");
      stencilFunMethod.addArg("const int k");

      const auto& stencilProp = codeGenProperties.getStencilProperties(
          StencilContext::SC_StencilFunction, stencilFunName);

      // We need to generate the arguments in order (of the fn call expr)
      for(const auto& exprArg : stencilFun->getArguments()) {
        if(exprArg->Kind != sir::StencilFunctionArg::ArgumentKind::AK_Field)
          continue;
        const std::string argName = exprArg->Name;

        DAWN_ASSERT(stencilProp->paramNameToType_.count(argName));
        const std::string argType = stencilProp->paramNameToType_[argName];
        // each parameter being passed to a stencil function, is wrapped around the param_wrapper
        // that contains the storage and the offset, in order to resolve offset passed to the
        // storage during the function call. For example:
        // fn_call(v(i+1), v(j-1))
        stencilFunMethod.addArg("param_wrapper<" + c_gt() + "data_view<" + argType + ">> pw_" +
                                argName);
      }

      // add global parameter
      if(stencilFun->hasGlobalVariables()) {
        stencilFunMethod.addArg("const globals& m_globals");
      }
      ASTStencilBody stencilBodyCXXVisitor(stencilInstantiation->getMetaData(),
                                           StencilContext::SC_StencilFunction);

      stencilFunMethod.startBody();

      for(std::size_t m = 0; m < fields.size(); ++m) {

        std::string paramName =
            stencilFun->getOriginalNameFromCallerAccessID(fields[m].getAccessID());

        stencilFunMethod << c_gt() << "data_view<StorageType" + std::to_string(m) + "> "
                         << paramName << " = pw_" << paramName << ".dview_;";
        stencilFunMethod << "auto " << paramName << "_offsets = pw_" << paramName << ".offsets_;";
      }
      stencilBodyCXXVisitor.setCurrentStencilFunction(stencilFun);
      stencilBodyCXXVisitor.setIndent(stencilFunMethod.getIndent());
      for(const auto& statementAccessesPair : stencilFun->getStatementAccessesPairs()) {
        statementAccessesPair->getStatement()->accept(stencilBodyCXXVisitor);
        stencilFunMethod.indentStatment();
        stencilFunMethod << stencilBodyCXXVisitor.getCodeAndResetStream();
      }

      stencilFunMethod.commit();
    }
    idx++;
  }
}

std::unique_ptr<TranslationUnit> CXXNaiveIcoCodeGen::generateCode() {
  DAWN_LOG(INFO) << "Starting code generation for GTClang ...";

  // Generate code for StencilInstantiations
  std::map<std::string, std::string> stencils;
  for(const auto& nameStencilCtxPair : context_) {
    std::string code = generateStencilInstantiation(nameStencilCtxPair.second);
    if(code.empty())
      return nullptr;
    stencils.emplace(nameStencilCtxPair.first, std::move(code));
  }

  std::string globals = generateGlobals(context_, "dawn_generated", "cxxnaiveico");

  std::vector<std::string> ppDefines;
  ppDefines.push_back("#define GRIDTOOLS_CLANG_GENERATED 1");
  ppDefines.push_back("#define GRIDTOOLS_CLANG_BACKEND_T CXXNAIVEICO");
  ppDefines.push_back("#include \"my_interface.hpp\"");
  ppDefines.push_back("using namespace MyInterface;");
  DAWN_LOG(INFO) << "Done generating code";

  std::string filename = generateFileName(context_);
  return std::make_unique<TranslationUnit>(filename, std::move(ppDefines), std::move(stencils),
                                           std::move(globals));
}

} // namespace cxxnaiveico
} // namespace codegen
} // namespace dawn
