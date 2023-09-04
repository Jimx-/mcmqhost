set(SOURCE_FILES
    main.cpp
)

set(EXT_SOURCE_FILES
    ${PROTO_SRCS}
)

set(LIBRARIES
    pthread
    atomic
    spdlog::spdlog
    cxxopts::cxxopts
    unvme
    mcmq    
)

add_executable(mcmqhost ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(mcmqhost ${LIBRARIES})
