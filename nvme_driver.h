#ifndef _NVME_DRIVER_H_
#define _NVME_DRIVER_H_

#include "memory_space.h"
#include "pcie_link.h"

#include <memory>
#include <mutex>
#include <vector>

enum class NVMeRegister {
    CAP = 0x0000,     /* Controller Capabilities */
    VS = 0x0008,      /* Version */
    INTMS = 0x000c,   /* Interrupt Mask Set */
    INTMC = 0x0010,   /* Interrupt Mask Clear */
    CC = 0x0014,      /* Controller Configuration */
    CSTS = 0x001c,    /* Controller Status */
    NSSR = 0x0020,    /* NVM Subsystem Reset */
    AQA = 0x0024,     /* Admin Queue Attributes */
    ASQ = 0x0028,     /* Admin SQ Base Address */
    ACQ = 0x0030,     /* Admin CQ Base Address */
    CMBLOC = 0x0038,  /* Controller Memory Buffer Location */
    CMBSZ = 0x003c,   /* Controller Memory Buffer Size */
    BPINFO = 0x0040,  /* Boot Partition Information */
    BPRSEL = 0x0044,  /* Boot Partition Read Select */
    BPMBL = 0x0048,   /* Boot Partition Memory Buffer
                       * Location
                       */
    PMRCAP = 0x0e00,  /* Persistent Memory Capabilities */
    PMRCTL = 0x0e04,  /* Persistent Memory Region Control */
    PMRSTS = 0x0e08,  /* Persistent Memory Region Status */
    PMREBS = 0x0e0c,  /* Persistent Memory Region Elasticity
                       * Buffer Size
                       */
    PMRSWTP = 0x0e10, /* Persistent Memory Region Sustained
                       * Write Throughput
                       */
    DBS = 0x1000,     /* SQ 0 Tail Doorbell */
};

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
