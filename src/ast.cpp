/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "ast.h"
#include "codegen.h"
#include "jam_llvm.h"
#include <stdexcept>

// Global variables to track loop context for break/continue
JamBasicBlockRef CurrentLoopContinue = nullptr;
JamBasicBlockRef CurrentLoopBreak = nullptr;

JamValueRef NumberExprAST::codegen(JamCodegenContext &ctx) {
	// Choose appropriate type based on value range
	JamTypeRef IntType;
	if (IsNegative) {
		// For negative values, use signed ranges
		if (Val <= 128) {
			IntType = ctx.getInt8Type();
		} else if (Val <= 32768) {
			IntType = ctx.getInt16Type();
		} else if (Val <= 2147483648ULL) {
			IntType = ctx.getInt32Type();
		} else {
			IntType = ctx.getInt64Type();
		}
		// Create two's complement representation
		int64_t signedVal = -static_cast<int64_t>(Val);
		return JamLLVMConstInt(IntType, static_cast<uint64_t>(signedVal), true);
	} else {
		// For positive values, use unsigned ranges
		if (Val <= 255) {
			IntType = ctx.getInt8Type();
		} else if (Val <= 65535) {
			IntType = ctx.getInt16Type();
		} else if (Val <= 4294967295ULL) {
			IntType = ctx.getInt32Type();
		} else {
			IntType = ctx.getInt64Type();
		}
		return JamLLVMConstInt(IntType, Val, false);
	}
}

JamValueRef BooleanExprAST::codegen(JamCodegenContext &ctx) {
	return JamLLVMConstInt(ctx.getInt1Type(), Val ? 1 : 0, false);
}

JamValueRef UnaryExprAST::codegen(JamCodegenContext &ctx) {
	JamValueRef operandVal = Operand->codegen(ctx);
	if (!operandVal) return nullptr;

	if (Op == "!") {
		// Logical NOT: XOR with true (1)
		return JamLLVMBuildXor(ctx.getBuilder(), operandVal,
		                       JamLLVMConstInt(ctx.getInt1Type(), 1, false),
		                       "nottmp");
	}

	throw std::runtime_error("Invalid unary operator: " + Op);
}

JamValueRef StringLiteralExprAST::codegen(JamCodegenContext &ctx) {
	// Create a global string constant (null-terminated for C compatibility)
	JamValueRef StrConstant =
	    JamLLVMConstString(ctx.getContext(), Val.c_str(), Val.length(), true);

	// Create array type for the string constant
	JamTypeRef strArrayType =
	    JamLLVMArrayType(ctx.getInt8Type(), Val.length() + 1);

	// Create a global variable with the string constant
	JamValueRef StrGlobal =
	    JamLLVMAddGlobal(ctx.getModule(), strArrayType, "str");
	JamLLVMSetGlobalConstant(StrGlobal, true);
	JamLLVMSetInitializer(StrGlobal, StrConstant);

	// Create a string slice struct { ptr: *u8, len: usize }
	JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
	JamTypeRef usizeType = ctx.getInt64Type();
	JamTypeRef sliceTypes[2] = {i8PtrType, usizeType};
	JamTypeRef sliceType =
	    JamLLVMStructType(ctx.getContext(), sliceTypes, 2, false);

	// Get pointer to the string data
	JamValueRef StrPtr =
	    JamLLVMBuildBitCast(ctx.getBuilder(), StrGlobal, i8PtrType, "str_ptr");

	// Create the slice struct
	JamValueRef SliceStruct = JamLLVMGetUndef(sliceType);
	SliceStruct = JamLLVMBuildInsertValue(ctx.getBuilder(), SliceStruct, StrPtr,
	                                      0, "slice_ptr");
	SliceStruct = JamLLVMBuildInsertValue(
	    ctx.getBuilder(), SliceStruct,
	    JamLLVMConstInt(usizeType, Val.length(), false), 1, "slice_len");

	return SliceStruct;
}

JamValueRef VariableExprAST::codegen(JamCodegenContext &ctx) {
	JamValueRef V = ctx.getVariable(Name);
	if (!V) throw std::runtime_error("Unknown variable name: " + Name);

	// Get the type from the allocated value
	JamTypeRef LoadType = JamLLVMGetAllocatedType(V);
	return JamLLVMBuildLoad(ctx.getBuilder(), LoadType, V, Name.c_str());
}

JamValueRef BinaryExprAST::codegen(JamCodegenContext &ctx) {
	// Handle short-circuit evaluation for 'and' and 'or'
	if (Op == "and" || Op == "or") {
		// Evaluate LHS first
		JamValueRef L = LHS->codegen(ctx);
		if (!L) return nullptr;

		// Get the current function and create blocks
		JamBasicBlockRef currentBlock = JamLLVMGetInsertBlock(ctx.getBuilder());
		JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(currentBlock);
		JamBasicBlockRef rhsBlock = JamLLVMAppendBasicBlock(
		    TheFunction, Op == "and" ? "and.rhs" : "or.rhs");
		JamBasicBlockRef mergeBlock = JamLLVMAppendBasicBlock(
		    TheFunction, Op == "and" ? "and.end" : "or.end");

		// For 'and': if LHS is false, short-circuit to false
		// For 'or': if LHS is true, short-circuit to true
		if (Op == "and") {
			JamLLVMBuildCondBr(ctx.getBuilder(), L, rhsBlock, mergeBlock);
		} else {
			JamLLVMBuildCondBr(ctx.getBuilder(), L, mergeBlock, rhsBlock);
		}

		// Evaluate RHS in rhsBlock
		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), rhsBlock);
		JamValueRef R = RHS->codegen(ctx);
		if (!R) return nullptr;
		JamLLVMBuildBr(ctx.getBuilder(), mergeBlock);
		// Update rhsBlock in case RHS codegen changed the current block
		rhsBlock = JamLLVMGetInsertBlock(ctx.getBuilder());

		// Create phi node in merge block
		JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), mergeBlock);
		JamValueRef phi =
		    JamLLVMBuildPhi(ctx.getBuilder(), ctx.getInt1Type(),
		                    Op == "and" ? "and.result" : "or.result");

		// Add incoming values
		JamValueRef shortCircuitVal =
		    JamLLVMConstInt(ctx.getInt1Type(), Op == "and" ? 0 : 1, false);
		JamValueRef incomingVals[2] = {shortCircuitVal, R};
		JamBasicBlockRef incomingBlocks[2] = {currentBlock, rhsBlock};
		JamLLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);

		return phi;
	}

	// For other operators, evaluate both sides
	JamValueRef L = LHS->codegen(ctx);
	JamValueRef R = RHS->codegen(ctx);

	if (!L || !R) return nullptr;

	if (Op == "+") return JamLLVMBuildAdd(ctx.getBuilder(), L, R, "addtmp");
	else if (Op == "==")
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_EQ, L, R, "cmptmp");
	else if (Op == "!=")
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, L, R, "cmptmp");
	else if (Op == "<")
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_ULT, L, R, "cmptmp");
	else if (Op == "<=")
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_ULE, L, R, "cmptmp");
	else if (Op == ">")
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_UGT, L, R, "cmptmp");
	else if (Op == ">=")
		return JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_UGE, L, R, "cmptmp");

	throw std::runtime_error("Invalid binary operator: " + Op);
}

JamValueRef CallExprAST::codegen(JamCodegenContext &ctx) {
	// Handle std.fmt print functions
	if (Callee == "std.fmt.print" || Callee == "std.fmt.println") {
		return generatePrintCall(ctx);
	}

	// Handle std.thread.sleep (takes milliseconds as u64)
	if (Callee == "std.thread.sleep") { return generateSleepCall(ctx); }

	// Handle assert from test module
	if (Callee == "assert") { return generateAssertCall(ctx); }

	JamFunctionRef CalleeF =
	    JamLLVMGetFunction(ctx.getModule(), Callee.c_str());
	if (!CalleeF)
		throw std::runtime_error("Unknown function referenced: " + Callee);

	if (JamLLVMCountParams(CalleeF) != Args.size())
		throw std::runtime_error("Incorrect number of arguments passed");

	std::vector<JamValueRef> ArgsV;
	for (unsigned i = 0, e = Args.size(); i != e; ++i) {
		ArgsV.push_back(Args[i]->codegen(ctx));
		if (!ArgsV.back()) return nullptr;
	}

	return JamLLVMBuildCall(ctx.getBuilder(), CalleeF, ArgsV.data(),
	                        ArgsV.size(), "calltmp");
}

JamValueRef CallExprAST::generatePrintCall(JamCodegenContext &ctx) {
	// Declare printf function if not already declared
	JamFunctionRef printfFunc = JamLLVMGetFunction(ctx.getModule(), "printf");
	if (!printfFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef printfRetType = ctx.getInt32Type();
		JamTypeRef printfParamTypes[1] = {i8PtrType};
		JamTypeRef printfType = JamLLVMFunctionType(
		    printfRetType, printfParamTypes, 1, true);  // varargs
		printfFunc = JamLLVMAddFunction(ctx.getModule(), "printf", printfType);
	}

	// Declare puts function for simple println
	JamFunctionRef putsFunc = JamLLVMGetFunction(ctx.getModule(), "puts");
	if (!putsFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef putsRetType = ctx.getInt32Type();
		JamTypeRef putsParamTypes[1] = {i8PtrType};
		JamTypeRef putsType =
		    JamLLVMFunctionType(putsRetType, putsParamTypes, 1, false);
		putsFunc = JamLLVMAddFunction(ctx.getModule(), "puts", putsType);
	}

	JamValueRef result = nullptr;

	if (Callee == "std.fmt.println" && Args.size() == 1) {
		// Simple println with one string argument - use puts
		JamValueRef arg = Args[0]->codegen(ctx);
		if (!arg) return nullptr;

		// If it's a string slice, extract the pointer
		if (JamLLVMTypeIsStruct(JamLLVMTypeOf(arg))) {
			arg = JamLLVMBuildExtractValue(ctx.getBuilder(), arg, 0, "str_ptr");
		}

		JamValueRef callArgs[1] = {arg};
		result = JamLLVMBuildCall(ctx.getBuilder(), putsFunc, callArgs, 1,
		                          "puts_call");
	} else if (Callee == "std.fmt.print" && Args.size() == 1) {
		// Simple print with one string argument - use printf without newline
		JamValueRef arg = Args[0]->codegen(ctx);
		if (!arg) return nullptr;

		// If it's a string slice, extract the pointer
		if (JamLLVMTypeIsStruct(JamLLVMTypeOf(arg))) {
			arg = JamLLVMBuildExtractValue(ctx.getBuilder(), arg, 0, "str_ptr");
		}

		// Create format string "%s" for printf
		JamValueRef formatPtr =
		    JamLLVMBuildGlobalStringPtr(ctx.getBuilder(), "%s", "print_fmt");

		JamValueRef callArgs[2] = {formatPtr, arg};
		result = JamLLVMBuildCall(ctx.getBuilder(), printfFunc, callArgs, 2,
		                          "printf_call");
	} else {
		// For now, just handle simple cases
		throw std::runtime_error(
		    "Complex print formatting not yet implemented");
	}

	// Return the result (printf/puts return int)
	return result;
}

JamValueRef CallExprAST::generateSleepCall(JamCodegenContext &ctx) {
	if (Args.size() != 1) {
		throw std::runtime_error(
		    "std.thread.sleep expects exactly 1 argument (milliseconds)");
	}

	// Declare usleep function if not already declared
	// usleep takes microseconds (useconds_t which is u32)
	JamFunctionRef usleepFunc = JamLLVMGetFunction(ctx.getModule(), "usleep");
	if (!usleepFunc) {
		JamTypeRef usleepRetType = ctx.getInt32Type();
		JamTypeRef usleepParamTypes[1] = {ctx.getInt32Type()};
		JamTypeRef usleepType =
		    JamLLVMFunctionType(usleepRetType, usleepParamTypes, 1, false);
		usleepFunc = JamLLVMAddFunction(ctx.getModule(), "usleep", usleepType);
	}

	// Get the milliseconds argument
	JamValueRef msArg = Args[0]->codegen(ctx);
	if (!msArg) return nullptr;

	// Convert milliseconds to microseconds (multiply by 1000)
	// First ensure we have a 64-bit value for the multiplication
	JamTypeRef i64Type = ctx.getInt64Type();
	JamValueRef msArg64 =
	    JamLLVMBuildIntCast(ctx.getBuilder(), msArg, i64Type, false, "ms_cast");
	JamValueRef thousand = JamLLVMConstInt(i64Type, 1000, false);
	JamValueRef usArg64 =
	    JamLLVMBuildMul(ctx.getBuilder(), msArg64, thousand, "us_mul");

	// Truncate to u32 for usleep (handles up to ~4 million ms = ~71 minutes)
	JamValueRef usArg = JamLLVMBuildIntCast(
	    ctx.getBuilder(), usArg64, ctx.getInt32Type(), false, "us_trunc");

	JamValueRef callArgs[1] = {usArg};
	return JamLLVMBuildCall(ctx.getBuilder(), usleepFunc, callArgs, 1,
	                        "usleep_call");
}

JamValueRef CallExprAST::generateAssertCall(JamCodegenContext &ctx) {
	if (Args.size() != 2) {
		throw std::runtime_error(
		    "assert expects exactly 2 arguments (actual, expected)");
	}

	JamValueRef actual = Args[0]->codegen(ctx);
	JamValueRef expected = Args[1]->codegen(ctx);
	if (!actual || !expected) return nullptr;

	// Ensure both values have the same type
	JamTypeRef actualType = JamLLVMTypeOf(actual);
	JamTypeRef expectedType = JamLLVMTypeOf(expected);
	if (actualType != expectedType) {
		if (JamLLVMTypeIsInteger(actualType) &&
		    JamLLVMTypeIsInteger(expectedType)) {
			unsigned actualWidth = JamLLVMGetIntTypeWidth(actualType);
			unsigned expectedWidth = JamLLVMGetIntTypeWidth(expectedType);
			if (actualWidth > expectedWidth) {
				expected =
				    JamLLVMBuildIntCast(ctx.getBuilder(), expected, actualType,
				                        false, "assert_cast");
			} else {
				actual =
				    JamLLVMBuildIntCast(ctx.getBuilder(), actual, expectedType,
				                        false, "assert_cast");
			}
		}
	}

	// Compare actual == expected
	JamValueRef cmpResult = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_EQ,
	                                         actual, expected, "assert_cmp");

	// Get current function and create blocks
	JamBasicBlockRef currentBlock = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(currentBlock);
	JamBasicBlockRef failBlock =
	    JamLLVMAppendBasicBlock(TheFunction, "assert.fail");
	JamBasicBlockRef passBlock =
	    JamLLVMAppendBasicBlock(TheFunction, "assert.pass");

	JamLLVMBuildCondBr(ctx.getBuilder(), cmpResult, passBlock, failBlock);

	// Fail block: print error and exit(1)
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), failBlock);

	// Declare printf if needed
	JamFunctionRef printfFunc = JamLLVMGetFunction(ctx.getModule(), "printf");
	if (!printfFunc) {
		JamTypeRef i8PtrType = JamLLVMPointerType(ctx.getInt8Type(), 0);
		JamTypeRef printfParamTypes[1] = {i8PtrType};
		JamTypeRef printfType =
		    JamLLVMFunctionType(ctx.getInt32Type(), printfParamTypes, 1, true);
		printfFunc = JamLLVMAddFunction(ctx.getModule(), "printf", printfType);
	}

	// Declare exit if needed
	JamFunctionRef exitFunc = JamLLVMGetFunction(ctx.getModule(), "exit");
	if (!exitFunc) {
		JamTypeRef exitParamTypes[1] = {ctx.getInt32Type()};
		JamTypeRef exitType =
		    JamLLVMFunctionType(ctx.getVoidType(), exitParamTypes, 1, false);
		exitFunc = JamLLVMAddFunction(ctx.getModule(), "exit", exitType);
	}

	// Print "assertion failed\n"
	JamValueRef fmtStr = JamLLVMBuildGlobalStringPtr(
	    ctx.getBuilder(), "assertion failed\n", "assert_fail_msg");
	JamValueRef printArgs[1] = {fmtStr};
	JamLLVMBuildCall(ctx.getBuilder(), printfFunc, printArgs, 1, "");

	// Call exit(1)
	JamValueRef exitCode = JamLLVMConstInt(ctx.getInt32Type(), 1, false);
	JamValueRef exitArgs[1] = {exitCode};
	JamLLVMBuildCall(ctx.getBuilder(), exitFunc, exitArgs, 1, "");
	JamLLVMBuildUnreachable(ctx.getBuilder());

	// Pass block: continue execution
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), passBlock);

	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

JamValueRef ReturnExprAST::codegen(JamCodegenContext &ctx) {
	JamValueRef RetVal = this->RetVal->codegen(ctx);
	if (!RetVal) return nullptr;

	JamLLVMBuildRet(ctx.getBuilder(), RetVal);
	return RetVal;
}

JamValueRef VarDeclAST::codegen(JamCodegenContext &ctx) {
	JamTypeRef VarType = ctx.getTypeFromString(Type);
	JamValueRef Alloca =
	    JamLLVMBuildAlloca(ctx.getBuilder(), VarType, Name.c_str());

	if (Init) {
		JamValueRef InitVal = Init->codegen(ctx);
		if (!InitVal) return nullptr;
		JamLLVMBuildStore(ctx.getBuilder(), InitVal, Alloca);
	} else {
		// Initialize with zero/null value
		JamValueRef ZeroVal = JamLLVMConstNull(VarType);
		JamLLVMBuildStore(ctx.getBuilder(), ZeroVal, Alloca);
	}

	ctx.setVariable(Name, Alloca);
	return Alloca;
}

JamValueRef IfExprAST::codegen(JamCodegenContext &ctx) {
	JamValueRef CondV = Condition->codegen(ctx);
	if (!CondV) return nullptr;

	// Convert condition to a bool by comparing non-equal to 0
	JamTypeRef condType = JamLLVMTypeOf(CondV);
	CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, CondV,
	                         JamLLVMConstInt(condType, 0, false), "ifcond");

	JamBasicBlockRef CurrentBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(CurrentBB);

	// Create blocks for the then and else cases
	JamBasicBlockRef ThenBB = JamLLVMAppendBasicBlock(TheFunction, "then");
	JamBasicBlockRef ElseBB = JamLLVMAppendBasicBlock(TheFunction, "else");
	JamBasicBlockRef MergeBB = JamLLVMAppendBasicBlock(TheFunction, "ifcont");

	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, ThenBB, ElseBB);

	// Emit then value.
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), ThenBB);
	JamValueRef ThenV = nullptr;
	for (auto &Expr : ThenBody) { ThenV = Expr->codegen(ctx); }
	// Only create branch if the block doesn't already have a terminator (like
	// return)
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), MergeBB);
	}

	// Emit else block.
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), ElseBB);
	JamValueRef ElseV = nullptr;
	for (auto &Expr : ElseBody) { ElseV = Expr->codegen(ctx); }
	// Only create branch if the block doesn't already have a terminator (like
	// return)
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), MergeBB);
	}

	// Emit merge block.
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), MergeBB);

	// For now, if statements don't return values, so we return a dummy value
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

JamValueRef WhileExprAST::codegen(JamCodegenContext &ctx) {
	JamBasicBlockRef CurrentBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(CurrentBB);

	// Create blocks for the loop
	JamBasicBlockRef CondBB = JamLLVMAppendBasicBlock(TheFunction, "whilecond");
	JamBasicBlockRef LoopBB = JamLLVMAppendBasicBlock(TheFunction, "whileloop");
	JamBasicBlockRef AfterBB =
	    JamLLVMAppendBasicBlock(TheFunction, "afterloop");

	// Save previous loop context
	JamBasicBlockRef PrevContinue = CurrentLoopContinue;
	JamBasicBlockRef PrevBreak = CurrentLoopBreak;
	CurrentLoopContinue = CondBB;
	CurrentLoopBreak = AfterBB;

	// Jump to condition block
	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	// Emit condition block
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), CondBB);
	JamValueRef CondV = Condition->codegen(ctx);
	if (!CondV) {
		// Restore previous loop context
		CurrentLoopContinue = PrevContinue;
		CurrentLoopBreak = PrevBreak;
		return nullptr;
	}

	// Convert condition to a bool by comparing non-equal to 0
	JamTypeRef condType = JamLLVMTypeOf(CondV);
	CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_NE, CondV,
	                         JamLLVMConstInt(condType, 0, false), "whilecond");
	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, LoopBB, AfterBB);

	// Emit loop body
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), LoopBB);
	for (auto &Expr : Body) { Expr->codegen(ctx); }

	// Only create branch if the block doesn't already have a terminator
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), CondBB);
	}

	// Emit after block
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), AfterBB);

	// Restore previous loop context
	CurrentLoopContinue = PrevContinue;
	CurrentLoopBreak = PrevBreak;

	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

JamValueRef ForExprAST::codegen(JamCodegenContext &ctx) {
	JamBasicBlockRef CurrentBB = JamLLVMGetInsertBlock(ctx.getBuilder());
	JamFunctionRef TheFunction = JamLLVMGetBasicBlockParent(CurrentBB);

	// Compute start and end values first
	JamValueRef StartVal = Start->codegen(ctx);
	JamValueRef EndVal = End->codegen(ctx);
	if (!StartVal || !EndVal) return nullptr;

	// Use the type of the start value for the loop variable
	JamTypeRef VarType = JamLLVMTypeOf(StartVal);

	// Convert end value to match start value type if needed
	JamTypeRef endType = JamLLVMTypeOf(EndVal);
	if (endType != VarType) {
		if (JamLLVMTypeIsInteger(VarType) && JamLLVMTypeIsInteger(endType)) {
			EndVal = JamLLVMBuildIntCast(ctx.getBuilder(), EndVal, VarType,
			                             true, "endcast");
		} else {
			throw std::runtime_error("Type mismatch in for loop range");
		}
	}

	// Create an alloca for the loop variable
	JamValueRef Alloca =
	    JamLLVMBuildAlloca(ctx.getBuilder(), VarType, VarName.c_str());

	// Store the start value
	JamLLVMBuildStore(ctx.getBuilder(), StartVal, Alloca);

	// Save the old variable binding (if any)
	JamValueRef OldVal = ctx.getVariable(VarName);
	ctx.setVariable(VarName, Alloca);

	// Create blocks for the loop
	JamBasicBlockRef CondBB = JamLLVMAppendBasicBlock(TheFunction, "forcond");
	JamBasicBlockRef LoopBB = JamLLVMAppendBasicBlock(TheFunction, "forloop");
	JamBasicBlockRef IncrBB = JamLLVMAppendBasicBlock(TheFunction, "forincr");
	JamBasicBlockRef AfterBB =
	    JamLLVMAppendBasicBlock(TheFunction, "afterloop");

	// Save previous loop context - continue should go to increment, break to
	// after
	JamBasicBlockRef PrevContinue = CurrentLoopContinue;
	JamBasicBlockRef PrevBreak = CurrentLoopBreak;
	CurrentLoopContinue = IncrBB;  // Continue goes to increment block
	CurrentLoopBreak = AfterBB;

	// Jump to condition block
	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	// Emit condition block
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), CondBB);
	JamValueRef CurVar =
	    JamLLVMBuildLoad(ctx.getBuilder(), VarType, Alloca, VarName.c_str());
	JamValueRef CondV = JamLLVMBuildICmp(ctx.getBuilder(), JAM_ICMP_SLT, CurVar,
	                                     EndVal, "forcond");
	JamLLVMBuildCondBr(ctx.getBuilder(), CondV, LoopBB, AfterBB);

	// Emit loop body
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), LoopBB);
	for (auto &Expr : Body) { Expr->codegen(ctx); }

	// Only create branch if the block doesn't already have a terminator
	if (!JamLLVMGetBasicBlockTerminator(
	        JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildBr(ctx.getBuilder(), IncrBB);
	}

	// Emit increment block
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), IncrBB);
	JamValueRef CurVarForIncrement =
	    JamLLVMBuildLoad(ctx.getBuilder(), VarType, Alloca, VarName.c_str());
	JamValueRef StepVal = JamLLVMConstInt(VarType, 1, false);
	JamValueRef NextVar = JamLLVMBuildAdd(ctx.getBuilder(), CurVarForIncrement,
	                                      StepVal, "nextvar");
	JamLLVMBuildStore(ctx.getBuilder(), NextVar, Alloca);
	JamLLVMBuildBr(ctx.getBuilder(), CondBB);

	// Emit after block
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), AfterBB);

	// Restore the old variable binding
	if (OldVal) ctx.setVariable(VarName, OldVal);
	// Note: We don't have a removeVariable, so we just leave the new one

	// Restore previous loop context
	CurrentLoopContinue = PrevContinue;
	CurrentLoopBreak = PrevBreak;

	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

JamValueRef BreakExprAST::codegen(JamCodegenContext &ctx) {
	if (!CurrentLoopBreak) {
		throw std::runtime_error("break statement not inside a loop");
	}

	JamLLVMBuildBr(ctx.getBuilder(), CurrentLoopBreak);
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

JamValueRef ContinueExprAST::codegen(JamCodegenContext &ctx) {
	if (!CurrentLoopContinue) {
		throw std::runtime_error("continue statement not inside a loop");
	}

	JamLLVMBuildBr(ctx.getBuilder(), CurrentLoopContinue);
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

JamValueRef ImportExprAST::codegen(JamCodegenContext &ctx) {
	// Import expressions are handled at compile time, not runtime
	// For now, we return a placeholder value
	// The actual import resolution happens in the module system
	return JamLLVMConstInt(ctx.getInt8Type(), 0, false);
}

std::string MemberAccessExprAST::getQualifiedName() const {
	std::string base;

	if (auto *memberAccess =
	        dynamic_cast<MemberAccessExprAST *>(Object.get())) {
		base = memberAccess->getQualifiedName();
	} else if (auto *varExpr = dynamic_cast<VariableExprAST *>(Object.get())) {
		base = varExpr->getName();
	} else {
		throw std::runtime_error("Invalid member access chain");
	}

	return base + "." + Member;
}

JamValueRef MemberAccessExprAST::codegen(JamCodegenContext &ctx) {
	// Member access is resolved at compile time for module access
	// For now, throw an error as we need the qualified name for function calls
	throw std::runtime_error(
	    "Direct member access codegen not yet implemented");
}

JamFunctionRef FunctionAST::codegen(JamCodegenContext &ctx) {
	// Test functions are always void with no args, use prefixed name to avoid
	// collision
	std::string funcName = isTest ? ("__test_" + Name) : Name;

	// Create function prototype
	std::vector<JamTypeRef> ArgTypes;
	if (!isTest) {
		for (const auto &arg : Args) {
			ArgTypes.push_back(ctx.getTypeFromString(arg.second));
		}
	}

	JamTypeRef RetType = (isTest || ReturnType.empty())
	                         ? ctx.getVoidType()
	                         : ctx.getTypeFromString(ReturnType);

	JamTypeRef FT =
	    JamLLVMFunctionType(RetType, ArgTypes.data(), ArgTypes.size(),
	                        false  // Not vararg
	    );

	// Create the function with appropriate linkage
	JamFunctionRef F =
	    JamLLVMAddFunction(ctx.getModule(), funcName.c_str(), FT);

	// Set linkage - main() is always exported, as are extern and export
	// functions
	if (isExtern || isExport || Name == "main") {
		JamLLVMSetLinkage((JamValueRef)F, JAM_LINKAGE_EXTERNAL);
	} else {
		JamLLVMSetLinkage((JamValueRef)F, JAM_LINKAGE_INTERNAL);
	}

	// Set calling convention to C for extern/export functions and main
	if (isExtern || isExport || Name == "main") {
		JamLLVMSetFunctionCallConv(F, JAM_CALLCONV_C);
	}

	// Set names for all arguments
	for (unsigned i = 0; i < Args.size(); i++) {
		JamValueRef param = JamLLVMGetParam(F, i);
		JamLLVMSetValueName(param, Args[i].first.c_str());
	}

	// Extern functions don't have a body
	if (isExtern) { return F; }

	// Create a new basic block to start insertion into
	JamBasicBlockRef BB = JamLLVMAppendBasicBlock(F, "entry");
	JamLLVMPositionBuilderAtEnd(ctx.getBuilder(), BB);

	// Record the function arguments in the NamedValues map
	ctx.clearVariables();
	for (unsigned i = 0; i < Args.size(); i++) {
		// Create an alloca for this variable using the correct type
		JamTypeRef ArgType = ctx.getTypeFromString(Args[i].second);
		JamValueRef Alloca = JamLLVMBuildAlloca(ctx.getBuilder(), ArgType,
		                                        Args[i].first.c_str());

		// Store the initial value into the alloca
		JamValueRef param = JamLLVMGetParam(F, i);
		JamLLVMBuildStore(ctx.getBuilder(), param, Alloca);

		// Add arguments to variable symbol table
		ctx.setVariable(Args[i].first, Alloca);
	}

	// Generate code for each expression in the function body
	for (auto &Expr : Body) { Expr->codegen(ctx); }

	// Add implicit return for void functions if not already present
	if (ReturnType.empty() && !JamLLVMGetBasicBlockTerminator(
	                              JamLLVMGetInsertBlock(ctx.getBuilder()))) {
		JamLLVMBuildRetVoid(ctx.getBuilder());
	}

	// Validate the generated code, checking for consistency
	JamLLVMVerifyFunction(F);

	return F;
}
