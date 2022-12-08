set(PROTO_FILES proto/ssd_config.proto
                proto/sim_result.proto)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

set(SOURCE_FILES
    config_reader.cpp
    result_exporter.cpp
    io_thread.cpp
    io_thread_synthetic.cpp
)

set(EXT_SOURCE_FILES
    ${PROTO_SRCS}
)

include_directories(${PROTOBUF_INCLUDE_DIRS})

set(LIBRARIES    
    CONAN_PKG::spdlog
    CONAN_PKG::yaml-cpp
    CONAN_PKG::nlohmann_json
    CONAN_PKG::hdrhistogram-c    
    ${PROTOBUF_LIBRARIES}    
)

ADD_LIBRARY(mcmq STATIC ${SOURCE_FILES} ${EXT_SOURCE_FILES})
TARGET_LINK_LIBRARIES(mcmq ${LIBRARIES})
