#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_vfio.h"

#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

static NVMeDriver* g_nvme_driver;
static MemorySpace* g_memory_space;
static PCIeLink* g_pcie_link;

extern "C"
{

    void unvme_init_driver(unsigned int num_workers, const char* group,
                           const char* device_id)
    {
        spdlog::cfg::load_env_levels();

        g_memory_space = new VfioMemorySpace(0x1000, 2 * 1024 * 1024);
        g_pcie_link =
            new PCIeLinkVfio(std::string(group), std::string(device_id));

        g_pcie_link->init();

        g_pcie_link->map_dma(*g_memory_space);
        g_pcie_link->start();

        g_nvme_driver = new NVMeDriver(num_workers, 1024, g_pcie_link,
                                       g_memory_space, false);
        g_nvme_driver->start();
    }

    void unvme_shutdown_driver()
    {
        if (g_nvme_driver) {
            g_nvme_driver->shutdown();
        }

        if (g_pcie_link) {
            g_pcie_link->stop();
        }

        delete g_memory_space;
        delete g_pcie_link;
        delete g_nvme_driver;

        g_memory_space = nullptr;
        g_pcie_link = nullptr;
        g_nvme_driver = nullptr;
    }

    void unvme_set_queue(unsigned int qid)
    {
        g_nvme_driver->set_thread_id(qid);
    }

    int unvme_read(unsigned int nsid, char* buf, size_t size, loff_t offset)
    {
        auto* mem_space = g_nvme_driver->get_dma_space();
        MemorySpace::Address dma_buf = mem_space->allocate_pages(size);
        int r = 0;

        spdlog::info("Read buffer {} {} {} {}", nsid, (void*)buf, size, offset);
        try {
            g_nvme_driver->read(nsid, offset, dma_buf, size);

            mem_space->read(dma_buf, buf, size);
        } catch (const NVMeDriver::DeviceIOError&) {
            r = -1;
        }

        mem_space->free(dma_buf, size);

        return r;
    }
}
