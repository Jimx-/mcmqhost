#ifndef _CONFIG_READER_H_
#define _CONFIG_READER_H_

#include "ssd_config.pb.h"

#include <string>
#include <unordered_map>
#include <vector>

struct NamespaceDefinition {
    size_t capacity_sects;

    explicit NamespaceDefinition(size_t capacity_sects)
        : capacity_sects(capacity_sects)
    {}
};

enum class FlowType {
    SYNTHETIC,
};

enum class RequestSizeDistribution {
    CONSTANT,
    NORMAL,
};

enum class AddressDistribution {
    UNIFORM,
    ZIPFIAN,
};

struct FlowDefinition {
    std::string name;
    FlowType type;
    unsigned int nsid;

    union {
        struct {
            unsigned int seed;
            size_t request_count;
            double read_ratio;

            RequestSizeDistribution request_size_distribution;
            int request_size_mean;
            int request_size_variance;

            AddressDistribution address_distribution;
            double zipfian_alpha;
            unsigned int address_alignment;

            unsigned int average_enqueued_requests;
        } synthetic;
    };
};

struct HostConfig {
    unsigned int io_queue_depth;
    size_t sector_size;

    std::unordered_map<unsigned int, NamespaceDefinition> namespaces;
    std::vector<FlowDefinition> flows;
};

struct ConfigReader {
    static bool load_ssd_config(const std::string& filename,
                                mcmq::SsdConfig& ssd_config);
    static bool load_host_config(const std::string& filename,
                                 const mcmq::SsdConfig& ssd_config,
                                 HostConfig& config);
};

#endif
