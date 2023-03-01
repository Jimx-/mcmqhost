cmake_minimum_required(VERSION 3.5)
project(mcmqhost)

set(CMAKE_CXX_STANDARD 17)

set(TOPDIR ${PROJECT_SOURCE_DIR})

set(CMAKE_MODULE_PATH "${TOPDIR}/cmake;${CMAKE_MODULE_PATH}")

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

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

find_package(Protobuf REQUIRED)
if (NOT PROTOBUF_FOUND)
    message(FATAL_ERROR "Fatal error: Protobuf required.")
endif (NOT PROTOBUF_FOUND)

find_package(FUSE3 3.12 REQUIRED)
if (NOT FUSE3_FOUND)
    message(FATAL_ERROR "Fatal error: Fuse required.")
endif (NOT FUSE3_FOUND)

include_directories(${TOPDIR}/include
                    ${TOPDIR}/lib
                    ${CMAKE_CURRENT_BINARY_DIR}
                    )

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
