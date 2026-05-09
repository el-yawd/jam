/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "ast.h"
#include "ast_flat.h"
#include "codegen.h"
#include "jam_llvm.h"
#include <cstdint>
#include <stdexcept>
#include "abi.h"
#include <string>
#include <vector>

JamBasicBlockRef CurrentLoopContinue = nullptr;
JamBasicBlockRef CurrentLoopBreak = nullptr;
// P8.4: index into the drop-scope stack of the *currently enclosing
// loop body's scope*. On `break` / `continue`, the codegen emits drops
// for every scope from the top down to (and including) this index, so
// locals declared inside the loop body — possibly under nested
// if/match scopes — get their drops fired before the branch leaves the
// loop. SIZE_MAX means "no enclosing loop body active here".
size_t CurrentLoopBodyScopeIdx = SIZE_MAX;

// Forward declaration so codegenCall (P9.5 auto-address path) can call
// codegenAddressOf which is defined further down in the file.
static JamValueRef codegenAddressOf(JamCodegenContext &ctx, const AstNode &n);

// Forward declaration so the struct-method dispatch in codegenCall can
// translate a qualified source-level name (`File.drop`) into the LLVM
// symbol (`__drop_File`). The implementation lives near declarePrototype.
static std::string mangledFunctionName(const FunctionAST &fn,
                                       const TypePool &types,
                                       const StringPool &strings);

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static JamValueRef coerceTo(JamCodegenContext &ctx, JamValueRef val,
                            JamTypeRef expected) {
	JamTypeRef actual = JamLLVMTypeOf(val);
	if (actual == expected) return val;
	if (JamLLVMTypeIsFloat(expected) && JamLLVMTypeIsInteger(actual)) {
		return JamLLVMBuildSIToFP(ctx.getBuilder(), val, expected,
		                          "assign_si2fp");
	}
	if (JamLLVMTypeIsFloat(expected) && JamLLVMTypeIsFloat(actual)) {
		return JamLLVMBuildFPCast(ctx.getBuilder(), val, expected,
		                          "assign_fpcast");
	}
	if (JamLLVMTypeIsInteger(expected) && JamLLVMTypeIsInteger(actual)) {
		return JamLLVMBuildIntCast(ctx.getBuilder(), val, expected, false,
		                           "assign_icast");
	}
	return val;
}

// Returns the pointee TypeIdx of `*T` or `[*]T`; kNoType otherwise.
static TypeIdx pointeeTypeOf(const TypePool &tp, TypeIdx ptrType) {
	if (ptrType == kNoType) return kNoType;
	const TypeKey &k = tp.get(ptrType);
	if (k.kind == TypeKind::PtrSingle || k.kind == TypeKind::PtrMany) {
		return static_cast<TypeIdx>(k.a);
	}
	return kNoType;
}

// Reconstruct the u64 numeric value from a NumberLit node (lhs holds low 32
// bits, rhs holds high 32 bits; flags bit 0 marks negation).
static uint64_t numberLitValue(const AstNode &n) {
	return static_cast<uint64_t>(n.lhs) |
	       (static_cast<uint64_t>(n.rhs) << 32);
}

// Materialize a NumberLit node directly at a known integer type. Falls back
// to natural smallest-fit width when expectedType is null or non-integer.
static JamValueRef numberLitConst(JamCodegenContext &ctx, const AstNode &n,
                                  JamTypeRef expectedType) {
	uint64_t val = numberLitValue(n);
	bool isNegative = (n.flags & 1) != 0;

	if (expectedType && JamLLVMTypeIsInteger(expectedType)) {
		if (isNegative) {
			int64_t signedVal = -static_cast<int64_t>(val);
			return JamLLVMConstInt(expectedType,
			                       static_cast<uint64_t>(signedVal), true);
		}
		return JamLLVMConstInt(expectedType, val, false);
	}

	JamTypeRef IntType;
	if (isNegative) {
		if (val <= 128) IntType = ctx.getInt8Type();
		else if (val <= 32768) IntType = ctx.getInt16Type();
		else if (val <= 2147483648ULL) IntType = ctx.getInt32Type();
		else IntType = ctx.getInt64Type();
		int64_t signedVal = -static_cast<int64_t>(val);
		return JamLLVMConstInt(IntType, static_cast<uint64_t>(signedVal),
		                       true);
	}
	if (val <= 255) IntType = ctx.getInt8Type();
	else if (val <= 65535) IntType = ctx.getInt16Type();
	else if (val <= 4294967295ULL) IntType = ctx.getInt32Type();
	else IntType = ctx.getInt64Type();
	return JamLLVMConstInt(IntType, val, false);
}

// Walk a MemberAccess/Variable chain back to its root variable. Fills `path`
// with member names (outermost-first → leaf-last). Returns the StringIdx of
// the root variable name, or kNoString if the root is not a Variable.
static StringIdx collectMemberChain(const NodeStore &nodes, NodeIdx idx,
                                    std::vector<StringIdx> &outPath) {
	std::vector<StringIdx> reversed;
	NodeIdx cur = idx;
	while (true) {
		const AstNode &n = nodes.get(cur);
		if (n.tag == AstTag::MemberAccess) {
			reversed.push_back(static_cast<StringIdx>(n.rhs));
			cur = static_cast<NodeIdx>(n.lhs);
			continue;
		}
		if (n.tag == AstTag::Variable) {
			for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
				outPath.push_back(*it);
			}
			return static_cast<StringIdx>(n.lhs);
		}
		return kNoString;
	}
}

// Resolve an indexable lvalue (the Object inside an Index node) to a pointer
// at the indexed element. Supports plain `arr[i]` on a local and indexed
// fields like `g.board[i]` reached through struct GEPs.
static JamValueRef resolveIndexedElementPtr(JamCodegenContext &ctx,
                                            NodeIdx objectIdx,
                                            JamValueRef idxVal,
                                            JamTypeRef &outElemType) {
	const NodeStore &ns = ctx.getNodeStore();
	const AstNode &on = ns.get(objectIdx);
	const StringPool &sp = ctx.getStringPool();

	if (on.tag == AstTag::Variable) {
		const std::string &name = sp.get(static_cast<StringIdx>(on.lhs));
		JamValueRef alloca = ctx.getVariable(name);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + name);
		}
		TypeIdx varTy = ctx.getVariableType(name);
		const TypeKey &k = ctx.getTypePool().get(varTy);

		// Resolve the binding's LLVM type from the source-level TypeIdx
		// rather than from the alloca instruction's allocated-type. This
		// keeps the codegen working uniformly whether the variable is
		// backed by an `alloca` (locals, value-passed params) or by a
		// function argument that's already a pointer to caller storage
		// (P9 mode-aware ABI for `mut` / `undefined` params).
		JamTypeRef allocatedType = ctx.getLLVMType(varTy);

		if (k.kind == TypeKind::PtrMany) {
			TypeIdx elemTy = static_cast<TypeIdx>(k.a);
			outElemType = ctx.getLLVMType(elemTy);
			JamValueRef ptrVal = JamLLVMBuildLoad(ctx.getBuilder(),
			                                      allocatedType, alloca,
			                                      name.c_str());
			return JamLLVMBuildPtrGEP(ctx.getBuilder(), outElemType, ptrVal,
			                          idxVal, "ptrgep");
		}
		if (k.kind == TypeKind::PtrSingle) {
			throw std::runtime_error(
			    "Cannot index single-item pointer; use `.*` to dereference, "
			    "or declare as `[*]T` for many-item indexing");
		}

		outElemType = JamLLVMGetArrayElementType(allocatedType);
		return JamLLVMBuildArrayGEP(ctx.getBuilder(), allocatedType, alloca,
		                            idxVal, "idxgep");
	}

	if (on.tag == AstTag::MemberAccess) {
		std::vector<StringIdx> path;
		StringIdx rootName = collectMemberChain(ns, objectIdx, path);
		if (rootName == kNoString) {
			throw std::runtime_error(
			    "Indexing into a non-variable lvalue is not supported");
		}
		const std::string &varName = sp.get(rootName);
		JamValueRef alloca = ctx.getVariable(varName);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + varName);
		}
		TypeIdx typeAtLevel = ctx.getVariableType(varName);
		JamValueRef currentPtr = alloca;
		JamTypeRef currentType = ctx.getLLVMType(typeAtLevel);

		for (size_t i = 0; i < path.size(); i++) {
			const auto *info = ctx.lookupStruct(typeAtLevel);
			if (!info) {
				throw std::runtime_error("Cannot index through field '" +
				                         sp.get(path[i]) + "' on non-struct");
			}
			const std::string &fieldName = sp.get(path[i]);
			int idx = ctx.getFieldIndex(info->name, fieldName);
			if (idx < 0) {
				throw std::runtime_error("Unknown field '" + fieldName +
				                         "' in struct " + info->name);
			}
			currentPtr = JamLLVMBuildStructGEP(
			    ctx.getBuilder(), currentType, currentPtr,
			    static_cast<unsigned>(idx), fieldName.c_str());
			typeAtLevel = info->fields[idx].second;
			currentType = ctx.getLLVMType(typeAtLevel);
		}

		outElemType = JamLLVMGetArrayElementType(currentType);
		return JamLLVMBuildArrayGEP(ctx.getBuilder(), currentType, currentPtr,
		                            idxVal, "idxgep");
	}

	throw std::runtime_error(
	    "Indexing supports only locals and struct field chains");
}

// ---------------------------------------------------------------------------
// Special-form calls (println/print/sleep/assert)
// ---------------------------------------------------------------------------

static JamValueRef genPrintCall(JamCodegenContext &ctx,
                                const std::string &callee,
                                const uint32_t *args, uint32_t argCount) {
	JamFunctionRef printfFunc = JamLLVMGetFunction(ctx.getModule(), "printf");
	if (!printfFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef printfRetType = ctx.getInt32Type();
		JamTypeRef printfParamTypes[1] = {i8PtrType};
		JamTypeRef printfType = JamLLVMFunctionType(
		    printfRetType, printfParamTypes, 1, true);
		printfFunc = JamLLVMAddFunction(ctx.getModule(), "printf", printfType);
		JamLLVMApplyDefaultFnAttrs(printfFunc, /*isExtern=*/true);
	}

	JamFunctionRef putsFunc = JamLLVMGetFunction(ctx.getModule(), "puts");
	if (!putsFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef putsRetType = ctx.getInt32Type();
		JamTypeRef putsParamTypes[1] = {i8PtrType};
		JamTypeRef putsType = JamLLVMFunctionType(
		    putsRetType, putsParamTypes, 1, false);
		putsFunc = JamLLVMAddFunction(ctx.getModule(), "puts", putsType);
		JamLLVMApplyDefaultFnAttrs(putsFunc, /*isExtern=*/true);
	}

	if (callee == "std.fmt.println" && argCount == 1) {
		JamValueRef arg = codegenNode(ctx, args[0]);
		if (!arg) return nullptr;
		if (JamLLVMTypeIsStruct(JamLLVMTypeOf(arg))) {
			arg = JamLLVMBuildExtractValue(ctx.getBuilder(), arg, 0,
			                               "str_ptr");
		}
		JamValueRef callArgs[1] = {arg};
		return JamLLVMBuildCall(ctx.getBuilder(), putsFunc, callArgs, 1,
		                        "puts_call");
	}
	if (callee == "std.fmt.print" && argCount == 1) {
		JamValueRef arg = codegenNode(ctx, args[0]);
		if (!arg) return nullptr;
		if (JamLLVMTypeIsStruct(JamLLVMTypeOf(arg))) {
			arg = JamLLVMBuildExtractValue(ctx.getBuilder(), arg, 0,
			                               "str_ptr");
		}
		JamValueRef formatPtr = JamLLVMBuildGlobalStringPtr(
		    ctx.getBuilder(), "%s", "print_fmt");
		JamValueRef callArgs[2] = {formatPtr, arg};
		return JamLLVMBuildCall(ctx.getBuilder(), printfFunc, callArgs, 2,
		                        "printf_call");
	}
	throw std::runtime_error("Complex print formatting not yet implemented");
}

static JamValueRef genSleepCall(JamCodegenContext &ctx, const uint32_t *args,
                                uint32_t argCount) {
	if (argCount != 1) {
		throw std::runtime_error(
		    "std.thread.sleep expects exactly 1 argument (milliseconds)");
	}
	JamFunctionRef usleepFunc =
	    JamLLVMGetFunction(ctx.getModule(), "usleep");
	if (!usleepFunc) {
		JamTypeRef usleepRetType = ctx.getInt32Type();
		JamTypeRef usleepParamTypes[1] = {ctx.getInt32Type()};
		JamTypeRef usleepType = JamLLVMFunctionType(usleepRetType,
		                                            usleepParamTypes, 1,
		                                            false);
		usleepFunc = JamLLVMAddFunction(ctx.getModule(), "usleep", usleepType);
		JamLLVMApplyDefaultFnAttrs(usleepFunc, /*isExtern=*/true);
	}

	JamValueRef msArg = codegenNode(ctx, args[0]);
	if (!msArg) return nullptr;

	JamTypeRef i64Type = ctx.getInt64Type();
	JamValueRef msArg64 = JamLLVMBuildIntCast(ctx.getBuilder(), msArg, i64Type,
	                                          false, "ms_cast");
	JamValueRef thousand = JamLLVMConstInt(i64Type, 1000, false);
	JamValueRef usArg64 = JamLLVMBuildMul(ctx.getBuilder(), msArg64, thousand,
	                                      "us_mul");
	JamValueRef usArg = JamLLVMBuildIntCast(ctx.getBuilder(), usArg64,
	                                        ctx.getInt32Type(), false,
	                                        "us_trunc");
	JamValueRef callArgs[1] = {usArg};
	return JamLLVMBuildCall(ctx.getBuilder(), usleepFunc, callArgs, 1,
	                        "usleep_call");
}

static JamValueRef genAssertCall(JamCodegenContext &ctx, const uint32_t *args,
                                 uint32_t argCount) {
	if (argCount != 2) {
		throw std::runtime_error(
		    "assert expects exactly 2 arguments (actual, expected)");
	}

	JamValueRef actual = codegenNode(ctx, args[0]);
	if (!actual) return nullptr;
	JamTypeRef actualType = JamLLVMTypeOf(actual);

	// Materialize integer-literal "expected" arg directly at actual's type
	// so we never need to guess sext vs zext.
	const AstNode &expectedNode = ctx.getNodeStore().get(args[1]);
	JamTypeRef expectedHint = nullptr;
	if (expectedNode.tag == AstTag::NumberLit &&
	    JamLLVMTypeIsInteger(actualType)) {
		expectedHint = actualType;
	}
	JamValueRef expected = codegenNode(ctx, args[1], expectedHint);
	if (!expected) return nullptr;

	JamTypeRef expectedType = JamLLVMTypeOf(expected);
	if (actualType != expectedType) {
		if (JamLLVMTypeIsInteger(actualType) &&
		    JamLLVMTypeIsInteger(expectedType)) {
			unsigned aw = JamLLVMGetIntTypeWidth(actualType);
			unsigned ew = JamLLVMGetIntTypeWidth(expectedType);
			if (aw > ew) {
				expected = JamLLVMBuildIntCast(ctx.getBuilder(), expected,
				                               actualType, false,
				                               "assert_cast");
			} else {
				actual = JamLLVMBuildIntCast(ctx.getBuilder(), actual,
				                             expectedType, false,
				                             "assert_cast");
			}
		}
	}

	JamValueRef cmpResult = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_EQ,
	                                         actual, expected, "assert_cmp");

	JamBasicBlockRef currentBlock = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(currentBlock);
	JamBasicBlockRef failBlock =
	    JamLLVMAppendBasicBlock(TheFunction, "assert.fail");
	JamBasicBlockRef passBlock =
	    JamLLVMAppendBasicBlock(TheFunction, "assert.pass");

	JamLLVMBuildCondBr(ctx.getBuilder(), cmpResult, passBlock, failBlock);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), failBlock);

	JamFunctionRef printfFunc = JamLLVMGetFunction(ctx.getModule(), "printf");
	if (!printfFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef printfParamTypes[1] = {i8PtrType};
		JamTypeRef printfType = JamLLVMFunctionType(
		    ctx.getInt32Type(), printfParamTypes, 1, true);
		printfFunc = JamLLVMAddFunction(ctx.getModule(), "printf", printfType);
		JamLLVMApplyDefaultFnAttrs(printfFunc, /*isExtern=*/true);
	}
	JamFunctionRef exitFunc = JamLLVMGetFunction(ctx.getModule(), "exit");
	if (!exitFunc) {
		JamTypeRef exitParamTypes[1] = {ctx.getInt32Type()};
		JamTypeRef exitType = JamLLVMFunctionType(
		    ctx.getVoidType(), exitParamTypes, 1, false);
		exitFunc = JamLLVMAddFunction(ctx.getModule(), "exit", exitType);
		JamLLVMApplyDefaultFnAttrs(exitFunc, /*isExtern=*/true);
	}

	JamValueRef fmtStr = JamLLVMBuildGlobalStringPtr(
	    ctx.getBuilder(), "assertion failed\n", "assert_fail_msg");
	JamValueRef printArgs[1] = {fmtStr};
	JamLLVMBuildCall(ctx.getBuilder(), printfFunc, printArgs, 1, "");

	JamValueRef exitCode = JamLLVMConstInt(ctx.getInt32Type(), 1, false);
	JamValueRef exitArgs[1] = {exitCode};
	JamLLVMBuildCall(ctx.getBuilder(), exitFunc, exitArgs, 1, "");
	JamLLVMBuildUnreachable(ctx.getBuilder());

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), passBlock);

	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

// ---------------------------------------------------------------------------
// Main switch-dispatched codegen
// ---------------------------------------------------------------------------

static JamValueRef codegenStringLit(JamCodegenContext &ctx,
                                    const std::string &val) {
	JamValueRef StrConstant = JamLLVMConstString(ctx.getContext(), val.c_str(),
	                                              val.length(), true);
	JamTypeRef strArrayType =
	    JamLLVMArrayType(ctx.getInt8Type(), val.length() + 1);
	JamValueRef StrGlobal =
	    JamLLVMAddGlobal(ctx.getModule(), strArrayType, "str");
	JamLLVMSetGlobalConstant(StrGlobal, true);
	JamLLVMSetInitializer(StrGlobal, StrConstant);

	JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
	JamTypeRef usizeType = ctx.getInt64Type();
	JamTypeRef sliceTypes[2] = {i8PtrType, usizeType};
	JamTypeRef sliceType = JamLLVMStructType(ctx.getContext(), sliceTypes, 2,
	                                          false);

	JamValueRef StrPtr = JamLLVMBuildBitCast(ctx.getBuilder(), StrGlobal,
	                                         i8PtrType, "str_ptr");
	JamValueRef SliceStruct = JamLLVMGetUndef(sliceType);
	SliceStruct = JamLLVMBuildInsertValue(ctx.getBuilder(), SliceStruct,
	                                       StrPtr, 0, "slice_ptr");
	SliceStruct = JamLLVMBuildInsertValue(
	    ctx.getBuilder(), SliceStruct,
	    JamLLVMConstInt(usizeType, val.length(), false), 1, "slice_len");
	return SliceStruct;
}

static JamValueRef codegenBinaryOp(JamCodegenContext &ctx, const AstNode &n) {
	BinOp op = static_cast<BinOp>(n.op);
	const NodeStore &ns = ctx.getNodeStore();
	NodeIdx lhsIdx = static_cast<NodeIdx>(n.lhs);
	NodeIdx rhsIdx = static_cast<NodeIdx>(n.rhs);

	// Short-circuit logical operators.
	if (op == BinOp::LogAnd || op == BinOp::LogOr) {
		JamValueRef L = codegenNode(ctx, lhsIdx);
		if (!L) return nullptr;

		JamBasicBlockRef currentBlock = JamLLVMGetInsertBlock(ctx.getBuilder());
		JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(currentBlock);
		JamBasicBlockRef rhsBlock = JamLLVMAppendBasicBlock(
		    TheFunction, op == BinOp::LogAnd ? "and.rhs" : "or.rhs");
		JamBasicBlockRef mergeBlock = JamLLVMAppendBasicBlock(
		    TheFunction, op == BinOp::LogAnd ? "and.end" : "or.end");

		if (op == BinOp::LogAnd) {
			JamLLVMBuildCondBr(ctx.getBuilder(), L, rhsBlock, mergeBlock);
		} else {
			JamLLVMBuildCondBr(ctx.getBuilder(), L, mergeBlock, rhsBlock);
		}

		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), rhsBlock);
		JamValueRef R = codegenNode(ctx, rhsIdx);
		if (!R) return nullptr;
		JamLLVMBuildBr(ctx.getBuilder(), mergeBlock);
		rhsBlock = JamLLVMGetInsertBlock(ctx.getBuilder());

		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), mergeBlock);
		JamValueRef phi = JamLLVMBuildPhi(
		    ctx.getBuilder(), ctx.getInt1Type(),
		    op == BinOp::LogAnd ? "and.result" : "or.result");

		JamValueRef shortCircuitVal = JamLLVMConstInt(
		    ctx.getInt1Type(), op == BinOp::LogAnd ? 0 : 1, false);
		JamValueRef incomingVals[2] = {shortCircuitVal, R};
		JamBasicBlockRef incomingBlocks[2] = {currentBlock, rhsBlock};
		JamLLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
		return phi;
	}

	// For mixed literal/concrete, materialize the literal at the concrete
	// side's type so the binary op sees aligned widths from the start.
	bool lIsLit = ns.get(lhsIdx).tag == AstTag::NumberLit;
	bool rIsLit = ns.get(rhsIdx).tag == AstTag::NumberLit;
	JamValueRef L = nullptr, R = nullptr;
	if (lIsLit && !rIsLit) {
		R = codegenNode(ctx, rhsIdx);
		JamTypeRef expected = R ? JamLLVMTypeOf(R) : nullptr;
		L = codegenNode(ctx, lhsIdx, expected);
	} else if (!lIsLit && rIsLit) {
		L = codegenNode(ctx, lhsIdx);
		JamTypeRef expected = L ? JamLLVMTypeOf(L) : nullptr;
		R = codegenNode(ctx, rhsIdx, expected);
	} else {
		L = codegenNode(ctx, lhsIdx);
		R = codegenNode(ctx, rhsIdx);
	}
	if (!L || !R) return nullptr;

	// Width-align if still mismatched (two literals of different widths, or
	// two vars of different declared widths).
	JamTypeRef lt = JamLLVMTypeOf(L);
	JamTypeRef rt = JamLLVMTypeOf(R);
	if (lt != rt && JamLLVMTypeIsInteger(lt) && JamLLVMTypeIsInteger(rt)) {
		unsigned lw = JamLLVMGetIntTypeWidth(lt);
		unsigned rw = JamLLVMGetIntTypeWidth(rt);
		if (lw > rw) R = coerceTo(ctx, R, lt);
		else L = coerceTo(ctx, L, rt);
	}

	switch (op) {
	case BinOp::Add: return JamLLVMBuildAdd(ctx.getBuilder(), L, R, "addtmp");
	case BinOp::Sub: return JamLLVMBuildSub(ctx.getBuilder(), L, R, "subtmp");
	case BinOp::Mul: return JamLLVMBuildMul(ctx.getBuilder(), L, R, "multmp");
	case BinOp::Mod:
		return JamLLVMBuildURem(ctx.getBuilder(), L, R, "remtmp");
	case BinOp::BitAnd:
		return JamLLVMBuildAnd(ctx.getBuilder(), L, R, "andtmp");
	case BinOp::BitOr:
		return JamLLVMBuildOr(ctx.getBuilder(), L, R, "ortmp");
	case BinOp::BitXor:
		return JamLLVMBuildXor(ctx.getBuilder(), L, R, "xortmp");
	case BinOp::Shl:
		return JamLLVMBuildShl(ctx.getBuilder(), L, R, "shltmp");
	case BinOp::Shr:
		return JamLLVMBuildLShr(ctx.getBuilder(), L, R, "shrtmp");
	case BinOp::Eq:
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_EQ, L, R, "cmptmp");
	case BinOp::Ne:
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, L, R, "cmptmp");
	case BinOp::Lt:
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_ULT, L, R,
		                         "cmptmp");
	case BinOp::Le:
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_ULE, L, R,
		                         "cmptmp");
	case BinOp::Gt:
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_UGT, L, R,
		                         "cmptmp");
	case BinOp::Ge:
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_UGE, L, R,
		                         "cmptmp");
	default:
		throw std::runtime_error("Invalid binary operator");
	}
}

static JamValueRef codegenUnaryOp(JamCodegenContext &ctx, const AstNode &n) {
	UnaryOp op = static_cast<UnaryOp>(n.op);
	JamValueRef operandVal =
	    codegenNode(ctx, static_cast<NodeIdx>(n.lhs));
	if (!operandVal) return nullptr;
	switch (op) {
	case UnaryOp::LogNot:
		return JamLLVMBuildXor(
		    ctx.getBuilder(), operandVal,
		    JamLLVMConstInt(ctx.getInt1Type(), 1, false), "nottmp");
	case UnaryOp::BitNot: {
		JamTypeRef ty = JamLLVMTypeOf(operandVal);
		JamValueRef allOnes = JamLLVMConstInt(ty, ~0ULL, false);
		return JamLLVMBuildXor(ctx.getBuilder(), operandVal, allOnes,
		                       "nottmp");
	}
	case UnaryOp::Neg: {
		JamTypeRef ty = JamLLVMTypeOf(operandVal);
		JamValueRef zero = JamLLVMConstInt(ty, 0, false);
		return JamLLVMBuildSub(ctx.getBuilder(), zero, operandVal, "negtmp");
	}
	default:
		throw std::runtime_error("Invalid unary operator");
	}
}

static JamValueRef codegenCall(JamCodegenContext &ctx, const AstNode &n) {
	const std::string &callee =
	    ctx.getStringPool().get(static_cast<StringIdx>(n.lhs));
	ExtraIdx extra = static_cast<ExtraIdx>(n.rhs);
	uint32_t argCount = ctx.getNodeStore().getExtra(extra);
	std::vector<uint32_t> args(argCount);
	for (uint32_t i = 0; i < argCount; i++) {
		args[i] = ctx.getNodeStore().getExtra(extra + 1 + i);
	}

	if (callee == "std.fmt.print" || callee == "std.fmt.println") {
		return genPrintCall(ctx, callee, args.data(), argCount);
	}
	if (callee == "std.thread.sleep") {
		return genSleepCall(ctx, args.data(), argCount);
	}
	if (callee == "assert") {
		return genAssertCall(ctx, args.data(), argCount);
	}

	// Enum-variant constructor: `EnumName.VariantName(arg0, arg1, …)`
	// builds a tagged enum value. The qualified callee name has shape
	// `<EnumName>.<VariantName>` — split it and check the registry.
	{
		size_t dot = callee.find('.');
		if (dot != std::string::npos &&
		    callee.find('.', dot + 1) == std::string::npos) {
			std::string enumName = callee.substr(0, dot);
			std::string varName = callee.substr(dot + 1);
			if (const auto *einfo = ctx.getEnum(enumName)) {
				int idx = ctx.getEnumVariantIndex(enumName, varName);
				if (idx < 0) {
					throw std::runtime_error("Enum `" + enumName +
					                         "` has no variant `" + varName +
					                         "`");
				}
				const auto &v = einfo->variants[idx];
				if (argCount != v.payloadTypes.size()) {
					throw std::runtime_error(
					    "Variant `" + enumName + "." + varName +
					    "` expects " +
					    std::to_string(v.payloadTypes.size()) +
					    " payload arg(s), got " + std::to_string(argCount));
				}
				// E1 unit-only enum: just the tag (discriminant value).
				if (!einfo->hasPayloadVariant) {
					return JamLLVMConstInt(
					    ctx.getInt8Type(),
					    static_cast<uint64_t>(v.discriminant), false);
				}
				JamTypeRef enumLLVMType = einfo->type;
				// Allocate an enum-typed slot, store the tag at offset
				// 0, and store each payload arg at its computed offset
				// within the payload area. Then load the whole struct
				// to return as an SSA value.
				uint64_t enumAlign =
				    einfo->maxPayloadAlign > 1 ? einfo->maxPayloadAlign : 1;
				JamValueRef alloca = JamLLVMBuildAlloca(
				    ctx.getBuilder(), enumLLVMType, enumAlign, "enum.tmp");
				// Store tag at field 0.
				JamValueRef tagPtr = JamLLVMBuildStructGEP(
				    ctx.getBuilder(), enumLLVMType, alloca, 0, "enum.tag");
				JamLLVMBuildStore(
				    ctx.getBuilder(),
				    JamLLVMConstInt(ctx.getInt8Type(),
				                    static_cast<uint64_t>(v.discriminant),
				                    false),
				    tagPtr);
				// If variant has payload, store fields. Payload area is
				// the LAST field of the enum struct (index 1 if no
				// padding, index 2 if padding bytes inserted).
				if (!v.payloadTypes.empty()) {
					// Payload starts at struct field 1 (the
					// alignment-driving scalar); larger payloads spill
					// into field 2 via byte-offset GEP from field 1.
					unsigned payloadFieldIdx = 1;
					JamValueRef payloadAreaPtr = JamLLVMBuildStructGEP(
					    ctx.getBuilder(), enumLLVMType, alloca,
					    payloadFieldIdx, "enum.payload");
					(void)einfo;
					uint64_t off = 0;
					for (size_t i = 0; i < v.payloadTypes.size(); i++) {
						uint64_t s = ctx.typeSize(v.payloadTypes[i]);
						uint64_t a = ctx.typeAlign(v.payloadTypes[i]);
						off = (off + a - 1) / a * a;
						// GEP into the payload bytes at offset `off`.
						JamValueRef i64Off = JamLLVMConstInt(
						    ctx.getInt64Type(), off, false);
						JamValueRef fieldPtr = JamLLVMBuildPtrGEP(
						    ctx.getBuilder(), ctx.getInt8Type(),
						    payloadAreaPtr, i64Off, "enum.field.ptr");
						JamTypeRef fieldTy =
						    ctx.getLLVMType(v.payloadTypes[i]);
						JamValueRef argVal =
						    codegenNode(ctx, args[i], fieldTy);
						if (!argVal) return nullptr;
						argVal = coerceTo(ctx, argVal, fieldTy);
						JamLLVMBuildStore(ctx.getBuilder(), argVal, fieldPtr);
						off += s;
					}
				}
				// Load the constructed enum value from the alloca.
				return JamLLVMBuildLoad(ctx.getBuilder(), enumLLVMType,
				                        alloca, "enum.val");
			}
		}
	}

	// Struct-method qualified call: `Struct.method(args...)`. Methods are
	// registered in the function table under their qualified source-level
	// name; the LLVM symbol is whatever mangledFunctionName produces
	// (drop methods share the existing `__drop_T` mangling). The rest of
	// this function uses `callee` (source-level) for FunctionAST lookups
	// and `llvmName` for the LLVM symbol — they only differ for methods.
	std::string llvmName = callee;
	{
		size_t dot = callee.find('.');
		if (dot != std::string::npos &&
		    callee.find('.', dot + 1) == std::string::npos) {
			std::string typeName = callee.substr(0, dot);
			if (ctx.getStruct(typeName)) {
				const FunctionAST *methodAST = ctx.getFunctionAST(callee);
				if (methodAST) {
					llvmName = mangledFunctionName(
					    *methodAST, ctx.getTypePool(),
					    ctx.getStringPool());
				}
			}
		}
	}

	JamFunctionRef CalleeF =
	    JamLLVMGetFunction(ctx.getModule(), llvmName.c_str());
	if (!CalleeF) {
		throw std::runtime_error("Unknown function referenced: " + callee);
	}

	// P9.5: when the callee's parameter is a Let/Move-mode aggregate that
	// the ABI classifier sends ByPointer (size > 16 B), the call site
	// implicitly passes the address of the arg's storage rather than the
	// value. The user does NOT write `&` for these — by-value semantics
	// are preserved at the source level; the pointer is purely an ABI
	// optimization. (Mut and Undefined modes still require explicit `&`
	// at the call site since their borrow shape is user-visible.)
	const FunctionAST *calleeAST = ctx.getFunctionAST(callee);

	// P9.6 sret: when the callee returns a large aggregate, the call
	// site allocates a result slot and passes its address as the
	// leading argument. The call itself returns void; the "value" of
	// the call expression is a load from the slot.
	JamValueRef calleeSretSlot = nullptr;
	JamTypeRef calleeSretPointee = nullptr;
	if (calleeAST && !calleeAST->isExtern && calleeAST->ReturnType != kNoType) {
		jam::abi::ReturnABI calleeRabi =
		    jam::abi::classifyReturn(calleeAST->ReturnType, ctx);
		if (calleeRabi.kind == jam::abi::ReturnABI::Kind::Indirect) {
			calleeSretPointee = ctx.getLLVMType(calleeAST->ReturnType);
			calleeSretSlot = JamLLVMBuildAlloca(
			    ctx.getBuilder(), calleeSretPointee,
			    ctx.typeAlign(calleeAST->ReturnType), "sretslot");
		}
	}

	// The user's arg count needs to match the callee's *source-level*
	// parameter count. The LLVM declaredParamCount also includes the
	// sret slot when applicable; compensate so the user-facing error
	// message matches what the user wrote.
	unsigned declaredParamCount = JamLLVMCountParams(CalleeF);
	unsigned userParamCount =
	    declaredParamCount - (calleeSretSlot != nullptr ? 1u : 0u);
	bool isVarArg = JamLLVMFunctionIsVarArg(CalleeF);
	if (isVarArg ? argCount < userParamCount
	             : argCount != userParamCount) {
		throw std::runtime_error("Incorrect number of arguments passed to " +
		                         callee);
	}

	const unsigned calleeSretOffset = (calleeSretSlot != nullptr) ? 1u : 0u;

	std::vector<JamValueRef> ArgsV;
	if (calleeSretSlot != nullptr) {
		ArgsV.push_back(calleeSretSlot);
	}
	for (unsigned i = 0; i < argCount; i++) {
		if (i < userParamCount) {
			JamTypeRef expected = JamLLVMTypeOf(
			    JamLLVMGetParam(CalleeF, i + calleeSretOffset));

			bool autoAddress = false;
			// P9.8: extern callees follow the C ABI literally; never
			// auto-address. The user wrote the FFI-facing parameter
			// types directly, and LLVM's backend handles any byval
			// passing per the platform's MEMORY classification.
			if (calleeAST && !calleeAST->isExtern &&
			    i < calleeAST->Args.size()) {
				const Param &p = calleeAST->Args[i];
				if (p.Mode == ParamMode::Let || p.Mode == ParamMode::Move) {
					auto pabi = jam::abi::classifyParam(p.Mode, p.Type, ctx);
					if (pabi.kind == jam::abi::ParamABI::Kind::ByPointer) {
						autoAddress = true;
					}
				}
			}

			JamValueRef argVal;
			if (autoAddress) {
				const AstNode &argNode = ctx.getNodeStore().get(args[i]);
				if (argNode.tag == AstTag::Variable ||
				    argNode.tag == AstTag::Index ||
				    argNode.tag == AstTag::MemberAccess) {
					// lvalue: take its address directly via the same
					// path that the explicit `&` operator uses.
					AstNode fakeAddrOf{
					    AstTag::AddressOf, 0, 0, 0,
					    static_cast<uint32_t>(args[i]), 0};
					argVal = codegenAddressOf(ctx, fakeAddrOf);
				} else {
					// rvalue: codegen as a value, store into a fresh
					// stack slot, pass the slot's address.
					JamTypeRef llvmTy =
					    ctx.getLLVMType(calleeAST->Args[i].Type);
					JamValueRef tmp = JamLLVMBuildAlloca(
					    ctx.getBuilder(), llvmTy,
					    ctx.typeAlign(calleeAST->Args[i].Type), "argtmp");
					JamValueRef val = codegenNode(ctx, args[i], llvmTy);
					if (!val) return nullptr;
					val = coerceTo(ctx, val, llvmTy);
					JamLLVMBuildStore(ctx.getBuilder(), val, tmp);
					argVal = tmp;
				}
			} else {
				argVal = codegenNode(ctx, args[i], expected);
				if (!argVal) return nullptr;
				argVal = coerceTo(ctx, argVal, expected);
			}
			ArgsV.push_back(argVal);
		} else {
			JamValueRef argVal = codegenNode(ctx, args[i]);
			if (!argVal) return nullptr;
			ArgsV.push_back(argVal);
		}
	}

	const char *callName =
	    JamLLVMTypeIsVoid(JamLLVMGetReturnType(CalleeF)) ? "" : "calltmp";
	JamValueRef callResult = JamLLVMBuildCall(
	    ctx.getBuilder(), CalleeF, ArgsV.data(), ArgsV.size(), callName);

	// P9.6: when sret was used, the call returned void and the actual
	// result lives in the pre-allocated slot. Load it so the surrounding
	// expression sees the value.
	if (calleeSretSlot != nullptr) {
		return JamLLVMBuildLoad(ctx.getBuilder(), calleeSretPointee,
		                        calleeSretSlot, "sretload");
	}
	return callResult;
}

// Forward decls — these helpers are defined further down (alongside
// FunctionAST codegen for mangledFunctionName, alongside codegenVarDecl
// for the drop emitters) so the source order can stay readable without
// re-ordering large blocks.
static std::string mangledFunctionName(const FunctionAST &fn,
                                       const TypePool &types,
                                       const StringPool &strings);
static void emitTopScopeDrops(JamCodegenContext &ctx);
static void emitAllScopeDrops(JamCodegenContext &ctx);

static JamValueRef codegenReturn(JamCodegenContext &ctx, const AstNode &n) {
	NodeIdx retIdx = static_cast<NodeIdx>(n.lhs);
	if (retIdx == kNoNode) {
		emitAllScopeDrops(ctx);
		JamLLVMBuildRetVoid(ctx.getBuilder());
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	}

	// P9.6: when the enclosing function returns via sret, the codegen
	// stores the return value into the caller-provided slot rather than
	// returning by value. The slot's LLVM pointee type tells us what
	// shape the codegen of the return expression should produce.
	JamValueRef sretSlot = ctx.getSretSlot();
	if (sretSlot != nullptr) {
		JamFunctionRef func = JamLLVMGetBasicBlockParent(
		    JamLLVMGetInsertBlock(ctx.getBuilder()));
		(void)func;  // sret functions return void; LLVM-level RetType is void
		JamValueRef RetVal = codegenNode(ctx, retIdx);
		if (!RetVal) return nullptr;
		JamLLVMBuildStore(ctx.getBuilder(), RetVal, sretSlot);
		emitAllScopeDrops(ctx);
		JamLLVMBuildRetVoid(ctx.getBuilder());
		return RetVal;
	}

	JamFunctionRef func = JamLLVMGetBasicBlockParent(
	    JamLLVMGetInsertBlock(ctx.getBuilder()));
	JamTypeRef expected = JamLLVMGetReturnType(func);
	// Codegen the return value BEFORE emitting drops so reads of any
	// drop-bearing binding still see live storage. Then emit drops for
	// every active scope (innermost first), then the actual ret.
	JamValueRef RetVal = codegenNode(ctx, retIdx, expected);
	if (!RetVal) return nullptr;
	if (JamLLVMTypeIsInteger(expected)) {
		RetVal = coerceTo(ctx, RetVal, expected);
	}
	emitAllScopeDrops(ctx);
	JamLLVMBuildRet(ctx.getBuilder(), RetVal);
	return RetVal;
}

static JamValueRef codegenAssign(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	const StringPool &sp = ctx.getStringPool();
	NodeIdx targetIdx = static_cast<NodeIdx>(n.lhs);
	NodeIdx valueIdx = static_cast<NodeIdx>(n.rhs);
	const AstNode &target = ns.get(targetIdx);

	// x = value
	if (target.tag == AstTag::Variable) {
		const std::string &name =
		    sp.get(static_cast<StringIdx>(target.lhs));
		JamValueRef alloca = ctx.getVariable(name);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + name);
		}
		JamTypeRef expected = ctx.getLLVMType(ctx.getVariableType(name));
		JamValueRef rhsVal = codegenNode(ctx, valueIdx, expected);
		if (!rhsVal) return nullptr;
		rhsVal = coerceTo(ctx, rhsVal, expected);
		JamLLVMBuildStore(ctx.getBuilder(), rhsVal, alloca);
		return rhsVal;
	}

	// p.* = value
	if (target.tag == AstTag::Deref) {
		const AstNode &op = ns.get(static_cast<NodeIdx>(target.lhs));
		if (op.tag != AstTag::Variable) {
			throw std::runtime_error(
			    "`.* =` is only supported on a pointer-typed variable");
		}
		const std::string &name = sp.get(static_cast<StringIdx>(op.lhs));
		JamValueRef alloca = ctx.getVariable(name);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + name);
		}
		TypeIdx pointee = pointeeTypeOf(ctx.getTypePool(),
		                                ctx.getVariableType(name));
		if (pointee == kNoType) {
			throw std::runtime_error("Cannot dereference non-pointer: " +
			                         name);
		}
		JamTypeRef pointeeType = ctx.getLLVMType(pointee);
		JamTypeRef ptrType = ctx.getLLVMType(ctx.getVariableType(name));
		JamValueRef ptrVal = JamLLVMBuildLoad(ctx.getBuilder(), ptrType,
		                                      alloca, name.c_str());
		JamValueRef rhsVal = codegenNode(ctx, valueIdx, pointeeType);
		if (!rhsVal) return nullptr;
		rhsVal = coerceTo(ctx, rhsVal, pointeeType);
		JamLLVMBuildStore(ctx.getBuilder(), rhsVal, ptrVal);
		return rhsVal;
	}

	// arr[i] = value (or g.board[i] = value)
	if (target.tag == AstTag::Index) {
		JamValueRef rhsVal = codegenNode(ctx, valueIdx);
		if (!rhsVal) return nullptr;
		JamValueRef idxVal = codegenNode(ctx, static_cast<NodeIdx>(target.rhs));
		if (!idxVal) return nullptr;
		idxVal = coerceTo(ctx, idxVal, ctx.getInt64Type());
		JamTypeRef elemType = nullptr;
		JamValueRef elemPtr = resolveIndexedElementPtr(
		    ctx, static_cast<NodeIdx>(target.lhs), idxVal, elemType);
		rhsVal = coerceTo(ctx, rhsVal, elemType);
		JamLLVMBuildStore(ctx.getBuilder(), rhsVal, elemPtr);
		return rhsVal;
	}

	// a.b.c = value (struct field chain)
	if (target.tag == AstTag::MemberAccess) {
		std::vector<StringIdx> path;
		StringIdx rootName = collectMemberChain(ns, targetIdx, path);
		if (rootName == kNoString) {
			throw std::runtime_error("Invalid assignment target");
		}
		const std::string &varName = sp.get(rootName);
		JamValueRef alloca = ctx.getVariable(varName);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + varName);
		}
		TypeIdx varTy = ctx.getVariableType(varName);

		// Union member write (single-level only in M1). All fields share
		// the same address, so the write is just a store of the rhs at
		// the union's allocation, typed as the field type.
		if (path.size() == 1) {
			const auto *uinfo = ctx.lookupUnion(varTy);
			if (uinfo) {
				const std::string &member = sp.get(path[0]);
				TypeIdx fieldTy =
				    ctx.getUnionFieldType(uinfo->name, member);
				if (fieldTy == kNoType) {
					throw std::runtime_error("Union `" + uinfo->name +
					                         "` has no field `" + member +
					                         "`");
				}
				JamTypeRef expected = ctx.getLLVMType(fieldTy);
				JamValueRef rhsVal =
				    codegenNode(ctx, valueIdx, expected);
				if (!rhsVal) return nullptr;
				rhsVal = coerceTo(ctx, rhsVal, expected);
				JamLLVMBuildStore(ctx.getBuilder(), rhsVal, alloca);
				return rhsVal;
			}
		}

		JamValueRef rhsVal = codegenNode(ctx, valueIdx);
		if (!rhsVal) return nullptr;

		const auto *info = ctx.lookupStruct(varTy);
		if (!info) {
			throw std::runtime_error(
			    "Cannot assign to field of non-struct: " + varName);
		}

		// Walk the field chain with struct GEPs so we end up with a pointer
		// straight to the leaf field, then store only that field. The earlier
		// approach loaded the whole struct, ran an extractvalue/insertvalue
		// dance, and stored the whole struct back — wasteful in the IR and
		// in unoptimized machine code, plus it loaded poison from an uninit
		// alloca whenever the struct hadn't been fully initialized.
		JamValueRef leafPtr = alloca;
		JamTypeRef leafLLVMType = info->type;
		TypeIdx typeAtLevel = varTy;
		TypeIdx leafFieldType = kNoType;
		for (size_t i = 0; i < path.size(); i++) {
			const auto *curInfo = ctx.lookupStruct(typeAtLevel);
			if (!curInfo) {
				throw std::runtime_error("Cannot access field '" +
				                         sp.get(path[i]) + "' on non-struct");
			}
			const std::string &fieldName = sp.get(path[i]);
			int idx = ctx.getFieldIndex(curInfo->name, fieldName);
			if (idx < 0) {
				throw std::runtime_error("Unknown field '" + fieldName +
				                         "' in " + curInfo->name);
			}
			leafPtr = JamLLVMBuildStructGEP(
			    ctx.getBuilder(), leafLLVMType, leafPtr,
			    static_cast<unsigned>(idx), fieldName.c_str());
			typeAtLevel = curInfo->fields[idx].second;
			leafLLVMType = ctx.getLLVMType(typeAtLevel);
			if (i + 1 == path.size()) {
				leafFieldType = typeAtLevel;
			}
		}

		JamTypeRef expected = ctx.getLLVMType(leafFieldType);
		JamValueRef newValue = coerceTo(ctx, rhsVal, expected);
		JamLLVMBuildStore(ctx.getBuilder(), newValue, leafPtr);
		return rhsVal;
	}

	throw std::runtime_error("Invalid assignment target");
}

static JamValueRef codegenIndex(JamCodegenContext &ctx, const AstNode &n) {
	JamValueRef idxVal = codegenNode(ctx, static_cast<NodeIdx>(n.rhs));
	if (!idxVal) return nullptr;
	idxVal = coerceTo(ctx, idxVal, ctx.getInt64Type());
	JamTypeRef elemType = nullptr;
	JamValueRef elemPtr = resolveIndexedElementPtr(
	    ctx, static_cast<NodeIdx>(n.lhs), idxVal, elemType);
	return JamLLVMBuildLoad(ctx.getBuilder(), elemType, elemPtr, "idxload");
}

static JamValueRef codegenDeref(JamCodegenContext &ctx, const AstNode &n) {
	const AstNode &op = ctx.getNodeStore().get(static_cast<NodeIdx>(n.lhs));
	if (op.tag != AstTag::Variable) {
		throw std::runtime_error(
		    "`.* ` is only supported on a pointer-typed variable");
	}
	const std::string &name =
	    ctx.getStringPool().get(static_cast<StringIdx>(op.lhs));
	JamValueRef alloca = ctx.getVariable(name);
	if (!alloca) {
		throw std::runtime_error("Unknown variable: " + name);
	}
	TypeIdx pointee = pointeeTypeOf(ctx.getTypePool(),
	                                ctx.getVariableType(name));
	if (pointee == kNoType) {
		throw std::runtime_error("Cannot dereference non-pointer: " + name);
	}
	JamTypeRef pointeeType = ctx.getLLVMType(pointee);
	JamTypeRef ptrType = ctx.getLLVMType(ctx.getVariableType(name));
	JamValueRef ptrVal = JamLLVMBuildLoad(ctx.getBuilder(), ptrType, alloca,
	                                      name.c_str());
	return JamLLVMBuildLoad(ctx.getBuilder(), pointeeType, ptrVal, "deref");
}

static JamValueRef codegenAddressOf(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	const StringPool &sp = ctx.getStringPool();
	NodeIdx operandIdx = static_cast<NodeIdx>(n.lhs);
	const AstNode &op = ns.get(operandIdx);

	if (op.tag == AstTag::Variable) {
		const std::string &name = sp.get(static_cast<StringIdx>(op.lhs));
		JamValueRef alloca = ctx.getVariable(name);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + name);
		}
		return alloca;
	}

	if (op.tag == AstTag::Index) {
		JamValueRef idxVal = codegenNode(ctx, static_cast<NodeIdx>(op.rhs));
		if (!idxVal) return nullptr;
		idxVal = coerceTo(ctx, idxVal, ctx.getInt64Type());
		JamTypeRef elemType = nullptr;
		return resolveIndexedElementPtr(ctx, static_cast<NodeIdx>(op.lhs),
		                                idxVal, elemType);
	}

	if (op.tag == AstTag::MemberAccess) {
		std::vector<StringIdx> path;
		StringIdx rootName = collectMemberChain(ns, operandIdx, path);
		if (rootName == kNoString) {
			throw std::runtime_error(
			    "Address-of a non-variable lvalue is not supported");
		}
		const std::string &varName = sp.get(rootName);
		JamValueRef alloca = ctx.getVariable(varName);
		if (!alloca) {
			throw std::runtime_error("Unknown variable: " + varName);
		}
		TypeIdx typeAtLevel = ctx.getVariableType(varName);
		JamValueRef currentPtr = alloca;
		JamTypeRef currentType = ctx.getLLVMType(typeAtLevel);
		for (size_t i = 0; i < path.size(); i++) {
			const auto *info = ctx.lookupStruct(typeAtLevel);
			if (!info) {
				throw std::runtime_error("Cannot take address of field '" +
				                         sp.get(path[i]) + "' on non-struct");
			}
			const std::string &fieldName = sp.get(path[i]);
			int idx = ctx.getFieldIndex(info->name, fieldName);
			if (idx < 0) {
				throw std::runtime_error("Unknown field '" + fieldName +
				                         "' in struct " + info->name);
			}
			currentPtr = JamLLVMBuildStructGEP(
			    ctx.getBuilder(), currentType, currentPtr,
			    static_cast<unsigned>(idx), fieldName.c_str());
			typeAtLevel = info->fields[idx].second;
			currentType = ctx.getLLVMType(typeAtLevel);
		}
		return currentPtr;
	}

	throw std::runtime_error("Cannot take address of this expression");
}

static JamValueRef codegenVarDecl(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	const StringPool &sp = ctx.getStringPool();
	ExtraIdx extra = static_cast<ExtraIdx>(n.lhs);
	StringIdx nameId = static_cast<StringIdx>(ns.getExtra(extra));
	TypeIdx type = static_cast<TypeIdx>(ns.getExtra(extra + 1));
	NodeIdx initIdx = static_cast<NodeIdx>(ns.getExtra(extra + 2));
	const std::string &name = sp.get(nameId);

	JamTypeRef VarType = ctx.getLLVMType(type);
	JamValueRef Alloca = JamLLVMBuildAlloca(
	    ctx.getBuilder(), VarType, ctx.typeAlign(type), name.c_str());

	if (initIdx != kNoNode) {
		const AstNode &initNode = ns.get(initIdx);
		// `= undefined` — leave alloca uninitialized.
		if (initNode.tag != AstTag::UndefinedLit) {
			// Patch struct literals with the declared target struct type so
			// they can resolve fields and coerce values during their codegen.
			if (initNode.tag == AstTag::StructLit) {
				ctx.getNodeStore().getMut(initIdx).lhs = type;
			}
			JamValueRef InitVal = codegenNode(ctx, initIdx, VarType);
			if (!InitVal) return nullptr;
			InitVal = coerceTo(ctx, InitVal, VarType);
			JamLLVMBuildStore(ctx.getBuilder(), InitVal, Alloca);
		}
	}

	ctx.setVariable(name, Alloca);
	ctx.setVariableType(name, type);

	// P8.1: register the binding for drop emission at scope exit if its
	// type has a user-defined `fn drop(self: mut T)`. The init analyzer
	// has already rejected `move` on drop-bearing bindings (P8 foundation),
	// so codegen can emit drops unconditionally without double-free risk
	// from caller-side moves. (Bindings declared `= undefined` and never
	// assigned would still drop on uninit memory; tracking that is P8.2.)
	if (const auto *reg = ctx.getDropRegistry()) {
		const TypeKey &k = ctx.getTypePool().get(type);
		if (k.kind == TypeKind::Struct || k.kind == TypeKind::Named) {
			StringIdx structNameIdx = static_cast<StringIdx>(k.a);
			if (structNameIdx != kNoString) {
				const std::string &structName =
				    ctx.getStringPool().get(structNameIdx);
				auto it = reg->find(structName);
				if (it != reg->end()) {
					ctx.registerLocalDrop(name, Alloca, VarType,
					                      it->second);
				}
			}
		}
	}
	return Alloca;
}

// P8.1 + P9: emit a single drop call for one tracked binding. P9's
// mode-aware ABI now lowers `fn drop(self: mut T)` as a pointer-typed
// parameter, so the call site passes the binding's storage address
// directly — no load-value-then-pass-by-value workaround. The drop fn
// reads/writes self through the pointer and the caller's storage is
// genuinely affected.
static void emitOneDrop(JamCodegenContext &ctx,
                        const JamCodegenContext::DropEntry &e) {
	std::string mangled = mangledFunctionName(*e.dropFn, ctx.getTypePool(),
	                                          ctx.getStringPool());
	JamFunctionRef dropFn =
	    JamLLVMGetFunction(ctx.getModule(), mangled.c_str());
	if (!dropFn) return;
	JamValueRef args[1] = {e.alloca};
	JamLLVMBuildCall(ctx.getBuilder(), dropFn, args, 1, "");
}

// P8.3: emit drops for the topmost active scope, in reverse declaration
// order. Used at the end of nested blocks (if/else arms, while/for body,
// match arm body, function body fall-through) just before branching to
// the merge / cond / next-statement BB.
static void emitTopScopeDrops(JamCodegenContext &ctx) {
	const auto &scopes = ctx.getDropScopes();
	if (scopes.empty()) return;
	const auto &top = scopes.back();
	for (auto it = top.rbegin(); it != top.rend(); ++it) {
		emitOneDrop(ctx, *it);
	}
}

// P8.3: emit drops for every active scope, innermost-first. Used at every
// `return` statement so a return inside a nested block drops both the
// block-scoped locals and the outer function-scoped locals.
static void emitAllScopeDrops(JamCodegenContext &ctx) {
	const auto &scopes = ctx.getDropScopes();
	for (auto sit = scopes.rbegin(); sit != scopes.rend(); ++sit) {
		for (auto eit = sit->rbegin(); eit != sit->rend(); ++eit) {
			emitOneDrop(ctx, *eit);
		}
	}
}

// P8.4: emit drops for every scope from the innermost down to (and
// including) `targetScopeIdx`. Used by `break` and `continue` to drop
// the loop body's locals — and any deeper nested scopes — before
// branching out of (or to the next iteration of) the loop. The function
// scope and any other scopes outside the target index are preserved.
static void emitDropsThroughScope(JamCodegenContext &ctx,
                                  std::size_t targetScopeIdx) {
	const auto &scopes = ctx.getDropScopes();
	if (scopes.empty() || targetScopeIdx >= scopes.size()) return;
	for (std::size_t i = scopes.size(); i > targetScopeIdx; i--) {
		const auto &scope = scopes[i - 1];
		for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
			emitOneDrop(ctx, *it);
		}
	}
}

static JamValueRef codegenIf(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	NodeIdx condIdx = static_cast<NodeIdx>(n.lhs);
	ExtraIdx extra = static_cast<ExtraIdx>(n.rhs);
	uint32_t thenCount = ns.getExtra(extra);
	uint32_t elseCount = ns.getExtra(extra + 1);

	JamValueRef CondV = codegenNode(ctx, condIdx);
	if (!CondV) return nullptr;
	JamTypeRef condType = JamLLVMTypeOf(CondV);
	CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, CondV,
	                         JamLLVMConstInt(condType, 0, false), "ifcond");

	JamBasicBlockRef CurrentBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(CurrentBB);
	JamBasicBlockRef ThenBB = JamLLVMAppendBasicBlock(TheFunction, "then");
	JamBasicBlockRef ElseBB = JamLLVMAppendBasicBlock(TheFunction, "else");
	JamBasicBlockRef MergeBB = JamLLVMAppendBasicBlock(TheFunction, "ifcont");

	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, ThenBB, ElseBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), ThenBB);
	ctx.pushDropScope();
	for (uint32_t i = 0; i < thenCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 2 + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		emitTopScopeDrops(ctx);
		JamLLVMBuildBr(ctx.getBuilder(), MergeBB);
	}
	ctx.popDropScope();

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), ElseBB);
	ctx.pushDropScope();
	for (uint32_t i = 0; i < elseCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 2 + thenCount + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		emitTopScopeDrops(ctx);
		JamLLVMBuildBr(ctx.getBuilder(), MergeBB);
	}
	ctx.popDropScope();

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), MergeBB);
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

static JamValueRef codegenWhile(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	NodeIdx condIdx = static_cast<NodeIdx>(n.lhs);
	ExtraIdx extra = static_cast<ExtraIdx>(n.rhs);
	uint32_t bodyCount = ns.getExtra(extra);

	JamBasicBlockRef CurrentBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(CurrentBB);
	JamBasicBlockRef CondBB =
	    JamLLVMAppendBasicBlock(TheFunction, "whilecond");
	JamBasicBlockRef LoopBB =
	    JamLLVMAppendBasicBlock(TheFunction, "whileloop");
	JamBasicBlockRef AfterBB =
	    JamLLVMAppendBasicBlock(TheFunction, "afterloop");

	JamBasicBlockRef PrevContinue = CurrentLoopContinue;
	JamBasicBlockRef PrevBreak = CurrentLoopBreak;
	std::size_t PrevLoopScope = CurrentLoopBodyScopeIdx;
	CurrentLoopContinue = CondBB;
	CurrentLoopBreak = AfterBB;

	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), CondBB);
	JamValueRef CondV = codegenNode(ctx, condIdx);
	if (!CondV) {
		CurrentLoopContinue = PrevContinue;
		CurrentLoopBreak = PrevBreak;
		CurrentLoopBodyScopeIdx = PrevLoopScope;
		return nullptr;
	}
	JamTypeRef condType = JamLLVMTypeOf(CondV);
	CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, CondV,
	                         JamLLVMConstInt(condType, 0, false), "whilecond");
	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, LoopBB, AfterBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), LoopBB);
	// P8.4: the loop body's scope is the next one to be pushed.
	CurrentLoopBodyScopeIdx = ctx.getDropScopes().size();
	ctx.pushDropScope();
	for (uint32_t i = 0; i < bodyCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 1 + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		emitTopScopeDrops(ctx);
		JamLLVMBuildBr(ctx.getBuilder(), CondBB);
	}
	ctx.popDropScope();

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), AfterBB);
	CurrentLoopContinue = PrevContinue;
	CurrentLoopBreak = PrevBreak;
	CurrentLoopBodyScopeIdx = PrevLoopScope;
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

static JamValueRef codegenFor(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	const StringPool &sp = ctx.getStringPool();
	ExtraIdx extra = static_cast<ExtraIdx>(n.lhs);
	StringIdx varNameId = static_cast<StringIdx>(ns.getExtra(extra));
	NodeIdx startIdx = static_cast<NodeIdx>(ns.getExtra(extra + 1));
	NodeIdx endIdx = static_cast<NodeIdx>(ns.getExtra(extra + 2));
	uint32_t bodyCount = ns.getExtra(extra + 3);
	const std::string &varName = sp.get(varNameId);

	JamBasicBlockRef CurrentBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(CurrentBB);

	JamValueRef StartVal = codegenNode(ctx, startIdx);
	JamValueRef EndVal = codegenNode(ctx, endIdx);
	if (!StartVal || !EndVal) return nullptr;

	JamTypeRef VarType = JamLLVMTypeOf(StartVal);
	JamTypeRef endType = JamLLVMTypeOf(EndVal);
	if (endType != VarType) {
		if (JamLLVMTypeIsInteger(VarType) && JamLLVMTypeIsInteger(endType)) {
			EndVal = JamLLVMBuildIntCast(ctx.getBuilder(), EndVal, VarType,
			                             true, "endcast");
		} else {
			throw std::runtime_error("Type mismatch in for loop range");
		}
	}

	// For-loop induction variable is always an integer here; alignment is
	// the integer's byte width.
	uint64_t loopVarAlign = JamLLVMGetIntTypeWidth(VarType) / 8;
	JamValueRef Alloca = JamLLVMBuildAlloca(ctx.getBuilder(), VarType,
	                                        loopVarAlign, varName.c_str());
	JamLLVMBuildStore(ctx.getBuilder(), StartVal, Alloca);

	JamValueRef OldVal = ctx.getVariable(varName);
	ctx.setVariable(varName, Alloca);
	// Record a source-level TypeIdx for the loop variable so subsequent
	// reads via codegenNode(Variable) can recover the LLVM load type.
	// The variable's type is whatever integer the range bounds inferred
	// to; treat it as unsigned by default (range syntax is `0:N`).
	if (JamLLVMTypeIsInteger(VarType)) {
		ctx.setVariableType(
		    varName,
		    ctx.getTypePool().internInt(
		        static_cast<uint16_t>(JamLLVMGetIntTypeWidth(VarType)), false));
	}

	JamBasicBlockRef CondBB = JamLLVMAppendBasicBlock(TheFunction, "forcond");
	JamBasicBlockRef LoopBB = JamLLVMAppendBasicBlock(TheFunction, "forloop");
	JamBasicBlockRef IncrBB = JamLLVMAppendBasicBlock(TheFunction, "forincr");
	JamBasicBlockRef AfterBB =
	    JamLLVMAppendBasicBlock(TheFunction, "afterloop");

	JamBasicBlockRef PrevContinue = CurrentLoopContinue;
	JamBasicBlockRef PrevBreak = CurrentLoopBreak;
	std::size_t PrevLoopScope = CurrentLoopBodyScopeIdx;
	CurrentLoopContinue = IncrBB;
	CurrentLoopBreak = AfterBB;

	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), CondBB);
	JamValueRef CurVar = JamLLVMBuildLoad(ctx.getBuilder(), VarType, Alloca,
	                                      varName.c_str());
	JamValueRef CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_SLT,
	                                     CurVar, EndVal, "forcond");
	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, LoopBB, AfterBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), LoopBB);
	CurrentLoopBodyScopeIdx = ctx.getDropScopes().size();
	ctx.pushDropScope();
	for (uint32_t i = 0; i < bodyCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 4 + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		emitTopScopeDrops(ctx);
		JamLLVMBuildBr(ctx.getBuilder(), IncrBB);
	}
	ctx.popDropScope();

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), IncrBB);
	JamValueRef CurVarForInc = JamLLVMBuildLoad(ctx.getBuilder(), VarType,
	                                            Alloca, varName.c_str());
	JamValueRef StepVal = JamLLVMConstInt(VarType, 1, false);
	JamValueRef NextVar = JamLLVMBuildAdd(ctx.getBuilder(), CurVarForInc,
	                                      StepVal, "nextvar");
	JamLLVMBuildStore(ctx.getBuilder(), NextVar, Alloca);
	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), AfterBB);

	if (OldVal) ctx.setVariable(varName, OldVal);

	CurrentLoopContinue = PrevContinue;
	CurrentLoopBreak = PrevBreak;
	CurrentLoopBodyScopeIdx = PrevLoopScope;
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

// ---------------------------------------------------------------------------
// `match` codegen — M1 (integer literals, inclusive ranges, or-patterns,
// wildcard, `else`).
//
// Strategy: a sequential icmp-cascade. For each arm, build a boolean
// expression representing "this pattern matches the scrutinee", branch to
// the arm body if true and to the next arm's test (or `else` / fallthrough)
// if false. LLVM's downstream simplifycfg pass collapses chains of equality
// tests on the same value into a `switch` instruction when profitable, so
// the pure-literal opcode-dispatch case still gets jump-table codegen
// without us emitting `switch` ourselves.
//
// The Maranget decision-tree formalism degenerates to "specialize the only
// column" in M1 (one scrutinee, one column); the cascade is the canonical
// one-column lowering. M2+ replaces this with a multi-column tree.
// ---------------------------------------------------------------------------

// Build a boolean (i1) value that is true iff the scrutinee matches the
// given pattern node. `scrut` is the already-loaded scrutinee value;
// `scrutType` is its LLVM type, used to materialize comparison constants.
static JamValueRef
emitPatternTest(JamCodegenContext &ctx, NodeIdx patIdx, JamValueRef scrut,
                JamTypeRef scrutType) {
	const NodeStore &ns = ctx.getNodeStore();
	const AstNode &pn = ns.get(patIdx);
	switch (pn.tag) {
	case AstTag::PatWildcard:
		return JamLLVMConstInt(ctx.getInt1Type(), 1, false);
	case AstTag::PatLit: {
		uint64_t val = static_cast<uint64_t>(pn.lhs) |
		               (static_cast<uint64_t>(pn.rhs) << 32);
		bool isNeg = (pn.flags & 1) != 0;
		uint64_t materialized = isNeg
		                            ? static_cast<uint64_t>(
		                                  -static_cast<int64_t>(val))
		                            : val;
		JamValueRef k =
		    JamLLVMConstInt(scrutType, materialized, isNeg);
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_EQ, scrut, k,
		                        "pat.eq");
	}
	case AstTag::PatRange: {
		JamValueRef lo = JamLLVMConstInt(scrutType, pn.lhs, false);
		JamValueRef hi = JamLLVMConstInt(scrutType, pn.rhs, false);
		// Unsigned bounds test: scrut >= lo && scrut <= hi.
		JamValueRef geLo = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_UGE,
		                                    scrut, lo, "pat.ge");
		JamValueRef leHi = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_ULE,
		                                    scrut, hi, "pat.le");
		return JamLLVMBuildAnd(ctx.getBuilder(), geLo, leHi, "pat.range");
	}
	case AstTag::PatOr: {
		ExtraIdx extra = static_cast<ExtraIdx>(pn.lhs);
		uint32_t count = ns.getExtra(extra);
		JamValueRef acc = nullptr;
		for (uint32_t i = 0; i < count; i++) {
			NodeIdx sub =
			    static_cast<NodeIdx>(ns.getExtra(extra + 1 + i));
			JamValueRef one = emitPatternTest(ctx, sub, scrut, scrutType);
			if (!acc) {
				acc = one;
			} else {
				acc = JamLLVMBuildOr(ctx.getBuilder(), acc, one, "pat.or");
			}
		}
		return acc ? acc
		           : JamLLVMConstInt(ctx.getInt1Type(), 0, false);
	}
	case AstTag::PatEnumVariant: {
		// Two encodings:
		//   no bindings : lhs = enumNameId, rhs = variantNameId, flags=0
		//   bindings    : lhs = ExtraIdx → [enumNameId, variantNameId,
		//                                   count, name0, name1, …]
		//                 flags bit 0 = 1
		const StringPool &sp = ctx.getStringPool();
		StringIdx enumNameId, variantNameId;
		if ((pn.flags & 1) == 0) {
			enumNameId = static_cast<StringIdx>(pn.lhs);
			variantNameId = static_cast<StringIdx>(pn.rhs);
		} else {
			ExtraIdx ex = static_cast<ExtraIdx>(pn.lhs);
			enumNameId = static_cast<StringIdx>(ns.getExtra(ex));
			variantNameId = static_cast<StringIdx>(ns.getExtra(ex + 1));
		}
		const std::string &enumName = sp.get(enumNameId);
		const std::string &variantName = sp.get(variantNameId);
		const auto *einfo = ctx.getEnum(enumName);
		int idx = ctx.getEnumVariantIndex(enumName, variantName);
		if (!einfo || idx < 0) {
			throw std::runtime_error("Enum `" + enumName +
			                         "` has no variant `" + variantName +
			                         "` (in match pattern)");
		}
		uint32_t discrim = einfo->variants[idx].discriminant;
		// Extract the tag if the scrutinee is a {tag, payload} struct.
		// For E1 (unit-only) enums the scrutinee is already i8.
		JamValueRef tagVal = scrut;
		JamTypeRef tagType = scrutType;
		if (JamLLVMTypeIsStruct(scrutType)) {
			tagVal = JamLLVMBuildExtractValue(ctx.getBuilder(), scrut, 0,
			                                   "pat.tag");
			tagType = ctx.getInt8Type();
		}
		JamValueRef k = JamLLVMConstInt(
		    tagType, static_cast<uint64_t>(discrim), false);
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_EQ, tagVal, k,
		                        "pat.variant");
	}
	default:
		throw std::runtime_error(
		    "Unsupported pattern node kind in M1 codegen");
	}
}

static JamValueRef codegenMatch(JamCodegenContext &ctx, const AstNode &n) {
	const NodeStore &ns = ctx.getNodeStore();
	NodeIdx scrutIdx = static_cast<NodeIdx>(n.lhs);
	ExtraIdx extra = static_cast<ExtraIdx>(n.rhs);

	uint32_t armCount = ns.getExtra(extra);
	uint32_t elseBodyCount = ns.getExtra(extra + 1);

	// Compute the offset just past the else body — that's where arm
	// records begin.
	uint32_t armsOff = 2 + elseBodyCount;

	// E3 — compile-time exhaustiveness + reachability over enum
	// variants. We scan all arm patterns once, accumulating which
	// variants are covered and detecting duplicates (an arm that
	// would never run because an earlier arm already covers it).
	// Exhaustiveness fires only when there is no `else` arm and no
	// catch-all `_`; duplicate detection runs in either case.
	{
		std::string foundEnum;
		std::vector<std::string> coveredVariants;
		bool sawCatchAll = false;
		// Recursive walk over a pattern node. Side effects: appends
		// variant names to `coveredVariants` for enum patterns whose
		// enum matches the (first-seen) `foundEnum`; sets
		// `sawCatchAll` if a wildcard makes the match exhaustive.
		// Return value: whether the pattern provably catches its
		// position (wildcard, or or-pattern whose any sub is a
		// catch-all).
		std::function<bool(NodeIdx)> walkPattern;
		walkPattern = [&](NodeIdx p) -> bool {
			const AstNode &pp = ns.get(p);
			switch (pp.tag) {
			case AstTag::PatWildcard:
				return true;
			case AstTag::PatEnumVariant: {
				StringIdx enumNameId, variantNameId;
				if ((pp.flags & 1) == 0) {
					enumNameId = static_cast<StringIdx>(pp.lhs);
					variantNameId = static_cast<StringIdx>(pp.rhs);
				} else {
					ExtraIdx ex = static_cast<ExtraIdx>(pp.lhs);
					enumNameId =
					    static_cast<StringIdx>(ns.getExtra(ex));
					variantNameId =
					    static_cast<StringIdx>(ns.getExtra(ex + 1));
				}
				const std::string &en =
				    ctx.getStringPool().get(enumNameId);
				const std::string &vn =
				    ctx.getStringPool().get(variantNameId);
				if (foundEnum.empty()) foundEnum = en;
				if (foundEnum == en) coveredVariants.push_back(vn);
				return false;
			}
			case AstTag::PatOr: {
				ExtraIdx ex = static_cast<ExtraIdx>(pp.lhs);
				uint32_t cnt = ns.getExtra(ex);
				bool catchAll = false;
				for (uint32_t i = 0; i < cnt; i++) {
					NodeIdx sub =
					    static_cast<NodeIdx>(ns.getExtra(ex + 1 + i));
					if (walkPattern(sub)) catchAll = true;
				}
				return catchAll;
			}
			default:
				// Literal / range / anything else — doesn't catch
				// everything; exhaustiveness must come from elsewhere.
				return false;
			}
		};
		uint32_t scanPos = armsOff;
		for (uint32_t i = 0; i < armCount; i++) {
			NodeIdx patIdx =
			    static_cast<NodeIdx>(ns.getExtra(extra + scanPos));
			uint32_t bodyCount = ns.getExtra(extra + scanPos + 1);
			if (walkPattern(patIdx)) sawCatchAll = true;
			scanPos = scanPos + 2 + bodyCount;
		}

		// Reachability: duplicate enum-variant pattern across arms is
		// always unreachable (the earlier arm consumes the value).
		// Runs regardless of `else` presence — `else` doesn't excuse
		// dead code.
		for (size_t i = 0; i < coveredVariants.size(); i++) {
			for (size_t j = i + 1; j < coveredVariants.size(); j++) {
				if (coveredVariants[i] == coveredVariants[j]) {
					throw std::runtime_error(
					    "Unreachable match arm: variant `" + foundEnum +
					    "." + coveredVariants[j] +
					    "` is already covered by an earlier arm");
				}
			}
		}

		// Exhaustiveness: only enforced when no `else` arm and no
		// catch-all (`_` somewhere) is present.
		if (elseBodyCount == 0 && !foundEnum.empty() && !sawCatchAll) {
			const auto *einfo = ctx.getEnum(foundEnum);
			if (einfo) {
				std::vector<std::string> missing;
				for (const auto &v : einfo->variants) {
					bool covered = false;
					for (const auto &c : coveredVariants) {
						if (c == v.name) { covered = true; break; }
					}
					if (!covered) missing.push_back(v.name);
				}
				if (!missing.empty()) {
					std::string msg =
					    "Non-exhaustive match on enum `" + foundEnum +
					    "`; missing variant(s): ";
					for (size_t i = 0; i < missing.size(); i++) {
						if (i > 0) msg += ", ";
						msg += missing[i];
					}
					throw std::runtime_error(msg);
				}
			}
		}
	}

	// Evaluate the scrutinee once. The value is reused for every pattern
	// test below; LLVM mem2reg promotes the local to an SSA value for
	// free. We also alloca a slot holding the scrutinee so payload
	// bindings (E2) can GEP into the {tag, payload} struct.
	JamValueRef scrut = codegenNode(ctx, scrutIdx);
	if (!scrut) return nullptr;
	JamTypeRef scrutType = JamLLVMTypeOf(scrut);
	// Classify the scrutinee. Enum scrutinees may be either an integer
	// (E1 unit-only enums lower to i8) or a struct (E2 payloaded enums
	// lower to a named struct). For struct scrutinees we MUST verify
	// the type matches a registered enum, otherwise a slice (which is
	// also a struct shape `{ptr, len}`) would silently route through
	// the tag-extraction path and produce nonsense.
	bool scrutIsEnum = false;
	if (JamLLVMTypeIsStruct(scrutType)) {
		if (ctx.findEnumByLLVMType(scrutType) != nullptr) {
			scrutIsEnum = true;
		} else {
			throw std::runtime_error(
			    "`match` on a struct-shaped value is only supported when "
			    "the value is an enum; got a different struct type");
		}
	} else if (!JamLLVMTypeIsInteger(scrutType)) {
		throw std::runtime_error(
		    "`match` only supports integer or enum scrutinees");
	}
	JamValueRef scrutPtr = nullptr;
	if (scrutIsEnum) {
		const auto *einfo = ctx.findEnumByLLVMType(scrutType);
		uint64_t scrutAlign =
		    einfo && einfo->maxPayloadAlign > 1 ? einfo->maxPayloadAlign : 1;
		scrutPtr = JamLLVMBuildAlloca(ctx.getBuilder(), scrutType,
		                              scrutAlign, "match.scrut");
		JamLLVMBuildStore(ctx.getBuilder(), scrut, scrutPtr);
	}

	JamBasicBlockRef curBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef func = JamLLVMGetBasicBlockParent(curBB);
	JamBasicBlockRef mergeBB =
	    JamLLVMAppendBasicBlock(func, "match.end");

	// Per-arm result for the M3 phi: pairs (basicBlock, value) where
	// the arm's body ended without terminating control flow. The
	// match's expression-form value is a phi over these.
	std::vector<std::pair<JamBasicBlockRef, JamValueRef>> armResults;

	// Pre-create a "test block" for every arm beyond the first, plus a
	// "fall block" for the else arm (or merge if no else). This lets each
	// arm body branch back to merge cleanly without lookahead.
	std::vector<JamBasicBlockRef> testBBs;
	testBBs.reserve(armCount);
	std::vector<JamBasicBlockRef> bodyBBs;
	bodyBBs.reserve(armCount);
	for (uint32_t i = 0; i < armCount; i++) {
		if (i == 0) {
			testBBs.push_back(curBB);
		} else {
			testBBs.push_back(
			    JamLLVMAppendBasicBlock(func, "match.test"));
		}
		bodyBBs.push_back(JamLLVMAppendBasicBlock(func, "match.arm"));
	}
	// Where do we go if every arm fails? Else block if present, else merge.
	JamBasicBlockRef fallBB = mergeBB;
	if (elseBodyCount > 0) {
		fallBB = JamLLVMAppendBasicBlock(func, "match.else");
	}

	// Emit each arm's pattern test in its dedicated test block.
	uint32_t pos = armsOff;
	for (uint32_t i = 0; i < armCount; i++) {
		NodeIdx patIdx = static_cast<NodeIdx>(ns.getExtra(extra + pos));
		uint32_t bodyCount = ns.getExtra(extra + pos + 1);
		uint32_t bodyStart = pos + 2;

		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), testBBs[i]);
		JamValueRef matched = emitPatternTest(ctx, patIdx, scrut, scrutType);
		JamBasicBlockRef nextTest =
		    (i + 1 < armCount) ? testBBs[i + 1] : fallBB;
		JamLLVMBuildCondBr(ctx.getBuilder(), matched, bodyBBs[i],
		                   nextTest);

		// Emit the arm body.
		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), bodyBBs[i]);

		// Materialize payload bindings if the pattern carries any.
		// Pattern shape:
		//   PatEnumVariant with flags & 1: lhs = ExtraIdx
		//   ExtraIdx → [enumNameId, variantNameId, count, n0, n1, ...]
		const AstNode &pn = ns.get(patIdx);
		if (scrutIsEnum && pn.tag == AstTag::PatEnumVariant &&
		    (pn.flags & 1) != 0) {
			ExtraIdx ex = static_cast<ExtraIdx>(pn.lhs);
			StringIdx enumNameId =
			    static_cast<StringIdx>(ns.getExtra(ex));
			StringIdx variantNameId =
			    static_cast<StringIdx>(ns.getExtra(ex + 1));
			uint32_t bcount = ns.getExtra(ex + 2);
			const std::string &enumName =
			    ctx.getStringPool().get(enumNameId);
			const std::string &variantName =
			    ctx.getStringPool().get(variantNameId);
			const auto *einfo = ctx.getEnum(enumName);
			int vidx = ctx.getEnumVariantIndex(enumName, variantName);
			if (!einfo || vidx < 0) {
				throw std::runtime_error(
				    "Pattern references unknown variant `" + enumName +
				    "." + variantName + "`");
			}
			const auto &v = einfo->variants[vidx];
			if (bcount != v.payloadTypes.size()) {
				throw std::runtime_error(
				    "Pattern for `" + enumName + "." + variantName +
				    "` binds " + std::to_string(bcount) +
				    " payload field(s); variant has " +
				    std::to_string(v.payloadTypes.size()));
			}
			// Payload area always starts at struct field 1 (the
			// alignment-driving scalar; larger payloads spill into
			// field 2 via byte-offset GEP from there).
			unsigned payloadFieldIdx = 1;
			JamValueRef payloadAreaPtr = JamLLVMBuildStructGEP(
			    ctx.getBuilder(), scrutType, scrutPtr,
			    payloadFieldIdx, "match.payload");
			(void)einfo;
			uint64_t off = 0;
			for (uint32_t b = 0; b < bcount; b++) {
				StringIdx nameId =
				    static_cast<StringIdx>(ns.getExtra(ex + 3 + b));
				const std::string &name =
				    ctx.getStringPool().get(nameId);
				TypeIdx ty = v.payloadTypes[b];
				uint64_t s = ctx.typeSize(ty);
				uint64_t a = ctx.typeAlign(ty);
				off = (off + a - 1) / a * a;
				JamValueRef i64Off = JamLLVMConstInt(
				    ctx.getInt64Type(), off, false);
				JamValueRef fieldPtr = JamLLVMBuildPtrGEP(
				    ctx.getBuilder(), ctx.getInt8Type(),
				    payloadAreaPtr, i64Off, "match.field.ptr");
				JamTypeRef fieldLLVM = ctx.getLLVMType(ty);
				JamValueRef fieldVal = JamLLVMBuildLoad(
				    ctx.getBuilder(), fieldLLVM, fieldPtr, name.c_str());
				JamValueRef bindAlloca = JamLLVMBuildAlloca(
				    ctx.getBuilder(), fieldLLVM, ctx.typeAlign(ty),
				    name.c_str());
				JamLLVMBuildStore(ctx.getBuilder(), fieldVal, bindAlloca);
				ctx.setVariable(name, bindAlloca);
				ctx.setVariableType(name, ty);
				off += s;
			}
		}

		// Track each arm's last produced value (for match-as-expression
		// support, M3). Arms that terminate via `return` / `break` /
		// `continue` don't fall through to merge and aren't recorded.
		JamValueRef lastValue = nullptr;
		ctx.pushDropScope();
		for (uint32_t b = 0; b < bodyCount; b++) {
			lastValue = codegenNode(ctx,
			                         ns.getExtra(extra + bodyStart + b));
		}
		JamBasicBlockRef armEndBB =
		    JamLLVMGetInsertBlock(ctx.getBuilder());
		if (!JamLLVMGetBasicBlockTerminator(armEndBB)) {
			emitTopScopeDrops(ctx);
			armResults.push_back({armEndBB, lastValue});
			JamLLVMBuildBr(ctx.getBuilder(), mergeBB);
		}
		ctx.popDropScope();

		pos = bodyStart + bodyCount;
	}

	// Else block — runs if no arm matched.
	if (elseBodyCount > 0) {
		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), fallBB);
		JamValueRef lastValue = nullptr;
		ctx.pushDropScope();
		for (uint32_t b = 0; b < elseBodyCount; b++) {
			lastValue = codegenNode(ctx, ns.getExtra(extra + 2 + b));
		}
		JamBasicBlockRef elseEndBB =
		    JamLLVMGetInsertBlock(ctx.getBuilder());
		if (!JamLLVMGetBasicBlockTerminator(elseEndBB)) {
			emitTopScopeDrops(ctx);
			armResults.push_back({elseEndBB, lastValue});
			JamLLVMBuildBr(ctx.getBuilder(), mergeBB);
		}
		ctx.popDropScope();
	}

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), mergeBB);

	// Build a phi over arm-produced values when all arms agree on a
	// non-void type. This is the value of the `match` expression. If
	// no arms reached the merge block (every arm terminated), return
	// a sentinel (caller is statement-form and won't use the value).
	if (armResults.empty()) {
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	}
	// Filter out null values (empty bodies) and replace with sentinel
	// of the unified type. Use the first non-null value's type as the
	// phi type.
	JamTypeRef phiType = nullptr;
	for (auto &r : armResults) {
		if (r.second && JamLLVMTypeOf(r.second)) {
			phiType = JamLLVMTypeOf(r.second);
			break;
		}
	}
	if (!phiType) {
		// Every arm body was empty — match has no meaningful value.
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	}
	JamValueRef phi =
	    JamLLVMBuildPhi(ctx.getBuilder(), phiType, "match.result");
	std::vector<JamValueRef> phiVals;
	std::vector<JamBasicBlockRef> phiBlocks;
	for (auto &r : armResults) {
		JamValueRef v = r.second;
		if (!v || JamLLVMTypeOf(v) != phiType) {
			// Empty body or type mismatch: substitute a typed zero so
			// the phi remains well-formed. Mismatched arm-types in an
			// expression-form match are a user bug; the resulting
			// value is unspecified rather than blowing up the build.
			v = JamLLVMConstInt(phiType, 0, false);
		}
		phiVals.push_back(v);
		phiBlocks.push_back(r.first);
	}
	JamLLVMAddIncoming(phi, phiVals.data(), phiBlocks.data(),
	                   static_cast<unsigned>(phiVals.size()));
	return phi;
}

static JamValueRef codegenStructLit(JamCodegenContext &ctx, const AstNode &n) {
	TypeIdx structType = static_cast<TypeIdx>(n.lhs);
	if (structType == kNoType) {
		throw std::runtime_error(
		    "Struct literal used without a known target type");
	}
	const auto *info = ctx.lookupStruct(structType);
	if (!info) {
		throw std::runtime_error("Struct literal target is not a struct");
	}

	const NodeStore &ns = ctx.getNodeStore();
	const StringPool &sp = ctx.getStringPool();
	ExtraIdx extra = static_cast<ExtraIdx>(n.rhs);
	uint32_t fieldCount = ns.getExtra(extra);

	JamValueRef structVal = JamLLVMGetUndef(info->type);
	for (uint32_t i = 0; i < fieldCount; i++) {
		StringIdx fldNameId = static_cast<StringIdx>(
		    ns.getExtra(extra + 1 + i * 2));
		NodeIdx fldExprIdx = static_cast<NodeIdx>(
		    ns.getExtra(extra + 2 + i * 2));
		const std::string &fieldName = sp.get(fldNameId);
		int idx = ctx.getFieldIndex(info->name, fieldName);
		if (idx < 0) {
			throw std::runtime_error("Unknown field '" + fieldName +
			                         "' in struct " + info->name);
		}

		// Propagate target struct type into nested struct literals.
		TypeIdx declaredFieldType = info->fields[idx].second;
		const AstNode &fldNode = ns.get(fldExprIdx);
		if (fldNode.tag == AstTag::StructLit &&
		    ctx.lookupStruct(declaredFieldType)) {
			ctx.getNodeStore().getMut(fldExprIdx).lhs = declaredFieldType;
		}

		JamTypeRef expectedType = ctx.getLLVMType(declaredFieldType);
		JamValueRef fieldVal = codegenNode(ctx, fldExprIdx, expectedType);
		if (!fieldVal) return nullptr;

		JamTypeRef actualType = JamLLVMTypeOf(fieldVal);
		if (actualType != expectedType) {
			if (JamLLVMTypeIsFloat(expectedType) &&
			    JamLLVMTypeIsInteger(actualType)) {
				fieldVal = JamLLVMBuildSIToFP(ctx.getBuilder(), fieldVal,
				                              expectedType, "fld_si2fp");
			} else if (JamLLVMTypeIsFloat(expectedType) &&
			           JamLLVMTypeIsFloat(actualType)) {
				fieldVal = JamLLVMBuildFPCast(ctx.getBuilder(), fieldVal,
				                              expectedType, "fld_fpcast");
			} else if (JamLLVMTypeIsInteger(expectedType) &&
			           JamLLVMTypeIsInteger(actualType)) {
				fieldVal = JamLLVMBuildIntCast(ctx.getBuilder(), fieldVal,
				                               expectedType, false,
				                               "fld_icast");
			}
		}

		structVal = JamLLVMBuildInsertValue(ctx.getBuilder(), structVal,
		                                    fieldVal,
		                                    static_cast<unsigned>(idx),
		                                    "fld_set");
	}
	return structVal;
}

static JamValueRef codegenMemberAccess(JamCodegenContext &ctx,
                                       const AstNode &n, NodeIdx selfIdx) {
	const NodeStore &ns = ctx.getNodeStore();
	const StringPool &sp = ctx.getStringPool();

	std::vector<StringIdx> path;
	StringIdx rootName = collectMemberChain(ns, selfIdx, path);
	if (rootName == kNoString) {
		throw std::runtime_error(
		    "Direct member access codegen not yet implemented");
	}

	// Enum-variant value: `EnumName.Variant` (no parens) resolves to
	// the discriminant for unit variants. For E2 enums (those with at
	// least one payloaded variant), the result is a fully-formed enum
	// struct with tag set and payload bytes undefined — equivalent to
	// `EnumName.Variant()` with no payload args.
	if (path.size() == 1) {
		const std::string &enumName = sp.get(rootName);
		const std::string &variantName = sp.get(path[0]);
		if (const auto *einfo = ctx.getEnum(enumName)) {
			int idx = ctx.getEnumVariantIndex(enumName, variantName);
			if (idx < 0) {
				throw std::runtime_error("Enum `" + enumName +
				                         "` has no variant `" + variantName +
				                         "`");
			}
			const auto &v = einfo->variants[idx];
			JamValueRef tagConst = JamLLVMConstInt(
			    ctx.getInt8Type(),
			    static_cast<uint64_t>(v.discriminant), false);
			if (!einfo->hasPayloadVariant) {
				// E1 path: enum is just i8.
				return tagConst;
			}
			if (!v.payloadTypes.empty()) {
				throw std::runtime_error(
				    "Variant `" + enumName + "." + variantName +
				    "` carries a payload; use `" + enumName + "." +
				    variantName + "(...)` to construct it");
			}
			// E2 unit variant: build a {tag, payload-undef} struct value.
			JamTypeRef enumLLVMType = einfo->type;
			uint64_t enumAlign =
			    einfo->maxPayloadAlign > 1 ? einfo->maxPayloadAlign : 1;
			JamValueRef alloca = JamLLVMBuildAlloca(
			    ctx.getBuilder(), enumLLVMType, enumAlign, "enum.unit");
			JamValueRef tagPtr = JamLLVMBuildStructGEP(
			    ctx.getBuilder(), enumLLVMType, alloca, 0, "enum.tag");
			JamLLVMBuildStore(ctx.getBuilder(), tagConst, tagPtr);
			return JamLLVMBuildLoad(ctx.getBuilder(), enumLLVMType,
			                        alloca, "enum.val");
		}
	}

	if (!ctx.hasVariable(sp.get(rootName))) {
		throw std::runtime_error(
		    "Direct member access codegen not yet implemented");
	}

	const std::string &varName = sp.get(rootName);
	TypeIdx varTy = ctx.getVariableType(varName);
	const TypeKey &varKey = ctx.getTypePool().get(varTy);

	// `slice.ptr` / `slice.len` projection (single-level).
	if (varKey.kind == TypeKind::Slice && path.size() == 1) {
		const std::string &member = sp.get(path[0]);
		if (member != "ptr" && member != "len") {
			throw std::runtime_error("Slice has no field '" + member +
			                         "' (only .ptr and .len)");
		}
		JamValueRef alloca = ctx.getVariable(varName);
		JamTypeRef sliceType = ctx.getLLVMType(varTy);
		JamValueRef sliceVal = JamLLVMBuildLoad(ctx.getBuilder(), sliceType,
		                                        alloca, varName.c_str());
		unsigned fieldIdx = (member == "ptr") ? 0 : 1;
		return JamLLVMBuildExtractValue(ctx.getBuilder(), sliceVal, fieldIdx,
		                                member.c_str());
	}

	// Union member read (single-level only in M1). All fields share the
	// same address; reading uses the field's type at the union's
	// allocation. Reading a different field than the most recently
	// written one reinterprets the bits — that's the union's whole job.
	//
	// The parser interns user-named types as TypeKind::Named without
	// distinguishing struct from union, so dispatch on the registry
	// (lookupUnion accepts either Struct or Union kinds).
	if (path.size() == 1) {
		if (const auto *uinfo = ctx.lookupUnion(varTy)) {
			const std::string &member = sp.get(path[0]);
			TypeIdx fieldTy =
			    ctx.getUnionFieldType(uinfo->name, member);
			if (fieldTy == kNoType) {
				throw std::runtime_error("Union `" + uinfo->name +
				                         "` has no field `" + member +
				                         "`");
			}
			JamValueRef alloca = ctx.getVariable(varName);
			JamTypeRef fieldLLVMType = ctx.getLLVMType(fieldTy);
			// Opaque-pointer LLVM: the alloca is just `ptr`; we load
			// the requested field type from it directly. No bitcast
			// required.
			return JamLLVMBuildLoad(ctx.getBuilder(), fieldLLVMType,
			                        alloca, member.c_str());
		}
	}

	const auto *info = ctx.lookupStruct(varTy);
	if (!info) {
		throw std::runtime_error(
		    "Direct member access codegen not yet implemented");
	}

	// Walk the field chain with struct GEPs and load only the leaf field —
	// the previous code loaded the whole struct and walked it with
	// extractvalue, which is N×fieldSize of pointless memory traffic in
	// debug builds and reads poison from any uninitialized fields.
	JamValueRef alloca = ctx.getVariable(varName);
	JamValueRef leafPtr = alloca;
	JamTypeRef leafLLVMType = info->type;
	TypeIdx currentType = varTy;
	for (StringIdx fldId : path) {
		const auto *curInfo = ctx.lookupStruct(currentType);
		if (!curInfo) {
			throw std::runtime_error("Cannot access field '" + sp.get(fldId) +
			                         "' on non-struct type");
		}
		const std::string &fieldName = sp.get(fldId);
		int idx = ctx.getFieldIndex(curInfo->name, fieldName);
		if (idx < 0) {
			throw std::runtime_error("Unknown field '" + fieldName +
			                         "' in struct " + curInfo->name);
		}
		leafPtr = JamLLVMBuildStructGEP(ctx.getBuilder(), leafLLVMType,
		                                leafPtr, static_cast<unsigned>(idx),
		                                fieldName.c_str());
		currentType = curInfo->fields[idx].second;
		leafLLVMType = ctx.getLLVMType(currentType);
	}
	return JamLLVMBuildLoad(ctx.getBuilder(), leafLLVMType, leafPtr, "field");
	(void)n;
}

JamValueRef codegenNode(JamCodegenContext &ctx, NodeIdx node,
                        JamTypeRef expectedType) {
	const AstNode &n = ctx.getNodeStore().get(node);
	switch (n.tag) {
	case AstTag::Invalid:
		throw std::runtime_error("Codegen on invalid node");
	case AstTag::NumberLit:
		return numberLitConst(ctx, n, expectedType);
	case AstTag::BoolLit:
		return JamLLVMConstInt(ctx.getInt1Type(), n.lhs ? 1 : 0, false);
	case AstTag::StringLit:
		return codegenStringLit(
		    ctx, ctx.getStringPool().get(static_cast<StringIdx>(n.lhs)));
	case AstTag::UndefinedLit:
		throw std::runtime_error(
		    "`undefined` is only valid as a `var` declaration initializer");
	case AstTag::Variable: {
		const std::string &name =
		    ctx.getStringPool().get(static_cast<StringIdx>(n.lhs));
		JamValueRef V = ctx.getVariable(name);
		if (V) {
			JamTypeRef LoadType =
			    ctx.getLLVMType(ctx.getVariableType(name));
			return JamLLVMBuildLoad(ctx.getBuilder(), LoadType, V,
			                        name.c_str());
		}
		// Fall back to module-scope `const NAME[: T]? = expr;` — re-codegen
		// the init expression in place. With the declared type passed as
		// expectedType, integer literals adopt the declared width naturally.
		if (const auto *mc = ctx.getModuleConst(name)) {
			JamTypeRef expected = mc->declaredType != kNoType
			                          ? ctx.getLLVMType(mc->declaredType)
			                          : nullptr;
			return codegenNode(ctx, mc->initExpr, expected);
		}
		throw std::runtime_error("Unknown variable name: " + name);
	}
	case AstTag::MemberAccess:
		return codegenMemberAccess(ctx, n, node);
	case AstTag::Index:
		return codegenIndex(ctx, n);
	case AstTag::Deref:
		return codegenDeref(ctx, n);
	case AstTag::AddressOf:
		return codegenAddressOf(ctx, n);
	case AstTag::UnaryOp:
		return codegenUnaryOp(ctx, n);
	case AstTag::BinaryOp:
		return codegenBinaryOp(ctx, n);
	case AstTag::Call:
		return codegenCall(ctx, n);
	case AstTag::Return:
		return codegenReturn(ctx, n);
	case AstTag::Assign:
		return codegenAssign(ctx, n);
	case AstTag::VarDecl:
		return codegenVarDecl(ctx, n);
	case AstTag::IfNode:
		return codegenIf(ctx, n);
	case AstTag::WhileNode:
		return codegenWhile(ctx, n);
	case AstTag::ForNode:
		return codegenFor(ctx, n);
	case AstTag::Break:
		if (!CurrentLoopBreak) {
			throw std::runtime_error("break statement not inside a loop");
		}
		// P8.4: drop all scopes inside (and including) the enclosing
		// loop body before exiting the loop.
		emitDropsThroughScope(ctx, CurrentLoopBodyScopeIdx);
		JamLLVMBuildBr(ctx.getBuilder(), CurrentLoopBreak);
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	case AstTag::Continue:
		if (!CurrentLoopContinue) {
			throw std::runtime_error("continue statement not inside a loop");
		}
		// P8.4: drop all scopes inside (and including) the enclosing
		// loop body before jumping to the next iteration.
		emitDropsThroughScope(ctx, CurrentLoopBodyScopeIdx);
		JamLLVMBuildBr(ctx.getBuilder(), CurrentLoopContinue);
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	case AstTag::ImportLit:
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	case AstTag::StructLit:
		return codegenStructLit(ctx, n);
	case AstTag::MatchNode:
		return codegenMatch(ctx, n);
	case AstTag::AsCast: {
		// `expr as Type` — explicit conversion. Handles:
		//   • integer ↔ integer (truncate/extend)
		//   • integer ↔ float (siToFP / fpToSI)
		//   • float ↔ float (FPCast)
		//   • enum ↔ integer: extracts the tag for E2 enums (which
		//     are {tag, payload} structs); identity for E1 enums
		//     (which are already i8).
		NodeIdx operandIdx = static_cast<NodeIdx>(n.lhs);
		TypeIdx targetTy = static_cast<TypeIdx>(n.rhs);
		JamTypeRef targetLLVM = ctx.getLLVMType(targetTy);
		JamValueRef val = codegenNode(ctx, operandIdx);
		if (!val) return nullptr;
		JamTypeRef srcLLVM = JamLLVMTypeOf(val);
		if (srcLLVM == targetLLVM) return val;
		// Enum-to-integer: extract the tag (struct field 0) when the
		// source is a struct, then cast that i8 to the target width.
		if (JamLLVMTypeIsStruct(srcLLVM) &&
		    ctx.findEnumByLLVMType(srcLLVM) != nullptr &&
		    JamLLVMTypeIsInteger(targetLLVM)) {
			JamValueRef tag = JamLLVMBuildExtractValue(
			    ctx.getBuilder(), val, 0, "as.tag");
			if (JamLLVMTypeOf(tag) == targetLLVM) return tag;
			return JamLLVMBuildIntCast(ctx.getBuilder(), tag, targetLLVM,
			                           false, "as.tag.cast");
		}
		if (JamLLVMTypeIsInteger(srcLLVM) &&
		    JamLLVMTypeIsInteger(targetLLVM)) {
			return JamLLVMBuildIntCast(ctx.getBuilder(), val, targetLLVM,
			                           false, "as.icast");
		}
		if (JamLLVMTypeIsInteger(srcLLVM) &&
		    JamLLVMTypeIsFloat(targetLLVM)) {
			return JamLLVMBuildSIToFP(ctx.getBuilder(), val, targetLLVM,
			                           "as.si2fp");
		}
		if (JamLLVMTypeIsFloat(srcLLVM) &&
		    JamLLVMTypeIsInteger(targetLLVM)) {
			throw std::runtime_error(
			    "`as` from float to integer is not yet supported");
		}
		if (JamLLVMTypeIsFloat(srcLLVM) &&
		    JamLLVMTypeIsFloat(targetLLVM)) {
			return JamLLVMBuildFPCast(ctx.getBuilder(), val, targetLLVM,
			                           "as.fpcast");
		}
		throw std::runtime_error(
		    "Unsupported `as` cast between these types");
	}
	case AstTag::PatLit:
	case AstTag::PatRange:
	case AstTag::PatWildcard:
	case AstTag::PatOr:
	case AstTag::PatEnumVariant:
		throw std::runtime_error(
		    "Pattern node reached top-level codegen; patterns are only "
		    "valid inside a `match` arm");
	case AstTag::Count:
		break;
	}
	throw std::runtime_error("Unhandled AST tag in codegen");
	(void)expectedType;
}

JamValueRef resolveLvaluePtr(JamCodegenContext &ctx, NodeIdx node,
                             JamTypeRef &outElemType) {
	(void)ctx;
	(void)node;
	(void)outElemType;
	throw std::runtime_error("resolveLvaluePtr is not yet exposed");
}

// ---------------------------------------------------------------------------
// FunctionAST codegen — split into declarePrototype + defineBody so the
// driver can register every function's signature before it starts emitting
// any body. That's what lets `main` (or any caller) appear above its
// callees in source order.
// ---------------------------------------------------------------------------

// P8.2: name-mangling for `fn drop(self: mut T)`. When the source-level
// name "drop" coincides with a recognizable drop-fn signature, mangle to
// "__drop_<TypeName>" at the LLVM level so multiple types can each have
// their own drop fn without colliding. Used by declarePrototype,
// defineBody, and the drop-emission helper so all three see the same
// LLVM function name.
static std::string mangledFunctionName(const FunctionAST &fn,
                                       const TypePool &types,
                                       const StringPool &strings) {
	if (fn.isTest) return "__test_" + fn.Name;
	if (fn.Name == "drop" && fn.Args.size() == 1) {
		const Param &p = fn.Args[0];
		if (p.Name == "self" && p.Mode == ParamMode::Mut) {
			const TypeKey &k = types.get(p.Type);
			if (k.kind == TypeKind::Struct || k.kind == TypeKind::Named) {
				StringIdx ni = static_cast<StringIdx>(k.a);
				if (ni != kNoString) {
					return "__drop_" + strings.get(ni);
				}
			}
		}
	}
	return fn.Name;
}

JamFunctionRef FunctionAST::declarePrototype(JamCodegenContext &ctx) {
	std::string funcName =
	    mangledFunctionName(*this, ctx.getTypePool(), ctx.getStringPool());

	// P9.6 return ABI: large aggregates returned by Jam-defined fns
	// (not extern, not test) are sret — caller passes a leading
	// `ptr sret(%T) align A` arg, callee stores into it and returns
	// void. Small returns stay direct.
	jam::abi::ReturnABI rabi =
	    (isTest || isExtern)
	        ? jam::abi::ReturnABI{jam::abi::ReturnABI::Kind::Direct,
	                              (isTest || ReturnType == kNoType)
	                                  ? ctx.getVoidType()
	                                  : ctx.getLLVMType(ReturnType),
	                              0}
	        : jam::abi::classifyReturn(ReturnType, ctx);

	std::vector<JamTypeRef> ArgTypes;
	if (rabi.kind == jam::abi::ReturnABI::Kind::Indirect) {
		// sret slot leads the LLVM arg list.
		ArgTypes.push_back(
		    JamLLVMPointerType(ctx.getLLVMType(ReturnType), 0));
	}
	if (!isTest) {
		for (const auto &arg : Args) {
			if (isExtern) {
				// P9.8: extern fns follow the C ABI literally. The user
				// already wrote the parameter types as they want them
				// to appear at the FFI boundary (e.g. `*const T` for a
				// pointer arg, `u32` for a scalar). LLVM's backend
				// handles `byval` for large aggregates per the
				// platform's MEMORY classification; we do NOT
				// re-classify with mode-aware rules.
				ArgTypes.push_back(ctx.getLLVMType(arg.Type));
				continue;
			}
			// P9 mode-aware ABI. classifyParam decides per-(mode, type)
			// whether the parameter is passed by value (the type's natural
			// LLVM representation) or by pointer.
			jam::abi::ParamABI pabi =
			    jam::abi::classifyParam(arg.Mode, arg.Type, ctx);
			if (pabi.kind == jam::abi::ParamABI::Kind::ByPointer) {
				ArgTypes.push_back(JamLLVMPointerType(
				    ctx.getLLVMType(arg.Type), 0));
			} else {
				ArgTypes.push_back(pabi.llvmType);
			}
		}
	}

	JamTypeRef RetType =
	    (rabi.kind == jam::abi::ReturnABI::Kind::Indirect)
	        ? ctx.getVoidType()
	        : rabi.directType;

	JamTypeRef FT = JamLLVMFunctionType(RetType, ArgTypes.data(),
	                                    ArgTypes.size(), isVarArgs);

	JamFunctionRef F =
	    JamLLVMAddFunction(ctx.getModule(), funcName.c_str(), FT);
	JamLLVMApplyDefaultFnAttrs(F, isExtern);

	// Apply sret attributes to the leading parameter when applicable.
	if (rabi.kind == jam::abi::ReturnABI::Kind::Indirect) {
		JamLLVMAddParamAttrSret(F, 0, ctx.getLLVMType(ReturnType),
		                        rabi.sretAlign);
	}

	if (isExtern || isExport || Name == "main") {
		JamLLVMSetLinkage((JamValueRef)F, JAM_LINKAGE_EXTERNAL);
	} else {
		JamLLVMSetLinkage((JamValueRef)F, JAM_LINKAGE_INTERNAL);
	}

	// P9.6: when the function uses sret, the user's parameter at source
	// index `i` lives at LLVM index `i + 1` (the sret slot is index 0).
	const unsigned argOffset =
	    (rabi.kind == jam::abi::ReturnABI::Kind::Indirect) ? 1u : 0u;

	if (isExtern || isExport || Name == "main") {
		JamLLVMSetFunctionCallConv(F, JAM_CALLCONV_C);
		for (unsigned i = 0; i < Args.size(); i++) {
			if (Args[i].Type == BuiltinType::Bool) {
				JamLLVMAddParamAttrZeroExt(F, i + argOffset);
			}
		}
		if (ReturnType == BuiltinType::Bool) { JamLLVMAddRetAttrZeroExt(F); }
	}

	for (unsigned i = 0; i < Args.size(); i++) {
		JamValueRef param = JamLLVMGetParam(F, i + argOffset);
		JamLLVMSetValueName(param, Args[i].Name.c_str());
	}

	return F;
}

void FunctionAST::defineBody(JamCodegenContext &ctx) {
	if (isExtern) return;

	std::string funcName =
	    mangledFunctionName(*this, ctx.getTypePool(), ctx.getStringPool());
	JamFunctionRef F = JamLLVMGetFunction(ctx.getModule(), funcName.c_str());
	if (!F) {
		throw std::runtime_error(
		    "defineBody: prototype not declared for " + funcName);
	}

	JamBasicBlockRef BB = JamLLVMAppendBasicBlock(F, "entry");
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), BB);

	ctx.clearVariables();
	ctx.clearDrops();
	ctx.pushDropScope();  // function-level scope

	// P9.6: if this function uses sret, the leading LLVM arg is the
	// caller-provided result slot. Record it so codegenReturn writes
	// through it; user parameters shift one slot to the right.
	jam::abi::ReturnABI rabi =
	    isExtern
	        ? jam::abi::ReturnABI{jam::abi::ReturnABI::Kind::Direct,
	                              ReturnType == kNoType
	                                  ? ctx.getVoidType()
	                                  : ctx.getLLVMType(ReturnType),
	                              0}
	        : jam::abi::classifyReturn(ReturnType, ctx);
	unsigned argOffset = 0;
	if (rabi.kind == jam::abi::ReturnABI::Kind::Indirect) {
		ctx.setSretSlot(JamLLVMGetParam(F, 0));
		argOffset = 1;
	} else {
		ctx.setSretSlot(nullptr);
	}

	for (unsigned i = 0; i < Args.size(); i++) {
		// P9 mode-aware ABI: ByValue parameters are stored to a local
		// alloca on entry (matching the existing pattern for value
		// semantics). ByPointer parameters are *already* pointers to
		// caller-owned storage; we register the parameter directly as
		// the variable's place — reads will load through it, writes
		// will store through it, and the caller observes the mutations.
		jam::abi::ParamABI pabi =
		    jam::abi::classifyParam(Args[i].Mode, Args[i].Type, ctx);
		JamValueRef param = JamLLVMGetParam(F, i + argOffset);
		if (pabi.kind == jam::abi::ParamABI::Kind::ByPointer) {
			ctx.setVariable(Args[i].Name, param);
		} else {
			JamTypeRef ArgType = ctx.getLLVMType(Args[i].Type);
			JamValueRef Alloca = JamLLVMBuildAlloca(
			    ctx.getBuilder(), ArgType, ctx.typeAlign(Args[i].Type),
			    Args[i].Name.c_str());
			JamLLVMBuildStore(ctx.getBuilder(), param, Alloca);
			ctx.setVariable(Args[i].Name, Alloca);
		}
		ctx.setVariableType(Args[i].Name, Args[i].Type);
	}

	for (NodeIdx stmt : Body) { codegenNode(ctx, stmt); }

	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		// Implicit fall-through end of body — emit drops for the
		// function-level scope (the only active scope at this point) and
		// then the implicit terminator.
		emitTopScopeDrops(ctx);
		if (ReturnType == kNoType) {
			JamLLVMBuildRetVoid(ctx.getBuilder());
		}
	}
	ctx.popDropScope();
	ctx.clearDrops();
}

JamFunctionRef FunctionAST::codegen(JamCodegenContext &ctx) {
	JamFunctionRef F = declarePrototype(ctx);
	defineBody(ctx);
	return F;
}
