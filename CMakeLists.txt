cmake_minimum_required(VERSION 4.0)

find_package(cmake-harden REQUIRED PATHS node_modules/cmake-harden)

project(cpuinfo C)

add_library(cpuinfo OBJECT)

harden(cpuinfo)

set_target_properties(
  cpuinfo
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  cpuinfo
  INTERFACE
    include/cpuinfo.h
)

if(APPLE)
  target_sources(
    cpuinfo
    PRIVATE
      src/darwin.c
  )
elseif(LINUX OR ANDROID)
  target_sources(
    cpuinfo
    PRIVATE
      src/linux.c
  )
elseif(WIN32)
  target_sources(
    cpuinfo
    PRIVATE
      src/win32.c
  )

  target_link_libraries(
    cpuinfo
    PUBLIC
      advapi32
      powrprof
  )
endif()

target_include_directories(
  cpuinfo
  PUBLIC
    include
)

add_library(cpuinfo_shared SHARED)

set_target_properties(
  cpuinfo_shared
  PROPERTIES
  OUTPUT_NAME cpuinfo
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_link_libraries(
  cpuinfo_shared
  PUBLIC
    cpuinfo
)

add_library(cpuinfo_static STATIC)

set_target_properties(
  cpuinfo_static
  PROPERTIES
  OUTPUT_NAME cpuinfo
  PREFIX lib
)

target_link_libraries(
  cpuinfo_static
  PUBLIC
    cpuinfo
)

install(TARGETS cpuinfo_shared cpuinfo_static)

install(FILES include/cpuinfo.h DESTINATION include)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
