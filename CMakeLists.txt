cmake_minimum_required(VERSION 3.20)

project(vlc_whisper VERSION 0.2.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC)  # plugin/src/vw_macos_menu.m (Subtitles-menu toggle); must be enabled at top level
endif()

set(VW_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VW_GENERATED_DIR}/lua/extensions")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vw_winhttp_version_override.h.in"
    "${VW_GENERATED_DIR}/vw_winhttp_version_override.h"
    @ONLY
)

# The checked-in Lua file remains directly runnable for development, while the
# packaged copy is generated from PROJECT_VERSION so release metadata cannot drift.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lua/extensions/vlc_whisper_settings.lua" VW_LUA_SETTINGS_CONTENT)
string(REGEX REPLACE
    "version = \"[0-9]+\\.[0-9]+\\.[0-9]+\""
    "version = \"${PROJECT_VERSION}\""
    VW_LUA_SETTINGS_CONTENT
    "${VW_LUA_SETTINGS_CONTENT}"
)
set(VW_GENERATED_LUA_SETTINGS "${VW_GENERATED_DIR}/lua/extensions/vlc_whisper_settings.lua")
file(WRITE "${VW_GENERATED_LUA_SETTINGS}" "${VW_LUA_SETTINGS_CONTENT}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Option to toggle building unit/integration tests
option(BUILD_TESTING "Build unit and integration tests" ON)

if(APPLE)
    # -std=c17 puts Darwin headers in strict ISO mode; restore the BSD/POSIX extensions the POSIX sources
    # and tests rely on (mkdtemp, LOCAL_PEERCRED, SO_NOSIGPIPE, strdup, ...). Linux gets this via _GNU_SOURCE.
    add_compile_definitions(_DARWIN_C_SOURCE)
    # Prefer the system curl over whatever package manager happens to be first on PATH; the worker spawns
    # this absolute path for model downloads and translation, so it must exist on every Mac.
    find_program(VW_CURL_EXECUTABLE NAMES curl PATHS /usr/bin NO_DEFAULT_PATH)
endif()
if(NOT WIN32)
    find_program(VW_CURL_EXECUTABLE NAMES curl REQUIRED)
    add_compile_definitions(VW_CURL_EXECUTABLE="${VW_CURL_EXECUTABLE}")
endif()

# Option to enable code coverage instrumentation
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
if(ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Configure static runtime linking for MinGW cross-compilation
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Core protocol library
add_subdirectory(protocol)

# Isolated transcription worker executable
add_subdirectory(worker)

# Source-local worker adapter emits recoverable translation blame frames before source-caption fallback delivery without
# exposing the private worker delivery context to the asynchronous translator threads.
set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c TARGET_DIRECTORY vlc-whisper-worker APPEND PROPERTY
             COMPILE_OPTIONS "-include" "${CMAKE_SOURCE_DIR}/worker/include/vw_worker_translation_diag_override.h")
set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c TARGET_DIRECTORY vlc-whisper-worker APPEND PROPERTY
             COMPILE_DEFINITIONS VW_WORKER_TRANSLATION_DIAG_OVERRIDE=1)

# Both Windows HTTP paths inherit the release identity from PROJECT_VERSION even
# though their legacy call sites still pass literal user-agent strings.
if(WIN32)
    foreach(_vw_http_source IN ITEMS vw_translate.c vw_model_download.c)
        set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/${_vw_http_source}
                     TARGET_DIRECTORY vlc-whisper-worker APPEND PROPERTY
                     COMPILE_OPTIONS "-include" "${VW_GENERATED_DIR}/vw_winhttp_version_override.h")
    endforeach()
endif()

# Native VLC plugin module
add_subdirectory(plugin)

# Automated unit & integration tests
if(BUILD_TESTING)
    find_program(VALGRIND_PROGRAM valgrind)
    if(VALGRIND_PROGRAM)
        set(MEMORYCHECK_COMMAND "${VALGRIND_PROGRAM}" CACHE FILEPATH "Valgrind command" FORCE)
        set(MEMORYCHECK_COMMAND_OPTIONS "--leak-check=full --show-leak-kinds=definite --error-exitcode=1 --errors-for-leak-kinds=definite --suppressions=${CMAKE_SOURCE_DIR}/cmake/valgrind.supp" CACHE STRING "" FORCE)
        set(MEMORYCHECK_SUPPRESSIONS_FILE "${CMAKE_SOURCE_DIR}/cmake/valgrind.supp" CACHE FILEPATH "" FORCE)
    endif()
    include(CTest)
    enable_testing()
    add_subdirectory(tests)

    # Tests that compile vw_worker.c directly must use the exact source-local translation diagnostic adapter that the
    # production worker uses, so cross-component tests exercise the macro-expanded submit/delivery path.
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c DIRECTORY ${CMAKE_SOURCE_DIR}/tests APPEND PROPERTY
                 COMPILE_OPTIONS "-include" "${CMAKE_SOURCE_DIR}/worker/include/vw_worker_translation_diag_override.h")
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c DIRECTORY ${CMAKE_SOURCE_DIR}/tests APPEND PROPERTY
                 COMPILE_DEFINITIONS VW_WORKER_TRANSLATION_DIAG_OVERRIDE=1)

    if(VALGRIND_PROGRAM)
        add_custom_target(vw_memcheck_gate
            COMMAND ${CMAKE_COMMAND}
                    -DCTEST_BUILD_DIR=${CMAKE_BINARY_DIR}
                    -P ${CMAKE_SOURCE_DIR}/cmake/vw_memcheck_gate.cmake
            USES_TERMINAL)
    endif()
endif()

# Separate sample snippet targets (marked EXCLUDE_FROM_ALL)
add_subdirectory(samples EXCLUDE_FROM_ALL)

# Developer-only local ASR quality benchmark target (not packaged or built by default)
add_subdirectory(tools/quality_benchmark EXCLUDE_FROM_ALL)

# Packaging and distribution
include(cmake/vw_packaging.cmake)
if(UNIX AND NOT APPLE)
    include(cmake/vw_packaging_linux.cmake)
elseif(APPLE)
    include(cmake/vw_packaging_macos.cmake)
endif()
