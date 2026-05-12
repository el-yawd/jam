/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "ast_flat.h"
#include "token.h"
#include <memory>
#include <vector>

class Parser {
  private:
	std::vector<Token> tokens;
	int current = 0;
	TypePool *typePool;
	StringPool *stringPool;
	NodeStore *nodes;

	Token peek() const;
	Token previous() const;
	bool isAtEnd() const;
	Token advance();
	bool check(TokenType type) const;
	bool match(TokenType type);
	void consume(TokenType type, const std::string &message);

	NodeIdx emit(AstNode n);

	NodeIdx parsePrimary();
	NodeIdx parseUnary();
	TypeIdx parseType();
	NodeIdx parseExpression();
	NodeIdx parseLogicalOr();
	NodeIdx parseLogicalAnd();
	NodeIdx parseComparison();
	NodeIdx parseBitwise();
	NodeIdx parseShift();
	NodeIdx parseAddition();
	NodeIdx parseMultiplication();
	NodeIdx parseStructLiteral();
	NodeIdx parseStructExpression();
	NodeIdx parseEnumExpression();
	NodeIdx parseMatch();
	NodeIdx parsePattern();          // OrPattern
	NodeIdx parsePatternAtom();      // single-atom pattern
	std::unique_ptr<FunctionAST> parseFunction();
	std::unique_ptr<ImportDeclAST> parseImportDecl();
	std::unique_ptr<DestructuringImportDeclAST> parseDestructuringImport();
	std::unique_ptr<StructDeclAST> parseStructDecl();
	// Generics G2: parses the `{ field: T, fn method(self: ...) {...} }`
	// body shared between top-level struct decls and anonymous struct
	// expressions. Caller must have already consumed the `struct` keyword
	// and the opening `{`. On return, the closing `}` has been consumed.
	void parseStructBody(
	    std::vector<std::pair<std::string, TypeIdx>> &fields,
	    std::vector<std::unique_ptr<FunctionAST>> &methods);

	// Generics G2: anonymous structs created by `struct { ... }`
	// expressions. Stored here during parse and transferred to
	// `ModuleAST::AnonStructs` at the end of parse().
	std::vector<std::unique_ptr<StructDeclAST>> anonStructs;

	// Mirror of anonStructs for `enum { ... }` expressions. Enables
	// generic enums (`Option(T)`, `Result(T, E)`, etc.). Transferred
	// to `ModuleAST::AnonEnums` at the end of parse().
	std::vector<std::unique_ptr<EnumDeclAST>> anonEnums;

	// Generics G3: stack of struct names being parsed, used to resolve
	// `Self` references inside method signatures. Top-level structs push
	// their declared name; struct expressions push their synthetic
	// `__anon_struct_<N>` name. Empty when not inside any struct body.
	std::vector<std::string> structContextStack;
	std::unique_ptr<UnionDeclAST> parseUnionDecl();
	std::unique_ptr<EnumDeclAST> parseEnumDecl();
	std::unique_ptr<ConstDeclAST> parseConstDecl();

	// Helper to walk a member-access chain at parse time and produce the
	// fully qualified name (e.g. "std.fmt.println") from a chain whose root
	// is a Variable node.
	std::string qualifiedName(NodeIdx chainRoot) const;

  public:
	Parser(std::vector<Token> tokens, TypePool &typePool,
	       StringPool &stringPool, NodeStore &nodes);
	std::unique_ptr<ModuleAST> parse();

	// Optional shared anon-storage. When set before `parse()` runs,
	// `struct {...}` / `enum {...}` expression bodies append to these
	// shared vectors (the same vectors every other Parser in the
	// compilation shares). Codegen consults the shared vectors to
	// resolve EnumExpr / StructExpr indices uniformly across the main
	// module + every imported module. Set by main.cpp; left null for
	// standalone unit tests.
	std::vector<std::unique_ptr<StructDeclAST>> *sharedAnonStructs =
	    nullptr;
	std::vector<std::unique_ptr<EnumDeclAST>> *sharedAnonEnums = nullptr;
};

#endif  // PARSER_H
