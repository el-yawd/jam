/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "codegen.h"
#include <stdexcept>

JamCodegenContext::JamCodegenContext(const char *moduleName) {
	ctx = JamLLVMCreateContext();
	mod = JamLLVMCreateModule(moduleName, ctx);
	builder = JamLLVMCreateBuilder(ctx);
}

JamCodegenContext::~JamCodegenContext() {
	JamLLVMDisposeBuilder(builder);
	JamLLVMDisposeModule(mod);
	JamLLVMDisposeContext(ctx);
}

JamTypeRef
JamCodegenContext::getTypeFromString(const std::string &typeStr) const {
	// Handle const T - strip const prefix and get underlying type
	// (LLVM doesn't have const at type level, it's semantic only)
	if (typeStr.length() >= 6 && typeStr.substr(0, 6) == "const ") {
		return getTypeFromString(typeStr.substr(6));
	}
	if (typeStr == "u8" || typeStr == "i8") {
		return getInt8Type();
	} else if (typeStr == "u16" || typeStr == "i16") {
		return getInt16Type();
	} else if (typeStr == "u32" || typeStr == "i32") {
		return getInt32Type();
	} else if (typeStr == "u64" || typeStr == "i64") {
		return getInt64Type();
	} else if (typeStr == "bool" || typeStr == "u1") {
		return getInt1Type();
	} else if (typeStr == "str") {
		// String slice: struct { ptr: *u8, len: usize }
		// Equivalent to []const u8 in Zig
		JamTypeRef i8Type = getInt8Type();
		JamTypeRef i8PtrType = JamLLVMPointerType(i8Type, 0);
		JamTypeRef usizeType = getInt64Type();
		JamTypeRef elementTypes[2] = {i8PtrType, usizeType};
		return JamLLVMStructType(ctx, elementTypes, 2, false);
	} else if (typeStr.length() >= 2 && typeStr.substr(0, 2) == "[]") {
		// Slice type: []T or []const T -> struct { ptr: *T, len: usize }
		std::string elementTypeStr = typeStr.substr(2);  // Remove "[]"
		JamTypeRef elemType = getTypeFromString(elementTypeStr);
		JamTypeRef elemPtrType = JamLLVMPointerType(elemType, 0);
		JamTypeRef usizeType = getInt64Type();
		JamTypeRef elementTypes[2] = {elemPtrType, usizeType};
		return JamLLVMStructType(ctx, elementTypes, 2, false);
	}
	throw std::runtime_error("Unknown type: " + typeStr);
}

void JamCodegenContext::setVariable(const std::string &name,
                                    JamValueRef value) {
	namedValues[name] = value;
}

JamValueRef JamCodegenContext::getVariable(const std::string &name) const {
	auto it = namedValues.find(name);
	if (it != namedValues.end()) { return it->second; }
	return nullptr;
}

void JamCodegenContext::clearVariables() { namedValues.clear(); }

bool JamCodegenContext::hasVariable(const std::string &name) const {
	return namedValues.find(name) != namedValues.end();
}
