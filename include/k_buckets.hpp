#pragma once

#include "base.hpp"
#include "remote.hpp"

#include <shared_mutex>
#include <memory>
#include <list>
#include <functional>

#include "node.hpp"

namespace c3::kademlia {
  class k_buckets {
  private:
    node* parent;
    // The webernet says that this is OK for sharing, so don't blame me
    mutable std::array<std::pair<std::shared_mutex, std::list<contact>>, B> base;

  public:
    inline node* get_parent() const { return parent; }

  private:
    inline decltype(base)::reference get_bucket(nid_t nid) {
      return base[distance(parent->get_nid(), nid)];
    }
    inline decltype(base)::const_reference get_bucket(nid_t nid) const {
      return base[distance(parent->get_nid(), nid)];
    }

  public:
    std::vector<contact> find_node(nid_t sender, nid_t nid) const;
    /// We don't use a string_view, because gRPC's peer returns a string, not a reference to one
    bool update(contact c);
    bool drop(nid_t nid);
    void add(contact location);
    size_t count() const;
    std::vector<contact> get_alpha(nid_t nid) const;
    std::vector<contact> get_all() const;

  public:
    inline k_buckets(node* parent) : parent{parent} {}
  };
}
