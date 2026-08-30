cmake_minimum_required(VERSION 3.21)
project(BandwidthGovernor VERSION 1.0.0 LANGUAGES CXX)

option(BG_BUILD_TESTS "Build the Bandwidth Governor test suites" ON)
option(BG_BUILD_BENCHMARKS "Build the Bandwidth Governor benchmarks" ON)
option(BG_BUILD_EXAMPLES "Build the Bandwidth Governor examples" ON)
option(BG_BUILD_CLI "Build the Bandwidth Governor CLI" ON)
option(BG_BUILD_CONSUMER "Build the downstream find_package consumer (standalone; built against the install prefix)" OFF)
option(BG_BUILD_WORKER "Build the distributed worker runtime" ON)
option(BG_BUILD_CUDA "Build the CUDA backend" ON)
option(BG_ENABLE_ASAN "Enable AddressSanitizer in Debug" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /WX /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

set(BG_CORE_SOURCES
  src/core.cpp
  src/resource.cpp
  src/flow.cpp
  src/policy.cpp
  src/explain.cpp
  src/reservation.cpp
  src/path.cpp
  src/governor.cpp
  src/persist.cpp
  src/backend.cpp
  src/wire.cpp
  src/transport.cpp
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(bgcore ${BG_CORE_SOURCES})
add_library(Bandwidth::bgcore ALIAS bgcore)
target_include_directories(bgcore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(bgcore PUBLIC cxx_std_20)
set_target_properties(bgcore PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
if(WIN32)
  target_link_libraries(bgcore PRIVATE ws2_32)
endif()

target_sources(bgcore PRIVATE src/backend_cuda.cpp)
find_package(CUDAToolkit QUIET)
if(BG_BUILD_CUDA AND CUDAToolkit_FOUND)
  message(STATUS "CUDA present; enabling CUDA transfer backend")
  target_compile_definitions(bgcore PRIVATE BG_HAVE_CUDA=1)
  target_include_directories(bgcore PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  target_link_libraries(bgcore PRIVATE CUDA::cudart)
else()
  message(STATUS "CUDA transfer backend stubbed (no CUDA enabled)")
endif()

if(BG_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(BG_BUILD_WORKER)
  add_subdirectory(workers)
endif()
if(BG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(BG_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(BG_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(BG_BUILD_CONSUMER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/consumer/CMakeLists.txt")
  # The consumer is a standalone downstream project; it is built separately
  # against the install prefix (see docs/BUILDING.md).
  message(STATUS "Downstream consumer is a standalone project (build after install)")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS bgcore
  EXPORT BandwidthGovernorTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(BG_CMAKE_CONFIG_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/BandwidthGovernor)

install(EXPORT BandwidthGovernorTargets
  FILE BandwidthGovernorTargets.cmake
  NAMESPACE Bandwidth::
  DESTINATION ${BG_CMAKE_CONFIG_DIR})

if(CUDAToolkit_FOUND AND BG_BUILD_CUDA)
  set(BG_CONFIG_WITH_CUDA 1)
else()
  set(BG_CONFIG_WITH_CUDA 0)
endif()

configure_package_config_file(
  cmake/BandwidthGovernorConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthGovernorConfig.cmake
  INSTALL_DESTINATION ${BG_CMAKE_CONFIG_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthGovernorConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthGovernorConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthGovernorConfigVersion.cmake
  DESTINATION ${BG_CMAKE_CONFIG_DIR})
