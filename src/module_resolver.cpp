/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "module_resolver.h"
#include "lexer.h"
#include "parser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

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

	fs::path stdPath = fs::path("std") / (path + ".jam");
	if (fs::exists(stdPath) && fs::is_regular_file(stdPath)) {
		return fs::canonical(stdPath).string();
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
