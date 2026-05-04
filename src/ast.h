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

// Function declaration. The body is a sequence of flat-AST node indices
// owned by the shared NodeStore on JamCodegenContext.
class FunctionAST {
  public:
	std::string Name;
	std::vector<std::pair<std::string, TypeIdx>> Args;  // (name, type)
	TypeIdx ReturnType;  // kNoType if void / unspecified
	std::vector<NodeIdx> Body;
	bool isExtern;
	bool isExport;
	bool isPub;
	bool isTest;
	bool isVarArgs;

	FunctionAST(std::string Name,
	            std::vector<std::pair<std::string, TypeIdx>> Args,
	            TypeIdx ReturnType, std::vector<NodeIdx> Body,
	            bool isExtern = false, bool isExport = false,
	            bool isPub = false, bool isTest = false,
	            bool isVarArgs = false)
	    : Name(std::move(Name)), Args(std::move(Args)), ReturnType(ReturnType),
	      Body(std::move(Body)), isExtern(isExtern), isExport(isExport),
	      isPub(isPub), isTest(isTest), isVarArgs(isVarArgs) {}

	JamFunctionRef codegen(JamCodegenContext &ctx);
};

// Top-level struct declaration: const Vec3 = struct { x: f32, y: f32 };
class StructDeclAST {
  public:
	std::string Name;
	std::vector<std::pair<std::string, TypeIdx>> Fields;  // (name, type)

	StructDeclAST(std::string Name,
	              std::vector<std::pair<std::string, TypeIdx>> Fields)
	    : Name(std::move(Name)), Fields(std::move(Fields)) {}
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
	std::vector<std::unique_ptr<FunctionAST>> Functions;

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
