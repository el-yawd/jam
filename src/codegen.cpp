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
		const auto *sinfo = getStruct(name);
		if (!sinfo) {
			throw std::runtime_error(
			    "Unknown struct type: " + name);
		}
		result = sinfo->type;
		break;
	}
	case TypeKind::Named: {
		// Parser-deferred user type — resolve against the three
		// declaration registries in order: struct, union, enum.
		const std::string &name = stringPool.get(static_cast<StringIdx>(k.a));
		if (const auto *sinfo = getStruct(name)) {
			result = sinfo->type;
		} else if (const auto *uinfo = getUnion(name)) {
			result = uinfo->type;
		} else if (const auto *einfo = getEnum(name)) {
			// E1 (unit-only): lowers to i8. E2 (with payloads): lowers
			// to {i8, [N x i8]} via the named struct type set during
			// declaration.
			result = einfo->hasPayloadVariant ? einfo->type : getInt8Type();
		} else {
			throw std::runtime_error(
			    "Unknown user-defined type: " + name);
		}
		break;
	}
	case TypeKind::Union: {
		const std::string &name = stringPool.get(static_cast<StringIdx>(k.a));
		const auto *info = getUnion(name);
		if (!info) {
			throw std::runtime_error("Unknown union type: " + name);
		}
		result = info->type;
		break;
	}
	case TypeKind::Enum: {
		// E1 enums lower to u8 — one byte per discriminant.
		result = getInt8Type();
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
	// Accept TypeKind::Struct (explicit) or TypeKind::Named (parser-
	// deferred user type that resolves to a struct).
	if (k.kind != TypeKind::Struct && k.kind != TypeKind::Named) {
		return nullptr;
	}
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

// ---- Union registry -------------------------------------------------------

void JamCodegenContext::registerUnion(
    const std::string &name, JamTypeRef type,
    std::vector<std::pair<std::string, TypeIdx>> fields) {
	UnionInfo info;
	info.name = name;
	info.type = type;
	info.fields = std::move(fields);
	unions[name] = std::move(info);
}

const JamCodegenContext::UnionInfo *
JamCodegenContext::getUnion(const std::string &name) const {
	auto it = unions.find(name);
	if (it != unions.end()) return &it->second;
	return nullptr;
}

const JamCodegenContext::UnionInfo *
JamCodegenContext::lookupUnion(TypeIdx ty) const {
	if (ty == kNoType) return nullptr;
	const TypeKey &k = typePool.get(ty);
	// Accept TypeKind::Union (explicit) or TypeKind::Named (parser-
	// deferred user type that resolves to a union).
	if (k.kind != TypeKind::Union && k.kind != TypeKind::Named) {
		return nullptr;
	}
	const std::string &name = stringPool.get(static_cast<StringIdx>(k.a));
	return getUnion(name);
}

TypeIdx
JamCodegenContext::getUnionFieldType(const std::string &unionName,
                                     const std::string &fieldName) const {
	const UnionInfo *info = getUnion(unionName);
	if (!info) return kNoType;
	for (const auto &f : info->fields) {
		if (f.first == fieldName) return f.second;
	}
	return kNoType;
}

// ---- Enum registry --------------------------------------------------------

void JamCodegenContext::registerEnum(
    const std::string &name, std::vector<EnumVariantInfo> variants) {
	EnumInfo info;
	info.name = name;
	info.variants = std::move(variants);
	for (const auto &v : info.variants) {
		if (!v.payloadTypes.empty()) {
			info.hasPayloadVariant = true;
			break;
		}
	}
	enums[name] = std::move(info);
}

void JamCodegenContext::setEnumLLVMType(const std::string &name,
                                        JamTypeRef llvmType,
                                        uint64_t maxPayloadSize,
                                        uint64_t maxPayloadAlign,
                                        bool hasPayloadVariant) {
	auto it = enums.find(name);
	if (it == enums.end()) {
		throw std::runtime_error("setEnumLLVMType: unknown enum " + name);
	}
	it->second.type = llvmType;
	it->second.maxPayloadSize = maxPayloadSize;
	it->second.maxPayloadAlign = maxPayloadAlign;
	it->second.hasPayloadVariant = hasPayloadVariant;
}

const JamCodegenContext::EnumInfo *
JamCodegenContext::getEnum(const std::string &name) const {
	auto it = enums.find(name);
	if (it != enums.end()) return &it->second;
	return nullptr;
}

const JamCodegenContext::EnumInfo *
JamCodegenContext::lookupEnum(TypeIdx ty) const {
	if (ty == kNoType) return nullptr;
	const TypeKey &k = typePool.get(ty);
	// Accept TypeKind::Enum (explicit) or TypeKind::Named (parser-
	// deferred user type that resolves to an enum).
	if (k.kind != TypeKind::Enum && k.kind != TypeKind::Named) {
		return nullptr;
	}
	const std::string &name = stringPool.get(static_cast<StringIdx>(k.a));
	return getEnum(name);
}

int JamCodegenContext::getEnumVariantIndex(
    const std::string &enumName, const std::string &variantName) const {
	const EnumInfo *info = getEnum(enumName);
	if (!info) return -1;
	for (size_t i = 0; i < info->variants.size(); i++) {
		if (info->variants[i].name == variantName) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

const JamCodegenContext::EnumInfo *
JamCodegenContext::findEnumByLLVMType(JamTypeRef ty) const {
	for (const auto &kv : enums) {
		if (kv.second.type == ty) return &kv.second;
	}
	return nullptr;
}

void JamCodegenContext::registerModuleConst(const std::string &name,
                                            NodeIdx init,
                                            TypeIdx declared) {
	moduleConsts[name] = ModuleConstInfo{init, declared};
}

const JamCodegenContext::ModuleConstInfo *
JamCodegenContext::getModuleConst(const std::string &name) const {
	auto it = moduleConsts.find(name);
	if (it != moduleConsts.end()) return &it->second;
	return nullptr;
}

// Size of a type in bytes. Used by union layout computation. The
// numbers assume a 64-bit target — pointers and slice lengths are 8
// bytes. Struct sizes do not currently account for inter-field padding;
// callers needing exact struct sizes should ask LLVM via the data
// layout instead. For union fields the simple sum is enough because we
// pick the field with the largest size as the layout type.
uint64_t JamCodegenContext::typeSize(TypeIdx ty) const {
	const TypeKey &k = typePool.get(ty);
	switch (k.kind) {
	case TypeKind::Invalid:
	case TypeKind::Void:
		return 0;
	case TypeKind::Bool:
		return 1;
	case TypeKind::Int:
	case TypeKind::Float:
		return k.a / 8;
	case TypeKind::PtrSingle:
	case TypeKind::PtrMany:
		return 8;
	case TypeKind::Slice:
		return 16;  // (ptr, len)
	case TypeKind::Array:
		return static_cast<uint64_t>(k.b) *
		       typeSize(static_cast<TypeIdx>(k.a));
	case TypeKind::Struct:
	case TypeKind::Named: {
		// User-named types resolve through any of the three registries.
		if (const StructInfo *info = lookupStruct(ty)) {
			uint64_t total = 0;
			for (const auto &f : info->fields) total += typeSize(f.second);
			return total;
		}
		if (const UnionInfo *info = lookupUnion(ty)) {
			uint64_t maxSize = 0;
			for (const auto &f : info->fields) {
				uint64_t s = typeSize(f.second);
				if (s > maxSize) maxSize = s;
			}
			return maxSize;
		}
		if (const EnumInfo *info = lookupEnum(ty)) {
			if (!info->hasPayloadVariant) return 1;
			// {tag, payload}: tag (1 byte), padding to align, payload.
			uint64_t padToAlign =
			    (info->maxPayloadAlign > 1)
			        ? info->maxPayloadAlign - 1
			        : 0;
			return 1 + padToAlign + info->maxPayloadSize;
		}
		throw std::runtime_error(
		    "typeSize on unregistered user type");
	}
	case TypeKind::Union: {
		const UnionInfo *info = lookupUnion(ty);
		if (!info) {
			throw std::runtime_error(
			    "typeSize on unregistered union type");
		}
		uint64_t maxSize = 0;
		for (const auto &f : info->fields) {
			uint64_t s = typeSize(f.second);
			if (s > maxSize) maxSize = s;
		}
		return maxSize;
	}
	case TypeKind::Enum:
		return 1;  // M2 E1 enums lower to u8
	}
	throw std::runtime_error("typeSize: unhandled type kind");
}

// Alignment requirement of a type. Equal to size for primitive scalars
// on every target we care about. For aggregates, the alignment is the
// max of the constituent alignments.
uint64_t JamCodegenContext::typeAlign(TypeIdx ty) const {
	const TypeKey &k = typePool.get(ty);
	switch (k.kind) {
	case TypeKind::Invalid:
	case TypeKind::Void:
		return 1;
	case TypeKind::Bool:
		return 1;
	case TypeKind::Int:
	case TypeKind::Float:
		return k.a / 8;
	case TypeKind::PtrSingle:
	case TypeKind::PtrMany:
	case TypeKind::Slice:
		return 8;
	case TypeKind::Array:
		return typeAlign(static_cast<TypeIdx>(k.a));
	case TypeKind::Struct:
	case TypeKind::Named: {
		if (const StructInfo *info = lookupStruct(ty)) {
			uint64_t maxAlign = 1;
			for (const auto &f : info->fields) {
				uint64_t a = typeAlign(f.second);
				if (a > maxAlign) maxAlign = a;
			}
			return maxAlign;
		}
		if (const UnionInfo *info = lookupUnion(ty)) {
			uint64_t maxAlign = 1;
			for (const auto &f : info->fields) {
				uint64_t a = typeAlign(f.second);
				if (a > maxAlign) maxAlign = a;
			}
			return maxAlign;
		}
		if (const EnumInfo *info = lookupEnum(ty)) {
			return info->hasPayloadVariant ? info->maxPayloadAlign : 1;
		}
		throw std::runtime_error(
		    "typeAlign on unregistered user type");
	}
	case TypeKind::Union: {
		const UnionInfo *info = lookupUnion(ty);
		if (!info) {
			throw std::runtime_error(
			    "typeAlign on unregistered union type");
		}
		uint64_t maxAlign = 1;
		for (const auto &f : info->fields) {
			uint64_t a = typeAlign(f.second);
			if (a > maxAlign) maxAlign = a;
		}
		return maxAlign;
	}
	case TypeKind::Enum:
		return 1;
	}
	throw std::runtime_error("typeAlign: unhandled type kind");
}
