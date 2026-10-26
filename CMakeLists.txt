cmake_minimum_required(VERSION 3.28)
project(jotdo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ICAL REQUIRED IMPORTED_TARGET libical)
find_package(pugixml QUIET)

# --- dependencies -------------------------------------------------------------

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v7.0.3
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)

FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(ftxui tomlplusplus)

if(NOT pugixml_FOUND)
  FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(pugixml)
endif()

# --- jotdo --------------------------------------------------------------------

add_library(jotdo_core STATIC
  src/config.cpp src/dav.cpp src/editor.cpp src/form.cpp src/http.cpp src/ical.cpp src/reminders.cpp
  src/theme.cpp src/when.cpp)
target_include_directories(jotdo_core PUBLIC src)
target_link_libraries(jotdo_core PUBLIC
  tomlplusplus::tomlplusplus CURL::libcurl PkgConfig::ICAL pugixml::pugixml ftxui::screen)

add_executable(jotdo src/main.cpp src/tui.cpp)
target_compile_definitions(jotdo PRIVATE JOTDO_VERSION="${PROJECT_VERSION}")
target_link_libraries(jotdo PRIVATE jotdo_core ftxui::component ftxui::dom ftxui::screen)

include(GNUInstallDirs)
install(TARGETS jotdo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES doc/jotdo.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# --- tests --------------------------------------------------------------------

option(JOTDO_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
if(JOTDO_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest
      GIT_TAG v1.17.0
      GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(jotdo_tests test/when_test.cpp test/ical_test.cpp test/form_test.cpp)
  target_link_libraries(jotdo_tests PRIVATE jotdo_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(jotdo_tests PROPERTIES ENVIRONMENT "TZ=America/New_York")
endif()
