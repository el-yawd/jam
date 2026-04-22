/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

/*
 * Jam LLVM Wrapper Implementation
 *
 * This file contains all LLVM C++ API interaction, providing a C interface
 * to the rest of the Jam compiler. This approach:
 * 1. Reduces compile times (only this file needs LLVM headers)
 * 2. Isolates LLVM version differences
 * 3. Enables potential future self-hosting
 *
 * Inspired by Zig's zig_llvm.cpp approach.
 */

#include "jam_llvm.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <cstring>

// ============================================================================
// Helper macros for type casting
// ============================================================================

#define WRAP_CONTEXT(ctx) reinterpret_cast<JamContextRef>(ctx)
#define UNWRAP_CONTEXT(ctx) reinterpret_cast<llvm::LLVMContext *>(ctx)

#define WRAP_MODULE(mod) reinterpret_cast<JamModuleRef>(mod)
#define UNWRAP_MODULE(mod) reinterpret_cast<llvm::Module *>(mod)

#define WRAP_BUILDER(builder) reinterpret_cast<JamBuilderRef>(builder)
#define UNWRAP_BUILDER(builder) reinterpret_cast<llvm::IRBuilder<> *>(builder)

#define WRAP_TYPE(type) reinterpret_cast<JamTypeRef>(type)
#define UNWRAP_TYPE(type) reinterpret_cast<llvm::Type *>(type)

#define WRAP_VALUE(val) reinterpret_cast<JamValueRef>(val)
#define UNWRAP_VALUE(val) reinterpret_cast<llvm::Value *>(val)

#define WRAP_BLOCK(block) reinterpret_cast<JamBasicBlockRef>(block)
#define UNWRAP_BLOCK(block) reinterpret_cast<llvm::BasicBlock *>(block)

#define WRAP_FUNCTION(func) reinterpret_cast<JamFunctionRef>(func)
#define UNWRAP_FUNCTION(func) reinterpret_cast<llvm::Function *>(func)

#define WRAP_TARGET_MACHINE(tm) reinterpret_cast<JamTargetMachineRef>(tm)
#define UNWRAP_TARGET_MACHINE(tm) reinterpret_cast<llvm::TargetMachine *>(tm)

// ============================================================================
// Initialization
// ============================================================================

void JamLLVMInitializeNativeTarget(void) { llvm::InitializeNativeTarget(); }

void JamLLVMInitializeNativeAsmPrinter(void) {
	llvm::InitializeNativeTargetAsmPrinter();
}

void JamLLVMInitializeNativeAsmParser(void) {
	llvm::InitializeNativeTargetAsmParser();
}

void JamLLVMInitializeAllTargets(void) {
	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllAsmPrinters();
}

// ============================================================================
// Context
// ============================================================================

JamContextRef JamLLVMCreateContext(void) {
	return WRAP_CONTEXT(new llvm::LLVMContext());
}

void JamLLVMDisposeContext(JamContextRef ctx) { delete UNWRAP_CONTEXT(ctx); }

// ============================================================================
// Module
// ============================================================================

JamModuleRef JamLLVMCreateModule(const char *name, JamContextRef ctx) {
	return WRAP_MODULE(new llvm::Module(name, *UNWRAP_CONTEXT(ctx)));
}

void JamLLVMDisposeModule(JamModuleRef mod) { delete UNWRAP_MODULE(mod); }

void JamLLVMSetTargetTriple(JamModuleRef mod, const char *triple) {
	UNWRAP_MODULE(mod)->setTargetTriple(triple);
}

void JamLLVMSetDataLayout(JamModuleRef mod, JamTargetMachineRef tm) {
	UNWRAP_MODULE(mod)->setDataLayout(
	    UNWRAP_TARGET_MACHINE(tm)->createDataLayout());
}

JamFunctionRef JamLLVMGetFunction(JamModuleRef mod, const char *name) {
	return WRAP_FUNCTION(UNWRAP_MODULE(mod)->getFunction(name));
}

char *JamLLVMPrintModuleToString(JamModuleRef mod) {
	std::string output;
	llvm::raw_string_ostream stream(output);
	UNWRAP_MODULE(mod)->print(stream, nullptr);
	return strdup(output.c_str());
}

void JamLLVMDisposeMessage(char *msg) { free(msg); }

// ============================================================================
// Builder
// ============================================================================

JamBuilderRef JamLLVMCreateBuilder(JamContextRef ctx) {
	return WRAP_BUILDER(new llvm::IRBuilder<>(*UNWRAP_CONTEXT(ctx)));
}

void JamLLVMDisposeBuilder(JamBuilderRef builder) {
	delete UNWRAP_BUILDER(builder);
}

void JamLLVMPositionBuilderAtEnd(JamBuilderRef builder,
                                 JamBasicBlockRef block) {
	UNWRAP_BUILDER(builder)->SetInsertPoint(UNWRAP_BLOCK(block));
}

JamBasicBlockRef JamLLVMGetInsertBlock(JamBuilderRef builder) {
	return WRAP_BLOCK(UNWRAP_BUILDER(builder)->GetInsertBlock());
}

// ============================================================================
// Types
// ============================================================================

JamTypeRef JamLLVMInt1Type(JamContextRef ctx) {
	return WRAP_TYPE(llvm::Type::getInt1Ty(*UNWRAP_CONTEXT(ctx)));
}

JamTypeRef JamLLVMInt8Type(JamContextRef ctx) {
	return WRAP_TYPE(llvm::Type::getInt8Ty(*UNWRAP_CONTEXT(ctx)));
}

JamTypeRef JamLLVMInt16Type(JamContextRef ctx) {
	return WRAP_TYPE(llvm::Type::getInt16Ty(*UNWRAP_CONTEXT(ctx)));
}

JamTypeRef JamLLVMInt32Type(JamContextRef ctx) {
	return WRAP_TYPE(llvm::Type::getInt32Ty(*UNWRAP_CONTEXT(ctx)));
}

JamTypeRef JamLLVMInt64Type(JamContextRef ctx) {
	return WRAP_TYPE(llvm::Type::getInt64Ty(*UNWRAP_CONTEXT(ctx)));
}

JamTypeRef JamLLVMVoidType(JamContextRef ctx) {
	return WRAP_TYPE(llvm::Type::getVoidTy(*UNWRAP_CONTEXT(ctx)));
}

JamTypeRef JamLLVMPointerType(JamTypeRef elementType, unsigned addressSpace) {
	return WRAP_TYPE(
	    llvm::PointerType::get(UNWRAP_TYPE(elementType), addressSpace));
}

JamTypeRef JamLLVMStructType(JamContextRef ctx, JamTypeRef *elementTypes,
                             unsigned elementCount, bool packed) {
	std::vector<llvm::Type *> types;
	for (unsigned i = 0; i < elementCount; i++) {
		types.push_back(UNWRAP_TYPE(elementTypes[i]));
	}
	return WRAP_TYPE(
	    llvm::StructType::get(*UNWRAP_CONTEXT(ctx), types, packed));
}

JamTypeRef JamLLVMFunctionType(JamTypeRef returnType, JamTypeRef *paramTypes,
                               unsigned paramCount, bool isVarArg) {
	std::vector<llvm::Type *> params;
	for (unsigned i = 0; i < paramCount; i++) {
		params.push_back(UNWRAP_TYPE(paramTypes[i]));
	}
	return WRAP_TYPE(
	    llvm::FunctionType::get(UNWRAP_TYPE(returnType), params, isVarArg));
}

bool JamLLVMTypeIsVoid(JamTypeRef type) {
	return UNWRAP_TYPE(type)->isVoidTy();
}

bool JamLLVMTypeIsStruct(JamTypeRef type) {
	return UNWRAP_TYPE(type)->isStructTy();
}

bool JamLLVMTypeIsInteger(JamTypeRef type) {
	return UNWRAP_TYPE(type)->isIntegerTy();
}

JamTypeRef JamLLVMArrayType(JamTypeRef elementType, unsigned elementCount) {
	return WRAP_TYPE(
	    llvm::ArrayType::get(UNWRAP_TYPE(elementType), elementCount));
}

unsigned JamLLVMGetIntTypeWidth(JamTypeRef type) {
	return UNWRAP_TYPE(type)->getIntegerBitWidth();
}

// ============================================================================
// Constants
// ============================================================================

JamValueRef JamLLVMConstInt(JamTypeRef type, uint64_t val, bool signExtend) {
	return WRAP_VALUE(
	    llvm::ConstantInt::get(UNWRAP_TYPE(type), val, signExtend));
}

JamValueRef JamLLVMConstNull(JamTypeRef type) {
	return WRAP_VALUE(llvm::Constant::getNullValue(UNWRAP_TYPE(type)));
}

JamValueRef JamLLVMConstString(JamContextRef ctx, const char *str,
                               unsigned length, bool nullTerminate) {
	return WRAP_VALUE(llvm::ConstantDataArray::getString(
	    *UNWRAP_CONTEXT(ctx), llvm::StringRef(str, length), nullTerminate));
}

JamValueRef JamLLVMConstStringInContext(JamContextRef ctx, const char *str,
                                        unsigned length,
                                        bool dontNullTerminate) {
	return WRAP_VALUE(llvm::ConstantDataArray::getString(
	    *UNWRAP_CONTEXT(ctx), llvm::StringRef(str, length),
	    !dontNullTerminate));
}

JamValueRef JamLLVMGetUndef(JamTypeRef type) {
	return WRAP_VALUE(llvm::UndefValue::get(UNWRAP_TYPE(type)));
}

// ============================================================================
// Global Variables
// ============================================================================

JamValueRef JamLLVMAddGlobalString(JamModuleRef mod, const char *str,
                                   const char *name) {
	llvm::Module *module = UNWRAP_MODULE(mod);
	llvm::Constant *strConstant =
	    llvm::ConstantDataArray::getString(module->getContext(), str, true);
	llvm::GlobalVariable *global = new llvm::GlobalVariable(
	    *module, strConstant->getType(),
	    true,  // isConstant
	    llvm::GlobalValue::PrivateLinkage, strConstant, name);
	return WRAP_VALUE(global);
}

JamValueRef JamLLVMBuildGlobalStringPtr(JamBuilderRef builder, const char *str,
                                        const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateGlobalString(str, name));
}

JamValueRef JamLLVMAddGlobal(JamModuleRef mod, JamTypeRef type,
                             const char *name) {
	llvm::Module *module = UNWRAP_MODULE(mod);
	return WRAP_VALUE(
	    new llvm::GlobalVariable(*module, UNWRAP_TYPE(type),
	                             false,  // isConstant (set separately)
	                             llvm::GlobalValue::PrivateLinkage,
	                             nullptr,  // initializer (set separately)
	                             name));
}

void JamLLVMSetGlobalConstant(JamValueRef global, bool isConstant) {
	llvm::cast<llvm::GlobalVariable>(UNWRAP_VALUE(global))
	    ->setConstant(isConstant);
}

void JamLLVMSetInitializer(JamValueRef global, JamValueRef constantVal) {
	llvm::cast<llvm::GlobalVariable>(UNWRAP_VALUE(global))
	    ->setInitializer(llvm::cast<llvm::Constant>(UNWRAP_VALUE(constantVal)));
}

// ============================================================================
// Functions
// ============================================================================

JamFunctionRef JamLLVMAddFunction(JamModuleRef mod, const char *name,
                                  JamTypeRef funcType) {
	return WRAP_FUNCTION(llvm::Function::Create(
	    llvm::cast<llvm::FunctionType>(UNWRAP_TYPE(funcType)),
	    llvm::Function::ExternalLinkage, name, UNWRAP_MODULE(mod)));
}

void JamLLVMSetFunctionCallConv(JamFunctionRef func, JamCallingConv cc) {
	llvm::CallingConv::ID llvmCC;
	switch (cc) {
	case JAM_CALLCONV_C:
		llvmCC = llvm::CallingConv::C;
		break;
	case JAM_CALLCONV_FAST:
		llvmCC = llvm::CallingConv::Fast;
		break;
	case JAM_CALLCONV_COLD:
		llvmCC = llvm::CallingConv::Cold;
		break;
	default:
		llvmCC = llvm::CallingConv::C;
		break;
	}
	UNWRAP_FUNCTION(func)->setCallingConv(llvmCC);
}

void JamLLVMSetLinkage(JamValueRef global, JamLinkage linkage) {
	llvm::GlobalValue::LinkageTypes llvmLinkage;
	switch (linkage) {
	case JAM_LINKAGE_EXTERNAL:
		llvmLinkage = llvm::GlobalValue::ExternalLinkage;
		break;
	case JAM_LINKAGE_INTERNAL:
		llvmLinkage = llvm::GlobalValue::InternalLinkage;
		break;
	case JAM_LINKAGE_PRIVATE:
		llvmLinkage = llvm::GlobalValue::PrivateLinkage;
		break;
	default:
		llvmLinkage = llvm::GlobalValue::ExternalLinkage;
		break;
	}
	llvm::cast<llvm::GlobalValue>(UNWRAP_VALUE(global))
	    ->setLinkage(llvmLinkage);
}

unsigned JamLLVMCountParams(JamFunctionRef func) {
	return UNWRAP_FUNCTION(func)->arg_size();
}

JamValueRef JamLLVMGetParam(JamFunctionRef func, unsigned index) {
	return WRAP_VALUE(UNWRAP_FUNCTION(func)->getArg(index));
}

void JamLLVMSetValueName(JamValueRef val, const char *name) {
	UNWRAP_VALUE(val)->setName(name);
}

JamTypeRef JamLLVMGetReturnType(JamFunctionRef func) {
	return WRAP_TYPE(UNWRAP_FUNCTION(func)->getReturnType());
}

bool JamLLVMVerifyFunction(JamFunctionRef func) {
	return !llvm::verifyFunction(*UNWRAP_FUNCTION(func), &llvm::errs());
}

// ============================================================================
// Basic Blocks
// ============================================================================

JamBasicBlockRef JamLLVMCreateBasicBlock(JamContextRef ctx, const char *name) {
	return WRAP_BLOCK(llvm::BasicBlock::Create(*UNWRAP_CONTEXT(ctx), name));
}

JamBasicBlockRef JamLLVMAppendBasicBlock(JamFunctionRef func,
                                         const char *name) {
	llvm::Function *function = UNWRAP_FUNCTION(func);
	return WRAP_BLOCK(
	    llvm::BasicBlock::Create(function->getContext(), name, function));
}

JamFunctionRef JamLLVMGetBasicBlockParent(JamBasicBlockRef block) {
	return WRAP_FUNCTION(UNWRAP_BLOCK(block)->getParent());
}

JamValueRef JamLLVMGetBasicBlockTerminator(JamBasicBlockRef block) {
	return WRAP_VALUE(UNWRAP_BLOCK(block)->getTerminator());
}

// ============================================================================
// Instructions - Memory
// ============================================================================

JamValueRef JamLLVMBuildAlloca(JamBuilderRef builder, JamTypeRef type,
                               const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateAlloca(UNWRAP_TYPE(type),
	                                                        nullptr, name));
}

JamValueRef JamLLVMBuildLoad(JamBuilderRef builder, JamTypeRef type,
                             JamValueRef ptr, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateLoad(
	    UNWRAP_TYPE(type), UNWRAP_VALUE(ptr), name));
}

JamValueRef JamLLVMBuildStore(JamBuilderRef builder, JamValueRef val,
                              JamValueRef ptr) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateStore(UNWRAP_VALUE(val),
	                                                       UNWRAP_VALUE(ptr)));
}

// ============================================================================
// Instructions - Arithmetic
// ============================================================================

JamValueRef JamLLVMBuildAdd(JamBuilderRef builder, JamValueRef lhs,
                            JamValueRef rhs, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateAdd(
	    UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

JamValueRef JamLLVMBuildSub(JamBuilderRef builder, JamValueRef lhs,
                            JamValueRef rhs, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateSub(
	    UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

JamValueRef JamLLVMBuildMul(JamBuilderRef builder, JamValueRef lhs,
                            JamValueRef rhs, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateMul(
	    UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

JamValueRef JamLLVMBuildAnd(JamBuilderRef builder, JamValueRef lhs,
                            JamValueRef rhs, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateAnd(
	    UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

JamValueRef JamLLVMBuildOr(JamBuilderRef builder, JamValueRef lhs,
                           JamValueRef rhs, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateOr(
	    UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

JamValueRef JamLLVMBuildXor(JamBuilderRef builder, JamValueRef lhs,
                            JamValueRef rhs, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateXor(
	    UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

// ============================================================================
// Instructions - Comparison
// ============================================================================

JamValueRef JamLLVMBuildICmp(JamBuilderRef builder, JamIntPredicate pred,
                             JamValueRef lhs, JamValueRef rhs,
                             const char *name) {
	llvm::CmpInst::Predicate llvmPred;
	switch (pred) {
	case JAM_ICMP_EQ:
		llvmPred = llvm::CmpInst::ICMP_EQ;
		break;
	case JAM_ICMP_NE:
		llvmPred = llvm::CmpInst::ICMP_NE;
		break;
	case JAM_ICMP_UGT:
		llvmPred = llvm::CmpInst::ICMP_UGT;
		break;
	case JAM_ICMP_UGE:
		llvmPred = llvm::CmpInst::ICMP_UGE;
		break;
	case JAM_ICMP_ULT:
		llvmPred = llvm::CmpInst::ICMP_ULT;
		break;
	case JAM_ICMP_ULE:
		llvmPred = llvm::CmpInst::ICMP_ULE;
		break;
	case JAM_ICMP_SGT:
		llvmPred = llvm::CmpInst::ICMP_SGT;
		break;
	case JAM_ICMP_SGE:
		llvmPred = llvm::CmpInst::ICMP_SGE;
		break;
	case JAM_ICMP_SLT:
		llvmPred = llvm::CmpInst::ICMP_SLT;
		break;
	case JAM_ICMP_SLE:
		llvmPred = llvm::CmpInst::ICMP_SLE;
		break;
	default:
		llvmPred = llvm::CmpInst::ICMP_EQ;
		break;
	}
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateICmp(
	    llvmPred, UNWRAP_VALUE(lhs), UNWRAP_VALUE(rhs), name));
}

// ============================================================================
// Instructions - Control Flow
// ============================================================================

JamValueRef JamLLVMBuildBr(JamBuilderRef builder, JamBasicBlockRef dest) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateBr(UNWRAP_BLOCK(dest)));
}

JamValueRef JamLLVMBuildCondBr(JamBuilderRef builder, JamValueRef cond,
                               JamBasicBlockRef thenBlock,
                               JamBasicBlockRef elseBlock) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateCondBr(
	    UNWRAP_VALUE(cond), UNWRAP_BLOCK(thenBlock), UNWRAP_BLOCK(elseBlock)));
}

JamValueRef JamLLVMBuildRet(JamBuilderRef builder, JamValueRef val) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateRet(UNWRAP_VALUE(val)));
}

JamValueRef JamLLVMBuildRetVoid(JamBuilderRef builder) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateRetVoid());
}

JamValueRef JamLLVMBuildUnreachable(JamBuilderRef builder) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateUnreachable());
}

JamValueRef JamLLVMBuildCall(JamBuilderRef builder, JamFunctionRef func,
                             JamValueRef *args, unsigned numArgs,
                             const char *name) {
	std::vector<llvm::Value *> argValues;
	for (unsigned i = 0; i < numArgs; i++) {
		argValues.push_back(UNWRAP_VALUE(args[i]));
	}
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateCall(UNWRAP_FUNCTION(func),
	                                                      argValues, name));
}

JamValueRef JamLLVMBuildPhi(JamBuilderRef builder, JamTypeRef type,
                            const char *name) {
	return WRAP_VALUE(
	    UNWRAP_BUILDER(builder)->CreatePHI(UNWRAP_TYPE(type), 2, name));
}

void JamLLVMAddIncoming(JamValueRef phi, JamValueRef *values,
                        JamBasicBlockRef *blocks, unsigned count) {
	llvm::PHINode *phiNode = llvm::cast<llvm::PHINode>(UNWRAP_VALUE(phi));
	for (unsigned i = 0; i < count; i++) {
		phiNode->addIncoming(UNWRAP_VALUE(values[i]), UNWRAP_BLOCK(blocks[i]));
	}
}

// ============================================================================
// Instructions - Conversions
// ============================================================================

JamValueRef JamLLVMBuildBitCast(JamBuilderRef builder, JamValueRef val,
                                JamTypeRef destType, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateBitCast(
	    UNWRAP_VALUE(val), UNWRAP_TYPE(destType), name));
}

JamValueRef JamLLVMBuildIntCast(JamBuilderRef builder, JamValueRef val,
                                JamTypeRef destType, bool isSigned,
                                const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateIntCast(
	    UNWRAP_VALUE(val), UNWRAP_TYPE(destType), isSigned, name));
}

// ============================================================================
// Instructions - Aggregates
// ============================================================================

JamValueRef JamLLVMBuildInsertValue(JamBuilderRef builder, JamValueRef agg,
                                    JamValueRef val, unsigned index,
                                    const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateInsertValue(
	    UNWRAP_VALUE(agg), UNWRAP_VALUE(val), index, name));
}

JamValueRef JamLLVMBuildExtractValue(JamBuilderRef builder, JamValueRef agg,
                                     unsigned index, const char *name) {
	return WRAP_VALUE(UNWRAP_BUILDER(builder)->CreateExtractValue(
	    UNWRAP_VALUE(agg), index, name));
}

// ============================================================================
// Value Utilities
// ============================================================================

JamTypeRef JamLLVMTypeOf(JamValueRef val) {
	return WRAP_TYPE(UNWRAP_VALUE(val)->getType());
}

JamTypeRef JamLLVMGetAllocatedType(JamValueRef alloca) {
	return WRAP_TYPE(
	    llvm::cast<llvm::AllocaInst>(UNWRAP_VALUE(alloca))->getAllocatedType());
}

// ============================================================================
// Target & Code Generation
// ============================================================================

char *JamLLVMGetDefaultTargetTriple(void) {
	return strdup(llvm::sys::getDefaultTargetTriple().c_str());
}

char *JamLLVMGetHostCPUName(void) {
	return strdup(llvm::sys::getHostCPUName().str().c_str());
}

char *JamLLVMGetHostCPUFeatures(void) {
	std::string features;
	auto hostFeatures = llvm::sys::getHostCPUFeatures();
	for (auto &f : hostFeatures) {
		if (!features.empty()) features += ",";
		features += (f.second ? "+" : "-");
		features += f.first().str();
	}
	return strdup(features.c_str());
}

JamTargetMachineRef JamLLVMCreateTargetMachine(const char *triple,
                                               const char *cpu,
                                               const char *features,
                                               bool isRelocationPIC) {
	std::string error;
	const llvm::Target *target =
	    llvm::TargetRegistry::lookupTarget(triple, error);
	if (!target) { return nullptr; }

	llvm::TargetOptions opt;
	auto rm = isRelocationPIC ? llvm::Reloc::PIC_ : llvm::Reloc::Static;

	llvm::TargetMachine *tm = target->createTargetMachine(
	    triple, cpu ? cpu : "generic", features ? features : "", opt, rm);

	return WRAP_TARGET_MACHINE(tm);
}

void JamLLVMDisposeTargetMachine(JamTargetMachineRef tm) {
	delete UNWRAP_TARGET_MACHINE(tm);
}

bool JamLLVMEmitObjectFile(JamModuleRef mod, JamTargetMachineRef tm,
                           const char *filename, char **errorMessage) {
	std::error_code ec;
	llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);

	if (ec) {
		if (errorMessage) { *errorMessage = strdup(ec.message().c_str()); }
		return false;
	}

	llvm::legacy::PassManager pass;
	if (UNWRAP_TARGET_MACHINE(tm)->addPassesToEmitFile(
	        pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
		if (errorMessage) {
			*errorMessage = strdup("Target machine cannot emit object file");
		}
		return false;
	}

	pass.run(*UNWRAP_MODULE(mod));
	dest.close();
	return true;
}
