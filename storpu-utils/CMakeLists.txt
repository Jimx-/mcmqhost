set(LIBRARIES
    pthread
    atomic
    CONAN_PKG::spdlog
    CONAN_PKG::cxxopts    
    unvme
    mcmq    
)

add_executable(loaddata data_loader.cpp)
target_link_libraries(loaddata ${LIBRARIES})
