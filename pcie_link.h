#ifndef _PCIE_LINK_H_
#define _PCIE_LINK_H_

#include <cstddef>
#include <cstdint>
#include <mutex>

class PCIeLink {
public:
    PCIeLink() : sock_fd(-1) {}

    bool init();

    void write_to_device(uint64_t addr, const void* buf, size_t len)
    {
        send_message(MessageType::WRITE_REQ, addr, buf, len);
    }

private:
    int sock_fd, peer_fd;
    std::mutex mutex;

    enum class MessageType {
        READ_REQ = 1,
        WRITE_REQ = 2,
    };

    void send_message(MessageType type, uint64_t addr, const void* buf,
                      size_t len);
};

#endif
