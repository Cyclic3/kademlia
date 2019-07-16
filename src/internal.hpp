#pragma once

#include <string>

namespace c3::kademlia {
  constexpr const char* metadata_nid_key = "nid-bin";
  constexpr const char* metadata_port_key = "port";

  std::string strip_port(std::string_view location);
}
