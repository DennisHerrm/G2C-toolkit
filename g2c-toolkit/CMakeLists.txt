cmake_minimum_required(VERSION 3.16)

# Link the runtime library statically, not dynamically.
#
# Without this, every user needs the "Visual C++ Redistributable". If it is
# missing, the program doesn't start and reports "VCRUNTIME140.dll was not
# found" - the most common cause of complaints about Windows programs that
# get passed around.
#
# Statically linked, the exe is about 100 kB larger but runs anywhere without
# installation. For a tool that gets passed around on a forum, that is the
# right trade-off.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(g2c LANGUAGES CXX VERSION 0.1.0)

# Whole-program optimization, if the compiler supports it.
#
# Under MSVC this corresponds to /GL and /LTCG: the optimizer sees all
# translation units at once and can inline across file boundaries. For a
# tool that pushes 1289 files through the same few functions, this is
# noticeable.
#
# Queried via check_ipo_supported rather than switched on unconditionally: if
# it fails, the build proceeds without it instead of not at all.
include(CheckIPOSupported)
check_ipo_supported(RESULT G2C_HAVE_IPO OUTPUT G2C_IPO_MSG)

# Source files are UTF-8.
#
# Without /utf-8, MSVC interprets them as code page 1252 and turns every
# umlaut into two characters - Chinese and Japanese texts become completely
# unreadable.
if(MSVC)
  add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Version stamped into the executables.
#
# The GitHub workflows pass it in: the release workflow the tag (v1.2.0 ->
# 1.2.0), the snapshot workflow "snapshot-<commit>". A local build stays
# "dev" - the updater then never checks on its own, so a self-built exe is
# not offered a replacement on every start.
set(G2C_VERSION "dev" CACHE STRING "Version stamped into the executables")
set(G2C_COMMIT "" CACHE STRING "Commit the build comes from")
string(REGEX REPLACE "^[vV]" "" G2C_VERSION_CLEAN "${G2C_VERSION}")
set(G2C_VER_MAJOR 0)
set(G2C_VER_MINOR 0)
set(G2C_VER_PATCH 0)
if(G2C_VERSION_CLEAN MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
  set(G2C_VER_MAJOR ${CMAKE_MATCH_1})
  set(G2C_VER_MINOR ${CMAKE_MATCH_2})
  set(G2C_VER_PATCH ${CMAKE_MATCH_3})
elseif(NOT G2C_VERSION_CLEAN STREQUAL "dev" AND NOT G2C_VERSION_CLEAN MATCHES "^snapshot-[0-9a-f]+$")
  # PowerShell splits an unquoted -DG2C_VERSION=v1.2.0 at the dots and
  # CMake gets "v1". Stop instead of shipping an exe that calls itself "1"
  # and can never be updated.
  message(FATAL_ERROR "G2C_VERSION \"${G2C_VERSION}\" is neither 1.2.3[-pre], snapshot-<commit> "
                      "nor dev. From PowerShell, quote the argument: \"-DG2C_VERSION=v1.2.0\"")
endif()
configure_file(assets/g2c_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/g2c_version.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)
message(STATUS "g2c version: ${G2C_VERSION_CLEAN} ${G2C_COMMIT}")

add_library(g2 STATIC
  src/compress.cpp
  src/mdxa.cpp
  src/mdxm.cpp
  src/xsi.cpp
  src/carscript.cpp
  src/xsi_anim.cpp
  src/carbuild.cpp
  src/parallel_stub.cpp
  src/gladiff.cpp
  src/animcache.cpp
  src/xsi_mesh.cpp src/xsi_export.cpp
  src/sidefiles.cpp
  src/animenums.cpp
  src/carvalidate.cpp
  src/carjob.cpp
  src/skelbuild.cpp
  src/ase.cpp
)
target_include_directories(g2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(g2 PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(g2 PRIVATE /W4 /permissive-)
else()
  target_compile_options(g2 PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(g2c tools/g2c.cpp)

# On Windows, the combined exe provides both. But the two targets must not
# write the same file, otherwise they overwrite each other depending on the
# order - sometimes with the UI, sometimes without.
#
# The pure command-line target is therefore called g2c-cli.exe there and is
# mainly meant for development; g2c.exe is what gets shipped.
if(WIN32)
  set_target_properties(g2c PROPERTIES OUTPUT_NAME "g2c-cli")
endif()
target_link_libraries(g2c PRIVATE g2)
if(G2C_HAVE_IPO)
  set_property(TARGET g2c PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# All programs with UTF-8 as the active code page (see assets/utf8.manifest).
# Without this, paths with umlauts can't be opened on Windows.
function(g2c_utf8 target)
  if(MSVC)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/assets/utf8.manifest)
  endif()
endfunction()
g2c_utf8(g2c)

enable_testing()
add_executable(g2_tests tests/tests.cpp)
g2c_utf8(g2_tests)
target_link_libraries(g2_tests PRIVATE g2)
add_test(NAME g2_tests COMMAND g2_tests)

# --- User interface ----------------------------------------------------------
#
# Dear ImGui lives under third_party/ in the project and is compiled along with
# it - no package manager, no installer. The window backend uses Win32 and
# DirectX 11, both part of Windows.
#
# gui/app.cpp only knows imgui.h and the g2 library, no Win32. That way the
# much larger part of the UI can be compiled and tested even without the
# Windows SDK - only the window backend can't.

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

if(EXISTS ${IMGUI_DIR}/imgui.cpp)
  add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
  )
  target_include_directories(imgui PUBLIC ${IMGUI_DIR})

  # Test of the UI logic: runs without a window, on any system.
  # Static check of the Windows backend.
  #
  # gui/main_win32.cpp can only be compiled on Windows. So that bugs in it
  # don't slip through to the user, a script checks during the build whatever
  # can be checked without Windows headers.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    add_custom_target(check_win32 ALL
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_win32.py
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_wiring.py
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_batch.py
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_imgui.py
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Pruefe Windows-Anbindung, Verdrahtung und build.bat")
  endif()

  add_executable(g2_gui_tests tests/gui_tests.cpp gui/app.cpp gui/i18n.cpp gui/icons.cpp gui/preview.cpp
                 gui/update.cpp)
  target_include_directories(g2_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(g2_gui_tests PRIVATE g2 imgui)
  g2c_utf8(g2_gui_tests)

  # Actually operate the UI: click, drag, type, without a window.
  #
  # Needs ImGui with test engine hooks enabled and assertions intercepted.
  # This is a separate build of the ImGui sources, so that the shipped
  # g2c.exe isn't affected by any of it.
  add_library(imgui_driver STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
  )
  target_include_directories(imgui_driver PUBLIC ${IMGUI_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(imgui_driver PUBLIC IMGUI_ENABLE_TEST_ENGINE
                             IMGUI_USER_CONFIG="gui_driver_imconfig.h")
  add_executable(g2_gui_driver tests/gui_driver.cpp gui/app.cpp gui/i18n.cpp gui/icons.cpp
                 gui/preview.cpp gui/update.cpp)
  target_include_directories(g2_gui_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(g2_gui_driver PRIVATE g2 imgui_driver)
  g2c_utf8(g2_gui_driver)

  if(WIN32)
    target_sources(imgui PRIVATE
      ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
      ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )

    # ONE exe for both: UI and command line.
    #
    # tools/g2c.cpp is included as well, its main() is disabled via
    # G2C_NO_MAIN; gui/main_win32.cpp calls g2cMain instead when the first
    # argument is a command. For distribution, that is the difference between
    # "download one file" and "first find the right one out of three".
    #
    # Program icon: the .rc is compiled by MSVC and linked into the exe.
    add_executable(g2c-gui WIN32 gui/app.cpp gui/i18n.cpp gui/icons.cpp gui/preview.cpp gui/main_win32.cpp
                   gui/update.cpp tools/g2c.cpp assets/g2c.rc)
    target_compile_definitions(g2c-gui PRIVATE G2C_NO_MAIN)
    set_target_properties(g2c-gui PROPERTIES OUTPUT_NAME "g2c")
    if(G2C_HAVE_IPO)
      set_property(TARGET g2c-gui PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
    target_include_directories(g2c-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(g2c-gui PRIVATE g2 imgui d3d11 dxgi comdlg32 ole32 shell32 winhttp)
    g2c_utf8(g2c-gui)
    if(MSVC)
      target_compile_options(g2c-gui PRIVATE /W4)
    endif()
  endif()
endif()
