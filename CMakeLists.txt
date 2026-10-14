# Weapon Switch - builds WeaponSwitch.dll on its own. Needs only CMake and a
# C++ compiler (Visual Studio 2022 or newer); MinHook is downloaded on the
# first build. Easiest: run build.bat.
cmake_minimum_required(VERSION 3.20)
project(weapon_switch LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No Visual C++ runtime to install: the DLL carries what it needs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(weapon_switch SHARED src/weapon_switch.cpp)
target_link_libraries(weapon_switch PRIVATE minhook)
set_target_properties(weapon_switch PROPERTIES OUTPUT_NAME "WeaponSwitch" PREFIX "")

