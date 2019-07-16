#include "base.hpp"

#include "node.hpp"

#include "internal.hpp"
#include "k_buckets.hpp"

#include "internal.hpp"
#include "format.pb.h"
#include "format.grpc.pb.h"

#include <queue>

#include <thread>
#include <future>

namespace c3::kademlia {
  class node::impl : public proto::Kademlia::Service {
  public:
    node* parent;
    k_buckets buckets;
    std::shared_ptr<backing_store> back;

    bool replicate_looping = true;
    std::mutex replicate_looping_mutex;
    std::condition_variable replicate_looping_condvar;
    std::thread replicate_thread{&node::impl::rep_loop, this};

  private:
    void rep_loop() {
      while (replicate_looping) {
        std::unique_lock lock{replicate_looping_mutex};
        if (replicate_looping_condvar.wait_for(lock, tReplicate, [&]() { return !replicate_looping; }))
          return;
        auto keys = back->get_all_keys();
        for (auto i : keys) {
          auto val = back->retrieve(i);
          if (!val)
            continue;
          parent->store(i, val->dat, val->age);
        }
      }
    }

  private:
    void find_node_impl(nid_t sender, nid_t nid, proto::FindNodeResponse* res) {
      auto nodes = buckets.find_node(sender, nid);

      for (auto& i : nodes) {
        auto c = res->add_contacts();
        c->set_nid(i.nid.data(), i.nid.size());
        c->set_location(i.location);
      }
    }
    nid_t update(grpc::ServerContext* ctx) {
      auto meta = ctx->client_metadata();

      auto nid_iter = meta.find(metadata_nid_key);
      if (nid_iter == meta.end())
        throw std::runtime_error("Bad client nid");
      auto nid = deserialise_nid(nid_iter->second);

      if (nid == parent->get_nid())
        throw std::runtime_error("Talking to yourself");

      auto port_iter = meta.find(metadata_port_key);
      if (port_iter == meta.end())
        throw std::runtime_error("Bad client port");
      //auto port = strtoul(port_iter->second.data(), nullptr, 10); //TODO: change port

      try {
        std::string_view port{ port_iter->second.data(), port_iter->second.size()};
        buckets.update({nid, replace_port(ctx->peer(), port)});
      }
      catch (const std::exception& e) {
        throw;
      }

      ctx->AddInitialMetadata(metadata_nid_key,
        {reinterpret_cast<const char*>(parent->our_nid.data()), parent->our_nid.size()});

      return nid;
    }

  public:
    grpc::Status ping(grpc::ServerContext* ctx, const proto::PingRequest*,
                      proto::PingResponse*) override {
      update(ctx);

      return grpc::Status::OK;
    }

    grpc::Status store(grpc::ServerContext* ctx, const proto::StoreRequest* req,
                       proto::StoreResponse* res) override {
      update(ctx);

      res->set_success(back->store(string_to_data(req->data())));

      return grpc::Status::OK;
    }

    grpc::Status find_node(grpc::ServerContext* ctx, const proto::FindNodeRequest* req,
                           proto::FindNodeResponse* res) override {
      nid_t sender = update(ctx);

      find_node_impl(sender, deserialise_nid(req->nid()), res);

      return grpc::Status::OK;
    }

    grpc::Status find_value(grpc::ServerContext* ctx, const proto::FindValueRequest* req,
                            proto::FindValueResponse* res) override {
      nid_t sender = update(ctx);

      nid_t nid = deserialise_nid(req->nid());

      if (auto val = back->retrieve(nid))
        res->set_found(val->dat.data(), val->dat.size());
      else
        find_node_impl(sender, deserialise_nid(req->nid()), res->mutable_not_found());

      return grpc::Status::OK;
    }

  public:
    impl(node* parent, std::shared_ptr<backing_store> store) :
      parent{parent}, buckets{parent}, back{std::move(store)} {}

    ~impl() {
      {
        std::unique_lock lock{replicate_looping_mutex};
        replicate_looping = false;
        replicate_looping_condvar.notify_all();
      }
      if (replicate_thread.joinable())
        replicate_thread.join();
    }
  };

  // Now we have the impl, we can define the destructor
  node::~node() = default;

  node::node(std::string addr, nid_t nid, std::shared_ptr<backing_store> store) :
    our_nid{nid},
    service{std::make_unique<impl>(this, store)} {
    int bound_port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials(), &bound_port);
    builder.RegisterService(service.get());
    server = builder.BuildAndStart();
    if (bound_port == 0)
      throw std::runtime_error("Could not open port");

    our_port = std::to_string(bound_port);
  }

  remote_node node::connect(std::string location) {
    return {this, location};
  }

  remote_node node::connect(contact c) {
    try {
      return { this, c };
    }
    catch(...) {
      service->buckets.drop(c.nid);
      throw;
    }
  }

  void node::add_peer(std::string location) {
    service->buckets.add(connect(location));
  }


  size_t node::count_peers() const {
    return service->buckets.count();
  }

  void node::join() {
    auto found = iterative_find_node(get_nid());

    for (auto i : found)
      add_peer(i.location);
  }

  std::optional<std::vector<uint8_t>> node::find(nid_t nid) {
    auto ret = iterative_find_value(nid);
    return std::visit([&](auto val) -> std::optional<std::vector<uint8_t>> {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<std::vector<uint8_t>, T>) {
        // Give us a copy that will not be replicated, in case we want it later
        service->back->store(val);
        return val;
      }
      else
        return std::nullopt;
    }, ret);
  }

  using found_node_t = std::vector<contact>;
  using found_value_t = std::vector<uint8_t>;
  using find_common_ret = std::variant<found_value_t, found_node_t>;

  struct find_iteration {
    std::vector<contact> contacted;
    std::queue<contact> uncontacted;
    // If something is in this collection, but neither of the contacted or uncontacted collections,
    // then it will be ignored
    std::set<nid_t> all;
    // The one value we can be sure will not turn up: us
    nid_t closest_node;
    nid_t nid;
    std::function<remote_node(contact)> connect;
    std::function<void(nid_t)> drop;
    std::function<find_common_ret(remote_node&)> find;

    void add_candidate(contact i) {
      all.insert(i.nid);
      uncontacted.push(i);
    }

    std::optional<find_common_ret> iterate() {
      size_t to_probe = std::min(alpha, uncontacted.size());
      std::vector<contact> closest_n;
      std::vector<std::future<find_common_ret>> threads;
      closest_n.reserve(to_probe);
      // NOTE: not a typo, this is to allow std::transform
      threads.resize(to_probe);

      for (size_t i = 0; i < to_probe; ++i) {
        closest_n.push_back(uncontacted.front());
        // Temporarily reject this node
        uncontacted.pop();
      }
      std::transform(closest_n.begin(), closest_n.end(), threads.begin(),
                     [&](auto c) {
        return std::async(std::launch::async, [&, c]() -> find_common_ret {
          remote_node remote = connect(c);
          auto remote_nid = remote.get_nid();

          if (std::find_if(contacted.begin(), contacted.end(),
                           [&](auto i) { return i.nid == remote_nid; }) != contacted.end())
            throw std::runtime_error("I already have been searched!");

          auto res = find(remote);

          return std::visit([&](auto val) -> find_common_ret {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<found_value_t, T>)
              return val;
            else {
              std::vector<contact> ret;
              for (auto& i : val)
                if (all.find(i.nid) == all.end())
                  ret.emplace_back(std::move(i));
              return find(remote);
            }
          }, res);
        });
      });

      std::optional<std::vector<uint8_t>> end_value;

      for (size_t i = 0; i < threads.size(); ++i) {
        try {
          auto res = threads[i].get();

          contacted.push_back(closest_n[i]);

          std::visit([&](auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, found_value_t>) {
              end_value = val;
            }
            else {
              // Unreject this node
              for (auto& i : val) {
                if (all.find(i.nid) != all.end())
                  continue;

                add_candidate(i);
              }
            }
          }, res);
        }
        // Do nothing, as it is already only in all, and therefore rejected
        catch (...) {
          drop(closest_n[i].nid);
        }
      }

      if (end_value)
        return *end_value;

      if (contacted.size() == k)
        return contacted;

      if (uncontacted.size() == 0 && contacted.size() == 0)
        throw std::runtime_error("All nodes broken");

      nid_t new_closest;
      // Note how we cannot have no nodes, so this will always select a node
      {
        auto iter = all.cbegin();
        size_t new_min_dist = distance(*iter, nid);
        auto closest = iter;
        while (iter != all.cend()) {
          if (auto dist = distance(nid, *iter); dist < new_min_dist) {
            closest = iter;
            new_min_dist = dist;
          }
		  ++iter;
        }
        new_closest = *closest;
      }
      // FIXME: This returns few nodes
      if (std::equal(closest_node.begin(), closest_node.end(), new_closest.begin()))
        return contacted;

      closest_node = new_closest;

      return std::nullopt;
    }

    find_common_ret iterate_until_done() {
      std::optional<find_common_ret> ret;
      do ret = iterate();
      while (!ret);
      return *ret;
    }

  public:
    find_iteration(nid_t nid, nid_t our_nid, decltype(connect) connect, decltype(drop) drop,
                   decltype(find) find) :
      closest_node{our_nid}, nid{nid}, connect{connect}, drop{drop}, find{find} {}
  };

  std::vector<contact> node::iterative_find_node(nid_t nid) {
    find_iteration obj(nid, our_nid,
                       [&](auto i) { return connect(i); },
                       [&](auto i) { service->buckets.drop(i); },
                       [&](remote_node& remote) { return remote.find_node(nid); });
    for (auto i : service->buckets.get_alpha(nid))
      obj.add_candidate(i);

    return std::get<std::vector<contact>>(obj.iterate_until_done());
  }

  std::variant<std::vector<uint8_t>, std::vector<contact>> node::iterative_find_value(nid_t nid) {
    find_iteration obj(nid, our_nid,
                       [&](auto i) { return connect(i); },
                       [&](auto i) { service->buckets.drop(i); },
                       [&](remote_node& remote) { return remote.find_value(nid); });

    for (auto i : service->buckets.get_alpha(nid))
      obj.add_candidate(i);

    auto ret = obj.iterate_until_done();

    std::visit([&](auto res) {
      using T = std::decay_t<decltype(res)>;

      if constexpr (std::is_same_v<found_value_t, T>) {
        // Sort in descending order of distance, so we can pop the back to get the closest
        std::sort(obj.contacted.begin(), obj.contacted.end(), [&](auto a, auto b) {
          return distance(a.nid, nid) > distance(b.nid, nid);
        });
        while (obj.contacted.size() == 0) {
          try {
            remote_node r = connect(obj.contacted.back());
            r.store(res);
          }
          catch (...) {
            obj.contacted.pop_back();
          }
        }
      }
    }, obj.iterate_until_done());

    return ret;
  }

  void node::iterative_store(nid_t key, span<const uint8_t> data, age_t age) {
    for (auto& node : iterative_find_node(key)) {
      connect(node).store(data, age);
    }
  }

  void node::store(nid_t key, span<const uint8_t> data, age_t age) {
    iterative_store(key, data, age);
  }

  std::shared_ptr<backing_store> node::back() const {
    return service->back;
  }

  void node::ping_all() {
    for (auto i : service->buckets.get_all()) {
      try { connect(i).ping(); }
      catch (...) { service->buckets.drop(i.nid); }
    }
  }
}
