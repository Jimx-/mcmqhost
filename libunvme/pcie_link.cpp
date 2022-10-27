#include "libunvme/pcie_link.h"
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

void PCIeLink::wait_for_device_ready()
{
    std::mutex ready_mutex;
    std::unique_lock<std::mutex> lock(ready_mutex);

    while (!device_ready)
        device_ready_cv.wait(lock);
}

void PCIeLink::start()
{
    event_fd = eventfd(0, 0);
    assert(event_fd >= 0);

    io_thread = std::thread([this]() { recv_thread(); });
}

void PCIeLink::stop()
{
    uint64_t val = 1;

    stopped.store(true);
    write(event_fd, &val, sizeof(val));

    io_thread.join();
    close(event_fd);
    event_fd = -1;
}
