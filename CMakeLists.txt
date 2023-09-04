cmake_minimum_required(VERSION 3.5)
project(mcmqhost)

set(CMAKE_CXX_STANDARD 17)

set(TOPDIR ${PROJECT_SOURCE_DIR})

set(CMAKE_MODULE_PATH "${TOPDIR}/cmake;${CMAKE_MODULE_PATH}")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Boost COMPONENTS system)
if (NOT Boost_FOUND)
    message(FATAL_ERROR "Fatal error: Boost (version >= 1.55) required.")
else()
    message(STATUS "Setting up BOOST")
    message(STATUS " Includes - ${Boost_INCLUDE_DIRS}")
    message(STATUS " Library  - ${Boost_LIBRARY_DIRS}")
    message(STATUS " boost_system ${Boost_SYSTEM_LIBRARY}")
    message(STATUS " boost_filesystem ${Boost_FILESYSTEM_LIBRARY}")
    message(STATUS " boost_iostreams ${Boost_IOSTREAMS_LIBRARY}")
    include_directories(${Boost_INCLUDE_DIRS})
    link_directories(${Boost_LIBRARY_DIRS})
endif (NOT Boost_FOUND)

find_package(Protobuf REQUIRED CONFIG)
if (NOT Protobuf_FOUND)
    message(FATAL_ERROR "Fatal error: Protobuf required.")
endif (NOT Protobuf_FOUND)
include_directories(${Protobuf_INCLUDE_DIRS})

find_package(FUSE3 3.12 REQUIRED)
if (NOT FUSE3_FOUND)
    message(FATAL_ERROR "Fatal error: Fuse required.")
endif (NOT FUSE3_FOUND)

find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(hdr_histogram REQUIRED)
find_package(yaml-cpp REQUIRED)

include_directories(${TOPDIR}/include
                    ${TOPDIR}/lib
                    ${CMAKE_BINARY_DIR}/libmcmq)

option(USE_INTERPROCESS "Enable interprocess support" OFF)

if (USE_INTERPROCESS)
   add_definitions(-DENABLE_INTERPROCESS)
endif()

add_subdirectory(libmcmq)
add_subdirectory(libunvme)
add_subdirectory(mcmqhost)
add_subdirectory(storpu-utils)
add_subdirectory(mirrorfs)

if (IS_DIRECTORY "${TOPDIR}/local")
   add_subdirectory(local)
endif()
