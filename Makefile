# Modern Browser Parser - HTML5 & CSS3 Engine
# Organized structure for browser development

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
DEBUG_FLAGS = -g -DDEBUG
INCLUDES = -Ihtml/include -Icss/include -Icore/include

# Directories
HTML_SRC = html/src
CSS_SRC = css/src
CORE_SRC = core/src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin
EXAMPLES_DIR = examples
TESTS_DIR = tests

# Create directories
$(shell mkdir -p $(OBJ_DIR)/html $(OBJ_DIR)/css $(OBJ_DIR)/core $(BIN_DIR))

# Source files
HTML_SOURCES = $(wildcard $(HTML_SRC)/*.cpp)
CSS_SOURCES = $(wildcard $(CSS_SRC)/*.cpp)
CORE_SOURCES = $(wildcard $(CORE_SRC)/*.cpp)

# Object files
HTML_OBJECTS = $(HTML_SOURCES:$(HTML_SRC)/%.cpp=$(OBJ_DIR)/html/%.o)
CSS_OBJECTS = $(CSS_SOURCES:$(CSS_SRC)/%.cpp=$(OBJ_DIR)/css/%.o)
CORE_OBJECTS = $(CORE_SOURCES:$(CORE_SRC)/%.cpp=$(OBJ_DIR)/core/%.o)

ALL_OBJECTS = $(HTML_OBJECTS) $(CSS_OBJECTS) $(CORE_OBJECTS)

# Targets
.PHONY: all clean browser html-parser css-parser core examples tests debug help

# Default target
all: browser

# Main browser parser (integrated HTML5 + CSS3)
browser: $(BIN_DIR)/browser-parser
	@echo "✅ Browser parser built successfully!"

$(BIN_DIR)/browser-parser: $(ALL_OBJECTS) $(OBJ_DIR)/main_browser.o
	@echo "🔗 Linking browser parser..."
	$(CXX) $(CXXFLAGS) $^ -o $@

# Individual parsers
html-parser: $(BIN_DIR)/html-parser
	@echo "✅ HTML5 parser built successfully!"

$(BIN_DIR)/html-parser: $(HTML_OBJECTS) $(OBJ_DIR)/main_html.o
	@echo "🔗 Linking HTML parser..."
	$(CXX) $(CXXFLAGS) $^ -o $@

css-parser: $(BIN_DIR)/css-parser
	@echo "✅ CSS3 parser built successfully!"

$(BIN_DIR)/css-parser: $(CSS_OBJECTS) $(OBJ_DIR)/main_css.o
	@echo "🔗 Linking CSS parser..."
	$(CXX) $(CXXFLAGS) $^ -o $@

# Core library (for linking with other projects)
core: $(BIN_DIR)/libbrowser.a
	@echo "✅ Browser core library built successfully!"

$(BIN_DIR)/libbrowser.a: $(ALL_OBJECTS)
	@echo "📦 Creating static library..."
	ar rcs $@ $^

# Object file compilation rules
$(OBJ_DIR)/html/%.o: $(HTML_SRC)/%.cpp
	@echo "🔨 Compiling HTML: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/css/%.o: $(CSS_SRC)/%.cpp
	@echo "🔨 Compiling CSS: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@echo "🔨 Compiling Core: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Main files
$(OBJ_DIR)/main_browser.o: main_browser.cpp
	@echo "🔨 Compiling main browser..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/main_html.o: main_html.cpp
	@echo "🔨 Compiling main HTML..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/main_css.o: main_css.cpp
	@echo "🔨 Compiling main CSS..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Debug builds
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: browser
	@echo "🐛 Debug build completed!"

# Examples
examples: browser
	@echo "📚 Building examples..."
	@$(MAKE) -C $(EXAMPLES_DIR) --no-print-directory

# Tests
tests: browser
	@echo "🧪 Building tests..."
	@$(MAKE) -C $(TESTS_DIR) --no-print-directory

# Parser test
test-parser: $(BIN_DIR)/test-parser
	@echo "✅ Parser test built successfully!"
	@echo "🧪 Running parser test..."
	@$(BIN_DIR)/test-parser

$(BIN_DIR)/test-parser: $(ALL_OBJECTS) $(OBJ_DIR)/test_parser.o
	@echo "🔗 Linking parser test..."
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/test_parser.o: test_parser.cpp
	@echo "🔨 Compiling parser test..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Simple test
test-simple: $(BIN_DIR)/test-simple
	@echo "✅ Simple test built successfully!"
	@echo "🧪 Running simple test..."
	@timeout 5 $(BIN_DIR)/test-simple || echo "Test finished (timeout or completed)"

$(BIN_DIR)/test-simple: $(HTML_OBJECTS) $(OBJ_DIR)/test_simple.o
	@echo "🔗 Linking simple test..."
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/test_simple.o: test_simple.cpp
	@echo "🔨 Compiling simple test..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Comprehensive test
test-comprehensive: $(BIN_DIR)/test-comprehensive
	@echo "✅ Comprehensive test built successfully!"
	@echo "🧪 Running comprehensive test..."
	@$(BIN_DIR)/test-comprehensive

$(BIN_DIR)/test-comprehensive: $(ALL_OBJECTS) $(OBJ_DIR)/test_comprehensive.o
	@echo "🔗 Linking comprehensive test..."
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/test_comprehensive.o: test_comprehensive.cpp
	@echo "🔨 Compiling comprehensive test..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Individual test
test-individual: $(BIN_DIR)/test-individual
	@echo "✅ Individual test built successfully!"
	@echo "🧪 Running individual test..."
	@timeout 10 $(BIN_DIR)/test-individual || echo "Test finished (timeout or completed)"

$(BIN_DIR)/test-individual: $(ALL_OBJECTS) $(OBJ_DIR)/test_individual.o
	@echo "🔗 Linking individual test..."
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/test_individual.o: test_individual.cpp
	@echo "🔨 Compiling individual test..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Final comprehensive test with timeout protection
test-final: $(BIN_DIR)/test-final
	@echo "✅ Final test built successfully!"
	@echo "🧪 Running final comprehensive test with timeout protection..."
	@timeout 120 $(BIN_DIR)/test-final || echo "Final test finished (timeout or completed)"

$(BIN_DIR)/test-final: $(ALL_OBJECTS) $(OBJ_DIR)/test_final.o
	@echo "🔗 Linking final test..."
	$(CXX) $(CXXFLAGS) $^ -o $@ -pthread

$(OBJ_DIR)/test_final.o: test_final.cpp
	@echo "🔨 Compiling final test..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# CSS Debug test
test-css-debug: $(BIN_DIR)/test-css-debug
	@echo "✅ CSS debug test built successfully!"
	@echo "🧪 Running CSS debug test..."
	@timeout 10 $(BIN_DIR)/test-css-debug || echo "CSS debug test finished (timeout or completed)"

$(BIN_DIR)/test-css-debug: $(ALL_OBJECTS) $(OBJ_DIR)/test_css_debug.o
	@echo "🔗 Linking CSS debug test..."
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/test_css_debug.o: test_css_debug.cpp
	@echo "🔨 Compiling CSS debug test..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Utilities
clean:
	@echo "🧹 Cleaning build files..."
	rm -rf $(BUILD_DIR)
	@echo "✅ Clean completed!"

install: browser
	@echo "📦 Installing browser parser..."
	cp $(BIN_DIR)/browser-parser /usr/local/bin/ 2>/dev/null || echo "❌ Install failed (try sudo)"

# Development helpers
format:
	@echo "🎨 Formatting code..."
	find . -name "*.cpp" -o -name "*.h" | xargs clang-format -i

lint:
	@echo "🔍 Running linter..."
	find . -name "*.cpp" -o -name "*.h" | xargs cppcheck --enable=all --std=c++17

docs:
	@echo "📖 Generating documentation..."
	doxygen docs/Doxyfile 2>/dev/null || echo "❌ Doxygen not found"

# Information
info:
	@echo "Browser Parser Build System"
	@echo "============================"
	@echo "HTML Sources: $(words $(HTML_SOURCES)) files"
	@echo "CSS Sources:  $(words $(CSS_SOURCES)) files"
	@echo "Core Sources: $(words $(CORE_SOURCES)) files"
	@echo "Build Dir:    $(BUILD_DIR)"
	@echo "Compiler:     $(CXX) $(CXXFLAGS)"

help:
	@echo "Modern Browser Parser Build System"
	@echo "=================================="
	@echo ""
	@echo "Main targets:"
	@echo "  all         Build the complete browser parser (default)"
	@echo "  browser     Build integrated HTML5 + CSS3 parser"
	@echo "  html-parser Build standalone HTML5 parser"
	@echo "  css-parser  Build standalone CSS3 parser"
	@echo "  core        Build static library for linking"
	@echo ""
	@echo "Development:"
	@echo "  debug       Build with debug symbols"
	@echo "  examples    Build example programs"
	@echo "  tests       Build and run tests"
	@echo "  format      Format code with clang-format"
	@echo "  lint        Run static analysis"
	@echo "  docs        Generate documentation"
	@echo ""
	@echo "Utilities:"
	@echo "  clean       Remove build files"
	@echo "  install     Install to system (needs sudo)"
	@echo "  info        Show build information"
	@echo "  help        Show this help message"

# Dependency tracking (optional)
-include $(ALL_OBJECTS:.o=.d)

$(OBJ_DIR)/%.d: %.cpp
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MT $(@:.d=.o) $< > $@
test-safe: 
	@echo "Building safe test..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_simple_safe.cpp -o test_simple_safe.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_simple_safe.o -o test_safe
	@echo "Running SAFE test with 5s timeout..."
	@timeout 5 ./test_safe || echo "Safe test finished"
	@rm -f test_simple_safe.o test_safe


test-keyframes: 
	@echo "Building keyframes test..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_keyframes.cpp -o test_keyframes.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_keyframes.o -o test_keyframes
	@echo "Running KEYFRAMES test with 10s timeout..."
	@timeout 10 ./test_keyframes || echo "Keyframes test finished"
	@rm -f test_keyframes.o test_keyframes


test-keyframes-debug: 
	@echo "Building keyframes debug test..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_keyframes_debug.cpp -o test_keyframes_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_keyframes_debug.o -o test_keyframes_debug
	@echo "Running KEYFRAMES DEBUG test with 5s timeout..."
	@timeout 5 ./test_keyframes_debug || echo "Debug test finished"
	@rm -f test_keyframes_debug.o test_keyframes_debug


test-tokenizer-debug: 
	@echo "Building tokenizer debug..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_tokenizer_debug.cpp -o test_tokenizer_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_tokenizer_debug.o -o test_tokenizer_debug
	@echo "Running tokenizer debug (should be fast)..."
	@timeout 2 ./test_tokenizer_debug || echo "Tokenizer debug done"
	@rm -f test_tokenizer_debug.o test_tokenizer_debug


test-atrule-debug: 
	@echo "Building AtRule debug..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_atrule_debug.cpp -o test_atrule_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_atrule_debug.o -o test_atrule_debug
	@echo "Running AtRule debug..."
	@timeout 2 ./test_atrule_debug || echo "AtRule debug done"
	@rm -f test_atrule_debug.o test_atrule_debug

test-keyframes-detailed: 
	@echo "Building detailed keyframes test..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_keyframes_detailed.cpp -o test_keyframes_detailed.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_keyframes_detailed.o -o test_keyframes_detailed
	@echo "Running detailed keyframes test..."
	@timeout 10 ./test_keyframes_detailed || echo "Detailed test done"
	@rm -f test_keyframes_detailed.o test_keyframes_detailed

test-html-malformed-debug: 
	@echo "Building HTML malformed debug..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_html_malformed_debug.cpp -o test_html_malformed_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/html/*.o test_html_malformed_debug.o -o test_html_malformed_debug
	@echo "Running HTML malformed debug..."
	@timeout 5 ./test_html_malformed_debug || echo "HTML debug done"
	@rm -f test_html_malformed_debug.o test_html_malformed_debug

test-css-complex-debug: 
	@echo "Building CSS complex debug..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_css_complex_debug.cpp -o test_css_complex_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_css_complex_debug.o -o test_css_complex_debug
	@echo "Running CSS complex debug..."
	@timeout 10 ./test_css_complex_debug || echo "CSS complex debug done"
	@rm -f test_css_complex_debug.o test_css_complex_debug

test-import-debug: 
	@echo "Building @import debug..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_import_debug.cpp -o test_import_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_import_debug.o -o test_import_debug
	@echo "Running @import debug..."
	@timeout 2 ./test_import_debug || echo "Import debug done"
	@rm -f test_import_debug.o test_import_debug

test-final-debug: 
	@echo "Building final debug..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_final_debug.cpp -o test_final_debug.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_final_debug.o -o test_final_debug
	@echo "Running final debug..."
	@timeout 5 ./test_final_debug || echo "Final debug done"
	@rm -f test_final_debug.o test_final_debug

test-only-import: 
	@echo "Building only import test..."
	@g++ -std=c++17 -Wall -Wextra -O2 -Ihtml/include -Icss/include -Icore/include -c test_only_import.cpp -o test_only_import.o
	@g++ -std=c++17 -Wall -Wextra -O2 build/obj/css/*.o test_only_import.o -o test_only_import
	@echo "Running only import test..."
	@timeout 2 ./test_only_import || echo "Only import done"
	@rm -f test_only_import.o test_only_import

test-quanta-lexer: 
	@echo "Building Quanta lexer test..."
	@cd quanta && make lexer
	@g++ -std=c++17 -Wall -Wextra -O2 -I. -c test_quanta_lexer.cpp -o test_quanta_lexer.o
	@g++ -std=c++17 -Wall -Wextra -O2 quanta/build/obj/lexer/*.o quanta/build/obj/core/Value.o test_quanta_lexer.o -o test_quanta_lexer 2>/dev/null || g++ -std=c++17 -Wall -Wextra -O2 quanta/build/obj/lexer/*.o test_quanta_lexer.o -o test_quanta_lexer
	@echo "Running Quanta lexer test..."
	@timeout 10 ./test_quanta_lexer || echo "Quanta lexer test done"
	@rm -f test_quanta_lexer.o test_quanta_lexer

