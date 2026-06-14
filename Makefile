# =============================================================================
# Makefile  –  httpserver-lite  (v0.4-lite)
#
# Works on:  Linux (gcc/clang)  |  macOS / Apple Silicon (clang)
#
# Targets
#   make / make all      Release build (library + example)
#   make debug           -g3 -O0
#   make asan            AddressSanitizer + UBSan
#   make tsan            ThreadSanitizer
#   make test            Build & run all test suites
#   make test-asan       Tests under ASAN+UBSan
#   make test-tsan       Queue test under TSan
#   make clean           Remove build/out/
#   make distclean       Remove build/ entirely
#   make help            Show this message
#
# Knobs (command-line overrides)
#   CC=clang                  Compiler
#   HS_RING_SIZE=16384        Ring buffer bytes (power-of-two, default 8192)
#   HS_USE_JEMALLOC=1         Use jemalloc instead of system malloc
#   HS_LOG_LEVEL=HS_LOG_INFO  Compile-time log level filter
# =============================================================================

CC            ?= cc
AR            ?= ar
HS_RING_SIZE  ?= 8192
HS_USE_JEMALLOC ?= 0

DEPS_BUILD   := build/deps
BUILD_DIR    := build/out

# ── detect OS ──────────────────────────────────────────────────────────────
OS := $(shell uname -s)

ifeq ($(OS),Darwin)
    NPROC    := $(shell sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    EXTRA_LDFLAGS  := -Wl,-rpath,$(DEPS_BUILD)/luajit/lib
    TEST_LDFLAGS   :=
    LUAJIT_ENV     := MACOSX_DEPLOYMENT_TARGET=15.0
else
    NPROC    := $(shell nproc 2>/dev/null || echo 4)
    EXTRA_LDFLAGS :=
    TEST_LDFLAGS  :=
    LUAJIT_ENV     :=
endif

# ── include / library paths ────────────────────────────────────────────────
INCS := \
    -Iinclude \
    -Isrc \
    -Ideps/fsae \
    -I$(DEPS_BUILD)/llhttp/include \
    -I$(DEPS_BUILD)/luajit/include/luajit-2.1

LIBS := \
    $(DEPS_BUILD)/llhttp/lib/libllhttp.a \
    $(DEPS_BUILD)/luajit/lib/libluajit-5.1.a \
    -lpthread -lm

# ── optional jemalloc ──────────────────────────────────────────────────────
ifeq ($(HS_USE_JEMALLOC),1)
    INCS += -I$(DEPS_BUILD)/jemalloc/include
    LIBS += $(DEPS_BUILD)/jemalloc/lib/libjemalloc.a
    JEMALLOC_FLAG := -DHS_USE_JEMALLOC
else
    JEMALLOC_FLAG :=
endif

# ── macOS: dl is part of libc, pthread already linked ─────────────────────
ifeq ($(OS),Darwin)
    LIBS += $(EXTRA_LDFLAGS)
else
    LIBS += -ldl
endif

# ── base compiler flags ────────────────────────────────────────────────────
BASE_CFLAGS := \
    -std=c11 \
    -Wall -Wextra -Wpedantic \
    -Wno-unused-parameter \
    -D_GNU_SOURCE \
    -D_POSIX_C_SOURCE=200809L \
    -DHS_RING_SIZE=$(HS_RING_SIZE) \
    $(JEMALLOC_FLAG)

# ── variant flags ──────────────────────────────────────────────────────────
REL_FLAGS  := -O2
DBG_FLAGS  := -g3 -O0 -DDEBUG
ASAN_FLAGS := -g3 -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS := -g3 -O1 -fsanitize=thread

# ── source files ───────────────────────────────────────────────────────────
LIB_SRCS := \
    src/hs_ae_impl.c \
    src/hs_conn.c \
    src/hs_http.c \
    src/hs_listener.c \
    src/hs_log.c \
    src/hs_lua.c \
    src/hs_lua_dir.c \
    src/hs_pool.c \
    src/hs_reactor.c \
    src/hs_server.c

EXAMPLE_SRC  := examples/main.c
EXAMPLE_BIN  := $(BUILD_DIR)/httpserver_example

# ── default target ─────────────────────────────────────────────────────────
.PHONY: all
all: deps $(EXAMPLE_BIN)

# ── build deps ─────────────────────────────────────────────────────────────
.PHONY: deps
deps: $(DEPS_BUILD)/llhttp/lib/libllhttp.a \
      $(DEPS_BUILD)/luajit/lib/libluajit-5.1.a

ifeq ($(HS_USE_JEMALLOC),1)
deps: $(DEPS_BUILD)/jemalloc/lib/libjemalloc.a
endif

$(DEPS_BUILD)/llhttp/lib/libllhttp.a:
	@echo "[deps] building llhttp..."
	@mkdir -p $(DEPS_BUILD)/llhttp
	cd deps/llhttp && npm install && npm run build
	$(MAKE) -C deps/llhttp install PREFIX=$(abspath $(DEPS_BUILD)/llhttp) \
	    CLANG=$(CC) CC=$(CC)

$(DEPS_BUILD)/luajit/lib/libluajit-5.1.a:
	@echo "[deps] building LuaJIT..."
	@mkdir -p $(DEPS_BUILD)/luajit
	$(LUAJIT_ENV) $(MAKE) -C deps/luajit install \
	    PREFIX=$(abspath $(DEPS_BUILD)/luajit) \
	    BUILDMODE=static \
	    CC=$(CC) \
	    XCFLAGS="-DLUAJIT_ENABLE_GC64" \
	    -j$(NPROC)

$(DEPS_BUILD)/jemalloc/lib/libjemalloc.a:
	@echo "[deps] building jemalloc..."
	@mkdir -p $(DEPS_BUILD)/jemalloc
	cd deps/jemalloc && autoconf && \
	    ./configure --prefix=$(abspath $(DEPS_BUILD)/jemalloc) \
	                --disable-debug --with-jemalloc-prefix=je_ \
	                --disable-shared && \
	    $(MAKE) -j$(NPROC) && $(MAKE) install

# ── library archive ────────────────────────────────────────────────────────
LIB_OBJS_REL := $(patsubst src/%.c,$(BUILD_DIR)/rel/%.o,$(LIB_SRCS))

$(BUILD_DIR)/rel/%.o: src/%.c | $(BUILD_DIR)/rel
	$(CC) $(BASE_CFLAGS) $(REL_FLAGS) $(INCS) -c $< -o $@

$(BUILD_DIR)/rel:
	@mkdir -p $@

$(BUILD_DIR)/libhttpserver.a: $(LIB_OBJS_REL)
	$(AR) rcs $@ $^

# ── example binary ─────────────────────────────────────────────────────────
$(EXAMPLE_BIN): $(BUILD_DIR)/libhttpserver.a $(EXAMPLE_SRC) | $(BUILD_DIR)
	$(CC) $(BASE_CFLAGS) $(REL_FLAGS) $(INCS) \
	    $(EXAMPLE_SRC) $< $(LIBS) -o $@

$(BUILD_DIR):
	@mkdir -p $@

# ── debug ──────────────────────────────────────────────────────────────────
.PHONY: debug
debug: CFLAGS_VARIANT := $(DBG_FLAGS)
debug: BUILD_SUBDIR := dbg
debug: _build_example

# ── asan ───────────────────────────────────────────────────────────────────
.PHONY: asan
asan: CFLAGS_VARIANT := $(ASAN_FLAGS)
asan: BUILD_SUBDIR := asan
asan: _build_example

# ── tsan ───────────────────────────────────────────────────────────────────
.PHONY: tsan
tsan: CFLAGS_VARIANT := $(TSAN_FLAGS)
tsan: BUILD_SUBDIR := tsan
tsan: _build_example

# ── helper to build variant example ───────────────────────────────────────
.PHONY: _build_example
_build_example: deps
	@mkdir -p $(BUILD_DIR)/$(BUILD_SUBDIR)
	$(CC) $(BASE_CFLAGS) $(CFLAGS_VARIANT) $(INCS) \
	    $(LIB_SRCS) $(EXAMPLE_SRC) $(LIBS) \
	    -o $(BUILD_DIR)/$(BUILD_SUBDIR)/httpserver_example

# ── tests ──────────────────────────────────────────────────────────────────
TEST_SRCS := \
    tests/test_ring.c \
    tests/test_queue.c \
    tests/test_parser.c \
    tests/test_lua_queue.c \
    tests/test_lua_dir.c \
    tests/test_integration.c

TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

.PHONY: test
test: deps $(TEST_BINS)
	@echo "===== Running all tests ====="
	@rc=0; \
	for t in $(TEST_BINS); do \
	    printf "  %-40s " "$$t"; \
	    if $$t > /tmp/hs_test_out.txt 2>&1; then \
	        echo "PASS"; \
	    else \
	        echo "FAIL"; cat /tmp/hs_test_out.txt; rc=1; \
	    fi; \
	done; \
	if [ -f tests/test_regression.sh ]; then \
	    printf "  %-40s " "tests/test_regression.sh (skipped: no server)"; echo "SKIP"; \
	fi; \
	exit $$rc

$(BUILD_DIR)/tests/test_ring: tests/test_ring.c | $(BUILD_DIR)/tests
	$(CC) $(BASE_CFLAGS) -UHS_RING_SIZE -DHS_RING_SIZE=64 $(REL_FLAGS) $(INCS) $< -o $@

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_SRCS) | $(BUILD_DIR)/tests
	$(CC) $(BASE_CFLAGS) $(REL_FLAGS) $(INCS) \
	    $< $(filter-out tests/%.c, $(LIB_SRCS)) $(LIBS) \
	    $(TEST_LDFLAGS) -o $@

$(BUILD_DIR)/tests:
	@mkdir -p $@

.PHONY: test-asan
test-asan: deps
	@mkdir -p $(BUILD_DIR)/tests-asan
	@rc=0; \
	for src in $(TEST_SRCS); do \
	    name=$$(basename $$src .c); \
	    bin=$(BUILD_DIR)/tests-asan/$$name; \
	    if [ "$$name" = "test_ring" ]; then \
	        $(CC) $(BASE_CFLAGS) -UHS_RING_SIZE -DHS_RING_SIZE=64 $(ASAN_FLAGS) $(INCS) $$src -o $$bin; \
	    else \
	        $(CC) $(BASE_CFLAGS) $(ASAN_FLAGS) $(INCS) \
	            $$src $(filter-out tests/%.c, $(LIB_SRCS)) $(LIBS) \
	            $(TEST_LDFLAGS) -o $$bin; \
	    fi; \
	    printf "  [ASAN] %-35s " $$name; \
	    if $$bin > /tmp/hs_asan_out.txt 2>&1; then \
	        echo "PASS"; \
	    else \
	        echo "FAIL"; cat /tmp/hs_asan_out.txt; rc=1; \
	    fi; \
	done; exit $$rc

.PHONY: test-tsan
test-tsan: deps
	@mkdir -p $(BUILD_DIR)/tests-tsan
	$(CC) $(BASE_CFLAGS) $(TSAN_FLAGS) $(INCS) \
	    tests/test_queue.c $(LIB_SRCS) $(LIBS) \
	    $(TEST_LDFLAGS) -o $(BUILD_DIR)/tests-tsan/test_queue
	$(BUILD_DIR)/tests-tsan/test_queue

# ── clean ──────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: distclean
distclean:
	rm -rf build/

# ── help ───────────────────────────────────────────────────────────────────
.PHONY: help
help:
	@echo "httpserver-lite v0.4-lite build targets:"
	@echo "  make [all]          Release build"
	@echo "  make debug          Debug build (-g3 -O0)"
	@echo "  make asan           AddressSanitizer + UBSan"
	@echo "  make tsan           ThreadSanitizer"
	@echo "  make test           Build & run all tests"
	@echo "  make test-asan      Tests under ASAN+UBSan"
	@echo "  make test-tsan      Queue test under TSan"
	@echo "  make clean          Remove $(BUILD_DIR)"
	@echo "  make distclean      Remove build/"
	@echo ""
	@echo "Knobs:"
	@echo "  CC=clang"
	@echo "  HS_RING_SIZE=16384  (default 8192)"
	@echo "  HS_USE_JEMALLOC=1   (default 0 = system malloc)"