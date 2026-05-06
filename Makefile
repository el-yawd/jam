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
	clang++ -o ./jam.out ./jam_llvm.o ./main.o ./lexer.o ./parser.o ./ast.o ./codegen.o ./target.o ./cabi.o ./module_resolver.o ./symbol_table.o ./number_literal.o `$(LLVM_CONFIG) --ldflags --libs --libfiles --system-libs`
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

# Run all tests: Jam language tests + C++ compiler unit tests
test: test-unit
	@echo ""
	@echo "Running C++ unit tests..."
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
	cd docs && bundle update && bundle exec jekyll serve
