// Codegen-time must-fail tests.
//
// The init-analyzer test suite (test_init_analysis.cpp) covers errors
// reported by analysis on a parsed module. This file covers errors that
// surface during *codegen* — primarily generic instantiation failures
// like "type T has no method default" — by invoking the jam.out
// binary as a subprocess and asserting on its stderr.
//
// Subprocess approach (rather than driving JamCodegenContext in-process)
// avoids replicating the LLVM-init / target-machine / drop-registry
// scaffolding that main.cpp builds end-to-end. Each test writes a Jam
// source file to /tmp, runs the compiler, captures stderr+exit, and
// asserts on the returned message.

#include "test_framework.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct CompileResult {
	int exitCode;
	std::string stderr_;
};

// Run jam.out on a one-off source string. The binary must already be
// built (the Makefile target depends on `build`). We invoke from the
// project root so jam.out's relative paths resolve correctly.
CompileResult compileSource(const std::string &name,
                             const std::string &source) {
	std::string path = "/tmp/" + name + ".jam";
	{
		std::ofstream out(path);
		out << source;
	}

	// Redirect stderr→stdout so popen captures both. jam.out usually
	// only writes to stderr on error, but this is robust either way.
	std::string cmd = "./jam.out " + path + " 2>&1";

	std::string output;
	FILE *pipe = popen(cmd.c_str(), "r");
	if (!pipe) {
		throw std::runtime_error("popen failed: " + cmd);
	}
	char buf[256];
	while (fgets(buf, sizeof(buf), pipe) != nullptr) output += buf;
	int status = pclose(pipe);

	int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return {exitCode, std::move(output)};
}

bool stderrContains(const CompileResult &r, const std::string &substr) {
	return r.stderr_.find(substr) != std::string::npos;
}

}  // namespace

class CodegenErrorTests {
  public:
	static void registerAllTests(TestFramework &framework) {
		framework.addTest("Codegen - Maybe(T) where T lacks default()",
		                  testMaybeOfTypeWithoutDefault);
		framework.addTest("Codegen - default() with parameters rejected",
		                  testDefaultWithParameters);
		framework.addTest("Codegen - default() with wrong return type",
		                  testDefaultWrongReturnType);
		framework.addTest("Codegen - non-drop non-default method on top-level",
		                  testForbiddenTopLevelMethod);
	}

  private:
	// A generic body that calls T.default() must instantiate to a
	// concrete T that has a default() method. NoDefault doesn't, so
	// instantiation should error with a precise message naming both
	// the missing method and the type that's missing it.
	static void testMaybeOfTypeWithoutDefault() {
		auto r = compileSource("must_fail_no_default", R"(
const NoDefault = struct {
    n: i32,
};

fn Maybe(T: type) type {
    return struct {
        storage: T,
        valid: bool,
        fn default() Self {
            return { storage: T.default(), valid: false };
        }
    };
}

const MaybeND = Maybe(NoDefault);

fn main() i32 {
    var m: MaybeND = MaybeND.default();
    return m.storage.n;
}
)");
		ASSERT_TRUE(r.exitCode != 0);
		ASSERT_TRUE(stderrContains(r, "NoDefault"));
		ASSERT_TRUE(stderrContains(r, "default"));
	}

	// `default` on a top-level struct must take no parameters. The
	// validation in main.cpp specifically checks Args.empty().
	static void testDefaultWithParameters() {
		auto r = compileSource("must_fail_default_with_params", R"(
const Bad = struct {
    n: i32,
    fn default(self: mut Self) Self {
        return { n: 0 };
    }
};

fn main() i32 { return 0; }
)");
		ASSERT_TRUE(r.exitCode != 0);
		ASSERT_TRUE(stderrContains(r, "default"));
		ASSERT_TRUE(stderrContains(r, "no parameters"));
	}

	// `default` must return Self (the enclosing struct's type).
	// Returning anything else is a typing error in the contract.
	static void testDefaultWrongReturnType() {
		auto r = compileSource("must_fail_default_wrong_return", R"(
const Bad = struct {
    n: i32,
    fn default() i32 {
        return 0;
    }
};

fn main() i32 { return 0; }
)");
		ASSERT_TRUE(r.exitCode != 0);
		ASSERT_TRUE(stderrContains(r, "default"));
		ASSERT_TRUE(stderrContains(r, "Self"));
	}

	// Top-level structs only allow `drop` and `default` methods. Other
	// names (e.g. `unwrap`) get a clear "not allowed" error so users
	// don't think method-as-namespace works on plain structs.
	static void testForbiddenTopLevelMethod() {
		auto r = compileSource("must_fail_other_method", R"(
const Bad = struct {
    n: i32,
    fn unwrap(self: mut Self) i32 {
        return self.n;
    }
};

fn main() i32 { return 0; }
)");
		ASSERT_TRUE(r.exitCode != 0);
		ASSERT_TRUE(stderrContains(r, "drop"));
		ASSERT_TRUE(stderrContains(r, "default"));
	}
};

int main() {
	TestFramework framework;
	CodegenErrorTests::registerAllTests(framework);
	framework.runAll();
	return framework.allPassed() ? 0 : 1;
}
