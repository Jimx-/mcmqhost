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

find_package(Protobuf REQUIRED)
if (NOT PROTOBUF_FOUND)
    message(FATAL_ERROR "Fatal error: Protobuf required.")
endif (NOT PROTOBUF_FOUND)

set(PROTO_FILES proto/ssd_config.proto
                proto/sim_result.proto)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

include_directories(${PROTOBUF_INCLUDE_DIRS}
                    ${CMAKE_CURRENT_BINARY_DIR}
                    )

set(SOURCE_FILES
    pcie_link.cpp
    pcie_link_mcmq.cpp
    pcie_link_vfio.cpp
    memory_space.cpp
    nvme_driver.cpp
    config_reader.cpp
    result_exporter.cpp
    io_thread.cpp
    io_thread_synthetic.cpp
    ${PROTO_SRCS}
)

set(EXT_SOURCE_FILES )

set(LIBRARIES
    pthread
    atomic
    CONAN_PKG::spdlog
    CONAN_PKG::cxxopts
    CONAN_PKG::yaml-cpp
    CONAN_PKG::nlohmann_json
    CONAN_PKG::hdrhistogram-c
    ${PROTOBUF_LIBRARIES}
)
    
add_executable(mcmqhost main.cpp ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(mcmqhost ${LIBRARIES})

add_executable(loaddata data_loader.cpp ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(loaddata ${LIBRARIES})
