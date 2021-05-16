#ifndef _NVME_DRIVER_H_
#define _NVME_DRIVER_H_

#include "memory_space.h"
#include "pcie_link.h"

#include <memory>
#include <mutex>
#include <vector>

class NVMeDriver {
public:
    explicit NVMeDriver(unsigned ncpus, PCIeLink* link,
                        MemorySpace* memory_space);

private:
    static constexpr unsigned AQ_DEPTH = 32;

    static constexpr unsigned char ADM_SQES = 6;
    static constexpr unsigned char NVM_IOSQES = 6;
    static constexpr unsigned char NVM_IOCQES = 4;

    struct NVMeQueue {
        std::mutex mutex;

        MemorySpace::Address sq_dma_addr;
        MemorySpace::Address cq_dma_addr;
        unsigned int depth;
        unsigned char sqe_shift;
        uint16_t sq_tail;
        uint16_t cq_head;
    };

    struct NVMeCompletion {
        /*
         * Used by Admin and Fabrics commands to return data:
         */
        union NVMeResult {
            uint16_t u16;
            uint32_t u32;
            uint64_t u64;
        } result;
        uint16_t sq_head;    /* how much of this queue may be reclaimed */
        uint16_t sq_id;      /* submission queue that generated this entry */
        uint16_t command_id; /* of the command which completed */
        uint16_t status;     /* did the command fail, and if so, why? */
    };

    PCIeLink* link;
    MemorySpace* memory_space;
    std::vector<std::unique_ptr<NVMeQueue>> queues;

    void allocate_queue(unsigned qid, unsigned depth);
    void init_queue(unsigned qid);

    void setup_admin_queue();
};

#endif
