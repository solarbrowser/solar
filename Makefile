CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -pthread -Iinclude -Itests -isystem third_party/quanta/include -MMD -MP

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

URL_SOURCES = $(wildcard src/url/*.cpp)
URL_OBJECTS = $(URL_SOURCES:%.cpp=$(OBJ_DIR)/%.o)
NET_SOURCES = $(wildcard src/net/*.cpp)
NET_OBJECTS = $(NET_SOURCES:%.cpp=$(OBJ_DIR)/%.o)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
PLATFORM = linux
else ifeq ($(UNAME),Darwin)
PLATFORM = macos
else ifneq (,$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
PLATFORM = windows
else
PLATFORM = other
endif

# Where OpenSSL lives differs by platform, so its flags come from pkg-config. The event loop
# libraries are the platform's own: io_uring on Linux, Winsock on Windows, nothing extra elsewhere.
OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS = $(shell pkg-config --libs openssl 2>/dev/null)
# The content codings. The encoders are only for the tests' servers, but are linked everywhere.
COMPRESSION_PACKAGES = zlib libbrotlidec libbrotlienc libzstd
COMPRESSION_CFLAGS = $(shell pkg-config --cflags $(COMPRESSION_PACKAGES) 2>/dev/null)
COMPRESSION_LIBS = $(shell pkg-config --libs $(COMPRESSION_PACKAGES) 2>/dev/null)
HTTP2_CFLAGS = $(shell pkg-config --cflags libnghttp2 2>/dev/null)
HTTP2_LIBS = $(shell pkg-config --libs libnghttp2 2>/dev/null)
ifeq ($(PLATFORM),linux)
NET_LIBS = $(shell pkg-config --libs liburing) $(OPENSSL_LIBS) $(COMPRESSION_LIBS) $(HTTP2_LIBS)
else ifeq ($(PLATFORM),windows)
NET_LIBS = $(OPENSSL_LIBS) $(COMPRESSION_LIBS) $(HTTP2_LIBS) -lws2_32 -lmswsock
else
NET_LIBS = $(OPENSSL_LIBS) $(COMPRESSION_LIBS) $(HTTP2_LIBS)
endif
CXXFLAGS += $(OPENSSL_CFLAGS) $(COMPRESSION_CFLAGS) $(HTTP2_CFLAGS)

WEB_SOURCES = $(wildcard src/web/*.cpp)
WEB_OBJECTS = $(WEB_SOURCES:%.cpp=$(OBJ_DIR)/%.o) $(OBJ_DIR)/$(GEN_DIR)/StreamsScript.o

# The scripts in src/web/js are compiled into the program as C++ sources made from them.
GEN_DIR = build/gen
STREAMS_JS = $(sort $(wildcard src/web/js/streams-*.js))
$(GEN_DIR)/StreamsScript.cpp: $(STREAMS_JS) $(BUILD_DIR)/EmbedScripts
	@mkdir -p $(GEN_DIR)
	@echo "[EMBED] $@"
	@$(BUILD_DIR)/EmbedScripts Streams $(STREAMS_JS) > $@

$(BUILD_DIR)/EmbedScripts: tools/EmbedScripts.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "[BUILD] $<"
	@$(CXX) -std=c++20 -Wall -Wextra -O2 -o $@ $<

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
PSL_URL = https://publicsuffix.org/list/public_suffix_list.dat
PSL_FILE = $(BUILD_DIR)/psl/public_suffix_list.dat

.DEFAULT_GOAL := all
.PHONY: all test asan-test idna-tables public-suffix-tables clean quanta

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

$(BUILD_DIR)/NetTest: $(OBJ_DIR)/tests/NetTest.o $(OBJ_DIR)/tests/support/TestServer.o $(OBJ_DIR)/tests/support/Compress.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -Itests -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/AddressRaceTest: $(OBJ_DIR)/tests/AddressRaceTest.o $(OBJ_DIR)/src/net/AddressRace.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/HstsTest: $(OBJ_DIR)/tests/HstsTest.o $(OBJ_DIR)/src/net/Hsts.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/ResolverTest: $(OBJ_DIR)/tests/ResolverTest.o $(NET_OBJECTS) $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/LoopTest: $(OBJ_DIR)/tests/LoopTest.o $(NET_OBJECTS) $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/HttpClientTest: $(OBJ_DIR)/tests/HttpClientTest.o $(OBJ_DIR)/tests/support/TestServer.o $(OBJ_DIR)/tests/support/Compress.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/TlsTest: $(OBJ_DIR)/tests/TlsTest.o $(OBJ_DIR)/tests/support/TestServer.o $(OBJ_DIR)/tests/support/TlsTestServer.o $(OBJ_DIR)/tests/support/Pki.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/Http2Test: $(OBJ_DIR)/tests/Http2Test.o $(OBJ_DIR)/tests/support/Http2TestServer.o $(OBJ_DIR)/tests/support/TlsTestServer.o $(OBJ_DIR)/tests/support/Pki.o $(OBJ_DIR)/tests/support/Compress.o $(URL_OBJECTS) $(NET_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/ContentDecoderTest: $(OBJ_DIR)/tests/ContentDecoderTest.o $(OBJ_DIR)/tests/support/Compress.o $(OBJ_DIR)/src/net/ContentDecoder.o $(OBJ_DIR)/src/net/Http1Parser.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(COMPRESSION_LIBS)

$(BUILD_DIR)/PublicSuffixTest: $(OBJ_DIR)/tests/PublicSuffixTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/CookiesTest: $(OBJ_DIR)/tests/CookiesTest.o $(OBJ_DIR)/src/net/Cookies.o $(OBJ_DIR)/src/net/HttpDate.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/HttpCacheTest: $(OBJ_DIR)/tests/HttpCacheTest.o $(OBJ_DIR)/src/net/HttpCache.o $(OBJ_DIR)/src/net/HttpDate.o $(OBJ_DIR)/src/net/Http1Parser.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/FetchHeadersTest: $(OBJ_DIR)/tests/FetchHeadersTest.o $(OBJ_DIR)/src/net/FetchHeaders.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/CorsTest: $(OBJ_DIR)/tests/CorsTest.o $(OBJ_DIR)/src/net/Cors.o $(OBJ_DIR)/src/net/FetchHeaders.o $(OBJ_DIR)/src/net/Http1Parser.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/NormalizerTest: $(OBJ_DIR)/tests/NormalizerTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/ValidationErrorTest: $(OBJ_DIR)/tests/ValidationErrorTest.o $(URL_OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/UrlBindingsTest: $(OBJ_DIR)/tests/UrlBindingsTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(NET_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/UrlRealmsTest: $(OBJ_DIR)/tests/UrlRealmsTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(NET_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/WptTest: $(OBJ_DIR)/tests/WptTest.o $(URL_OBJECTS) $(WEB_OBJECTS) $(NET_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/FetchBindingsTest: $(OBJ_DIR)/tests/FetchBindingsTest.o $(OBJ_DIR)/tests/support/TestServer.o $(URL_OBJECTS) $(WEB_OBJECTS) $(NET_OBJECTS) $(QUANTA_LIBS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(NET_LIBS)

$(BUILD_DIR)/GenIdnaTables: tools/GenIdnaTables.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "[BUILD] $<"
	@$(CXX) -std=c++20 -Wall -Wextra -O2 -o $@ $<

WPT_FETCH = $(wildcard tests/wpt/fetch/api/headers/*.any.js tests/wpt/fetch/api/request/*.any.js tests/wpt/fetch/api/response/*.any.js)
WPT_STREAMS = $(wildcard tests/wpt/streams/*.any.js tests/wpt/streams/piping/*.any.js tests/wpt/streams/readable-byte-streams/*.any.js tests/wpt/streams/readable-streams/*.any.js tests/wpt/streams/transform-streams/*.any.js tests/wpt/streams/writable-streams/*.any.js)
WPT_DOM = $(wildcard tests/wpt/dom/abort/*.any.js tests/wpt/dom/events/*.any.js tests/wpt/webidl/*.any.js tests/wpt/encoding/*.any.js tests/wpt/encoding/streams/*.any.js tests/wpt/FileAPI/blob/*.any.js tests/wpt/FileAPI/file/*.any.js tests/wpt/xhr/formdata/*.any.js)

PORTABLE_TESTS = UrlTest SearchParamsTest ValidationErrorTest NormalizerTest PublicSuffixTest CookiesTest HttpCacheTest FetchHeadersTest CorsTest Http1ParserTest ContentDecoderTest AddressRaceTest HstsTest
NET_TESTS = LoopTest ResolverTest NetTest HttpClientTest TlsTest Http2Test
QUANTA_TESTS = UrlBindingsTest UrlRealmsTest FetchBindingsTest
ifeq ($(PLATFORM),linux)
TESTS = $(PORTABLE_TESTS) $(NET_TESTS) $(QUANTA_TESTS)
else
TESTS = $(PORTABLE_TESTS) $(NET_TESTS)
endif

# SOLAR_LOOP_BACKEND picks the event loop the network tests run on. Linux has two, and both are
# run: io_uring where the kernel has it, and epoll everywhere.
test: $(addprefix $(BUILD_DIR)/,$(TESTS)) $(if $(filter linux,$(PLATFORM)),$(BUILD_DIR)/WptTest)
	@for t in $(TESTS); do $(BUILD_DIR)/$$t || exit 1; done
ifeq ($(PLATFORM),linux)
	@for t in $(NET_TESTS) FetchBindingsTest; do SOLAR_LOOP_BACKEND=readiness $(BUILD_DIR)/$$t || exit 1; done
	@$(BUILD_DIR)/WptTest $(wildcard tests/wpt/url/*.any.js) $(WPT_DOM) $(WPT_FETCH) $(WPT_STREAMS)
	@QUANTA_GC_STRESS=2 $(BUILD_DIR)/WptTest $(WPT_DOM) $(WPT_FETCH) $(WPT_STREAMS) > /dev/null
endif

# The tests that do not need Quanta, built with AddressSanitizer, UndefinedBehaviorSanitizer and
# leak detection and run. Each is compiled straight from its sources.
ASAN_FLAGS = -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iinclude -Itests -pthread
ASAN_URL = $(URL_SOURCES)
ASAN_NET = $(NET_SOURCES) tests/support/TestServer.cpp tests/support/Compress.cpp
ASAN_TLS = tests/support/TlsTestServer.cpp tests/support/Pki.cpp
ASAN_HTTP2 = tests/support/Http2TestServer.cpp
asan-test:
	@mkdir -p $(BUILD_DIR)/asan
	@for t in UrlTest SearchParamsTest ValidationErrorTest NormalizerTest PublicSuffixTest; do \
	    echo "[ASAN] $$t"; \
	    $(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/$$t tests/$$t.cpp $(ASAN_URL) && $(BUILD_DIR)/asan/$$t || exit 1; \
	done
	@echo "[ASAN] HstsTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/HstsTest tests/HstsTest.cpp src/net/Hsts.cpp $(ASAN_URL) && $(BUILD_DIR)/asan/HstsTest
	@echo "[ASAN] CookiesTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/CookiesTest tests/CookiesTest.cpp src/net/Cookies.cpp src/net/HttpDate.cpp $(ASAN_URL) && $(BUILD_DIR)/asan/CookiesTest
	@echo "[ASAN] HttpCacheTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/HttpCacheTest tests/HttpCacheTest.cpp src/net/HttpCache.cpp src/net/HttpDate.cpp src/net/Http1Parser.cpp && $(BUILD_DIR)/asan/HttpCacheTest
	@echo "[ASAN] FetchHeadersTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/FetchHeadersTest tests/FetchHeadersTest.cpp src/net/FetchHeaders.cpp && $(BUILD_DIR)/asan/FetchHeadersTest
	@echo "[ASAN] AddressRaceTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/AddressRaceTest tests/AddressRaceTest.cpp src/net/AddressRace.cpp && $(BUILD_DIR)/asan/AddressRaceTest
	@echo "[ASAN] Http1ParserTest"
	@$(CXX) $(ASAN_FLAGS) -o $(BUILD_DIR)/asan/Http1ParserTest tests/Http1ParserTest.cpp src/net/Http1Parser.cpp && $(BUILD_DIR)/asan/Http1ParserTest
	@echo "[ASAN] ContentDecoderTest"
	@$(CXX) $(ASAN_FLAGS) $(COMPRESSION_CFLAGS) -o $(BUILD_DIR)/asan/ContentDecoderTest tests/ContentDecoderTest.cpp tests/support/Compress.cpp src/net/ContentDecoder.cpp src/net/Http1Parser.cpp $(COMPRESSION_LIBS) && $(BUILD_DIR)/asan/ContentDecoderTest
	@for t in $(NET_TESTS); do \
	    extra=""; [ $$t = TlsTest ] && extra="$(ASAN_TLS)"; [ $$t = Http2Test ] && extra="$(ASAN_TLS) $(ASAN_HTTP2)"; \
	    $(CXX) $(ASAN_FLAGS) $(OPENSSL_CFLAGS) $(COMPRESSION_CFLAGS) -o $(BUILD_DIR)/asan/$$t tests/$$t.cpp $(ASAN_NET) $$extra $(ASAN_URL) $(NET_LIBS) || exit 1; \
	    for b in uring readiness; do \
	        echo "[ASAN] $$t on $$b"; \
	        SOLAR_LOOP_BACKEND=$$b $(BUILD_DIR)/asan/$$t || exit 1; \
	    done; \
	done

# It needs the URL parser to turn the list's names into hosts, but not the list it is making.
$(BUILD_DIR)/GenPublicSuffix: tools/GenPublicSuffix.cpp $(filter-out %/PublicSuffix.o %/PublicSuffixTables.o,$(URL_OBJECTS))
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^

# The list changes daily; regenerate to take a newer one, and commit the result with its version.
public-suffix-tables: $(BUILD_DIR)/GenPublicSuffix
	@mkdir -p $(dir $(PSL_FILE))
	@curl -sfL $(PSL_URL) -o $(PSL_FILE)
	@$(BUILD_DIR)/GenPublicSuffix $(PSL_FILE) > src/url/PublicSuffixTables.cpp
	@echo "[OK] src/url/PublicSuffixTables.cpp"

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
