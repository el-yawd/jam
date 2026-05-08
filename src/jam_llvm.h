/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

/*
 * Jam LLVM Wrapper Layer
 *
 * The purpose of this file is to:
 * 1. Isolate all LLVM C++ API interaction to reduce compile times
 * 2. Provide a C interface for potential future self-hosting
 * 3. Prevent LLVM C++ headers from infecting the rest of the project
 *
 * Inspired by Zig's zig_llvm.h approach.
 */

#ifndef JAM_LLVM_H
#define JAM_LLVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define JAM_EXTERN_C extern "C"
#else
#define JAM_EXTERN_C
#endif

// Opaque pointer types for LLVM objects
typedef struct JamLLVMContext *JamContextRef;
typedef struct JamLLVMModule *JamModuleRef;
typedef struct JamLLVMBuilder *JamBuilderRef;
typedef struct JamLLVMType *JamTypeRef;
typedef struct JamLLVMValue *JamValueRef;
typedef struct JamLLVMBasicBlock *JamBasicBlockRef;
typedef struct JamLLVMFunction *JamFunctionRef;
typedef struct JamLLVMTargetMachine *JamTargetMachineRef;

// Calling conventions
typedef enum {
	JAM_CALLCONV_C = 0,
	JAM_CALLCONV_FAST = 8,
	JAM_CALLCONV_COLD = 9,
} JamCallingConv;

// Linkage types
typedef enum {
	JAM_LINKAGE_EXTERNAL = 0,
	JAM_LINKAGE_INTERNAL = 1,
	JAM_LINKAGE_PRIVATE = 2,
} JamLinkage;

// Integer comparison predicates
typedef enum {
	JAM_ICMP_EQ = 32,   // equal
	JAM_ICMP_NE = 33,   // not equal
	JAM_ICMP_UGT = 34,  // unsigned greater than
	JAM_ICMP_UGE = 35,  // unsigned greater or equal
	JAM_ICMP_ULT = 36,  // unsigned less than
	JAM_ICMP_ULE = 37,  // unsigned less or equal
	JAM_ICMP_SGT = 38,  // signed greater than
	JAM_ICMP_SGE = 39,  // signed greater or equal
	JAM_ICMP_SLT = 40,  // signed less than
	JAM_ICMP_SLE = 41,  // signed less or equal
} JamIntPredicate;

// ============================================================================
// Initialization
// ============================================================================

JAM_EXTERN_C void JamLLVMInitializeNativeTarget(void);
JAM_EXTERN_C void JamLLVMInitializeNativeAsmPrinter(void);
JAM_EXTERN_C void JamLLVMInitializeNativeAsmParser(void);
JAM_EXTERN_C void JamLLVMInitializeAllTargets(void);

// ============================================================================
// Context
// ============================================================================

JAM_EXTERN_C JamContextRef JamLLVMCreateContext(void);
JAM_EXTERN_C void JamLLVMDisposeContext(JamContextRef ctx);

// ============================================================================
// Module
// ============================================================================

JAM_EXTERN_C JamModuleRef JamLLVMCreateModule(const char *name,
                                              JamContextRef ctx);
JAM_EXTERN_C void JamLLVMDisposeModule(JamModuleRef mod);
JAM_EXTERN_C void JamLLVMSetTargetTriple(JamModuleRef mod, const char *triple);
JAM_EXTERN_C void JamLLVMSetDataLayout(JamModuleRef mod,
                                       JamTargetMachineRef tm);
JAM_EXTERN_C JamFunctionRef JamLLVMGetFunction(JamModuleRef mod,
                                               const char *name);
JAM_EXTERN_C char *JamLLVMPrintModuleToString(JamModuleRef mod);
JAM_EXTERN_C void JamLLVMDisposeMessage(char *msg);

// ============================================================================
// Builder
// ============================================================================

JAM_EXTERN_C JamBuilderRef JamLLVMCreateBuilder(JamContextRef ctx);
JAM_EXTERN_C void JamLLVMDisposeBuilder(JamBuilderRef builder);
JAM_EXTERN_C void JamLLVMPositionBuilderAtEnd(JamBuilderRef builder,
                                              JamBasicBlockRef block);
JAM_EXTERN_C JamBasicBlockRef JamLLVMGetInsertBlock(JamBuilderRef builder);

// ============================================================================
// Types
// ============================================================================

JAM_EXTERN_C JamTypeRef JamLLVMInt1Type(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMInt8Type(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMInt16Type(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMInt32Type(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMInt64Type(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMFloatType(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMDoubleType(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMVoidType(JamContextRef ctx);
JAM_EXTERN_C JamTypeRef JamLLVMPointerType(JamTypeRef elementType,
                                           unsigned addressSpace);
JAM_EXTERN_C JamTypeRef JamLLVMStructType(JamContextRef ctx,
                                          JamTypeRef *elementTypes,
                                          unsigned elementCount, bool packed);
JAM_EXTERN_C JamTypeRef JamLLVMStructCreateNamed(JamContextRef ctx,
                                                 const char *name);
JAM_EXTERN_C void JamLLVMStructSetBody(JamTypeRef structType,
                                       JamTypeRef *elementTypes,
                                       unsigned elementCount, bool packed);
JAM_EXTERN_C JamTypeRef JamLLVMFunctionType(JamTypeRef returnType,
                                            JamTypeRef *paramTypes,
                                            unsigned paramCount, bool isVarArg);
JAM_EXTERN_C JamTypeRef JamLLVMArrayType(JamTypeRef elementType,
                                         unsigned elementCount);
JAM_EXTERN_C bool JamLLVMTypeIsVoid(JamTypeRef type);
JAM_EXTERN_C bool JamLLVMTypeIsStruct(JamTypeRef type);
JAM_EXTERN_C bool JamLLVMTypeIsInteger(JamTypeRef type);
JAM_EXTERN_C bool JamLLVMTypeIsFloat(JamTypeRef type);
JAM_EXTERN_C bool JamLLVMTypeIsPointer(JamTypeRef type);
JAM_EXTERN_C bool JamLLVMTypeIsArray(JamTypeRef type);
JAM_EXTERN_C unsigned JamLLVMGetIntTypeWidth(JamTypeRef type);

// ============================================================================
// Constants
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMConstInt(JamTypeRef type, uint64_t val,
                                         bool signExtend);
JAM_EXTERN_C JamValueRef JamLLVMConstReal(JamTypeRef type, double val);
JAM_EXTERN_C JamValueRef JamLLVMConstNull(JamTypeRef type);
JAM_EXTERN_C JamValueRef JamLLVMConstString(JamContextRef ctx, const char *str,
                                            unsigned length,
                                            bool nullTerminate);
JAM_EXTERN_C JamValueRef JamLLVMConstStringInContext(JamContextRef ctx,
                                                     const char *str,
                                                     unsigned length,
                                                     bool dontNullTerminate);
JAM_EXTERN_C JamValueRef JamLLVMGetUndef(JamTypeRef type);

// ============================================================================
// Global Variables
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMAddGlobalString(JamModuleRef mod,
                                                const char *str,
                                                const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildGlobalStringPtr(JamBuilderRef builder,
                                                     const char *str,
                                                     const char *name);
JAM_EXTERN_C JamValueRef JamLLVMAddGlobal(JamModuleRef mod, JamTypeRef type,
                                          const char *name);
JAM_EXTERN_C void JamLLVMSetGlobalConstant(JamValueRef global, bool isConstant);
JAM_EXTERN_C void JamLLVMSetInitializer(JamValueRef global,
                                        JamValueRef constantVal);

// ============================================================================
// Functions
// ============================================================================

JAM_EXTERN_C JamFunctionRef JamLLVMAddFunction(JamModuleRef mod,
                                               const char *name,
                                               JamTypeRef funcType);
JAM_EXTERN_C void JamLLVMSetFunctionCallConv(JamFunctionRef func,
                                             JamCallingConv cc);
JAM_EXTERN_C void JamLLVMSetLinkage(JamValueRef global, JamLinkage linkage);
JAM_EXTERN_C unsigned JamLLVMCountParams(JamFunctionRef func);
JAM_EXTERN_C JamValueRef JamLLVMGetParam(JamFunctionRef func, unsigned index);
JAM_EXTERN_C bool JamLLVMFunctionIsVarArg(JamFunctionRef func);
// Attach the LLVM `zeroext` attribute to a function parameter / return slot.
// Used to satisfy the C ABI when an i1 (Jam bool) crosses the extern/export
// boundary — C's _Bool is conventionally zero-extended to int.
JAM_EXTERN_C void JamLLVMAddParamAttrZeroExt(JamFunctionRef func,
                                             unsigned argIdx);
JAM_EXTERN_C void JamLLVMAddRetAttrZeroExt(JamFunctionRef func);

// P9.6: mark a function parameter as the sret (struct-return) slot.
// Equivalent to LLVM `sret(<type>) align <a> noalias`. The argument
// must be `ptr`-typed; the pointee type and alignment are passed
// explicitly. Used by codegen for functions returning aggregates whose
// size exceeds the by-value threshold.
JAM_EXTERN_C void JamLLVMAddParamAttrSret(JamFunctionRef func,
                                          unsigned argIdx,
                                          JamTypeRef pointeeType,
                                          unsigned align);
JAM_EXTERN_C void JamLLVMSetValueName(JamValueRef val, const char *name);
JAM_EXTERN_C JamTypeRef JamLLVMGetReturnType(JamFunctionRef func);
JAM_EXTERN_C bool JamLLVMVerifyFunction(JamFunctionRef func);

// ============================================================================
// Basic Blocks
// ============================================================================

JAM_EXTERN_C JamBasicBlockRef JamLLVMCreateBasicBlock(JamContextRef ctx,
                                                      const char *name);
JAM_EXTERN_C JamBasicBlockRef JamLLVMAppendBasicBlock(JamFunctionRef func,
                                                      const char *name);
JAM_EXTERN_C JamFunctionRef JamLLVMGetBasicBlockParent(JamBasicBlockRef block);
JAM_EXTERN_C JamValueRef JamLLVMGetBasicBlockTerminator(JamBasicBlockRef block);

// ============================================================================
// Instructions - Memory
// ============================================================================

// Stack alloca with an explicit alignment in bytes. Pass 0 to fall back to
// LLVM's data-layout-derived inference, but prefer passing the type's real
// alignment (via JamCodegenContext::typeAlign). LLVM's getPrefTypeAlign
// over-aligns aggregates relative to what C/C++/Zig produce on the same
// target — Zig works around this the same way (see Zig codegen/llvm.zig
// `buildAllocaInner` which always calls `setAlignment` on the result).
JAM_EXTERN_C JamValueRef JamLLVMBuildAlloca(JamBuilderRef builder,
                                            JamTypeRef type,
                                            uint64_t alignBytes,
                                            const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildLoad(JamBuilderRef builder,
                                          JamTypeRef type, JamValueRef ptr,
                                          const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildStore(JamBuilderRef builder,
                                           JamValueRef val, JamValueRef ptr);
// In-bounds GEP for indexing into a fixed-size array: gep [N x T], ptr, 0, idx.
// `arrayType` must be the array aggregate type that `ptr` points to.
JAM_EXTERN_C JamValueRef JamLLVMBuildArrayGEP(JamBuilderRef builder,
                                              JamTypeRef arrayType,
                                              JamValueRef ptr, JamValueRef idx,
                                              const char *name);
// Struct field GEP: returns a pointer to field `fieldIdx` of the struct
// pointed to by `ptr`. `structType` must be the struct type `ptr` points to.
JAM_EXTERN_C JamValueRef JamLLVMBuildStructGEP(JamBuilderRef builder,
                                               JamTypeRef structType,
                                               JamValueRef ptr,
                                               unsigned fieldIdx,
                                               const char *name);
// Pointer GEP: single-index `gep T, ptr, idx` for stepping by element-sized
// strides through a many-item pointer (no leading 0). `elemType` is the
// pointee type (T).
JAM_EXTERN_C JamValueRef JamLLVMBuildPtrGEP(JamBuilderRef builder,
                                            JamTypeRef elemType,
                                            JamValueRef ptr, JamValueRef idx,
                                            const char *name);
// Returns the element type of an array type (e.g. [200 x i8] -> i8).
JAM_EXTERN_C JamTypeRef JamLLVMGetArrayElementType(JamTypeRef arrayType);

// ============================================================================
// Instructions - Arithmetic
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMBuildAdd(JamBuilderRef builder, JamValueRef lhs,
                                         JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildSub(JamBuilderRef builder, JamValueRef lhs,
                                         JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildMul(JamBuilderRef builder, JamValueRef lhs,
                                         JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildURem(JamBuilderRef builder,
                                          JamValueRef lhs, JamValueRef rhs,
                                          const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildAnd(JamBuilderRef builder, JamValueRef lhs,
                                         JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildOr(JamBuilderRef builder, JamValueRef lhs,
                                        JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildXor(JamBuilderRef builder, JamValueRef lhs,
                                         JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildShl(JamBuilderRef builder, JamValueRef lhs,
                                         JamValueRef rhs, const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildLShr(JamBuilderRef builder,
                                          JamValueRef lhs, JamValueRef rhs,
                                          const char *name);

// ============================================================================
// Instructions - Comparison
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMBuildICmp(JamBuilderRef builder,
                                          JamIntPredicate pred, JamValueRef lhs,
                                          JamValueRef rhs, const char *name);

// ============================================================================
// Instructions - Control Flow
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMBuildBr(JamBuilderRef builder,
                                        JamBasicBlockRef dest);
JAM_EXTERN_C JamValueRef JamLLVMBuildCondBr(JamBuilderRef builder,
                                            JamValueRef cond,
                                            JamBasicBlockRef thenBlock,
                                            JamBasicBlockRef elseBlock);
JAM_EXTERN_C JamValueRef JamLLVMBuildRet(JamBuilderRef builder,
                                         JamValueRef val);
JAM_EXTERN_C JamValueRef JamLLVMBuildRetVoid(JamBuilderRef builder);
JAM_EXTERN_C JamValueRef JamLLVMBuildUnreachable(JamBuilderRef builder);
JAM_EXTERN_C JamValueRef JamLLVMBuildCall(JamBuilderRef builder,
                                          JamFunctionRef func,
                                          JamValueRef *args, unsigned numArgs,
                                          const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildPhi(JamBuilderRef builder, JamTypeRef type,
                                         const char *name);
JAM_EXTERN_C void JamLLVMAddIncoming(JamValueRef phi, JamValueRef *values,
                                     JamBasicBlockRef *blocks, unsigned count);

// ============================================================================
// Instructions - Conversions
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMBuildBitCast(JamBuilderRef builder,
                                             JamValueRef val,
                                             JamTypeRef destType,
                                             const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildIntCast(JamBuilderRef builder,
                                             JamValueRef val,
                                             JamTypeRef destType, bool isSigned,
                                             const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildSIToFP(JamBuilderRef builder,
                                            JamValueRef val,
                                            JamTypeRef destType,
                                            const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildUIToFP(JamBuilderRef builder,
                                            JamValueRef val,
                                            JamTypeRef destType,
                                            const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildFPCast(JamBuilderRef builder,
                                            JamValueRef val,
                                            JamTypeRef destType,
                                            const char *name);

// ============================================================================
// Instructions - Aggregates
// ============================================================================

JAM_EXTERN_C JamValueRef JamLLVMBuildInsertValue(JamBuilderRef builder,
                                                 JamValueRef agg,
                                                 JamValueRef val,
                                                 unsigned index,
                                                 const char *name);
JAM_EXTERN_C JamValueRef JamLLVMBuildExtractValue(JamBuilderRef builder,
                                                  JamValueRef agg,
                                                  unsigned index,
                                                  const char *name);

// ============================================================================
// Value Utilities
// ============================================================================

JAM_EXTERN_C JamTypeRef JamLLVMTypeOf(JamValueRef val);
JAM_EXTERN_C JamTypeRef JamLLVMGetAllocatedType(JamValueRef alloca);

// ============================================================================
// Target & Code Generation
// ============================================================================

JAM_EXTERN_C char *JamLLVMGetDefaultTargetTriple(void);
JAM_EXTERN_C char *JamLLVMGetHostCPUName(void);
JAM_EXTERN_C char *JamLLVMGetHostCPUFeatures(void);

// Optimization levels mirror llvm::CodeGenOpt::Level. Default for jam is
// `None` to match Zig's Debug mode — Debug compiles 5-10× faster than -O2
// because LLVM's machine codegen is the dominant cost.
typedef enum {
	JAM_OPT_NONE = 0,        // -O0 (Zig "Debug")
	JAM_OPT_LESS = 1,        // -O1
	JAM_OPT_DEFAULT = 2,     // -O2 (LLVM default)
	JAM_OPT_AGGRESSIVE = 3,  // -O3 (Zig "ReleaseFast")
} JamOptLevel;

JAM_EXTERN_C JamTargetMachineRef
JamLLVMCreateTargetMachine(const char *triple, const char *cpu,
                           const char *features, bool isRelocationPIC,
                           JamOptLevel optLevel);
JAM_EXTERN_C void JamLLVMDisposeTargetMachine(JamTargetMachineRef tm);

JAM_EXTERN_C bool JamLLVMEmitObjectFile(JamModuleRef mod,
                                        JamTargetMachineRef tm,
                                        const char *filename,
                                        char **errorMessage);

#endif  // JAM_LLVM_H
