cmake_minimum_required(VERSION 3.13)
project(libbgcode)

# Prusa binary G-code (.bgcode): libbgcode at the commit used by PrusaSlicer 2.9.6, with heatshrink 0.4.1
# (see README-Orio.txt). Only core, binarize and convert are built, as a static library.

add_library(heatshrink STATIC
    heatshrink/heatshrink_encoder.c
    heatshrink/heatshrink_decoder.c
)
target_include_directories(heatshrink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/heatshrink)

add_library(libbgcode STATIC
    LibBGCode/core/core.cpp
    LibBGCode/binarize/binarize.cpp
    LibBGCode/binarize/meatpack.cpp
    LibBGCode/convert/convert.cpp
)
target_include_directories(libbgcode PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/LibBGCode
)
target_link_libraries(libbgcode PRIVATE heatshrink ZLIB::ZLIB boost_headeronly)
# Given by libbgcode's own CMake (pyproject.toml of the commit above).
target_compile_definitions(libbgcode PRIVATE LibBGCode_VERSION="0.3.0")

if (MSVC)
    # heatshrink as released: zero-size arrays (C4200), 32-bit shift widened (C4334), harmless.
    target_compile_options(heatshrink PRIVATE /wd4200 /wd4334)
    target_compile_options(libbgcode PRIVATE /wd4200)
endif ()
