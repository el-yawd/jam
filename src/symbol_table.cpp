/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "symbol_table.h"
#include <iostream>

void SymbolTable::registerModule(const std::string &modulePath,
                                 ModuleAST *module) {
	if (!module) return;

	auto &symbols = moduleSymbols[modulePath];

	for (auto &func : module->Functions) {
		// Only register pub functions as exportable
		if (func->isPub) {
			ExportedSymbol sym;
			sym.name = func->Name;
			sym.modulePath = modulePath;
			sym.function = func.get();
			sym.isPub = true;
			symbols[func->Name] = sym;
		}
	}
}

void SymbolTable::registerBuiltinSymbol(const std::string &modulePath,
                                        const std::string &name) {
	ExportedSymbol sym;
	sym.name = name;
	sym.modulePath = modulePath;
	sym.function = nullptr;
	sym.isPub = true;
	moduleSymbols[modulePath][name] = sym;
}

const ExportedSymbol *
SymbolTable::lookupByModule(const std::string &modulePath,
                            const std::string &name) const {
	auto modIt = moduleSymbols.find(modulePath);
	if (modIt == moduleSymbols.end()) { return nullptr; }

	auto symIt = modIt->second.find(name);
	if (symIt == modIt->second.end()) { return nullptr; }

	return &symIt->second;
}

const ExportedSymbol *SymbolTable::lookupByName(const std::string &name) const {
	// Search all modules for this symbol
	for (const auto &modPair : moduleSymbols) {
		auto symIt = modPair.second.find(name);
		if (symIt != modPair.second.end()) { return &symIt->second; }
	}
	return nullptr;
}

void SymbolTable::registerBinding(const std::string &localName,
                                  const std::string &modulePath,
                                  const std::string &symbolName) {
	bindings[localName] = {modulePath, symbolName};
}

const ExportedSymbol *
SymbolTable::lookupBinding(const std::string &localName) const {
	auto it = bindings.find(localName);
	if (it == bindings.end()) { return nullptr; }

	return lookupByModule(it->second.first, it->second.second);
}

bool SymbolTable::hasSymbol(const std::string &modulePath,
                            const std::string &name) const {
	return lookupByModule(modulePath, name) != nullptr;
}

std::vector<const ExportedSymbol *>
SymbolTable::getModuleSymbols(const std::string &modulePath) const {
	std::vector<const ExportedSymbol *> result;

	auto modIt = moduleSymbols.find(modulePath);
	if (modIt != moduleSymbols.end()) {
		for (const auto &symPair : modIt->second) {
			result.push_back(&symPair.second);
		}
	}

	return result;
}
