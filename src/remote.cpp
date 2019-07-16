#include "remote.hpp"
#include "base.hpp"

#include "internal.hpp"
#include "k_buckets.hpp"
#include "node.hpp"

#include "format.pb.h"
#include "format.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <variant>
#include <gsl/span>

#include <thread>

namespace c3::kademlia {
  void remote_node::handle_status(grpc::Status status) {
    switch (status.error_code()) {
      case grpc::StatusCode::OK:
        return;
      case grpc::StatusCode::DEADLINE_EXCEEDED:
        throw timed_out{};
      case grpc::StatusCode::UNAVAILABLE:
        throw std::runtime_error("Could not connect");
      case grpc::StatusCode::UNKNOWN:
        throw std::runtime_error("Remote RPC encountered issue");
      default:
        throw std::runtime_error("Unknown RPC error");
    }
  }

  void remote_node::ping() {
    proto::PingRequest req;
    proto::PingResponse res;
    grpc::ClientContext ctx;
    init_ctx(ctx);

    auto status = stub->ping(&ctx, req, &res);
    check_ctx(ctx);
    handle_status(status);
  }
  bool remote_node::store(span<const uint8_t> data, age_t age) {
    proto::StoreRequest req;
    proto::StoreResponse res;
    grpc::ClientContext ctx;
    init_ctx(ctx);

    req.set_data(data.data(), fix_gsl_bs(data.size()));
    req.set_age(age.count());

    auto status = stub->store(&ctx, req, &res);
    check_ctx(ctx);
    handle_status(status);

    return res.success();
  }

  std::vector<contact> remote_node::find_node(nid_t nid) {
    proto::FindNodeRequest req;
    proto::FindNodeResponse res;
    grpc::ClientContext ctx;
    init_ctx(ctx);

    req.set_nid(nid.data(), nid.size());

    auto status = stub->find_node(&ctx, req, &res);
    check_ctx(ctx);
    handle_status(status);

    if (static_cast<size_t>(res.contacts().size()) > k)
      throw std::invalid_argument("Too many found nodes");

    std::vector<contact> ret;
    for (auto& i : res.contacts()) {
      auto nid = deserialise_nid(i.nid());
      if (nid == parent->get_nid())
        throw std::invalid_argument("Was given own nid");
      ret.emplace_back(contact{nid, i.location()});
    }

    return ret;
  }

  std::variant<std::vector<uint8_t>, std::vector<contact>> remote_node::find_value(nid_t nid) {
    proto::FindValueRequest req;
    proto::FindValueResponse res;
    grpc::ClientContext ctx;
    init_ctx(ctx);

    ctx.peer();

    req.set_nid(nid.data(), nid.size());

    auto status = stub->find_value(&ctx, req, &res);
    check_ctx(ctx);
    handle_status(status);

    switch (res.value_case()) {
      case (proto::FindValueResponse::ValueCase::kFound): {
        auto b = string_to_data(res.found());
        return std::vector<uint8_t>{b.begin(), b.end()};
      }
      case (proto::FindValueResponse::ValueCase::kNotFound): {
        if (static_cast<size_t>(res.not_found().contacts().size()) > k)
          throw std::invalid_argument("Too many found nodes");

        std::vector<contact> ret;
        for (auto& i : res.not_found().contacts()) {
          auto nid = deserialise_nid(i.nid());
          if (nid == parent->get_nid())
            throw std::invalid_argument("Was given own nid");
          ret.emplace_back(contact{nid, i.location()});
        }

        return ret;
      }
      default:
        throw std::invalid_argument("Unknown find_value response");
    }
  }

  void remote_node::init_ctx(grpc::ClientContext& ctx) {
    ctx.set_deadline(std::chrono::system_clock::now() + timeout);
    nid_t our_nid = parent->get_nid();
    ctx.AddMetadata(metadata_nid_key,
                    {reinterpret_cast<const char*>(our_nid.data()), our_nid.size()});
    ctx.AddMetadata(metadata_port_key, parent->get_port());
  }

  void remote_node::check_ctx(grpc::ClientContext& ctx) {
    std::string str = ctx.peer();
    std::cout << str << std::endl;
    auto& meta = ctx.GetServerInitialMetadata();
    auto iter = meta.find(metadata_nid_key);
    if (iter == meta.end())
      throw std::runtime_error("Server did not give a nid");
    else if (deserialise_nid(iter->second) != get_nid())
      throw std::runtime_error("Remote nid is inconsistent");

    details.nid = deserialise_nid(iter->second);
  }

  void remote_node::first_ping() {
    proto::PingRequest req;
    proto::PingResponse res;
    grpc::ClientContext ctx;
    init_ctx(ctx);

    auto status = stub->ping(&ctx, req, &res);
    handle_status(status);

    auto& meta = ctx.GetServerInitialMetadata();
    auto iter = meta.find(metadata_nid_key);
    if (iter == meta.end())
      throw std::runtime_error("Server did not give a nid");

    details.nid = deserialise_nid(iter->second);
  }
}
