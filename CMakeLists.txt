cmake_minimum_required(VERSION 3.5)
project(mcmqhost)

set(CMAKE_CXX_STANDARD 17)

set(TOPDIR ${PROJECT_SOURCE_DIR})

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

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
