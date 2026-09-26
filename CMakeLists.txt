cmake_minimum_required(VERSION 3.20)
project(dysonbehind LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DY_VENDOR "${CMAKE_SOURCE_DIR}/vendor")

add_library(dysonbehind SHARED
    src/main.cpp
    src/fonts_data.cpp
    src/DevLog.cpp
    src/RegionChanger.cpp
    src/UISound.cpp
    src/icons.cpp
    src/fontello_data.cpp

    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/imgui_impl_dx11.cpp
    vendor/imgui/imgui_impl_win32.cpp

    vendor/minhook/MinHook.c
    vendor/minhook/buffer.c
    vendor/minhook/trampoline.c
    vendor/minhook/hde/hde32.c
    vendor/minhook/hde/hde64.c
)

target_include_directories(dysonbehind PRIVATE
    src vendor/imgui vendor/minhook)
target_compile_definitions(dysonbehind PRIVATE
    WIN32 _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0601 WINVER=0x0601
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>)
target_link_libraries(dysonbehind PRIVATE d3d11 dxgi winmm)

if(MSVC)
    target_compile_options(dysonbehind PRIVATE /permissive- /W3 /utf-8 /MP)
    set_source_files_properties(
        vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp
        vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp
        vendor/imgui/imgui_impl_dx11.cpp vendor/imgui/imgui_impl_win32.cpp
        vendor/minhook/MinHook.c vendor/minhook/buffer.c
        vendor/minhook/trampoline.c
        vendor/minhook/hde/hde32.c vendor/minhook/hde/hde64.c
        PROPERTIES COMPILE_OPTIONS "/W3;/WX-")
    set_property(TARGET dysonbehind PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(Injector
    src/injector.cpp
)
target_compile_definitions(Injector PRIVATE
    WIN32 _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0601 WINVER=0x0601
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>)
target_link_libraries(Injector PRIVATE shell32)
# Просит права администратора при запуске — UAC сам поднимает окно.
set_target_properties(Injector PROPERTIES LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
if(MSVC)
    target_compile_options(Injector PRIVATE /permissive- /W3 /utf-8 /MP)
    set_property(TARGET Injector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# После сборки кладём готовые файлы в release/, чтобы не собирать их руками.
add_custom_command(TARGET Injector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/release"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Injector>"
        "$<TARGET_FILE:dysonbehind>"
        "${CMAKE_SOURCE_DIR}/release"
    COMMENT "Deploying Injector.exe and dysonbehind.dll to release/")
