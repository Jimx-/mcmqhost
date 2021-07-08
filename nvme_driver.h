#ifndef _NVME_DRIVER_H_
#define _NVME_DRIVER_H_

#include "memory_space.h"
#include "nvme.h"
#include "pcie_link.h"

#include <memory>
#include <mutex>
#include <vector>

class NVMeDriver {
    friend class AsyncCommand;

public:
    struct DeviceIOError : public virtual std::runtime_error {
        DeviceIOError(const char* msg);
    };

    using NVMeStatus = uint32_t;
    using NVMeResult = nvme_completion::nvme_result;

    class AsyncCommand {
        friend class NVMeDriver;

    public:
        NVMeStatus wait(NVMeResult* resp);

    private:
        std::mutex mutex;
        std::condition_variable cv;
        NVMeDriver* driver;

        uint16_t id;
        NVMeStatus status;
        NVMeResult result;
        bool completed;
    };

    explicit NVMeDriver(unsigned ncpus, PCIeLink* link,
                        MemorySpace* memory_space);

    void start(const mcmq::SsdConfig& config);

    void set_thread_id(unsigned int thread_id);

    void read(unsigned int nsid, loff_t pos, size_t size)
    {
        return submit_rw_command(false, nsid, pos, size);
    }

    void write(unsigned int nsid, loff_t pos, size_t size)
    {
        return submit_rw_command(true, nsid, pos, size);
    }

    void report(mcmq::SimResult& result) { link->report(result); }

private:
    static constexpr unsigned AQ_DEPTH = 32;

    static constexpr unsigned IO_QUEUE_DEPTH = 1024;

    struct NVMeQueue {
        std::mutex mutex;

        MemorySpace::Address sq_dma_addr;
        MemorySpace::Address cq_dma_addr;
        unsigned int depth;
        unsigned char sqe_shift;
        uint16_t qid;
        uint16_t sq_tail;
        uint16_t last_sq_tail;
        uint16_t cq_head;
        uint8_t cq_phase;
        uint32_t q_db;

        inline void update_cq_head()
        {
            uint16_t tmp = cq_head + 1;

            if (tmp == depth) {
                cq_head = 0;
                cq_phase ^= 1;
            } else {
                cq_head = tmp;
            }
        }
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

    unsigned int ncpus;
    PCIeLink* link;
    MemorySpace* memory_space;
    std::vector<std::unique_ptr<NVMeQueue>> queues;
    size_t queue_count, online_queues;
    std::mutex command_mutex;
    std::atomic<uint16_t> command_id_counter;
    std::unordered_map<uint16_t, std::unique_ptr<AsyncCommand>> command_map;
    static thread_local struct NVMeQueue* thread_io_queue;

    uint64_t ctrl_cap;
    uint32_t ctrl_config;
    uint32_t ctrl_page_size;
    int queue_depth;
    int db_stride;

    void reset();

    void allocate_queue(unsigned qid, unsigned depth);
    void init_queue(unsigned qid);

    void disable_controller();
    void enable_controller();
    void wait_ready(bool enabled);

    AsyncCommand* setup_async_command();
    void remove_async_command(uint16_t command_id);

    void write_sq_doorbell(NVMeQueue* nvmeq, bool write_sq);
    void ring_cq_doorbell(NVMeQueue* nvmeq);

    void submit_sq_command(NVMeQueue* nvmeq, struct nvme_command* cmd,
                           bool write_sq);
    NVMeStatus submit_sync_command(NVMeQueue* nvmeq, struct nvme_command* cmd,
                                   union nvme_completion::nvme_result* result);

    NVMeDriver::NVMeStatus nvme_features(uint8_t op, unsigned int fid,
                                         unsigned int dword11,
                                         uint32_t* result);
    NVMeDriver::NVMeStatus set_features(unsigned int fid, unsigned int dword11,
                                        uint32_t* result)
    {
        return nvme_features(nvme_admin_set_features, fid, dword11, result);
    }

    NVMeDriver::NVMeStatus set_queue_count(int& count);

    void setup_admin_queue();
    void setup_io_queues();

    NVMeStatus create_queue(NVMeQueue* nvmeq, unsigned qid);
    NVMeStatus create_cq(NVMeQueue* nvmeq, unsigned qid, unsigned int vector);
    NVMeStatus create_sq(NVMeQueue* nvmeq, unsigned qid);

    bool cqe_pending(NVMeQueue* nvmeq);
    void handle_cqe(NVMeQueue* nvmeq, uint16_t idx);
    void nvme_irq(NVMeQueue* nvmeq);

    void submit_rw_command(bool do_write, unsigned int nsid, loff_t pos,
                           size_t size);
};

#endif
