#include "libmcmq/config_reader.h"

#include <yaml-cpp/yaml.h>

static mcmq::PlaneAllocateScheme
get_plane_allocate_scheme(const std::string& scheme)
{
    if (scheme == "CWDP") {
        return mcmq::PAS_CWDP;
    } else if (scheme == "CWPD") {
        return mcmq::PAS_CWPD;
    } else if (scheme == "CDWP") {
        return mcmq::PAS_CDWP;
    } else if (scheme == "CDPW") {
        return mcmq::PAS_CDPW;
    } else if (scheme == "CPWD") {
        return mcmq::PAS_CPWD;
    } else if (scheme == "CPDW") {
        return mcmq::PAS_CPDW;
    } else if (scheme == "WCDP") {
        return mcmq::PAS_WCDP;
    } else if (scheme == "WCPD") {
        return mcmq::PAS_WCPD;
    } else if (scheme == "WDCP") {
        return mcmq::PAS_WDCP;
    } else if (scheme == "WDPC") {
        return mcmq::PAS_WDPC;
    } else if (scheme == "WPCD") {
        return mcmq::PAS_WPCD;
    } else if (scheme == "WPDC") {
        return mcmq::PAS_WPDC;
    } else if (scheme == "DCWP") {
        return mcmq::PAS_DCWP;
    } else if (scheme == "DCPW") {
        return mcmq::PAS_DCPW;
    } else if (scheme == "DWCP") {
        return mcmq::PAS_DWCP;
    } else if (scheme == "DWPC") {
        return mcmq::PAS_DWPC;
    } else if (scheme == "DPCW") {
        return mcmq::PAS_DPCW;
    } else if (scheme == "DPWC") {
        return mcmq::PAS_DPWC;
    } else if (scheme == "PCWD") {
        return mcmq::PAS_PCWD;
    } else if (scheme == "PCDW") {
        return mcmq::PAS_PCDW;
    } else if (scheme == "PWCD") {
        return mcmq::PAS_PWCD;
    } else if (scheme == "PWDC") {
        return mcmq::PAS_PWDC;
    } else if (scheme == "PDCW") {
        return mcmq::PAS_PDCW;
    } else if (scheme == "PDWC") {
        return mcmq::PAS_PDWC;
    }

    return mcmq::PAS_CWDP;
}

static void load_ssd_namespaces(YAML::Node namespaces,
                                mcmq::SsdConfig& ssd_config)
{
    for (int i = 0; i < namespaces.size(); i++) {
        YAML::Node ns = namespaces[i];
        auto* ns_config = ssd_config.add_namespaces();

        auto channels = ns["channels"];
        for (int j = 0; j < channels.size(); j++) {
            ns_config->add_channel_ids(channels[j].as<uint32_t>());
        }

        auto chips = ns["chips"];
        for (int j = 0; j < chips.size(); j++) {
            ns_config->add_chip_ids(chips[j].as<uint32_t>());
        }

        auto dies = ns["dies"];
        for (int j = 0; j < dies.size(); j++) {
            ns_config->add_die_ids(dies[j].as<uint32_t>());
        }

        auto planes = ns["planes"];
        for (int j = 0; j < planes.size(); j++) {
            ns_config->add_plane_ids(planes[j].as<uint32_t>());
        }

        auto plane_allocate_scheme =
            (ns["plane_allocate_scheme"].as<std::string>("CWDP"));

        ns_config->set_plane_allocate_scheme(
            get_plane_allocate_scheme(plane_allocate_scheme));
    }
}

bool ConfigReader::load_ssd_config(const std::string& filename,
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

    load_ssd_namespaces(root["namespaces"], config);

    return true;
}

static void load_workload_flow(YAML::Node flow_node, HostConfig& config)
{
    config.flows.push_back({});
    auto& flow = config.flows.back();

    auto ns = flow_node["namespace"].as<uint32_t>(0);
    flow.nsid = ns;

    auto type = flow_node["type"].as<std::string>("synthetic");
    if (type == "synthetic") {
        flow.type = FlowType::SYNTHETIC;

        flow.synthetic.seed = flow_node["seed"].as<unsigned int>(123);

        flow.synthetic.request_count =
            flow_node["request_count"].as<size_t>(10000);

        flow.synthetic.read_ratio = flow_node["read_ratio"].as<double>(0.5);

        auto req_size_dist =
            flow_node["request_size_distribution"].as<std::string>("constant");

        flow.synthetic.request_size_mean =
            flow_node["request_size_mean"].as<int>(8);

        if (req_size_dist == "constant")
            flow.synthetic.request_size_distribution =
                RequestSizeDistribution::CONSTANT;
        else if (req_size_dist == "normal") {
            flow.synthetic.request_size_distribution =
                RequestSizeDistribution::NORMAL;
            flow.synthetic.request_size_variance =
                flow_node["request_size_variance"].as<int>(0);
        }

        auto addr_dist =
            flow_node["address_distribution"].as<std::string>("uniform");
        if (addr_dist == "uniform")
            flow.synthetic.address_distribution = AddressDistribution::UNIFORM;
        else if (addr_dist == "zipfian") {
            flow.synthetic.address_distribution = AddressDistribution::ZIPFIAN;
            flow.synthetic.zipfian_alpha =
                flow_node["zipfian_alpha"].as<double>(1.0);
        }

        flow.synthetic.address_alignment =
            flow_node["address_alignment"].as<unsigned int>(0);

        flow.synthetic.average_enqueued_requests =
            flow_node["average_enqueued_requests"].as<unsigned int>(1);
    }
}

bool ConfigReader::load_host_config(const std::string& filename,
                                    const mcmq::SsdConfig& ssd_config,
                                    HostConfig& config)
{
    YAML::Node root;

    try {
        root = YAML::LoadFile(filename);
    } catch (YAML::BadFile&) {
        return false;
    }

    config.sector_size = 512;

    config.io_queue_depth = root["io_queue_depth"].as<unsigned int>(1024);

    auto& flash_config = ssd_config.flash_config();
    size_t chip_capacity_sects =
        flash_config.nr_dies_per_chip() * flash_config.nr_planes_per_die() *
        flash_config.nr_blocks_per_plane() * flash_config.nr_pages_per_block() *
        flash_config.nr_pages_per_block() / config.sector_size;

    for (int i = 0; i < ssd_config.namespaces_size(); i++) {
        auto& ns = ssd_config.namespaces(i);
        unsigned int nsid = i + 1;
        size_t ns_capacity_sects =
            chip_capacity_sects * ns.channel_ids_size() * ns.chip_ids_size();
        config.namespaces.emplace(nsid, NamespaceDefinition{ns_capacity_sects});
    }

    auto flows = root["flows"];
    for (int i = 0; i < flows.size(); i++) {
        YAML::Node flow = flows[i];
        load_workload_flow(flow, config);
    }

    return true;
}
