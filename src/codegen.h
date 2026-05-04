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

  private:
	JamContextRef ctx;
	JamModuleRef mod;
	JamBuilderRef builder;
	std::map<std::string, JamValueRef> namedValues;
	std::map<std::string, TypeIdx> namedValueTypes;
	std::map<std::string, StructInfo> structs;
	mutable TypePool typePool;
	mutable StringPool stringPool;
	mutable NodeStore nodeStore;
	// Lazy LLVM type per TypeIdx (built once, reused). Indexed by TypeIdx.
	mutable std::vector<JamTypeRef> llvmTypeCache;
};

#endif  // CODEGEN_H
