#include "config_reader.h"

#include <yaml-cpp/yaml.h>

bool ConfigReader::load_config(const std::string& filename,
                               mcmq::SsdConfig& config)
{
    YAML::Node root;

    try {
        root = YAML::LoadFile(filename);
    } catch (YAML::BadFile&) {
        return false;
    }

    auto flash_config = config.mutable_flash_config();

    config.set_seed(root["seed"].as<uint32_t>(123));

    auto cache_mode = (root["cache_mode"].as<std::string>("no"));
    if (cache_mode == "no")
        config.set_cache_mode(mcmq::CM_NO_CACHE);
    else if (cache_mode == "write")
        config.set_cache_mode(mcmq::CM_WRITE_CACHE);

    config.set_mapping_table_capacity(
        root["mapping_table_capacity"].as<uint32_t>(2 << 20));
    config.set_data_cache_capacity(
        root["data_cache_capacity"].as<uint32_t>(512 << 20));

    YAML::Node gc_node = root["gc_threshold"];
    config.set_gc_threshold(gc_node["normal"].as<uint32_t>(20));
    config.set_gc_hard_threshold(gc_node["hard"].as<uint32_t>(200));

    auto block_selection_policy =
        (root["block_selection_policy"].as<std::string>("greedy"));
    if (block_selection_policy == "greedy")
        config.set_block_selection_policy(mcmq::BSP_GREEDY);

    config.set_channel_count(root["channel_count"].as<uint32_t>(8));
    YAML::Node channel_node = root["channel"];
    config.set_nr_chips_per_channel(
        channel_node["chips_per_channel"].as<uint32_t>(4));
    config.set_channel_transfer_rate(
        channel_node["transfer_rate"].as<uint32_t>(300));
    config.set_channel_width(channel_node["width"].as<uint32_t>(1));

    YAML::Node flash_node = root["flash"];

    auto technology = (flash_node["technology"].as<std::string>("mlc"));
    if (technology == "slc")
        flash_config->set_technology(mcmq::FT_SLC);
    else if (technology == "mlc")
        flash_config->set_technology(mcmq::FT_MLC);
    else if (technology == "tlc")
        flash_config->set_technology(mcmq::FT_TLC);

    YAML::Node read_latency = flash_node["page_read_latency"];
    flash_config->set_page_read_latency_lsb(
        read_latency["lsb"].as<uint64_t>(75000));
    flash_config->set_page_read_latency_csb(
        read_latency["csb"].as<uint64_t>(75000));
    flash_config->set_page_read_latency_msb(
        read_latency["msb"].as<uint64_t>(75000));

    YAML::Node program_latency = flash_node["page_program_latency"];
    flash_config->set_page_program_latency_lsb(
        program_latency["lsb"].as<uint64_t>(750000));
    flash_config->set_page_program_latency_csb(
        program_latency["csb"].as<uint64_t>(750000));
    flash_config->set_page_program_latency_msb(
        program_latency["msb"].as<uint64_t>(750000));

    flash_config->set_block_erase_latency(
        flash_node["block_erase_latency"].as<uint64_t>(3800000));

    flash_config->set_nr_dies_per_chip(
        flash_node["dies_per_chip"].as<uint32_t>(2));
    flash_config->set_nr_planes_per_die(
        flash_node["planes_per_die"].as<uint32_t>(2));
    flash_config->set_nr_blocks_per_plane(
        flash_node["blocks_per_plane"].as<uint32_t>(2048));
    flash_config->set_nr_pages_per_block(
        flash_node["pages_per_block"].as<uint32_t>(256));

    flash_config->set_page_capacity(
        flash_node["page_capacity"].as<uint32_t>(8192));

    return true;
}
