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

#ifndef DAWN_IIR_STENCILINSTANTIATION_H
#define DAWN_IIR_STENCILINSTANTIATION_H

#include "dawn/IIR/Accesses.h"
#include "dawn/IIR/IIR.h"
#include "dawn/IIR/Stencil.h"
#include "dawn/IIR/StencilFunctionInstantiation.h"
#include "dawn/IIR/StencilMetaInformation.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/NonCopyable.h"
#include "dawn/Support/StringRef.h"
#include "dawn/Support/UIDGenerator.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace dawn {
class OptimizerContext;

namespace iir {

enum class TemporaryScope { TS_LocalVariable, TS_StencilTemporary };

/// @brief Specific instantiation of a stencil
/// @ingroup optimizer
class StencilInstantiation : NonCopyable {
  StencilMetaInformation metadata_;
  std::unique_ptr<IIR> IIR_;

public:
  /// @brief Dump the StencilInstantiation to stdout
  void dump() const;

  /// @brief Assemble StencilInstantiation for stencil
  StencilInstantiation(
      sir::GlobalVariableMap const& globalVariables = {},
      std::vector<std::shared_ptr<sir::StencilFunction>> const& stencilFunctions = {});

  StencilMetaInformation& getMetaData();
  const StencilMetaInformation& getMetaData() const { return metadata_; }

  std::shared_ptr<StencilInstantiation> clone() const;

  bool checkTreeConsistency() const;

  /// @brief Get the name of the StencilInstantiation (corresponds to the name of the SIRStencil)
  const std::string getName() const;

  /// @brief Get the orginal `name` and a list of source locations of the field (or variable)
  /// associated with the `AccessID` in the given statement.
  std::pair<std::string, std::vector<SourceLocation>>
  getOriginalNameAndLocationsFromAccessID(int AccessID,
                                          const std::shared_ptr<iir::Stmt>& stmt) const;

  /// @brief Get the original name of the field (as registered in the AST)
  std::string getOriginalNameFromAccessID(int AccessID) const;

  /// @brief check whether the `accessID` is accessed in more than one stencil
  bool isIDAccessedMultipleStencils(int accessID) const;

  /// @brief check whether the `accessID` is accessed in more than one MS
  bool isIDAccessedMultipleMSs(int accessID) const;

  /// @brief Get the value of the global variable `name`
  const sir::Value& getGlobalVariableValue(const std::string& name) const;

  enum RenameDirection {
    RD_Above, ///< Rename all fields above the current statement
    RD_Below  ///< Rename all fields below the current statement
  };

  /// @brief Get the list of stencils
  inline const std::vector<std::unique_ptr<Stencil>>& getStencils() const {
    return getIIR()->getChildren();
  }

  /// @brief get the IIR tree
  inline const std::unique_ptr<IIR>& getIIR() const { return IIR_; }

  /// @brief get the IIR tree
  inline std::unique_ptr<IIR>& getIIR() { return IIR_; }

  bool insertBoundaryConditions(std::string originalFieldName,
                                std::shared_ptr<iir::BoundaryConditionDeclStmt> bc);

  /// @brief Get a unique (positive) identifier
  inline int nextUID() { return UIDGenerator::getInstance()->get(); }

  /// @brief Dump the StencilInstantiation to stdout
  void jsonDump(std::string filename) const;

  /// @brief Register a new stencil function
  ///
  /// If `curStencilFunctionInstantiation` is not NULL, the stencil function is treated as a nested
  /// stencil function.
  std::shared_ptr<StencilFunctionInstantiation> makeStencilFunctionInstantiation(
      const std::shared_ptr<iir::StencilFunCallExpr>& expr,
      const std::shared_ptr<sir::StencilFunction>& SIRStencilFun,
      const std::shared_ptr<iir::AST>& ast, const Interval& interval,
      const std::shared_ptr<StencilFunctionInstantiation>& curStencilFunctionInstantiation);

  /// @brief Report the accesses to the console (according to `-freport-accesses`)
  void reportAccesses() const;
};
} // namespace iir
} // namespace dawn

#endif
