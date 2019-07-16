#pragma once

#include <array>
#include <string>
#include <tuple>
#include <gsl/span>
#include <chrono>

namespace c3::kademlia {
  // Seconds
  using age_t = std::chrono::duration<uint64_t>;

  //
  // Constants that control the working of the nework
  //
  // Please be _very_ careful, future me
  // ================[ CONSTANTS ]================
  constexpr size_t k = 20;
  constexpr size_t B = 256;
  constexpr size_t alpha = 3;
  constexpr age_t tExpire{86410};
  constexpr age_t tRefresh{3600};
  constexpr age_t tReplicate{3600};
  constexpr age_t tRepublish{86400};
  // ================[ CONSTANTS ]================

  using nid_t = std::array<uint8_t, B / 8>;
  struct contact {
    nid_t nid;
    std::string location;
  };

  template<typename T>
  using span = gsl::span<T>;

  constexpr ssize_t fix_gsl_bs(size_t sensibly_typed_value) {
    return static_cast<ssize_t>(sensibly_typed_value);
  }

  inline gsl::span<const uint8_t> string_to_data(std::string_view str) {
    return {reinterpret_cast<const uint8_t*>(str.data()), fix_gsl_bs(str.size())};
  }

  template<typename Container>
  constexpr nid_t deserialise_nid(Container s) {
    if (s.size() != std::tuple_size_v<nid_t>)
      throw std::invalid_argument("NID of invalid size");

    nid_t ret;
    std::copy(s.begin(), s.end(), ret.begin());
    return ret;
  }

  nid_t compute_nid(span<const uint8_t> data);

  class timed_out : public std::runtime_error {
  public:
    inline timed_out() : std::runtime_error("An RPC timed out") {};
  };

  size_t distance(nid_t a, nid_t b);

  nid_t generate_nid();

  std::string nid_to_string(nid_t nid);

  nid_t parse_nid(std::string_view str) ;

  std::string replace_port(std::string_view location, std::string_view port);

  std::string strip_port(std::string_view location);
}
