#include <fstream>
#include <iostream>
#include <cstdlib>
#include <vector>

#include "all.h"
#include "json.hpp"

namespace {

bool load_config_file(const std::string& path, nlohmann::json& req_body)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    try {
        file >> req_body;
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

void load_default_config()
{
    nlohmann::json req_body;
    std::string loaded_path;
    const char* config_path = std::getenv("EDGE_VOICE_CONFIG");
    if (config_path != nullptr && load_config_file(config_path, req_body)) {
        loaded_path = config_path;
    } else {
        const std::vector<std::string> config_paths = {
                "../master_config.json",
                "master_config.json",
                "../config/master_config.json",
                "config/master_config.json",
        };
        for (const auto& path : config_paths) {
            if (load_config_file(path, req_body)) {
                loaded_path = path;
                break;
            }
        }
    }

    if (loaded_path.empty()) {
        ALOGE("load config failed");
        return;
    }

    for (auto it = req_body.begin(); it != req_body.end(); ++it) {
        if (req_body[it.key()].is_number()) {
            key_sql[(std::string)it.key()] = it.value().get<int>();
        }
        if (req_body[it.key()].is_string()) {
            key_sql[(std::string)it.key()] = it.value().get<std::string>();
        }
    }
}
