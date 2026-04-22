/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "cabi.h"
#include "codegen.h"
#include "jam_llvm.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_table.h"
#include "target.h"
#include <filesystem>

static int compileAndRun(const std::string &filename,
                         const std::string &outputName, bool runFlag,
                         bool emitIR, bool testMode) {
	std::ifstream file(filename);
	if (!file.is_open()) {
		std::cerr << "Could not open file: " << filename << std::endl;
		return 1;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string source = buffer.str();

	// Create codegen context using wrapper
	JamCodegenContext codegenCtx("jam_module");

	// Tokenize the source code
	Lexer lexer(source);
	std::vector<Token> tokens = lexer.scanTokens();

	// Parse the tokens into an AST
	Parser parser(tokens);
	std::unique_ptr<ModuleAST> module = parser.parse();

	// Get base directory for module resolution
	std::filesystem::path sourcePath(filename);
	std::string baseDir = sourcePath.parent_path().string();
	if (baseDir.empty()) { baseDir = "."; }

	// Create module resolver and symbol table
	ModuleResolver resolver(baseDir);
	SymbolTable symbolTable;

	// Register builtin test module symbols
	symbolTable.registerBuiltinSymbol("test", "assert");

	// Process regular imports (const std = import("std"))
	for (auto &import : module->Imports) {
		if (import->Path == "std" || import->Path == "test") {
			// builtin modules are handled specially in codegen
			continue;
		}

		ModuleAST *importedModule = resolver.getOrLoadModule(import->Path);
		if (!importedModule) {
			std::cerr << "Error: Failed to load module: " << import->Path
			          << std::endl;
			return 1;
		}

		// Register the module's exported symbols
		symbolTable.registerModule(import->Path, importedModule);
	}

	// Process destructuring imports (const { func } = import("mod"))
	for (auto &destImport : module->DestructuringImports) {
		ModuleAST *importedModule = resolver.getOrLoadModule(destImport->Path);
		if (!importedModule) {
			std::cerr << "Error: Failed to load module: " << destImport->Path
			          << std::endl;
			return 1;
		}

		// Register the module's exported symbols
		symbolTable.registerModule(destImport->Path, importedModule);

		// Register each binding
		for (const auto &name : destImport->Names) {
			if (!symbolTable.hasSymbol(destImport->Path, name)) {
				std::cerr << "Error: Symbol '" << name
				          << "' is not exported from module '"
				          << destImport->Path << "'" << std::endl;
				return 1;
			}
			symbolTable.registerBinding(name, destImport->Path, name);
		}
	}

	// Generate code for imported modules first
	for (const auto &[path, importedModule] : resolver.getLoadedModules()) {
		if (path == "std") continue;  // Skip std

		for (auto &func : importedModule->Functions) {
			if (func->isPub) { func->codegen(codegenCtx); }
		}
	}

	// Generate code from the main module's AST
	std::vector<std::string> testFunctionNames;
	for (auto &function : module->Functions) {
		if (function->isTest && !testMode) {
			continue;  // Skip test functions in non-test builds
		}
		if (!function->isTest && testMode && function->Name == "main") {
			continue;  // Skip user main in test mode
		}
		if (function->isTest) {
			testFunctionNames.push_back("__test_" + function->Name);
		}
		function->codegen(codegenCtx);
	}

	// In test mode, generate a main() that calls all test functions
	if (testMode) {
		if (testFunctionNames.empty()) {
			// No tests in this file: nothing to link or run.
			return 0;
		}

		JamTypeRef mainRetType = codegenCtx.getInt32Type();
		JamTypeRef mainFT = JamLLVMFunctionType(mainRetType, nullptr, 0, false);
		JamFunctionRef mainFunc =
		    JamLLVMAddFunction(codegenCtx.getModule(), "main", mainFT);
		JamLLVMSetLinkage((JamValueRef)mainFunc, JAM_LINKAGE_EXTERNAL);
		JamLLVMSetFunctionCallConv(mainFunc, JAM_CALLCONV_C);

		JamBasicBlockRef entryBB = JamLLVMAppendBasicBlock(mainFunc, "entry");
		JamLLVMPositionBuilderAtEnd(codegenCtx.getBuilder(), entryBB);

		// Declare printf for test output
		JamFunctionRef printfFunc =
		    JamLLVMGetFunction(codegenCtx.getModule(), "printf");
		if (!printfFunc) {
			JamTypeRef i8PtrType =
			    JamLLVMPointerType(codegenCtx.getInt8Type(), 0);
			JamTypeRef printfParamTypes[1] = {i8PtrType};
			JamTypeRef printfType = JamLLVMFunctionType(
			    codegenCtx.getInt32Type(), printfParamTypes, 1, true);
			printfFunc = JamLLVMAddFunction(codegenCtx.getModule(), "printf",
			                                printfType);
		}

		for (const auto &name : testFunctionNames) {
			JamFunctionRef testFunc =
			    JamLLVMGetFunction(codegenCtx.getModule(), name.c_str());
			if (testFunc) {
				// Print test name (strip __test_ prefix for display)
				std::string displayName = name.substr(7);  // remove "__test_"
				std::string msg = "testing " + displayName + "... ";
				JamValueRef msgStr = JamLLVMBuildGlobalStringPtr(
				    codegenCtx.getBuilder(), msg.c_str(), "test_msg");
				JamValueRef printArgs[1] = {msgStr};
				JamLLVMBuildCall(codegenCtx.getBuilder(), printfFunc, printArgs,
				                 1, "");

				// Call test function
				JamLLVMBuildCall(codegenCtx.getBuilder(), testFunc, nullptr, 0,
				                 "");

				// Print pass (if assert fails, exit(1) is called before
				// reaching here)
				JamValueRef passStr = JamLLVMBuildGlobalStringPtr(
				    codegenCtx.getBuilder(), "ok\n", "test_pass");
				JamValueRef passArgs[1] = {passStr};
				JamLLVMBuildCall(codegenCtx.getBuilder(), printfFunc, passArgs,
				                 1, "");
			}
		}

		// Print summary
		std::string summary =
		    std::to_string(testFunctionNames.size()) + " test(s) passed\n";
		JamValueRef summaryStr = JamLLVMBuildGlobalStringPtr(
		    codegenCtx.getBuilder(), summary.c_str(), "test_summary");
		JamValueRef summaryArgs[1] = {summaryStr};
		JamLLVMBuildCall(codegenCtx.getBuilder(), printfFunc, summaryArgs, 1,
		                 "");

		JamLLVMBuildRet(codegenCtx.getBuilder(),
		                JamLLVMConstInt(mainRetType, 0, false));
		JamLLVMVerifyFunction(mainFunc);
	}

	// Optionally print LLVM IR
	if (emitIR) {
		char *irStr = JamLLVMPrintModuleToString(codegenCtx.getModule());
		std::cout << irStr;
		JamLLVMDisposeMessage(irStr);
	}

	// Get target triple
	char *tripleStr = JamLLVMGetDefaultTargetTriple();
	JamLLVMSetTargetTriple(codegenCtx.getModule(), tripleStr);

	// Create target machine
	JamTargetMachineRef tm =
	    JamLLVMCreateTargetMachine(tripleStr, "generic", "",
	                               false  // not PIC for now
	    );
	JamLLVMDisposeMessage(tripleStr);

	if (!tm) {
		std::cerr << "Failed to create target machine" << std::endl;
		return 1;
	}

	JamLLVMSetDataLayout(codegenCtx.getModule(), tm);

	// Emit object file
	std::string objectFile = outputName + ".o";
	char *emitError = nullptr;
	bool success = JamLLVMEmitObjectFile(codegenCtx.getModule(), tm,
	                                     objectFile.c_str(), &emitError);

	JamLLVMDisposeTargetMachine(tm);

	if (!success) {
		std::cerr << "Failed to emit object file: "
		          << (emitError ? emitError : "unknown error") << std::endl;
		if (emitError) JamLLVMDisposeMessage(emitError);
		return 1;
	}

	// Link to create executable using system compiler
	std::string linkCmd = "clang " + objectFile + " -o " + outputName;
	int linkResult = system(linkCmd.c_str());
	if (linkResult != 0) {
		std::cerr << "Linking failed" << std::endl;
		return 1;
	}

	// Clean up object file
	std::remove(objectFile.c_str());

	if (testMode || runFlag) {
		// Execute the compiled program (like Zig does)
		std::string runCmd = "./" + outputName;
		int exitCode = system(runCmd.c_str());

		// Clean up executable after running
		std::remove(outputName.c_str());

// Extract actual exit code (system() returns encoded status)
#ifdef _WIN32
		return exitCode;
#else
		return WEXITSTATUS(exitCode);
#endif
	}

	std::cout << "Compilation successful: " << outputName << std::endl;
	return 0;
}

static std::vector<std::string> collectJamFiles(const std::string &dir) {
	std::vector<std::string> files;
	std::error_code ec;
	for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
	     it != std::filesystem::recursive_directory_iterator();
	     it.increment(ec)) {
		if (ec) break;
		const auto &entry = *it;
		if (entry.is_regular_file() && entry.path().extension() == ".jam") {
			files.push_back(entry.path().string());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

static void printHelp(const char *prog) {
	std::cout << "Usage: " << prog
	          << " [OPTIONS] <file|directory>\n"
	             "       "
	          << prog
	          << " test [<file|directory>]\n"
	             "\n"
	             "Options:\n"
	             "  --run           Compile, run, and clean up the executable\n"
	             "  --emit-ir       Print LLVM IR to stdout\n"
	             "  --target-info   Show host target info (arch, triple, "
	             "pointer size, ...)\n"
	             "  -o <name>       Output binary name (default: output)\n"
	             "  test, --test    Test mode: compile test functions (tfn) "
	             "and run them\n"
	             "  -h, --help      Show this help and exit\n"
	             "\n"
	             "Examples:\n"
	             "  "
	          << prog
	          << " hello.jam                 # compile to ./output\n"
	             "  "
	          << prog
	          << " --run hello.jam           # compile and run\n"
	             "  "
	          << prog
	          << " test                      # run tests in cwd (recursive)\n"
	             "  "
	          << prog
	          << " test tests/unit           # run tests under tests/unit\n"
	             "  "
	          << prog
	          << " test tests/unit/foo.jam   # run tests in a single file\n";
}

static bool fileHasTests(const std::string &path) {
	// Cheap substring scan — avoids lexing files with no `tfn` declarations.
	std::ifstream f(path);
	if (!f.is_open()) return false;
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string src = ss.str();
	// Match `tfn` at start of a line or after whitespace, followed by
	// space/ident char.
	size_t pos = 0;
	while ((pos = src.find("tfn", pos)) != std::string::npos) {
		bool startOk = (pos == 0) ||
		               std::isspace(static_cast<unsigned char>(src[pos - 1]));
		bool endOk = (pos + 3 < src.size()) &&
		             (std::isspace(static_cast<unsigned char>(src[pos + 3])) ||
		              src[pos + 3] == '(');
		if (startOk && endOk) return true;
		pos += 3;
	}
	return false;
}

int main(int argc, char *argv[]) {
	// Parse command line arguments
	bool runFlag = false;
	bool showTarget = false;
	bool emitIR = false;
	bool testMode = false;
	std::string filename;
	std::string outputName = "output";

	if (argc < 2) {
		printHelp(argv[0]);
		return 1;
	}

	// Parse flags
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			printHelp(argv[0]);
			return 0;
		} else if (arg == "--run") {
			runFlag = true;
		} else if (arg == "--target-info") {
			showTarget = true;
		} else if (arg == "--emit-ir") {
			emitIR = true;
		} else if (arg == "test" || arg == "--test") {
			testMode = true;
		} else if (arg == "-o" && i + 1 < argc) {
			outputName = argv[++i];
		} else {
			filename = arg;
			break;
		}
	}

	// `jam test` with no path means "run every test under cwd".
	if (testMode && filename.empty()) { filename = "."; }

	if (filename.empty()) {
		std::cerr << "Error: No input file specified. Run `" << argv[0]
		          << " --help` for usage." << std::endl;
		return 1;
	}

	// Get target information
	jam::Target target = jam::Target::getHostTarget();
	jam::CAbi cabi(target);

	if (showTarget) {
		std::cout << "Target Information:" << std::endl;
		std::cout << "  Name: " << target.getName() << std::endl;
		std::cout << "  Triple: " << target.toLLVMTriple() << std::endl;
		std::cout << "  Pointer size: " << target.getPointerSize() << " bytes"
		          << std::endl;
		std::cout << "  Libc: " << target.getLibCName() << std::endl;
		std::cout << "  Requires PIC: " << (target.requiresPIC() ? "yes" : "no")
		          << std::endl;
		std::cout << "  Requires PIE: " << (target.requiresPIE() ? "yes" : "no")
		          << std::endl;
		std::cout << "  Uses C ABI: " << (target.usesCabi() ? "yes" : "no")
		          << std::endl;
		std::cout << std::endl;
	}

	// Initialize LLVM using wrapper (once, even across multiple files)
	JamLLVMInitializeNativeTarget();
	JamLLVMInitializeNativeAsmPrinter();
	JamLLVMInitializeNativeAsmParser();

	// Directory input: only meaningful with `test`. Recursively discover .jam
	// files, run each one's tests, aggregate results.
	if (std::filesystem::is_directory(filename)) {
		if (!testMode) {
			std::cerr
			    << "Error: directory input is only supported with `test` (got '"
			    << filename << "')" << std::endl;
			return 1;
		}

		std::vector<std::string> files = collectJamFiles(filename);
		if (files.empty()) {
			std::cout << "No .jam files found under " << filename << std::endl;
			return 0;
		}

		int passed = 0;
		int failed = 0;
		int skipped = 0;
		for (const auto &f : files) {
			if (!fileHasTests(f)) {
				skipped++;
				continue;
			}
			std::cout << std::endl << "@" << f << std::endl;
			std::filesystem::path p(f);
			std::string perFileOutput = "jam_test_" + p.stem().string();
			int rc = compileAndRun(f, perFileOutput, runFlag, emitIR, testMode);
			if (rc != 0) failed++;
			else passed++;
		}

		std::cout << std::endl;
		std::cout << "Summary: " << passed << " file(s) passed, " << failed
		          << " file(s) failed, " << skipped
		          << " file(s) without tests, " << files.size()
		          << " file(s) scanned" << std::endl;
		return failed == 0 ? 0 : 1;
	}

	return compileAndRun(filename, outputName, runFlag, emitIR, testMode);
}
