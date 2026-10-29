find_package(Threads REQUIRED)

macro(add_vw_test test_name)
    add_executable(${test_name} ${ARGN})
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/protocol/include
        ${CMAKE_SOURCE_DIR}/worker/include
        ${CMAKE_SOURCE_DIR}/plugin/include
        ${CMAKE_SOURCE_DIR}/worker/third_party/vlc-3.0.23/include
        ${CMAKE_SOURCE_DIR}/worker/third_party/whisper.cpp/include
        ${CMAKE_SOURCE_DIR}/worker/third_party/whisper.cpp/ggml/include
        ${CMAKE_SOURCE_DIR}/tests/include
        ${CMAKE_SOURCE_DIR}/tests/support
    )
    target_link_libraries(${test_name} PRIVATE vw_protocol Threads::Threads)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
    # Test assertions contain the checks and side effects; never compile them away in release builds.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endmacro()

# Every test target that compiles vw_translate.c uses the same atomic nonblocking-pipe policy as the production worker.
if(NOT WIN32)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/worker/include/vw_translate_pipe_policy.h"
        COMPILE_DEFINITIONS "VW_TRANSLATE_PIPE_POLICY_OVERRIDE=1"
    )
endif()

add_vw_test(test_protocol_codec unit/test_protocol_codec.c)
add_vw_test(test_protocol_validate unit/test_protocol_validate.c)
add_vw_test(test_protocol_start_failure_paths unit/test_protocol_start_failure_paths.c)
add_vw_test(test_protocol_util unit/test_protocol_util.c)
add_vw_test(test_log unit/test_log.c)
add_vw_test(test_plugin_log_scope unit/test_plugin_log_scope.c)
add_vw_test(test_worker_process_policy unit/test_worker_process_policy.c)
add_vw_test(test_worker_log_pruning unit/test_worker_log_pruning.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_log_policy.c)
add_vw_test(test_translate_pipe_policy unit/test_translate_pipe_policy.c)
add_vw_test(test_queue unit/test_queue.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_queue.c)
add_vw_test(test_audio_capture unit/test_audio_capture.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_audio_capture.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_queue.c)
add_vw_test(test_segment_builder unit/test_segment_builder.c ${CMAKE_SOURCE_DIR}/worker/src/vw_segment_builder.c ${CMAKE_SOURCE_DIR}/worker/src/vw_hallucination_filter.c)
add_vw_test(test_hallucination_filter unit/test_hallucination_filter.c ${CMAKE_SOURCE_DIR}/worker/src/vw_hallucination_filter.c)
add_vw_test(test_worker_queue unit/test_worker_queue.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_queue.c)
add_vw_test(test_caption_timing unit/test_caption_timing.c)
add_vw_test(test_benchmark unit/test_benchmark.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_benchmark.c)
add_vw_test(test_caption_presenter unit/test_caption_presenter.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_caption_presenter.c)
target_compile_definitions(test_caption_presenter PRIVATE __PLUGIN__ MODULE_STRING="vlc_whisper" _GNU_SOURCE)

if(WIN32)
    set(VW_SOURCE_DECODER_SRCS ${CMAKE_SOURCE_DIR}/worker/src/vw_source_decoder_mf.c)
else()
    set(VW_SOURCE_DECODER_SRCS ${CMAKE_SOURCE_DIR}/worker/src/vw_source_decoder_ffmpeg.c)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
    endif()
endif()

add_vw_test(test_source_decoder unit/test_source_decoder.c ${VW_SOURCE_DECODER_SRCS})
if(NOT WIN32 AND FFMPEG_FOUND)
    add_vw_test(vw_test_decoder_boundaries integration/vw_test_decoder_boundaries.c ${VW_SOURCE_DECODER_SRCS})
    target_compile_definitions(vw_test_decoder_boundaries PRIVATE VW_WITH_FFMPEG)
    target_link_libraries(vw_test_decoder_boundaries PRIVATE PkgConfig::FFMPEG)
    target_link_options(vw_test_decoder_boundaries PRIVATE
        -Wl,--wrap=av_seek_frame -Wl,--wrap=swr_init -Wl,--wrap=avformat_open_input)
    foreach(vw_case paths preroll failure)
        add_test(NAME vw_decoder_${vw_case} COMMAND vw_test_decoder_boundaries ${vw_case})
    endforeach()
endif()
if(FFMPEG_FOUND AND NOT WIN32)
    target_compile_definitions(test_source_decoder PRIVATE -DVW_WITH_FFMPEG)
    target_link_libraries(test_source_decoder PRIVATE PkgConfig::FFMPEG)
    # Cross-component regression: millisecond-timebase (Matroska) source audio must accumulate in the look-ahead buffer.
    add_vw_test(test_source_ms_timebase_audio_accumulates integration/test_source_ms_timebase_audio_accumulates.c
                ${VW_SOURCE_DECODER_SRCS} ${CMAKE_SOURCE_DIR}/worker/src/vw_audio_buffer.c)
    target_compile_definitions(test_source_ms_timebase_audio_accumulates PRIVATE VW_WITH_FFMPEG
                               VW_TEST_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(test_source_ms_timebase_audio_accumulates PRIVATE PkgConfig::FFMPEG)
endif()

add_vw_test(test_worker_config unit/test_worker_config.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_config.c)
target_link_libraries(test_worker_config PRIVATE whisper)
add_vw_test(test_worker_config_failure_paths unit/test_worker_config_failure_paths.c
            ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_config.c)
target_link_libraries(test_worker_config_failure_paths PRIVATE whisper)
add_vw_test(test_protocol_edge_contracts unit/test_protocol_edge_contracts.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_config.c)
target_link_libraries(test_protocol_edge_contracts PRIVATE whisper)
add_vw_test(test_audio_buffer unit/test_audio_buffer.c ${CMAKE_SOURCE_DIR}/worker/src/vw_audio_buffer.c)
add_vw_test(test_vad unit/test_vad.c ${CMAKE_SOURCE_DIR}/worker/src/vw_vad.c)
target_link_libraries(test_vad PRIVATE whisper)
add_vw_test(test_vad_trailing_silence unit/vw_test_vad_trailing_silence.c ${CMAKE_SOURCE_DIR}/worker/src/vw_vad.c)

add_vw_test(test_whisper_engine unit/test_whisper_engine.c ${CMAKE_SOURCE_DIR}/worker/src/vw_whisper_engine.c
            ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)
target_link_libraries(test_whisper_engine PRIVATE whisper)
set_tests_properties(test_whisper_engine PROPERTIES SKIP_RETURN_CODE 77)

add_vw_test(test_model_download unit/test_model_download.c ${CMAKE_SOURCE_DIR}/worker/src/vw_sha256.c ${CMAKE_SOURCE_DIR}/worker/src/vw_model_download.c)
if(WIN32)
    target_link_libraries(test_model_download PRIVATE winhttp)
endif()

add_vw_test(test_translate unit/test_translate.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c)
target_compile_definitions(test_translate PRIVATE VW_TRANSLATE_TESTING=1)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_translate PRIVATE -Wframe-larger-than=131072 -Werror=frame-larger-than=131072)
endif()
add_vw_test(vw_test_translate_stack unit/vw_test_translate_stack.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c)
target_compile_definitions(vw_test_translate_stack PRIVATE VW_TRANSLATE_TESTING=1)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vw_test_translate_stack PRIVATE -Wframe-larger-than=131072 -Werror=frame-larger-than=131072)
endif()
if(WIN32)
    target_link_libraries(test_translate PRIVATE winhttp)
    target_link_libraries(vw_test_translate_stack PRIVATE winhttp)
endif()

add_vw_test(test_translate_async unit/test_translate_async.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c
            ${CMAKE_SOURCE_DIR}/worker/src/vw_translate_async.c)
target_compile_definitions(test_translate_async PRIVATE VW_TRANSLATE_TESTING=1)
if(WIN32)
    target_link_libraries(test_translate_async PRIVATE winhttp)
endif()

add_vw_test(test_translation_blame unit/test_translation_blame.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c
            ${CMAKE_SOURCE_DIR}/plugin/src/vw_benchmark.c)
target_compile_definitions(test_translation_blame PRIVATE VW_TRANSLATE_TESTING=1)
if(WIN32)
    target_link_libraries(test_translation_blame PRIVATE winhttp)
endif()

add_vw_test(test_queue_audio_timeline integration/test_queue_audio_timeline.c
            ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_queue.c ${CMAKE_SOURCE_DIR}/worker/src/vw_audio_buffer.c)

add_vw_test(test_worker_ipc integration/test_worker_ipc.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_config.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_queue.c ${CMAKE_SOURCE_DIR}/worker/src/vw_audio_buffer.c ${CMAKE_SOURCE_DIR}/worker/src/vw_segment_builder.c ${CMAKE_SOURCE_DIR}/worker/src/vw_hallucination_filter.c ${CMAKE_SOURCE_DIR}/worker/src/vw_whisper_engine.c ${CMAKE_SOURCE_DIR}/worker/src/vw_vad.c ${CMAKE_SOURCE_DIR}/worker/src/vw_sha256.c ${CMAKE_SOURCE_DIR}/worker/src/vw_model_download.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate_async.c ${VW_SOURCE_DECODER_SRCS} ${CMAKE_SOURCE_DIR}/plugin/src/vw_worker_client.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)
target_link_libraries(test_worker_ipc PRIVATE whisper)
if(FFMPEG_FOUND AND NOT WIN32)
    target_compile_definitions(test_worker_ipc PRIVATE -DVW_WITH_FFMPEG)
    target_link_libraries(test_worker_ipc PRIVATE PkgConfig::FFMPEG)
endif()

if(WIN32)
    target_link_libraries(test_worker_ipc PRIVATE winhttp)
endif()
add_vw_test(test_worker_lifecycle integration/test_worker_lifecycle.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_config.c ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_queue.c ${CMAKE_SOURCE_DIR}/worker/src/vw_audio_buffer.c ${CMAKE_SOURCE_DIR}/worker/src/vw_segment_builder.c ${CMAKE_SOURCE_DIR}/worker/src/vw_hallucination_filter.c ${CMAKE_SOURCE_DIR}/worker/src/vw_whisper_engine.c ${CMAKE_SOURCE_DIR}/worker/src/vw_vad.c ${CMAKE_SOURCE_DIR}/worker/src/vw_sha256.c ${CMAKE_SOURCE_DIR}/worker/src/vw_model_download.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c ${CMAKE_SOURCE_DIR}/worker/src/vw_translate_async.c ${VW_SOURCE_DECODER_SRCS} ${CMAKE_SOURCE_DIR}/plugin/src/vw_worker_client.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)
target_link_libraries(test_worker_lifecycle PRIVATE whisper)
if(FFMPEG_FOUND AND NOT WIN32)
    target_compile_definitions(test_worker_lifecycle PRIVATE -DVW_WITH_FFMPEG)
    target_link_libraries(test_worker_lifecycle PRIVATE PkgConfig::FFMPEG)
endif()

if(WIN32)
    target_link_libraries(test_worker_lifecycle PRIVATE winhttp)
endif()
add_vw_test(test_worker_client unit/vw_test_worker_client.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_worker_client.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)
add_vw_test(test_worker_client_seek_epoch unit/test_worker_client_seek_epoch.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_worker_client.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)
add_vw_test(test_worker_client_compat unit/test_worker_client_compat.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_worker_client.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)

# Failure-path worker seam tests use deterministic test doubles for decoder/VAD/Whisper so they do not need
# model files, real media, VLC GUI, or a platform decoder. Production worker/session code remains unmodified.
set(VW_STUB_WORKER_CONTRACT_SRCS
    ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_config.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_worker_queue.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_audio_buffer.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_segment_builder.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_hallucination_filter.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_sha256.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_model_download.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_translate.c
    ${CMAKE_SOURCE_DIR}/worker/src/vw_translate_async.c
    ${CMAKE_SOURCE_DIR}/plugin/src/vw_worker_client.c
    ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c
    ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c
    ${CMAKE_SOURCE_DIR}/tests/support/vw_test_worker_stubs.c)

function(add_vw_stub_worker_contract_test test_name source_file)
    add_vw_test(${test_name} ${source_file} ${VW_STUB_WORKER_CONTRACT_SRCS})
    target_link_libraries(${test_name} PRIVATE whisper)
    if(WIN32)
        target_link_libraries(${test_name} PRIVATE winhttp bcrypt mfplat mfreadwrite mfuuid ole32 propsys)
    endif()
endfunction()

add_vw_stub_worker_contract_test(test_decoder_again_never_becomes_eof
                                 integration/test_decoder_again_never_becomes_eof.c)
add_vw_stub_worker_contract_test(test_source_error_never_becomes_clean_eof
                                 integration/test_source_error_never_becomes_clean_eof.c)
add_vw_stub_worker_contract_test(test_live_media_end_flushes_tail
                                 integration/test_live_media_end_flushes_tail.c)
target_link_options(test_live_media_end_flushes_tail PRIVATE -Wl,--wrap=vw_whisper_engine_transcribe_pcm)
target_compile_definitions(test_live_media_end_flushes_tail PRIVATE VW_TRANSLATE_TESTING)
target_sources(test_live_media_end_flushes_tail PRIVATE ${CMAKE_SOURCE_DIR}/plugin/src/vw_benchmark.c)
set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c TARGET_DIRECTORY test_live_media_end_flushes_tail APPEND PROPERTY
             COMPILE_OPTIONS "-include" "${CMAKE_SOURCE_DIR}/worker/include/vw_worker_translation_diag_override.h")
set_property(SOURCE ${CMAKE_SOURCE_DIR}/worker/src/vw_worker.c TARGET_DIRECTORY test_live_media_end_flushes_tail APPEND PROPERTY
             COMPILE_DEFINITIONS VW_WORKER_TRANSLATION_DIAG_OVERRIDE=1)
add_test(NAME vw_tail_translation_timeout COMMAND test_live_media_end_flushes_tail timeout)
add_vw_stub_worker_contract_test(test_new_session_resets_all_session_state
                                 integration/test_new_session_resets_all_session_state.c)
add_vw_stub_worker_contract_test(test_worker_p2_contracts
                                 integration/vw_test_worker_p2_contracts.c)
add_vw_stub_worker_contract_test(test_paused_source_preview
                                 integration/test_paused_source_preview.c)
target_sources(test_paused_source_preview PRIVATE ${CMAKE_SOURCE_DIR}/plugin/src/vw_caption_presenter.c)
target_compile_definitions(test_paused_source_preview PRIVATE __PLUGIN__ MODULE_STRING="vlc_whisper" _GNU_SOURCE)

add_vw_test(test_oversized_uri_rejected_before_truncation unit/test_oversized_uri_rejected_before_truncation.c)
target_compile_definitions(test_oversized_uri_rejected_before_truncation
                           PRIVATE VW_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

# The quality benchmark currently lives on its feature branch, not main. Keep its failure contracts registered now;
# they return 77 while the feature is absent and automatically become active when the tool lands on this branch.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_benchmark_rejects_empty_reference
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/quality/test_quality_benchmark_failure_paths.py
                     empty-reference)
    add_test(NAME test_benchmark_rejects_fixture_hash_mismatch
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/quality/test_quality_benchmark_failure_paths.py
                     hash-mismatch)
    set_tests_properties(test_benchmark_rejects_empty_reference test_benchmark_rejects_fixture_hash_mismatch
                         PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(WIN32)
    add_vw_test(test_platform unit/test_platform.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_win32.c)
else()
    add_vw_test(test_platform unit/test_platform.c ${CMAKE_SOURCE_DIR}/plugin/src/vw_platform_linux.c)
endif()

if(WIN32)
    # Windows Media Foundation and BCrypt libraries
    target_link_libraries(test_whisper_engine PRIVATE bcrypt)
    target_link_libraries(test_source_decoder PRIVATE bcrypt mfplat mfreadwrite mfuuid ole32 propsys)
    target_link_libraries(test_worker_ipc PRIVATE bcrypt mfplat mfreadwrite mfuuid ole32 propsys)
    target_link_libraries(test_worker_lifecycle PRIVATE bcrypt mfplat mfreadwrite mfuuid ole32 propsys)
    target_link_libraries(test_worker_client PRIVATE bcrypt)
    target_link_libraries(test_worker_client_seek_epoch PRIVATE bcrypt)
    target_link_libraries(test_worker_client_compat PRIVATE bcrypt)
    target_link_libraries(test_platform PRIVATE bcrypt)
endif()

add_executable(test_plugin_load integration/test_plugin_load.c)
target_compile_options(test_plugin_load PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_plugin_load PRIVATE -UNDEBUG)
endif()
target_link_libraries(test_plugin_load PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME test_plugin_load COMMAND test_plugin_load $<TARGET_FILE:vlc_whisper_plugin>)

# Transport framing regressions: preserve seqpacket/message framing while chunking legal large payloads.
if(NOT WIN32)
    add_vw_test(vw_test_ipc_transport unit/vw_test_ipc_transport.c)
endif()

if(VALGRIND_PROGRAM)
    get_property(_vw_tests DIRECTORY PROPERTY TESTS)
    if(_vw_tests)
        set_tests_properties(${_vw_tests} PROPERTIES ENVIRONMENT "VW_FORCE_CPU=1")
    endif()
endif()
