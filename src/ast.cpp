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
#include <string>
#include <vector>

JamBasicBlockRef CurrentLoopContinue = nullptr;
JamBasicBlockRef CurrentLoopBreak = nullptr;

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

		if (k.kind == TypeKind::PtrMany) {
			TypeIdx elemTy = static_cast<TypeIdx>(k.a);
			outElemType = ctx.getLLVMType(elemTy);
			JamTypeRef allocatedType = JamLLVMGetAllocatedType(alloca);
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

		JamTypeRef allocatedType = JamLLVMGetAllocatedType(alloca);
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
	}

	JamFunctionRef putsFunc = JamLLVMGetFunction(ctx.getModule(), "puts");
	if (!putsFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef putsRetType = ctx.getInt32Type();
		JamTypeRef putsParamTypes[1] = {i8PtrType};
		JamTypeRef putsType = JamLLVMFunctionType(
		    putsRetType, putsParamTypes, 1, false);
		putsFunc = JamLLVMAddFunction(ctx.getModule(), "puts", putsType);
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
	}
	JamFunctionRef exitFunc = JamLLVMGetFunction(ctx.getModule(), "exit");
	if (!exitFunc) {
		JamTypeRef exitParamTypes[1] = {ctx.getInt32Type()};
		JamTypeRef exitType = JamLLVMFunctionType(
		    ctx.getVoidType(), exitParamTypes, 1, false);
		exitFunc = JamLLVMAddFunction(ctx.getModule(), "exit", exitType);
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

	JamFunctionRef CalleeF =
	    JamLLVMGetFunction(ctx.getModule(), callee.c_str());
	if (!CalleeF) {
		throw std::runtime_error("Unknown function referenced: " + callee);
	}

	unsigned declaredParamCount = JamLLVMCountParams(CalleeF);
	bool isVarArg = JamLLVMFunctionIsVarArg(CalleeF);
	if (isVarArg ? argCount < declaredParamCount
	             : argCount != declaredParamCount) {
		throw std::runtime_error("Incorrect number of arguments passed to " +
		                         callee);
	}

	std::vector<JamValueRef> ArgsV;
	for (unsigned i = 0; i < argCount; i++) {
		if (i < declaredParamCount) {
			JamTypeRef expected = JamLLVMTypeOf(JamLLVMGetParam(CalleeF, i));
			JamValueRef argVal = codegenNode(ctx, args[i], expected);
			if (!argVal) return nullptr;
			argVal = coerceTo(ctx, argVal, expected);
			ArgsV.push_back(argVal);
		} else {
			JamValueRef argVal = codegenNode(ctx, args[i]);
			if (!argVal) return nullptr;
			ArgsV.push_back(argVal);
		}
	}

	const char *callName =
	    JamLLVMTypeIsVoid(JamLLVMGetReturnType(CalleeF)) ? "" : "calltmp";
	return JamLLVMBuildCall(ctx.getBuilder(), CalleeF, ArgsV.data(),
	                        ArgsV.size(), callName);
}

static JamValueRef codegenReturn(JamCodegenContext &ctx, const AstNode &n) {
	NodeIdx retIdx = static_cast<NodeIdx>(n.lhs);
	if (retIdx == kNoNode) {
		JamLLVMBuildRetVoid(ctx.getBuilder());
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	}
	JamFunctionRef func = JamLLVMGetBasicBlockParent(
	    JamLLVMGetInsertBlock(ctx.getBuilder()));
	JamTypeRef expected = JamLLVMGetReturnType(func);
	JamValueRef RetVal = codegenNode(ctx, retIdx, expected);
	if (!RetVal) return nullptr;
	if (JamLLVMTypeIsInteger(expected)) {
		RetVal = coerceTo(ctx, RetVal, expected);
	}
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
		JamTypeRef expected = JamLLVMGetAllocatedType(alloca);
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
		JamTypeRef ptrType = JamLLVMGetAllocatedType(alloca);
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
		JamValueRef rhsVal = codegenNode(ctx, valueIdx);
		if (!rhsVal) return nullptr;

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
		const auto *info = ctx.lookupStruct(varTy);
		if (!info) {
			throw std::runtime_error(
			    "Cannot assign to field of non-struct: " + varName);
		}

		JamValueRef outerValue = JamLLVMBuildLoad(ctx.getBuilder(), info->type,
		                                          alloca, varName.c_str());

		std::vector<JamValueRef> values;
		std::vector<unsigned> indices;
		std::vector<std::string> pathNames;
		TypeIdx typeAtLevel = varTy;
		values.push_back(outerValue);
		TypeIdx leafFieldType = kNoType;
		for (size_t i = 0; i < path.size(); i++) {
			const auto *curInfo = ctx.lookupStruct(typeAtLevel);
			if (!curInfo) {
				throw std::runtime_error("Cannot access field '" +
				                         sp.get(path[i]) + "' on non-struct");
			}
			const std::string &fieldName = sp.get(path[i]);
			pathNames.push_back(fieldName);
			int idx = ctx.getFieldIndex(curInfo->name, fieldName);
			if (idx < 0) {
				throw std::runtime_error("Unknown field '" + fieldName +
				                         "' in " + curInfo->name);
			}
			indices.push_back(static_cast<unsigned>(idx));
			if (i + 1 < path.size()) {
				JamValueRef inner = JamLLVMBuildExtractValue(
				    ctx.getBuilder(), values.back(),
				    static_cast<unsigned>(idx), fieldName.c_str());
				values.push_back(inner);
				typeAtLevel = curInfo->fields[idx].second;
			} else {
				leafFieldType = curInfo->fields[idx].second;
			}
		}

		JamTypeRef expected = ctx.getLLVMType(leafFieldType);
		JamValueRef newValue = coerceTo(ctx, rhsVal, expected);

		for (size_t i = path.size(); i > 0; i--) {
			newValue = JamLLVMBuildInsertValue(
			    ctx.getBuilder(), values[i - 1], newValue, indices[i - 1],
			    pathNames[i - 1].c_str());
		}

		JamLLVMBuildStore(ctx.getBuilder(), newValue, alloca);
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
	JamTypeRef ptrType = JamLLVMGetAllocatedType(alloca);
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
	JamValueRef Alloca =
	    JamLLVMBuildAlloca(ctx.getBuilder(), VarType, name.c_str());

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
	return Alloca;
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
	for (uint32_t i = 0; i < thenCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 2 + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), MergeBB);
	}

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), ElseBB);
	for (uint32_t i = 0; i < elseCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 2 + thenCount + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), MergeBB);
	}

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
	CurrentLoopContinue = CondBB;
	CurrentLoopBreak = AfterBB;

	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), CondBB);
	JamValueRef CondV = codegenNode(ctx, condIdx);
	if (!CondV) {
		CurrentLoopContinue = PrevContinue;
		CurrentLoopBreak = PrevBreak;
		return nullptr;
	}
	JamTypeRef condType = JamLLVMTypeOf(CondV);
	CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, CondV,
	                         JamLLVMConstInt(condType, 0, false), "whilecond");
	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, LoopBB, AfterBB);

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), LoopBB);
	for (uint32_t i = 0; i < bodyCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 1 + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), CondBB);
	}

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), AfterBB);
	CurrentLoopContinue = PrevContinue;
	CurrentLoopBreak = PrevBreak;
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

	JamValueRef Alloca =
	    JamLLVMBuildAlloca(ctx.getBuilder(), VarType, varName.c_str());
	JamLLVMBuildStore(ctx.getBuilder(), StartVal, Alloca);

	JamValueRef OldVal = ctx.getVariable(varName);
	ctx.setVariable(varName, Alloca);

	JamBasicBlockRef CondBB = JamLLVMAppendBasicBlock(TheFunction, "forcond");
	JamBasicBlockRef LoopBB = JamLLVMAppendBasicBlock(TheFunction, "forloop");
	JamBasicBlockRef IncrBB = JamLLVMAppendBasicBlock(TheFunction, "forincr");
	JamBasicBlockRef AfterBB =
	    JamLLVMAppendBasicBlock(TheFunction, "afterloop");

	JamBasicBlockRef PrevContinue = CurrentLoopContinue;
	JamBasicBlockRef PrevBreak = CurrentLoopBreak;
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
	for (uint32_t i = 0; i < bodyCount; i++) {
		codegenNode(ctx, ns.getExtra(extra + 4 + i));
	}
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), IncrBB);
	}

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

	// Evaluate the scrutinee once. The value will be reused for every
	// pattern test below; LLVM mem2reg promotes the local to an SSA
	// value for free.
	JamValueRef scrut = codegenNode(ctx, scrutIdx);
	if (!scrut) return nullptr;
	JamTypeRef scrutType = JamLLVMTypeOf(scrut);
	if (!JamLLVMTypeIsInteger(scrutType)) {
		throw std::runtime_error(
		    "M1 `match` only supports integer scrutinees");
	}

	JamBasicBlockRef curBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef func = JamLLVMGetBasicBlockParent(curBB);
	JamBasicBlockRef mergeBB =
	    JamLLVMAppendBasicBlock(func, "match.end");

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
		for (uint32_t b = 0; b < bodyCount; b++) {
			codegenNode(ctx, ns.getExtra(extra + bodyStart + b));
		}
		// Branch to merge unless the body already terminated (e.g. via
		// return).
		if (!JamLLVMGetBasicBlockTerminator(
		        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
			JamLLVMBuildBr(ctx.getBuilder(), mergeBB);
		}

		pos = bodyStart + bodyCount;
	}

	// Else block — runs if no arm matched.
	if (elseBodyCount > 0) {
		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), fallBB);
		for (uint32_t b = 0; b < elseBodyCount; b++) {
			codegenNode(ctx, ns.getExtra(extra + 2 + b));
		}
		if (!JamLLVMGetBasicBlockTerminator(
		        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
			JamLLVMBuildBr(ctx.getBuilder(), mergeBB);
		}
	}

	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), mergeBB);
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
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
	if (rootName == kNoString || !ctx.hasVariable(sp.get(rootName))) {
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

	const auto *info = ctx.lookupStruct(varTy);
	if (!info) {
		throw std::runtime_error(
		    "Direct member access codegen not yet implemented");
	}

	JamValueRef alloca = ctx.getVariable(varName);
	JamValueRef value = JamLLVMBuildLoad(ctx.getBuilder(), info->type, alloca,
	                                     varName.c_str());

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
		value = JamLLVMBuildExtractValue(ctx.getBuilder(), value,
		                                 static_cast<unsigned>(idx),
		                                 fieldName.c_str());
		currentType = curInfo->fields[idx].second;
	}
	return value;
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
		if (!V) throw std::runtime_error("Unknown variable name: " + name);
		JamTypeRef LoadType = JamLLVMGetAllocatedType(V);
		return JamLLVMBuildLoad(ctx.getBuilder(), LoadType, V, name.c_str());
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
		JamLLVMBuildBr(ctx.getBuilder(), CurrentLoopBreak);
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	case AstTag::Continue:
		if (!CurrentLoopContinue) {
			throw std::runtime_error("continue statement not inside a loop");
		}
		JamLLVMBuildBr(ctx.getBuilder(), CurrentLoopContinue);
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	case AstTag::ImportLit:
		return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
	case AstTag::StructLit:
		return codegenStructLit(ctx, n);
	case AstTag::MatchNode:
		return codegenMatch(ctx, n);
	case AstTag::PatLit:
	case AstTag::PatRange:
	case AstTag::PatWildcard:
	case AstTag::PatOr:
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

JamFunctionRef FunctionAST::declarePrototype(JamCodegenContext &ctx) {
	std::string funcName = isTest ? ("__test_" + Name) : Name;

	std::vector<JamTypeRef> ArgTypes;
	if (!isTest) {
		for (const auto &arg : Args) {
			ArgTypes.push_back(ctx.getLLVMType(arg.second));
		}
	}

	JamTypeRef RetType = (isTest || ReturnType == kNoType)
	                         ? ctx.getVoidType()
	                         : ctx.getLLVMType(ReturnType);

	JamTypeRef FT = JamLLVMFunctionType(RetType, ArgTypes.data(),
	                                    ArgTypes.size(), isVarArgs);

	JamFunctionRef F =
	    JamLLVMAddFunction(ctx.getModule(), funcName.c_str(), FT);

	if (isExtern || isExport || Name == "main") {
		JamLLVMSetLinkage((JamValueRef)F, JAM_LINKAGE_EXTERNAL);
	} else {
		JamLLVMSetLinkage((JamValueRef)F, JAM_LINKAGE_INTERNAL);
	}

	if (isExtern || isExport || Name == "main") {
		JamLLVMSetFunctionCallConv(F, JAM_CALLCONV_C);
		for (unsigned i = 0; i < Args.size(); i++) {
			if (Args[i].second == BuiltinType::Bool) {
				JamLLVMAddParamAttrZeroExt(F, i);
			}
		}
		if (ReturnType == BuiltinType::Bool) { JamLLVMAddRetAttrZeroExt(F); }
	}

	for (unsigned i = 0; i < Args.size(); i++) {
		JamValueRef param = JamLLVMGetParam(F, i);
		JamLLVMSetValueName(param, Args[i].first.c_str());
	}

	return F;
}

void FunctionAST::defineBody(JamCodegenContext &ctx) {
	if (isExtern) return;

	std::string funcName = isTest ? ("__test_" + Name) : Name;
	JamFunctionRef F = JamLLVMGetFunction(ctx.getModule(), funcName.c_str());
	if (!F) {
		throw std::runtime_error(
		    "defineBody: prototype not declared for " + funcName);
	}

	JamBasicBlockRef BB = JamLLVMAppendBasicBlock(F, "entry");
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), BB);

	ctx.clearVariables();
	for (unsigned i = 0; i < Args.size(); i++) {
		JamTypeRef ArgType = ctx.getLLVMType(Args[i].second);
		JamValueRef Alloca = JamLLVMBuildAlloca(ctx.getBuilder(), ArgType,
		                                        Args[i].first.c_str());
		JamValueRef param = JamLLVMGetParam(F, i);
		JamLLVMBuildStore(ctx.getBuilder(), param, Alloca);
		ctx.setVariable(Args[i].first, Alloca);
		ctx.setVariableType(Args[i].first, Args[i].second);
	}

	for (NodeIdx stmt : Body) { codegenNode(ctx, stmt); }

	if (ReturnType == kNoType && !JamLLVMGetBasicBlockTerminator(
	                                JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildRetVoid(ctx.getBuilder());
	}
}

JamFunctionRef FunctionAST::codegen(JamCodegenContext &ctx) {
	JamFunctionRef F = declarePrototype(ctx);
	defineBody(ctx);
	return F;
}
