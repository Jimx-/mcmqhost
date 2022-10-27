#include "libunvme/pcie_link_mcmq.h"
#include "ringbuffer.h"

#include "spdlog/spdlog.h"

#include <cstring>
#include <netinet/in.h>
#include <optional>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <linux/vm_sockets.h>

bool PCIeLinkMcmq::init()
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

void PCIeLinkMcmq::send_message(MessageType type, uint64_t addr,
                                const void* buf, size_t len)
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

    spdlog::trace("Sending PCIe message addr={} buf={} len={}", addr, buf, len);
    {
        std::lock_guard<std::mutex> guard(sock_mutex);

        int n = ::send(peer_fd, &msg[0], msg.size(), 0);
        if (n != msg.size()) {
            spdlog::error("Failed to send PCIe message");
            throw std::runtime_error("Failed to send PCIe message");
        }
    }
}

void PCIeLinkMcmq::send_config(const mcmq::SsdConfig& config)
{
    size_t msg_len = config.ByteSizeLong();
    std::vector<uint8_t> msg;
    msg.resize(2 + msg_len);

    *(uint16_t*)&msg[0] = htons(msg_len);

    config.SerializeToArray(&msg[2], msg_len);

    {
        std::lock_guard<std::mutex> guard(sock_mutex);

        int n = ::send(peer_fd, &msg[0], msg.size(), 0);
        if (n != msg.size()) {
            spdlog::error("Failed to send SSD config");
            throw std::runtime_error("Failed to send SSD config");
        }
    }
}

void PCIeLinkMcmq::report(mcmq::SimResult& result)
{
    std::unique_lock<std::mutex> lock(mutex);

    result_buf.clear();
    result_len = 0;
    result_ready = false;

    send_message(MessageType::REPORT, 0, nullptr, 0);

    while (!result_ready)
        result_cv.wait(lock);

    spdlog::trace("Buffer size {}", result_buf.size());
    result.ParseFromArray(&result_buf[0], result_buf.size());
}

PCIeLinkMcmq::ReadRequest* PCIeLinkMcmq::setup_read_request(void* buf,
                                                            size_t buflen)
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

size_t PCIeLinkMcmq::read_from_device(uint64_t addr, void* buf, size_t buflen)
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

void PCIeLinkMcmq::complete_read_request(uint32_t id, const void* buf,
                                         size_t len)
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

void PCIeLinkMcmq::recv_thread()
{
    int epfd;
    struct epoll_event events[2] = {0};
    uint8_t rbuf[4096];
    jnk0le::Ringbuffer<uint8_t, 16384> ringbuf;
    int retval;

    epfd = epoll_create1(0);
    assert(epfd >= 0);

    events[0].events = EPOLLIN;

    events[0].data.fd = peer_fd;
    retval = epoll_ctl(epfd, EPOLL_CTL_ADD, peer_fd, &events[0]);
    assert(!retval);

    events[0].data.fd = event_fd;
    retval = epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &events[0]);
    assert(!retval);

    while (!stopped.load()) {
        std::optional<uint16_t> last_len;
        int nevents = epoll_wait(epfd, events, 2, -1);
        if (nevents < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (stopped.load()) break;

        for (int i = 0; i < nevents; i++) {
            struct epoll_event* event = &events[i];

            if (event->data.fd == peer_fd) {
                int n = ::recv(peer_fd, rbuf, sizeof(rbuf), 0);

                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }

                spdlog::trace("Receive {} bytes from socket", n);

                ringbuf.writeBuff(rbuf, n);

                while ((!last_len && ringbuf.readAvailable() > 2) ||
                       (last_len && ringbuf.readAvailable() >= *last_len)) {
                    uint16_t len, type, vector, buf_len;
                    uint32_t id;
                    size_t offset = 0;
                    std::vector<uint8_t> payload;

                    if (!last_len) {
                        ringbuf.readBuff((uint8_t*)&len, sizeof(len));
                        len = ntohs(len);

                        if (ringbuf.readAvailable() < len) {
                            last_len = len;
                            continue;
                        }
                    } else {
                        len = *last_len;
                        last_len = std::nullopt;
                    }

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

                        spdlog::trace(
                            "Receive read complete message id={} len={}", id,
                            len);

                        complete_read_request(id, &payload[offset], len);
                        break;
                    case (int)MessageType::IRQ:
                        assert(len == sizeof(vector));
                        vector = ntohs(*(uint16_t*)&payload[offset]);
                        len -= sizeof(vector);
                        offset += sizeof(vector);

                        spdlog::trace("Receive IRQ message vector={}", vector);

                        if (irq_handler) irq_handler(vector);
                        break;
                    case (int)MessageType::DEV_READY: {
                        spdlog::trace("Receive device ready message");
                        set_ready();
                        break;
                    }
                    case (int)MessageType::RESULT: {
                        spdlog::trace("Receive result message");

                        std::unique_lock<std::mutex> lock(mutex);

                        if (result_buf.empty()) {
                            assert(len >= sizeof(buf_len));
                            buf_len = ntohs(*(uint16_t*)&payload[offset]);
                            len -= sizeof(buf_len);
                            offset += sizeof(buf_len);

                            result_len = buf_len;
                        }

                        result_buf.reserve(
                            result_buf.size() +
                            distance(payload.begin() + offset, payload.end()));
                        result_buf.insert(result_buf.end(),
                                          payload.begin() + offset,
                                          payload.end());

                        if (result_buf.size() == result_len) {
                            result_ready = true;
                            result_cv.notify_all();
                        }
                        break;
                    }
                    default:
                        spdlog::error("Bad message type {}", type);
                        break;
                    }
                }
            }
        }
    }

    close(epfd);
}
