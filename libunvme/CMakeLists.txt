set(SOURCE_FILES
    pcie_link.cpp
    pcie_link_mcmq.cpp
    pcie_link_vfio.cpp
    memory_space.cpp
    nvme_driver.cpp
    capi.cpp
)

set(LIBRARIES
    pthread
    atomic
    CONAN_PKG::spdlog
)

add_library(unvme STATIC ${SOURCE_FILES})
target_link_libraries(unvme ${LIBRARIES})
