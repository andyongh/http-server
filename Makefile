# =============================================================================
# Makefile  –  httpserver
#
# Works on:  Linux (gcc/clang)  |  macOS / Apple Silicon (clang)
#
# Targets
#   make / make all      Release build (library + example)
#   make debug           -g3 -O0
#   make asan            AddressSanitizer + UBSan
#   make tsan            ThreadSanitizer
#   make test            Build & run all four test suites
#   make test-asan       Tests under ASAN+UBSan
#   make test-tsan       Queue test under TSan
#   make clean           Remove build/out/
#   make distclean       Remove build/ entirely
#   make help            Show this message
#
# Knobs (command-line overrides)
#   CC=clang             Compiler
#   HS_RING_SIZE=16384   Ring buffer bytes (power-of-two, default 8192)
# =============================================================================

CC           ?= cc
AR           ?= ar
HS_RING_SIZE ?= 8192

DEPS_BUILD   := build/deps
BUILD_DIR    := build/out

# ── detect OS ─────────────────────────────────────────────────────────────
OS := $(shell uname -s)

ifeq ($(OS),Darwin)
    NPROC    := $(shell sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    # macOS: LuaJIT and jemalloc may need flat_namespace or extra flags
    EXTRA_LDFLAGS  := -Wl,-rpath,$(DEPS_BUILD)/luajit/lib
    # macOS linker is strict about duplicate symbols; tests provide stubs
    # that intentionally shadow library symbols.
    # The test .c is compiled first so its symbols take precedence over the
    # archive (libraries are searched in order); no extra linker flag needed.
    TEST_LDFLAGS   :=
else
    NPROC    := $(shell nproc 2>/dev/null || echo 4)
    EXTRA_LDFLAGS :=
    TEST_LDFLAGS  :=
endif

# ── include / library paths ───────────────────────────────────────────────
INCS := \
    -Iinclude \
    -Isrc \
    -Ideps/fsae \
    -I$(DEPS_BUILD)/jemalloc/include \
    -I$(DEPS_BUILD)/llhttp/include \
    -I$(DEPS_BUILD)/luajit/include/luajit-2.1

LIBS := \
    $(DEPS_BUILD)/jemalloc/lib/libjemalloc.a \
    $(DEPS_BUILD)/llhttp/lib/libllhttp.a \
    $(DEPS_BUILD)/luajit/lib/libluajit-5.1.a \
    -lpthread -lm -ldl

# ── macOS: dl is part of libc, pthread already linked ────────────────────
ifeq ($(OS),Darwin)
    LIBS := $(filter-out -ldl,$(LIBS))
    LIBS += $(EXTRA_LDFLAGS)
endif

# ── base compiler flags ───────────────────────────────────────────────────
BASE_CFLAGS := \
    -std=c11 \
    -Wall -Wextra -Wpedantic \
    -Wno-unused-parameter \
    -D_GNU_SOURCE \
    -D_POSIX_C_SOURCE=200809L \
    -DHS_RING_SIZE=$(HS_RING_SIZE)

# macOS: _GNU_SOURCE may warn; suppress with _DARWIN_C_SOURCE
ifeq ($(OS),Darwin)
    BASE_CFLAGS += -D_DARWIN_C_SOURCE
endif

RELEASE_CFLAGS  := $(BASE_CFLAGS) -O3
DEBUG_CFLAGS    := $(BASE_CFLAGS) -O0 -g3
ASAN_CFLAGS     := $(BASE_CFLAGS) -O1 -g3 \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -fno-optimize-sibling-calls
TSAN_CFLAGS     := $(BASE_CFLAGS) -O1 -g3 \
    -fsanitize=thread \
    -fno-omit-frame-pointer

# Note: -march=native is omitted on macOS ARM to avoid clang warnings;
# the compiler already optimises for the host with default flags on Apple Silicon
ifneq ($(OS),Darwin)
    RELEASE_CFLAGS += -march=native
endif

ASAN_LDFLAGS := -fsanitize=address,undefined
TSAN_LDFLAGS := -fsanitize=thread

# ── jemalloc shim (used for standalone tests without libjemalloc) ─────────
JEMALLOC_SHIM_DIR := $(BUILD_DIR)/jemalloc_shim/jemalloc
JEMALLOC_SHIM     := $(JEMALLOC_SHIM_DIR)/jemalloc.h

# ── source files ──────────────────────────────────────────────────────────
LIB_SRCS := $(wildcard src/*.c)
LIB_HDRS := $(wildcard src/*.h) $(wildcard include/*.h)

.PHONY: all debug asan tsan test test-asan test-tsan clean distclean help \
        _jemalloc_shim

# ══════════════════════════════════════════════════════════════════════════
# Default: Release
# ══════════════════════════════════════════════════════════════════════════
all: $(BUILD_DIR)/release/example_hello
	@echo "Build OK → $<"

$(BUILD_DIR)/release/%.o: src/%.c $(LIB_HDRS)
	@mkdir -p $(@D)
	$(CC) $(RELEASE_CFLAGS) $(INCS) -c $< -o $@

$(BUILD_DIR)/release/libhttpserver.a: \
        $(patsubst src/%.c,$(BUILD_DIR)/release/%.o,$(LIB_SRCS))
	$(AR) rcs $@ $^

$(BUILD_DIR)/release/example_hello: \
        examples/main.c $(BUILD_DIR)/release/libhttpserver.a
	$(CC) $(RELEASE_CFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/release/libhttpserver.a $(LIBS) -o $@

# ══════════════════════════════════════════════════════════════════════════
# Debug
# ══════════════════════════════════════════════════════════════════════════
debug: $(BUILD_DIR)/debug/example_hello
	@echo "Debug build OK → $<"

$(BUILD_DIR)/debug/%.o: src/%.c $(LIB_HDRS)
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) $(INCS) -c $< -o $@

$(BUILD_DIR)/debug/libhttpserver.a: \
        $(patsubst src/%.c,$(BUILD_DIR)/debug/%.o,$(LIB_SRCS))
	$(AR) rcs $@ $^

$(BUILD_DIR)/debug/example_hello: \
        examples/main.c $(BUILD_DIR)/debug/libhttpserver.a
	$(CC) $(DEBUG_CFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/debug/libhttpserver.a $(LIBS) -o $@

# ══════════════════════════════════════════════════════════════════════════
# ASAN
# ══════════════════════════════════════════════════════════════════════════
asan: $(BUILD_DIR)/asan/example_hello
	@echo "ASAN build OK → $<"

$(BUILD_DIR)/asan/%.o: src/%.c $(LIB_HDRS)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCS) -c $< -o $@

$(BUILD_DIR)/asan/libhttpserver.a: \
        $(patsubst src/%.c,$(BUILD_DIR)/asan/%.o,$(LIB_SRCS))
	$(AR) rcs $@ $^

$(BUILD_DIR)/asan/example_hello: \
        examples/main.c $(BUILD_DIR)/asan/libhttpserver.a
	$(CC) $(ASAN_CFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/asan/libhttpserver.a $(LIBS) $(ASAN_LDFLAGS) -o $@

# ══════════════════════════════════════════════════════════════════════════
# TSan
# ══════════════════════════════════════════════════════════════════════════
tsan: $(BUILD_DIR)/tsan/example_hello
	@echo "TSan build OK → $<"

$(BUILD_DIR)/tsan/%.o: src/%.c $(LIB_HDRS)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCS) -c $< -o $@

$(BUILD_DIR)/tsan/libhttpserver.a: \
        $(patsubst src/%.c,$(BUILD_DIR)/tsan/%.o,$(LIB_SRCS))
	$(AR) rcs $@ $^

$(BUILD_DIR)/tsan/example_hello: \
        examples/main.c $(BUILD_DIR)/tsan/libhttpserver.a
	$(CC) $(TSAN_CFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/tsan/libhttpserver.a $(LIBS) $(TSAN_LDFLAGS) -o $@

# ══════════════════════════════════════════════════════════════════════════
# jemalloc shim (in-memory header for standalone tests)
# ══════════════════════════════════════════════════════════════════════════
_jemalloc_shim: $(JEMALLOC_SHIM)

$(JEMALLOC_SHIM):
	@mkdir -p $(JEMALLOC_SHIM_DIR)
	@printf '#pragma once\n#include <stdlib.h>\n#include <string.h>\n\
static inline void *je_malloc(size_t s)           { return malloc(s); }\n\
static inline void  je_free(void *p)              { free(p); }\n\
static inline void *je_realloc(void *p, size_t s) { return realloc(p,s); }\n\
static inline char *je_strdup(const char *s)      { return strdup(s); }\n\
static inline void *je_calloc(size_t n, size_t s) { return calloc(n,s); }\n' \
	    > $@

# ══════════════════════════════════════════════════════════════════════════
# Tests (Release mode, no sanitizer)
# ══════════════════════════════════════════════════════════════════════════

# test_ring – standalone, zero deps
$(BUILD_DIR)/tests/test_ring: tests/test_ring.c src/hs_ring.h _jemalloc_shim
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -g \
	    -DHS_RING_SIZE=64 \
	    -D_GNU_SOURCE \
	    -Isrc -I$(JEMALLOC_SHIM_DIR)/.. \
	    $< -o $@

# test_queue – MPSC/SPMC thread safety
$(BUILD_DIR)/tests/test_queue: tests/test_queue.c src/hs_queue.h _jemalloc_shim
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -g \
	    -D_GNU_SOURCE \
	    -Isrc -I$(JEMALLOC_SHIM_DIR)/.. \
	    $< -lpthread -o $@

# test_parser – llhttp error paths (needs full library in debug mode)
$(BUILD_DIR)/tests/test_parser: \
        tests/test_parser.c $(BUILD_DIR)/debug/libhttpserver.a
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/debug/libhttpserver.a $(LIBS) $(TEST_LDFLAGS) -o $@

# test_integration – end-to-end HTTP over TCP
$(BUILD_DIR)/tests/test_integration: \
        tests/test_integration.c $(BUILD_DIR)/debug/libhttpserver.a
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/debug/libhttpserver.a $(LIBS) $(TEST_LDFLAGS) -o $@

TEST_BINS := \
    $(BUILD_DIR)/tests/test_ring \
    $(BUILD_DIR)/tests/test_queue \
    $(BUILD_DIR)/tests/test_parser \
    $(BUILD_DIR)/tests/test_integration

test: $(TEST_BINS)
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo "  Running test suite"
	@echo "══════════════════════════════════════════════════════"
	@fail=0; pass=0; \
	for t in $(TEST_BINS); do \
	    printf "  %-38s " "$$(basename $$t)"; \
	    if "$$t" > $(BUILD_DIR)/_hs_test 2>&1; then \
	        echo "PASS"; pass=$$((pass+1)); \
	    else \
	        echo "FAIL"; fail=$$((fail+1)); cat $(BUILD_DIR)/_hs_test; \
	    fi; \
	done; \
	echo "══════════════════════════════════════════════════════"; \
	echo "  passed=$$pass  failed=$$fail"; \
	test $$fail -eq 0

# ══════════════════════════════════════════════════════════════════════════
# ASAN test variants
# ══════════════════════════════════════════════════════════════════════════
$(BUILD_DIR)/tests/asan/test_ring: tests/test_ring.c src/hs_ring.h _jemalloc_shim
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -g $(ASAN_CFLAGS) $(ASAN_LDFLAGS) \
	    -DHS_RING_SIZE=64 -D_GNU_SOURCE \
	    -Isrc -I$(JEMALLOC_SHIM_DIR)/.. $< -o $@

$(BUILD_DIR)/tests/asan/test_queue: tests/test_queue.c src/hs_queue.h _jemalloc_shim
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -g $(ASAN_CFLAGS) $(ASAN_LDFLAGS) \
	    -D_GNU_SOURCE \
	    -Isrc -I$(JEMALLOC_SHIM_DIR)/.. $< -lpthread -o $@

$(BUILD_DIR)/tests/asan/test_parser: \
        tests/test_parser.c $(BUILD_DIR)/asan/libhttpserver.a
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(ASAN_LDFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/asan/libhttpserver.a $(LIBS) -o $@

$(BUILD_DIR)/tests/asan/test_integration: \
        tests/test_integration.c $(BUILD_DIR)/asan/libhttpserver.a
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(ASAN_LDFLAGS) $(INCS) \
	    $< $(BUILD_DIR)/asan/libhttpserver.a $(LIBS) -o $@

ASAN_TEST_BINS := \
    $(BUILD_DIR)/tests/asan/test_ring \
    $(BUILD_DIR)/tests/asan/test_queue \
    $(BUILD_DIR)/tests/asan/test_parser \
    $(BUILD_DIR)/tests/asan/test_integration

# On macOS, LSAN is not supported (part of ASAN on Linux only)
ifeq ($(OS),Darwin)
ASAN_OPTS := ASAN_OPTIONS=detect_stack_use_after_return=1:halt_on_error=1
UBSAN_OPTS := UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
else
ASAN_OPTS := ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1
UBSAN_OPTS := UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
endif

test-asan: asan $(ASAN_TEST_BINS)
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo "  Tests under ASAN+UBSan"
	@echo "══════════════════════════════════════════════════════"
	@fail=0; pass=0; \
	for t in $(ASAN_TEST_BINS); do \
	    printf "  %-38s " "$$(basename $$t)"; \
	    if env $(ASAN_OPTS) $(UBSAN_OPTS) "$$t" > $(BUILD_DIR)/_hs_test 2>&1; then \
	        echo "PASS"; pass=$$((pass+1)); \
	    else \
	        echo "FAIL"; fail=$$((fail+1)); cat $(BUILD_DIR)/_hs_test; \
	    fi; \
	done; \
	echo "══════════════════════════════════════════════════════"; \
	echo "  passed=$$pass  failed=$$fail"; \
	test $$fail -eq 0

# ══════════════════════════════════════════════════════════════════════════
# TSan test variant (queue only – thread-safety focus)
# ══════════════════════════════════════════════════════════════════════════
$(BUILD_DIR)/tests/tsan/test_queue: tests/test_queue.c src/hs_queue.h _jemalloc_shim
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -g $(TSAN_CFLAGS) $(TSAN_LDFLAGS) \
	    -D_GNU_SOURCE \
	    -Isrc -I$(JEMALLOC_SHIM_DIR)/.. $< -lpthread -o $@

test-tsan: $(BUILD_DIR)/tests/tsan/test_queue
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo "  Queue test under ThreadSanitizer"
	@echo "══════════════════════════════════════════════════════"
	@TSAN_OPTIONS=halt_on_error=1 \
	    $(BUILD_DIR)/tests/tsan/test_queue && echo "PASS" || echo "FAIL"

# ══════════════════════════════════════════════════════════════════════════
clean:
	rm -rf $(BUILD_DIR)

distclean:
	rm -rf build

help:
	@head -25 $(MAKEFILE_LIST)