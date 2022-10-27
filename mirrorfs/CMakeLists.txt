set(SOURCE_FILES
    mirrorfs.cpp
)

include_directories(${FUSE3_INCLUDE_DIRS})

set(LIBRARIES
    pthread
    atomic
    CONAN_PKG::spdlog
    CONAN_PKG::cxxopts
    unvme
    ${FUSE3_LIBRARIES}
)

add_executable(mirrorfs mirrorfs.cpp ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(mirrorfs ${LIBRARIES})
