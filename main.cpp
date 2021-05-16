#include "memory_space.h"
#include "nvme_driver.h"
#include "pcie_link.h"

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <thread>

using cxxopts::OptionException;

cxxopts::ParseResult parse_arguments(int argc, char* argv[])
{
    try {
        cxxopts::Options options(argv[0], " - Host frontend for MCMQ");

        options.add_options()("m,memory", "Path to the shared memory file",
                              cxxopts::value<std::string>()->default_value(
                                  "/dev/shm/ivshmem"))("h,help", "Print help");

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
    try {
        shared_memory = args["memory"].as<std::string>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    MemorySpace memory_space(shared_memory);

    PCIeLink link;
    if (!link.init()) {
        spdlog::error("Failed to initialize PCIe link");
        return EXIT_FAILURE;
    }

    std::thread io_thread([&link]() { link.recv_thread(); });
    io_thread.detach();

    NVMeDriver driver(8, &link, &memory_space);

    link.stop();

    return 0;
}
