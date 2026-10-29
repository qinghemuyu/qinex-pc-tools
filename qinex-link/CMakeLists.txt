# QInEX-Link:P4 上位机直驱(Mode C)PC 端转发工具。独立小工程,不进 QInEX.exe 构建。
# 构建:cmake -S . -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.21)
project(QInEXLink LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QInEX-Link 需要 Windows x64 和 Visual Studio C++。")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 支持直接以 qinex-link 为源目录构建。
if(NOT TARGET pointer_desktop)
    set(QINEX_BUILD_POINTER OFF)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../qinex-pointer" "${CMAKE_CURRENT_BINARY_DIR}/pointer")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(QInEX-Link WIN32
    src/main.cpp
    src/link.cpp
    src/capture.cpp
    src/sender.cpp
    src/settings.cpp
    src/netserver.cpp
    src/performance.cpp
    src/taskbar_widget.cpp
    res/app.rc
)

target_compile_options(QInEX-Link PRIVATE /utf-8 /W3 /EHsc)
target_compile_definitions(QInEX-Link PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(QInEX-Link PRIVATE
    pointer_desktop
    setupapi winmm shell32 user32 gdi32 advapi32 shcore ws2_32
    ole32 oleaut32 uiautomationcore d3d11 d2d1 dwrite dcomp dxgi
)

target_link_options(QInEX-Link PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")

add_custom_command(TARGET QInEX-Link POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
            "$<TARGET_FILE_DIR:QInEX-Link>/THIRD_PARTY_NOTICES.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/后台性能说明.txt"
            "$<TARGET_FILE_DIR:QInEX-Link>/后台性能说明.txt"
)

install(TARGETS QInEX-Link RUNTIME DESTINATION .)
install(FILES THIRD_PARTY_NOTICES.txt 后台性能说明.txt DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.txt" "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md" DESTINATION .)
include(CTest)
if(BUILD_TESTING)
    get_target_property(link_ui_sources QInEX-Link SOURCES)
    list(REMOVE_ITEM link_ui_sources src/main.cpp)
    add_executable(link_ui_tests tests/ui_tests.cpp ${link_ui_sources})
    target_compile_options(link_ui_tests PRIVATE /utf-8 /W3 /EHsc)
    target_compile_definitions(link_ui_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS QINEX_WITH_ANDROID_TOOLS=$<BOOL:${QINEX_WITH_ANDROID_TOOLS}>)
    get_target_property(link_ui_libraries QInEX-Link LINK_LIBRARIES)
    target_link_libraries(link_ui_tests PRIVATE ${link_ui_libraries})
    target_link_options(link_ui_tests PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
    set_target_properties(link_ui_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ui-test")
    add_test(NAME link_embedded_ui COMMAND link_ui_tests)
    set_tests_properties(link_embedded_ui PROPERTIES TIMEOUT 30)
    add_executable(link_protocol_tests tests/protocol_tests.cpp)
    target_include_directories(link_protocol_tests PRIVATE src)
    target_compile_options(link_protocol_tests PRIVATE /utf-8 /W4)
    add_test(NAME link_protocol COMMAND link_protocol_tests)
endif()
