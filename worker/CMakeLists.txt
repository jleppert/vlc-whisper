set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_INSTALL OFF CACHE BOOL "" FORCE)
set(GGML_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ggml's OpenMP (default ON) links OpenMP::OpenMP_CXX, which resolves to the explicit path
# libgomp.dll.a on MinGW — an import lib that adds a libgomp-1.dll runtime dependency no -static
# flag can remove (explicit paths beat driver flags). On Windows the worker exe is
# deployed standalone (ADR-010: statically linked MinGW runtime); a missing libgomp-1.dll would
# kill the spawned worker in the loader before main. ggml computes graphs with its own
# pthread-based ggml_threadpool (n_threads still works), so OpenMP is not needed for the worker.
if(WIN32)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
endif()

# GPU whisper (Vulkan) acceleration — roadmap step 17a, default ON. Development builds may
# transparently degrade to CPU when the Vulkan SDK is unavailable. Official GPU release presets
# set VW_REQUIRE_VULKAN=ON so a missing SDK/glslc is a configuration error instead of silently
# producing a CPU-only artifact under the canonical release preset name.
if(APPLE)
    set(VW_WITH_VULKAN_DEFAULT OFF)  # macOS uses ggml-metal (GGML_METAL defaults ON on Apple)
else()
    set(VW_WITH_VULKAN_DEFAULT ON)
endif()
option(VW_WITH_VULKAN "worker: build whisper.cpp with ggml-vulkan GPU backend" ${VW_WITH_VULKAN_DEFAULT})
option(VW_REQUIRE_VULKAN "worker: fail configure when Vulkan was requested but cannot be enabled" OFF)
option(VW_QUALITY_BENCHMARK_HOOKS "worker: enable developer-only ASR benchmark completion hooks" OFF)
if(VW_REQUIRE_VULKAN AND NOT VW_WITH_VULKAN)
    message(FATAL_ERROR "VW_REQUIRE_VULKAN=ON requires VW_WITH_VULKAN=ON")
endif()
if(VW_QUALITY_BENCHMARK_HOOKS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "VW_QUALITY_BENCHMARK_HOOKS=ON requires BUILD_TESTING=ON")
endif()
if(VW_QUALITY_BENCHMARK_HOOKS AND NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "VW_QUALITY_BENCHMARK_HOOKS requires a GNU/Clang-compatible linker with --wrap support")
endif()

set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
if(VW_WITH_VULKAN)
    # The Windows MinGW toolchain sets FIND_ROOT_PATH_MODE_PACKAGE ONLY, so find_package(Vulkan)
    # can never see the host SDK. Allow an explicit SDK root (postmortem layout: <root>/mingw for
    # libs+headers, <root>/spv for SPIRV-Headers) to be injected into the cross find path:
    #   VW_VULKAN_SDK=/home/razvan/vw-vulkan cmake --preset windows-x64-debug
    if(DEFINED ENV{VW_VULKAN_SDK} AND NOT "$ENV{VW_VULKAN_SDK}" STREQUAL "")
        list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{VW_VULKAN_SDK}/mingw" "$ENV{VW_VULKAN_SDK}/spv")
        list(APPEND CMAKE_PREFIX_PATH "$ENV{VW_VULKAN_SDK}/mingw" "$ENV{VW_VULKAN_SDK}/spv")
        set(Vulkan_INCLUDE_DIR "$ENV{VW_VULKAN_SDK}/mingw/include" CACHE PATH "" FORCE)
        message(STATUS "VW: using Vulkan SDK root '$ENV{VW_VULKAN_SDK}' for cross find path")
    endif()
    # Probe for the SDK + glslc (same COMPONENTS ggml-vulkan will require).
    find_package(Vulkan COMPONENTS glslc)
    if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
        message(STATUS "VW: Vulkan SDK found — enabling ggml-vulkan GPU backend")
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    elseif(VW_REQUIRE_VULKAN)
        message(FATAL_ERROR
            "VW: Vulkan GPU build is required for this preset, but Vulkan SDK/glslc was not found. "
            "Provide VW_VULKAN_SDK (for MinGW cross builds) or use an explicit *-cpu preset.")
    else()
        message(WARNING "VW: VW_WITH_VULKAN=ON but no Vulkan SDK/glslc found — building CPU-only worker")
    endif()
endif()

find_package(Threads REQUIRED)

# whisper.cpp/ggml build with GGML_OPENMP (see above) and link OpenMP::OpenMP_CXX transitively
# on non-Windows; find it at this scope so the worker can also manage the OpenMP runtime.
find_package(OpenMP)

if(APPLE)
    # whisper.cpp/ggml register unconditional install() rules for their static libs, headers and cmake/pkg-config
    # files. The macOS install prefix is the live VLC.app bundle, so keep those rules out of `cmake --install`;
    # EXCLUDE_FROM_ALL only drops the subdirectory's install rules, the worker still links the whisper target.
    add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)
else()
    add_subdirectory(third_party/whisper.cpp)
endif()

# Optimize third-party tensor compute kernels (ggml & whisper) so VAD and Whisper unit tests
# run in milliseconds rather than suffering unvectorized -O0 CPU slowdowns during debug runs.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(t whisper ggml ggml-base ggml-cpu)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -O3)
        endif()
    endforeach()
endif()

if(WIN32)
    set(VW_SOURCE_DECODER_SRCS src/vw_source_decoder_mf.c)
else()
    set(VW_SOURCE_DECODER_SRCS src/vw_source_decoder_ffmpeg.c)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
    endif()
endif()

# Preinclude the POSIX translation pipe policy so original descriptors are CLOEXEC, curl stdin/stdout stay blocking,
# and only parent endpoints become nonblocking after spawn through the same namespaced production/test seam.
if(NOT WIN32)
    set_source_files_properties(src/vw_translate.c PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/include/vw_translate_pipe_policy.h"
        COMPILE_DEFINITIONS "VW_TRANSLATE_PIPE_POLICY_OVERRIDE=1"
    )
endif()

add_executable(vlc-whisper-worker
    src/main.c
    src/vw_worker_log_policy.c
    src/vw_worker.c
    src/vw_whisper_engine.c
    src/vw_vad.c
    src/vw_segment_builder.c
    src/vw_hallucination_filter.c
    src/vw_audio_buffer.c
    src/vw_worker_config.c
    src/vw_worker_queue.c
    src/vw_sha256.c
    src/vw_model_download.c
    src/vw_translate.c
    src/vw_translate_async.c
    ${VW_SOURCE_DECODER_SRCS}
    ../plugin/src/vw_platform_linux.c
    ../plugin/src/vw_platform_win32.c
)

# Explicit opt-in keeps benchmark-only completion hooks out of normal debug/release/package workers.
# When enabled, GNU/Clang and MinGW --wrap intercepts expose only temporary EOF/drop metadata when
# VW_QUALITY_MARKER_PREFIX is set by the benchmark runner; ordinary worker protocol behavior is unchanged.
if(VW_QUALITY_BENCHMARK_HOOKS)
    target_sources(vlc-whisper-worker PRIVATE src/vw_quality_hooks.c)
    target_compile_definitions(vlc-whisper-worker PRIVATE VW_QUALITY_LINK_WRAPS=1)
    target_link_options(vlc-whisper-worker PRIVATE
        -Wl,--wrap=vw_source_decoder_read_s16le
        -Wl,--wrap=vw_worker_queue_push
    )
endif()

if(FFMPEG_FOUND AND NOT WIN32)
    target_compile_definitions(vlc-whisper-worker PRIVATE -DVW_WITH_FFMPEG)
    target_link_libraries(vlc-whisper-worker PRIVATE PkgConfig::FFMPEG)
endif()

target_include_directories(vlc-whisper-worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp/ggml/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vlc-3.0.23/include
)

target_compile_options(vlc-whisper-worker PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
if(APPLE)
    target_compile_definitions(vlc-whisper-worker PRIVATE _DARWIN_C_SOURCE)
endif()

# Dist artifact naming: the GPU (default) build keeps the canonical name the plugin discovers;
# the CPU-only build gets a distinct -cpu suffix so both can ship side by side (see plan 17a).
# On macOS the GPU backend is ggml-metal (whisper.cpp default on Apple), which needs no SDK and
# falls back to CPU inside ggml itself, so the Metal worker keeps the canonical name.
if(GGML_VULKAN)
    set_target_properties(vlc-whisper-worker PROPERTIES OUTPUT_NAME "vlc-whisper-worker")
    target_compile_definitions(vlc-whisper-worker PRIVATE VW_HAVE_VULKAN=1)
elseif(APPLE AND GGML_METAL)
    set_target_properties(vlc-whisper-worker PROPERTIES OUTPUT_NAME "vlc-whisper-worker")
    target_compile_definitions(vlc-whisper-worker PRIVATE VW_HAVE_METAL=1)
else()
    set_target_properties(vlc-whisper-worker PROPERTIES OUTPUT_NAME "vlc-whisper-worker-cpu")
endif()
target_link_libraries(vlc-whisper-worker PRIVATE
    vw_protocol
    whisper
    Threads::Threads
)

if(WIN32)
    # Windows: reserve a bigger stack. The MinGW default of 2 MiB overflows during
    # whisper.cpp model loading (STATUS_STACK_OVERFLOW 0xC00000FD) because GNU ld
    # does not apply the 8 MiB Linux main-thread default. All clean worker exit
    # paths run on the main thread, so this reserve covers them too.
    target_link_options(vlc-whisper-worker PRIVATE -Wl,--stack,16777216)
    # Windows Media Foundation, Crypto, and ACL/SID APIs plus WinHTTP model/translation transport.
    target_link_libraries(vlc-whisper-worker PRIVATE bcrypt mfplat mfreadwrite mfuuid ole32 propsys winhttp advapi32
                                                       shell32)
endif()

if(OpenMP_CXX_FOUND AND NOT WIN32)
    # On Linux ggml still builds with OpenMP; link the runtime here too. On Windows OpenMP is
    # disabled entirely (see GGML_OPENMP above) so the worker has no libgomp DLL dependency.
    target_link_libraries(vlc-whisper-worker PRIVATE OpenMP::OpenMP_CXX)
endif()
