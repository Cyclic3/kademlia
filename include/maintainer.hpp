#pragma once

#include "base.hpp"
#include "backing_store.hpp"
#include "node.hpp"

namespace c3::kademlia {
  class maintainer {
  public:
    node* parent;
    std::shared_ptr<backing_store> back;

  private:
    std::vector<nid_t> nids;

  public:
    void upload(nid_t nid) {

    }

  public:
    maintainer(node* parent, std::shared_ptr<backing_store> back):
      parent{parent}, back{back} {}
  };
}
