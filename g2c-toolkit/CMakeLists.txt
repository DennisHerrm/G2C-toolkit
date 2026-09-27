cmake_minimum_required(VERSION 3.16)

# Laufzeitbibliothek fest einbauen, nicht dynamisch binden.
#
# Ohne das braucht jeder Nutzer das "Visual C++ Redistributable". Fehlt es,
# startet das Programm nicht und meldet "VCRUNTIME140.dll wurde nicht
# gefunden" — die haeufigste Ruecklaufursache bei weitergegebenen
# Windows-Programmen.
#
# Fest eingebaut ist die Exe rund 100 kB groesser und laeuft dafuer ueberall
# ohne Installation. Fuer ein Werkzeug, das in einem Forum weitergereicht
# wird, ist das der richtige Tausch.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(g2c LANGUAGES CXX VERSION 0.1.0)

# Programmweite Optimierung, wenn der Compiler sie beherrscht.
#
# Unter MSVC entspricht das /GL und /LTCG: der Optimierer sieht alle
# Uebersetzungseinheiten auf einmal und kann ueber Dateigrenzen hinweg
# einbetten. Bei einem Werkzeug, das 1289 Dateien durch dieselben paar
# Funktionen schiebt, ist das spuerbar.
#
# Ueber check_ipo_supported abgefragt statt fest eingeschaltet: schlaegt es
# fehl, wird ohne gebaut statt gar nicht.
include(CheckIPOSupported)
check_ipo_supported(RESULT G2C_HAVE_IPO OUTPUT G2C_IPO_MSG)

# Quelldateien sind UTF-8.
#
# Ohne /utf-8 deutet MSVC sie als Codepage 1252 und macht aus jedem Umlaut
# zwei Zeichen — chinesische und japanische Texte werden vollends unlesbar.
if(MSVC)
  add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

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

# Unter Windows liefert die zusammengefuehrte Exe beides. Beide Ziele
# duerfen aber nicht dieselbe Datei schreiben, sonst ueberschreiben sie
# sich je nach Reihenfolge — mal mit, mal ohne Oberflaeche.
#
# Das reine Kommandozeilenziel heisst dort deshalb g2c-cli.exe und ist
# hauptsaechlich zum Entwickeln da; ausgeliefert wird g2c.exe.
if(WIN32)
  set_target_properties(g2c PROPERTIES OUTPUT_NAME "g2c-cli")
endif()
target_link_libraries(g2c PRIVATE g2)
if(G2C_HAVE_IPO)
  set_property(TARGET g2c PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Alle Programme mit UTF-8 als aktiver Codepage (siehe assets/utf8.manifest).
# Ohne das lassen sich unter Windows keine Pfade mit Umlauten oeffnen.
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

# --- Oberflaeche -------------------------------------------------------------
#
# Dear ImGui liegt unter third_party/ im Projekt und wird mituebersetzt — kein
# Paketmanager, kein Installer. Die Fensteranbindung nutzt Win32 und
# DirectX 11, beides Bestandteil von Windows.
#
# gui/app.cpp kennt nur imgui.h und die g2-Bibliothek, kein Win32. Dadurch
# laesst sich der weitaus groessere Teil der Oberflaeche auch ohne
# Windows-SDK uebersetzen und testen — nur die Fensteranbindung nicht.

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

if(EXISTS ${IMGUI_DIR}/imgui.cpp)
  add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
  )
  target_include_directories(imgui PUBLIC ${IMGUI_DIR})

  # Test der Oberflaechenlogik: laeuft ohne Fenster, auf jedem System.
  # Statische Pruefung der Windows-Anbindung.
  #
  # gui/main_win32.cpp laesst sich nur unter Windows uebersetzen. Damit
  # Fehler darin nicht bis zum Nutzer durchrutschen, prueft ein Skript beim
  # Bauen, was sich ohne Windows-Header pruefen laesst.
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

  add_executable(g2_gui_tests tests/gui_tests.cpp gui/app.cpp gui/i18n.cpp gui/icons.cpp gui/preview.cpp)
  target_include_directories(g2_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(g2_gui_tests PRIVATE g2 imgui)
  g2c_utf8(g2_gui_tests)

  # Oberflaeche wirklich bedienen: klicken, ziehen, tippen, ohne Fenster.
  #
  # Braucht ImGui mit eingeschalteten Test-Engine-Hooks und abgefangenen
  # Zusicherungen. Das ist ein eigener Bau der ImGui-Quellen, damit die
  # ausgelieferte g2c.exe davon nichts mitbekommt.
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
                 gui/preview.cpp)
  target_include_directories(g2_gui_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(g2_gui_driver PRIVATE g2 imgui_driver)
  g2c_utf8(g2_gui_driver)

  if(WIN32)
    target_sources(imgui PRIVATE
      ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
      ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )

    # EINE Exe fuer beides: Oberflaeche und Kommandozeile.
    #
    # tools/g2c.cpp kommt mit dazu, sein main() wird per G2C_NO_MAIN
    # abgeschaltet; gui/main_win32.cpp ruft stattdessen g2cMain auf, wenn
    # das erste Argument ein Befehl ist. Fuer die Weitergabe ist das der
    # Unterschied zwischen "eine Datei herunterladen" und "erst mal die
    # richtige von dreien suchen".
    #
    # Programmsymbol: die .rc wird von MSVC uebersetzt und an die Exe
    # gebunden.
    add_executable(g2c-gui WIN32 gui/app.cpp gui/i18n.cpp gui/icons.cpp gui/preview.cpp gui/main_win32.cpp
                   tools/g2c.cpp assets/g2c.rc)
    target_compile_definitions(g2c-gui PRIVATE G2C_NO_MAIN)
    set_target_properties(g2c-gui PROPERTIES OUTPUT_NAME "g2c")
    if(G2C_HAVE_IPO)
      set_property(TARGET g2c-gui PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
    target_include_directories(g2c-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(g2c-gui PRIVATE g2 imgui d3d11 dxgi comdlg32 ole32 shell32)
    g2c_utf8(g2c-gui)
    if(MSVC)
      target_compile_options(g2c-gui PRIVATE /W4)
    endif()
  endif()
endif()
