cmake_minimum_required(VERSION 3.30.3)
project("hook")

set(CMAKE_CXX_STANDARD 23)

# --- hook_sdk: ARM64 inline hook engine ---
add_library(hook_sdk STATIC
    hook_sdk/src/assembler.cpp
    hook_sdk/src/code_patch.cpp
    hook_sdk/src/memory_allocator.cpp
    hook_sdk/src/relocator.cpp
    hook_sdk/src/trampoline.cpp
    hook_sdk/src/hook_registry.cpp
    hook_sdk/src/hook_api.cpp
    hook_sdk/src/elf_symbol_resolver.cpp
    # xz-embedded (Linux kernel lib/xz, 0BSD license)
    hook_sdk/src/xz/xz_dec.c
    hook_sdk/src/xz/xz_crc32.c
    hook_sdk/src/xz/xz_dec_stream.c
    hook_sdk/src/xz/xz_dec_lzma2.c
    hook_sdk/src/xz/xz_dec_bcj.c
)
target_include_directories(hook_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/hook_sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/hook_sdk/src
    ${CMAKE_CURRENT_SOURCE_DIR}/hook_sdk/src/xz
)
target_link_libraries(hook_sdk PUBLIC log android dl)

# --- lsplant: Java method hook engine ---
add_subdirectory(lsplant)

# --- quickjs: JavaScript engine ---
option(ENABLE_JS_ENGINE "Build JavaScript scripting engine" ON)
add_subdirectory(quickjs)

# --- js_engine: JS scripting layer (QuickJS + Frida-compatible API) ---
if(ENABLE_JS_ENGINE)
    add_subdirectory(js_engine)
endif()

# --- main shared library ---
add_library(${CMAKE_PROJECT_NAME} SHARED
    native-lib.cpp
)

if(ENABLE_JS_ENGINE)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE hook_sdk lsplant quickjs js_engine log android dl)
else()
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE hook_sdk lsplant quickjs log android dl)
endif()
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_SCAN_FOR_MODULES ON)
