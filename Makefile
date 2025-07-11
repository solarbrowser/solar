# HTML5 Parser Enhanced Makefile
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
DEBUG_FLAGS = -std=c++17 -Wall -Wextra -g -DDEBUG -Iinclude
TARGET = html5parser
SRCDIR = src
OBJDIR = obj
INCDIR = include

# Source files
SOURCES = $(wildcard $(SRCDIR)/*.cpp $(SRCDIR)/*/*.cpp)
OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)

# Default target
all: $(TARGET)

# Debug build
debug: CXXFLAGS = $(DEBUG_FLAGS)
debug: $(TARGET)

# Main target
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJECTS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

# Object file compilation
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Test with default file
test: $(TARGET)
	@echo "Running parser test..."
	./$(TARGET) test.html

# Test with custom file
test-file: $(TARGET)
	@read -p "Enter HTML file path: " file; \
	./$(TARGET) "$$file"

# Performance test
perf: $(TARGET)
	@echo "Running performance test..."
	time ./$(TARGET) test.html > /dev/null

# Memory check (requires valgrind)
memcheck: $(TARGET)
	@echo "Running memory check..."
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET) test.html

# Install target
install: $(TARGET)
	@echo "Installing $(TARGET) to /usr/local/bin/"
	sudo cp $(TARGET) /usr/local/bin/
	sudo chmod +x /usr/local/bin/$(TARGET)

# Create example files
examples:
	@echo "Creating example HTML files..."
	@mkdir -p examples
	@echo '<!DOCTYPE html><html><head><title>Simple</title></head><body><h1>Hello</h1></body></html>' > examples/simple.html
	@echo '<!DOCTYPE html><html><body><div><p>Nested <span>content</span></p></div></body></html>' > examples/nested.html
	@echo '<!DOCTYPE html><html><body><!-- Comment --><p>Text</p></body></html>' > examples/comment.html

# Documentation
docs:
	@echo "HTML5 Parser Usage:"
	@echo "  make           - Build the parser"
	@echo "  make debug     - Build with debug symbols"
	@echo "  make test      - Test with test.html"
	@echo "  make test-file - Test with custom file"
	@echo "  make perf      - Performance benchmark"
	@echo "  make memcheck  - Memory leak check"
	@echo "  make examples  - Create example files"
	@echo "  make clean     - Remove build files"
	@echo "  make install   - Install system-wide"

# Clean build files
clean:
	@echo "Cleaning build files..."
	rm -rf $(OBJDIR) $(TARGET)
	@echo "Clean complete"

# Clean everything including examples
clean-all: clean
	rm -rf examples

# Help target
help: docs

# Phony targets
.PHONY: all debug test test-file perf memcheck install examples docs clean clean-all help

# Show variables (debug)
show-vars:
	@echo "CXX: $(CXX)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "SOURCES: $(SOURCES)"
	@echo "OBJECTS: $(OBJECTS)"
	@echo "TARGET: $(TARGET)"
