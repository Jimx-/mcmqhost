#include "config_reader.h"
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

        options.add_options()(
            "m,memory", "Path to the shared memory file",
            cxxopts::value<std::string>()->default_value("/dev/shm/ivshmem"))(
            "c,config", "Path to the SSD config file",
            cxxopts::value<std::string>()->default_value(
                "ssdconfig.yaml"))("r,result", "Path to the result file",
                                   cxxopts::value<std::string>()->default_value(
                                       "result.json"))("h,help", "Print help");

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
    std::string config_file, result_file;
    try {
        shared_memory = args["memory"].as<std::string>();
        config_file = args["config"].as<std::string>();
        result_file = args["result"].as<std::string>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    mcmq::SsdConfig config;
    if (!ConfigReader::load_config(config_file, config)) {
        spdlog::error("Failed to read SSD config");
        exit(EXIT_FAILURE);
    }

    MemorySpace memory_space(shared_memory);

    PCIeLink link;
    if (!link.init()) {
        spdlog::error("Failed to initialize PCIe link");
        return EXIT_FAILURE;
    }

    link.start();

    NVMeDriver driver(2, &link, &memory_space);
    driver.start(config);
    driver.set_thread_id(0);

    driver.write(1, 16384, 8192);
    // for (int i = 0; i < 511; i++)
    //     driver.write(1, 8192, 8192);
    driver.write(2, 16384, 8192);

    mcmq::SimResult result;
    driver.report(result);

    ResultExporter::export_result(result_file, result);

    link.stop();

    return 0;
}
