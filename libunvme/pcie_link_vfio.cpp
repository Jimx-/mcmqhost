#include "libunvme/pcie_link_vfio.h"

#include "spdlog/spdlog.h"

#include <cstring>
#include <fcntl.h>
#include <linux/vfio.h>
#include <netinet/in.h>
#include <optional>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <linux/vm_sockets.h>

PCIeLinkVfio::~PCIeLinkVfio()
{
    close(device_fd);
    close(group_fd);
    close(container_fd);

    if (bar0_base) munmap(bar0_base, bar0_size);
}

bool PCIeLinkVfio::init()
{
    int i;
    struct vfio_group_status group_status = {.argsz = sizeof(group_status)};
    struct vfio_device_info device_info = {.argsz = sizeof(device_info)};
    struct vfio_irq_set* irq_set;
    void *map_base, *cfg_base, *dma_base;

    container_fd = open("/dev/vfio/vfio", O_RDWR);

    if (ioctl(container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        spdlog::error("unknown VFIO API version");
        return false;
    }

    if (!ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        spdlog::error("IOMMU driver not supported");
        return false;
    }

    group_fd = open(("/dev/vfio/" + vfio_group).c_str(), O_RDWR);

    ioctl(group_fd, VFIO_GROUP_GET_STATUS, &group_status);

    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        spdlog::error("VFIO group is not viable");
        return false;
    }

    ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd);

    ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

    device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, device_id.c_str());

    ioctl(device_fd, VFIO_DEVICE_GET_INFO, &device_info);

    {
        // Enable bus master
        uint16_t pci_cmd;
        struct vfio_region_info reg = {.argsz = sizeof(reg)};
        reg.index = VFIO_PCI_CONFIG_REGION_INDEX;

        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg);

        pread(device_fd, &pci_cmd, sizeof(pci_cmd), reg.offset + 4);
        pci_cmd |= 0x4;
        pwrite(device_fd, &pci_cmd, sizeof(pci_cmd), reg.offset + 4);
    }

    {
        struct vfio_region_info reg = {.argsz = sizeof(reg)};
        reg.index = VFIO_PCI_BAR0_REGION_INDEX;

        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg);

        bar0_size = reg.size;
        bar0_base = mmap(NULL, bar0_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         device_fd, reg.offset);
    }

    for (i = 0; i < 16; i++) {
        int irq_fd = eventfd(0, 0);
        irq_fds.push_back(irq_fd);
        irq_map[irq_fd] = i;
    }

    irq_set = (struct vfio_irq_set*)new uint8_t[sizeof(*irq_set) +
                                                16 * sizeof(uint32_t)];
    irq_set->argsz = sizeof(*irq_set) + 16 * sizeof(uint32_t);

    for (i = 0; i < device_info.num_irqs; i++) {
        struct vfio_irq_info irq = {.argsz = sizeof(irq)};
        int j;
        int32_t* pfd = (int32_t*)&irq_set->data;

        irq.index = i;

        ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq);

        if (irq.count < 16) continue;

        irq_set->flags =
            VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
        irq_set->index = i;
        irq_set->start = 0;
        irq_set->count = irq.count;

        for (j = 0; j < irq.count; j++)
            pfd[j] = irq_fds[j];

        ioctl(device_fd, VFIO_DEVICE_SET_IRQS, irq_set);
    }
    delete irq_set;

    set_ready();

    return true;
}

void PCIeLinkVfio::map_dma(const MemorySpace& mem_space)
{
    int r;
    struct vfio_iommu_type1_dma_map dma_map = {.argsz = sizeof(dma_map)};

    dma_map.vaddr = (uintptr_t)mem_space.get_map_base();
    dma_map.size = mem_space.get_map_size();
    dma_map.iova = mem_space.get_iova_base();
    dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

    r = ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map);
    if (r < 0) {
        spdlog::error("Failed to map DMA memory: {}", errno);
        abort();
    }
}

MemorySpace* PCIeLinkVfio::map_bar(unsigned int bar_id)
{
    size_t bar_size;
    void* bar_base;

    struct vfio_region_info reg = {.argsz = sizeof(reg)};
    reg.index = VFIO_PCI_BAR0_REGION_INDEX + bar_id;

    ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg);

    bar_size = reg.size;
    bar_base = mmap(NULL, bar_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    device_fd, reg.offset);

    return new BARMemorySpace(bar_base, bar_size);
}

size_t PCIeLinkVfio::read_from_device(uint64_t addr, void* buf, size_t buflen)
{
    uint32_t lo, hi;

    switch (buflen) {
    case 4:
        *(uint32_t*)buf = *(volatile uint32_t*)((uintptr_t)bar0_base + addr);
        break;
    case 8:
        lo = *(volatile uint32_t*)((uintptr_t)bar0_base + addr);
        hi = *(volatile uint32_t*)((uintptr_t)bar0_base + addr + 4);
        *(uint64_t*)buf = ((uint64_t)hi << 32) | lo;
        break;
    default:
        spdlog::error("Unsupport I/O size: {}", buflen);
    }

    return buflen;
}

void PCIeLinkVfio::write_to_device(uint64_t addr, const void* buf, size_t len)
{
    uint64_t u64_val;

    switch (len) {
    case 4:
        *(volatile uint32_t*)((uintptr_t)bar0_base + addr) = *(uint32_t*)buf;
        break;
    case 8:
        u64_val = *(uint64_t*)buf;
        *(volatile uint32_t*)((uintptr_t)bar0_base + addr) =
            u64_val & 0xffffffff;
        *(volatile uint32_t*)((uintptr_t)bar0_base + addr + 4) =
            (u64_val >> 32) & 0xffffffff;
        break;
    default:
        spdlog::error("Unsupport I/O size: {}", len);
    }
}

void PCIeLinkVfio::recv_thread()
{
    int epfd;
    struct epoll_event events[2] = {0};
    int retval;

    epfd = epoll_create1(0);
    assert(epfd >= 0);

    events[0].events = EPOLLIN;

    events[0].data.fd = event_fd;
    retval = epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &events[0]);
    assert(!retval);

    for (auto fd : irq_fds) {
        events[0].data.fd = fd;
        retval = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &events[0]);
        assert(!retval);
    }

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
            auto it = irq_map.find(event->data.fd);

            if (it != irq_map.end()) {
                uint64_t val;

                read(event->data.fd, &val, sizeof(val));

                if (irq_handler) irq_handler(it->second);
            }
        }
    }

    close(epfd);
}
