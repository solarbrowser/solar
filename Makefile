CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -pthread -Iinclude -MMD -MP

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

LIB_SOURCES = $(wildcard src/url/*.cpp)
LIB_OBJECTS = $(LIB_SOURCES:%.cpp=$(OBJ_DIR)/%.o)

# IdnaTables.cpp must be regenerated from the same Unicode version's data files.
UNICODE_VERSION = 18.0.0
UCD_URL = https://www.unicode.org/Public/$(UNICODE_VERSION)
UCD_DIR = $(BUILD_DIR)/ucd

.DEFAULT_GOAL := all
.PHONY: all test idna-tables clean

all: solar

solar: $(OBJ_DIR)/src/main.o $(LIB_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/UrlTest: $(OBJ_DIR)/tests/UrlTest.o $(LIB_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/SearchParamsTest: $(OBJ_DIR)/tests/SearchParamsTest.o $(LIB_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/GenIdnaTables: tools/GenIdnaTables.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "[BUILD] $<"
	@$(CXX) -std=c++20 -Wall -Wextra -O2 -o $@ $<

test: $(BUILD_DIR)/UrlTest $(BUILD_DIR)/SearchParamsTest
	@$(BUILD_DIR)/UrlTest
	@$(BUILD_DIR)/SearchParamsTest

idna-tables: $(BUILD_DIR)/GenIdnaTables
	@mkdir -p $(UCD_DIR)
	@curl -sfL $(UCD_URL)/idna/IdnaMappingTable.txt -o $(UCD_DIR)/IdnaMappingTable.txt
	@for f in UnicodeData.txt DerivedNormalizationProps.txt extracted/DerivedJoiningType.txt extracted/DerivedBidiClass.txt; do \
	    curl -sfL $(UCD_URL)/ucd/$$f -o $(UCD_DIR)/$$(basename $$f) || exit 1; \
	done
	@$(BUILD_DIR)/GenIdnaTables $(UCD_DIR) > src/url/IdnaTables.cpp
	@echo "[OK] src/url/IdnaTables.cpp (Unicode $(UNICODE_VERSION))"

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "[BUILD] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) solar

-include $(LIB_OBJECTS:.o=.d) $(OBJ_DIR)/src/main.d $(OBJ_DIR)/tests/UrlTest.d $(OBJ_DIR)/tests/SearchParamsTest.d
