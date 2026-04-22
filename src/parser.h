/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "token.h"
#include <memory>
#include <vector>

class Parser {
  private:
	std::vector<Token> tokens;
	int current = 0;

	Token peek() const;
	Token previous() const;
	bool isAtEnd() const;
	Token advance();
	bool check(TokenType type) const;
	bool match(TokenType type);
	void consume(TokenType type, const std::string &message);

	std::unique_ptr<ExprAST> parsePrimary();
	std::unique_ptr<ExprAST> parseUnary();
	std::string parseType();
	std::unique_ptr<ExprAST> parseExpression();
	std::unique_ptr<ExprAST> parseLogicalOr();
	std::unique_ptr<ExprAST> parseLogicalAnd();
	std::unique_ptr<ExprAST> parseComparison();
	std::unique_ptr<ExprAST> parseAddition();
	std::unique_ptr<FunctionAST> parseFunction();
	std::unique_ptr<ImportDeclAST> parseImportDecl();
	std::unique_ptr<DestructuringImportDeclAST> parseDestructuringImport();

  public:
	explicit Parser(std::vector<Token> tokens);
	std::unique_ptr<ModuleAST> parse();
};

#endif  // PARSER_H
