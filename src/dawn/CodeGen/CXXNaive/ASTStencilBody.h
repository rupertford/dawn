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

#ifndef DAWN_CODEGEN_CXXNAIVE_ASTSTENCILBODY_H
#define DAWN_CODEGEN_CXXNAIVE_ASTSTENCILBODY_H

#include "dawn/CodeGen/ASTCodeGenCXX.h"
#include "dawn/CodeGen/CodeGenProperties.h"
#include "dawn/IIR/Interval.h"
#include "dawn/Support/StringUtil.h"
#include <stack>
#include <unordered_map>

namespace dawn {

namespace iir {
class StencilFunctionInstantiation;
class StencilMetaInformation;
} // namespace iir

namespace codegen {
namespace cxxnaive {

/// @brief ASTVisitor to generate C++ naive code for the stencil and stencil function bodies
/// @ingroup cxxnaive
class ASTStencilBody : public ASTCodeGenCXX {
protected:
  const iir::StencilMetaInformation& metadata_;
  RangeToString offsetPrinter_;

  /// The stencil function we are currently generating or NULL
  std::shared_ptr<iir::StencilFunctionInstantiation> currentFunction_;

  /// Nesting level of argument lists of stencil function *calls*
  int nestingOfStencilFunArgLists_;

  StencilContext stencilContext_;

  ///
  /// @brief produces a string of (i,j,k) accesses for the C++ generated naive code,
  /// from an array of offseted accesses
  ///
  template <long unsigned N>
  std::array<std::string, N> ijkfyOffset(const std::array<int, N>& offsets,
                                         std::string accessName) {
    int n = -1;
    std::array<std::string, N> res;
    std::transform(offsets.begin(), offsets.end(), res.begin(), [&](int const& off) {
      ++n;
      std::array<std::string, 3> indices{"i+", "j+", "k+"};

      return ((n < 4) ? indices[n] : "") + std::to_string(off) +
             ((stencilContext_ == StencilContext::SC_StencilFunction)
                  ? "+" + accessName + "_offsets[" + std::to_string(n) + "]"
                  : "");
    });
    return res;
  }

public:
  using Base = ASTCodeGenCXX;

  /// @brief constructor
  ASTStencilBody(const iir::StencilMetaInformation& metadata, StencilContext stencilContext);

  virtual ~ASTStencilBody();

  /// @name Statement implementation
  /// @{
  virtual void visit(const std::shared_ptr<iir::BlockStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::ExprStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::ReturnStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::VarDeclStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::VerticalRegionDeclStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::StencilCallDeclStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::BoundaryConditionDeclStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<iir::IfStmt>& stmt) override;
  /// @}

  /// @name Expression implementation
  /// @{
  virtual void visit(const std::shared_ptr<iir::UnaryOperator>& expr) override;
  virtual void visit(const std::shared_ptr<iir::BinaryOperator>& expr) override;
  virtual void visit(const std::shared_ptr<iir::AssignmentExpr>& expr) override;
  virtual void visit(const std::shared_ptr<iir::TernaryOperator>& expr) override;
  virtual void visit(const std::shared_ptr<iir::FunCallExpr>& expr) override;
  virtual void visit(const std::shared_ptr<iir::StencilFunCallExpr>& expr) override;
  virtual void visit(const std::shared_ptr<iir::StencilFunArgExpr>& expr) override;
  virtual void visit(const std::shared_ptr<iir::VarAccessExpr>& expr) override;
  virtual void visit(const std::shared_ptr<iir::LiteralAccessExpr>& expr) override;
  virtual void visit(const std::shared_ptr<iir::FieldAccessExpr>& expr) override;
  /// @}

  /// @brief Set the current stencil function (can be NULL)
  void setCurrentStencilFunction(
      const std::shared_ptr<iir::StencilFunctionInstantiation>& currentFunction);

  /// @brief Mapping of VarDeclStmt and Var/FieldAccessExpr to their name
  std::string getName(const std::shared_ptr<iir::Expr>& expr) const override;
  std::string getName(const std::shared_ptr<iir::Stmt>& stmt) const override;
  int getAccessID(const std::shared_ptr<iir::Expr>& expr) const;
};

} // namespace cxxnaive
} // namespace codegen
} // namespace dawn

#endif
