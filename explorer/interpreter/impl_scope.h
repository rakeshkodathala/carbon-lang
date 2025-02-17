// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CARBON_EXPLORER_INTERPRETER_IMPL_SCOPE_H_
#define CARBON_EXPLORER_INTERPRETER_IMPL_SCOPE_H_

#include "explorer/ast/declaration.h"
#include "explorer/ast/value.h"
#include "explorer/interpreter/type_structure.h"

namespace Carbon {

class TypeChecker;

// The `ImplScope` class is responsible for mapping a type and
// interface to the location of the witness table for the `impl` for
// that type and interface.  A scope may have parent scopes, whose
// impls will also be visible in the child scope.
//
// There is typically one instance of `ImplScope` class per scope
// because the impls that are visible for a given type and interface
// can vary from scope to scope. For example, consider the `bar` and
// `baz` methods in the following class C and nested class D.
//
//     class C(U:! type, T:! type)  {
//       class D(V:! type where U is Fooable(T)) {
//         fn bar[self: Self](x: U, y : T) -> T{
//           return x.foo(y)
//         }
//       }
//       fn baz[self: Self](x: U, y : T) -> T {
//         return x.foo(y);
//       }
//     }
//
//  The call to `x.foo` in `bar` is valid because the `U is Fooable(T)`
//  impl is visible in the body of `bar`. In contrast, the call to
//  `x.foo` in `baz` is not valid because there is no visible impl for
//  `U` and `Fooable` in that scope.
//
// `ImplScope` also tracks the type equalities that are known in a particular
// scope.
class ImplScope {
 public:
  // The `Impl` struct is a key-value pair where the key is the
  // combination of a type and an interface, e.g., `List` and `Container`,
  // and the value is the result of statically resolving to the `impl`
  // for `List` as `Container`, which is an `Expression` that produces
  // the witness for that `impl`.
  //
  // When the `impl` is parameterized, `deduced` and `impl_bindings`
  // are non-empty. The former contains the type parameters and the
  // later are impl bindings, that is, parameters for witnesses. In this case,
  // `sort_key` indicates the order in which this impl should be considered
  // relative to other matching impls.
  struct Impl {
    Nonnull<const InterfaceType*> interface;
    std::vector<Nonnull<const GenericBinding*>> deduced;
    Nonnull<const Value*> type;
    std::vector<Nonnull<const ImplBinding*>> impl_bindings;
    Nonnull<const Witness*> witness;
    std::optional<TypeStructureSortKey> sort_key;
  };

  // Internal type used to represent the result of resolving a lookup in a
  // particular impl scope.
  struct ResolveResult {
    Nonnull<const Impl*> impl;
    Nonnull<const Witness*> witness;
  };

  explicit ImplScope() {}
  explicit ImplScope(Nonnull<const ImplScope*> parent)
      : parent_scope_(parent) {}

  // Associates `iface` and `type` with the `impl` in this scope. If `iface` is
  // a constraint type, it will be split into its constituent components, and
  // any references to `.Self` are expected to have been substituted for the
  // type implementing the constraint.
  void Add(Nonnull<const Value*> iface, Nonnull<const Value*> type,
           Nonnull<const Witness*> witness, const TypeChecker& type_checker);
  // For a parameterized impl, associates `iface` and `type`
  // with the `impl` in this scope. Otherwise, the same as the previous
  // overload.
  void Add(Nonnull<const Value*> iface,
           llvm::ArrayRef<Nonnull<const GenericBinding*>> deduced,
           Nonnull<const Value*> type,
           llvm::ArrayRef<Nonnull<const ImplBinding*>> impl_bindings,
           Nonnull<const Witness*> witness, const TypeChecker& type_checker,
           std::optional<TypeStructureSortKey> sort_key = std::nullopt);
  // Adds a list of impl constraints from a constraint type into scope. Any
  // references to `.Self` are expected to have already been substituted for
  // the type implementing the constraint.
  void Add(llvm::ArrayRef<ImplConstraint> impls,
           llvm::ArrayRef<Nonnull<const GenericBinding*>> deduced,
           llvm::ArrayRef<Nonnull<const ImplBinding*>> impl_bindings,
           Nonnull<const Witness*> witness, const TypeChecker& type_checker);

  // Adds a type equality constraint.
  void AddEqualityConstraint(Nonnull<const EqualityConstraint*> equal) {
    equalities_.push_back(equal);
  }

  // Returns the associated impl for the given `constraint` and `type` in
  // the ancestor graph of this scope, or reports a compilation error
  // at `source_loc` there isn't exactly one matching impl.
  //
  // If any substitutions should be made into the constraint before resolving
  // it, those should be passed in `bindings`. The witness returned will be for
  // `constraint`, not for the result of substituting the bindings into the
  // constraint. The substituted type might in general have a different shape
  // of witness due to deduplication.
  auto Resolve(Nonnull<const Value*> constraint, Nonnull<const Value*> type,
               SourceLocation source_loc, const TypeChecker& type_checker,
               const Bindings& bindings = {}) const
      -> ErrorOr<Nonnull<const Witness*>>;

  // Same as Resolve, except that failure due to a missing implementation of a
  // constraint produces `nullopt` instead of an error if
  // `diagnose_missing_impl` is `false`. This is intended for cases where we're
  // selecting between options based on whether constraints are satisfied, such
  // as during `impl` selection.
  auto TryResolve(Nonnull<const Value*> constraint, Nonnull<const Value*> type,
                  SourceLocation source_loc, const TypeChecker& type_checker,
                  const Bindings& bindings, bool diagnose_missing_impl) const
      -> ErrorOr<std::optional<Nonnull<const Witness*>>>;

  // Visits the values that are a single step away from `value` according to an
  // equality constraint that is in scope. That is, the values `v` such that we
  // have a `value == v` equality constraint in scope.
  //
  // Stops and returns `false` if any call to the visitor returns `false`,
  // otherwise returns `true`.
  auto VisitEqualValues(
      Nonnull<const Value*> value,
      llvm::function_ref<bool(Nonnull<const Value*>)> visitor) const -> bool;

  void Print(llvm::raw_ostream& out) const;

 private:
  // Returns the associated impl for the given `iface` and `type` in
  // the ancestor graph of this scope. Reports a compilation error
  // at `source_loc` if there's an ambiguity, or if `diagnose_missing_impl` is
  // set and there's no matching impl.
  auto TryResolveInterface(Nonnull<const InterfaceType*> iface,
                           Nonnull<const Value*> type,
                           SourceLocation source_loc,
                           const TypeChecker& type_checker,
                           bool diagnose_missing_impl) const
      -> ErrorOr<std::optional<Nonnull<const Witness*>>>;

  // Returns the associated impl for the given `iface` and `type` in
  // the ancestor graph of this scope, returns std::nullopt if there
  // is none, or reports a compilation error is there is not a most
  // specific impl for the given `iface` and `type`.
  // Use `original_scope` to satisfy requirements of any generic impl
  // that matches `iface` and `type`.
  auto TryResolveInterfaceRecursively(Nonnull<const InterfaceType*> iface_type,
                                      Nonnull<const Value*> type,
                                      SourceLocation source_loc,
                                      const ImplScope& original_scope,
                                      const TypeChecker& type_checker) const
      -> ErrorOr<std::optional<ResolveResult>>;

  // Returns the associated impl for the given `iface` and `type` in
  // this scope, returns std::nullopt if there is none, or reports
  // a compilation error is there is not a most specific impl for the
  // given `iface` and `type`.
  // Use `original_scope` to satisfy requirements of any generic impl
  // that matches `iface` and `type`.
  auto TryResolveInterfaceHere(Nonnull<const InterfaceType*> iface_type,
                               Nonnull<const Value*> impl_type,
                               SourceLocation source_loc,
                               const ImplScope& original_scope,
                               const TypeChecker& type_checker) const
      -> ErrorOr<std::optional<ResolveResult>>;

  std::vector<Impl> impls_;
  std::vector<Nonnull<const EqualityConstraint*>> equalities_;
  std::optional<Nonnull<const ImplScope*>> parent_scope_;
};

// An equality context that considers two values to be equal if they are a
// single step apart according to an equality constraint in the given impl
// scope.
struct SingleStepEqualityContext : public EqualityContext {
 public:
  explicit SingleStepEqualityContext(Nonnull<const ImplScope*> impl_scope)
      : impl_scope_(impl_scope) {}

  // Visits the values that are equal to the given value and a single step away
  // according to an equality constraint that is in the given impl scope. Stops
  // and returns `false` if the visitor returns `false`, otherwise returns
  // `true`.
  auto VisitEqualValues(Nonnull<const Value*> value,
                        llvm::function_ref<bool(Nonnull<const Value*>)> visitor)
      const -> bool override;

 private:
  Nonnull<const ImplScope*> impl_scope_;
};

}  // namespace Carbon

#endif  // CARBON_EXPLORER_INTERPRETER_IMPL_SCOPE_H_
