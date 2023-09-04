set(SOURCE_FILES
    pcie_link.cpp
    pcie_link_mcmq.cpp
    pcie_link_vfio.cpp
    memory_space.cpp
    nvme_driver.cpp
)

set(LIBRARIES
    pthread
    atomic
    spdlog::spdlog
    protobuf::libprotobuf
)

add_library(unvme STATIC ${SOURCE_FILES})
target_link_libraries(unvme ${LIBRARIES})
