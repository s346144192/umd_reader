# 独立实验目标：自研 HTML/CSS 渲染排版引擎。
# 该目标不参与阅读器 app 的依赖链，仅用于验证解析、布局与两种图形后端的一致性。

cmake_minimum_required(VERSION 3.20)
project(web_render LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 渲染器源码使用 std::filesystem 与 std::string::data() 的可写重载，需要 C++17。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 解析器生成器位置：优先使用 CMake 缓存变量，其次使用已知安装目录，最后回退 PATH。
find_program(RE2C_EXECUTABLE re2c
    HINTS "E:/code/tools/re2"
    DOC "re2c 词法分析器生成器")
find_program(BISON_EXECUTABLE win_bison bison
    HINTS "E:/code/tools/win_flex_bison-latest"
    DOC "GNU Bison 语法分析器生成器")

find_package(Threads REQUIRED)

if (NOT RE2C_EXECUTABLE)
    message(FATAL_ERROR "未找到 re2c，请设置 -DRE2C_EXECUTABLE=<path>")
endif()
if (NOT BISON_EXECUTABLE)
    message(FATAL_ERROR "未找到 bison，请设置 -DBISON_EXECUTABLE=<path>")
endif()

message(STATUS "web_render 使用 re2c: ${RE2C_EXECUTABLE}")
message(STATUS "web_render 使用 bison: ${BISON_EXECUTABLE}")

set(WEB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${WEB_GEN_DIR}")

function(web_generate_parser name grammar source)
    set(parser_c "${WEB_GEN_DIR}/${name}_parser.c")
    set(parser_h "${WEB_GEN_DIR}/${name}_parser.h")
    set(lexer_c "${WEB_GEN_DIR}/${name}_lexer.c")

    add_custom_command(
        OUTPUT "${parser_c}" "${parser_h}"
        COMMAND "${BISON_EXECUTABLE}" -d
                -o "${parser_c}"
                "--defines=${parser_h}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/${grammar}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${grammar}"
        COMMENT "生成 ${name} 语法分析器 (bison)"
        VERBATIM)

    add_custom_command(
        OUTPUT "${lexer_c}"
        COMMAND "${RE2C_EXECUTABLE}" --no-generation-date
                -o "${lexer_c}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${source}" "${parser_h}"
        COMMENT "生成 ${name} 词法分析器 (re2c)"
        VERBATIM)

    set_source_files_properties("${parser_c}" PROPERTIES LANGUAGE C COMPILE_OPTIONS "/W0")
    set_source_files_properties("${lexer_c}" PROPERTIES LANGUAGE C COMPILE_OPTIONS "/W0")

    set(WEB_GEN_SOURCES "${WEB_GEN_SOURCES}" "${parser_c}" "${lexer_c}" PARENT_SCOPE)
    set(WEB_GEN_HEADERS "${WEB_GEN_HEADERS}" "${parser_h}" PARENT_SCOPE)
endfunction()

set(WEB_GEN_SOURCES "")
set(WEB_GEN_HEADERS "")
web_generate_parser(html "html_parser.y" "html_lexer.re")
web_generate_parser(css "css_parser.y" "css_lexer.re")

# QuickJS：以子目录方式引入，关闭安装规则与示例，只产出核心静态库。
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs"
                 "${CMAKE_CURRENT_BINARY_DIR}/quickjs" EXCLUDE_FROM_ALL)

add_executable(web_render
    src/main.cpp
    src/utf8.cpp
    src/dom.cpp
    src/html_builder.cpp
    src/css_support.cpp
    src/css_selector.cpp
    src/css_style.cpp
    src/layout.cpp
    src/paint.cpp
    src/svg_render.cpp
    src/text_service.cpp
    src/image_io.cpp
    src/render_factory.cpp
    src/render_dx11.cpp
    src/render_gl33.cpp
    src/gl_loader.cpp
    src/engine.cpp
    src/script_host.cpp
    src/canvas.cpp
    src/selection.cpp
    src/clipboard.cpp
    src/form_events.cpp
    ${WEB_GEN_SOURCES}
)

target_include_directories(web_render PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WEB_GEN_DIR}"
)

target_compile_definitions(web_render PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                                              _CRT_SECURE_NO_WARNINGS)

# 源文件以 UTF-8 保存且含中文注释，必须按 UTF-8 解析，否则 MSVC 会按本地代码页
# 误读多字节序列并吞掉换行，导致头文件语法错误。
if (MSVC)
    target_compile_options(web_render PRIVATE /utf-8)
endif()

target_link_libraries(web_render PRIVATE
    qjs
    d3d11
    dxgi
    d3dcompiler
    dwrite
    windowscodecs
    opengl32
    gdi32
    user32
    imm32
    comdlg32
    ole32
    shell32
    psapi
    Threads::Threads
)
