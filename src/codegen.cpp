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

// Parse a type-syntax string into the canonical TypeIdx (recursive). The
// parser will eventually produce TypeIdx directly and this function will
// only be called by legacy callers that still hold strings.
TypeIdx JamCodegenContext::internFromString(const std::string &typeStr) const {
	// `const T` is purely semantic — strip and continue.
	if (typeStr.length() >= 6 && typeStr.substr(0, 6) == "const ") {
		return internFromString(typeStr.substr(6));
	}
	// Built-in scalars resolve to pre-interned indices in the pool.
	if (typeStr == "u8") return BuiltinType::U8;
	if (typeStr == "i8") return BuiltinType::I8;
	if (typeStr == "u16") return BuiltinType::U16;
	if (typeStr == "i16") return BuiltinType::I16;
	if (typeStr == "u32") return BuiltinType::U32;
	if (typeStr == "i32") return BuiltinType::I32;
	if (typeStr == "u64") return BuiltinType::U64;
	if (typeStr == "i64") return BuiltinType::I64;
	if (typeStr == "f32") return BuiltinType::F32;
	if (typeStr == "f64") return BuiltinType::F64;
	if (typeStr == "bool" || typeStr == "u1") return BuiltinType::Bool;
	// `str` is a slice of u8.
	if (typeStr == "str") {
		return typePool.internSlice(BuiltinType::U8);
	}
	// User-defined struct (looked up in the registry by name).
	if (getStruct(typeStr)) {
		StringIdx nameId = stringPool.intern(typeStr);
		return typePool.internStruct(nameId);
	}
	// Pointers: *T  or  [*]T.
	if (typeStr.length() >= 3 && typeStr.substr(0, 3) == "[*]") {
		return typePool.internPtrMany(internFromString(typeStr.substr(3)));
	}
	if (typeStr.length() >= 2 && typeStr[0] == '*') {
		return typePool.internPtrSingle(internFromString(typeStr.substr(1)));
	}
	// Slice []T (`[]const T` already had its const stripped above).
	if (typeStr.length() >= 2 && typeStr.substr(0, 2) == "[]") {
		return typePool.internSlice(internFromString(typeStr.substr(2)));
	}
	// Fixed-size array [N]T.
	if (typeStr.length() >= 3 && typeStr[0] == '[') {
		size_t closeBracket = typeStr.find(']');
		if (closeBracket == std::string::npos || closeBracket == 1) {
			throw std::runtime_error("Malformed array type: " + typeStr);
		}
		uint32_t len = static_cast<uint32_t>(
		    std::stoull(typeStr.substr(1, closeBracket - 1)));
		TypeIdx elem = internFromString(typeStr.substr(closeBracket + 1));
		return typePool.internArray(elem, len);
	}
	throw std::runtime_error("Unknown type: " + typeStr);
}

JamTypeRef JamCodegenContext::getLLVMType(TypeIdx ty) const {
	if (ty >= llvmTypeCache.size()) {
		llvmTypeCache.resize(ty + 1, nullptr);
	}
	if (llvmTypeCache[ty]) return llvmTypeCache[ty];

	const TypeKey &k = typePool.get(ty);
	JamTypeRef result = nullptr;
	switch (k.kind) {
	case TypeKind::Invalid:
	case TypeKind::Void:
		result = getVoidType();
		break;
	case TypeKind::Bool:
		result = getInt1Type();
		break;
	case TypeKind::Int: {
		switch (k.a) {
		case 8:
			result = getInt8Type();
			break;
		case 16:
			result = getInt16Type();
			break;
		case 32:
			result = getInt32Type();
			break;
		case 64:
			result = getInt64Type();
			break;
		default:
			throw std::runtime_error("Unsupported int width");
		}
		break;
	}
	case TypeKind::Float:
		result = (k.a == 32) ? getFloatType() : getDoubleType();
		break;
	case TypeKind::PtrSingle:
	case TypeKind::PtrMany: {
		JamTypeRef elem = getLLVMType(static_cast<TypeIdx>(k.a));
		result = JamLLVMPointerType(elem, 0);
		break;
	}
	case TypeKind::Slice: {
		JamTypeRef elem = getLLVMType(static_cast<TypeIdx>(k.a));
		JamTypeRef elemPtr = JamLLVMPointerType(elem, 0);
		JamTypeRef usize = getInt64Type();
		JamTypeRef parts[2] = {elemPtr, usize};
		result = JamLLVMStructType(ctx, parts, 2, false);
		break;
	}
	case TypeKind::Array: {
		JamTypeRef elem = getLLVMType(static_cast<TypeIdx>(k.a));
		result = JamLLVMArrayType(elem, k.b);
		break;
	}
	case TypeKind::Struct: {
		const std::string &name = stringPool.get(static_cast<StringIdx>(k.a));
		const auto *info = getStruct(name);
		if (!info) {
			throw std::runtime_error("Unknown struct type: " + name);
		}
		result = info->type;
		break;
	}
	}
	llvmTypeCache[ty] = result;
	return result;
}

JamTypeRef
JamCodegenContext::getTypeFromString(const std::string &typeStr) const {
	return getLLVMType(internFromString(typeStr));
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
                                        TypeIdx type) {
	namedValueTypes[name] = type;
}

TypeIdx JamCodegenContext::getVariableType(const std::string &name) const {
	auto it = namedValueTypes.find(name);
	if (it != namedValueTypes.end()) return it->second;
	return kNoType;
}

void JamCodegenContext::registerStruct(
    const std::string &name, JamTypeRef type,
    std::vector<std::pair<std::string, TypeIdx>> fields) {
	StructInfo info;
	info.name = name;
	info.type = type;
	info.fields = std::move(fields);
	structs[name] = std::move(info);
}

const JamCodegenContext::StructInfo *
JamCodegenContext::lookupStruct(TypeIdx ty) const {
	if (ty == kNoType) return nullptr;
	const TypeKey &k = typePool.get(ty);
	if (k.kind != TypeKind::Struct) return nullptr;
	const std::string &name = stringPool.get(static_cast<StringIdx>(k.a));
	return getStruct(name);
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
