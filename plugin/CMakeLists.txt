# Legacy module teardown still uses vw_log_set_sink(NULL, NULL). Preinclude source-local adapters that bind anonymous
# removal to the current VLC object and route translation diagnostics before the legacy generic-error switch sees them.
set_source_files_properties(src/vw_whisper_module.c PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/include/vw_translation_failure_preclose_override.h;-include;${CMAKE_CURRENT_SOURCE_DIR}/include/vw_plugin_log_scope_override.h;-include;${CMAKE_CURRENT_SOURCE_DIR}/include/vw_translation_failure_module_override.h"
    COMPILE_DEFINITIONS "VW_PLUGIN_LOG_SCOPE_OVERRIDE=1"
)

add_library(vlc_whisper_plugin SHARED
    src/vw_whisper_module.c
    src/vw_benchmark.c
    src/vw_audio_capture.c
    src/vw_caption_presenter.c
    src/vw_worker_client.c
    src/vw_queue.c
    src/vw_platform_win32.c
    src/vw_platform_linux.c
)

find_package(Threads REQUIRED)

target_compile_definitions(vlc_whisper_plugin PRIVATE
    __PLUGIN__
    MODULE_STRING="vlc_whisper"
    _GNU_SOURCE
)

target_include_directories(vlc_whisper_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../worker/third_party/vlc-3.0.23/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../worker/include
)

target_link_libraries(vlc_whisper_plugin PRIVATE
    vw_protocol
    Threads::Threads
)
if(UNIX AND NOT APPLE)
  target_link_libraries(vlc_whisper_plugin PRIVATE dl)
endif()

if(APPLE)
  # VLC.app resolves libvlccore symbols at load time (plugins link @rpath/libvlccore.dylib but we build
  # without the VLC SDK), so let the linker defer unresolved symbols to dyld. _DARWIN_C_SOURCE restores
  # the BSD/Darwin extensions (LOCAL_PEERCRED, SO_NOSIGPIPE, ...) that -std=c17 strict mode hides.
  target_link_options(vlc_whisper_plugin PRIVATE -Wl,-undefined,dynamic_lookup)
  target_compile_definitions(vlc_whisper_plugin PRIVATE _DARWIN_C_SOURCE)
  set_target_properties(vlc_whisper_plugin PROPERTIES SUFFIX ".dylib")
  # Subtitles-menu toggle: an Objective-C "interface" submodule (see src/vw_macos_menu.m).
  target_sources(vlc_whisper_plugin PRIVATE src/vw_macos_menu.m)
  set_source_files_properties(src/vw_macos_menu.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wno-pedantic")
  target_link_libraries(vlc_whisper_plugin PRIVATE "-framework Cocoa")
endif()

if(WIN32)
    # Windows DLLs require an import library to resolve external symbols.
    # We generate one from a .def file containing the VLC API symbols we use.
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libvlccore.dll.a
        COMMAND ${CMAKE_DLLTOOL} -d ${CMAKE_CURRENT_SOURCE_DIR}/libvlccore.def -l ${CMAKE_CURRENT_BINARY_DIR}/libvlccore.dll.a
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libvlccore.def
    )
    add_custom_target(vlc_core_import DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libvlccore.dll.a)
    add_dependencies(vlc_whisper_plugin vlc_core_import)
    target_link_libraries(vlc_whisper_plugin PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libvlccore.dll.a bcrypt advapi32)
endif()

target_compile_options(vlc_whisper_plugin PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(vlc_whisper_plugin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "vlc_whisper_plugin"
)
