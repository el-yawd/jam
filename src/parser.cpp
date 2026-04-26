/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "parser.h"
#include <stdexcept>

Parser::Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {}

Token Parser::peek() const { return tokens[current]; }

Token Parser::previous() const { return tokens[current - 1]; }

bool Parser::isAtEnd() const { return peek().type == TOK_EOF; }

Token Parser::advance() {
	if (!isAtEnd()) current++;
	return previous();
}

bool Parser::check(TokenType type) const {
	if (isAtEnd()) return false;
	return peek().type == type;
}

bool Parser::match(TokenType type) {
	if (check(type)) {
		advance();
		return true;
	}
	return false;
}

void Parser::consume(TokenType type, const std::string &message) {
	if (check(type)) {
		advance();
		return;
	}

	throw std::runtime_error(message);
}

std::unique_ptr<ExprAST> Parser::parsePrimary() {
	if (match(TOK_NUMBER)) {
		std::string numStr = previous().lexeme;
		bool isNegative = !numStr.empty() && numStr[0] == '-';
		if (isNegative) {
			// For negative numbers, parse with stoll and convert
			int64_t signedVal = std::stoll(numStr);
			return std::make_unique<NumberExprAST>(
			    static_cast<uint64_t>(-signedVal), true);
		} else {
			// For positive numbers, use stoull to handle full u64 range
			return std::make_unique<NumberExprAST>(std::stoull(numStr), false);
		}
	} else if (match(TOK_TRUE)) {
		return std::make_unique<BooleanExprAST>(true);
	} else if (match(TOK_FALSE)) {
		return std::make_unique<BooleanExprAST>(false);
	} else if (match(TOK_STRING_LITERAL)) {
		return std::make_unique<StringLiteralExprAST>(previous().lexeme);
	} else if (match(TOK_IMPORT)) {
		// import("path") expression
		consume(TOK_OPEN_PAREN, "Expected '(' after 'import'");
		consume(TOK_STRING_LITERAL, "Expected string literal for import path");
		std::string path = previous().lexeme;
		consume(TOK_CLOSE_PAREN, "Expected ')' after import path");
		return std::make_unique<ImportExprAST>(path);
	} else if (match(TOK_OPEN_PAREN)) {
		auto expr = parseLogicalOr();
		consume(TOK_CLOSE_PAREN, "Expected ')' after expression");
		return expr;
	} else if (match(TOK_OPEN_BRACE)) {
		return parseStructLiteral();
	} else if (match(TOK_IDENTIFIER)) {
		std::string name = previous().lexeme;
		std::unique_ptr<ExprAST> expr = std::make_unique<VariableExprAST>(name);

		// Handle member access chain (std.fmt.println)
		while (match(TOK_DOT)) {
			consume(TOK_IDENTIFIER, "Expected member name after '.'");
			std::string member = previous().lexeme;
			expr =
			    std::make_unique<MemberAccessExprAST>(std::move(expr), member);
		}

		// Check if this is a function call
		if (match(TOK_OPEN_PAREN)) {
			std::vector<std::unique_ptr<ExprAST>> args;

			if (!check(TOK_CLOSE_PAREN)) {
				do {
					args.push_back(parseComparison());
				} while (match(TOK_COMMA));
			}

			consume(TOK_CLOSE_PAREN, "Expected ')' after function arguments");

			// Get the qualified name from the expression chain
			std::string callee;
			if (auto *memberAccess =
			        dynamic_cast<MemberAccessExprAST *>(expr.get())) {
				callee = memberAccess->getQualifiedName();
			} else if (auto *varExpr =
			               dynamic_cast<VariableExprAST *>(expr.get())) {
				callee = name;
			} else {
				callee = name;
			}

			return std::make_unique<CallExprAST>(callee, std::move(args));
		}

		return expr;
	}

	throw std::runtime_error("Expected primary expression");
}

std::string Parser::parseType() {
	// Handle slice types: []T or []const T
	if (match(TOK_OPEN_BRACKET)) {
		consume(TOK_CLOSE_BRACKET, "Expected ']' after '['");
		// Check for []const T (like Zig)
		bool isConst = match(TOK_CONST);
		std::string elementType = parseType();
		if (isConst) { return "[]const " + elementType; }
		return "[]" + elementType;
	}
	// Handle const T (like Zig)
	if (match(TOK_CONST)) {
		std::string innerType = parseType();
		return "const " + innerType;
	}
	// Handle base types
	if (match(TOK_TYPE)) { return previous().lexeme; }
	// Handle user-defined types (struct names)
	if (match(TOK_IDENTIFIER)) { return previous().lexeme; }
	throw std::runtime_error("Expected type");
}

std::unique_ptr<ExprAST> Parser::parseStructLiteral() {
	// Caller has already consumed '{'
	std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> fields;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		consume(TOK_IDENTIFIER, "Expected field name in struct literal");
		std::string fieldName = previous().lexeme;
		consume(TOK_COLON, "Expected ':' after field name");
		auto value = parseLogicalOr();
		fields.emplace_back(fieldName, std::move(value));
		if (!match(TOK_COMMA)) break;
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close struct literal");
	return std::make_unique<StructLiteralExprAST>(std::move(fields));
}

std::unique_ptr<ExprAST> Parser::parseExpression() {
	if (match(TOK_RETURN)) {
		auto expr = parseLogicalOr();
		consume(TOK_SEMI, "Expected ';' after return statement");
		return std::make_unique<ReturnExprAST>(std::move(expr));
	} else if (match(TOK_CONST) || match(TOK_VAR)) {
		bool isConst = previous().type == TOK_CONST;
		consume(TOK_IDENTIFIER, "Expected variable name");
		std::string name = previous().lexeme;

		// Optional type annotation
		std::string type = "u8";  // Default type
		if (match(TOK_COLON)) { type = parseType(); }

		std::unique_ptr<ExprAST> init = nullptr;
		if (match(TOK_EQUAL)) { init = parseLogicalOr(); }
		consume(TOK_SEMI, "Expected ';' after variable declaration");

		return std::make_unique<VarDeclAST>(name, type, isConst,
		                                    std::move(init));
	} else if (match(TOK_IF)) {
		consume(TOK_OPEN_PAREN, "Expected '(' after 'if'");
		auto condition = parseLogicalOr();
		consume(TOK_CLOSE_PAREN, "Expected ')' after if condition");

		consume(TOK_OPEN_BRACE, "Expected '{' after if condition");
		std::vector<std::unique_ptr<ExprAST>> thenBody;
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			thenBody.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected '}' after if body");

		std::vector<std::unique_ptr<ExprAST>> elseBody;
		if (match(TOK_ELSE)) {
			consume(TOK_OPEN_BRACE, "Expected '{' after 'else'");
			while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
				elseBody.push_back(parseExpression());
			}
			consume(TOK_CLOSE_BRACE, "Expected '}' after else body");
		}

		return std::make_unique<IfExprAST>(
		    std::move(condition), std::move(thenBody), std::move(elseBody));
	} else if (match(TOK_WHILE)) {
		consume(TOK_OPEN_PAREN, "Expected '(' after 'while'");
		auto condition = parseLogicalOr();
		consume(TOK_CLOSE_PAREN, "Expected ')' after while condition");

		consume(TOK_OPEN_BRACE, "Expected '{' after while condition");
		std::vector<std::unique_ptr<ExprAST>> body;
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			body.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected '}' after while body");

		return std::make_unique<WhileExprAST>(std::move(condition),
		                                      std::move(body));
	} else if (match(TOK_FOR)) {
		consume(TOK_IDENTIFIER, "Expected variable name after 'for'");
		std::string varName = previous().lexeme;

		consume(TOK_IN, "Expected 'in' after for variable");
		auto start = parseComparison();
		consume(TOK_COLON, "Expected ':' in for range");
		auto end = parseComparison();

		consume(TOK_OPEN_BRACE, "Expected '{' after for range");
		std::vector<std::unique_ptr<ExprAST>> body;
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			body.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected '}' after for body");

		return std::make_unique<ForExprAST>(varName, std::move(start),
		                                    std::move(end), std::move(body));
	} else if (match(TOK_BREAK)) {
		consume(TOK_SEMI, "Expected ';' after break");
		return std::make_unique<BreakExprAST>();
	} else if (match(TOK_CONTINUE)) {
		consume(TOK_SEMI, "Expected ';' after continue");
		return std::make_unique<ContinueExprAST>();
	} else if (check(TOK_IDENTIFIER)) {
		// Parse the expression and check if it's a call (function call
		// statement) or an assignment target.
		auto expr = parseComparison();

		// Assignment statement: target = value;
		if (match(TOK_EQUAL)) {
			auto value = parseLogicalOr();
			consume(TOK_SEMI, "Expected ';' after assignment");
			return std::make_unique<AssignExprAST>(std::move(expr),
			                                       std::move(value));
		}

		// If we got a CallExprAST, consume the semicolon
		if (dynamic_cast<CallExprAST *>(expr.get())) {
			consume(TOK_SEMI, "Expected ';' after function call");
		}

		return expr;
	}

	return parseLogicalOr();
}

std::unique_ptr<ExprAST> Parser::parseLogicalOr() {
	auto LHS = parseLogicalAnd();

	while (match(TOK_OR)) {
		auto RHS = parseLogicalAnd();
		LHS = std::make_unique<BinaryExprAST>("or", std::move(LHS),
		                                      std::move(RHS));
	}

	return LHS;
}

std::unique_ptr<ExprAST> Parser::parseLogicalAnd() {
	auto LHS = parseComparison();

	while (match(TOK_AND)) {
		auto RHS = parseComparison();
		LHS = std::make_unique<BinaryExprAST>("and", std::move(LHS),
		                                      std::move(RHS));
	}

	return LHS;
}

std::unique_ptr<ExprAST> Parser::parseComparison() {
	auto LHS = parseAddition();

	if (match(TOK_EQUAL_EQUAL)) {
		auto RHS = parseAddition();
		return std::make_unique<BinaryExprAST>("==", std::move(LHS),
		                                       std::move(RHS));
	} else if (match(TOK_NOT_EQUAL)) {
		auto RHS = parseAddition();
		return std::make_unique<BinaryExprAST>("!=", std::move(LHS),
		                                       std::move(RHS));
	} else if (match(TOK_LESS)) {
		auto RHS = parseAddition();
		return std::make_unique<BinaryExprAST>("<", std::move(LHS),
		                                       std::move(RHS));
	} else if (match(TOK_LESS_EQUAL)) {
		auto RHS = parseAddition();
		return std::make_unique<BinaryExprAST>("<=", std::move(LHS),
		                                       std::move(RHS));
	} else if (match(TOK_GREATER)) {
		auto RHS = parseAddition();
		return std::make_unique<BinaryExprAST>(">", std::move(LHS),
		                                       std::move(RHS));
	} else if (match(TOK_GREATER_EQUAL)) {
		auto RHS = parseAddition();
		return std::make_unique<BinaryExprAST>(">=", std::move(LHS),
		                                       std::move(RHS));
	}

	return LHS;
}

std::unique_ptr<ExprAST> Parser::parseAddition() {
	auto LHS = parseUnary();

	if (match(TOK_PLUS)) {
		auto RHS = parseUnary();
		return std::make_unique<BinaryExprAST>("+", std::move(LHS),
		                                       std::move(RHS));
	}

	return LHS;
}

std::unique_ptr<ExprAST> Parser::parseUnary() {
	if (match(TOK_NOT)) {
		auto operand = parseUnary();
		return std::make_unique<UnaryExprAST>("!", std::move(operand));
	}

	return parsePrimary();
}

std::unique_ptr<FunctionAST> Parser::parseFunction() {
	// Check for extern, export, pub, or tfn keywords
	bool isExtern = false;
	bool isExport = false;
	bool isPub = false;
	bool isTest = false;

	if (match(TOK_EXTERN)) {
		isExtern = true;
	} else if (match(TOK_EXPORT)) {
		isExport = true;
	} else if (match(TOK_PUB)) {
		isPub = true;
	} else if (match(TOK_TFN)) {
		isTest = true;
	}

	if (!isTest) { consume(TOK_FN, "Expected 'fn' keyword"); }
	consume(TOK_IDENTIFIER, "Expected function name");
	std::string name = previous().lexeme;

	consume(TOK_OPEN_PAREN, "Expected '(' after function name");

	std::vector<std::pair<std::string, std::string>> args;
	if (!check(TOK_CLOSE_PAREN)) {
		do {
			consume(TOK_IDENTIFIER, "Expected parameter name");
			std::string paramName = previous().lexeme;

			consume(TOK_COLON, "Expected ':' after parameter name");
			std::string paramType = parseType();

			args.emplace_back(paramName, paramType);
		} while (match(TOK_COMMA));
	}

	consume(TOK_CLOSE_PAREN, "Expected ')' after parameters");

	// Parse the return type (directly after closing paren, no arrow)
	std::string returnType;
	if (check(TOK_TYPE) || check(TOK_OPEN_BRACKET) || check(TOK_CONST) ||
	    check(TOK_IDENTIFIER)) {
		returnType = parseType();
	}

	// Extern functions don't have a body
	if (isExtern) {
		consume(TOK_SEMI, "Expected ';' after extern function declaration");
		std::vector<std::unique_ptr<ExprAST>> emptyBody;
		return std::make_unique<FunctionAST>(name, std::move(args), returnType,
		                                     std::move(emptyBody), true, false,
		                                     false);
	}

	consume(TOK_OPEN_BRACE, "Expected '{' before function body");

	std::vector<std::unique_ptr<ExprAST>> body;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		body.push_back(parseExpression());
	}

	consume(TOK_CLOSE_BRACE, "Expected '}' after function body");

	return std::make_unique<FunctionAST>(name, std::move(args), returnType,
	                                     std::move(body), false, isExport,
	                                     isPub, isTest);
}

std::unique_ptr<StructDeclAST> Parser::parseStructDecl() {
	// const Name = struct { field1: T, field2: T };
	consume(TOK_CONST, "Expected 'const' for struct declaration");
	consume(TOK_IDENTIFIER, "Expected struct name");
	std::string name = previous().lexeme;
	consume(TOK_EQUAL, "Expected '=' after struct name");
	consume(TOK_STRUCT, "Expected 'struct' keyword");
	consume(TOK_OPEN_BRACE, "Expected '{' after 'struct'");

	std::vector<std::pair<std::string, std::string>> fields;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		consume(TOK_IDENTIFIER, "Expected field name");
		std::string fieldName = previous().lexeme;
		consume(TOK_COLON, "Expected ':' after field name");
		std::string fieldType = parseType();
		fields.emplace_back(fieldName, fieldType);
		if (!match(TOK_COMMA)) break;
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close struct definition");
	consume(TOK_SEMI, "Expected ';' after struct declaration");

	return std::make_unique<StructDeclAST>(name, std::move(fields));
}

std::unique_ptr<ImportDeclAST> Parser::parseImportDecl() {
	// const name = import("path");
	consume(TOK_CONST, "Expected 'const' for import declaration");
	consume(TOK_IDENTIFIER, "Expected identifier for import name");
	std::string name = previous().lexeme;

	consume(TOK_EQUAL, "Expected '=' after import name");
	consume(TOK_IMPORT, "Expected 'import' keyword");
	consume(TOK_OPEN_PAREN, "Expected '(' after 'import'");
	consume(TOK_STRING_LITERAL, "Expected string literal for import path");
	std::string path = previous().lexeme;
	consume(TOK_CLOSE_PAREN, "Expected ')' after import path");
	consume(TOK_SEMI, "Expected ';' after import declaration");

	return std::make_unique<ImportDeclAST>(name, path);
}

std::unique_ptr<DestructuringImportDeclAST> Parser::parseDestructuringImport() {
	// const { func1, func2 } = import("path");
	consume(TOK_CONST, "Expected 'const' for destructuring import");
	consume(TOK_OPEN_BRACE, "Expected '{' for destructuring import");

	std::vector<std::string> names;
	do {
		consume(TOK_IDENTIFIER, "Expected identifier in destructuring import");
		names.push_back(previous().lexeme);
	} while (match(TOK_COMMA));

	consume(TOK_CLOSE_BRACE, "Expected '}' after destructuring names");
	consume(TOK_EQUAL, "Expected '=' after destructuring");
	consume(TOK_IMPORT, "Expected 'import' keyword");
	consume(TOK_OPEN_PAREN, "Expected '(' after 'import'");
	consume(TOK_STRING_LITERAL, "Expected string literal for import path");
	std::string path = previous().lexeme;
	consume(TOK_CLOSE_PAREN, "Expected ')' after import path");
	consume(TOK_SEMI, "Expected ';' after import declaration");

	return std::make_unique<DestructuringImportDeclAST>(std::move(names), path);
}

std::unique_ptr<ModuleAST> Parser::parse() {
	auto module = std::make_unique<ModuleAST>();

	while (!isAtEnd()) {
		// Check if this is an import or struct declaration
		if (check(TOK_CONST)) {
			int saved = current;
			advance();  // consume const

			// Check for destructuring import: const { ... } = import(...)
			if (check(TOK_OPEN_BRACE)) {
				current = saved;
				module->DestructuringImports.push_back(
				    parseDestructuringImport());
				continue;
			}

			// Check for regular import or struct decl: const name = ...
			if (check(TOK_IDENTIFIER)) {
				advance();  // consume identifier
				if (check(TOK_EQUAL)) {
					advance();  // consume =
					if (check(TOK_IMPORT)) {
						// This is an import, reset and parse it
						current = saved;
						module->Imports.push_back(parseImportDecl());
						continue;
					}
					if (check(TOK_STRUCT)) {
						// const Name = struct { ... };
						current = saved;
						module->Structs.push_back(parseStructDecl());
						continue;
					}
				}
			}

			// Not an import or struct, reset and fall through to function parsing
			current = saved;
		}

		// Parse function (fn, extern fn, export fn, pub fn, tfn)
		module->Functions.push_back(parseFunction());
	}

	return module;
}
