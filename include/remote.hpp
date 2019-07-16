#pragma once
#include "base.hpp"

#include <variant>

#include <grpcpp/grpcpp.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "format.pb.h"
#include "format.grpc.pb.h"

namespace c3::kademlia {
  class node;

  class remote_node {
  private:
    node* parent;
    contact details;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<proto::Kademlia::Stub> stub = proto::Kademlia::NewStub(channel);
    std::chrono::milliseconds timeout;

  private:
    void first_ping();
    void init_ctx(grpc::ClientContext& ctx);
    void check_ctx(grpc::ClientContext& ctx);
    void handle_status(grpc::Status s);

  public:
    nid_t get_nid() const { return details.nid; }
    std::string get_location() const { return details.location; }
    contact get_contact() const { return details; }
    operator contact() const { return details; }

    void ping();
    bool store(span<const uint8_t> data, age_t age = age_t{0});
    std::vector<contact> find_node(nid_t nid);
    std::variant<std::vector<uint8_t>, std::vector<contact>> find_value(nid_t nid);
    void republish(span<const uint8_t> data, age_t age);

  public:
    template<typename Duration>
    inline remote_node(node* parent, contact c, Duration net_timeout) :
      parent{parent},
      details{c},
      channel{grpc::CreateChannel(c.location, grpc::InsecureChannelCredentials())},
      timeout{std::chrono::duration_cast<decltype(timeout)>(net_timeout)} {
      ping();
    }

    /// XXX: Please be very afraid of using this in k_buckets: it WILL deadlock any parent mutex
    template<typename Duration>
    inline remote_node(node* parent, std::string location, Duration net_timeout) :
      parent{parent},
      // We set the nid in first_ping()
      details{{}, location},
      channel(grpc::CreateChannel(location, grpc::InsecureChannelCredentials())),
      timeout{std::chrono::duration_cast<decltype(timeout)>(net_timeout)} {
      first_ping();
    }
    /// XXX: Please be very afraid of using this in k_buckets: it WILL deadlock any parent mutex
    inline remote_node(node* parent, std::string location) :
      remote_node{parent, location, std::chrono::seconds(3)} {}

    inline remote_node(node* parent, contact c) :
      remote_node{parent, c, std::chrono::seconds(3)} {}
  };
}
