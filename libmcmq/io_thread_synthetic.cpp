#include "libmcmq/io_thread_synthetic.h"
#include "libmcmq/zipf_distribution.h"

#include "spdlog/spdlog.h"

IOThreadSynthetic::IOThreadSynthetic(
    NVMeDriver* driver, MemorySpace* memory_space, int thread_id,
    unsigned int nsid, unsigned int queue_depth, unsigned int sector_size,
    size_t max_lsa, unsigned int seed, size_t request_count, double read_ratio,
    RequestSizeDistribution request_size_distribution, int request_size_mean,
    int request_size_variance, AddressDistribution addr_distribution,
    double zipfian_alpha, unsigned int addr_alignment,
    unsigned int average_enqueued_requests)
    : IOThread(driver, memory_space, thread_id, queue_depth, request_count),
      nsid(nsid), sector_size(sector_size), max_lsa(max_lsa), generator(seed),
      read_ratio(read_ratio),
      request_size_distribution(request_size_distribution),
      request_size_mean(request_size_mean),
      request_size_variance(request_size_variance),
      addr_distribution(addr_distribution), zipfian_alpha(zipfian_alpha),
      addr_alignment(addr_alignment),
      average_enqueued_requests(average_enqueued_requests)
{}

void IOThreadSynthetic::generate_request(bool& do_write, loff_t& pos,
                                         size_t& size)
{
    unsigned long pos_sector;
    int size_sectors;
    std::bernoulli_distribution request_type_dist(read_ratio);

    do_write = !request_type_dist(generator);

    switch (request_size_distribution) {
    case RequestSizeDistribution::CONSTANT:
        size_sectors = request_size_mean;
        break;
    case RequestSizeDistribution::NORMAL: {
        std::normal_distribution dist((float)request_size_mean,
                                      (float)request_size_variance);

        size_sectors = std::round(dist(generator));
        if (size_sectors <= 0) size_sectors = 1;
        break;
    }
    }

    switch (addr_distribution) {
    case AddressDistribution::UNIFORM: {
        std::uniform_int_distribution dist(0UL, max_lsa - 1);
        pos_sector = dist(generator);
        break;
    }
    case AddressDistribution::ZIPFIAN: {
        zipf_distribution dist(max_lsa, zipfian_alpha);
        pos_sector = dist(generator) - 1;
        break;
    }
    }

    if (addr_alignment) {
        pos_sector -= pos_sector % addr_alignment;
    }

    pos = (loff_t)pos_sector * sector_size;
    size = (size_t)size_sectors * sector_size;
}

void IOThreadSynthetic::on_request_completed()
{
    if (nr_submitted_requests < request_count) {
        bool do_write;
        loff_t pos;
        size_t size;

        generate_request(do_write, pos, size);
        submit_io_request(do_write, nsid, pos, size);
    }
}

void IOThreadSynthetic::run_impl()
{
    for (int i = 0; i < average_enqueued_requests; i++) {
        bool do_write;
        loff_t pos;
        size_t size;

        generate_request(do_write, pos, size);
        submit_io_request(do_write, nsid, pos, size);
    }

    while (nr_completed_requests < request_count) {
        wait_for_completed_requests();
    }
}
