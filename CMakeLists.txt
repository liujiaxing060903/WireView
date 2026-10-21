cmake_minimum_required(VERSION 3.25)

project(WireView VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 发布包使用静态 MSVC 运行库，避免目标电脑因缺少 VC++ Redistributable
# 而在控制中心窗口出现前就退出。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)

add_subdirectory(protocol)

if(WIN32)
    add_subdirectory(windows/host)
endif()
