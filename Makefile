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