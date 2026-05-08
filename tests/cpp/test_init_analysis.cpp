// In-process tests for the MVS init analyzer (P2 through P5.5).
//
// Each test compiles a Jam source string through lexer + parser, runs
// init_analysis::analyze on every function in the parsed module, and
// asserts on the returned Diagnostic vector. No fork+exec, no stderr
// scraping — the analyzer's structured output is checked directly.
//
// This complements the .jam-based must-pass suite under tests/unit/ by
// covering the rejected-program cases that require precise diagnostic
// shape (count, message substring, varName) rather than a binary
// "did the compile fail?" oracle.

#include "../../src/ast.h"
#include "../../src/ast_flat.h"
#include "../../src/init_analysis.h"
#include "../../src/lexer.h"
#include "../../src/parser.h"
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

namespace {

// One run's outputs: the diagnostics from every analyzed function plus
// the supporting state kept alive long enough for the diagnostics to
// reference (the strings live in StringPool, etc.).
struct AnalyzeResult {
	std::unique_ptr<NodeStore> nodes;
	std::unique_ptr<StringPool> strings;
	std::unique_ptr<TypePool> types;
	std::unique_ptr<ModuleAST> module;
	std::vector<jam::init_analysis::Diagnostic> diagnostics;
};

AnalyzeResult analyzeSource(const std::string &src) {
	AnalyzeResult result;
	result.nodes = std::make_unique<NodeStore>();
	result.strings = std::make_unique<StringPool>();
	result.types = std::make_unique<TypePool>();

	Lexer lexer(src);
	auto tokens = lexer.scanTokens();

	Parser parser(tokens, *result.types, *result.strings, *result.nodes);
	result.module = parser.parse();

	jam::init_analysis::FunctionRegistry registry;
	for (auto &fn : result.module->Functions) {
		registry[fn->Name] = fn.get();
	}

	jam::drops::DropRegistry drops = jam::drops::buildDropRegistry(
	    *result.module, *result.types, *result.strings);

	for (auto &fn : result.module->Functions) {
		if (fn->isExtern) continue;
		auto diags = jam::init_analysis::analyze(*fn, *result.nodes,
		                                         *result.strings, tokens,
		                                         &registry, &drops,
		                                         result.types.get());
		for (auto &d : diags) result.diagnostics.push_back(std::move(d));
	}
	return result;
}

bool diagsContain(const std::vector<jam::init_analysis::Diagnostic> &diags,
                  const std::string &msgSubstring) {
	for (const auto &d : diags) {
		if (d.message.find(msgSubstring) != std::string::npos) return true;
	}
	return false;
}

bool diagsAbout(const std::vector<jam::init_analysis::Diagnostic> &diags,
                const std::string &varName) {
	for (const auto &d : diags) {
		if (d.varName == varName) return true;
	}
	return false;
}

// =========================================================================
// P2 — definite initialization
// =========================================================================

void testUninitRead() {
	auto r = analyzeSource(R"(
fn f() u32 {
    var x: u32 = undefined;
    return x;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message, "uninitialized");
	ASSERT_EQ(std::string("x"), r.diagnostics[0].varName);
}

void testPartialBranchInit() {
	auto r = analyzeSource(R"(
fn f(b: bool) u32 {
    var x: u32 = undefined;
    if (b) {
        x = 100;
    }
    return x;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message,
	                "may not be initialized on every path");
	ASSERT_EQ(std::string("x"), r.diagnostics[0].varName);
}

void testStraightLineOK() {
	auto r = analyzeSource(R"(
fn f() u32 {
    var x: u32 = undefined;
    x = 5;
    return x;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

void testIfElseBothInitOK() {
	auto r = analyzeSource(R"(
fn f(b: bool) u32 {
    var x: u32 = undefined;
    if (b) {
        x = 1;
    } else {
        x = 2;
    }
    return x;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

// =========================================================================
// P3 — undefined-mode parameter rules
// =========================================================================

void testUndefinedParamUnset() {
	auto r = analyzeSource(R"(
fn f(out: undefined u32) u32 {
    return 42;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message,
	                "`undefined`-mode parameter `out` was not initialized");
}

void testUndefinedParamReadBeforeWrite() {
	auto r = analyzeSource(R"(
fn f(out: undefined u32) u32 {
    return out;
}
)");
	// Two diagnostics expected: read-of-uninit AND undefined-param-not-init.
	// The second is redundant but informative; both are produced.
	ASSERT_TRUE(diagsContain(r.diagnostics, "use of uninitialized binding"));
	ASSERT_TRUE(diagsContain(r.diagnostics, "`undefined`-mode parameter"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "out"));
}

void testUndefinedParamInitOK() {
	auto r = analyzeSource(R"(
fn f(out: undefined u32) u32 {
    out = 42;
    return out;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

// =========================================================================
// P4 — callsite mode propagation
// =========================================================================

void testReadAfterMove() {
	auto r = analyzeSource(R"(
fn consume(buf: move u32) u32 { return buf; }

fn caller() u32 {
    var x: u32 = 100;
    consume(x);
    return x;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message, "uninitialized");
	ASSERT_EQ(std::string("x"), r.diagnostics[0].varName);
}

void testDoubleMove() {
	auto r = analyzeSource(R"(
fn consume(buf: move u32) u32 { return buf; }

fn caller() u32 {
    var x: u32 = 100;
    consume(x);
    return consume(x);
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message, "uninitialized");
	ASSERT_EQ(std::string("x"), r.diagnostics[0].varName);
}

void testUninitLetArg() {
	auto r = analyzeSource(R"(
fn read(x: u32) u32 { return x; }

fn caller() u32 {
    var x: u32 = undefined;
    return read(x);
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message, "uninitialized");
}

void testMoveThenSeparateBindingOK() {
	auto r = analyzeSource(R"(
fn consume(buf: move u32) u32 { return buf; }

fn caller() u32 {
    var x: u32 = 5;
    consume(x);
    var y: u32 = 10;
    return y;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

// =========================================================================
// P5 — exclusivity rule
// =========================================================================

void testExclusivityMutLet() {
	auto r = analyzeSource(R"(
fn modify(x: mut u32, y: u32) u32 {
    x = x + y;
    return x;
}

fn caller() u32 {
    var n: u32 = 5;
    return modify(n, n);
}
)");
	ASSERT_TRUE(diagsContain(r.diagnostics, "conflicting borrows"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "n"));
}

void testExclusivityTwoMoves() {
	auto r = analyzeSource(R"(
fn consumeTwo(a: move u32, b: move u32) u32 { return a + b; }

fn caller() u32 {
    var x: u32 = 5;
    return consumeTwo(x, x);
}
)");
	ASSERT_TRUE(diagsContain(r.diagnostics, "conflicting borrows"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "x"));
}

void testExclusivityOverlappingPath() {
	auto r = analyzeSource(R"(
const Pair = struct { a: u32, b: u32 };

fn modifyAndRead(whole: mut Pair, part: u32) u32 { return part; }

fn caller() u32 {
    var p: Pair = undefined;
    p.a = 1;
    p.b = 2;
    return modifyAndRead(p, p.a);
}
)");
	ASSERT_TRUE(diagsContain(r.diagnostics, "conflicting borrows"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "p"));
}

void testExclusivityDisjointFieldsOK() {
	auto r = analyzeSource(R"(
const Pair = struct { a: u32, b: u32 };

fn add(a: u32, b: u32) u32 { return a + b; }

fn caller() u32 {
    var p: Pair = undefined;
    p.a = 10;
    p.b = 20;
    return add(p.a, p.b);
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

// =========================================================================
// P5.5 — scope-escape check
// =========================================================================

void testEscapeMutParam() {
	auto r = analyzeSource(R"(
fn dangle(p: mut u32) *mut u32 {
    return &p;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message, "cannot return `&` of `mut`-mode");
	ASSERT_EQ(std::string("p"), r.diagnostics[0].varName);
}

void testEscapeMutField() {
	auto r = analyzeSource(R"(
const Pair = struct { a: u32, b: u32 };

fn dangleField(p: mut Pair) *mut u32 {
    return &p.a;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message, "cannot return `&` of `mut`-mode");
}

void testEscapeUndefinedParam() {
	auto r = analyzeSource(R"(
fn dangleUndef(out: undefined u32) *mut u32 {
    out = 5;
    return &out;
}
)");
	ASSERT_EQ(static_cast<size_t>(1), r.diagnostics.size());
	ASSERT_CONTAINS(r.diagnostics[0].message,
	                "cannot return `&` of `undefined`-mode");
}

void testReturnMutParamByValueOK() {
	// `return p;` (no `&`) is a value copy under value semantics — not an
	// escape. The check must NOT flag this.
	auto r = analyzeSource(R"(
fn doubleIt(x: mut u32) u32 {
    x = x + x;
    return x;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

// =========================================================================
// P8 — drop registry foundation
// =========================================================================

void testMoveOnDropBearingRejected() {
	// A type with a user-defined `fn drop(self: mut T)` is "drop-bearing".
	// Until P8.1 lands move-aware drop tracking, the analyzer rejects
	// `move` on drop-bearing bindings to prevent the codegen from emitting
	// drop on a moved-out slot (double-free).
	auto r = analyzeSource(R"(
const File = struct {
    fd: i32,
};

fn drop(self: mut File) {
    self.fd = 0;
}

fn consume(f: move File) i32 {
    return f.fd;
}

fn caller() i32 {
    var f: File = undefined;
    f.fd = 7;
    return consume(f);
}
)");
	ASSERT_TRUE(diagsContain(r.diagnostics, "drop-bearing type"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "f"));
}

void testLetOnDropBearingOK() {
	// Passing a drop-bearing binding by `let` (read-only borrow, default)
	// or `mut` is fine — only `move` is rejected in P8 foundation.
	auto r = analyzeSource(R"(
const File = struct {
    fd: i32,
};

fn drop(self: mut File) {
    self.fd = 0;
}

fn read(f: File) i32 {
    return f.fd;
}

fn caller() i32 {
    var f: File = undefined;
    f.fd = 7;
    return read(f);
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

void testMutOnDropBearingOK() {
	auto r = analyzeSource(R"(
const File = struct {
    fd: i32,
};

fn drop(self: mut File) {
    self.fd = 0;
}

fn close(f: mut File) {
    f.fd = -1;
}

fn caller() i32 {
    var f: File = undefined;
    f.fd = 7;
    close(&f);
    return f.fd;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

// =========================================================================
// P8.2b — drop-bearing locals must be Init at every exit
// =========================================================================

void testDropBearingUninitAtReturnRejected() {
	auto r = analyzeSource(R"(
const File = struct { fd: i32 };
fn drop(self: mut File) { self.fd = 0; }

fn forgotInit() i32 {
    var f: File = undefined;
    return 42;
}
)");
	ASSERT_TRUE(diagsContain(r.diagnostics, "drop-bearing binding"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "f"));
}

void testDropBearingPartialBranchInitRejected() {
	auto r = analyzeSource(R"(
const File = struct { fd: i32 };
fn drop(self: mut File) { self.fd = 0; }

fn maybe(b: bool) i32 {
    var f: File = undefined;
    if (b) {
        f.fd = 7;
    }
    return 0;
}
)");
	ASSERT_TRUE(diagsContain(r.diagnostics, "drop-bearing binding"));
	ASSERT_TRUE(diagsContain(r.diagnostics,
	                         "may not be initialized on every path"));
	ASSERT_TRUE(diagsAbout(r.diagnostics, "f"));
}

void testDropBearingFullyInitOK() {
	// Initialized via partial-write rule (every path touches f) → Init at
	// exit, no rejection.
	auto r = analyzeSource(R"(
const File = struct { fd: i32 };
fn drop(self: mut File) { self.fd = 0; }

fn ok() i32 {
    var f: File = undefined;
    f.fd = 9;
    return 0;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

void testDropBearingInitOnAllBranchesOK() {
	auto r = analyzeSource(R"(
const File = struct { fd: i32 };
fn drop(self: mut File) { self.fd = 0; }

fn ok(b: bool) i32 {
    var f: File = undefined;
    if (b) {
        f.fd = 1;
    } else {
        f.fd = 2;
    }
    return 0;
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

void testNonDropBearingMoveOK() {
	// Non-drop-bearing struct can still be moved freely (no double-free
	// risk because no drop runs at scope exit).
	auto r = analyzeSource(R"(
const Plain = struct {
    a: u32,
    b: u32,
};

fn consume(p: move Plain) u32 {
    return p.a + p.b;
}

fn caller() u32 {
    var p: Plain = undefined;
    p.a = 1;
    p.b = 2;
    return consume(p);
}
)");
	ASSERT_EQ(static_cast<size_t>(0), r.diagnostics.size());
}

}  // namespace

class InitAnalysisTests {
  public:
	static void registerAllTests(TestFramework &framework) {
		// P2 — definite-init
		framework.addTest("InitAnalysis P2 - uninit read", testUninitRead);
		framework.addTest("InitAnalysis P2 - partial branch init",
		                  testPartialBranchInit);
		framework.addTest("InitAnalysis P2 - straight line OK",
		                  testStraightLineOK);
		framework.addTest("InitAnalysis P2 - if/else both init OK",
		                  testIfElseBothInitOK);

		// P3 — undefined-mode parameter
		framework.addTest("InitAnalysis P3 - undefined param unset",
		                  testUndefinedParamUnset);
		framework.addTest("InitAnalysis P3 - undefined param read before write",
		                  testUndefinedParamReadBeforeWrite);
		framework.addTest("InitAnalysis P3 - undefined param init OK",
		                  testUndefinedParamInitOK);

		// P4 — callsite mode propagation
		framework.addTest("InitAnalysis P4 - read after move",
		                  testReadAfterMove);
		framework.addTest("InitAnalysis P4 - double move", testDoubleMove);
		framework.addTest("InitAnalysis P4 - uninit let arg",
		                  testUninitLetArg);
		framework.addTest("InitAnalysis P4 - move then separate binding OK",
		                  testMoveThenSeparateBindingOK);

		// P5 — exclusivity
		framework.addTest("InitAnalysis P5 - mut + let same binding",
		                  testExclusivityMutLet);
		framework.addTest("InitAnalysis P5 - two moves same binding",
		                  testExclusivityTwoMoves);
		framework.addTest("InitAnalysis P5 - overlapping path",
		                  testExclusivityOverlappingPath);
		framework.addTest("InitAnalysis P5 - disjoint fields OK",
		                  testExclusivityDisjointFieldsOK);

		// P5.5 — scope escape
		framework.addTest("InitAnalysis P5.5 - escape &mut param",
		                  testEscapeMutParam);
		framework.addTest("InitAnalysis P5.5 - escape &mut field",
		                  testEscapeMutField);
		framework.addTest("InitAnalysis P5.5 - escape &undefined param",
		                  testEscapeUndefinedParam);
		framework.addTest("InitAnalysis P5.5 - mut param by-value return OK",
		                  testReturnMutParamByValueOK);

		// P8 — drop registry foundation
		framework.addTest("InitAnalysis P8 - move on drop-bearing rejected",
		                  testMoveOnDropBearingRejected);
		framework.addTest("InitAnalysis P8 - let on drop-bearing OK",
		                  testLetOnDropBearingOK);
		framework.addTest("InitAnalysis P8 - mut on drop-bearing OK",
		                  testMutOnDropBearingOK);
		framework.addTest("InitAnalysis P8 - move on non-drop OK",
		                  testNonDropBearingMoveOK);

		// P8.2 — drop-bearing locals must be Init at every exit
		framework.addTest("InitAnalysis P8.2 - uninit at return rejected",
		                  testDropBearingUninitAtReturnRejected);
		framework.addTest("InitAnalysis P8.2 - partial branch init rejected",
		                  testDropBearingPartialBranchInitRejected);
		framework.addTest("InitAnalysis P8.2 - fully init OK",
		                  testDropBearingFullyInitOK);
		framework.addTest("InitAnalysis P8.2 - init on all branches OK",
		                  testDropBearingInitOnAllBranchesOK);
	}
};

int main() {
	TestFramework framework;
	InitAnalysisTests::registerAllTests(framework);
	framework.runAll();
	return framework.allPassed() ? 0 : 1;
}
