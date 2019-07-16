#include "base.hpp"

#include <random>
#include <openssl/sha.h>

#include <sstream>
#include <iomanip>

namespace c3::kademlia {
  nid_t compute_nid(span<const uint8_t> data) {
    nid_t ret;
    ::SHA256(data.data(), data.size(), ret.data());
    return ret;
  }

  size_t distance(nid_t a, nid_t b) {
    // A table will be fast and efficient
    constexpr size_t _ffs_table[256] = {
      8, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
      5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
      7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    };

    for (size_t i = 0; i < a.size(); ++i) {
      if ((a[i] ^= b[i]) == 0) continue;
      else return (a.size() - i - 1) * 8 + _ffs_table[a[i]];
    }

    return 0;
  }

  nid_t generate_nid() {
    thread_local std::random_device rng;
    thread_local std::uniform_int_distribution<uint8_t> dist;

    nid_t ret;
    std::generate(ret.begin(), ret.end(), [&] () { return dist(rng); });

    return ret;
  }

  std::string nid_to_string(nid_t nid) {
    std::stringstream ss;

    ss << std::hex << std::setfill('0');

    for (auto i : nid)
      ss << std::setw(2) << (int)i;

    return ss.str();
  }

  nid_t parse_nid(std::string_view str) {
    nid_t ret;

    if (str.size() != std::tuple_size_v<nid_t> * 2)
      throw std::invalid_argument("Bad nid");

    auto iter = str.cbegin();

    for (size_t i = 0; i < std::tuple_size_v<nid_t>; ++i) {
      char buf[3] = {*iter++, *iter++, 0};
      auto byte = ::strtoul(buf, nullptr, 16);

      if (byte > 255)
        throw std::invalid_argument("Bad byte in nid");
      ret[i] = byte;
    }

    return ret;
  }

  std::string strip_port(std::string_view location) {
    auto pos = location.find_last_of(':');
    if (pos == std::string_view::npos)
      throw std::invalid_argument("No port found on location");

    return { location.begin() + pos + 1, location.end() };
  }

  std::string replace_port(std::string_view location, std::string_view port) {
    std::string ret;

    auto pos = location.find_last_of(':');
    if (pos == std::string_view::npos)
      throw std::invalid_argument("No port found on location");

    ret.reserve(pos + port.size());

    // Add 1 to include the colon
    ret.append(location.begin(), location.begin() + pos + 1);
    ret.append(port);

    return ret;
  }
}
