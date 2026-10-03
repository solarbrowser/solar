CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -pthread -Iinclude -Itests -isystem third_party/quanta/include -MMD -MP

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

URL_SOURCES = $(wildcard src/url/*.cpp)
URL_OBJECTS = $(URL_SOURCES:%.cpp=$(OBJ_DIR)/%.o)
NET_SOURCES = $(wildcard src/net/*.cpp)
NET_OBJECTS = $(NET_SOURCES:%.cpp=$(OBJ_DIR)/%.o)
LIBURING_LIBS = $(shell pkg-config --libs liburing)
OPENSSL_LIBS = $(shell pkg-config --libs openssl)
NET_LIBS = $(LIBURING_LIBS) $(OPENSSL_LIBS)
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

solar: $(OBJ_DIR)/src/main.o $(URL_OBJECTS) $(NET_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/UrlTest: $(OBJ_DIR)/tests/UrlTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/SearchParamsTest: $(OBJ_DIR)/tests/SearchParamsTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/Http1ParserTest: $(OBJ_DIR)/tests/Http1ParserTest.o $(OBJ_DIR)/src/net/Http1Parser.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/NetTest: $(OBJ_DIR)/tests/NetTest.o $(OBJ_DIR)/tests/support/TestServer.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -Itests -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/HttpClientTest: $(OBJ_DIR)/tests/HttpClientTest.o $(OBJ_DIR)/tests/support/TestServer.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/TlsTest: $(OBJ_DIR)/tests/TlsTest.o $(OBJ_DIR)/tests/support/TestServer.o $(OBJ_DIR)/tests/support/TlsTestServer.o $(OBJ_DIR)/tests/support/Pki.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/NormalizerTest: $(OBJ_DIR)/tests/NormalizerTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/ValidationErrorTest: $(OBJ_DIR)/tests/ValidationErrorTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/UrlBindingsTest: $(OBJ_DIR)/tests/UrlBindingsTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/UrlRealmsTest: $(OBJ_DIR)/tests/UrlRealmsTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/WptTest: $(OBJ_DIR)/tests/WptTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/GenIdnaTables: tools/GenIdnaTables.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "[BUILD] $<"
	@$(CXX) -std=c++20 -Wall -Wextra -O2 -o $@ $<

test: $(BUILD_DIR)/UrlTest $(BUILD_DIR)/SearchParamsTest $(BUILD_DIR)/ValidationErrorTest $(BUILD_DIR)/NormalizerTest $(BUILD_DIR)/Http1ParserTest $(BUILD_DIR)/NetTest $(BUILD_DIR)/HttpClientTest $(BUILD_DIR)/TlsTest $(BUILD_DIR)/UrlBindingsTest $(BUILD_DIR)/UrlRealmsTest $(BUILD_DIR)/WptTest
	@$(BUILD_DIR)/UrlTest
	@$(BUILD_DIR)/SearchParamsTest
	@$(BUILD_DIR)/ValidationErrorTest
	@$(BUILD_DIR)/NormalizerTest
	@$(BUILD_DIR)/Http1ParserTest
	@$(BUILD_DIR)/NetTest
	@$(BUILD_DIR)/HttpClientTest
	@$(BUILD_DIR)/TlsTest
	@$(BUILD_DIR)/UrlBindingsTest
	@$(BUILD_DIR)/UrlRealmsTest
	@$(BUILD_DIR)/WptTest $(wildcard tests/wpt/url/*.any.js)

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
