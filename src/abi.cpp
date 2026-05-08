/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "abi.h"
#include "ast.h"
#include "codegen.h"

namespace jam {
namespace abi {

namespace {

// A type is "scalar" for ABI purposes when it lowers to a single
// LLVM register-shaped value: integers, floats, booleans, pointers.
// Slices are 16-byte (ptr, len) aggregates and don't qualify here;
// they are aggregates that fit under the by-value size threshold.
bool isScalar(TypeKind k) {
	switch (k) {
	case TypeKind::Int:
	case TypeKind::Float:
	case TypeKind::Bool:
	case TypeKind::PtrSingle:
	case TypeKind::PtrMany:
		return true;
	default:
		return false;
	}
}

}  // namespace

ParamABI classifyParam(ParamMode mode, TypeIdx ty,
                       const JamCodegenContext &ctx) {
	// `mut` and `undefined` always carry a pointer to caller-owned (or
	// uninit-caller) storage. Size is irrelevant.
	if (mode == ParamMode::Mut || mode == ParamMode::Undefined) {
		return ParamABI{ParamABI::Kind::ByPointer, nullptr,
		                static_cast<uint32_t>(ctx.typeAlign(ty))};
	}

	// `let` and `move` are by-value at the call boundary. Whether the
	// value travels in a register / scalar pair (LLVM-level "ByValue")
	// or via a pointer to caller-owned storage depends on the type's
	// size. Move differs from let only on the caller side (the source
	// binding's tracked init state); the callee sees the same bytes.
	const TypeKey &k = ctx.getTypePool().get(ty);
	if (isScalar(k.kind)) {
		return ParamABI{ParamABI::Kind::ByValue, ctx.getLLVMType(ty), 0};
	}

	uint64_t size = ctx.typeSize(ty);
	if (size > kByValueMaxBytes) {
		return ParamABI{ParamABI::Kind::ByPointer, nullptr,
		                static_cast<uint32_t>(ctx.typeAlign(ty))};
	}
	return ParamABI{ParamABI::Kind::ByValue, ctx.getLLVMType(ty), 0};
}

ReturnABI classifyReturn(TypeIdx ty, const JamCodegenContext &ctx) {
	// Void / unspecified return — represented as kNoType and lowered to
	// LLVM `void`. Treat as Direct so codegen uses BuildRetVoid.
	if (ty == kNoType) {
		return ReturnABI{ReturnABI::Kind::Direct, ctx.getVoidType(), 0};
	}

	const TypeKey &k = ctx.getTypePool().get(ty);
	if (isScalar(k.kind)) {
		return ReturnABI{ReturnABI::Kind::Direct, ctx.getLLVMType(ty), 0};
	}

	uint64_t size = ctx.typeSize(ty);
	if (size > kByValueMaxBytes) {
		return ReturnABI{ReturnABI::Kind::Indirect, nullptr,
		                 static_cast<uint32_t>(ctx.typeAlign(ty))};
	}
	return ReturnABI{ReturnABI::Kind::Direct, ctx.getLLVMType(ty), 0};
}

}  // namespace abi
}  // namespace jam
