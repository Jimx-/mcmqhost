#include "nvme_driver.h"
#include "nvme.h"

#include "spdlog/spdlog.h"

#define SQ_SIZE(nvmeq) ((nvmeq)->depth << ((nvmeq)->sqe_shift))
#define CQ_SIZE(nvmeq) ((nvmeq)->depth * sizeof(NVMeDriver::NVMeCompletion))

NVMeDriver::NVMeDriver(unsigned ncpus, PCIeLink* link,
                       MemorySpace* memory_space)
    : link(link), memory_space(memory_space)
{
    /* IO queues + admin queue */
    for (int i = 0; i < ncpus + 1; i++)
        queues.emplace_back(std::make_unique<NVMeQueue>());

    setup_admin_queue();
}

void NVMeDriver::allocate_queue(unsigned qid, unsigned depth)
{
    auto& nvmeq = queues[qid];

    nvmeq->sqe_shift = qid ? NVM_IOSQES : ADM_SQES;
    nvmeq->depth = depth;

    nvmeq->sq_dma_addr = memory_space->allocate_pages(SQ_SIZE(nvmeq));
    nvmeq->cq_dma_addr = memory_space->allocate_pages(CQ_SIZE(nvmeq));

    spdlog::info("Allocate NVMe queue {} sq_addr={:#x} cq_addr={:#x} depth={}",
                 qid, nvmeq->sq_dma_addr, nvmeq->cq_dma_addr, depth);
}

void NVMeDriver::init_queue(unsigned qid)
{
    auto& nvmeq = queues[qid];

    nvmeq->sq_tail = 0;
    nvmeq->cq_head = 0;
    memory_space->memset(nvmeq->cq_dma_addr, 0, CQ_SIZE(nvmeq));
}

void NVMeDriver::setup_admin_queue()
{
    allocate_queue(0, AQ_DEPTH);

    auto& nvmeq = queues.front();
    uint32_t aqa = nvmeq->depth - 1;
    aqa |= aqa << 16;

    uint64_t asq = nvmeq->sq_dma_addr;
    uint64_t acq = nvmeq->cq_dma_addr;

    link->write_to_device((uint64_t)NVME_REG_AQA, &aqa, sizeof(aqa));
    link->write_to_device((uint64_t)NVME_REG_ASQ, &asq, sizeof(asq));
    link->write_to_device((uint64_t)NVME_REG_ACQ, &acq, sizeof(acq));

    init_queue(0);
}
