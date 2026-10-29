cmake_minimum_required(VERSION 3.28)

project(LaskManager
  VERSION 0.1.0
  DESCRIPTION "Lask Manager - a lightweight Task Manager for Windows"
  LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Lask Manager targets Windows with the MSVC toolset only.")
endif()

# Static CRT: one exe, no runtime installer, fewer pages to fault in under memory pressure.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

add_executable(lask WIN32
  src/main.cpp
  src/snapshot.cpp
  src/theme.cpp
  src/lask.rc
  src/app.manifest)   # CMake merges a .manifest source into the linker-generated manifest

# MSVC 14.51 spells C++23 as /std:c++23preview; 14.52+ accepts /std:c++23 and keeps the old
# spelling as an alias, so this flag works on both.
target_compile_options(lask PRIVATE
  /std:c++23preview
  /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc
  $<$<CONFIG:Release>:/GL /Gy /Gw>)

target_compile_definitions(lask PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX STRICT
  _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006   # Windows 10 1809 (RS5) baseline
  LASK_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(lask PRIVATE
  ntdll comctl32 dwmapi uxtheme advapi32 shell32 user32 gdi32)

target_link_options(lask PRIVATE
  /SUBSYSTEM:WINDOWS
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>)

set_target_properties(lask PROPERTIES
  OUTPUT_NAME "lask"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:lask>")

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT lask)
