/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef MODULE_RESOLVER_H
#define MODULE_RESOLVER_H

#include "ast.h"
#include "ast_flat.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ModuleResolver {
  public:
	// Construct with base directory and the shared type/string/node pools
	// so imported modules' types and AST nodes intern into the same pools
	// as the main one.
	ModuleResolver(const std::string &baseDir, TypePool &typePool,
	               StringPool &stringPool, NodeStore &nodeStore);

	// Resolve import path to absolute file path
	// Returns empty string if not found
	std::string resolve(const std::string &importPath) const;

	// Load and parse a module, with caching
	// Returns nullptr if module cannot be loaded
	ModuleAST *getOrLoadModule(const std::string &importPath);

	// Check if a module has been loaded
	bool isLoaded(const std::string &importPath) const;

	// Get all loaded modules
	const std::unordered_map<std::string, std::unique_ptr<ModuleAST>> &
	getLoadedModules() const;

	// Get the base directory
	const std::string &getBaseDir() const { return baseDir; }

  private:
	std::string baseDir;
	TypePool *typePool;
	StringPool *stringPool;
	NodeStore *nodeStore;
	std::unordered_map<std::string, std::unique_ptr<ModuleAST>> loadedModules;
	std::unordered_set<std::string>
	    currentlyLoading;  // For circular import detection

	// Read file contents
	std::string readFile(const std::string &path) const;

	// Parse source into AST
	std::unique_ptr<ModuleAST> parseSource(const std::string &source) const;
};

#endif  // MODULE_RESOLVER_H
