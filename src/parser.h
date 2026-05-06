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
	NodeIdx parseMatch();
	NodeIdx parsePattern();          // OrPattern
	NodeIdx parsePatternAtom();      // single-atom pattern
	std::unique_ptr<FunctionAST> parseFunction();
	std::unique_ptr<ImportDeclAST> parseImportDecl();
	std::unique_ptr<DestructuringImportDeclAST> parseDestructuringImport();
	std::unique_ptr<StructDeclAST> parseStructDecl();
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
};

#endif  // PARSER_H
