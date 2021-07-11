#include "result_exporter.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>

static const int TICKS_PER_HALF_DISTANCE = 5;

using json = nlohmann::json;

static json export_histogram_entry(const mcmq::HistogramEntry& entry)
{
    json obj;

    obj["percentile"] = entry.percentile();
    obj["value"] = entry.value();
    obj["total_count"] = entry.total_count();

    return obj;
}

static json export_queue_stats(const mcmq::HostQueueStats& queue_stats)
{
    json obj;

    obj["queue_id"] = queue_stats.queue_id();
    obj["read_request_turnaround_time_mean"] =
        queue_stats.read_request_turnaround_time_mean();
    obj["read_request_turnaround_time_stddev"] =
        queue_stats.read_request_turnaround_time_stddev();
    obj["max_read_request_turnaround_time"] =
        queue_stats.max_read_request_turnaround_time();

    auto read_request_latency_histogram = json::array();

    for (int i = 0;
         i < queue_stats.read_request_turnaround_time_histogram_size(); i++) {
        read_request_latency_histogram.push_back(export_histogram_entry(
            queue_stats.read_request_turnaround_time_histogram(i)));
    }
    obj["read_request_latency_histogram"] = read_request_latency_histogram;

    obj["write_request_turnaround_time_mean"] =
        queue_stats.write_request_turnaround_time_mean();
    obj["write_request_turnaround_time_stddev"] =
        queue_stats.write_request_turnaround_time_stddev();
    obj["max_write_request_turnaround_time"] =
        queue_stats.max_write_request_turnaround_time();

    auto write_request_latency_histogram = json::array();

    for (int i = 0;
         i < queue_stats.write_request_turnaround_time_histogram_size(); i++) {
        write_request_latency_histogram.push_back(export_histogram_entry(
            queue_stats.write_request_turnaround_time_histogram(i)));
    }
    obj["write_request_latency_histogram"] = write_request_latency_histogram;

    obj["read_request_count"] = queue_stats.read_request_count();
    obj["write_request_count"] = queue_stats.write_request_count();

    return obj;
}

static void export_sim_result(json& root, const mcmq::SimResult& sim_result)
{
    json host_queue_stats = json::array();

    for (int i = 0; i < sim_result.host_queue_stats_size(); i++) {
        host_queue_stats.push_back(
            export_queue_stats(sim_result.host_queue_stats(i)));
    }

    root["host_queue_stats"] = host_queue_stats;
}

static json export_histogram(const hdr_histogram* hist)
{
    json root = json::array();

    struct hdr_iter iter;
    struct hdr_iter_percentiles* percentiles;

    hdr_iter_percentile_init(&iter, hist, 5);

    percentiles = &iter.specifics.percentiles;
    while (hdr_iter_next(&iter)) {
        double value = iter.highest_equivalent_value;
        double percentile = percentiles->percentile;
        int64_t total_count = iter.cumulative_count;

        json entry = json::object();

        entry["value"] = value;
        entry["percentile"] = percentile;
        entry["total_count"] = total_count;

        root.push_back(entry);
    }

    return root;
}

static json export_thread_stats(const IOThread::Stats& stats)
{
    json root = json::object();

    root["id"] = stats.thread_id;

    root["total_requests"] = stats.request_count;
    root["read_requests"] = stats.read_request_count;
    root["write_requests"] = stats.write_request_count;

    root["iops_total"] = stats.iops_total;
    root["iops_read"] = stats.iops_read;
    root["iops_write"] = stats.iops_write;

    root["bandwidth_total"] = stats.bandwidth_total;
    root["bandwidth_read"] = stats.bandwidth_read;
    root["bandwidth_write"] = stats.bandwidth_write;

    root["device_response_time_histogram"] =
        export_histogram(stats.device_response_time_hist.get());
    root["end_to_end_request_latency_histogram"] =
        export_histogram(stats.e2e_latency_hist.get());

    return root;
}

static void export_host_result(json& root, const HostResult& host_result)
{
    json thread_stats = json::array();

    for (auto&& stats : host_result.thread_stats)
        thread_stats.push_back(export_thread_stats(stats));

    root["host_thread_stats"] = thread_stats;
}

void ResultExporter::export_result(const std::string& filename,
                                   const HostResult& host_result,
                                   const mcmq::SimResult& sim_result)
{
    json root;

    export_host_result(root, host_result);

    export_sim_result(root, sim_result);

    std::cout << std::setw(4) << root << std::endl;

    std::ofstream os(filename);
    os << root;
}
