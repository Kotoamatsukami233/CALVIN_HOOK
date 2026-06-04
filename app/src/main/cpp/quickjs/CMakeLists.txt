cmake_minimum_required(VERSION 3.22.1)
project(quickjs C)

set(CMAKE_C_STANDARD 11)

# QuickJS source files
set(QJS_SOURCES
    quickjs.c
    libregexp.c
    libunicode.c
    libbf.c
    cutils.c
)

add_library(quickjs STATIC ${QJS_SOURCES})

target_include_directories(quickjs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# QuickJS compile definitions
target_compile_definitions(quickjs PRIVATE
    _GNU_SOURCE
    CONFIG_BIGNUM
    CONFIG_VERSION="\\"6.0.0\\""
)

# Suppress QuickJS warnings
target_compile_options(quickjs PRIVATE
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-implicit-fallthrough
    -Wno-unused-variable
    -Wno-unused-function
    -fno-rtti
)

target_link_libraries(quickjs PUBLIC log dl m)
