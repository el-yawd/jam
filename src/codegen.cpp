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
	} else if (typeStr == "f32") {
		return getFloatType();
	} else if (typeStr == "f64") {
		return getDoubleType();
	} else if (typeStr == "bool" || typeStr == "u1") {
		return getInt1Type();
	} else if (auto *info = getStruct(typeStr)) {
		return info->type;
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
	} else if (typeStr.length() >= 3 && typeStr[0] == '[') {
		// Fixed-size array: [N]T
		size_t closeBracket = typeStr.find(']');
		if (closeBracket == std::string::npos || closeBracket == 1) {
			throw std::runtime_error("Malformed array type: " + typeStr);
		}
		std::string sizeStr = typeStr.substr(1, closeBracket - 1);
		std::string elementTypeStr = typeStr.substr(closeBracket + 1);
		unsigned long long size = std::stoull(sizeStr);
		JamTypeRef elemType = getTypeFromString(elementTypeStr);
		return JamLLVMArrayType(elemType, static_cast<unsigned>(size));
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

void JamCodegenContext::clearVariables() {
	namedValues.clear();
	namedValueTypes.clear();
}

bool JamCodegenContext::hasVariable(const std::string &name) const {
	return namedValues.find(name) != namedValues.end();
}

void JamCodegenContext::setVariableType(const std::string &name,
                                        const std::string &typeName) {
	namedValueTypes[name] = typeName;
}

std::string
JamCodegenContext::getVariableType(const std::string &name) const {
	auto it = namedValueTypes.find(name);
	if (it != namedValueTypes.end()) return it->second;
	return "";
}

void JamCodegenContext::registerStruct(
    const std::string &name, JamTypeRef type,
    std::vector<std::pair<std::string, std::string>> fields) {
	StructInfo info;
	info.name = name;
	info.type = type;
	info.fields = std::move(fields);
	structs[name] = std::move(info);
}

const JamCodegenContext::StructInfo *
JamCodegenContext::getStruct(const std::string &name) const {
	auto it = structs.find(name);
	if (it != structs.end()) return &it->second;
	return nullptr;
}

int JamCodegenContext::getFieldIndex(const std::string &structName,
                                     const std::string &fieldName) const {
	const StructInfo *info = getStruct(structName);
	if (!info) return -1;
	for (size_t i = 0; i < info->fields.size(); i++) {
		if (info->fields[i].first == fieldName) return static_cast<int>(i);
	}
	return -1;
}
