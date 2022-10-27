#ifndef _REPORT_EXPORTER_H_
#define _REPORT_EXPORTER_H_

#include "sim_result.pb.h"

#include "io_thread.h"

#include <string>

struct HostResult {
    std::vector<IOThread::Stats> thread_stats;
};

struct ResultExporter {
    static void export_result(const std::string& filename,
                              const HostResult& host_result,
                              const mcmq::SimResult& sim_result);
};

#endif
