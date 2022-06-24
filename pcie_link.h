#ifndef _PCIE_LINK_H_
#define _PCIE_LINK_H_

#include "memory_space.h"
#include "sim_result.pb.h"
#include "ssd_config.pb.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

class PCIeLink {
public:
    PCIeLink() : event_fd(-1), stopped(false), device_ready(false) {}

    virtual ~PCIeLink() {}

    virtual bool init() = 0;

    void start();
    void stop();

    void set_irq_handler(std::function<void(uint16_t)>&& handler)
    {
        irq_handler = handler;
    }

    virtual void map_dma(const MemorySpace& mem_space) = 0;

    virtual MemorySpace* map_bar(unsigned int bar_id) = 0;

    virtual void send_config(const mcmq::SsdConfig& config) = 0;

    virtual void report(mcmq::SimResult& result) = 0;

    void wait_for_device_ready();

    uint32_t readl(uint64_t addr)
    {
        uint32_t val;
        read_from_device(addr, &val, sizeof(val));
        return val;
    }

    void writel(uint64_t addr, uint32_t val)
    {
        write_to_device(addr, &val, sizeof(val));
    }

    uint64_t readq(uint64_t addr)
    {
        uint64_t val;
        read_from_device(addr, &val, sizeof(val));
        return val;
    }

    void writeq(uint64_t addr, uint64_t val)
    {
        write_to_device(addr, &val, sizeof(val));
    }

protected:
    int event_fd;
    std::atomic<bool> stopped;
    std::function<void(uint16_t)> irq_handler;

    virtual void recv_thread() = 0;

    virtual size_t read_from_device(uint64_t addr, void* buf,
                                    size_t buflen) = 0;
    virtual void write_to_device(uint64_t addr, const void* buf,
                                 size_t len) = 0;

    void set_ready()
    {
        device_ready = true;
        device_ready_cv.notify_all();
    }

private:
    std::thread io_thread;
    bool device_ready;
    std::condition_variable device_ready_cv;
};

#endif
