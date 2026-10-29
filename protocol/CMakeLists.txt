add_library(vw_protocol STATIC
    src/vw_protocol_codec.c
    src/vw_protocol_validate.c
    src/vw_ipc_pipe_win32.c
    src/vw_ipc_socket_linux.c
    src/vw_log.c
)

target_include_directories(vw_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vw_protocol PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(vw_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    target_compile_definitions(vw_protocol PRIVATE _DARWIN_C_SOURCE)
endif()
