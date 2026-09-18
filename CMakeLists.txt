cmake_minimum_required(VERSION 3.16)
project(ember VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EMBER_BUILD_OPENGL "Build the OpenGL backend and compatibility API" ON)
option(EMBER_BUILD_VULKAN "Build the Vulkan backend" ON)
option(EMBER_BUILD_GLFW "Build the optional GLFW convenience module" ON)
option(EMBER_FETCH_DEPS   "Download glad/glfw/glm automatically (FetchContent)" ON)
option(EMBER_BUILD_EXAMPLES "Build the example programs" ON)
option(EMBER_BUILD_TESTS    "Build the standalone config tests" ON)
option(EMBER_BUILD_EDITOR   "Build the interactive emitter editor into the examples" ON)
option(EMBER_BUILD_BENCHMARKS "Build reproducible offscreen performance benchmarks" OFF)
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
    foreach(_name IN ITEMS glm glad glfw)
      if(NOT EMBER_BUILD_OPENGL AND NOT _name STREQUAL "glm")
        continue()
      endif()
      if(_name STREQUAL "glfw" AND NOT EMBER_BUILD_GLFW)
        continue()
      endif()
      if(_name STREQUAL "glm")
        set(_p "glm;1.0.1;1.0.1")
      elseif(_name STREQUAL "glad")
        set(_p "glad;2.0.8;v2.0.8")
      else()
        set(_p "glfw;3.4;3.4")
      endif()
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
      string(TOUPPER "${_name}" _upper_name)
      set("FETCHCONTENT_SOURCE_DIR_${_upper_name}" "${_offline}/${_name}-${_ver}" CACHE PATH "" FORCE)
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

  FetchContent_MakeAvailable(glm)
  if(EMBER_BUILD_OPENGL)
    FetchContent_MakeAvailable(glad)
    if(EMBER_BUILD_GLFW)
      FetchContent_MakeAvailable(glfw)
    endif()
  endif()
  # glad v2's CMake only DEFINES glad_add_library(); we must call it to create
  # the actual target (generates glad.c/gl.h at build time via Python).
  if(EMBER_BUILD_OPENGL)
    glad_add_library(glad STATIC REPRODUCIBLE API gl:core=4.3)
  endif()
else()
  find_package(glm REQUIRED)
  if(EMBER_BUILD_OPENGL)
    find_package(glad REQUIRED)
    if(EMBER_BUILD_GLFW)
      find_package(glfw3 REQUIRED)
    endif()
  endif()
endif()

# Accept FetchContent targets and namespaced installed dependency targets.
foreach(_dep IN ITEMS glm glad glfw)
  if(TARGET ${_dep})
    set(_ember_${_dep} ${_dep})
  elseif(TARGET ${_dep}::${_dep})
    set(_ember_${_dep} ${_dep}::${_dep})
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Vulkan backend (optional): needs Vulkan-Headers + the loader import library.
# GitHub is unreachable in the reference environment; MSYS2 provides both.
# Missing Vulkan silently skips the module so consumers without an SDK still
# build (same rationale as GLFW-disabled examples).
# ---------------------------------------------------------------------------
set(EMBER_HAVE_VULKAN OFF)
if(EMBER_BUILD_VULKAN)
  find_package(Vulkan QUIET)
  if(NOT Vulkan_FOUND)
    # Manual fallback when FindVulkan cannot infer MSYS2's cached-prefix layout.
    find_path(Vulkan_INCLUDE_DIR NAMES vulkan/vulkan.h)
    find_library(Vulkan_LIBRARY NAMES vulkan-1 vulkan)
    if(Vulkan_INCLUDE_DIR AND Vulkan_LIBRARY)
      set(Vulkan_FOUND TRUE)
      set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
      set(Vulkan_LIBRARIES "${Vulkan_LIBRARY}")
    endif()
  endif()
  if(Vulkan_FOUND)
    set(EMBER_HAVE_VULKAN ON)
  else()
    message(STATUS "ember: Vulkan not found; skipping the Vulkan backend")
  endif()
endif()

# Python drives the build-time SPIR-V embed step; optional like the GLFW module.
find_package(Python3 QUIET COMPONENTS Interpreter)

# Public CPU facade: configuration, emitters, request encoding and parameters.
add_library(ember_core STATIC src/particle_system.cpp)
add_library(ember::core ALIAS ember_core)
target_include_directories(ember_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(ember_core PUBLIC ${_ember_glm})
target_compile_features(ember_core PUBLIC cxx_std_17)
set(_ember_install_targets ember_core)

if(EMBER_BUILD_OPENGL)
  include(cmake/EmberOpenGLSources.cmake)
  # Preserve the existing ember target/archive for OpenGL consumers.
  add_library(ember STATIC ${EMBER_OPENGL_SOURCES})
  add_library(ember::ember ALIAS ember)
  add_library(ember::opengl ALIAS ember)
  target_link_libraries(ember PUBLIC ember_core ${_ember_glad})
  target_include_directories(ember PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
  target_compile_definitions(ember PRIVATE EMBER_USE_STB)
  list(APPEND _ember_install_targets ember)
  if(EMBER_BUILD_GLFW)
    add_library(ember_glfw STATIC src/glfw_window.cpp)
    add_library(ember::ember_glfw ALIAS ember_glfw)
    target_link_libraries(ember_glfw PUBLIC ember ${_ember_glfw})
    list(APPEND _ember_install_targets ember_glfw)
  endif()
endif()
# ---------------------------------------------------------------------------
# Build-time SPIR-V for the Vulkan path. The built-in GLSL carries no loose
# uniforms and uses explicit bindings/locations, so the same sources compile
# for both OpenGL and Vulkan. Binaries are produced offline (no runtime
# compiler dependency); the Vulkan backend embeds them via tools/embed_spirv.py.
# ---------------------------------------------------------------------------
option(EMBER_BUILD_SPIRV "Compile shaders/ to SPIR-V at build time (needs glslangValidator)" ON)
set(_ember_spv_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/spirv)
set(_ember_spv_outputs)
set(_ember_spv_shaders
  simulate.comp sort.comp schedule.comp
  particle.vert particle.frag
  bloom.vert bloom_threshold.frag bloom_blur.frag bloom_composite.frag)
if(EMBER_BUILD_SPIRV)
  find_program(EMBER_GLSLANG_VALIDATOR NAMES glslangValidator)
  if(EMBER_GLSLANG_VALIDATOR)
    foreach(_shader IN LISTS _ember_spv_shaders)
      add_custom_command(
        OUTPUT ${_ember_spv_dir}/${_shader}.spv
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_ember_spv_dir}
        COMMAND ${EMBER_GLSLANG_VALIDATOR} -V --target-env vulkan1.0
                -DEMBER_CLIP_VULKAN
                -o ${_ember_spv_dir}/${_shader}.spv
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
        COMMENT "ember SPIR-V: ${_shader}"
        VERBATIM)
      list(APPEND _ember_spv_outputs ${_ember_spv_dir}/${_shader}.spv)
    endforeach()
    add_custom_target(ember_spirv ALL DEPENDS ${_ember_spv_outputs})
  else()
    message(STATUS "ember: glslangValidator not found; SPIR-V generation skipped")
  endif()
endif()

if(EMBER_HAVE_VULKAN)
  # Prefer the freshly compiled SPIR-V; fall back to committed sources only if
  # glslangValidator was unavailable but the binaries already exist.
  if(NOT _ember_spv_outputs AND EXISTS ${_ember_spv_dir}/simulate.comp.spv)
    foreach(_shader IN LISTS _ember_spv_shaders)
      list(APPEND _ember_spv_outputs ${_ember_spv_dir}/${_shader}.spv)
    endforeach()
  endif()
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "ember: the Vulkan backend needs Python 3 for SPIR-V embedding")
  endif()
  set(_ember_spv_blob_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/spirv_blobs.cpp)
  add_library(ember_vulkan STATIC
    src/backends/vulkan/vulkan_backend.cpp
    src/backends/vulkan/context.cpp
    src/backends/vulkan/resources.cpp
    src/backends/vulkan/simulation.cpp
    src/backends/vulkan/statistics.cpp
    src/backends/vulkan/sort.cpp
    src/backends/vulkan/render.cpp
    src/backends/vulkan/bloom.cpp
    src/backends/vulkan/sprite.cpp
    src/backends/vulkan/options.cpp
    ${_ember_spv_blob_cpp})
  add_library(ember::vulkan ALIAS ember_vulkan)
  target_include_directories(ember_vulkan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_include_directories(ember_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${Vulkan_INCLUDE_DIRS})
  target_link_libraries(ember_vulkan PUBLIC ember_core ${Vulkan_LIBRARIES})
  # Same opt-in PNG support as the GL backend; without EMBER_USE_STB the
  # built-in gradient remains the fallback.
  target_compile_definitions(ember_vulkan PRIVATE EMBER_USE_STB)
  list(APPEND _ember_install_targets ember_vulkan)
  if(TARGET ember_spirv)
    add_dependencies(ember_vulkan ember_spirv)
  endif()
  add_custom_command(
    OUTPUT ${_ember_spv_blob_cpp}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py
            ${_ember_spv_dir} ${_ember_spv_blob_cpp}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py ${_ember_spv_outputs}
    COMMENT "ember SPIR-V: embedding built-in shaders"
    VERBATIM)
endif()

foreach(_target IN LISTS _ember_install_targets)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /W4)
  else()
    target_compile_options(${_target} PRIVATE -Wall -Wextra)
  endif()
endforeach()

if(EMBER_BUILD_BENCHMARKS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT TARGET ember_glfw)
    message(FATAL_ERROR "OpenGL benchmarks require EMBER_BUILD_OPENGL and EMBER_BUILD_GLFW")
  endif()
  configure_file(benchmarks/build_info.hpp.in generated/benchmark_build_info.hpp @ONLY)
  add_library(ember_benchmark_core STATIC ${EMBER_OPENGL_SOURCES} src/glfw_window.cpp)
  target_include_directories(ember_benchmark_core PRIVATE third_party/stb benchmarks)
  target_compile_definitions(ember_benchmark_core PRIVATE EMBER_USE_STB EMBER_BENCHMARK_HOOKS)
  target_link_libraries(ember_benchmark_core PUBLIC ember_core ${_ember_glad} ${_ember_glfw})
  foreach(_variant plain stages)
    add_executable(ember_benchmark_${_variant} benchmarks/particle_benchmark.cpp)
    target_include_directories(ember_benchmark_${_variant} PRIVATE benchmarks ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(ember_benchmark_${_variant} PRIVATE
      EMBER_BENCH_VARIANT="${_variant}" EMBER_BENCH_BUILD_TYPE="$<CONFIG>")
    if(_variant STREQUAL "plain")
      target_link_libraries(ember_benchmark_${_variant} PRIVATE ember_glfw)
    else()
      target_link_libraries(ember_benchmark_${_variant} PRIVATE ember_benchmark_core)
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Install / export so other projects can consume the library:
#   find_package(ember COMPONENTS core|opengl|glfw|vulkan)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ${_ember_install_targets}
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
if(EMBER_BUILD_EXAMPLES AND TARGET ember_glfw AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(example_basic examples/basic.cpp)
  target_link_libraries(example_basic PRIVATE ember_glfw)
  if(EMBER_BUILD_EDITOR)
    target_compile_definitions(example_basic PRIVATE EMBER_EDITOR_ENABLED)
  endif()
  add_executable(example_fireworks examples/fireworks.cpp)
  target_link_libraries(example_fireworks PRIVATE ember_glfw)
  add_executable(example_glass examples/glass.cpp)
  target_include_directories(example_glass PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
  target_link_libraries(example_glass PRIVATE ember_glfw)
endif()

# ---------------------------------------------------------------------------
# Standalone tests (config_test runs anywhere; system_test needs a GL context,
# skips with exit code 77 when none is available)
# ---------------------------------------------------------------------------
if(EMBER_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(ember_config_test tests/config_test.cpp)
  target_link_libraries(ember_config_test PRIVATE ember_core)
  add_executable(ember_backend_contract_test tests/backend_contract_test.cpp)
  target_link_libraries(ember_backend_contract_test PRIVATE ember_core)
  enable_testing()
  add_test(NAME config_test COMMAND ember_config_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME backend_contract_test COMMAND ember_backend_contract_test)
  if(TARGET ember_vulkan)
  add_executable(ember_vulkan_test tests/vulkan_test.cpp)
  target_link_libraries(ember_vulkan_test PRIVATE ember_vulkan)
  # When the GL backend is also built, link it so the same-process dual-backend
  # smoke can hold a GL context and a VkDevice alive together (WO-06).
  if(TARGET ember_glfw)
    target_link_libraries(ember_vulkan_test PRIVATE ember_glfw)
    target_compile_definitions(ember_vulkan_test PRIVATE EMBER_VULKAN_TEST_HAS_GL)
  endif()
  add_test(NAME vulkan_test COMMAND ember_vulkan_test)
  set_tests_properties(vulkan_test PROPERTIES SKIP_RETURN_CODE 77
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif() # Vulkan backend
  # Cross-backend consistency: one command stream drives the OpenGL and Vulkan
  # backends and their states are compared frame by frame.
  if(TARGET ember_glfw AND TARGET ember_vulkan)
  add_executable(ember_cross_backend_test tests/cross_backend_test.cpp)
  target_link_libraries(ember_cross_backend_test PRIVATE ember_glfw ember_vulkan)
  add_test(NAME cross_backend_test COMMAND ember_cross_backend_test)
  set_tests_properties(cross_backend_test PROPERTIES SKIP_RETURN_CODE 77
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
  if(TARGET ember_glfw)
  add_executable(ember_system_test tests/system_test.cpp)
  target_link_libraries(ember_system_test PRIVATE ember_glfw)
  # Single-header consumption test: compiles the WHOLE library from
  # single_header/ember.hpp (EMBER_IMPLEMENTATION in this TU) and links only
  # the documented external deps — NOT ember.
  add_executable(ember_single_header_test tests/single_header_test.cpp)
  target_include_directories(ember_single_header_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/single_header
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
  target_link_libraries(ember_single_header_test PRIVATE ${_ember_glad} ${_ember_glfw} ${_ember_glm})
  add_test(NAME system_test COMMAND ember_system_test)
  add_test(NAME single_header_test COMMAND ember_single_header_test)
  # Run from the source root so the bloom/sort shaders load from shaders/
  # (the library prefers editable files over its embedded copies).
  set_tests_properties(system_test PROPERTIES SKIP_RETURN_CODE 77
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(single_header_test PROPERTIES SKIP_RETURN_CODE 77
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif() # OpenGL context tests
  if(Python3_Interpreter_FOUND)
    add_test(NAME benchmark_report_test COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_report_test.py)
    add_test(NAME embedded_shader_sync COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/sync_embedded_shaders.py --verify)
    add_test(NAME single_header_sync COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py --verify)
    set_tests_properties(embedded_shader_sync single_header_sync PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
  if(EMBER_BUILD_BENCHMARKS)
    add_test(NAME benchmark_smoke COMMAND ember_benchmark_stages
      --counts 10000 --frames 8 --warmup 2 --repeats 1
      --shader-dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders
      --output ${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-ctest.json)
    add_test(NAME benchmark_gpu_smoke COMMAND ember_benchmark_stages
      --counts 10000 --frames 8 --warmup 2 --repeats 1 --scheduling gpu
      --shader-dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders
      --output ${CMAKE_CURRENT_BINARY_DIR}/benchmark-gpu-smoke-ctest.json)
  endif()
endif()
