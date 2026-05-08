/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

#include "drop_registry.h"
#include "ast.h"
#include "ast_flat.h"

namespace jam {
namespace drops {

DropRegistry buildDropRegistry(const ModuleAST &module, const TypePool &types,
                               const StringPool &strings) {
	DropRegistry registry;
	for (const auto &fn : module.Functions) {
		if (fn->Name != "drop") continue;
		if (fn->Args.size() != 1) continue;
		const Param &p = fn->Args[0];
		if (p.Name != "self") continue;
		if (p.Mode != ParamMode::Mut) continue;

		// Resolve the parameter's TypeIdx to a struct name. Both Struct
		// and Named TypeKinds carry the struct's name in their `a` slot
		// as a StringIdx; either form means "drop fn for that struct".
		const TypeKey &key = types.get(p.Type);
		if (key.kind != TypeKind::Struct && key.kind != TypeKind::Named) {
			continue;
		}
		StringIdx nameIdx = static_cast<StringIdx>(key.a);
		if (nameIdx == kNoString) continue;
		const std::string &structName = strings.get(nameIdx);
		registry[structName] = fn.get();
	}
	return registry;
}

}  // namespace drops
}  // namespace jam
