#include "pcie_link.h"
#include "ringbuffer.h"

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

void PCIeLink::stop() { stopped.store(true); }

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

    if (buf) ::memcpy(&msg[12], buf, len);

    spdlog::trace("Sending PCIe message, addr = {}, buf = {}, len = {}", addr,
                  buf, len);
    {
        std::lock_guard<std::mutex> guard(sock_mutex);

        int n = ::send(peer_fd, &msg[0], msg.size(), 0);
        if (n != msg.size()) {
            spdlog::error("Failed to send PCIe message");
            throw std::runtime_error("Failed to send PCIe message");
        }
    }
}

PCIeLink::ReadRequest* PCIeLink::setup_read_request(void* buf, size_t buflen)
{
    std::lock_guard<std::mutex> guard(mutex);
    auto id = read_id_counter.fetch_add(1);

    auto it = read_requests.emplace(id, std::make_unique<ReadRequest>()).first;
    auto& req = it->second;
    req->id = id;
    req->buf = buf;
    req->buflen = buflen;
    req->completed = false;

    return it->second.get();
}

size_t PCIeLink::read_from_device(uint64_t addr, void* buf, size_t buflen)
{
    size_t outlen;
    auto* req = setup_read_request(buf, buflen);

    send_message(MessageType::READ_REQ, addr, &req->id, sizeof(req->id));

    {
        std::unique_lock<std::mutex> lock(req->mutex);

        while (!req->completed)
            req->cv.wait(lock);

        outlen = req->len;
    }

    {
        std::lock_guard<std::mutex> guard(mutex);
        read_requests.erase(req->id);
    }

    return outlen;
}

void PCIeLink::complete_read_request(uint32_t id, const void* buf, size_t len)
{
    std::lock_guard<std::mutex> guard(mutex);
    auto it = read_requests.find(id);

    if (it == read_requests.end()) {
        spdlog::error("Read completion message without read request id={}", id);
        return;
    }

    auto& req = it->second;
    {
        std::unique_lock<std::mutex> lock(req->mutex);

        auto outlen = std::min(len, req->buflen);
        ::memcpy(req->buf, buf, outlen);
        req->len = outlen;
        req->completed = true;

        req->cv.notify_all();
    }
}

void PCIeLink::recv_thread()
{
    uint8_t rbuf[4096];
    jnk0le::Ringbuffer<uint8_t, 4096> ringbuf;

    while (!stopped.load()) {
        int n = ::recv(peer_fd, rbuf, sizeof(rbuf), 0);

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        ringbuf.writeBuff(rbuf, n);

        while (ringbuf.readAvailable() > 2) {
            uint16_t len;
            uint16_t type;
            uint32_t id;
            size_t offset = 0;
            std::vector<uint8_t> payload;

            ringbuf.readBuff((uint8_t*)&len, sizeof(len));
            len = ntohs(len);
            assert(ringbuf.readAvailable() >= len);

            payload.resize(len);
            ringbuf.readBuff(&payload[0], len);

            assert(len >= sizeof(type));
            type = *(uint16_t*)&payload[offset];
            type = ntohs(type);
            len -= sizeof(type);
            offset += sizeof(type);

            switch (type) {
            case (int)MessageType::READ_COMP:
                assert(len >= sizeof(id));
                id = *(uint32_t*)&payload[offset];
                len -= sizeof(id);
                offset += sizeof(id);

                spdlog::trace("Receive read complete message id={} len={}", id,
                              len);

                complete_read_request(id, &payload[offset], len);
                break;
            }
        }
    }
}
