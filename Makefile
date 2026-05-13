.PHONY: build install uninstall clean cmake-build cmake-install cmake-uninstall test test-unit docs format check-format
.DEFAULT_GOAL := build

LLVM_CONFIG=$(shell which llvm-config 2>/dev/null || echo "llvm-config")
OPTFLAGS ?= -O2 -DNDEBUG

CLANG_FORMAT ?= clang-format
CLANG_FORMAT_STYLE := file:clang-format
FORMAT_SOURCES := $(wildcard src/*.cpp src/*.h) $(wildcard tests/cpp/*.cpp tests/cpp/*.h)

# Check if we're on macOS or Linux
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PLATFORM = macOS
    PREFIX ?= $(HOME)/.local
else ifeq ($(UNAME_S),Linux)
    PLATFORM = Linux
    PREFIX ?= /usr/local
else
    PLATFORM = Unknown
    PREFIX ?= /usr/local
endif

BINDIR ?= $(PREFIX)/bin

build:
	@echo "Building Jam compiler for $(PLATFORM)..."
	@if ! command -v $(LLVM_CONFIG) >/dev/null 2>&1; then \
		echo "Error: llvm-config not found. Please install LLVM development packages."; \
		exit 1; \
	fi
	clang++ -c ./src/jam_llvm.cpp -o ./jam_llvm.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/main.cpp -o ./main.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/lexer.cpp -o ./lexer.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/parser.cpp -o ./parser.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/ast.cpp -o ./ast.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/codegen.cpp -o ./codegen.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/target.cpp -o ./target.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/cabi.cpp -o ./cabi.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/module_resolver.cpp -o ./module_resolver.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/symbol_table.cpp -o ./symbol_table.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/number_literal.cpp -o ./number_literal.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/init_analysis.cpp -o ./init_analysis.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/drop_registry.cpp -o ./drop_registry.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -c ./src/abi.cpp -o ./abi.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -o ./jam.out ./jam_llvm.o ./main.o ./lexer.o ./parser.o ./ast.o ./codegen.o ./target.o ./cabi.o ./module_resolver.o ./symbol_table.o ./number_literal.o ./init_analysis.o ./drop_registry.o ./abi.o `$(LLVM_CONFIG) --ldflags --libs --libfiles --system-libs`
	@echo "Build complete! Executable: ./jam.out"

# CMake-based build (recommended)
cmake-build:
	@echo "Building with CMake..."
	@mkdir -p build
	cd build && cmake .. && cmake --build .

# Install using CMake (recommended)
cmake-install: cmake-build
	@echo "Installing Jam compiler using CMake..."
	cd build && make install
	@echo ""
	@echo "Jam compiler installed successfully!"
	@echo "Try: jam --help"

# Uninstall using CMake
cmake-uninstall:
	@echo "Uninstalling Jam compiler..."
	@if [ -f build/cmake_uninstall.cmake ]; then \
		cd build && make uninstall; \
		echo "Jam compiler uninstalled successfully!"; \
	else \
		echo "No installation found. Run 'make cmake-install' first."; \
	fi

# Manual install (fallback)
install: build
	@echo "Installing Jam compiler to $(PREFIX)..."
	@echo "Platform: $(PLATFORM)"
	
	# Install executable
	cp ./jam.out $(BINDIR)/jam
	chmod 755 $(BINDIR)/jam
	
	@echo ""
	@echo "Jam compiler installed successfully!"
	@echo "Try: jam --help"

# Manual uninstall
uninstall:
	@echo "Uninstalling Jam compiler..."
	rm -f $(BINDIR)/jam
	@echo "Jam compiler uninstalled successfully!"

# Clean build artifacts
clean:
	rm -f ./jam_llvm.o ./main.o ./lexer.o ./parser.o ./ast.o ./codegen.o ./target.o ./cabi.o ./module_resolver.o ./symbol_table.o ./jam.out
	rm -rf build/
	@echo "Build artifacts cleaned!"

# Show installation info
info:
	@echo "Platform: $(PLATFORM)"
	@echo "LLVM Config: $(LLVM_CONFIG)"
	@echo "Install Prefix: $(PREFIX)"
	@echo "Binary Directory: $(BINDIR)"
	@echo ""
	@echo "Available targets:"
	@echo "  make build          - Build the compiler"
	@echo "  make test-unit      - Run Jam unit tests"
	@echo "  make test           - Run all tests (Jam + C++)"
	@echo "  make docs           - Serve documentation site"
	@echo "  make install        - Install using manual method"
	@echo "  make uninstall      - Uninstall manual installation"
	@echo "  make cmake-install  - Install using CMake (recommended)"
	@echo "  make cmake-uninstall- Uninstall CMake installation"
	@echo "  make clean          - Clean build artifacts"
	@echo "  make info           - Show this information"

# Run Jam unit tests (auto-discovers any .jam with tfn declarations)
test-unit: build
	@echo "Running Jam unit tests..."
	./jam.out test tests/unit

# Run init-analyzer C++ tests (in-process: drives lexer + parser +
# analyzer over source strings and asserts on the structured Diagnostic
# vector). Replaces the old shell-based must-fail runner — same coverage,
# precise assertions, no fork+exec, no stderr scraping.
test-init: build
	@echo ""
	@echo "Building and running init-analyzer C++ tests..."
	clang++ -c ./tests/cpp/test_init_analysis.cpp -o ./test_init_analysis.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -o ./init_tests ./test_init_analysis.o ./jam_llvm.o ./lexer.o ./parser.o ./ast.o ./codegen.o ./target.o ./cabi.o ./module_resolver.o ./symbol_table.o ./number_literal.o ./init_analysis.o ./drop_registry.o ./abi.o `$(LLVM_CONFIG) --ldflags --libs --libfiles --system-libs`
	./init_tests

# Run ABI classifier C++ tests (P9.1). Pure unit tests of the
# (mode, type) → ParamABI / type → ReturnABI mappings; no runtime
# dependency on a parsed program.
test-abi: build
	@echo ""
	@echo "Building and running ABI classifier C++ tests..."
	clang++ -c ./tests/cpp/test_abi.cpp -o ./test_abi.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -o ./abi_tests ./test_abi.o ./jam_llvm.o ./lexer.o ./parser.o ./ast.o ./codegen.o ./target.o ./cabi.o ./module_resolver.o ./symbol_table.o ./number_literal.o ./init_analysis.o ./drop_registry.o ./abi.o `$(LLVM_CONFIG) --ldflags --libs --libfiles --system-libs`
	./abi_tests

# Codegen-time must-fail tests. Each test invokes ./jam.out as a
# subprocess on a small Jam source string and asserts on stderr/exit
# (the kind of error that surfaces during instantiation, not during
# semantic analysis — generic methods missing, default() with the
# wrong shape, etc.). Subprocess approach because driving the full
# codegen in-process would require replicating main.cpp's LLVM init
# scaffolding.
test-codegen-errors: build
	@echo ""
	@echo "Building and running codegen-error C++ tests..."
	clang++ -c ./tests/cpp/test_codegen_errors.cpp -o ./test_codegen_errors.o `$(LLVM_CONFIG) --cxxflags` -fexceptions $(OPTFLAGS)
	clang++ -o ./codegen_error_tests ./test_codegen_errors.o
	./codegen_error_tests

# Run all tests: Jam must-pass + analyzer C++ tests + ABI C++ tests +
# codegen-error C++ tests. The legacy `tests/cpp/build_and_run.sh`
# CMake suite was retired (drifted out of sync with `--emit-ir`'s
# linking behavior; see test-legacy-cpp below for archaeology).
test: test-unit test-init test-abi test-codegen-errors

# Legacy CMake-driven C++ test suite. Pre-dates the in-process analyzer
# and ABI tests; kept only for archaeology. Not run by `make test`
# because it captures the link path (no `main` in test sources → all
# 32 tests fail at link, not at the assertion they actually want to
# check).
test-legacy-cpp:
	cd tests/cpp && ./build_and_run.sh

# Format all C++ sources in-place
format:
	@echo "Formatting $(words $(FORMAT_SOURCES)) C++ file(s)..."
	@$(CLANG_FORMAT) --style=$(CLANG_FORMAT_STYLE) -i $(FORMAT_SOURCES)
	@echo "Done."

# Fail if any C++ source is not formatted
check-format:
	@echo "Checking format of $(words $(FORMAT_SOURCES)) C++ file(s)..."
	@$(CLANG_FORMAT) --style=$(CLANG_FORMAT_STYLE) --dry-run --Werror $(FORMAT_SOURCES)

# Serve documentation site
docs:
	@echo "Serving documentation at http://localhost:4000..."
	cd docs && bundle install && bundle exec jekyll serve --livereload
