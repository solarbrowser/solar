CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -pthread -Iinclude -isystem third_party/quanta/include -MMD -MP

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

URL_SOURCES = $(wildcard src/url/*.cpp)
URL_OBJECTS = $(URL_SOURCES:%.cpp=$(OBJ_DIR)/%.o)
WEB_SOURCES = $(wildcard src/web/*.cpp)
WEB_OBJECTS = $(WEB_SOURCES:%.cpp=$(OBJ_DIR)/%.o)

QUANTA_DIR = third_party/quanta
LIBQUANTA = $(QUANTA_DIR)/build/lib/libquanta.a
# Named on the link line, not archived: an archive member nothing references is dropped, so
# only a plain object makes the whole process allocate through mimalloc.
QUANTA_OVERRIDE = $(QUANTA_DIR)/build/lib/quanta_mimalloc_override.o
QUANTA_LIBS = $(LIBQUANTA) $(QUANTA_OVERRIDE)

# IdnaTables.cpp must be regenerated from the same Unicode version's data files.
UNICODE_VERSION = 18.0.0
UCD_URL = https://www.unicode.org/Public/$(UNICODE_VERSION)
UCD_DIR = $(BUILD_DIR)/ucd

.DEFAULT_GOAL := all
.PHONY: all test idna-tables clean quanta

all: solar

quanta:
	@$(MAKE) --no-print-directory -C $(QUANTA_DIR) lib

$(QUANTA_LIBS): | quanta

solar: $(OBJ_DIR)/src/main.o $(URL_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/UrlTest: $(OBJ_DIR)/tests/UrlTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/SearchParamsTest: $(OBJ_DIR)/tests/SearchParamsTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/ValidationErrorTest: $(OBJ_DIR)/tests/ValidationErrorTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/UrlBindingsTest: $(OBJ_DIR)/tests/UrlBindingsTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/WptTest: $(OBJ_DIR)/tests/WptTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/GenIdnaTables: tools/GenIdnaTables.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "[BUILD] $<"
	@$(CXX) -std=c++20 -Wall -Wextra -O2 -o $@ $<

# One process per WPT file: Quanta segfaults after several Runtimes have been created and used
# in one process (tests/wpt/url/url-setters-stripping.any.js six times in a row reproduces it).
test: $(BUILD_DIR)/UrlTest $(BUILD_DIR)/SearchParamsTest $(BUILD_DIR)/ValidationErrorTest $(BUILD_DIR)/UrlBindingsTest $(BUILD_DIR)/WptTest
	@$(BUILD_DIR)/UrlTest
	@$(BUILD_DIR)/SearchParamsTest
	@$(BUILD_DIR)/ValidationErrorTest
	@$(BUILD_DIR)/UrlBindingsTest
	@for f in $(wildcard tests/wpt/url/*.any.js); do $(BUILD_DIR)/WptTest $$f || exit 1; done

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

-include $(wildcard $(OBJ_DIR)/*/*.d $(OBJ_DIR)/*/*/*.d)
