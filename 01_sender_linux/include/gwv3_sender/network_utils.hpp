#pragma once

#include <cstdint>
#include <string>

#include <netinet/in.h>

namespace gwv3 {

bool is_valid_ipv4_or_hostname(const std::string &host);
sockaddr_in resolve_ipv4_endpoint(const std::string &host, uint16_t port);

}  // namespace gwv3
