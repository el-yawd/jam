/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef CODEGEN_H
#define CODEGEN_H

#include "jam_llvm.h"
#include <map>
#include <string>

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
	JamTypeRef getVoidType() const { return JamLLVMVoidType(ctx); }

	// Get type from Jam type string
	JamTypeRef getTypeFromString(const std::string &typeStr) const;

	// Variable management
	void setVariable(const std::string &name, JamValueRef value);
	JamValueRef getVariable(const std::string &name) const;
	void clearVariables();
	bool hasVariable(const std::string &name) const;

  private:
	JamContextRef ctx;
	JamModuleRef mod;
	JamBuilderRef builder;
	std::map<std::string, JamValueRef> namedValues;
};

#endif  // CODEGEN_H
