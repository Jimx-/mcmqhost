set(LIBRARIES
    pthread
    atomic
    spdlog::spdlog
    cxxopts::cxxopts    
    unvme
    mcmq    
)

add_executable(loaddata data_loader.cpp)
target_link_libraries(loaddata ${LIBRARIES})
