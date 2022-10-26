#include "config_reader.h"
#include "io_thread_synthetic.h"
#include "memory_space.h"
#include "nvme_driver.h"
#include "pcie_link_mcmq.h"
#include "pcie_link_vfio.h"
#include "result_exporter.h"

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <thread>

using cxxopts::OptionException;

static const size_t CHUNK_SIZE = 256UL << 20;
static const size_t LOAD_PAGE_SIZE = 0x4000;

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
            ("r,result", "Path to the result file",
            cxxopts::value<std::string>()->default_value("result.json"))
            ("f,file", "Path to the file to be loaded",
            cxxopts::value<std::string>())
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
    std::string config_file, result_file;
    std::string load_file;
    try {
        backend = args["backend"].as<std::string>();
        config_file = args["config"].as<std::string>();
        result_file = args["result"].as<std::string>();
        load_file = args["file"].as<std::string>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    mcmq::SsdConfig ssd_config;
    if (!ConfigReader::load_ssd_config(config_file, ssd_config)) {
        spdlog::error("Failed to read SSD config");
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

        memory_space = std::make_unique<VfioMemorySpace>(
            0x1000, 2 * 1024 * 1024 + CHUNK_SIZE);
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

    NVMeDriver driver(1, 1024, link.get(), memory_space.get(), false);
    driver.start(ssd_config);

    unsigned int ctx = driver.create_context(
        "/home/jimx/projects/storpu/libstorpu/libtest.so");
    spdlog::info("Created context {}", ctx);

    auto load_buf = memory_space->allocate(CHUNK_SIZE, 0x4000);

    std::ifstream ifs(load_file, std::ios::binary);
    if (!ifs.is_open()) {
        spdlog::error("Failed to open load file");
        return EXIT_FAILURE;
    }

    size_t fsize = ifs.tellg();
    ifs.seekg(0, std::ios::end);
    fsize = (size_t)ifs.tellg() - fsize;

    spdlog::info("File size {}", fsize);

    struct {
        unsigned long fd;
        unsigned long host_addr;
        unsigned long flash_addr;
        unsigned long length;
    } lda;

    auto* scratchpad = driver.get_scratchpad();
    auto argbuf = scratchpad->allocate(sizeof(lda));

    driver.set_thread_id(1);

    size_t offset = 0;
    auto page_buf = std::make_unique<char[]>(LOAD_PAGE_SIZE);

    while (offset < fsize) {
        auto chunk = std::min(fsize - offset, CHUNK_SIZE);
        size_t read_len = 0;

        while (read_len < chunk) {
            size_t page_len = std::min(chunk - read_len, LOAD_PAGE_SIZE);

            ifs.seekg(offset + read_len, std::ios::beg);
            ifs.read(page_buf.get(), page_len);

            memory_space->write(load_buf + read_len, page_buf.get(), page_len);
            read_len += page_len;
        }

        lda.fd = 0;
        lda.flash_addr = offset;
        lda.host_addr = load_buf;
        lda.length = chunk;

        scratchpad->write(argbuf, &lda, sizeof(lda));

        unsigned long r = driver.invoke_function(ctx, 0x12d0, argbuf);

        spdlog::info("Invoke {:#x}", r);

        offset += chunk;
    }

    scratchpad->free(argbuf, sizeof(lda));

    driver.shutdown();
    link->stop();

    return 0;
}
