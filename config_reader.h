#ifndef _CONFIG_READER_H_
#define _CONFIG_READER_H_

#include "ssd_config.pb.h"

#include <string>

struct ConfigReader {
    static bool load_config(const std::string& filename,
                            mcmq::SsdConfig& config);
};

#endif
