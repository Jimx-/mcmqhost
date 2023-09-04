#ifndef _NVME_DRIVER_H_
#define _NVME_DRIVER_H_

#include "memory_space.h"
#include "nvme.h"
#include "pcie_link.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#ifdef ENABLE_INTERPROCESS
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/smart_ptr/unique_ptr.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#endif

class NVMeDriver {
    friend class AsyncCommand;

public:
    struct DeviceIOError : public virtual std::runtime_error {
        DeviceIOError(const char* msg);
    };

    struct NamespaceUnavailableError : public virtual std::runtime_error {
        NamespaceUnavailableError();
    };

    using NVMeStatus = uint32_t;
    using NVMeResult = nvme_completion::nvme_result;

    using AsyncCommandCallback =
        std::function<void(NVMeStatus, const NVMeResult&)>;

    class AsyncCommand {
        friend class NVMeDriver;

    public:
        ~AsyncCommand()
        {
            for (auto&& prp : prp_lists)
                space->free(prp, 0x1000);
        }

        NVMeStatus wait(NVMeResult* resp);

    private:
#ifdef ENABLE_INTERPROCESS
        using MutexType = boost::interprocess::interprocess_mutex;
        using CondType = boost::interprocess::interprocess_condition;
#else
        using MutexType = std::mutex;
        using CondType = std::condition_variable;
#endif
        MutexType mutex;
        CondType cv;
        NVMeDriver* driver;
        MemorySpace* space;

        uint16_t id;
        NVMeStatus status;
        NVMeResult result;
        bool completed;
        AsyncCommandCallback callback;
        std::vector<MemorySpace::Address> prp_lists;
    };

    explicit NVMeDriver(unsigned ncpus, unsigned int io_queue_depth,
                        PCIeLink* link, MemorySpace* memory_space,
                        bool use_dbbuf = false);
    ~NVMeDriver();

    void start();

    void set_thread_id(unsigned int thread_id);

    void read(unsigned int nsid, loff_t pos, MemorySpace::Address buf,
              size_t size);

    void write(unsigned int nsid, loff_t pos, MemorySpace::Address buf,
               size_t size);

    void flush(unsigned int nsid);

    void read_async(unsigned int nsid, loff_t pos, MemorySpace::Address buf,
                    size_t size, AsyncCommandCallback&& callback)
    {
        (void)submit_rw_command(false, nsid, pos, buf, size,
                                std::move(callback));
    }

    void write_async(unsigned int nsid, loff_t pos, MemorySpace::Address buf,
                     size_t size, AsyncCommandCallback&& callback)
    {
        (void)submit_rw_command(true, nsid, pos, buf, size,
                                std::move(callback));
    }

    void flush_async(unsigned int nsid, AsyncCommandCallback&& callback)
    {
        (void)submit_flush_command(nsid, std::move(callback));
    }

    void report(mcmq::SimResult& result) { link->report(result); }

    void shutdown();

    MemorySpace* get_dma_space() { return memory_space; }
    MemorySpace* get_scratchpad() { return bar4_mem; }

    uint32_t create_context(const std::filesystem::path& filename);
    void delete_context(unsigned int cid);

    unsigned long invoke_function(unsigned int cid, MemorySpace::Address entry,
                                  unsigned long arg);

    void invoke_function_async(unsigned int cid, MemorySpace::Address entry,
                               unsigned long arg,
                               AsyncCommandCallback&& callback)
    {
        (void)submit_invoke_command(cid, entry, arg, std::move(callback));
    }

    unsigned int create_namespace(size_t size_bytes);
    void delete_namespace(unsigned int nsid);
    void attach_namespace(unsigned int nsid);
    void detach_namespace(unsigned int nsid);

private:
    static constexpr unsigned AQ_DEPTH = 32;

#ifdef ENABLE_INTERPROCESS
    using MutexType = boost::interprocess::interprocess_mutex;
    using CondType = boost::interprocess::interprocess_condition;
#else
    using MutexType = std::mutex;
    using CondType = std::condition_variable;
#endif

    struct NVMeQueue {
        MutexType mutex;

        MemorySpace::Address sq_dma_addr;
        MemorySpace::Address cq_dma_addr;
        MemorySpace::Address dbbuf_sq_db;
        MemorySpace::Address dbbuf_cq_db;
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

#ifdef ENABLE_INTERPROCESS
    template <typename T>
    using Allocator = boost::interprocess::allocator<
        T, boost::interprocess::managed_shared_memory::segment_manager>;

    template <typename T>
    using UniquePtrType = typename boost::interprocess::managed_unique_ptr<
        T, boost::interprocess::managed_shared_memory>::type;

    template <typename T, typename... Args>
    UniquePtrType<T> make_unique(Args&&... args)
    {
        return boost::interprocess::make_managed_unique_ptr(
            segment.construct<T>(boost::interprocess::anonymous_instance)(
                std::forward<Args>(args)...),
            segment);
    }

    using PAsyncCommand = boost::interprocess::offset_ptr<AsyncCommand>;
    using PNVMeQueue = boost::interprocess::offset_ptr<NVMeQueue>;
#else
    template <typename T> using Allocator = std::allocator<T>;

    template <typename T> using UniquePtrType = std::unique_ptr<T>;

    template <typename T, typename... Args>
    std::unique_ptr<T> make_unique(Args&&... args)
    {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }

    using PAsyncCommand = AsyncCommand*;
    using PNVMeQueue = NVMeQueue*;
#endif

#ifdef ENABLE_INTERPROCESS
    boost::interprocess::managed_shared_memory segment;
#endif

    unsigned int ncpus;
    PCIeLink* link;
    MemorySpace* memory_space;

    std::vector<UniquePtrType<NVMeQueue>>* queues;

    size_t queue_count, online_queues;
    MutexType* command_mutex;
    std::atomic<uint16_t>* command_id_counter;

    using CommandMapType = std::unordered_map<
        uint16_t, UniquePtrType<AsyncCommand>, std::hash<uint16_t>,
        std::equal_to<uint16_t>,
        Allocator<std::pair<uint16_t, UniquePtrType<AsyncCommand>>>>;
    CommandMapType* command_map;

    static thread_local PNVMeQueue thread_io_queue;

    uint64_t ctrl_cap;
    uint32_t ctrl_config;
    uint32_t ctrl_page_size;
    int queue_depth;
    int io_queue_depth;
    int db_stride;
    MemorySpace::Address dbbuf_dbs;

    MemorySpace* bar4_mem;

    void reset();

    void check_status(int status);

    void allocate_queue(unsigned qid, unsigned depth);
    void init_queue(unsigned qid);

    void disable_controller();
    void enable_controller();
    void wait_ready(bool enabled);

    PAsyncCommand setup_async_command(AsyncCommandCallback&& callback);
    void remove_async_command(uint16_t command_id);

    void setup_buffer(PAsyncCommand acmd, struct nvme_command* cmd,
                      MemorySpace::Address buf, size_t buflen);

    void write_sq_doorbell(PNVMeQueue nvmeq, bool write_sq);
    void ring_cq_doorbell(PNVMeQueue nvmeq);

    void submit_sq_command(PNVMeQueue nvmeq, struct nvme_command* cmd,
                           bool write_sq);
    NVMeStatus submit_sync_command(PNVMeQueue nvmeq, struct nvme_command* cmd,
                                   MemorySpace::Address buf, size_t buflen,
                                   union nvme_completion::nvme_result* result);
    PAsyncCommand submit_async_command(PNVMeQueue nvmeq,
                                       struct nvme_command* cmd,
                                       MemorySpace::Address buf, size_t buflen,
                                       AsyncCommandCallback&& callback);

    void dbbuf_config();

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

    NVMeStatus create_queue(PNVMeQueue nvmeq, unsigned qid);
    NVMeStatus create_cq(PNVMeQueue nvmeq, unsigned qid, unsigned int vector);
    NVMeStatus create_sq(PNVMeQueue nvmeq, unsigned qid);

    NVMeStatus identify_controller();

    bool cqe_pending(PNVMeQueue nvmeq);
    void handle_cqe(PNVMeQueue nvmeq, uint16_t idx);
    void nvme_irq(PNVMeQueue nvmeq);

    PAsyncCommand submit_rw_command(bool do_write, unsigned int nsid,
                                    loff_t pos, MemorySpace::Address buf,
                                    size_t size,
                                    AsyncCommandCallback&& callback);

    PAsyncCommand submit_flush_command(unsigned int nsid,
                                       AsyncCommandCallback&& callback);

    PAsyncCommand submit_invoke_command(unsigned int cid,
                                        MemorySpace::Address entry,
                                        unsigned long arg,
                                        AsyncCommandCallback&& callback);

    NVMeStatus submit_ns_mgmt(unsigned int nsid, int sel,
                              MemorySpace::Address buffer, size_t size,
                              union nvme_completion::nvme_result* res);

    NVMeStatus submit_ns_attach(unsigned int nsid, int sel,
                                MemorySpace::Address buffer, size_t size);
};

#endif
