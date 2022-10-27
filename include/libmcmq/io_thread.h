#ifndef _IO_THREAD_H_
#define _IO_THREAD_H_

#include "config_reader.h"
#include "histogram.h"
#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

class IOThread {
public:
    struct Stats {
        int thread_id;

        size_t request_count;
        size_t read_request_count;
        size_t write_request_count;

        double iops_total;
        double iops_read;
        double iops_write;

        double bandwidth_total;
        double bandwidth_read;
        double bandwidth_write;

        Histogram device_response_time_hist;
        Histogram e2e_latency_hist;

        Stats()
            : device_response_time_hist(10000000), e2e_latency_hist(10000000)
        {}
    };

    virtual ~IOThread() {}

    static std::unique_ptr<IOThread>
    create_thread(NVMeDriver* driver, MemorySpace* memory_space, int thread_id,
                  unsigned int queue_depth, size_t sector_size, size_t max_lsa,
                  const FlowDefinition& def);

    const Stats& get_stats() const { return stats; }

    void run();

    void join();

protected:
    IOThread(NVMeDriver* driver, MemorySpace* memory_space, int thread_id,
             unsigned int queue_depth, size_t request_count);

    virtual void run_impl() = 0;

    void submit_io_request(bool do_write, unsigned int nsid, loff_t pos,
                           size_t size);

    void wait_for_completed_requests();

    virtual void on_request_completed() = 0;

    size_t request_count;

    size_t nr_submitted_requests;

    size_t nr_completed_requests;
    size_t nr_completed_read_requests;
    size_t nr_completed_write_requests;

    size_t transferred_bytes_total;
    size_t transferred_bytes_read;
    size_t transferred_bytes_write;

    Stats stats;

private:
    struct IORequest {
        bool do_write;
        unsigned int nsid;
        loff_t pos;
        MemorySpace::Address buf;
        size_t size;

        std::chrono::time_point<std::chrono::system_clock> arrival_time;
        std::chrono::time_point<std::chrono::system_clock> enqueued_time;
    };

    NVMeDriver* driver;
    MemorySpace* memory_space;
    int thread_id;
    unsigned int queue_depth;
    unsigned int inflight_requests;
    std::queue<IORequest*> waiting_requests;

    std::queue<IORequest*> completed_requests;
    std::mutex completion_mutex;
    std::condition_variable completion_cv;

    std::thread thread;

    void submit_to_device(IORequest* req);

    void notify_request_completion(IORequest* req);

    void process_completed_request(IORequest* req);
};

#endif
