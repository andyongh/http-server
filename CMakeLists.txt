cmake_minimum_required(VERSION 3.18)
project(httpserver VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── tunables ─────────────────────────────────────────────────────────────
set(HS_RING_SIZE "8192" CACHE STRING
    "Per-conn ring buffer (bytes, must be power-of-2, default 8 KiB)")

# ── sanitizer ────────────────────────────────────────────────────────────
set(SANITIZE "none" CACHE STRING
    "Sanitizer to enable: none | asan | tsan | ubsan")

if(SANITIZE STREQUAL "asan")
    message(STATUS "Sanitizer: AddressSanitizer + UBSan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-optimize-sibling-calls)
    add_link_options  (-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "tsan")
    message(STATUS "Sanitizer: ThreadSanitizer")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options  (-fsanitize=thread)
elseif(SANITIZE STREQUAL "ubsan")
    message(STATUS "Sanitizer: UBSan")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options  (-fsanitize=undefined)
endif()

# ── compiler warnings ─────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-gnu-zero-variadic-macro-arguments
    $<$<CONFIG:Release>:-O3 -march=native>
    $<$<CONFIG:Debug>:  -O0 -g3>
)

# ── dep build paths (populated by scripts/bootstrap.sh) ─────────────────
set(DEPS_BUILD "${CMAKE_SOURCE_DIR}/build/deps")

# ── fsae (single-header – no separate build step needed) ────────────────
#    hs_ae_impl.c defines AE_IMPLEMENTATION; all other TUs include ae.h normally.
#    We create an INTERFACE target so every consumer gets deps/fsae on include path.
add_library(fsae INTERFACE)
target_include_directories(fsae INTERFACE deps/fsae)

# ── llhttp ────────────────────────────────────────────────────────────────
if(IS_DIRECTORY "${DEPS_BUILD}/llhttp")
    add_library(llhttp STATIC IMPORTED)
    set_target_properties(llhttp PROPERTIES
        IMPORTED_LOCATION             "${DEPS_BUILD}/llhttp/lib/libllhttp.a"
        INTERFACE_INCLUDE_DIRECTORIES "${DEPS_BUILD}/llhttp/include")
else()
    set(LLHTTP_BUILD_HTML OFF CACHE BOOL "")
    add_subdirectory(deps/llhttp EXCLUDE_FROM_ALL)
    add_library(llhttp ALIAS llhttp_static)
endif()

# ── jemalloc ─────────────────────────────────────────────────────────────
find_library(JEMALLOC_LIB NAMES jemalloc
    HINTS "${DEPS_BUILD}/jemalloc/lib" NO_DEFAULT_PATH)
if(NOT JEMALLOC_LIB)
    message(FATAL_ERROR "jemalloc not found – run scripts/bootstrap.sh first")
endif()
add_library(jemalloc STATIC IMPORTED)
set_target_properties(jemalloc PROPERTIES
    IMPORTED_LOCATION             "${JEMALLOC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_BUILD}/jemalloc/include")

# ── LuaJIT ───────────────────────────────────────────────────────────────
find_library(LUAJIT_LIB NAMES luajit-5.1 luajit
    HINTS "${DEPS_BUILD}/luajit/lib" NO_DEFAULT_PATH)
if(NOT LUAJIT_LIB)
    message(FATAL_ERROR "LuaJIT not found – run scripts/bootstrap.sh first")
endif()
add_library(luajit STATIC IMPORTED)
set_target_properties(luajit PROPERTIES
    IMPORTED_LOCATION             "${LUAJIT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_BUILD}/luajit/include/luajit-2.1")

# ── httpserver static library ─────────────────────────────────────────────
file(GLOB HS_SRC "src/*.c")
add_library(httpserver STATIC ${HS_SRC})

target_include_directories(httpserver
    PUBLIC  include
    PRIVATE src)

target_compile_definitions(httpserver PRIVATE
    _GNU_SOURCE
    _POSIX_C_SOURCE=200809L
    HS_RING_SIZE=${HS_RING_SIZE})

target_link_libraries(httpserver
    PUBLIC fsae llhttp jemalloc luajit pthread m dl)

# ── examples ──────────────────────────────────────────────────────────────
add_executable(example_hello examples/main.c)
target_link_libraries(example_hello PRIVATE httpserver)

# ── tests ─────────────────────────────────────────────────────────────────
enable_testing()

# test_ring: standalone, no deps except libc
add_executable(test_ring tests/test_ring.c)
target_include_directories(test_ring PRIVATE src)
target_compile_definitions(test_ring PRIVATE
    HS_RING_SIZE=64
    "je_malloc=malloc"
    "je_free=free"
    "je_realloc=realloc"
    "je_strdup=strdup"
    "je_calloc=calloc")
add_test(NAME ring COMMAND test_ring)

# test_queue: MPSC + SPMC thread safety (needs jemalloc + pthread)
add_executable(test_queue tests/test_queue.c)
target_include_directories(test_queue PRIVATE src)
target_link_libraries(test_queue PRIVATE jemalloc pthread)
target_compile_definitions(test_queue PRIVATE _GNU_SOURCE)
add_test(NAME queue COMMAND test_queue)

# test_parser: llhttp error handling (needs full library)
add_executable(test_parser tests/test_parser.c)
target_include_directories(test_parser PRIVATE src include)
target_link_libraries(test_parser PRIVATE httpserver)
target_compile_definitions(test_parser PRIVATE _GNU_SOURCE HS_RING_SIZE=${HS_RING_SIZE})
add_test(NAME parser COMMAND test_parser)

# test_integration: end-to-end HTTP over socketpair
add_executable(test_integration tests/test_integration.c)
target_include_directories(test_integration PRIVATE src include)
target_link_libraries(test_integration PRIVATE httpserver)
target_compile_definitions(test_integration PRIVATE _GNU_SOURCE HS_RING_SIZE=${HS_RING_SIZE})
add_test(NAME integration COMMAND test_integration)
