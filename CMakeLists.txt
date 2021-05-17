cmake_minimum_required(VERSION 3.5)
project(mcmqhost)

set(CMAKE_CXX_STANDARD 17)

set(TOPDIR ${PROJECT_SOURCE_DIR})

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

set(SOURCE_FILES
    main.cpp
)

set(EXT_SOURCE_FILES
    pcie_link.cpp
    memory_space.cpp
    nvme_driver.cpp
    main.cpp
)

set(LIBRARIES
    pthread
    atomic
    CONAN_PKG::spdlog
    CONAN_PKG::cxxopts
)
    
add_executable(mcmqhost ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(mcmqhost ${LIBRARIES})
