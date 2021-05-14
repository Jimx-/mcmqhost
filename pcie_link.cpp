#include "pcie_link.h"

#include "spdlog/spdlog.h"

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <linux/vm_sockets.h>

bool PCIeLink::init()
{
    int fd, pfd, retval;

    if (sock_fd != -1) return true;

    fd = socket(AF_VSOCK, SOCK_STREAM, 0);

    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(struct sockaddr_vm));
    addr.svm_family = AF_VSOCK;
    addr.svm_port = 9999;
    addr.svm_cid = VMADDR_CID_HOST;

    retval =
        bind(fd, (const struct sockaddr*)&addr, sizeof(struct sockaddr_vm));
    if (retval != 0) {
        spdlog::error("Failed to bind vsock: {}", std::strerror(retval));
        return false;
    }

    retval = listen(fd, 0);
    if (retval != 0) {
        spdlog::error("Failed to listen on vsock: {}", std::strerror(retval));
        return false;
    }

    struct sockaddr_vm peer_addr;
    socklen_t peer_addr_size = sizeof(struct sockaddr_vm);
    pfd = accept(fd, (struct sockaddr*)&peer_addr, &peer_addr_size);

    if (pfd < 0) {
        spdlog::error("Failed to accept vsock connection: {}",
                      std::strerror(-pfd));
        return false;
    }

    sock_fd = fd;
    peer_fd = pfd;

    return true;
}

void PCIeLink::send_message(MessageType type, uint64_t addr, const void* buf,
                            size_t len)
{
    size_t msg_len = 2 + 8 + len;
    std::vector<uint8_t> msg;
    msg.resize(2 + msg_len);

    *(uint16_t*)&msg[0] = htons(msg_len);
    *(uint16_t*)&msg[2] = htons((uint16_t)type);
    *(uint32_t*)&msg[4] =
        ((unsigned long)htonl(addr >> 32 & 0xffffffff) << 32UL);
    *(uint32_t*)&msg[8] = htonl(addr & 0xffffffff);
    ::memcpy(&msg[12], buf, len);

    spdlog::trace("Sending PCIe message, addr = {}, buf = {}, len = {}", addr,
                  buf, len);
    {
        std::lock_guard<std::mutex> guard(mutex);

        int n = ::send(peer_fd, &msg[0], msg.size(), 0);
        if (n != msg.size()) {
            spdlog::error("Failed to send PCIe message");
            throw std::runtime_error("Failed to send PCIe message");
        }
    }
}
