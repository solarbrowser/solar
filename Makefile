CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -pthread -Iinclude -Itests -isystem third_party/quanta/include -MMD -MP

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

URL_SOURCES = $(wildcard src/url/*.cpp)
URL_OBJECTS = $(URL_SOURCES:%.cpp=$(OBJ_DIR)/%.o)
NET_SOURCES = $(wildcard src/net/*.cpp)
NET_OBJECTS = $(NET_SOURCES:%.cpp=$(OBJ_DIR)/%.o)

# The event loop is io_uring and exists only for Linux so far. What does not touch it (the URL
# library and the HTTP parser) is built and tested everywhere.
UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
LIBURING_LIBS = $(shell pkg-config --libs liburing)
OPENSSL_LIBS = $(shell pkg-config --libs openssl)
NET_LIBS = $(LIBURING_LIBS) $(OPENSSL_LIBS)
endif
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
.PHONY: all test asan-test idna-tables clean quanta

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

PORTABLE_TESTS = UrlTest SearchParamsTest ValidationErrorTest NormalizerTest Http1ParserTest
NET_TESTS = NetTest HttpClientTest TlsTest
LINUX_TESTS = $(NET_TESTS) UrlBindingsTest UrlRealmsTest
ifeq ($(UNAME),Linux)
TESTS = $(PORTABLE_TESTS) $(LINUX_TESTS)
else
TESTS = $(PORTABLE_TESTS)
endif

# SOLAR_LOOP_BACKEND picks the event loop the network tests run on. Linux has two, and both are
# run: io_uring where the kernel has it, and epoll everywhere.
test: $(addprefix $(BUILD_DIR)/,$(TESTS)) $(if $(filter Linux,$(UNAME)),$(BUILD_DIR)/WptTest)
	@for t in $(TESTS); do $(BUILD_DIR)/$$t || exit 1; done
ifeq ($(UNAME),Linux)
	@for t in $(NET_TESTS); do SOLAR_LOOP_BACKEND=readiness $(BUILD_DIR)/$$t || exit 1; done
	@$(BUILD_DIR)/WptTest $(wildcard tests/wpt/url/*.any.js)
endif

# The tests that do not need Quanta, built with AddressSanitizer, UndefinedBehaviorSanitizer and
# leak detection and run. Each is compiled straight from its sources.
ASAN_FLAGS = -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iinclude -Itests -pthread
ASAN_URL = $(URL_SOURCES)
ASAN_NET = $(NET_SOURCES) tests/support/TestServer.cpp
ASAN_TLS = tests/support/TlsTestServer.cpp tests/support/Pki.cpp
asan-test:
	@mkdir -p $(BUILD_DIR)/asan
	@for t in UrlTest SearchParamsTest ValidationErrorTest NormalizerTest; do \
	    echo "[ASAN] $$t"; \
	    $(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/$$t tests/$$t.cpp $(ASAN_URL) && $(BUILD_DIR)/asan/$$t || exit 1; \
	done
	@echo "[ASAN] Http1ParserTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/Http1ParserTest tests/Http1ParserTest.cpp src/net/Http1Parser.cpp && $(BUILD_DIR)/asan/Http1ParserTest
	@for t in $(NET_TESTS); do \
	    extra=""; [ $$t = TlsTest ] && extra="$(ASAN_TLS)"; \
	    $(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/$$t tests/$$t.cpp $(ASAN_NET) $$extra $(ASAN_URL) $(NET_LIBS) || exit 1; \
	    for b in uring readiness; do \
	        echo "[ASAN] $$t on $$b"; \
	        SOLAR_LOOP_BACKEND=$$b $(BUILD_DIR)/asan/$$t || exit 1; \
	    done; \
	done

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
