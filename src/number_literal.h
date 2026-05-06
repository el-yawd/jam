/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

// Numeric-literal validation. The lexer scans number tokens permissively
// — it accepts essentially every character that could plausibly belong
// to a number — and this module walks those bytes, decides whether they
// form a well-formed integer / big-integer / float literal, and returns
// either the parsed value or a position-bearing error.
//
// Keeping validation separate from the lexer keeps the tokenizer small
// and fast (the path a syntax-highlighting tool would take) while
// letting the validator own the rich error vocabulary used at parse
// time.

#ifndef NUMBER_LITERAL_H
#define NUMBER_LITERAL_H

#include <cstddef>
#include <cstdint>
#include <string>

enum class NumberBase : uint8_t {
	Decimal = 10,
	Hex = 16,
	Binary = 2,
	Octal = 8,
};

// Per-error variants with the position where the problem was found.
// Position is a byte offset into the lexeme, not into the source file.
enum class NumberErrorKind {
	LeadingZero,                  // "07" — leading zero on decimal
	DigitAfterBase,               // "0x" with no digit
	UpperCaseBase,                // "0X42" — uppercase prefix
	InvalidFloatBase,             // float in non-decimal/hex base
	RepeatedUnderscore,           // "1__0"
	InvalidUnderscoreAfterSpecial, // "0x_1", "1._5", etc.
	InvalidDigit,                 // digit out of range for the base
	InvalidDigitExponent,         // non-decimal digit in an exponent
	DuplicatePeriod,              // "1.0.5"
	DuplicateExponent,            // "1e10e2"
	InvalidHexExponent,           // hex literal with `e`/`E` exponent
	ExponentAfterUnderscore,      // "1_e10"
	SpecialAfterUnderscore,       // "1_."
	TrailingSpecial,              // "1.", "1+", "1e"
	TrailingUnderscore,           // "1_"
	InvalidCharacter,             // out of [0-9a-zA-Z._+-]
	InvalidExponentSign,          // "+" or "-" not after p/P/e/E
	IntegerTooLarge,              // overflows u64 (caller asks for int)
};

struct NumberError {
	NumberErrorKind kind;
	std::size_t pos;
	NumberBase base;  // meaningful for InvalidDigit, otherwise Decimal
};

// Tagged-union result: exactly one of the four variants is valid based
// on `kind`. The codegen / parser path consumes Int directly; BigInt and
// Float are surfaced for future support and currently cause the parser
// to throw an "unsupported literal" message.
enum class NumberResultKind {
	Int,
	BigInt,
	Float,
	Failure,
};

struct NumberResult {
	NumberResultKind kind = NumberResultKind::Failure;
	uint64_t intValue = 0;
	NumberBase base = NumberBase::Decimal;
	NumberError failure{};
};

// Validate a number lexeme produced by the lexer's permissive scan.
// `bytes` is expected to be non-empty and to start with a digit (the
// lexer guarantees this). A leading `-` sign is *not* accepted here —
// callers that handle negation strip the sign first.
NumberResult parseNumberLiteral(const std::string &bytes);

// Stable human-readable description for an error kind. Used by the
// parser to construct diagnostic messages.
const char *numberErrorMessage(NumberErrorKind kind);

#endif  // NUMBER_LITERAL_H
