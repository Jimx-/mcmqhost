#ifndef _PCIE_LINK_VFIO_H_
#define _PCIE_LINK_VFIO_H_

#include "pcie_link.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

class PCIeLinkVfio : public PCIeLink {
public:
    PCIeLinkVfio(const std::string& vfio_group, const std::string& device_id)
        : vfio_group(vfio_group), device_id(device_id), group_fd(-1),
          container_fd(-1), device_fd(-1), bar0_base(nullptr)
    {}

    ~PCIeLinkVfio();

    bool init();

    void map_dma(const MemorySpace& mem_space);

    MemorySpace* map_bar(unsigned int bar_id);

    void send_config(const mcmq::SsdConfig& config) {}

    void report(mcmq::SimResult& result) {}

private:
    std::string vfio_group;
    std::string device_id;
    int group_fd, container_fd, device_fd;
    std::vector<int> irq_fds;
    std::unordered_map<int, int> irq_map;
    void* bar0_base;
    size_t bar0_size;

    void recv_thread();

    virtual size_t read_from_device(uint64_t addr, void* buf, size_t buflen);
    virtual void write_to_device(uint64_t addr, const void* buf, size_t len);
};

#endif
