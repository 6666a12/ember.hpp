cmake_minimum_required(VERSION 3.16)
project(ember VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EMBER_FETCH_DEPS   "Download glad/glfw/glm automatically (FetchContent)" ON)
option(EMBER_BUILD_EXAMPLES "Build the example programs" ON)
option(EMBER_BUILD_TESTS    "Build the standalone config tests" ON)
option(EMBER_BUILD_EDITOR   "Build the interactive emitter editor into the examples" ON)
set(EMBER_DEPS_DIR "" CACHE PATH
  "Directory with local dependency tarballs for offline builds (glad-v2.0.8.tar.gz, glfw-3.4.tar.gz, glm-1.0.1.tar.gz)")

# ---------------------------------------------------------------------------
# Dependencies: glad (GL loader), glfw (windowing, optional), glm (math)
# ---------------------------------------------------------------------------
if(EMBER_FETCH_DEPS)
  include(FetchContent)

  set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)
  set(GLAD_API       "gl:core=4.3" CACHE STRING "" FORCE)
  set(GLAD_PROFILE   "core" CACHE STRING "" FORCE)
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
  set(GLM_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLM_BUILD_LIBRARY  OFF CACHE BOOL "" FORCE)

  if(EMBER_DEPS_DIR)
    # Offline mode: extract local tarballs once, then point FetchContent at them.
    set(_offline "${CMAKE_BINARY_DIR}/_offline-deps")
    file(MAKE_DIRECTORY "${_offline}")
    foreach(_p "glad;2.0.8;v2.0.8" "glfw;3.4;3.4" "glm;1.0.1;1.0.1")
      list(GET _p 0 _name)
      list(GET _p 1 _ver)
      list(GET _p 2 _tag)
      set(_stamp "${_offline}/${_name}-${_ver}-stamp")
      if(NOT EXISTS "${_stamp}")
        message(STATUS "ember: extracting ${_name}-${_tag}.tar.gz from ${EMBER_DEPS_DIR}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf
                        "${EMBER_DEPS_DIR}/${_name}-${_tag}.tar.gz"
                        WORKING_DIRECTORY "${_offline}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
          message(FATAL_ERROR "ember: cannot extract ${EMBER_DEPS_DIR}/${_name}-${_tag}.tar.gz")
        endif()
        file(TOUCH "${_stamp}")
      endif()
      set("FETCHCONTENT_SOURCE_DIR_${_name}" "${_offline}/${_name}-${_ver}" CACHE PATH "" FORCE)
    endforeach()
  endif()

  FetchContent_Declare(glad
    URL https://github.com/Dav1dde/glad/archive/refs/tags/v2.0.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR cmake)   # glad v2 keeps its CMakeLists.txt in cmake/
  FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

  FetchContent_MakeAvailable(glad glfw glm)
  # glad v2's CMake only DEFINES glad_add_library(); we must call it to create
  # the actual target (generates glad.c/gl.h at build time via Python).
  glad_add_library(glad STATIC API gl:core=4.3)
else()
  find_package(glad REQUIRED)
  find_package(glfw3 REQUIRED)
  find_package(glm  REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Core library. GLFW-independent: the host application owns the GL context
# (GLFW / SDL / Qt / Win32 / EGL...). Call ember::gl::init() with a loader.
# ---------------------------------------------------------------------------
add_library(ember STATIC
  src/shader.cpp
  src/particle_system.cpp
  src/stb_image.cpp)   # PNG sprite loading (optional, see third_party/stb/)
target_include_directories(ember PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(ember PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_definitions(ember PRIVATE EMBER_USE_STB) # PNG sprites via stb_image
target_link_libraries(ember PUBLIC glad glm)
target_compile_features(ember PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(ember PRIVATE /W4)
else()
  target_compile_options(ember PRIVATE -Wall -Wextra)
endif()

# Convenience module: minimal GLFW window wrapper (optional).
add_library(ember_glfw STATIC src/glfw_window.cpp)
target_include_directories(ember_glfw PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(ember_glfw PUBLIC ember glfw)

# ---------------------------------------------------------------------------
# Install / export so other projects can consume the library:
#   find_package(ember)  ->  ember::ember, ember::ember_glfw
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ember ember_glfw
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ember DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_package_config_file(cmake/emberConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/emberConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ember)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/emberConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/emberConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/emberConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ember)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(EMBER_BUILD_EXAMPLES AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(example_basic examples/basic.cpp)
  target_link_libraries(example_basic PRIVATE ember_glfw)
  if(EMBER_BUILD_EDITOR)
    target_compile_definitions(example_basic PRIVATE EMBER_EDITOR_ENABLED)
  endif()
  add_executable(example_fireworks examples/fireworks.cpp)
  target_link_libraries(example_fireworks PRIVATE ember_glfw)
endif()

# ---------------------------------------------------------------------------
# Standalone tests (config_test runs anywhere; system_test needs a GL context,
# skips with exit code 77 when none is available)
# ---------------------------------------------------------------------------
if(EMBER_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(ember_config_test tests/config_test.cpp)
  target_link_libraries(ember_config_test PRIVATE ember)
  add_executable(ember_system_test tests/system_test.cpp)
  target_link_libraries(ember_system_test PRIVATE ember_glfw)
  # Single-header consumption test: compiles the WHOLE library from
  # single_header/ember.hpp (EMBER_IMPLEMENTATION in this TU) and links only
  # the documented external deps — NOT ember.
  add_executable(ember_single_header_test tests/single_header_test.cpp)
  target_include_directories(ember_single_header_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/single_header
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
  target_link_libraries(ember_single_header_test PRIVATE glad glfw glm)
  enable_testing()
  add_test(NAME config_test COMMAND ember_config_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME system_test COMMAND ember_system_test)
  add_test(NAME single_header_test COMMAND ember_single_header_test)
  # Run from the source root so the bloom/sort shaders load from shaders/
  # (the library prefers editable files over its embedded copies).
  set_tests_properties(system_test PROPERTIES SKIP_RETURN_CODE 77
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(single_header_test PROPERTIES SKIP_RETURN_CODE 77
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
