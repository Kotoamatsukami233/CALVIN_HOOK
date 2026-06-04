cmake_minimum_required(VERSION 3.22.1)
project(js_engine CXX)

set(CMAKE_CXX_STANDARD 23)

# Embed js_runtime.js as a C string literal at configure time
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/js_runtime.js" _JS_RUNTIME_CONTENT)
string(REPLACE "\\" "\\\\" _JS_RUNTIME_CONTENT "${_JS_RUNTIME_CONTENT}")
string(REPLACE "\"" "\\\"" _JS_RUNTIME_CONTENT "${_JS_RUNTIME_CONTENT}")
string(REPLACE "\n" "\\n\"\n\"" _JS_RUNTIME_CONTENT "${_JS_RUNTIME_CONTENT}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/js_runtime_inc.cpp"
    "extern const char js_runtime_source[] = \"${_JS_RUNTIME_CONTENT}\";\n"
)

add_library(js_engine STATIC
    src/js_engine.cpp
    src/js_ptr.cpp
    src/js_memory.cpp
    src/js_module.cpp
    src/js_interceptor.cpp
    src/js_native.cpp
    src/js_java_bridge.cpp
    src/js_socket_server.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/js_runtime_inc.cpp
)

target_include_directories(js_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../hook_sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../hook_sdk/src
)

target_link_libraries(js_engine PUBLIC quickjs hook_sdk log)
target_compile_options(js_engine PRIVATE -fvisibility=hidden -fno-rtti)
target_compile_definitions(js_engine PRIVATE
    _GNU_SOURCE
    CONFIG_BIGNUM
    CONFIG_VERSION="\\"2024-01-13\\""
)
