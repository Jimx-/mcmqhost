#include "libmcmq/io_thread.h"
#include "libmcmq/io_thread_synthetic.h"

#include "spdlog/spdlog.h"

IOThread::IOThread(NVMeDriver* driver, MemorySpace* memory_space, int thread_id,
                   unsigned int queue_depth, size_t request_count)
    : driver(driver), memory_space(memory_space), thread_id(thread_id),
      queue_depth(queue_depth), request_count(request_count),
      nr_submitted_requests(0), nr_completed_requests(0), inflight_requests(0),
      nr_completed_read_requests(0), nr_completed_write_requests(0),
      transferred_bytes_total(0), transferred_bytes_read(0),
      transferred_bytes_write(0)
{
    stats.thread_id = thread_id;
}

std::unique_ptr<IOThread>
IOThread::create_thread(NVMeDriver* driver, MemorySpace* memory_space,
                        int thread_id, unsigned int queue_depth,
                        size_t sector_size, size_t max_lsa,
                        const FlowDefinition& def)
{
    switch (def.type) {
    case FlowType::SYNTHETIC:
        return std::make_unique<IOThreadSynthetic>(
            driver, memory_space, thread_id, def.nsid, queue_depth, sector_size,
            max_lsa, def.synthetic.seed, def.synthetic.request_count,
            def.synthetic.read_ratio, def.synthetic.request_size_distribution,
            def.synthetic.request_size_mean,
            def.synthetic.request_size_variance,
            def.synthetic.address_distribution, def.synthetic.zipfian_alpha,
            def.synthetic.address_alignment,
            def.synthetic.average_enqueued_requests);
    default:
        return nullptr;
    }
}

void IOThread::run()
{
    thread = std::thread([this]() {
        driver->set_thread_id(thread_id);

        auto start = std::chrono::system_clock::now();
        this->run_impl();
        auto end = std::chrono::system_clock::now();

        std::chrono::duration<double> delta = end - start;

        stats.request_count = nr_completed_requests;
        stats.read_request_count = nr_completed_read_requests;
        stats.write_request_count = nr_completed_write_requests;

        stats.iops_total = nr_completed_requests / delta.count();
        stats.iops_read = nr_completed_read_requests / delta.count();
        stats.iops_write = nr_completed_write_requests / delta.count();

        stats.bandwidth_total = transferred_bytes_total / delta.count();
        stats.bandwidth_read = transferred_bytes_read / delta.count();
        stats.bandwidth_write = transferred_bytes_write / delta.count();

        spdlog::info(
            "Thread {} stats: Request count (total/read/write): {}/{}/{}",
            thread_id, nr_completed_requests, nr_completed_read_requests,
            nr_completed_write_requests);
        spdlog::info(
            "Thread {} stats: IOPS (total/read/write): {:.2f}/{:.2f}/{:.2f}",
            thread_id, stats.iops_total, stats.iops_read, stats.iops_write);
        spdlog::info("Thread {} stats: Bandwidth (total/read/write): "
                     "{:.2f}/{:.2f}/{:.2f}",
                     thread_id, stats.bandwidth_total, stats.bandwidth_read,
                     stats.bandwidth_write);
    });
}

void IOThread::join()
{
    if (thread.joinable()) thread.join();
}

void IOThread::submit_io_request(bool do_write, unsigned int nsid, loff_t pos,
                                 size_t size)
{
    auto* req = new IORequest();

    req->do_write = do_write;
    req->nsid = nsid;
    req->pos = pos;
    req->buf = memory_space->allocate_pages(size);
    req->size = size;

    nr_submitted_requests++;

    req->arrival_time = std::chrono::system_clock::now();

    if (inflight_requests >= queue_depth) {
        waiting_requests.push(req);
        return;
    }

    submit_to_device(req);
}

void IOThread::submit_to_device(IORequest* req)
{
    bool do_write = req->do_write;
    unsigned int nsid = req->nsid;
    loff_t pos = req->pos;
    size_t size = req->size;

    auto callback = [this, req](NVMeDriver::NVMeStatus status,
                                const NVMeDriver::NVMeResult& res) {
        this->notify_request_completion(req);
    };

    inflight_requests++;

    spdlog::trace("Submitting {} request to device nsid={} pos={} size={}",
                  do_write ? "write" : "read", nsid, pos, size);

    req->enqueued_time = std::chrono::system_clock::now();

    if (do_write) {
        driver->write_async(nsid, pos, req->buf, size, std::move(callback));
    } else {
        driver->read_async(nsid, pos, req->buf, size, std::move(callback));
    }
}

void IOThread::notify_request_completion(IORequest* req)
{
    std::lock_guard<std::mutex> lock(completion_mutex);
    auto now = std::chrono::system_clock::now();

    completed_requests.push(req);
    completion_cv.notify_all();

    auto device_response_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - req->enqueued_time)
            .count();
    auto e2e_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              now - req->arrival_time)
                              .count();

    hdr_record_value(stats.device_response_time_hist.get(),
                     device_response_time_us);
    hdr_record_value(stats.e2e_latency_hist.get(), e2e_latency_us);
}

void IOThread::wait_for_completed_requests()
{
    std::vector<IORequest*> reqs;

    {
        std::unique_lock<std::mutex> lock(completion_mutex);

        while (completed_requests.empty())
            completion_cv.wait(lock);

        while (!completed_requests.empty()) {
            auto req = completed_requests.front();
            completed_requests.pop();
            reqs.push_back(req);
        }
    }

    for (auto&& req : reqs)
        process_completed_request(req);
}

void IOThread::process_completed_request(IORequest* req)
{
    static const size_t LOG_STEP = 10000;

    inflight_requests--;
    nr_completed_requests++;
    transferred_bytes_total += req->size;

    memory_space->free(req->buf, req->size);

    if (nr_completed_requests % LOG_STEP == 0)
        spdlog::info("Thread {} {}/{} requests completed", thread_id,
                     nr_completed_requests, request_count);

    if (req->do_write) {
        nr_completed_write_requests++;
        transferred_bytes_write += req->size;
    } else {
        nr_completed_read_requests++;
        transferred_bytes_read += req->size;
    }

    while (!waiting_requests.empty() && inflight_requests < queue_depth) {
        auto req = waiting_requests.front();
        waiting_requests.pop();
        submit_to_device(req);
    }

    on_request_completed();

    delete req;
}
