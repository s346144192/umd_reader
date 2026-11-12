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

# 构建目标使用中性名称；产物文件名单独由 APP_OUTPUT_NAME 控制。
set(APP_OUTPUT_NAME umd_reader)

add_library(format STATIC
    src/umd/umd_parser.cpp
    src/pdf/pdf_book.cpp
)
target_include_directories(format PUBLIC src)
target_link_libraries(format PUBLIC miniz)

# PDFium 以动态库形式引入：链接使用导入库，运行时需要把 pdfium.dll 放到可执行文件同目录。
set(PDFIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium")
add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFIUM_ROOT}/bin/pdfium.dll"
    IMPORTED_IMPLIB "${PDFIUM_ROOT}/lib/pdfium.dll.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_ROOT}/include"
)

function(copy_pdfium_runtime target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_ROOT}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMENT "复制 pdfium.dll 到输出目录")
endfunction()

add_library(pdf STATIC
    src/pdf/pdf_document.cpp
)
target_include_directories(pdf PUBLIC src)
target_link_libraries(pdf PUBLIC pdfium)

add_executable(pdf_probe
    tools/pdf_probe.cpp
)
target_link_libraries(pdf_probe PRIVATE pdf psapi)
copy_pdfium_runtime(pdf_probe)

# 格式层负责把 PDF 转换为书籍模型，因此依赖 PDF 文档层。
target_link_libraries(format PRIVATE pdf)

# 页面文字层：PDF 内嵌文本提取与系统 OCR 识别，供固定页阅读选择文字。
add_library(reader_text STATIC
    src/reader/page_text.cpp
)
target_include_directories(reader_text PUBLIC src)
target_link_libraries(reader_text PUBLIC pdf windowsapp)
if (MSVC AND DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    # C++/WinRT 头文件位于 Windows SDK 的 cppwinrt 目录，部分生成器不会自动加入包含路径。
    target_include_directories(reader_text PUBLIC
        "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
endif()

add_executable(ocr_probe
    tools/ocr_probe.cpp
)
target_link_libraries(ocr_probe PRIVATE reader_text)
copy_pdfium_runtime(ocr_probe)

add_executable(format_probe
    tools/umd_probe.cpp
)
target_link_libraries(format_probe PRIVATE format)
copy_pdfium_runtime(format_probe)

add_library(data STATIC
    src/core/text_util.cpp
    src/data/library.cpp
    src/data/reading_log.cpp
    src/data/reading_stats.cpp
)
target_include_directories(data PUBLIC src)
target_include_directories(data PRIVATE third_party)
target_link_libraries(data PUBLIC format bcrypt crypt32)

add_library(gfx STATIC
    src/gfx/d2d_renderer.cpp
    src/gfx/renderer_factory.cpp
)
target_include_directories(gfx PUBLIC src)
target_link_libraries(gfx PUBLIC d2d1 dwrite windowscodecs ole32)

# 局域网书籍分享：只读 HTTP 服务端与 WinHTTP 客户端。
add_library(net STATIC
    src/net/lan_share.cpp
)
target_include_directories(net PUBLIC src)
target_include_directories(net PRIVATE third_party)
target_link_libraries(net PUBLIC ws2_32 winhttp iphlpapi)

add_library(ui STATIC
    src/ui/render_context.cpp
    src/reader/fixed_page_reader.cpp
    src/ui/reader_pdf_text.cpp
    src/ui/reader_pager.cpp
    src/ui/chapter_display.cpp
    src/ui/ui_internal.cpp
    src/ui/ai_client.cpp
    src/ui/ai_services.cpp
    src/ui/ai_generate.cpp
    src/ui/chat_view.cpp
    src/ui/shelf_view.cpp
    src/ui/reader_input.cpp
    src/ui/reader_navigation.cpp
    src/ui/reader_view.cpp
    src/ui/ui_resources.cpp
    src/ui/ui_primitives.cpp
    src/ui/ui_menu.cpp
    src/ui/achievement_view.cpp
    src/ui/lan_view.cpp
    src/ui/application.cpp
)
target_include_directories(ui PUBLIC src)
target_include_directories(ui PRIVATE third_party)
target_link_libraries(ui PUBLIC data gfx net reader_text d2d1 dwrite windowscodecs ole32 shell32 imm32 advapi32 sapi winhttp winmm)

add_executable(app WIN32
    src/main.cpp
)
target_link_libraries(app PRIVATE ui)
set_target_properties(app PROPERTIES OUTPUT_NAME ${APP_OUTPUT_NAME})
copy_pdfium_runtime(app)

# 自研 HTML/CSS 渲染引擎实验目标（独立可执行程序，不参与阅读器依赖链）。
option(UMD_BUILD_WEB_RENDER "构建自研 HTML/CSS 渲染引擎实验程序" ON)
if (UMD_BUILD_WEB_RENDER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/experiments/web_render/CMakeLists.txt")
    add_subdirectory(experiments/web_render)
endif()
