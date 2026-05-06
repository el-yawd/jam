/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
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

// Charm-gum-style "dot" spinner (Braille pattern frames) drawn in pink to
// stderr while a build is running. RAII-managed: scope-guards stop the
// thread on early returns / exceptions.
static std::atomic<bool> gSpinnerActive{false};

static void runSpinner(std::string title) {
	static const char *frames[8] = {
	    "\xE2\xA3\xBE", "\xE2\xA3\xBD", "\xE2\xA3\xBB", "\xE2\xA2\xBF",
	    "\xE2\xA1\xBF", "\xE2\xA3\x9F", "\xE2\xA3\xAF", "\xE2\xA3\xB7",
	};
	int i = 0;
	while (gSpinnerActive.load()) {
		// 256-color hot pink (#FF5FAF) for the spinner glyph; default
		// foreground for the title text. \033[K clears any residual.
		std::cerr << "\r\033[38;5;205m" << frames[i] << "\033[0m " << title
		          << "\033[K" << std::flush;
		i = (i + 1) % 8;
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
	}
	std::cerr << "\r\033[K" << std::flush;
}

class SpinnerGuard {
	std::thread t;
	bool started = false;

  public:
	SpinnerGuard(bool enabled, std::string title) {
		if (!enabled) return;
		if (!isatty(STDERR_FILENO)) return;
		gSpinnerActive = true;
		t = std::thread(runSpinner, std::move(title));
		started = true;
	}
	~SpinnerGuard() { stop(); }
	void stop() {
		if (started) {
			gSpinnerActive = false;
			if (t.joinable()) t.join();
			started = false;
		}
	}
};

static int compileAndRun(const std::string &filename,
                         const std::string &outputName, bool runFlag,
                         bool emitIR, bool testMode, bool releaseMode,
                         const std::vector<std::string> &linkLibs) {
	// Show a pink dot spinner during build/run; suppress for test mode (the
	// per-test logging is its own progress indicator).
	SpinnerGuard spinner(!testMode, "Jam Making");

	std::ifstream file(filename);
	if (!file.is_open()) {
		spinner.stop();
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

	// Parse the tokens into an AST. The parser shares the codegen
	// context's TypePool / StringPool so types are interned at parse time
	// instead of being repeatedly re-parsed from strings during codegen.
	Parser parser(tokens, codegenCtx.getTypePool(),
	              codegenCtx.getStringPool(), codegenCtx.getNodeStore());
	std::unique_ptr<ModuleAST> module = parser.parse();

	// Get base directory for module resolution
	std::filesystem::path sourcePath(filename);
	std::string baseDir = sourcePath.parent_path().string();
	if (baseDir.empty()) { baseDir = "."; }

	// Create module resolver (sharing the codegen pools so imports intern
	// into the same TypePool / StringPool as the main module).
	ModuleResolver resolver(baseDir, codegenCtx.getTypePool(),
	                        codegenCtx.getStringPool(),
	                        codegenCtx.getNodeStore());
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

	// Register struct types from imported modules and main module first so
	// that codegen has the struct registry available. Two phases so structs
	// can reference each other regardless of declaration order.
	auto declareStructs = [&](ModuleAST *m) {
		for (auto &s : m->Structs) {
			JamTypeRef structType = JamLLVMStructCreateNamed(
			    codegenCtx.getContext(), s->Name.c_str());
			codegenCtx.registerStruct(s->Name, structType, s->Fields);
		}
	};
	auto declareUnions = [&](ModuleAST *m) {
		for (auto &u : m->Unions) {
			JamTypeRef unionType = JamLLVMStructCreateNamed(
			    codegenCtx.getContext(), u->Name.c_str());
			codegenCtx.registerUnion(u->Name, unionType, u->Fields);
		}
	};
	auto fillStructBodies = [&](ModuleAST *m) {
		for (auto &s : m->Structs) {
			std::vector<JamTypeRef> fieldTypes;
			fieldTypes.reserve(s->Fields.size());
			for (auto &f : s->Fields) {
				fieldTypes.push_back(codegenCtx.getLLVMType(f.second));
			}
			const auto *info = codegenCtx.getStruct(s->Name);
			JamLLVMStructSetBody(info->type, fieldTypes.data(),
			                     static_cast<unsigned>(fieldTypes.size()),
			                     false);
		}
	};
	// Lay out a union as `{ alignedField, [paddingBytes x i8] }`. The
	// alignedField is the field with the largest alignment requirement;
	// padding makes up the difference between that field's size and the
	// largest field's size, so the union ends up with the right size and
	// alignment for any field's stored value.
	auto fillUnionBodies = [&](ModuleAST *m) {
		for (auto &u : m->Unions) {
			if (u->Fields.empty()) {
				throw std::runtime_error(
				    "Union `" + u->Name + "` must have at least one field");
			}
			uint64_t maxSize = 0, maxAlign = 1;
			size_t alignFieldIdx = 0;
			for (size_t i = 0; i < u->Fields.size(); i++) {
				uint64_t sz = codegenCtx.typeSize(u->Fields[i].second);
				uint64_t al = codegenCtx.typeAlign(u->Fields[i].second);
				if (sz > maxSize) maxSize = sz;
				if (al > maxAlign) {
					maxAlign = al;
					alignFieldIdx = i;
				}
			}
			// Round size up to a multiple of alignment so writes through
			// the most-aligned field don't run off the end.
			uint64_t allocSize =
			    (maxSize + maxAlign - 1) / maxAlign * maxAlign;
			JamTypeRef alignedTy =
			    codegenCtx.getLLVMType(u->Fields[alignFieldIdx].second);
			uint64_t alignedSz =
			    codegenCtx.typeSize(u->Fields[alignFieldIdx].second);
			uint64_t paddingBytes =
			    allocSize > alignedSz ? allocSize - alignedSz : 0;

			std::vector<JamTypeRef> bodyTypes;
			bodyTypes.push_back(alignedTy);
			if (paddingBytes > 0) {
				bodyTypes.push_back(JamLLVMArrayType(
				    codegenCtx.getInt8Type(),
				    static_cast<unsigned>(paddingBytes)));
			}
			const auto *info = codegenCtx.getUnion(u->Name);
			JamLLVMStructSetBody(info->type, bodyTypes.data(),
			                     static_cast<unsigned>(bodyTypes.size()),
			                     false);
		}
	};
	auto declareEnums = [&](ModuleAST *m) {
		for (auto &e : m->Enums) {
			std::vector<JamCodegenContext::EnumVariantInfo> variants;
			variants.reserve(e->Variants.size());
			for (auto &v : e->Variants) {
				JamCodegenContext::EnumVariantInfo info;
				info.name = v.Name;
				info.payloadTypes = v.PayloadTypes;
				info.discriminant = v.Discriminant;
				variants.push_back(std::move(info));
			}
			codegenCtx.registerEnum(e->Name, std::move(variants));
		}
	};
	// For payloaded enums, lay out as `{i8 tag, alignDriver,
	// [extraBytes x i8]}` where `alignDriver` is the smallest scalar
	// type whose alignment matches the strictest variant's alignment
	// (i8 / i16 / i32 / i64 for align 1 / 2 / 4 / 8 respectively).
	// LLVM gives the resulting struct the alignment of `alignDriver`,
	// which propagates to allocas and stores — without that we get
	// align-1 enum slots even for u64-payload variants, which forces
	// LLVM to emit unaligned memory ops.
	//
	// `extraBytes` makes up the difference between the largest variant's
	// payload size and the alignDriver scalar's size, rounded up to
	// maxAlign so the trailing slot in an array of enums is correctly
	// aligned.
	auto fillEnumBodies = [&](ModuleAST *m) {
		for (auto &e : m->Enums) {
			const auto *info = codegenCtx.getEnum(e->Name);
			if (!info || !info->hasPayloadVariant) continue;

			uint64_t maxSize = 0, maxAlign = 1;
			for (const auto &v : info->variants) {
				uint64_t off = 0, varAlign = 1;
				for (TypeIdx t : v.payloadTypes) {
					uint64_t s = codegenCtx.typeSize(t);
					uint64_t a = codegenCtx.typeAlign(t);
					off = (off + a - 1) / a * a;  // align this field
					off += s;
					if (a > varAlign) varAlign = a;
				}
				if (varAlign > 1) {
					off = (off + varAlign - 1) / varAlign * varAlign;
				}
				if (off > maxSize) maxSize = off;
				if (varAlign > maxAlign) maxAlign = varAlign;
			}

			// Pick a scalar to drive struct alignment.
			JamTypeRef alignDriver;
			uint64_t alignDriverSize;
			switch (maxAlign) {
			case 1:
				alignDriver = codegenCtx.getInt8Type();
				alignDriverSize = 1;
				break;
			case 2:
				alignDriver = codegenCtx.getInt16Type();
				alignDriverSize = 2;
				break;
			case 4:
				alignDriver = codegenCtx.getInt32Type();
				alignDriverSize = 4;
				break;
			case 8:
				alignDriver = codegenCtx.getInt64Type();
				alignDriverSize = 8;
				break;
			default:
				throw std::runtime_error(
				    "Enum `" + e->Name +
				    "` requires alignment > 8, which is not yet "
				    "supported");
			}

			// Round payload size up to maxAlign so a contiguous array
			// of enums tiles correctly.
			uint64_t paddedSize =
			    (maxSize + maxAlign - 1) / maxAlign * maxAlign;
			uint64_t extraBytes = (paddedSize > alignDriverSize)
			                          ? paddedSize - alignDriverSize
			                          : 0;

			std::vector<JamTypeRef> bodyTypes;
			bodyTypes.push_back(codegenCtx.getInt8Type());  // tag
			bodyTypes.push_back(alignDriver);
			if (extraBytes > 0) {
				bodyTypes.push_back(JamLLVMArrayType(
				    codegenCtx.getInt8Type(),
				    static_cast<unsigned>(extraBytes)));
			}

			JamLLVMStructSetBody(info->type, bodyTypes.data(),
			                     static_cast<unsigned>(bodyTypes.size()),
			                     false);
			codegenCtx.setEnumLLVMType(e->Name, info->type, maxSize,
			                           maxAlign, true);
		}
	};
	// Enums that need a named struct type (i.e. those with payload
	// variants) get their LLVM type created here, in declareEnums, so
	// that fillEnumBodies can set the body in a second pass.
	auto declareEnumLLVMTypes = [&](ModuleAST *m) {
		for (auto &e : m->Enums) {
			const auto *info = codegenCtx.getEnum(e->Name);
			if (!info || !info->hasPayloadVariant) continue;
			JamTypeRef ty = JamLLVMStructCreateNamed(
			    codegenCtx.getContext(), e->Name.c_str());
			codegenCtx.setEnumLLVMType(e->Name, ty, 0, 1, true);
		}
	};
	for (const auto &[path, importedModule] : resolver.getLoadedModules()) {
		if (path == "std") continue;
		declareStructs(importedModule.get());
		declareUnions(importedModule.get());
		declareEnums(importedModule.get());
		declareEnumLLVMTypes(importedModule.get());
	}
	declareStructs(module.get());
	declareUnions(module.get());
	declareEnums(module.get());
	declareEnumLLVMTypes(module.get());
	for (const auto &[path, importedModule] : resolver.getLoadedModules()) {
		if (path == "std") continue;
		fillStructBodies(importedModule.get());
		fillUnionBodies(importedModule.get());
		fillEnumBodies(importedModule.get());
	}
	fillStructBodies(module.get());
	fillUnionBodies(module.get());
	fillEnumBodies(module.get());

	// Two-pass codegen: declare every function's prototype first, then
	// emit bodies. Without this, calling a function defined later in the
	// file (or another module) would fail with "Unknown function". This
	// is also how Zig handles forward references — top-down reads naturally
	// (main on top, helpers below) without a manual "forward declarations"
	// section.

	// Pass 1a: prototypes for pub functions in imported modules.
	for (const auto &[path, importedModule] : resolver.getLoadedModules()) {
		if (path == "std") continue;
		for (auto &func : importedModule->Functions) {
			if (func->isPub) { func->declarePrototype(codegenCtx); }
		}
	}

	// Pass 1b: prototypes for the main module's functions (we still skip
	// test funcs in non-test mode and user `main` in test mode).
	std::vector<std::string> testFunctionNames;
	std::vector<FunctionAST *> mainModuleEmits;
	for (auto &function : module->Functions) {
		if (function->isTest && !testMode) continue;
		if (!function->isTest && testMode && function->Name == "main") {
			continue;
		}
		if (function->isTest) {
			testFunctionNames.push_back("__test_" + function->Name);
		}
		function->declarePrototype(codegenCtx);
		mainModuleEmits.push_back(function.get());
	}

	// Pass 2a: bodies for pub functions in imported modules.
	for (const auto &[path, importedModule] : resolver.getLoadedModules()) {
		if (path == "std") continue;
		for (auto &func : importedModule->Functions) {
			if (func->isPub) { func->defineBody(codegenCtx); }
		}
	}

	// Pass 2b: bodies for the main module's functions.
	for (FunctionAST *function : mainModuleEmits) {
		function->defineBody(codegenCtx);
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

	// Create target machine. Default to JAM_OPT_NONE (Zig Debug-equivalent)
	// — LLVM's machine codegen at -O2 dominates compile time. Debug builds
	// drop compile time roughly 30×; `--release` opts into -O3.
	JamTargetMachineRef tm =
	    JamLLVMCreateTargetMachine(tripleStr, "generic", "",
	                               false,  // not PIC for now
	                               releaseMode ? JAM_OPT_AGGRESSIVE
	                                           : JAM_OPT_NONE);
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

	// Link to create executable using system compiler. Append any -l flags
	// the user passed (`-lncurses`, `-l ncurses`, `--library ncurses`) so
	// extern fns from system libraries resolve.
	std::string linkCmd = "clang " + objectFile + " -o " + outputName;
	for (const auto &lib : linkLibs) {
		linkCmd += " -l" + lib;
	}
	int linkResult = system(linkCmd.c_str());
	if (linkResult != 0) {
		std::cerr << "Linking failed" << std::endl;
		return 1;
	}

	// Clean up object file
	std::remove(objectFile.c_str());

	// Stop the spinner thread before either handing the terminal to the
	// child process or printing the success line — otherwise the spinner
	// would keep redrawing on top of whatever the program prints.
	spinner.stop();

	if (testMode || runFlag) {
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
	          << " run [LINKER-FLAGS] <file>\n"
	             "       "
	          << prog
	          << " test [<file|directory>]\n"
	             "\n"
	             "Subcommands:\n"
	             "  run             Compile, run, and clean up the executable. "
	             "Only linker\n"
	             "                  flags (-l<name>) may accompany it.\n"
	             "  test            Test mode: compile test functions and "
	             "run them\n"
	             "\n"
	             "Options:\n"
	             "  --release       Optimized build (LLVM -O3). Default is "
	             "debug (no opts,\n"
	             "                  ~30× faster compile).\n"
	             "  --emit-ir       Print LLVM IR to stdout\n"
	             "  --target-info   Show host target info (arch, triple, "
	             "pointer size, ...)\n"
	             "  -o <name>       Output binary name (default: output)\n"
	             "  -l<name>, --library <name>\n"
	             "                  Link against system library <name>\n"
	             "  -h, --help      Show this help and exit\n"
	             "\n"
	             "Examples:\n"
	             "  "
	          << prog
	          << " hello.jam                 # compile to ./output\n"
	             "  "
	          << prog
	          << " run hello.jam             # compile and run\n"
	             "  "
	          << prog
	          << " run -lncurses tetris.jam  # compile, link with ncurses, run\n"
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
	bool releaseMode = false;
	std::string filename;
	std::string outputName = "output";
	std::vector<std::string> linkLibs;

	if (argc < 2) {
		printHelp(argv[0]);
		return 1;
	}

	// Parse subcommand + flags. `run` and `test` are subcommands; everything
	// else is either a flag or the filename. When `run` is in effect, only
	// linker flags (`-l<name>`, `-l <name>`, `--library <name>`) are
	// permitted alongside it.
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "run") {
			runFlag = true;
			continue;
		}
		if (arg == "test") {
			testMode = true;
			continue;
		}
		// Linker flags — accepted in every mode.
		if ((arg == "-l" || arg == "--library") && i + 1 < argc) {
			linkLibs.push_back(argv[++i]);
			continue;
		}
		if (arg.length() > 2 && arg.substr(0, 2) == "-l") {
			linkLibs.push_back(arg.substr(2));
			continue;
		}
		// --release toggles LLVM optimizations (off by default — debug
		// builds compile ~30× faster). Accepted in every mode.
		if (arg == "--release") {
			releaseMode = true;
			continue;
		}
		// Inside `run`, anything else flag-shaped is an error.
		if (runFlag) {
			if (!arg.empty() && arg[0] == '-') {
				std::cerr
				    << "Error: `run` only accepts linker flags "
				       "(-l<name>, -l <name>, --library <name>); got `"
				    << arg << "`" << std::endl;
				return 1;
			}
			filename = arg;
			break;
		}
		// Compile-only / test-mode flags.
		if (arg == "--help" || arg == "-h") {
			printHelp(argv[0]);
			return 0;
		}
		if (arg == "--target-info") {
			showTarget = true;
			continue;
		}
		if (arg == "--emit-ir") {
			emitIR = true;
			continue;
		}
		if (arg == "-o" && i + 1 < argc) {
			outputName = argv[++i];
			continue;
		}
		filename = arg;
		break;
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
			int rc = compileAndRun(f, perFileOutput, runFlag, emitIR, testMode,
			                       releaseMode, linkLibs);
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

	return compileAndRun(filename, outputName, runFlag, emitIR, testMode,
	                     releaseMode, linkLibs);
}
