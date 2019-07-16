#pragma once

#include "base.hpp"
#include "backing_store.hpp"
#include "remote.hpp"

namespace c3::kademlia {
  class node {
  private:
    class impl;

  private:
    nid_t our_nid;
    std::string our_port;

    std::unique_ptr<impl> service;
    std::unique_ptr<grpc::Server> server;

  public:
    constexpr nid_t get_nid() const { return our_nid; }
    inline std::string get_port() const { return our_port; }
    void add_peer(std::string location);
    size_t count_peers() const;

   private:
    remote_node connect(std::string location);
    remote_node connect(contact c);
    void iterative_store(nid_t key, span<const uint8_t> data, age_t age);
    std::vector<contact> iterative_find_node(nid_t nid);
    std::variant<std::vector<uint8_t>, std::vector<contact>> iterative_find_value(nid_t nid);

  public:
    std::shared_ptr<backing_store> back() const;
    void join();
    void store(nid_t nid, span<const uint8_t> data, age_t age = age_t{0});
    inline nid_t store(span<const uint8_t> b) {
      nid_t nid = compute_nid(b);
      store(nid, b);
      return nid;
    }
    std::optional<std::vector<uint8_t>> find(nid_t);
    void ping_all();

  public:
    node(std::string addr, nid_t nid, std::shared_ptr<backing_store> store);
    // To allow us to have a unique_ptr of a (currently) incomplete type
    ~node();
  };
}
