/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "ast.h"
#include <string>
#include <unordered_map>
#include <vector>

// Represents an exported symbol from a module
struct ExportedSymbol {
	std::string name;        // Function name
	std::string modulePath;  // Module it came from
	FunctionAST *function;   // Pointer to the function AST
	bool isPub;              // Is it a pub function?
};

class SymbolTable {
  public:
	SymbolTable() = default;

	// Register a module's exported symbols
	void registerModule(const std::string &modulePath, ModuleAST *module);

	// Look up a symbol by module path and name (e.g., "math", "add")
	const ExportedSymbol *lookupByModule(const std::string &modulePath,
	                                     const std::string &name) const;

	// Look up a symbol by direct name (for destructuring imports)
	// Returns the first matching symbol
	const ExportedSymbol *lookupByName(const std::string &name) const;

	// Register a direct binding (from destructuring import)
	// Maps a local name to a module's symbol
	void registerBinding(const std::string &localName,
	                     const std::string &modulePath,
	                     const std::string &symbolName);

	// Look up a bound symbol
	const ExportedSymbol *lookupBinding(const std::string &localName) const;

	// Register a builtin symbol (no AST function)
	void registerBuiltinSymbol(const std::string &modulePath,
	                           const std::string &name);

	// Check if a symbol exists in a module
	bool hasSymbol(const std::string &modulePath,
	               const std::string &name) const;

	// Get all symbols from a module
	std::vector<const ExportedSymbol *>
	getModuleSymbols(const std::string &modulePath) const;

  private:
	// Module path -> (symbol name -> ExportedSymbol)
	std::unordered_map<std::string,
	                   std::unordered_map<std::string, ExportedSymbol>>
	    moduleSymbols;

	// Direct bindings: local name -> (module path, symbol name)
	std::unordered_map<std::string, std::pair<std::string, std::string>>
	    bindings;
};

#endif  // SYMBOL_TABLE_H
