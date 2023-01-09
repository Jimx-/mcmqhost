#include "libmcmq/config_reader.h"
#include "libmcmq/io_thread_synthetic.h"
#include "libmcmq/result_exporter.h"
#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_mcmq.h"
#include "libunvme/pcie_link_vfio.h"

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
            ("b,backend", "Backend type", cxxopts::value<std::string>()->default_value("mcmq"))
            ("m,memory", "Path to the shared memory file",
            cxxopts::value<std::string>()->default_value("/dev/shm/ivshmem"))
            ("c,config", "Path to the SSD config file",
            cxxopts::value<std::string>()->default_value("ssdconfig.yaml"))
            ("w,workload", "Path to the workload file",
            cxxopts::value<std::string>()->default_value("workload.yaml"))
            ("r,result", "Path to the result file",
            cxxopts::value<std::string>()->default_value("result.json"))
            ("g,group", "VFIO group",
            cxxopts::value<std::string>())
            ("d,device", "PCI device ID",
            cxxopts::value<std::string>())
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

    std::string backend;
    std::string config_file, workload_file, result_file;
    try {
        backend = args["backend"].as<std::string>();
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

    std::unique_ptr<MemorySpace> memory_space;
    std::unique_ptr<PCIeLink> link;

    if (backend == "mcmq") {
        std::string shared_memory;

        try {
            shared_memory = args["memory"].as<std::string>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }

        memory_space = std::make_unique<SharedMemorySpace>(shared_memory);
        link = std::make_unique<PCIeLinkMcmq>();
    } else if (backend == "vfio") {
        std::string group, device_id;

        try {
            group = args["group"].as<std::string>();
            device_id = args["device"].as<std::string>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }

        memory_space =
            std::make_unique<VfioMemorySpace>(0x1000, 2 * 1024 * 1024);
        link = std::make_unique<PCIeLinkVfio>(group, device_id);
    } else {
        spdlog::error("Unknown backend type: {}", backend);
        return EXIT_FAILURE;
    }

    if (!link->init()) {
        spdlog::error("Failed to initialize PCIe link");
        return EXIT_FAILURE;
    }

    link->map_dma(*memory_space);
    link->start();

    NVMeDriver driver(host_config.flows.size(), host_config.io_queue_depth,
                      link.get(), memory_space.get(), false);
    link->send_config(ssd_config);
    driver.start();

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
            &driver, memory_space.get(), thread_id, host_config.io_queue_depth,
            host_config.sector_size, ns.capacity_sects, flow));

        thread_id++;
    }

    for (auto&& thread : io_threads)
        thread->run();

    for (auto&& thread : io_threads)
        thread->join();

    driver.set_thread_id(1);
    driver.flush(1);

    HostResult host_result;
    for (auto&& thread : io_threads)
        host_result.thread_stats.push_back(thread->get_stats());

    mcmq::SimResult sim_result;
    driver.report(sim_result);

    ResultExporter::export_result(result_file, host_result, sim_result);

    driver.shutdown();
    link->stop();

    return 0;
}
