#include "config_reader.h"
#include "io_thread_synthetic.h"
#include "memory_space.h"
#include "nvme_driver.h"
#include "pcie_link.h"
#include "result_exporter.h"

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <thread>

using cxxopts::OptionException;

cxxopts::ParseResult parse_arguments(int argc, char* argv[])
{
    try {
        cxxopts::Options options(argv[0], " - Host frontend for MCMQ");

        // clang-format off
        options.add_options()
            ("m,memory", "Path to the shared memory file",
            cxxopts::value<std::string>()->default_value("/dev/shm/ivshmem"))
            ("c,config", "Path to the SSD config file",
            cxxopts::value<std::string>()->default_value("ssdconfig.yaml"))
            ("w,workload", "Path to the workload file",
            cxxopts::value<std::string>()->default_value("workload.yaml"))
            ("r,result", "Path to the result file",
            cxxopts::value<std::string>()->default_value("result.json"))
            ("h,help", "Print help");
        // clang-format on

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cerr << options.help({""}) << std::endl;
            exit(EXIT_SUCCESS);
        }

        return result;
    } catch (const OptionException& e) {
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char* argv[])
{
    spdlog::cfg::load_env_levels();

    auto args = parse_arguments(argc, argv);

    std::string shared_memory;
    std::string config_file, workload_file, result_file;
    try {
        shared_memory = args["memory"].as<std::string>();
        config_file = args["config"].as<std::string>();
        workload_file = args["workload"].as<std::string>();
        result_file = args["result"].as<std::string>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    HostConfig host_config;
    mcmq::SsdConfig ssd_config;
    if (!ConfigReader::load_ssd_config(config_file, ssd_config)) {
        spdlog::error("Failed to read SSD config");
        exit(EXIT_FAILURE);
    }

    if (!ConfigReader::load_host_config(workload_file, ssd_config,
                                        host_config)) {
        spdlog::error("Failed to read workload config");
        exit(EXIT_FAILURE);
    }

    MemorySpace memory_space(shared_memory);

    PCIeLink link;
    if (!link.init()) {
        spdlog::error("Failed to initialize PCIe link");
        return EXIT_FAILURE;
    }

    link.start();

    NVMeDriver driver(host_config.flows.size(), host_config.io_queue_depth,
                      &link, &memory_space);
    driver.start(ssd_config);

    int thread_id = 1;
    std::vector<std::unique_ptr<IOThread>> io_threads;
    for (auto&& flow : host_config.flows) {
        auto it = host_config.namespaces.find(flow.nsid);
        if (it == host_config.namespaces.end()) {
            spdlog::error("Unknown namespace {} for flow {}", flow.nsid,
                          thread_id);
            return EXIT_FAILURE;
        }

        const auto& ns = it->second;

        io_threads.emplace_back(IOThread::create_thread(
            &driver, thread_id, host_config.io_queue_depth,
            host_config.sector_size, ns.capacity_sects, flow));

        thread_id++;
    }

    for (auto&& thread : io_threads)
        thread->run();

    for (auto&& thread : io_threads)
        thread->join();

    HostResult host_result;
    for (auto&& thread : io_threads)
        host_result.thread_stats.push_back(thread->get_stats());

    mcmq::SimResult sim_result;
    driver.report(sim_result);

    ResultExporter::export_result(result_file, host_result, sim_result);

    link.stop();

    return 0;
}
