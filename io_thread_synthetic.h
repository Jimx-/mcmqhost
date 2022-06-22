#ifndef _IO_THREAD_SYNTHETIC_H_
#define _IO_THREAD_SYNTHETIC_H_

#include "io_thread.h"

#include <random>

class IOThreadSynthetic : public IOThread {
public:
    IOThreadSynthetic(NVMeDriver* driver, MemorySpace* memory_space,
                      int thread_id, unsigned int nsid,
                      unsigned int queue_depth, unsigned int sector_size,
                      size_t max_lsa, unsigned int seed, size_t request_count,
                      double read_ratio,
                      RequestSizeDistribution request_size_distribution,
                      int request_size_mean, int request_size_variance,
                      AddressDistribution addr_distribution,
                      double zipfian_alpha, unsigned int addr_alignment,
                      unsigned int average_enqueued_requests);

private:
    unsigned int nsid;
    unsigned int sector_size;
    size_t max_lsa;
    double read_ratio;
    std::default_random_engine generator;

    RequestSizeDistribution request_size_distribution;
    int request_size_mean;
    int request_size_variance;

    AddressDistribution addr_distribution;
    double zipfian_alpha;
    unsigned int addr_alignment;

    unsigned int average_enqueued_requests;

    void generate_request(bool& do_write, loff_t& pos, size_t& size);

    virtual void run_impl();

    virtual void on_request_completed();
};

#endif
