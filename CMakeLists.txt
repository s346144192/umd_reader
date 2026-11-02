cmake_minimum_required(VERSION 3.20)
project(UmdReader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_compile_options(/utf-8)
endif()

add_library(miniz STATIC
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
)
target_include_directories(miniz PUBLIC third_party/miniz)
if (MSVC)
    target_compile_options(miniz PRIVATE /W0)
endif()

add_library(umd_core STATIC
    src/umd/umd_parser.cpp
)
target_include_directories(umd_core PUBLIC src)
target_link_libraries(umd_core PUBLIC miniz)

add_executable(umd_probe
    tools/umd_probe.cpp
)
target_link_libraries(umd_probe PRIVATE umd_core)

add_library(umd_data STATIC
    src/core/text_util.cpp
    src/data/library.cpp
)
target_include_directories(umd_data PUBLIC src)
target_include_directories(umd_data PRIVATE third_party)
target_link_libraries(umd_data PUBLIC umd_core bcrypt crypt32)

add_library(umd_ui STATIC
    src/ui/render_context.cpp
    src/ui/reader_pager.cpp
    src/ui/chapter_display.cpp
    src/ui/application.cpp
)
target_include_directories(umd_ui PUBLIC src)
target_include_directories(umd_ui PRIVATE third_party)
target_link_libraries(umd_ui PUBLIC umd_data d2d1 dwrite windowscodecs ole32 shell32 imm32 advapi32 sapi winhttp)

add_executable(umd_reader WIN32
    src/main.cpp
)
target_link_libraries(umd_reader PRIVATE umd_ui)
