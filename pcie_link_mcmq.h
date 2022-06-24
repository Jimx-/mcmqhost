#ifndef _PCIE_LINK_MCMQ_H_
#define _PCIE_LINK_MCMQ_H_

#include "pcie_link.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

class PCIeLinkMcmq : public PCIeLink {
public:
    PCIeLinkMcmq() : sock_fd(-1), peer_fd(-1) {}

    virtual bool init();

    virtual void map_dma(const MemorySpace& mem_space) {}

    virtual MemorySpace* map_bar(unsigned int bar_id) { return nullptr; }

    void send_config(const mcmq::SsdConfig& config);

    void report(mcmq::SimResult& result);

private:
    int sock_fd, peer_fd;
    std::mutex mutex, sock_mutex;
    std::atomic<uint32_t> read_id_counter;

    std::vector<uint8_t> result_buf;
    size_t result_len;
    bool result_ready;
    std::condition_variable result_cv;

    enum class MessageType {
        READ_REQ = 1,
        WRITE_REQ = 2,
        READ_COMP = 3,
        IRQ = 4,
        DEV_READY = 5,
        REPORT = 6,
        RESULT = 7,
    };

    struct ReadRequest {
        std::mutex mutex;
        std::condition_variable cv;

        uint32_t id;
        void* buf;
        size_t buflen;
        size_t len;
        bool completed;
    };

    std::unordered_map<uint32_t, std::unique_ptr<ReadRequest>> read_requests;

    void send_message(MessageType type, uint64_t addr, const void* buf,
                      size_t len);

    void recv_thread();

    ReadRequest* setup_read_request(void* buf, size_t buflen);
    void complete_read_request(uint32_t id, const void* buf, size_t len);

    virtual size_t read_from_device(uint64_t addr, void* buf, size_t buflen);
    virtual void write_to_device(uint64_t addr, const void* buf, size_t len)
    {
        send_message(MessageType::WRITE_REQ, addr, buf, len);
    }
};

#endif
