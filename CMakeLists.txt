# macOS build. Linux uses the GNUmakefile, so it needs nothing Omarchy doesn't already have.
cmake_minimum_required(VERSION 3.20)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
project(omapad VERSION 0.3.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SCI ${CMAKE_SOURCE_DIR}/third_party/scintilla)
set(LEX ${CMAKE_SOURCE_DIR}/third_party/lexilla)

# --- Scintilla (editing component) and Lexilla (language lexers), statically linked.
file(GLOB SCI_SOURCES CONFIGURE_DEPENDS ${SCI}/src/*.cxx)
file(GLOB LEX_SOURCES CONFIGURE_DEPENDS ${LEX}/src/*.cxx ${LEX}/lexlib/*.cxx ${LEX}/lexers/*.cxx)
# Notepad++'s user-defined language lexer depends on Windows. omapad doesn't support those yet.
list(FILTER LEX_SOURCES EXCLUDE REGEX "LexUser\\.cxx$")
set(SCI_COCOA_SOURCES
  ${SCI}/cocoa/InfoBar.mm
  ${SCI}/cocoa/PlatCocoa.mm
  ${SCI}/cocoa/ScintillaCocoa.mm
  ${SCI}/cocoa/ScintillaView.mm)

add_library(engine STATIC ${SCI_SOURCES} ${LEX_SOURCES} ${SCI_COCOA_SOURCES})
target_include_directories(engine PUBLIC
  ${SCI}/include ${SCI}/src ${SCI}/cocoa ${LEX}/include ${LEX}/lexlib)
target_compile_options(engine PRIVATE -w)
set_source_files_properties(${SCI_COCOA_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# --- PCRE2, the regular expression engine.
file(GLOB PCRE2_SOURCES CONFIGURE_DEPENDS third_party/pcre2/*.c)
add_library(pcre2 STATIC ${PCRE2_SOURCES})
target_compile_definitions(pcre2 PRIVATE HAVE_CONFIG_H PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC SUPPORT_UNICODE)
target_include_directories(pcre2 PRIVATE third_party/pcre2)
target_compile_options(pcre2 PRIVATE -w)

# --- The shared core.
add_library(omapad_core STATIC
  core/TextCore.cpp core/Languages.cpp core/Workspace.cpp core/Regex.cpp core/Completion.cpp core/FunctionList.cpp
  third_party/pugixml/pugixml.cpp)
target_link_libraries(omapad_core PUBLIC engine pcre2 iconv)
target_include_directories(omapad_core PUBLIC core third_party/json third_party/pugixml third_party/pcre2)
target_compile_options(omapad_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)

# --- The Mac interface.
file(GLOB THEME_FILES CONFIGURE_DEPENDS resources/themes/*.xml)
file(GLOB COMPLETION_FILES CONFIGURE_DEPENDS resources/autoCompletion/*.xml)
file(GLOB FUNCTION_FILES CONFIGURE_DEPENDS resources/functionList/*.xml)
file(GLOB LICENSE_FILES CONFIGURE_DEPENDS licenses/*.txt)
set(DATA_FILES resources/langs.model.xml resources/stylers.model.xml resources/omapad.icns LICENSE NOTICE)
# Scintilla loads its cursors as TIFF. Its Xcode project makes these from the PNGs in cocoa/res.
set(SCI_RES_FILES
  ${SCI}/cocoa/res/info_bar_bg.png ${SCI}/cocoa/res/info_bar_bg@2x.png
  resources/mac_cursor_busy.tiff resources/mac_cursor_flipped.tiff)

add_executable(omapad MACOSX_BUNDLE
  mac/main.mm mac/AppController.mm mac/TabBar.mm mac/FindPanel.mm mac/FolderPanel.mm mac/FunctionPanel.mm mac/Dialogs.mm
  ${DATA_FILES} ${THEME_FILES} ${COMPLETION_FILES} ${FUNCTION_FILES} ${SCI_RES_FILES} ${LICENSE_FILES})
target_link_libraries(omapad PRIVATE omapad_core "-framework Cocoa" "-framework QuartzCore")
target_include_directories(omapad PRIVATE mac)
target_compile_options(omapad PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
set_target_properties(omapad PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
set_source_files_properties(${DATA_FILES} ${SCI_RES_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties(${THEME_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/themes)
set_source_files_properties(${COMPLETION_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/autoCompletion)
set_source_files_properties(${FUNCTION_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/functionList)
set_source_files_properties(${LICENSE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)

# --- Tests: pure logic plus the real Workspace with real, hidden editor views.
enable_testing()
add_executable(omapad_tests tests/core_tests.cpp tests/TestViews_mac.mm)
target_link_libraries(omapad_tests PRIVATE omapad_core "-framework Cocoa" "-framework QuartzCore")
target_include_directories(omapad_tests PRIVATE tests)
set_source_files_properties(tests/TestViews_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
target_compile_definitions(omapad_tests PRIVATE OMAPAD_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
add_test(NAME omapad_tests COMMAND omapad_tests)
