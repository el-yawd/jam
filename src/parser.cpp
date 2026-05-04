/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "parser.h"
#include <stdexcept>

Parser::Parser(std::vector<Token> tokens, TypePool &typePool_,
               StringPool &stringPool_, NodeStore &nodes_)
    : tokens(std::move(tokens)), typePool(&typePool_),
      stringPool(&stringPool_), nodes(&nodes_) {}

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

NodeIdx Parser::emit(AstNode n) { return nodes->addNode(n); }

// Walk a chain of MemberAccess nodes back to its root Variable and produce
// the dotted qualified name. Used to resolve `std.fmt.println`-style call
// targets at parse time so the codegen can switch on the full name.
std::string Parser::qualifiedName(NodeIdx chainRoot) const {
	const AstNode &n = nodes->get(chainRoot);
	if (n.tag == AstTag::Variable) {
		return stringPool->get(static_cast<StringIdx>(n.lhs));
	}
	if (n.tag == AstTag::MemberAccess) {
		std::string base = qualifiedName(static_cast<NodeIdx>(n.lhs));
		const std::string &member =
		    stringPool->get(static_cast<StringIdx>(n.rhs));
		return base + "." + member;
	}
	throw std::runtime_error("Invalid member access chain");
}

NodeIdx Parser::parsePrimary() {
	if (match(TOK_NUMBER)) {
		const std::string &numStr = previous().lexeme;
		bool isNegative = !numStr.empty() && numStr[0] == '-';
		uint64_t val;
		if (isNegative) {
			int64_t signedVal = std::stoll(numStr);
			val = static_cast<uint64_t>(-signedVal);
		} else {
			val = std::stoull(numStr);
		}
		AstNode n{AstTag::NumberLit, 0, isNegative ? uint16_t{1} : uint16_t{0},
		          0, static_cast<uint32_t>(val & 0xFFFFFFFFu),
		          static_cast<uint32_t>(val >> 32)};
		return emit(n);
	}
	if (match(TOK_TRUE)) {
		return emit(AstNode{AstTag::BoolLit, 0, 0, 0, 1, 0});
	}
	if (match(TOK_FALSE)) {
		return emit(AstNode{AstTag::BoolLit, 0, 0, 0, 0, 0});
	}
	if (match(TOK_UNDEFINED)) {
		return emit(AstNode{AstTag::UndefinedLit, 0, 0, 0, 0, 0});
	}
	if (match(TOK_STRING_LITERAL)) {
		StringIdx s = stringPool->intern(previous().lexeme);
		return emit(AstNode{AstTag::StringLit, 0, 0, 0, s, 0});
	}
	if (match(TOK_IMPORT)) {
		consume(TOK_OPEN_PAREN, "Expected '(' after 'import'");
		consume(TOK_STRING_LITERAL, "Expected string literal for import path");
		StringIdx path = stringPool->intern(previous().lexeme);
		consume(TOK_CLOSE_PAREN, "Expected ')' after import path");
		return emit(AstNode{AstTag::ImportLit, 0, 0, 0, path, 0});
	}
	if (match(TOK_OPEN_PAREN)) {
		NodeIdx expr = parseLogicalOr();
		consume(TOK_CLOSE_PAREN, "Expected ')' after expression");
		return expr;
	}
	if (match(TOK_OPEN_BRACE)) { return parseStructLiteral(); }
	if (match(TOK_IDENTIFIER)) {
		std::string name = previous().lexeme;
		StringIdx nameId = stringPool->intern(name);
		NodeIdx expr = emit(AstNode{AstTag::Variable, 0, 0, 0, nameId, 0});

		// Member access chain (foo.bar.baz) and pointer deref (.*).
		while (match(TOK_DOT)) {
			if (match(TOK_STAR)) {
				expr = emit(AstNode{AstTag::Deref, 0, 0, 0,
				                    static_cast<uint32_t>(expr), 0});
			} else {
				consume(TOK_IDENTIFIER, "Expected member name after '.'");
				StringIdx mem = stringPool->intern(previous().lexeme);
				expr = emit(AstNode{AstTag::MemberAccess, 0, 0, 0,
				                    static_cast<uint32_t>(expr), mem});
			}
		}

		// Function call.
		if (match(TOK_OPEN_PAREN)) {
			std::vector<NodeIdx> args;
			if (!check(TOK_CLOSE_PAREN)) {
				do {
					args.push_back(parseComparison());
				} while (match(TOK_COMMA));
			}
			consume(TOK_CLOSE_PAREN, "Expected ')' after function arguments");

			std::string callee;
			const AstNode &en = nodes->get(expr);
			if (en.tag == AstTag::MemberAccess) {
				callee = qualifiedName(expr);
			} else {
				callee = std::move(name);
			}
			StringIdx calleeId = stringPool->intern(callee);

			// extra layout: [argCount, arg0, arg1, ...]
			ExtraIdx extra = nodes->reserveExtra(1 + args.size());
			nodes->setExtra(extra, static_cast<uint32_t>(args.size()));
			for (size_t i = 0; i < args.size(); i++) {
				nodes->setExtra(extra + 1 + i, args[i]);
			}
			return emit(
			    AstNode{AstTag::Call, 0, 0, 0, calleeId, extra});
		}

		// Postfix indexing chain: arr[i], arr[i][j], etc.
		while (match(TOK_OPEN_BRACKET)) {
			NodeIdx idx = parseLogicalOr();
			consume(TOK_CLOSE_BRACKET, "Expected ']' after index");
			expr = emit(AstNode{AstTag::Index, 0, 0, 0,
			                    static_cast<uint32_t>(expr),
			                    static_cast<uint32_t>(idx)});
		}

		return expr;
	}

	throw std::runtime_error("Expected primary expression");
}

TypeIdx Parser::parseType() {
	if (match(TOK_STAR)) { return typePool->internPtrSingle(parseType()); }
	if (match(TOK_OPEN_BRACKET)) {
		if (match(TOK_CLOSE_BRACKET)) {
			(void)match(TOK_CONST);
			return typePool->internSlice(parseType());
		}
		if (match(TOK_STAR)) {
			consume(TOK_CLOSE_BRACKET, "Expected ']' after '[*'");
			return typePool->internPtrMany(parseType());
		}
		consume(TOK_NUMBER, "Expected size or ']' after '['");
		uint32_t len = static_cast<uint32_t>(std::stoul(previous().lexeme));
		consume(TOK_CLOSE_BRACKET, "Expected ']' after array size");
		return typePool->internArray(parseType(), len);
	}
	if (match(TOK_CONST)) { return parseType(); }
	if (match(TOK_TYPE)) {
		const std::string &s = previous().lexeme;
		if (s == "u8") return BuiltinType::U8;
		if (s == "i8") return BuiltinType::I8;
		if (s == "u16") return BuiltinType::U16;
		if (s == "i16") return BuiltinType::I16;
		if (s == "u32") return BuiltinType::U32;
		if (s == "i32") return BuiltinType::I32;
		if (s == "u64") return BuiltinType::U64;
		if (s == "i64") return BuiltinType::I64;
		if (s == "f32") return BuiltinType::F32;
		if (s == "f64") return BuiltinType::F64;
		if (s == "bool" || s == "u1") return BuiltinType::Bool;
		if (s == "str") return typePool->internSlice(BuiltinType::U8);
		throw std::runtime_error("Unknown base type: " + s);
	}
	if (match(TOK_IDENTIFIER)) {
		return typePool->internStruct(stringPool->intern(previous().lexeme));
	}
	throw std::runtime_error("Expected type");
}

NodeIdx Parser::parseStructLiteral() {
	// Caller has consumed '{'. Layout in extra:
	//   [fieldCount, name0, expr0, name1, expr1, ...]
	std::vector<std::pair<StringIdx, NodeIdx>> fields;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		consume(TOK_IDENTIFIER, "Expected field name in struct literal");
		StringIdx fieldName = stringPool->intern(previous().lexeme);
		consume(TOK_COLON, "Expected ':' after field name");
		NodeIdx value = parseLogicalOr();
		fields.emplace_back(fieldName, value);
		if (!match(TOK_COMMA)) break;
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close struct literal");

	ExtraIdx extra = nodes->reserveExtra(1 + fields.size() * 2);
	nodes->setExtra(extra, static_cast<uint32_t>(fields.size()));
	for (size_t i = 0; i < fields.size(); i++) {
		nodes->setExtra(extra + 1 + i * 2, fields[i].first);
		nodes->setExtra(extra + 2 + i * 2, fields[i].second);
	}
	// d.lhs holds the struct TypeIdx; codegen fills it from the use site
	// (var-decl target type or enclosing struct field type).
	return emit(AstNode{AstTag::StructLit, 0, 0, 0, kNoType, extra});
}

NodeIdx Parser::parseExpression() {
	if (match(TOK_RETURN)) {
		if (match(TOK_SEMI)) {
			return emit(AstNode{AstTag::Return, 0, 0, 0, kNoNode, 0});
		}
		NodeIdx expr = parseLogicalOr();
		consume(TOK_SEMI, "Expected ';' after return statement");
		return emit(
		    AstNode{AstTag::Return, 0, 0, 0, static_cast<uint32_t>(expr), 0});
	}
	if (match(TOK_CONST) || match(TOK_VAR)) {
		bool isConst = previous().type == TOK_CONST;
		consume(TOK_IDENTIFIER, "Expected variable name");
		StringIdx name = stringPool->intern(previous().lexeme);

		TypeIdx type = BuiltinType::U8;
		if (match(TOK_COLON)) { type = parseType(); }

		consume(TOK_EQUAL,
		        "Expected '=' (use `= undefined` to leave uninitialized)");
		NodeIdx init = parseLogicalOr();
		consume(TOK_SEMI, "Expected ';' after variable declaration");

		// extra layout: [name StringIdx, type TypeIdx, init NodeIdx]
		ExtraIdx extra = nodes->reserveExtra(3);
		nodes->setExtra(extra, name);
		nodes->setExtra(extra + 1, type);
		nodes->setExtra(extra + 2, init);
		return emit(AstNode{AstTag::VarDecl, 0, 0, 0, extra,
		                    isConst ? 1u : 0u});
	}
	if (match(TOK_IF)) {
		consume(TOK_OPEN_PAREN, "Expected '(' after 'if'");
		NodeIdx cond = parseLogicalOr();
		consume(TOK_CLOSE_PAREN, "Expected ')' after if condition");

		consume(TOK_OPEN_BRACE, "Expected '{' after if condition");
		std::vector<NodeIdx> thenBody;
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			thenBody.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected '}' after if body");

		std::vector<NodeIdx> elseBody;
		if (match(TOK_ELSE)) {
			if (check(TOK_IF)) {
				elseBody.push_back(parseExpression());
			} else {
				consume(TOK_OPEN_BRACE, "Expected '{' or 'if' after 'else'");
				while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
					elseBody.push_back(parseExpression());
				}
				consume(TOK_CLOSE_BRACE, "Expected '}' after else body");
			}
		}

		// extra layout: [thenCount, elseCount, then..., else...]
		ExtraIdx extra =
		    nodes->reserveExtra(2 + thenBody.size() + elseBody.size());
		nodes->setExtra(extra, static_cast<uint32_t>(thenBody.size()));
		nodes->setExtra(extra + 1, static_cast<uint32_t>(elseBody.size()));
		for (size_t i = 0; i < thenBody.size(); i++) {
			nodes->setExtra(extra + 2 + i, thenBody[i]);
		}
		for (size_t i = 0; i < elseBody.size(); i++) {
			nodes->setExtra(extra + 2 + thenBody.size() + i, elseBody[i]);
		}
		return emit(AstNode{AstTag::IfNode, 0, 0, 0,
		                    static_cast<uint32_t>(cond), extra});
	}
	if (match(TOK_WHILE)) {
		consume(TOK_OPEN_PAREN, "Expected '(' after 'while'");
		NodeIdx cond = parseLogicalOr();
		consume(TOK_CLOSE_PAREN, "Expected ')' after while condition");

		consume(TOK_OPEN_BRACE, "Expected '{' after while condition");
		std::vector<NodeIdx> body;
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			body.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected '}' after while body");

		ExtraIdx extra = nodes->reserveExtra(1 + body.size());
		nodes->setExtra(extra, static_cast<uint32_t>(body.size()));
		for (size_t i = 0; i < body.size(); i++) {
			nodes->setExtra(extra + 1 + i, body[i]);
		}
		return emit(AstNode{AstTag::WhileNode, 0, 0, 0,
		                    static_cast<uint32_t>(cond), extra});
	}
	if (match(TOK_FOR)) {
		consume(TOK_IDENTIFIER, "Expected variable name after 'for'");
		StringIdx varName = stringPool->intern(previous().lexeme);

		consume(TOK_IN, "Expected 'in' after for variable");
		NodeIdx start = parseComparison();
		consume(TOK_COLON, "Expected ':' in for range");
		NodeIdx end = parseComparison();

		consume(TOK_OPEN_BRACE, "Expected '{' after for range");
		std::vector<NodeIdx> body;
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			body.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected '}' after for body");

		// extra layout: [varName, start, end, bodyCount, body...]
		ExtraIdx extra = nodes->reserveExtra(4 + body.size());
		nodes->setExtra(extra, varName);
		nodes->setExtra(extra + 1, start);
		nodes->setExtra(extra + 2, end);
		nodes->setExtra(extra + 3, static_cast<uint32_t>(body.size()));
		for (size_t i = 0; i < body.size(); i++) {
			nodes->setExtra(extra + 4 + i, body[i]);
		}
		return emit(AstNode{AstTag::ForNode, 0, 0, 0, extra, 0});
	}
	if (match(TOK_BREAK)) {
		consume(TOK_SEMI, "Expected ';' after break");
		return emit(AstNode{AstTag::Break, 0, 0, 0, 0, 0});
	}
	if (match(TOK_CONTINUE)) {
		consume(TOK_SEMI, "Expected ';' after continue");
		return emit(AstNode{AstTag::Continue, 0, 0, 0, 0, 0});
	}
	if (check(TOK_IDENTIFIER)) {
		NodeIdx expr = parseComparison();

		if (match(TOK_EQUAL)) {
			NodeIdx value = parseLogicalOr();
			consume(TOK_SEMI, "Expected ';' after assignment");
			return emit(AstNode{AstTag::Assign, 0, 0, 0,
			                    static_cast<uint32_t>(expr),
			                    static_cast<uint32_t>(value)});
		}

		// Function-call statement requires a trailing semicolon.
		if (nodes->get(expr).tag == AstTag::Call) {
			consume(TOK_SEMI, "Expected ';' after function call");
		}
		return expr;
	}

	return parseLogicalOr();
}

NodeIdx Parser::parseLogicalOr() {
	NodeIdx lhs = parseLogicalAnd();
	while (match(TOK_OR)) {
		NodeIdx rhs = parseLogicalAnd();
		lhs = emit(AstNode{AstTag::BinaryOp,
		                   static_cast<uint8_t>(BinOp::LogOr), 0, 0,
		                   static_cast<uint32_t>(lhs),
		                   static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

NodeIdx Parser::parseLogicalAnd() {
	NodeIdx lhs = parseComparison();
	while (match(TOK_AND)) {
		NodeIdx rhs = parseComparison();
		lhs = emit(AstNode{AstTag::BinaryOp,
		                   static_cast<uint8_t>(BinOp::LogAnd), 0, 0,
		                   static_cast<uint32_t>(lhs),
		                   static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

static BinOp comparisonOp(TokenType t) {
	switch (t) {
	case TOK_EQUAL_EQUAL:   return BinOp::Eq;
	case TOK_NOT_EQUAL:     return BinOp::Ne;
	case TOK_LESS:          return BinOp::Lt;
	case TOK_LESS_EQUAL:    return BinOp::Le;
	case TOK_GREATER:       return BinOp::Gt;
	case TOK_GREATER_EQUAL: return BinOp::Ge;
	default:                return BinOp::Invalid;
	}
}

NodeIdx Parser::parseComparison() {
	NodeIdx lhs = parseBitwise();
	if (check(TOK_EQUAL_EQUAL) || check(TOK_NOT_EQUAL) || check(TOK_LESS) ||
	    check(TOK_LESS_EQUAL) || check(TOK_GREATER) ||
	    check(TOK_GREATER_EQUAL)) {
		Token op = advance();
		NodeIdx rhs = parseBitwise();
		BinOp k = comparisonOp(op.type);
		return emit(AstNode{AstTag::BinaryOp, static_cast<uint8_t>(k), 0, 0,
		                    static_cast<uint32_t>(lhs),
		                    static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

NodeIdx Parser::parseBitwise() {
	NodeIdx lhs = parseShift();
	while (true) {
		BinOp k = BinOp::Invalid;
		if (match(TOK_AMP)) k = BinOp::BitAnd;
		else if (match(TOK_PIPE)) k = BinOp::BitOr;
		else if (match(TOK_CARET)) k = BinOp::BitXor;
		else break;
		NodeIdx rhs = parseShift();
		lhs = emit(AstNode{AstTag::BinaryOp, static_cast<uint8_t>(k), 0, 0,
		                   static_cast<uint32_t>(lhs),
		                   static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

NodeIdx Parser::parseShift() {
	NodeIdx lhs = parseAddition();
	while (true) {
		BinOp k = BinOp::Invalid;
		if (match(TOK_LSHIFT)) k = BinOp::Shl;
		else if (match(TOK_RSHIFT)) k = BinOp::Shr;
		else break;
		NodeIdx rhs = parseAddition();
		lhs = emit(AstNode{AstTag::BinaryOp, static_cast<uint8_t>(k), 0, 0,
		                   static_cast<uint32_t>(lhs),
		                   static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

NodeIdx Parser::parseAddition() {
	NodeIdx lhs = parseMultiplication();
	while (true) {
		BinOp k = BinOp::Invalid;
		if (match(TOK_PLUS)) k = BinOp::Add;
		else if (match(TOK_MINUS)) k = BinOp::Sub;
		else break;
		NodeIdx rhs = parseMultiplication();
		lhs = emit(AstNode{AstTag::BinaryOp, static_cast<uint8_t>(k), 0, 0,
		                   static_cast<uint32_t>(lhs),
		                   static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

NodeIdx Parser::parseMultiplication() {
	NodeIdx lhs = parseUnary();
	while (true) {
		BinOp k = BinOp::Invalid;
		if (match(TOK_STAR)) k = BinOp::Mul;
		else if (match(TOK_PERCENT)) k = BinOp::Mod;
		else break;
		NodeIdx rhs = parseUnary();
		lhs = emit(AstNode{AstTag::BinaryOp, static_cast<uint8_t>(k), 0, 0,
		                   static_cast<uint32_t>(lhs),
		                   static_cast<uint32_t>(rhs)});
	}
	return lhs;
}

NodeIdx Parser::parseUnary() {
	if (match(TOK_NOT)) {
		NodeIdx operand = parseUnary();
		return emit(AstNode{AstTag::UnaryOp,
		                    static_cast<uint8_t>(UnaryOp::LogNot), 0, 0,
		                    static_cast<uint32_t>(operand), 0});
	}
	if (match(TOK_TILDE)) {
		NodeIdx operand = parseUnary();
		return emit(AstNode{AstTag::UnaryOp,
		                    static_cast<uint8_t>(UnaryOp::BitNot), 0, 0,
		                    static_cast<uint32_t>(operand), 0});
	}
	if (match(TOK_AMP)) {
		NodeIdx operand = parseUnary();
		return emit(AstNode{AstTag::AddressOf, 0, 0, 0,
		                    static_cast<uint32_t>(operand), 0});
	}
	if (match(TOK_MINUS)) {
		NodeIdx operand = parseUnary();
		return emit(AstNode{AstTag::UnaryOp,
		                    static_cast<uint8_t>(UnaryOp::Neg), 0, 0,
		                    static_cast<uint32_t>(operand), 0});
	}
	return parsePrimary();
}

std::unique_ptr<FunctionAST> Parser::parseFunction() {
	bool isExtern = false;
	bool isExport = false;
	bool isPub = false;
	bool isTest = false;

	if (match(TOK_EXTERN)) isExtern = true;
	else if (match(TOK_EXPORT)) isExport = true;
	else if (match(TOK_PUB)) isPub = true;
	else if (match(TOK_TFN)) isTest = true;

	if (!isTest) { consume(TOK_FN, "Expected 'fn' keyword"); }
	consume(TOK_IDENTIFIER, "Expected function name");
	std::string name = previous().lexeme;

	consume(TOK_OPEN_PAREN, "Expected '(' after function name");

	std::vector<std::pair<std::string, TypeIdx>> args;
	bool isVarArgs = false;
	if (!check(TOK_CLOSE_PAREN)) {
		do {
			if (match(TOK_ELLIPSIS)) {
				if (!isExtern) {
					throw std::runtime_error(
					    "`...` is only allowed in extern fn declarations");
				}
				isVarArgs = true;
				break;
			}
			consume(TOK_IDENTIFIER, "Expected parameter name");
			std::string paramName = previous().lexeme;

			consume(TOK_COLON, "Expected ':' after parameter name");
			TypeIdx paramType = parseType();
			args.emplace_back(std::move(paramName), paramType);
		} while (match(TOK_COMMA));
	}

	consume(TOK_CLOSE_PAREN, "Expected ')' after parameters");

	TypeIdx returnType = kNoType;
	if (check(TOK_TYPE) || check(TOK_OPEN_BRACKET) || check(TOK_CONST) ||
	    check(TOK_IDENTIFIER) || check(TOK_STAR)) {
		returnType = parseType();
	}

	if (isExtern) {
		consume(TOK_SEMI, "Expected ';' after extern function declaration");
		return std::make_unique<FunctionAST>(name, std::move(args), returnType,
		                                     std::vector<NodeIdx>{}, true,
		                                     false, false, false, isVarArgs);
	}

	consume(TOK_OPEN_BRACE, "Expected '{' before function body");

	std::vector<NodeIdx> body;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		body.push_back(parseExpression());
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' after function body");

	return std::make_unique<FunctionAST>(name, std::move(args), returnType,
	                                     std::move(body), false, isExport,
	                                     isPub, isTest, false);
}

std::unique_ptr<StructDeclAST> Parser::parseStructDecl() {
	consume(TOK_CONST, "Expected 'const' for struct declaration");
	consume(TOK_IDENTIFIER, "Expected struct name");
	std::string name = previous().lexeme;
	consume(TOK_EQUAL, "Expected '=' after struct name");
	consume(TOK_STRUCT, "Expected 'struct' keyword");
	consume(TOK_OPEN_BRACE, "Expected '{' after 'struct'");

	std::vector<std::pair<std::string, TypeIdx>> fields;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		consume(TOK_IDENTIFIER, "Expected field name");
		std::string fieldName = previous().lexeme;
		consume(TOK_COLON, "Expected ':' after field name");
		TypeIdx fieldType = parseType();
		fields.emplace_back(std::move(fieldName), fieldType);
		if (!match(TOK_COMMA)) break;
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close struct definition");
	consume(TOK_SEMI, "Expected ';' after struct declaration");

	return std::make_unique<StructDeclAST>(name, std::move(fields));
}

std::unique_ptr<ImportDeclAST> Parser::parseImportDecl() {
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
		if (check(TOK_CONST)) {
			int saved = current;
			advance();

			if (check(TOK_OPEN_BRACE)) {
				current = saved;
				module->DestructuringImports.push_back(
				    parseDestructuringImport());
				continue;
			}

			if (check(TOK_IDENTIFIER)) {
				advance();
				if (check(TOK_EQUAL)) {
					advance();
					if (check(TOK_IMPORT)) {
						current = saved;
						module->Imports.push_back(parseImportDecl());
						continue;
					}
					if (check(TOK_STRUCT)) {
						current = saved;
						module->Structs.push_back(parseStructDecl());
						continue;
					}
				}
			}

			current = saved;
		}

		module->Functions.push_back(parseFunction());
	}

	return module;
}
