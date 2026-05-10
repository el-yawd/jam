/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "parser.h"
#include "number_literal.h"
#include <stdexcept>

// Validate a number lexeme via the dedicated validator
// (number_literal.cpp). Strips a leading `-` sign before delegating —
// negation is the caller's responsibility, since it depends on context
// (negative literal vs. unary minus on a literal).
//
// On non-int results (float, big_int, validation failure), throws a
// runtime_error with a descriptive message. The validator's rich error
// vocabulary surfaces here unchanged.
static uint64_t parseNumLexeme(const std::string &s, bool &isNegOut) {
	bool neg = !s.empty() && s[0] == '-';
	const std::string &abs = neg ? s.substr(1) : s;
	isNegOut = neg;

	NumberResult r = parseNumberLiteral(abs);
	switch (r.kind) {
	case NumberResultKind::Int:
		return r.intValue;
	case NumberResultKind::BigInt:
		throw std::runtime_error(
		    "integer literal `" + abs + "` exceeds u64 range");
	case NumberResultKind::Float:
		throw std::runtime_error(
		    "float literal `" + abs +
		    "` is not yet supported (only integer literals)");
	case NumberResultKind::Failure:
		throw std::runtime_error(
		    std::string("invalid numeric literal `") + abs + "`: " +
		    numberErrorMessage(r.failure.kind));
	}
	throw std::runtime_error("unreachable");
}

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

NodeIdx Parser::emit(AstNode n) {
	// Most call sites pass mainToken=0; default to "the most recently
	// consumed token" so error messages report meaningful line numbers
	// without forcing every emit site to thread the token index. Sites
	// that need a specific token (e.g. a construct's start keyword
	// rather than its closing punctuation) can set mainToken explicitly.
	if (n.mainToken == 0 && current > 0) {
		n.mainToken = static_cast<uint32_t>(current - 1);
	}
	return nodes->addNode(n);
}

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
	// `match (…) { … }` is also valid in expression position so it can
	// produce a value (M3). The same call works for both statement and
	// expression forms; the codegen builds a phi over arm values.
	if (check(TOK_MATCH)) { return parseMatch(); }
	if (match(TOK_NUMBER)) {
		// `parseNumLexeme` returns the magnitude; the sign is recorded in
		// the node's flags bit 0 and the codegen applies negation. This
		// matches the convention established before hex-literal support
		// landed.
		bool isNegative = false;
		uint64_t mag = parseNumLexeme(previous().lexeme, isNegative);
		AstNode n{AstTag::NumberLit, 0, isNegative ? uint16_t{1} : uint16_t{0},
		          0, static_cast<uint32_t>(mag & 0xFFFFFFFFu),
		          static_cast<uint32_t>(mag >> 32)};
		return emit(n);
	}
	if (match(TOK_TRUE)) {
		return emit(AstNode{AstTag::BoolLit, 0, 0, 0, 1, 0});
	}
	if (match(TOK_FALSE)) {
		return emit(AstNode{AstTag::BoolLit, 0, 0, 0, 0, 0});
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
	if (match(TOK_OPEN_BRACKET)) {
		// Array literal `[a, b, c]`, array repeat `[expr; N]`, or empty
		// `[]` (well-typed only against a slice or zero-length array
		// target — codegen rejects others).
		if (match(TOK_CLOSE_BRACKET)) {
			ExtraIdx extra = nodes->reserveExtra(1);
			nodes->setExtra(extra, 0);
			return emit(AstNode{AstTag::ArrayLit, 0, 0, 0, kNoType,
			                    extra});
		}
		NodeIdx first = parseLogicalOr();
		if (match(TOK_SEMI)) {
			NodeIdx count = parseLogicalOr();
			consume(TOK_CLOSE_BRACKET,
			        "Expected `]` after array repeat count");
			ExtraIdx extra = nodes->reserveExtra(2);
			nodes->setExtra(extra, static_cast<uint32_t>(first));
			nodes->setExtra(extra + 1, static_cast<uint32_t>(count));
			return emit(AstNode{AstTag::ArrayRepeat, 0, 0, 0, kNoType,
			                    extra});
		}
		std::vector<NodeIdx> elems;
		elems.push_back(first);
		while (match(TOK_COMMA)) {
			if (check(TOK_CLOSE_BRACKET)) break;  // trailing comma
			elems.push_back(parseLogicalOr());
		}
		consume(TOK_CLOSE_BRACKET,
		        "Expected `]` or `,` in array literal");
		ExtraIdx extra = nodes->reserveExtra(1 + elems.size());
		nodes->setExtra(extra, static_cast<uint32_t>(elems.size()));
		for (size_t i = 0; i < elems.size(); i++) {
			nodes->setExtra(extra + 1 + i, elems[i]);
		}
		return emit(AstNode{AstTag::ArrayLit, 0, 0, 0, kNoType, extra});
	}
	// Generics G2: `struct { ... }` as an expression evaluates to a value
	// of type `type` — the struct type being constructed. Used as the
	// body of a generic type-returning function.
	if (match(TOK_STRUCT)) { return parseStructExpression(); }
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

// Type grammar:
//
//   *const T     — single-item pointer, read-only pointee
//   *mut T       — single-item pointer, writable pointee
//   *const[] T   — many-item pointer, read-only pointee
//   *mut[] T     — many-item pointer, writable pointee
//   []T          — slice (mutability follows the binding)
//   [N]T         — fixed-size array (mutability follows the binding;
//                  `N` is part of the type)
//
// Pointer types require a `const` or `mut` qualifier (the type
// alone cannot say whether the pointee is writable; the binding's
// `var` / `const` only governs reassignment of the pointer itself).
// An optional `[]` between the qualifier and the element type promotes
// the pointer to many-item form.
//
// Slices `[]T` and fixed arrays `[N]T` take no qualifier — their
// element mutability follows the binding (`var x: []u8` permits
// `x[i] = …`; `const x: []u8` does not).
TypeIdx Parser::parseType() {
	if (match(TOK_STAR)) {
		bool ptrConst = false;
		if (match(TOK_CONST)) {
			ptrConst = true;
		} else if (!match(TOK_MUT)) {
			throw std::runtime_error(
			    "Expected `const` or `mut` after `*` (e.g. `*const T`, "
			    "`*mut T`)");
		}
		// Optional `[]` promotes to many-item form. We need to commit to
		// "many" only when the next two tokens are exactly `[ ]`; a `[`
		// followed by a number is the start of a `[N]T` element type
		// (single-item pointer to a fixed array).
		bool isMany = false;
		if (check(TOK_OPEN_BRACKET)) {
			int saved = current;
			advance();  // consume `[`
			if (match(TOK_CLOSE_BRACKET)) {
				isMany = true;
			} else {
				current = saved;  // rewind: `[` belongs to element type
			}
		}
		TypeIdx elem = parseType();
		(void)ptrConst;  // mutability not enforced yet
		return isMany ? typePool->internPtrMany(elem)
		             : typePool->internPtrSingle(elem);
	}
	if (match(TOK_OPEN_BRACKET)) {
		// `[]T` — slice. No tag.
		if (match(TOK_CLOSE_BRACKET)) {
			return typePool->internSlice(parseType());
		}
		// `[N]T` — fixed-size array. No tag.
		consume(TOK_NUMBER, "Expected size or `]` after `[`");
		uint32_t len = static_cast<uint32_t>(std::stoul(previous().lexeme));
		consume(TOK_CLOSE_BRACKET, "Expected `]` after array size");
		return typePool->internArray(parseType(), len);
	}
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
		// Generics G1: `type` resolves to the singleton meta-type. A
		// parameter of this type accepts a TypeIdx at compile time.
		if (s == "type") return BuiltinType::Type;
		throw std::runtime_error("Unknown base type: " + s);
	}
	if (match(TOK_IDENTIFIER)) {
		const std::string &ident = previous().lexeme;
		// Generics G3: `Self` resolves to the enclosing struct's name —
		// the top of the parser's struct-context stack. Outside any
		// struct body it's an error.
		if (ident == "Self") {
			if (structContextStack.empty()) {
				throw std::runtime_error(
				    "`Self` is only valid inside a struct body");
			}
			return typePool->internNamed(
			    stringPool->intern(structContextStack.back()));
		}
		// Generics G4: `Identifier(arg, ...)` in a type position is a
		// generic instantiation. The args are parsed recursively as
		// types; the result is a `TypeKind::GenericCall` TypeIdx that
		// the codegen resolves on demand via the substitution engine.
		if (check(TOK_OPEN_PAREN)) {
			advance();  // consume `(`
			std::vector<TypeIdx> args;
			if (!check(TOK_CLOSE_PAREN)) {
				do {
					args.push_back(parseType());
				} while (match(TOK_COMMA));
			}
			consume(TOK_CLOSE_PAREN,
			        "Expected ')' after generic type arguments");
			return typePool->internGenericCall(
			    stringPool->intern(ident), std::move(args));
		}
		// User-named types (struct / union / enum) are interned with
		// kind = Named; codegen resolves to the concrete kind via the
		// declaration registries. The parser does not need to know
		// which kind the user meant.
		return typePool->internNamed(stringPool->intern(ident));
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

// Parse a single atom pattern: integer literal, inclusive range,
// enum-variant (`Color.Red`), or wildcard. Or-patterns are handled by
// parsePattern.
NodeIdx Parser::parsePatternAtom() {
	// `Identifier '.' Identifier` — enum-variant pattern, optionally
	// followed by `(binding1, binding2, ...)` to destructure payload
	// fields. Must come before the literal/wildcard branches.
	if (check(TOK_IDENTIFIER) && peek().lexeme != "_") {
		int saved = current;
		advance();  // consume enum name
		if (match(TOK_DOT)) {
			consume(TOK_IDENTIFIER, "Expected variant name after `.`");
			StringIdx enumNameId =
			    stringPool->intern(tokens[saved].lexeme);
			StringIdx variantNameId =
			    stringPool->intern(previous().lexeme);

			// Optional payload binding: `(name1, name2, ...)`. Empty
			// list `()` is permitted and equivalent to no parens.
			if (match(TOK_OPEN_PAREN)) {
				std::vector<StringIdx> bindings;
				if (!check(TOK_CLOSE_PAREN)) {
					do {
						consume(TOK_IDENTIFIER,
						        "Expected binding name in variant payload");
						bindings.push_back(
						    stringPool->intern(previous().lexeme));
					} while (match(TOK_COMMA));
				}
				consume(TOK_CLOSE_PAREN,
				        "Expected `)` to close payload bindings");
				// Pack as: extra = [enumNameId, variantNameId, count,
				// binding0, binding1, ...]. lhs = extra-idx; rhs = 1
				// to mark "with bindings".
				ExtraIdx extra = nodes->reserveExtra(3 + bindings.size());
				nodes->setExtra(extra, enumNameId);
				nodes->setExtra(extra + 1, variantNameId);
				nodes->setExtra(extra + 2,
				                static_cast<uint32_t>(bindings.size()));
				for (size_t i = 0; i < bindings.size(); i++) {
					nodes->setExtra(extra + 3 + i, bindings[i]);
				}
				return emit(AstNode{AstTag::PatEnumVariant, 0, 1u, 0,
				                    extra, 0});
			}
			// No bindings: lhs = enumNameId, rhs = variantNameId,
			// flags = 0.
			return emit(AstNode{AstTag::PatEnumVariant, 0, 0, 0,
			                    enumNameId, variantNameId});
		}
		current = saved;
		throw std::runtime_error(
		    "Bare identifier patterns are not yet supported "
		    "(use `EnumName.Variant`, an integer literal, or `_`)");
	}
	if (match(TOK_NUMBER)) {
		bool isNegative = false;
		uint64_t lo = parseNumLexeme(previous().lexeme, isNegative);
		// Inclusive range `lo..=hi`?
		if (match(TOK_DOTDOT_EQ)) {
			consume(TOK_NUMBER, "Expected upper bound after `..=`");
			bool hiNeg = false;
			uint64_t hi = parseNumLexeme(previous().lexeme, hiNeg);
			// M1: range bounds fit in u32; truncate gracefully.
			return emit(AstNode{AstTag::PatRange, 0, 0, 0,
			                    static_cast<uint32_t>(lo & 0xFFFFFFFFu),
			                    static_cast<uint32_t>(hi & 0xFFFFFFFFu)});
		}
		uint16_t flags = isNegative ? 1u : 0u;
		return emit(AstNode{AstTag::PatLit, 0, flags, 0,
		                    static_cast<uint32_t>(lo & 0xFFFFFFFFu),
		                    static_cast<uint32_t>(lo >> 32)});
	}
	// Char literal — TOK_STRING_LITERAL is currently the only string-y
	// token; for M1 we treat single-quote chars as TOK_NUMBER via the
	// lexer in a future patch. For now, only TOK_NUMBER is accepted.
	if (match(TOK_STRING_LITERAL)) {
		throw std::runtime_error(
		    "Char literals in patterns are not yet supported (M1)");
	}
	// Wildcard `_` is lexed as TOK_IDENTIFIER; recognize it here.
	if (check(TOK_IDENTIFIER) && peek().lexeme == "_") {
		advance();
		return emit(AstNode{AstTag::PatWildcard, 0, 0, 0, 0, 0});
	}
	throw std::runtime_error(
	    "Expected pattern (integer literal, range, or `_`)");
}

// Parse an or-pattern: A | B | C. Returns a single PatLit/PatRange/
// PatWildcard if no `|` is present, else a PatOr wrapping the list.
NodeIdx Parser::parsePattern() {
	NodeIdx first = parsePatternAtom();
	if (!check(TOK_PIPE)) { return first; }
	std::vector<NodeIdx> alternatives;
	alternatives.push_back(first);
	while (match(TOK_PIPE)) {
		alternatives.push_back(parsePatternAtom());
	}
	ExtraIdx extra = nodes->reserveExtra(1 + alternatives.size());
	nodes->setExtra(extra, static_cast<uint32_t>(alternatives.size()));
	for (size_t i = 0; i < alternatives.size(); i++) {
		nodes->setExtra(extra + 1 + i, alternatives[i]);
	}
	return emit(AstNode{AstTag::PatOr, 0, 0, 0, extra, 0});
}

// Parse a match statement: `match (expr) { Pattern Block ... }`.
// The catch-all is the wildcard pattern `_`; there is no `else` arm.
// Layout in extra:
//   [armCount,
//    arm0_patIdx, arm0_bodyCount, arm0_body...,
//    arm1_patIdx, arm1_bodyCount, arm1_body..., ...]
NodeIdx Parser::parseMatch() {
	consume(TOK_MATCH, "Expected `match`");
	consume(TOK_OPEN_PAREN, "Expected `(` after `match`");
	NodeIdx scrutinee = parseLogicalOr();
	consume(TOK_CLOSE_PAREN, "Expected `)` after match scrutinee");
	consume(TOK_OPEN_BRACE, "Expected `{` to begin match body");

	struct Arm {
		NodeIdx pat;
		std::vector<NodeIdx> body;
	};
	std::vector<Arm> arms;

	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		Arm arm;
		arm.pat = parsePattern();
		consume(TOK_OPEN_BRACE, "Expected `{` to begin arm body");
		while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
			arm.body.push_back(parseExpression());
		}
		consume(TOK_CLOSE_BRACE, "Expected `}` to close arm body");
		arms.push_back(std::move(arm));
	}
	consume(TOK_CLOSE_BRACE, "Expected `}` to close match body");

	// Compute total extra size and pack the arms.
	size_t total = 1;  // armCount
	for (const Arm &a : arms) {
		total += 2 + a.body.size();  // patIdx + bodyCount + body...
	}

	ExtraIdx extra = nodes->reserveExtra(total);
	uint32_t pos = 0;
	nodes->setExtra(extra + pos++, static_cast<uint32_t>(arms.size()));
	for (const Arm &a : arms) {
		nodes->setExtra(extra + pos++, a.pat);
		nodes->setExtra(extra + pos++,
		                static_cast<uint32_t>(a.body.size()));
		for (NodeIdx s : a.body) { nodes->setExtra(extra + pos++, s); }
	}

	return emit(AstNode{AstTag::MatchNode, 0, 0, 0,
	                    static_cast<uint32_t>(scrutinee), extra});
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
		        "Expected '=' (every variable must be initialized at "
		        "declaration)");
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
	if (check(TOK_MATCH)) { return parseMatch(); }
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

// Parse a postfix `as Type` chain on top of any unary expression. The
// cast binds tightly — `5 + (x as u32)` not `(5 + x) as u32` — matching
// the convention from C/Rust/Zig where `as` sits just above primary
// expressions.
static NodeIdx parseAsChain(Parser *self, NodeIdx expr,
                             NodeStore &nodes,
                             bool (Parser::*matchTok)(TokenType),
                             TypeIdx (Parser::*parseType)());
// Forward declaration removed; we just inline the postfix loop below.

NodeIdx Parser::parseUnary() {
	auto wrapAs = [&](NodeIdx e) {
		while (match(TOK_AS)) {
			TypeIdx ty = parseType();
			e = emit(AstNode{AstTag::AsCast, 0, 0, 0,
			                  static_cast<uint32_t>(e), ty});
		}
		return e;
	};

	if (match(TOK_NOT)) {
		NodeIdx operand = parseUnary();
		return wrapAs(emit(AstNode{
		    AstTag::UnaryOp, static_cast<uint8_t>(UnaryOp::LogNot), 0,
		    0, static_cast<uint32_t>(operand), 0}));
	}
	if (match(TOK_TILDE)) {
		NodeIdx operand = parseUnary();
		return wrapAs(emit(AstNode{
		    AstTag::UnaryOp, static_cast<uint8_t>(UnaryOp::BitNot), 0,
		    0, static_cast<uint32_t>(operand), 0}));
	}
	if (match(TOK_AMP)) {
		NodeIdx operand = parseUnary();
		return wrapAs(emit(AstNode{AstTag::AddressOf, 0, 0, 0,
		                            static_cast<uint32_t>(operand), 0}));
	}
	if (match(TOK_MINUS)) {
		NodeIdx operand = parseUnary();
		return wrapAs(emit(AstNode{
		    AstTag::UnaryOp, static_cast<uint8_t>(UnaryOp::Neg), 0, 0,
		    static_cast<uint32_t>(operand), 0}));
	}
	return wrapAs(parsePrimary());
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

	std::vector<Param> args;
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

			// Optional mode keyword between `:` and the type.
			//   x: u32             — Let (default, read-only)
			//   x: mut u32         — Mut (exclusive read-write)
			//   x: move List       — Move (consume ownership)
			//
			// `*mut T` pointer types start with `*`, not `mut`, so there is
			// no conflict with the existing pointer syntax. See MVS.md §2.
			ParamMode mode = ParamMode::Let;
			if (match(TOK_MUT)) {
				mode = ParamMode::Mut;
			} else if (match(TOK_MOVE)) {
				mode = ParamMode::Move;
			}

			TypeIdx paramType = parseType();
			args.push_back(Param{std::move(paramName), paramType, mode});
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

void Parser::parseStructBody(
    std::vector<std::pair<std::string, TypeIdx>> &fields,
    std::vector<std::unique_ptr<FunctionAST>> &methods) {
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		// Method: `fn name(self: ..., ...) ReturnType { body }`. Methods
		// can appear in any order relative to fields. parseFunction
		// consumes the `fn` keyword itself.
		if (check(TOK_FN)) {
			methods.push_back(parseFunction());
			match(TOK_COMMA);  // optional trailing comma after a method
			continue;
		}
		// Field: `name: Type`. Comma separates from the next member.
		consume(TOK_IDENTIFIER, "Expected field name or 'fn'");
		std::string fieldName = previous().lexeme;
		consume(TOK_COLON, "Expected ':' after field name");
		TypeIdx fieldType = parseType();
		fields.emplace_back(std::move(fieldName), fieldType);
		if (!check(TOK_CLOSE_BRACE)) {
			consume(TOK_COMMA,
			        "Expected ',' or '}' after struct field");
		}
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close struct definition");
}

std::unique_ptr<StructDeclAST> Parser::parseStructDecl() {
	consume(TOK_CONST, "Expected 'const' for struct declaration");
	consume(TOK_IDENTIFIER, "Expected struct name");
	std::string name = previous().lexeme;
	consume(TOK_EQUAL, "Expected '=' after struct name");
	consume(TOK_STRUCT, "Expected 'struct' keyword");
	consume(TOK_OPEN_BRACE, "Expected '{' after 'struct'");

	std::vector<std::pair<std::string, TypeIdx>> fields;
	std::vector<std::unique_ptr<FunctionAST>> methods;
	structContextStack.push_back(name);  // G3: Self resolves to this name
	parseStructBody(fields, methods);
	structContextStack.pop_back();
	consume(TOK_SEMI, "Expected ';' after struct declaration");

	return std::make_unique<StructDeclAST>(name, std::move(fields),
	                                       std::move(methods));
}

NodeIdx Parser::parseStructExpression() {
	// Caller has matched the `struct` keyword. Anonymous struct: synthetic
	// name keyed by index in anonStructs (the substitution engine in G4
	// reads from there to instantiate the body with concrete type args).
	consume(TOK_OPEN_BRACE, "Expected '{' after 'struct'");
	uint32_t idx = static_cast<uint32_t>(anonStructs.size());
	std::string name = "__anon_struct_" + std::to_string(idx);

	std::vector<std::pair<std::string, TypeIdx>> fields;
	std::vector<std::unique_ptr<FunctionAST>> methods;
	// G3: Self inside this anonymous struct's body resolves to the
	// synthetic name. The substitution engine in G4 will rewrite that
	// name to the per-instantiation struct name.
	structContextStack.push_back(name);
	parseStructBody(fields, methods);
	structContextStack.pop_back();

	anonStructs.push_back(std::make_unique<StructDeclAST>(
	    std::move(name), std::move(fields), std::move(methods)));

	return emit(AstNode{AstTag::StructExpr, 0, 0, 0, idx, 0});
}

// Parse `const Name = enum { Variant1, Variant2(T1, T2), ... };`.
// Variants get sequential discriminant values starting from zero in
// declaration order. Unit variants (no payload) and tagged variants
// (positional payload types) coexist — the parser accepts both.
std::unique_ptr<EnumDeclAST> Parser::parseEnumDecl() {
	consume(TOK_CONST, "Expected 'const' for enum declaration");
	consume(TOK_IDENTIFIER, "Expected enum name");
	std::string name = previous().lexeme;
	consume(TOK_EQUAL, "Expected '=' after enum name");
	consume(TOK_ENUM, "Expected 'enum' keyword");
	consume(TOK_OPEN_BRACE, "Expected '{' after 'enum'");

	std::vector<EnumVariantAST> variants;
	uint32_t nextDiscrim = 0;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		consume(TOK_IDENTIFIER, "Expected enum variant name");
		EnumVariantAST v;
		v.Name = previous().lexeme;
		// Optional payload: `Variant(T1, T2, ...)`. Unit variants omit
		// the parenthesized list.
		if (match(TOK_OPEN_PAREN)) {
			if (!check(TOK_CLOSE_PAREN)) {
				do {
					v.PayloadTypes.push_back(parseType());
				} while (match(TOK_COMMA));
			}
			consume(TOK_CLOSE_PAREN,
			        "Expected `)` to close variant payload list");
		}
		// Optional explicit discriminant: `Variant = N` or
		// `Variant(payload) = N`. The supplied integer overrides the
		// running counter; subsequent variants resume from N + 1.
		if (match(TOK_EQUAL)) {
			consume(TOK_NUMBER,
			        "Expected integer literal after `=` in enum variant");
			bool neg = false;
			uint64_t mag = 0;
			try {
				NumberResult r = parseNumberLiteral(previous().lexeme);
				if (r.kind != NumberResultKind::Int) {
					throw std::runtime_error(
					    "Enum discriminant must be a non-negative integer");
				}
				mag = r.intValue;
			} catch (const std::exception &e) {
				throw std::runtime_error(
				    std::string("Invalid enum discriminant: ") + e.what());
			}
			(void)neg;
			if (mag > 255) {
				throw std::runtime_error(
				    "Enum discriminant " + std::to_string(mag) +
				    " is out of range; M2 enums are u8-tagged");
			}
			v.Discriminant = static_cast<uint32_t>(mag);
			nextDiscrim = v.Discriminant + 1;
		} else {
			v.Discriminant = nextDiscrim++;
		}
		variants.push_back(std::move(v));
		if (!match(TOK_COMMA)) break;
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close enum definition");
	consume(TOK_SEMI, "Expected ';' after enum declaration");

	if (variants.empty()) {
		throw std::runtime_error(
		    "Enum `" + name + "` must declare at least one variant");
	}
	if (variants.size() > 256) {
		throw std::runtime_error(
		    "Enum `" + name +
		    "` has more than 256 variants; M2 enums are u8-tagged");
	}
	return std::make_unique<EnumDeclAST>(name, std::move(variants));
}

// Parse `const Name = union { f1: T1, f2: T2, ... };`. Same shape as
// parseStructDecl — only the keyword differs.
std::unique_ptr<UnionDeclAST> Parser::parseUnionDecl() {
	consume(TOK_CONST, "Expected 'const' for union declaration");
	consume(TOK_IDENTIFIER, "Expected union name");
	std::string name = previous().lexeme;
	consume(TOK_EQUAL, "Expected '=' after union name");
	consume(TOK_UNION, "Expected 'union' keyword");
	consume(TOK_OPEN_BRACE, "Expected '{' after 'union'");

	std::vector<std::pair<std::string, TypeIdx>> fields;
	while (!check(TOK_CLOSE_BRACE) && !isAtEnd()) {
		consume(TOK_IDENTIFIER, "Expected union field name");
		std::string fieldName = previous().lexeme;
		consume(TOK_COLON, "Expected ':' after field name");
		TypeIdx fieldType = parseType();
		fields.emplace_back(std::move(fieldName), fieldType);
		if (!match(TOK_COMMA)) break;
	}
	consume(TOK_CLOSE_BRACE, "Expected '}' to close union definition");
	consume(TOK_SEMI, "Expected ';' after union declaration");

	return std::make_unique<UnionDeclAST>(name, std::move(fields));
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

std::unique_ptr<ConstDeclAST> Parser::parseConstDecl() {
	consume(TOK_CONST, "Expected 'const' for module-scope constant");
	consume(TOK_IDENTIFIER, "Expected identifier for constant name");
	std::string name = previous().lexeme;

	TypeIdx declared = kNoType;
	if (match(TOK_COLON)) { declared = parseType(); }

	consume(TOK_EQUAL, "Expected '=' in module-scope const declaration");

	// Generics G4: try-parse the RHS as a type. If it parses as a
	// `TypeKind::GenericCall` (e.g. `Box(i32)`) and is followed by a
	// semicolon, treat the const as a type alias. Anything else
	// rewinds and parses as a value expression. Save/restore the
	// parser cursor since parseType can throw on non-type inputs.
	int saved = current;
	TypeIdx aliased = kNoType;
	try {
		TypeIdx maybeType = parseType();
		if (typePool->get(maybeType).kind == TypeKind::GenericCall &&
		    check(TOK_SEMI)) {
			aliased = maybeType;
		} else {
			current = saved;
		}
	} catch (...) {
		current = saved;
	}

	if (aliased != kNoType) {
		consume(TOK_SEMI,
		        "Expected ';' after module-scope const declaration");
		auto decl = std::make_unique<ConstDeclAST>(std::move(name),
		                                           declared, kNoNode);
		decl->AliasedType = aliased;
		return decl;
	}

	NodeIdx init = parseLogicalOr();
	consume(TOK_SEMI, "Expected ';' after module-scope const declaration");

	return std::make_unique<ConstDeclAST>(std::move(name), declared, init);
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
				// `const NAME : T = …` — typed module const.
				if (check(TOK_COLON)) {
					current = saved;
					module->Consts.push_back(parseConstDecl());
					continue;
				}
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
					if (check(TOK_UNION)) {
						current = saved;
						module->Unions.push_back(parseUnionDecl());
						continue;
					}
					if (check(TOK_ENUM)) {
						current = saved;
						module->Enums.push_back(parseEnumDecl());
						continue;
					}
					// `const NAME = expr;` — untyped module const,
					// type inferred from the initializer.
					current = saved;
					module->Consts.push_back(parseConstDecl());
					continue;
				}
			}

			current = saved;
		}

		module->Functions.push_back(parseFunction());
	}

	// Generics G2: hand off anonymous struct bodies (`struct {...}`
	// expressions encountered during parse) to the ModuleAST so the
	// instantiation engine can find them by index.
	module->AnonStructs = std::move(anonStructs);

	return module;
}
