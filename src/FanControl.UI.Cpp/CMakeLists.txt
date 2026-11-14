cmake_minimum_required(VERSION 3.20)
project(FanControl.UI LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(FanControl.UI WIN32
    src/main.cpp
    src/app.cpp
    src/ipc.cpp
    src/i18n.cpp
    resource.rc
)

target_compile_definitions(FanControl.UI PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(FanControl.UI PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(FanControl.UI PRIVATE d2d1 dwrite dwmapi user32 gdi32 shell32 ole32 advapi32)

# 管理员启动器（安装包与开机自启任务指向 FanControl.exe）
add_executable(FanControl.Launcher WIN32
    src/launcher.cpp
    launcher.rc
)
set_target_properties(FanControl.Launcher PROPERTIES OUTPUT_NAME "FanControl")
target_compile_definitions(FanControl.Launcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(FanControl.Launcher PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(FanControl.Launcher PRIVATE user32 shell32)

if(MSVC)
    target_compile_options(FanControl.UI PRIVATE /MP)
    target_link_options(FanControl.UI PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
    target_compile_options(FanControl.Launcher PRIVATE /MP)
    target_link_options(FanControl.Launcher PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/launcher.manifest")
endif()
