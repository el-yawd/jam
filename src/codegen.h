/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast_flat.h"
#include "jam_llvm.h"
#include <map>
#include <string>
#include <vector>

class JamCodegenContext {
  public:
	JamCodegenContext(const char *moduleName);
	~JamCodegenContext();

	// Non-copyable
	JamCodegenContext(const JamCodegenContext &) = delete;
	JamCodegenContext &operator=(const JamCodegenContext &) = delete;

	// Accessors
	JamContextRef getContext() const { return ctx; }
	JamModuleRef getModule() const { return mod; }
	JamBuilderRef getBuilder() const { return builder; }

	// Type helpers
	JamTypeRef getInt1Type() const { return JamLLVMInt1Type(ctx); }
	JamTypeRef getInt8Type() const { return JamLLVMInt8Type(ctx); }
	JamTypeRef getInt16Type() const { return JamLLVMInt16Type(ctx); }
	JamTypeRef getInt32Type() const { return JamLLVMInt32Type(ctx); }
	JamTypeRef getInt64Type() const { return JamLLVMInt64Type(ctx); }
	JamTypeRef getFloatType() const { return JamLLVMFloatType(ctx); }
	JamTypeRef getDoubleType() const { return JamLLVMDoubleType(ctx); }
	JamTypeRef getVoidType() const { return JamLLVMVoidType(ctx); }

	// Get type from Jam type string. Internally parses once, interns into
	// the TypePool, then resolves to an LLVM type via getLLVMType(TypeIdx).
	JamTypeRef getTypeFromString(const std::string &typeStr) const;

	// First-class type-pool entry points. internFromString parses a type
	// syntax fragment into a canonical TypeIdx; getLLVMType resolves that
	// TypeIdx to its LLVM type (cached). Once the parser/AST migrate to
	// TypeIdx, getTypeFromString becomes a thin wrapper.
	TypeIdx internFromString(const std::string &typeStr) const;
	JamTypeRef getLLVMType(TypeIdx ty) const;
	TypePool &getTypePool() { return typePool; }
	const TypePool &getTypePool() const { return typePool; }
	StringPool &getStringPool() { return stringPool; }
	const StringPool &getStringPool() const { return stringPool; }
	NodeStore &getNodeStore() { return nodeStore; }
	const NodeStore &getNodeStore() const { return nodeStore; }

	// Variable management
	void setVariable(const std::string &name, JamValueRef value);
	JamValueRef getVariable(const std::string &name) const;
	void clearVariables();
	bool hasVariable(const std::string &name) const;

	// Variable type tracking. Types are interned TypeIdx; kNoType means
	// "unknown" (caller must handle).
	void setVariableType(const std::string &name, TypeIdx type);
	TypeIdx getVariableType(const std::string &name) const;

	// Struct registry. Field names stay as std::string for now (struct
	// fields are queried by name); field types are TypeIdx.
	struct StructInfo {
		std::string name;
		JamTypeRef type;
		std::vector<std::pair<std::string, TypeIdx>> fields;
	};
	void registerStruct(const std::string &name, JamTypeRef type,
	                    std::vector<std::pair<std::string, TypeIdx>> fields);
	const StructInfo *getStruct(const std::string &name) const;
	// Convenience: resolve a TypeIdx of kind Struct back to its StructInfo.
	const StructInfo *lookupStruct(TypeIdx ty) const;
	int getFieldIndex(const std::string &structName,
	                  const std::string &fieldName) const;

	// Union registry. Untagged unions: every field shares the same
	// address. UnionInfo carries the union's LLVM storage type plus the
	// list of (fieldName, fieldType) pairs for member-access lookup.
	struct UnionInfo {
		std::string name;
		JamTypeRef type;
		std::vector<std::pair<std::string, TypeIdx>> fields;
	};
	void registerUnion(const std::string &name, JamTypeRef type,
	                   std::vector<std::pair<std::string, TypeIdx>> fields);
	const UnionInfo *getUnion(const std::string &name) const;
	const UnionInfo *lookupUnion(TypeIdx ty) const;
	TypeIdx getUnionFieldType(const std::string &unionName,
	                          const std::string &fieldName) const;

	// Enum registry. Each variant has a name and zero or more payload
	// fields (positional). When every variant is payload-less ("unit"),
	// the enum lowers to plain `i8`. When at least one variant carries
	// a payload, the enum lowers to a struct
	//     { i8 tag, [maxPayloadSize x i8] payload }
	// aligned to the max alignment of any variant's payload struct.
	struct EnumVariantInfo {
		std::string name;
		std::vector<TypeIdx> payloadTypes;  // empty for unit variants
		uint32_t discriminant = 0;          // wire / runtime tag value
	};
	struct EnumInfo {
		std::string name;
		JamTypeRef type = nullptr;          // LLVM type (i8 or {i8, [N x i8]})
		std::vector<EnumVariantInfo> variants;
		// True if at least one variant carries a payload. Determines
		// whether construction / pattern-matching follow the unit-only
		// fast path or the tagged-union path.
		bool hasPayloadVariant = false;
		// Max payload size in bytes; 0 if every variant is unit.
		uint64_t maxPayloadSize = 0;
		// Max payload alignment; 1 if every variant is unit.
		uint64_t maxPayloadAlign = 1;
	};
	void registerEnum(const std::string &name,
	                  std::vector<EnumVariantInfo> variants);
	void setEnumLLVMType(const std::string &name, JamTypeRef llvmType,
	                     uint64_t maxPayloadSize, uint64_t maxPayloadAlign,
	                     bool hasPayloadVariant);
	const EnumInfo *getEnum(const std::string &name) const;
	const EnumInfo *lookupEnum(TypeIdx ty) const;
	// Reverse-lookup: given an LLVM struct type, return the enum whose
	// payloaded layout matches, or nullptr. Used to disambiguate enum
	// scrutinees from slice-shaped structs in `match`.
	const EnumInfo *findEnumByLLVMType(JamTypeRef ty) const;
	// Index of `variantName` within `enumName`, or -1 if not found.
	int getEnumVariantIndex(const std::string &enumName,
	                        const std::string &variantName) const;

	// Type-size helpers used by union layout. Returns size/alignment in
	// bytes for any TypeIdx we currently support. Throws for types whose
	// size we don't know how to compute (e.g. user types with
	// unresolvable bodies).
	uint64_t typeSize(TypeIdx ty) const;
	uint64_t typeAlign(TypeIdx ty) const;

  private:
	JamContextRef ctx;
	JamModuleRef mod;
	JamBuilderRef builder;
	std::map<std::string, JamValueRef> namedValues;
	std::map<std::string, TypeIdx> namedValueTypes;
	std::map<std::string, StructInfo> structs;
	std::map<std::string, UnionInfo> unions;
	std::map<std::string, EnumInfo> enums;
	mutable TypePool typePool;
	mutable StringPool stringPool;
	mutable NodeStore nodeStore;
	// Lazy LLVM type per TypeIdx (built once, reused). Indexed by TypeIdx.
	mutable std::vector<JamTypeRef> llvmTypeCache;
};

#endif  // CODEGEN_H
