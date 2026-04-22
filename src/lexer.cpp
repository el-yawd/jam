/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "lexer.h"
#include <iostream>
#include <stdexcept>

Lexer::Lexer(std::string source) : source(std::move(source)) {}

bool Lexer::isAtEnd() const { return current >= source.length(); }

char Lexer::advance() { return source[current++]; }

char Lexer::peek() const {
	if (isAtEnd()) return '\0';
	return source[current];
}

char Lexer::peekNext() const {
	if (current + 1 >= source.length()) return '\0';
	return source[current + 1];
}

bool Lexer::match(char expected) {
	if (isAtEnd() || source[current] != expected) return false;
	current++;
	return true;
}

void Lexer::skipWhitespace() {
	while (true) {
		char c = peek();
		switch (c) {
		case ' ':
		case '\r':
		case '\t':
			advance();
			break;
		case '\n':
			line++;
			advance();
			break;
		case '/':
			if (peekNext() == '/') {
				// Comment until end of line
				while (peek() != '\n' && !isAtEnd()) advance();
			} else {
				return;
			}
			break;
		default:
			return;
		}
	}
}

bool Lexer::isDigit(char c) const { return c >= '0' && c <= '9'; }

bool Lexer::isAlpha(char c) const {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::isAlphaNumeric(char c) const { return isAlpha(c) || isDigit(c); }

void Lexer::addToken(TokenType type) { addToken(type, ""); }

void Lexer::addToken(TokenType type, const std::string &lexeme) {
	tokens.emplace_back(type, lexeme, line);
}

void Lexer::identifier() {
	int start = current -
	            1;  // Start position (we already consumed the first character)
	while (isAlphaNumeric(peek())) advance();

	std::string text = source.substr(start, current - start);

	// Check for keywords
	if (text == "fn") {
		addToken(TOK_FN, text);
	} else if (text == "return") {
		addToken(TOK_RETURN, text);
	} else if (text == "const") {
		addToken(TOK_CONST, text);
	} else if (text == "var") {
		addToken(TOK_VAR, text);
	} else if (text == "if") {
		addToken(TOK_IF, text);
	} else if (text == "else") {
		addToken(TOK_ELSE, text);
	} else if (text == "while") {
		addToken(TOK_WHILE, text);
	} else if (text == "for") {
		addToken(TOK_FOR, text);
	} else if (text == "break") {
		addToken(TOK_BREAK, text);
	} else if (text == "continue") {
		addToken(TOK_CONTINUE, text);
	} else if (text == "in") {
		addToken(TOK_IN, text);
	} else if (text == "true") {
		addToken(TOK_TRUE, text);
	} else if (text == "false") {
		addToken(TOK_FALSE, text);
	} else if (text == "extern") {
		addToken(TOK_EXTERN, text);
	} else if (text == "export") {
		addToken(TOK_EXPORT, text);
	} else if (text == "pub") {
		addToken(TOK_PUB, text);
	} else if (text == "import") {
		addToken(TOK_IMPORT, text);
	} else if (text == "and") {
		addToken(TOK_AND, text);
	} else if (text == "or") {
		addToken(TOK_OR, text);
	} else if (text == "tfn") {
		addToken(TOK_TFN, text);
	} else if (text == "u1" || text == "u8" || text == "u16" || text == "u32" ||
	           text == "u64" || text == "i8" || text == "i16" ||
	           text == "i32" || text == "i64" || text == "bool" ||
	           text == "str") {
		addToken(TOK_TYPE, text);
	} else {
		addToken(TOK_IDENTIFIER, text);
	}
}

void Lexer::number() {
	int start =
	    current - 1;  // Start position (we already consumed the first digit)
	while (isDigit(peek())) advance();

	std::string num = source.substr(start, current - start);
	addToken(TOK_NUMBER, num);
}

void Lexer::negativeNumber() {
	int start = current - 1;  // Start position (we already consumed the minus)
	while (isDigit(peek())) advance();

	std::string num = source.substr(start, current - start);
	addToken(TOK_NUMBER, num);
}

char Lexer::parseHexByte() {
	// Parse two hex digits
	char value = 0;
	for (int i = 0; i < 2; i++) {
		char c = peek();
		if (c >= '0' && c <= '9') {
			value = value * 16 + (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			value = value * 16 + (c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			value = value * 16 + (c - 'A' + 10);
		} else {
			throw std::runtime_error("Expected hex digit at line " +
			                         std::to_string(line));
		}
		advance();
	}
	return value;
}

std::string Lexer::parseUnicodeEscape() {
	// Parse \u{HHHHH} - Unicode codepoint (1-6 hex digits)
	if (peek() != '{') {
		throw std::runtime_error("Expected '{' after \\u at line " +
		                         std::to_string(line));
	}
	advance();  // consume '{'

	uint32_t codepoint = 0;
	int digitCount = 0;

	while (peek() != '}' && !isAtEnd()) {
		char c = peek();
		if (c >= '0' && c <= '9') {
			codepoint = codepoint * 16 + (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			codepoint = codepoint * 16 + (c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			codepoint = codepoint * 16 + (c - 'A' + 10);
		} else {
			throw std::runtime_error(
			    "Expected hex digit in unicode escape at line " +
			    std::to_string(line));
		}
		advance();
		digitCount++;

		if (digitCount > 6) {
			throw std::runtime_error(
			    "Unicode escape too long (max 6 hex digits) at line " +
			    std::to_string(line));
		}
	}

	if (isAtEnd() || peek() != '}') {
		throw std::runtime_error(
		    "Expected '}' to close unicode escape at line " +
		    std::to_string(line));
	}
	advance();  // consume '}'

	if (digitCount == 0) {
		throw std::runtime_error("Empty unicode escape at line " +
		                         std::to_string(line));
	}

	if (codepoint > 0x10FFFF) {
		throw std::runtime_error(
		    "Unicode codepoint out of range (max 0x10FFFF) at line " +
		    std::to_string(line));
	}

	// Encode codepoint as UTF-8
	std::string result;
	if (codepoint <= 0x7F) {
		result += static_cast<char>(codepoint);
	} else if (codepoint <= 0x7FF) {
		result += static_cast<char>(0xC0 | (codepoint >> 6));
		result += static_cast<char>(0x80 | (codepoint & 0x3F));
	} else if (codepoint <= 0xFFFF) {
		result += static_cast<char>(0xE0 | (codepoint >> 12));
		result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		result += static_cast<char>(0x80 | (codepoint & 0x3F));
	} else {
		result += static_cast<char>(0xF0 | (codepoint >> 18));
		result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
		result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		result += static_cast<char>(0x80 | (codepoint & 0x3F));
	}
	return result;
}

void Lexer::stringLiteral() {
	std::string value;

	while (peek() != '"' && !isAtEnd()) {
		if (peek() == '\n') {
			throw std::runtime_error("Unterminated string at line " +
			                         std::to_string(line));
		}

		if (peek() == '\\') {
			advance();  // consume backslash
			if (isAtEnd()) {
				throw std::runtime_error(
				    "Unterminated escape sequence at line " +
				    std::to_string(line));
			}

			char escaped = advance();
			switch (escaped) {
			case 'n':
				value += '\n';
				break;
			case 'r':
				value += '\r';
				break;
			case 't':
				value += '\t';
				break;
			case '\\':
				value += '\\';
				break;
			case '"':
				value += '"';
				break;
			case '\'':
				value += '\'';
				break;
			case '0':
				value += '\0';
				break;
			case 'x':
				value += parseHexByte();
				break;
			case 'u':
				value += parseUnicodeEscape();
				break;
			default:
				throw std::runtime_error("Invalid escape sequence '\\" +
				                         std::string(1, escaped) +
				                         "' at line " + std::to_string(line));
			}
		} else {
			value += advance();
		}
	}

	if (isAtEnd()) {
		throw std::runtime_error("Unterminated string at line " +
		                         std::to_string(line));
	}

	// The closing "
	advance();

	addToken(TOK_STRING_LITERAL, value);
}

std::vector<Token> Lexer::scanTokens() {
	while (!isAtEnd()) {
		skipWhitespace();
		if (isAtEnd()) break;

		char c = advance();

		switch (c) {
		case '(':
			addToken(TOK_OPEN_PAREN, "(");
			break;
		case ')':
			addToken(TOK_CLOSE_PAREN, ")");
			break;
		case '{':
			addToken(TOK_OPEN_BRACE, "{");
			break;
		case '}':
			addToken(TOK_CLOSE_BRACE, "}");
			break;
		case '[':
			addToken(TOK_OPEN_BRACKET, "[");
			break;
		case ']':
			addToken(TOK_CLOSE_BRACKET, "]");
			break;
		case ',':
			addToken(TOK_COMMA, ",");
			break;
		case ';':
			addToken(TOK_SEMI, ";");
			break;
		case ':':
			addToken(TOK_COLON, ":");
			break;
		case '+':
			addToken(TOK_PLUS, "+");
			break;
		case '.':
			addToken(TOK_DOT, ".");
			break;
		case '"':
			stringLiteral();
			break;

		case '=':
			if (match('=')) {
				addToken(TOK_EQUAL_EQUAL, "==");
			} else {
				addToken(TOK_EQUAL, "=");
			}
			break;

		case '!':
			if (match('=')) {
				addToken(TOK_NOT_EQUAL, "!=");
			} else {
				addToken(TOK_NOT, "!");
			}
			break;

		case '<':
			if (match('=')) {
				addToken(TOK_LESS_EQUAL, "<=");
			} else {
				addToken(TOK_LESS, "<");
			}
			break;

		case '>':
			if (match('=')) {
				addToken(TOK_GREATER_EQUAL, ">=");
			} else {
				addToken(TOK_GREATER, ">");
			}
			break;

		case '-':
			if (isDigit(peek())) {
				// Handle negative number
				negativeNumber();
			} else {
				addToken(TOK_MINUS, "-");
			}
			break;

		default:
			if (isDigit(c)) {
				number();
			} else if (isAlpha(c)) {
				identifier();
			} else {
				std::cerr << "Unexpected character at line " << line << ": "
				          << c << std::endl;
			}
			break;
		}
	}

	tokens.emplace_back(TOK_EOF, "", line);
	return tokens;
}
