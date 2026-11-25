#pragma once
#include "sfx/application/node_pool.hpp"
#include <filesystem>

namespace sfx::apps::runtime {
application::NodeEnablement read_linux_node_controls(const std::filesystem::path&);
void publish_linux_node_controls(const std::filesystem::path&,const application::NodeEnablement&);
}
