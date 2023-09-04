set(PROTO_FILES proto/ssd_config.proto
                proto/sim_result.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
                
set(SOURCE_FILES
    config_reader.cpp
    result_exporter.cpp
    io_thread.cpp
    io_thread_synthetic.cpp
)

set(EXT_SOURCE_FILES
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

include_directories(${PROTOBUF_INCLUDE_DIRS})

set(LIBRARIES    
    spdlog::spdlog
    yaml-cpp
    nlohmann_json::nlohmann_json
    hdr_histogram::hdr_histogram_static
    protobuf::libprotobuf
)

ADD_LIBRARY(mcmq STATIC ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_include_directories(mcmq PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
TARGET_LINK_LIBRARIES(mcmq ${LIBRARIES})
