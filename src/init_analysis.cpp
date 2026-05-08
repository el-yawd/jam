/*
 * Copyright (c) 2026-present Raphael Amorim
 *
 * This file is part of jam.
 * Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
 */

// Definite-initialization analysis for Jam (MVS phase P2).
//
// A tree-walking dataflow over the function body produces, at each program
// point, a map from binding name to lattice state {Unknown, Init, Uninit,
// MaybeInit}. The lattice is the same one Clang's UninitializedValues pass
// uses (clang/lib/Analysis/UninitializedValues.cpp): two bits per binding,
// merge by bitwise OR.
//
// Jam programs are structured (no goto, no break-out-of-multiple-levels),
// so a recursive analyzer over the AST suffices — there is no separate
// CFG-construction pass. Branches fork and merge in lockstep with the
// `if` / `match` AST shape; loops merge their body output back with the
// pre-loop state.
//
// P2 scope (this file):
//   - Tracks state for locals declared `var name: T = ...`.
//   - Function parameters enter as Init (modes are applied in P3).
//   - Detects reads of Uninit / MaybeInit Variable nodes.
//   - if/else: precise state merge.
//   - while/for/match: single-pass merge of body output with pre-state
//     (sound for most patterns; refine to fixed-point if needed).
//
// Out of P2 scope (deferred):
//   - Field-level init tracking (struct.field, array[i] partial init).
//   - Mode-aware parameter entry states.
//   - `move` / `undefined` callsite mode propagation.
//   - Drop synthesis driven by the live-init set.
//
// See docs/MVS.md §5 (Definite Initialization) and §8.3 (P2 phase).

#include "init_analysis.h"
#include "ast.h"
#include <utility>

namespace jam {
namespace init_analysis {

namespace {

using NameMap = std::unordered_map<std::string, InitState>;

// Result of analyzing a statement or expression.
//
//   state       — the binding states at the statement's exit
//   terminated  — true ⇒ control flow does not reach the next statement
//                 (the statement was a return/break/continue or contained
//                 such an exit on every path)
struct Result {
	NameMap state;
	bool terminated = false;
};

class Analyzer {
  public:
	Analyzer(const NodeStore &nodes, const StringPool &strings,
	         const std::vector<Token> &tokens)
	    : nodes_(nodes), strings_(strings), tokens_(tokens) {}

	std::vector<Diagnostic> run(const FunctionAST &fn);

  private:
	Result analyze(NodeIdx idx, NameMap state);
	Result analyzeVarDecl(NodeIdx idx, NameMap state);
	Result analyzeAssign(NodeIdx idx, NameMap state);
	Result analyzeAssignTarget(NodeIdx idx, NameMap state);
	Result analyzeIf(NodeIdx idx, NameMap state);
	Result analyzeWhile(NodeIdx idx, NameMap state);
	Result analyzeFor(NodeIdx idx, NameMap state);
	Result analyzeMatch(NodeIdx idx, NameMap state);
	Result analyzeReturn(NodeIdx idx, NameMap state);
	Result analyzeCall(NodeIdx idx, NameMap state);
	Result analyzeStructLit(NodeIdx idx, NameMap state);

	void checkVariableRead(NodeIdx idx, const NameMap &state);
	void emitError(std::string message, NodeIdx anchor, std::string varName);
	int lineOf(NodeIdx idx) const;

	static NameMap mergeMaps(const NameMap &a, const NameMap &b);

	const NodeStore &nodes_;
	const StringPool &strings_;
	const std::vector<Token> &tokens_;
	std::vector<Diagnostic> diagnostics_;
};

// --------------------------------------------------------------------------
// Entry point.
// --------------------------------------------------------------------------

std::vector<Diagnostic> Analyzer::run(const FunctionAST &fn) {
	NameMap state;
	// P2: parameters enter as Init regardless of declared mode. P3 will
	// honor `undefined`-mode parameters by entering as Uninit.
	for (const Param &p : fn.Args) { state[p.Name] = InitState::Init; }

	Result r{std::move(state), false};
	for (NodeIdx stmt : fn.Body) {
		if (r.terminated) {
			// Statements after an unconditional return/break/continue are
			// unreachable. Don't analyze them — they would emit spurious
			// errors against state we lost track of.
			break;
		}
		r = analyze(stmt, std::move(r.state));
	}

	return std::move(diagnostics_);
}

// --------------------------------------------------------------------------
// Tag dispatch.
// --------------------------------------------------------------------------

Result Analyzer::analyze(NodeIdx idx, NameMap state) {
	if (idx == kNoNode) return Result{std::move(state), false};
	const AstNode &n = nodes_.get(idx);

	switch (n.tag) {
	// Control flow.
	case AstTag::VarDecl:
		return analyzeVarDecl(idx, std::move(state));
	case AstTag::Assign:
		return analyzeAssign(idx, std::move(state));
	case AstTag::IfNode:
		return analyzeIf(idx, std::move(state));
	case AstTag::WhileNode:
		return analyzeWhile(idx, std::move(state));
	case AstTag::ForNode:
		return analyzeFor(idx, std::move(state));
	case AstTag::MatchNode:
		return analyzeMatch(idx, std::move(state));
	case AstTag::Return:
		return analyzeReturn(idx, std::move(state));
	case AstTag::Break:
	case AstTag::Continue:
		// Both terminate the current straight-line path. The enclosing
		// loop's caller is responsible for reasoning about loop exits.
		return Result{std::move(state), true};

	// Expressions that may read variables.
	case AstTag::Variable:
		checkVariableRead(idx, state);
		return Result{std::move(state), false};
	case AstTag::Call:
		return analyzeCall(idx, std::move(state));
	case AstTag::BinaryOp: {
		auto r = analyze(n.lhs, std::move(state));
		if (r.terminated) return r;
		return analyze(n.rhs, std::move(r.state));
	}
	case AstTag::UnaryOp:
		return analyze(n.lhs, std::move(state));
	case AstTag::MemberAccess:
		// Walk the base. We treat `s.f` as reading `s` (the binding),
		// not the field — P2 doesn't track field-level init.
		return analyze(n.lhs, std::move(state));
	case AstTag::Index: {
		auto r = analyze(n.lhs, std::move(state));
		if (r.terminated) return r;
		return analyze(n.rhs, std::move(r.state));
	}
	case AstTag::Deref:
		// `p.*` — read the pointer. (Whether the pointee was initialized
		// is a separate concern P2 does not track.)
		return analyze(n.lhs, std::move(state));
	case AstTag::AddressOf:
		// `&x` — taking an address does not read the binding's value.
		// Ignore the operand for read-check purposes. P2 imprecisely
		// allows `&x` even when `x` is Uninit; pointer-write through it
		// is a separate analysis layer.
		return Result{std::move(state), false};
	case AstTag::AsCast:
		return analyze(n.lhs, std::move(state));
	case AstTag::StructLit:
		return analyzeStructLit(idx, std::move(state));

	// Literals — no init effect on bindings.
	case AstTag::NumberLit:
	case AstTag::BoolLit:
	case AstTag::StringLit:
	case AstTag::UndefinedLit:
	case AstTag::ImportLit:
		return Result{std::move(state), false};

	// Pattern atoms appear only inside MatchNode arms; they don't read
	// bindings (M1's patterns are integer literals / ranges / wildcards).
	case AstTag::PatLit:
	case AstTag::PatRange:
	case AstTag::PatWildcard:
	case AstTag::PatOr:
	case AstTag::PatEnumVariant:
		return Result{std::move(state), false};

	case AstTag::Invalid:
	case AstTag::Count:
		return Result{std::move(state), false};
	}

	return Result{std::move(state), false};
}

// --------------------------------------------------------------------------
// Statement / declaration cases.
// --------------------------------------------------------------------------

Result Analyzer::analyzeVarDecl(NodeIdx idx, NameMap state) {
	// VarDecl: d.lhs = ExtraIdx → [StringIdx name, TypeIdx type, NodeIdx init]
	const AstNode &n = nodes_.get(idx);
	ExtraIdx extra = n.lhs;
	StringIdx nameIdx = nodes_.getExtra(extra + 0);
	NodeIdx initIdx = nodes_.getExtra(extra + 2);
	const std::string &name = strings_.get(nameIdx);

	if (initIdx == kNoNode) {
		// Should not happen — Jam syntactically requires an initializer
		// for `var` declarations. Treat as Uninit defensively.
		state[name] = InitState::Uninit;
		return Result{std::move(state), false};
	}

	const AstNode &initNode = nodes_.get(initIdx);
	if (initNode.tag == AstTag::UndefinedLit) {
		state[name] = InitState::Uninit;
		return Result{std::move(state), false};
	}

	auto r = analyze(initIdx, std::move(state));
	if (r.terminated) return r;
	r.state[name] = InitState::Init;
	return r;
}

Result Analyzer::analyzeAssign(NodeIdx idx, NameMap state) {
	const AstNode &n = nodes_.get(idx);
	// Right-hand side is evaluated first (Rust semantics: drop old, then
	// store new). Sub-expressions of the RHS may read other bindings.
	auto r = analyze(n.rhs, std::move(state));
	if (r.terminated) return r;
	return analyzeAssignTarget(n.lhs, std::move(r.state));
}

// Walk an lvalue (assignment target). The "base binding" — the leftmost
// Variable in the path — is being *written to*, not read, so it does not
// trigger a read check. Sub-expressions like array indices *are* reads.
//
// In P2, any write to a sub-path (`arr[i] = x`, `s.field = x`) marks the
// whole base binding as Init. This is intentionally imprecise: P2 does
// not track field-level init. A later phase can refine this to only mark
// the touched sub-path.
Result Analyzer::analyzeAssignTarget(NodeIdx idx, NameMap state) {
	if (idx == kNoNode) return Result{std::move(state), false};
	const AstNode &n = nodes_.get(idx);
	switch (n.tag) {
	case AstTag::Variable: {
		StringIdx nameIdx = n.lhs;
		const std::string &name = strings_.get(nameIdx);
		state[name] = InitState::Init;
		return Result{std::move(state), false};
	}
	case AstTag::MemberAccess:
		// `s.f = ...` — recurse on `s` as the lvalue base.
		return analyzeAssignTarget(n.lhs, std::move(state));
	case AstTag::Index: {
		// `a[i] = ...` — `a` is the lvalue base; `i` is read normally.
		auto r = analyze(n.rhs, std::move(state));
		if (r.terminated) return r;
		return analyzeAssignTarget(n.lhs, std::move(r.state));
	}
	case AstTag::Deref:
		// `p.* = ...` — we read `p` (the pointer) to know where to write.
		// The pointee is not a binding we track at the source level.
		return analyze(n.lhs, std::move(state));
	default:
		// Unexpected lvalue shape — fall back to a normal walk so we at
		// least surface read-checks on any sub-expressions.
		return analyze(idx, std::move(state));
	}
}

Result Analyzer::analyzeIf(NodeIdx idx, NameMap state) {
	// IfNode: d.lhs = NodeIdx (cond)
	//         d.rhs = ExtraIdx → [thenCount, elseCount, then0, ..., else0, ...]
	const AstNode &n = nodes_.get(idx);
	auto r = analyze(n.lhs, std::move(state));
	if (r.terminated) return r;

	ExtraIdx extra = n.rhs;
	uint32_t thenCount = nodes_.getExtra(extra + 0);
	uint32_t elseCount = nodes_.getExtra(extra + 1);

	NameMap stateBeforeBranch = r.state;  // copy for the else path

	// Then-branch: walks under thenR, reusing r.state to avoid an extra copy.
	Result thenR{std::move(r.state), false};
	for (uint32_t i = 0; i < thenCount; i++) {
		if (thenR.terminated) break;
		NodeIdx s = nodes_.getExtra(extra + 2 + i);
		thenR = analyze(s, std::move(thenR.state));
	}

	// Else-branch: starts from the saved pre-branch state.
	Result elseR{std::move(stateBeforeBranch), false};
	for (uint32_t i = 0; i < elseCount; i++) {
		if (elseR.terminated) break;
		NodeIdx s = nodes_.getExtra(extra + 2 + thenCount + i);
		elseR = analyze(s, std::move(elseR.state));
	}

	// Merge.
	if (thenR.terminated && elseR.terminated) return Result{NameMap{}, true};
	if (thenR.terminated) return elseR;
	if (elseR.terminated) return thenR;
	return Result{mergeMaps(thenR.state, elseR.state), false};
}

Result Analyzer::analyzeWhile(NodeIdx idx, NameMap state) {
	// WhileNode: d.lhs = NodeIdx (cond), d.rhs = ExtraIdx →
	//            [bodyCount, body0, body1, ...]
	const AstNode &n = nodes_.get(idx);
	auto r = analyze(n.lhs, std::move(state));
	if (r.terminated) return r;

	NameMap stateBefore = r.state;  // body may execute zero times

	ExtraIdx extra = n.rhs;
	uint32_t bodyCount = nodes_.getExtra(extra);
	Result bodyR{std::move(r.state), false};
	for (uint32_t i = 0; i < bodyCount; i++) {
		if (bodyR.terminated) break;
		NodeIdx s = nodes_.getExtra(extra + 1 + i);
		bodyR = analyze(s, std::move(bodyR.state));
	}

	// Loop body executes zero or more times. The post-loop state is the
	// merge of "body never ran" (stateBefore) with "body ran at least once"
	// (bodyR.state). If bodyR terminated (via break/return), the merge
	// degenerates to stateBefore.
	if (bodyR.terminated) return Result{std::move(stateBefore), false};
	return Result{mergeMaps(stateBefore, bodyR.state), false};
}

Result Analyzer::analyzeFor(NodeIdx idx, NameMap state) {
	// ForNode: d.lhs = ExtraIdx → [StringIdx var, NodeIdx start, NodeIdx end,
	//                              bodyCount, body0, body1, ...]
	const AstNode &n = nodes_.get(idx);
	ExtraIdx extra = n.lhs;
	StringIdx varIdx = nodes_.getExtra(extra + 0);
	NodeIdx startIdx = nodes_.getExtra(extra + 1);
	NodeIdx endIdx = nodes_.getExtra(extra + 2);
	uint32_t bodyCount = nodes_.getExtra(extra + 3);

	auto r = analyze(startIdx, std::move(state));
	if (r.terminated) return r;
	r = analyze(endIdx, std::move(r.state));
	if (r.terminated) return r;

	NameMap stateBefore = r.state;
	const std::string &varName = strings_.get(varIdx);
	r.state[varName] = InitState::Init;

	Result bodyR{std::move(r.state), false};
	for (uint32_t i = 0; i < bodyCount; i++) {
		if (bodyR.terminated) break;
		NodeIdx s = nodes_.getExtra(extra + 4 + i);
		bodyR = analyze(s, std::move(bodyR.state));
	}

	// For-over-range: P2 assumes the body executes at least once. This
	// matches existing Jam idioms (e.g., `for i in 0:N { arr[i] = ... }`
	// followed by reading `arr`). Strictly speaking the body may run
	// zero times for an empty range; accepting that imprecision keeps
	// the analyzer practical on real code. A later phase can refine
	// when start/end are known constants.
	//
	// If the body terminated via `break`, fall back to stateBefore — we
	// can't tell whether the break ran on the first iteration or after
	// the body had effects. Conservative: pretend body never ran.
	if (bodyR.terminated) return Result{std::move(stateBefore), false};
	return bodyR;
}

Result Analyzer::analyzeMatch(NodeIdx idx, NameMap state) {
	// MatchNode: d.lhs = NodeIdx (scrutinee), d.rhs = ExtraIdx →
	//   [armCount, elseBodyCount, elseBody...,
	//    arm0_patIdx, arm0_bodyCount, arm0_body...,
	//    arm1_patIdx, arm1_bodyCount, arm1_body..., ...]
	const AstNode &n = nodes_.get(idx);
	auto r = analyze(n.lhs, std::move(state));
	if (r.terminated) return r;

	ExtraIdx extra = n.rhs;
	uint32_t armCount = nodes_.getExtra(extra + 0);
	uint32_t elseBodyCount = nodes_.getExtra(extra + 1);

	NameMap stateBefore = r.state;

	// Else-arm body (catch-all).
	Result mergedR{NameMap{}, true};  // start as "no arm taken yet"
	if (elseBodyCount > 0) {
		mergedR = Result{stateBefore, false};
		for (uint32_t i = 0; i < elseBodyCount; i++) {
			if (mergedR.terminated) break;
			NodeIdx s = nodes_.getExtra(extra + 2 + i);
			mergedR = analyze(s, std::move(mergedR.state));
		}
	}

	uint32_t cursor = 2 + elseBodyCount;
	for (uint32_t a = 0; a < armCount; a++) {
		// Arm pattern (index ignored for init analysis — patterns don't
		// touch existing-binding state in M1).
		cursor++;  // patIdx
		uint32_t armBodyCount = nodes_.getExtra(extra + cursor);
		cursor++;

		Result armR{stateBefore, false};
		for (uint32_t i = 0; i < armBodyCount; i++) {
			if (armR.terminated) break;
			NodeIdx s = nodes_.getExtra(extra + cursor + i);
			armR = analyze(s, std::move(armR.state));
		}
		cursor += armBodyCount;

		if (mergedR.terminated && armR.terminated) {
			// both paths terminated; stay terminated
		} else if (mergedR.terminated) {
			mergedR = std::move(armR);
		} else if (armR.terminated) {
			// keep mergedR
		} else {
			mergedR.state = mergeMaps(mergedR.state, armR.state);
		}
	}

	return mergedR;
}

Result Analyzer::analyzeReturn(NodeIdx idx, NameMap state) {
	const AstNode &n = nodes_.get(idx);
	Result r{std::move(state), false};
	if (n.lhs != kNoNode) { r = analyze(n.lhs, std::move(r.state)); }
	r.terminated = true;
	return r;
}

Result Analyzer::analyzeCall(NodeIdx idx, NameMap state) {
	// Call: d.lhs = StringIdx (callee), d.rhs = ExtraIdx → [argCount, args...]
	const AstNode &n = nodes_.get(idx);
	ExtraIdx extra = n.rhs;
	uint32_t argCount = nodes_.getExtra(extra);
	Result r{std::move(state), false};
	for (uint32_t i = 0; i < argCount; i++) {
		if (r.terminated) return r;
		NodeIdx arg = nodes_.getExtra(extra + 1 + i);
		r = analyze(arg, std::move(r.state));
	}
	return r;
}

Result Analyzer::analyzeStructLit(NodeIdx idx, NameMap state) {
	// StructLit: d.rhs = ExtraIdx →
	//   [fieldCount, fieldName0, fieldExpr0, fieldName1, fieldExpr1, ...]
	const AstNode &n = nodes_.get(idx);
	ExtraIdx extra = n.rhs;
	uint32_t fieldCount = nodes_.getExtra(extra);
	Result r{std::move(state), false};
	for (uint32_t i = 0; i < fieldCount; i++) {
		if (r.terminated) return r;
		NodeIdx exprIdx = nodes_.getExtra(extra + 1 + 2 * i + 1);
		r = analyze(exprIdx, std::move(r.state));
	}
	return r;
}

// --------------------------------------------------------------------------
// Read check + diagnostics.
// --------------------------------------------------------------------------

void Analyzer::checkVariableRead(NodeIdx idx, const NameMap &state) {
	const AstNode &n = nodes_.get(idx);
	StringIdx nameIdx = n.lhs;
	const std::string &name = strings_.get(nameIdx);

	auto it = state.find(name);
	if (it == state.end()) {
		// Not in our tracking — likely a global, an imported name, or a
		// for-loop variable that escaped its scope. P2 conservatively
		// assumes Init for unseen names so it doesn't false-fire.
		return;
	}

	switch (it->second) {
	case InitState::Init:
	case InitState::Unknown:
		return;  // OK
	case InitState::Uninit:
		emitError("use of uninitialized binding `" + name + "`", idx, name);
		return;
	case InitState::MaybeInit:
		emitError("binding `" + name +
		              "` may not be initialized on every path that reaches here",
		          idx, name);
		return;
	}
}

int Analyzer::lineOf(NodeIdx idx) const {
	if (idx == kNoNode) return 0;
	const AstNode &n = nodes_.get(idx);
	if (n.mainToken >= tokens_.size()) return 0;
	return tokens_[n.mainToken].line;
}

void Analyzer::emitError(std::string message, NodeIdx anchor,
                         std::string varName) {
	Diagnostic d;
	d.message = std::move(message);
	d.line = lineOf(anchor);
	d.varName = std::move(varName);
	diagnostics_.push_back(std::move(d));
}

// --------------------------------------------------------------------------
// Lattice merge.
// --------------------------------------------------------------------------

NameMap Analyzer::mergeMaps(const NameMap &a, const NameMap &b) {
	NameMap result = a;
	for (const auto &kv : b) {
		auto it = result.find(kv.first);
		if (it == result.end()) {
			result[kv.first] = kv.second;
		} else {
			it->second = mergeState(it->second, kv.second);
		}
	}
	return result;
}

}  // namespace

// --------------------------------------------------------------------------
// Public entry point.
// --------------------------------------------------------------------------

std::vector<Diagnostic> analyze(const FunctionAST &fn, const NodeStore &nodes,
                                const StringPool &strings,
                                const std::vector<Token> &tokens) {
	Analyzer a(nodes, strings, tokens);
	return a.run(fn);
}

}  // namespace init_analysis
}  // namespace jam
