# macOS (VLC.app bundle) install rules. Runtime architecture remains plugin -> IPC -> worker.
#
# CMAKE_INSTALL_PREFIX is expected to be the bundle's executable directory, normally
# /Applications/VLC.app/Contents/MacOS (the macos-arm64-release preset sets this). VLC.app loads every
# *.dylib in <prefix>/plugins, its data dir is <prefix>/share, and the plugin discovers the worker and
# bundled models by probing ancestors of its own path, so the layout is:
#
#   <prefix>/plugins/libvlc_whisper_plugin.dylib
#   <prefix>/vlc-whisper-worker              (Metal build; -cpu suffix for GGML_METAL=OFF builds)
#   <prefix>/share/models/{ggml-tiny.bin,ggml-silero-vad.bin,manifest.json}
#   <prefix>/models -> share/models          (worker/plugin probe <exe_dir>/models, Lua uses datadir/models)
#   <prefix>/share/lua/extensions/vlc_whisper_settings.lua
#
# VLC.app ships with com.apple.security.cs.disable-library-validation, so an ad-hoc (linker) signed
# plugin loads; the install step still re-signs ad-hoc so a stripped or edited binary stays valid.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  message(WARNING "VW: macOS packaging has only been exercised on Apple Silicon (host: ${CMAKE_SYSTEM_PROCESSOR})")
endif()

set(VW_MACOS_VLC_PLUGIN_DIR "plugins")
set(VW_MACOS_VLC_LUA_DIR "share/lua/extensions")
set(VW_MACOS_MODEL_DIR "share/models")

option(VW_PROVISION_MODELS "Allow build-time model download (package/provision targets)" OFF)
add_custom_target(provision_models
  COMMAND ${CMAKE_COMMAND}
          -DMODEL_PATH=${VW_MODEL_TINY}
          -DMODEL_URL=${VW_MODEL_TINY_URL}
          -DMODEL_SHA256=${VW_MODEL_TINY_SHA256}
          -DALLOW_DOWNLOAD=${VW_PROVISION_MODELS}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vw_provision_model.cmake
  COMMAND ${CMAKE_COMMAND}
          -DMODEL_PATH=${VW_MODEL_VAD}
          -DMODEL_URL=${VW_MODEL_VAD_URL}
          -DMODEL_SHA256=${VW_MODEL_VAD_SHA256}
          -DALLOW_DOWNLOAD=${VW_PROVISION_MODELS}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vw_provision_model.cmake
  COMMENT "Verifying/provisioning pinned Whisper tiny and Silero VAD release models..."
  VERBATIM
)

install(TARGETS vlc_whisper_plugin
  LIBRARY DESTINATION "${VW_MACOS_VLC_PLUGIN_DIR}"
)
install(TARGETS vlc-whisper-worker
  RUNTIME DESTINATION "."
)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/models/manifest.json"
  "${VW_MODEL_TINY}"
  "${VW_MODEL_VAD}"
  DESTINATION "${VW_MACOS_MODEL_DIR}"
)
install(FILES
  "${VW_GENERATED_LUA_SETTINGS}"
  DESTINATION "${VW_MACOS_VLC_LUA_DIR}"
)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
  DESTINATION "share/doc/vlc-whisper"
)

# Runtime discovery expects models adjacent to the worker, while the Lua extension uses VLC's data
# directory. One relative symlink satisfies both without duplicating ~75 MB of model data.
install(CODE "
  set(_vw_models_link \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/models\")
  if(IS_SYMLINK \"\${_vw_models_link}\")
    file(READ_SYMLINK \"\${_vw_models_link}\" _vw_existing_target)
    if(NOT _vw_existing_target STREQUAL \"${VW_MACOS_MODEL_DIR}\")
      message(FATAL_ERROR \"VW: existing models symlink points to an unexpected location\")
    endif()
  elseif(EXISTS \"\${_vw_models_link}\")
    message(FATAL_ERROR \"VW: \${_vw_models_link} exists and is not the expected symlink\")
  else()
    file(CREATE_LINK \"${VW_MACOS_MODEL_DIR}\" \"\${_vw_models_link}\" SYMBOLIC)
  endif()
")

# Re-sign installed binaries ad hoc. The linker already ad-hoc signs arm64 output, but an install-time
# strip or any post-link edit invalidates that signature and dyld would then refuse the plugin.
install(CODE "
  foreach(_vw_bin
      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${VW_MACOS_VLC_PLUGIN_DIR}/$<TARGET_FILE_NAME:vlc_whisper_plugin>\"
      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:vlc-whisper-worker>\")
    execute_process(COMMAND codesign --force --sign - \"\${_vw_bin}\" RESULT_VARIABLE _vw_rc)
    if(NOT _vw_rc EQUAL 0)
      message(FATAL_ERROR \"VW: ad-hoc codesign failed for \${_vw_bin}\")
    endif()
  endforeach()
")
