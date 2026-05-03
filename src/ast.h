/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef AST_H
#define AST_H

#include "jam_llvm.h"
#include <memory>
#include <string>
#include <vector>

// Forward declarations
class ExprAST;
class FunctionAST;
class JamCodegenContext;

// AST node base class
class ExprAST {
  public:
	virtual ~ExprAST() = default;
	virtual JamValueRef codegen(JamCodegenContext &ctx) = 0;
};

// Number literal — stays untyped until the use site sets an expected type.
// If no expected type is set, falls back to the smallest integer type that
// fits the literal value.
class NumberExprAST : public ExprAST {
	uint64_t Val;
	bool IsNegative;
	JamTypeRef ExpectedType = nullptr;

  public:
	NumberExprAST(uint64_t Val, bool IsNegative = false)
	    : Val(Val), IsNegative(IsNegative) {}
	void setExpectedType(JamTypeRef ty) { ExpectedType = ty; }
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Boolean literal
class BooleanExprAST : public ExprAST {
	bool Val;

  public:
	BooleanExprAST(bool Val) : Val(Val) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// `undefined` literal — marks a binding's storage as left uninitialized.
// Only meaningful as a VarDeclAST initializer; calling codegen() directly
// throws.
class UndefinedExprAST : public ExprAST {
  public:
	UndefinedExprAST() {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// String literal
class StringLiteralExprAST : public ExprAST {
	std::string Val;

  public:
	StringLiteralExprAST(std::string Val) : Val(std::move(Val)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Variable reference
class VariableExprAST : public ExprAST {
	std::string Name;

  public:
	VariableExprAST(std::string Name) : Name(std::move(Name)) {}
	const std::string &getName() const { return Name; }
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Unary operation (e.g., logical NOT)
class UnaryExprAST : public ExprAST {
	std::string Op;
	std::unique_ptr<ExprAST> Operand;

  public:
	UnaryExprAST(std::string Op, std::unique_ptr<ExprAST> Operand)
	    : Op(std::move(Op)), Operand(std::move(Operand)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Binary operation
class BinaryExprAST : public ExprAST {
	std::string Op;
	std::unique_ptr<ExprAST> LHS, RHS;

  public:
	BinaryExprAST(std::string Op, std::unique_ptr<ExprAST> LHS,
	              std::unique_ptr<ExprAST> RHS)
	    : Op(std::move(Op)), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Function call
class CallExprAST : public ExprAST {
	std::string Callee;
	std::vector<std::unique_ptr<ExprAST>> Args;

  public:
	CallExprAST(std::string Callee, std::vector<std::unique_ptr<ExprAST>> Args)
	    : Callee(std::move(Callee)), Args(std::move(Args)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;

  private:
	JamValueRef generatePrintCall(JamCodegenContext &ctx);
	JamValueRef generateSleepCall(JamCodegenContext &ctx);
	JamValueRef generateAssertCall(JamCodegenContext &ctx);
};

// Return statement
class ReturnExprAST : public ExprAST {
	std::unique_ptr<ExprAST> RetVal;

  public:
	ReturnExprAST(std::unique_ptr<ExprAST> RetVal)
	    : RetVal(std::move(RetVal)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Assignment statement: target = value;
class AssignExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Target;
	std::unique_ptr<ExprAST> Value;

  public:
	AssignExprAST(std::unique_ptr<ExprAST> Target,
	              std::unique_ptr<ExprAST> Value)
	    : Target(std::move(Target)), Value(std::move(Value)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Variable declaration
class VarDeclAST : public ExprAST {
	std::string Name;
	std::string Type;
	bool IsConst;
	std::unique_ptr<ExprAST> Init;

  public:
	VarDeclAST(std::string Name, std::string Type, bool IsConst,
	           std::unique_ptr<ExprAST> Init)
	    : Name(std::move(Name)), Type(std::move(Type)), IsConst(IsConst),
	      Init(std::move(Init)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// If statement
class IfExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Condition;
	std::vector<std::unique_ptr<ExprAST>> ThenBody;
	std::vector<std::unique_ptr<ExprAST>> ElseBody;

  public:
	IfExprAST(std::unique_ptr<ExprAST> Condition,
	          std::vector<std::unique_ptr<ExprAST>> ThenBody,
	          std::vector<std::unique_ptr<ExprAST>> ElseBody)
	    : Condition(std::move(Condition)), ThenBody(std::move(ThenBody)),
	      ElseBody(std::move(ElseBody)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// While loop
class WhileExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Condition;
	std::vector<std::unique_ptr<ExprAST>> Body;

  public:
	WhileExprAST(std::unique_ptr<ExprAST> Condition,
	             std::vector<std::unique_ptr<ExprAST>> Body)
	    : Condition(std::move(Condition)), Body(std::move(Body)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// For loop
class ForExprAST : public ExprAST {
	std::string VarName;
	std::unique_ptr<ExprAST> Start;
	std::unique_ptr<ExprAST> End;
	std::vector<std::unique_ptr<ExprAST>> Body;

  public:
	ForExprAST(std::string VarName, std::unique_ptr<ExprAST> Start,
	           std::unique_ptr<ExprAST> End,
	           std::vector<std::unique_ptr<ExprAST>> Body)
	    : VarName(std::move(VarName)), Start(std::move(Start)),
	      End(std::move(End)), Body(std::move(Body)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Break statement
class BreakExprAST : public ExprAST {
  public:
	BreakExprAST() {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Continue statement
class ContinueExprAST : public ExprAST {
  public:
	ContinueExprAST() {}
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Import expression - import("path")
class ImportExprAST : public ExprAST {
	std::string ModulePath;

  public:
	ImportExprAST(std::string ModulePath) : ModulePath(std::move(ModulePath)) {}
	const std::string &getModulePath() const { return ModulePath; }
	JamValueRef codegen(JamCodegenContext &ctx) override;
};

// Member access expression - for std.fmt.println style access
class MemberAccessExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Object;
	std::string Member;

  public:
	MemberAccessExprAST(std::unique_ptr<ExprAST> Object, std::string Member)
	    : Object(std::move(Object)), Member(std::move(Member)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;

	// Helper to get the full qualified name (e.g., "std.fmt.println")
	std::string getQualifiedName() const;

	const std::string &getMember() const { return Member; }
	ExprAST *getObject() const { return Object.get(); }
};

// Array indexing: obj[idx]
class IndexExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Object;
	std::unique_ptr<ExprAST> Index;

  public:
	IndexExprAST(std::unique_ptr<ExprAST> Object,
	             std::unique_ptr<ExprAST> Index)
	    : Object(std::move(Object)), Index(std::move(Index)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;

	ExprAST *getObject() const { return Object.get(); }
	ExprAST *getIndex() const { return Index.get(); }
};

// Struct literal expression - { x: 0, y: 100, z: 50 }
class StructLiteralExprAST : public ExprAST {
	std::string TypeName;  // Filled in by VarDeclAST when target type is known
	std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> Fields;

  public:
	StructLiteralExprAST(
	    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> Fields)
	    : Fields(std::move(Fields)) {}
	JamValueRef codegen(JamCodegenContext &ctx) override;

	void setTypeName(const std::string &name) { TypeName = name; }
	const std::string &getTypeName() const { return TypeName; }
	const std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> &
	getFields() const {
		return Fields;
	}
};

// Function declaration
class FunctionAST {
  public:
	std::string Name;
	std::vector<std::pair<std::string, std::string>> Args;  // (name, type)
	std::string ReturnType;
	std::vector<std::unique_ptr<ExprAST>> Body;
	bool isExtern;  // extern function (no body, import from C)
	bool isExport;  // export function (C ABI export)
	bool isPub;     // pub function (visible to Jam modules, like Zig)
	bool isTest;    // test function (tfn)

	FunctionAST(std::string Name,
	            std::vector<std::pair<std::string, std::string>> Args,
	            std::string ReturnType,
	            std::vector<std::unique_ptr<ExprAST>> Body,
	            bool isExtern = false, bool isExport = false,
	            bool isPub = false, bool isTest = false)
	    : Name(std::move(Name)), Args(std::move(Args)),
	      ReturnType(std::move(ReturnType)), Body(std::move(Body)),
	      isExtern(isExtern), isExport(isExport), isPub(isPub), isTest(isTest) {
	}

	JamFunctionRef codegen(JamCodegenContext &ctx);
};

// Top-level struct declaration
// const Vec3 = struct { x: f32, y: f32, z: f32 };
class StructDeclAST {
  public:
	std::string Name;
	std::vector<std::pair<std::string, std::string>> Fields;  // (name, type)

	StructDeclAST(std::string Name,
	              std::vector<std::pair<std::string, std::string>> Fields)
	    : Name(std::move(Name)), Fields(std::move(Fields)) {}
};

// Top-level import declaration
// const std = import("std");
class ImportDeclAST {
  public:
	std::string Name;  // The binding name (e.g., "std")
	std::string Path;  // The import path (e.g., "std" or "./utils")

	ImportDeclAST(std::string Name, std::string Path)
	    : Name(std::move(Name)), Path(std::move(Path)) {}
};

// Destructuring import declaration (like Zig)
// const { func1, func2 } = import("mod");
class DestructuringImportDeclAST {
  public:
	std::vector<std::string> Names;  // The imported symbol names
	std::string Path;                // The import path

	DestructuringImportDeclAST(std::vector<std::string> Names, std::string Path)
	    : Names(std::move(Names)), Path(std::move(Path)) {}
};

// Module - holds all top-level declarations
class ModuleAST {
  public:
	std::vector<std::unique_ptr<ImportDeclAST>> Imports;
	std::vector<std::unique_ptr<DestructuringImportDeclAST>>
	    DestructuringImports;
	std::vector<std::unique_ptr<StructDeclAST>> Structs;
	std::vector<std::unique_ptr<FunctionAST>> Functions;

	ModuleAST() = default;
};

// Global variables to track loop context for break/continue
extern JamBasicBlockRef CurrentLoopContinue;
extern JamBasicBlockRef CurrentLoopBreak;

#endif  // AST_H
