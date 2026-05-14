/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "module_resolver.h"
#include "lexer.h"
#include "parser.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <climits>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string g_stdPathOverride;

// Locate the running jam executable's filesystem path, with symlinks
// resolved. Returns "" on platforms we don't handle; callers must
// gracefully fall back to other lookups.
std::string getExecutablePath() {
#if defined(__APPLE__)
	char buf[PATH_MAX];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) != 0) return "";
	char real[PATH_MAX];
	if (realpath(buf, real) != nullptr) return std::string(real);
	return std::string(buf);
#elif defined(__linux__)
	char buf[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len <= 0) return "";
	buf[len] = '\0';
	return std::string(buf);
#else
	return "";
#endif
}

// Compute the standard-library root once per process. Order:
//   1. `--std-path <path>` CLI flag (via setStdPathOverride).
//   2. `JAM_STD_PATH` env var — used as-is when non-empty.
//   3. `<bindir>/../lib/jam/std` — binary-relative install layout
//      (matches `$PREFIX/lib/jam/std` shipped by `make install`).
// Returns "" when none are available; the resolver then falls back
// to the in-tree CWD `std/` lookup so dev workflows still work.
const std::string &stdRoot() {
	static const std::string root = []() -> std::string {
		if (!g_stdPathOverride.empty()) return g_stdPathOverride;
		if (const char *env = std::getenv("JAM_STD_PATH")) {
			if (env[0] != '\0') return std::string(env);
		}
		std::string exe = getExecutablePath();
		if (exe.empty()) return "";
		fs::path binDir = fs::path(exe).parent_path();
		fs::path candidate = binDir / ".." / "lib" / "jam" / "std";
		std::error_code ec;
		fs::path canonical = fs::weakly_canonical(candidate, ec);
		if (ec) return "";
		if (fs::exists(canonical) && fs::is_directory(canonical)) {
			return canonical.string();
		}
		return "";
	}();
	return root;
}

}  // namespace

void setStdPathOverride(const std::string &path) { g_stdPathOverride = path; }

ModuleResolver::ModuleResolver(const std::string &baseDir, TypePool &typePool_,
                               StringPool &stringPool_, NodeStore &nodeStore_)
    : baseDir(baseDir), typePool(&typePool_), stringPool(&stringPool_),
      nodeStore(&nodeStore_) {}

std::string ModuleResolver::resolve(const std::string &importPath) const {
	if (importPath == "std" || importPath == "test") { return importPath; }
	std::string path = importPath;

	if (path.substr(0, 2) == "./") { path = path.substr(2); }

	fs::path base(baseDir);
	fs::path directPath = base / (path + ".jam");
	if (fs::exists(directPath) && fs::is_regular_file(directPath)) {
		return fs::canonical(directPath).string();
	}

	fs::path indexPath = base / path / "mod.jam";
	if (fs::exists(indexPath) && fs::is_regular_file(indexPath)) {
		return fs::canonical(indexPath).string();
	}

	// Standard-library lookup. Accept both `import("collections")` and
	// `import("std/collections")` spellings by stripping a leading
	// `std/` so the bare module name resolves under the std root.
	std::string stdPath = path;
	if (stdPath.rfind("std/", 0) == 0) { stdPath = stdPath.substr(4); }

	const std::string &root = stdRoot();
	if (!root.empty()) {
		fs::path fileCandidate = fs::path(root) / (stdPath + ".jam");
		if (fs::exists(fileCandidate) && fs::is_regular_file(fileCandidate)) {
			return fs::canonical(fileCandidate).string();
		}
		fs::path indexCandidate = fs::path(root) / stdPath / "mod.jam";
		if (fs::exists(indexCandidate) && fs::is_regular_file(indexCandidate)) {
			return fs::canonical(indexCandidate).string();
		}
	}

	// In-tree dev fallback: `<CWD>/std/<path>.jam`. Lets a fresh build
	// of jam.out run unit tests without first installing the std lib.
	fs::path devPath = fs::path("std") / (stdPath + ".jam");
	if (fs::exists(devPath) && fs::is_regular_file(devPath)) {
		return fs::canonical(devPath).string();
	}

	return "";  // Not found
}

std::string ModuleResolver::readFile(const std::string &path) const {
	std::ifstream file(path);
	if (!file.is_open()) { return ""; }
	std::stringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

std::unique_ptr<ModuleAST>
ModuleResolver::parseSource(const std::string &source) const {
	Lexer lexer(source);
	std::vector<Token> tokens = lexer.scanTokens();
	Parser parser(tokens, *typePool, *stringPool, *nodeStore);
	parser.sharedAnonStructs = sharedAnonStructs_;
	parser.sharedAnonEnums = sharedAnonEnums_;
	return parser.parse();
}

ModuleAST *ModuleResolver::getOrLoadModule(const std::string &importPath) {
	auto it = loadedModules.find(importPath);
	if (it != loadedModules.end()) { return it->second.get(); }

	if (currentlyLoading.count(importPath) > 0) {
		std::cerr << "Error: Circular import detected for module: "
		          << importPath << std::endl;
		return nullptr;
	}

	std::string resolvedPath = resolve(importPath);
	if (resolvedPath.empty()) {
		std::cerr << "Error: Cannot resolve import path: " << importPath
		          << std::endl;
		return nullptr;
	}

	if (resolvedPath == "std" || resolvedPath == "test") {
		auto builtinModule = std::make_unique<ModuleAST>();
		loadedModules[importPath] = std::move(builtinModule);
		return loadedModules[importPath].get();
	}

	currentlyLoading.insert(importPath);
	std::string source = readFile(resolvedPath);
	if (source.empty()) {
		std::cerr << "Error: Cannot read module file: " << resolvedPath
		          << std::endl;
		currentlyLoading.erase(importPath);
		return nullptr;
	}

	auto module = parseSource(source);
	if (!module) {
		std::cerr << "Error: Failed to parse module: " << resolvedPath
		          << std::endl;
		currentlyLoading.erase(importPath);
		return nullptr;
	}

	for (const auto &import : module->Imports) {
		if (import->Path != "std") {
			fs::path modulePath(resolvedPath);
			std::string moduleDir = modulePath.parent_path().string();
			ModuleResolver nestedResolver(moduleDir, *typePool, *stringPool,
			                              *nodeStore);

			std::string nestedResolved = nestedResolver.resolve(import->Path);
			if (!nestedResolved.empty() && nestedResolved != "std") {
				getOrLoadModule(import->Path);
			}
		}
	}

	currentlyLoading.erase(importPath);
	loadedModules[importPath] = std::move(module);
	return loadedModules[importPath].get();
}

bool ModuleResolver::isLoaded(const std::string &importPath) const {
	return loadedModules.find(importPath) != loadedModules.end();
}

const std::unordered_map<std::string, std::unique_ptr<ModuleAST>> &
ModuleResolver::getLoadedModules() const {
	return loadedModules;
}
