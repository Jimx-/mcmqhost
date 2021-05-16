#ifndef _PCIE_LINK_H_
#define _PCIE_LINK_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

class PCIeLink {
public:
    PCIeLink() : sock_fd(-1), stopped(false) {}

    bool init();
    void stop();

    void recv_thread();

    size_t read_from_device(uint64_t addr, void* buf, size_t buflen);

    void write_to_device(uint64_t addr, const void* buf, size_t len)
    {
        send_message(MessageType::WRITE_REQ, addr, buf, len);
    }

private:
    int sock_fd, peer_fd;
    std::mutex mutex, sock_mutex;
    std::atomic<bool> stopped;
    std::atomic<uint32_t> read_id_counter;

    enum class MessageType {
        READ_REQ = 1,
        WRITE_REQ = 2,
        READ_COMP = 3,
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

    ReadRequest* setup_read_request(void* buf, size_t buflen);
    void complete_read_request(uint32_t id, const void* buf, size_t len);
};

#endif
