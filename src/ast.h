/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef AST_H
#define AST_H

#include "ast_flat.h"
#include "jam_llvm.h"
#include <memory>
#include <string>
#include <vector>

class JamCodegenContext;

// Parameter passing mode under MVS (mutable value semantics).
//
// Default (Let)      — read-only borrow; param is initialized at entry,
//                      cannot be reassigned, caller's binding unchanged.
// Mut                — exclusive read-write borrow; param is initialized
//                      at entry, may be read and written, caller's binding
//                      stays initialized after the call.
// Move               — consume ownership; caller's binding becomes
//                      uninitialized after the call.
// Undefined          — write-to-uninit destination; param is uninitialized
//                      at entry, function must initialize before any return,
//                      caller's binding becomes initialized after the call.
//
// See docs/MVS.md §2 for the full specification. Modes are static-only;
// the calling convention is unchanged at the LLVM IR level (see §7).
enum class ParamMode : uint8_t {
	Let = 0,
	Mut,
	Move,
};

// One function parameter. Mode defaults to Let when not annotated at the
// declaration site (the common case for read-only parameters).
struct Param {
	std::string Name;
	TypeIdx Type;
	ParamMode Mode = ParamMode::Let;
};

// Function declaration. The body is a sequence of flat-AST node indices
// owned by the shared NodeStore on JamCodegenContext.
class FunctionAST {
  public:
	std::string Name;
	std::vector<Param> Args;
	TypeIdx ReturnType;  // kNoType if void / unspecified
	std::vector<NodeIdx> Body;
	bool isExtern;
	bool isExport;
	bool isPub;
	bool isTest;
	bool isVarArgs;

	FunctionAST(std::string Name, std::vector<Param> Args, TypeIdx ReturnType,
	            std::vector<NodeIdx> Body, bool isExtern = false,
	            bool isExport = false, bool isPub = false, bool isTest = false,
	            bool isVarArgs = false)
	    : Name(std::move(Name)), Args(std::move(Args)), ReturnType(ReturnType),
	      Body(std::move(Body)), isExtern(isExtern), isExport(isExport),
	      isPub(isPub), isTest(isTest), isVarArgs(isVarArgs) {}

	// Two-pass codegen: declarePrototype emits just the LLVM function
	// signature (so other functions can reference this one before its body
	// is built), and defineBody fills in the body. This is what enables
	// out-of-order definitions, including the common "main on top" layout.
	JamFunctionRef declarePrototype(JamCodegenContext &ctx);
	void defineBody(JamCodegenContext &ctx);

	// Convenience: declare + define in one shot. Kept so single-pass
	// callers (extern, repl-style code) still work.
	JamFunctionRef codegen(JamCodegenContext &ctx);

	// Generics G1: a function is generic iff any of its parameters has
	// type `type` (the meta-type) or its return type is `type`. Generic
	// functions are not lowered to LLVM at decl time — instead they are
	// registered for compile-time instantiation at each call site that
	// supplies concrete type arguments.
	bool isGeneric() const {
		if (ReturnType == BuiltinType::Type) return true;
		for (const Param &p : Args) {
			if (p.Type == BuiltinType::Type) return true;
		}
		return false;
	}
};

// Top-level struct declaration: const Vec3 = struct { x: f32, y: f32 };
//
// Methods declared inside the struct body — `fn name(self: ..., ...) ...`
// — live in `Methods`. The method's `FunctionAST::Name` stays as the user
// wrote it (e.g. `"drop"`); qualification (`Vec3.drop`) is applied at
// registration time in module dispatch. See docs/STRUCT_METHODS.md.
class StructDeclAST {
  public:
	std::string Name;
	std::vector<std::pair<std::string, TypeIdx>> Fields;  // (name, type)
	std::vector<std::unique_ptr<FunctionAST>> Methods;

	StructDeclAST(std::string Name,
	              std::vector<std::pair<std::string, TypeIdx>> Fields,
	              std::vector<std::unique_ptr<FunctionAST>> Methods = {})
	    : Name(std::move(Name)), Fields(std::move(Fields)),
	      Methods(std::move(Methods)) {}
};

// One variant of an enum declaration. Unit variants (no payload) have
// an empty `PayloadTypes` vector; tagged variants carry a sequence of
// positional payload types: `Variant(T1, T2, ...)`.
//
// Discriminants default to "previous variant's value + 1" (starting
// from zero for the first variant). An explicit `Variant = N` overrides
// that for the variant in question, and subsequent variants resume
// counting from N+1.
struct EnumVariantAST {
	std::string Name;
	std::vector<TypeIdx> PayloadTypes;
	uint32_t Discriminant = 0;  // resolved value, set during parse
};

// Top-level enum declaration:
//   const Direction = enum { Up, Down, Left, Right };          (E1)
//   const Op = enum { Nop, LdR8R8(u8, u8), Jp(u16) };           (E2)
//
// Variants get sequential discriminants assigned in declaration order
// (Up=0, Down=1, …). Unit variants have no payload; tagged variants
// carry positional fields. Codegen lowers the enum to:
//   - `i8` if every variant is unit (E1 path)
//   - `{i8 tag, [maxPayloadSize x i8]}` aligned to max payload align
//     if any variant has a payload (E2 path)
class EnumDeclAST {
  public:
	std::string Name;
	std::vector<EnumVariantAST> Variants;

	EnumDeclAST(std::string Name, std::vector<EnumVariantAST> Variants)
	    : Name(std::move(Name)), Variants(std::move(Variants)) {}
};

// Top-level union declaration:
//   const FloatBits = union { i: u32, f: f32 };
//
// Untagged C-style union — every field shares the same address; the
// program tracks which field is "live" out-of-band. Reading a field
// other than the most recently written one reinterprets the bits.
//
// Layout: a struct of `{ alignedType, [pad x i8] }` where alignedType
// is the field with the largest alignment requirement and `pad` is the
// extra bytes needed to reach the largest field's size. This gives the
// union the alignment of the most-aligned field and the size of the
// largest field — matching the C ABI convention.
class UnionDeclAST {
  public:
	std::string Name;
	std::vector<std::pair<std::string, TypeIdx>> Fields;  // (name, type)

	UnionDeclAST(std::string Name,
	             std::vector<std::pair<std::string, TypeIdx>> Fields)
	    : Name(std::move(Name)), Fields(std::move(Fields)) {}
};

// Module-scope value binding: `const NAME[: T]? = expr;`.
//
// Bound to a single integer/bool/float literal (or constant expression
// evaluable at codegen time). At each use site, the init expression is
// codegen'd inline with `DeclaredType` as the expected type — a small
// inlining pass that costs nothing at runtime and lets the optimizer
// fold across uses just like a literal would. This is the same model
// Zig uses for `pub const` of comptime-known values.
class ConstDeclAST {
  public:
	std::string Name;
	TypeIdx DeclaredType;  // kNoType when omitted; init drives the type
	NodeIdx InitExpr;
	// Generics G4: if non-kNoType, this const is a type alias (RHS is a
	// generic-instantiation expression like `Box(i32)`). InitExpr is
	// kNoNode in that case. main.cpp processes type-alias consts
	// before regular consts and registers the alias in the codegen
	// context's type-alias table.
	TypeIdx AliasedType = kNoType;

	ConstDeclAST(std::string Name, TypeIdx DeclaredType, NodeIdx InitExpr)
	    : Name(std::move(Name)), DeclaredType(DeclaredType),
	      InitExpr(InitExpr) {}
};

// const std = import("std");
class ImportDeclAST {
  public:
	std::string Name;
	std::string Path;

	ImportDeclAST(std::string Name, std::string Path)
	    : Name(std::move(Name)), Path(std::move(Path)) {}
};

// const { f1, f2 } = import("mod");
class DestructuringImportDeclAST {
  public:
	std::vector<std::string> Names;
	std::string Path;

	DestructuringImportDeclAST(std::vector<std::string> Names, std::string Path)
	    : Names(std::move(Names)), Path(std::move(Path)) {}
};

class ModuleAST {
  public:
	std::vector<std::unique_ptr<ImportDeclAST>> Imports;
	std::vector<std::unique_ptr<DestructuringImportDeclAST>>
	    DestructuringImports;
	std::vector<std::unique_ptr<StructDeclAST>> Structs;
	std::vector<std::unique_ptr<UnionDeclAST>> Unions;
	std::vector<std::unique_ptr<EnumDeclAST>> Enums;
	std::vector<std::unique_ptr<ConstDeclAST>> Consts;
	std::vector<std::unique_ptr<FunctionAST>> Functions;
	// Generics G2: bodies of `struct { ... }` expressions. Each entry is
	// a regular StructDeclAST with a synthetic name (`__anon_struct_<N>`).
	// Index N comes from the AnonStructs vector at parse time and is
	// stored in the StructExpr AST node's d.lhs slot. The substitution
	// engine in G4 reads from here to instantiate the struct with
	// concrete type arguments.
	std::vector<std::unique_ptr<StructDeclAST>> AnonStructs;

	ModuleAST() = default;
};

// Loop-context globals consumed by Break/Continue codegen.
extern JamBasicBlockRef CurrentLoopContinue;
extern JamBasicBlockRef CurrentLoopBreak;

// Codegen entry point for a single flat-AST node. expectedType (if non-null)
// drives literal materialization (e.g. integer literals adopt the expected
// width, struct literals know their target struct).
JamValueRef codegenNode(JamCodegenContext &ctx, NodeIdx node,
                        JamTypeRef expectedType = nullptr);

// Lvalue-pointer resolver: returns a pointer to the storage backing `node`
// and writes the element type through `outElemType`. Used by assignment and
// address-of paths for `arr[i]`, `g.board[i]`, struct field chains, etc.
JamValueRef resolveLvaluePtr(JamCodegenContext &ctx, NodeIdx node,
                             JamTypeRef &outElemType);

#endif  // AST_H
