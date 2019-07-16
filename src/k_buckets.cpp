#include "k_buckets.hpp"

namespace c3::kademlia {
  std::vector<contact> k_buckets::find_node(nid_t sender, nid_t nid) const {
   std::vector<contact> ret;
   auto inserter = std::back_inserter(ret);

   size_t nid_distance = distance(parent->get_nid(), nid);

   {
     auto& mutex_and_bucket = base[nid_distance];
     std::shared_lock lock{mutex_and_bucket.first};
     const auto& bucket = mutex_and_bucket.second;

     size_t limit = std::min(bucket.size(), k);
     auto bucket_iter = bucket.cbegin();

     for (size_t i = 0; i < limit; ++i) {
       if (bucket_iter->nid == sender) continue;
       *inserter++ = *(bucket_iter++);
     }
   }

   if (ret.size() == k)
     return ret;

   bool hit_max = false;
   bool hit_min = false;

   size_t i = 0;

   while(true) {
     if (!hit_max) {
       auto offset = nid_distance + i;
       hit_max = (offset == B - 1);

       if (offset == nid_distance)
         goto skip_max;

       auto& mutex_and_bucket = base[offset];
       std::shared_lock lock{mutex_and_bucket.first};
       const auto& bucket = mutex_and_bucket.second;

       for (auto i : bucket) {
         if (i.nid == sender)
           continue;
         ret.push_back(i);
         if (ret.size() == k)
           return ret;
       }
     }
     skip_max:
     if (!hit_min) {
       auto offset = nid_distance - i;
       hit_min = (offset == 0);

       if (offset == nid_distance)
         goto skip_min;

       auto& mutex_and_bucket = base[offset];
       std::shared_lock lock{mutex_and_bucket.first};
       const auto& bucket = mutex_and_bucket.second;

       for (auto i : bucket) {
         if (i.nid == sender)
           continue;
         ret.push_back(i);
         if (ret.size() == k)
           return ret;
       }
     }

     skip_min:
     if (hit_max && hit_min)
       break;
     else ++i;
   }

   return ret;
  }

  bool k_buckets::update(contact c) {
    if (c.nid == parent->get_nid())
      throw std::runtime_error("Tried to add self!");

    try {
      auto& mutex_and_bucket = get_bucket(c.nid);
      std::unique_lock lock{mutex_and_bucket.first};
      auto& bucket = mutex_and_bucket.second;

      auto pos = std::find_if(bucket.begin(), bucket.end(),
                              [&](auto a) { return a.nid == c.nid; });
      if (pos != bucket.end()) {
        auto elem = std::move(*pos);
        bucket.erase(pos);
        bucket.push_front(elem);
      }
      else
        bucket.push_front(c);

      return true;
    }
    catch (...) {
      drop(c.nid);
      return false;
    }
  }

  bool k_buckets::drop(nid_t nid) {
    auto& mutex_and_bucket = get_bucket(nid);
    std::unique_lock lock{mutex_and_bucket.first};
    auto& bucket = mutex_and_bucket.second;

    auto pos = std::find_if(bucket.begin(), bucket.end(),
                            [&](auto a) { return a.nid == nid; });
    if (pos == bucket.end()) return false;

    bucket.erase(pos);

    return true;
  }

  std::vector<contact> k_buckets::get_alpha(nid_t nid) const {
    std::vector<contact> ret;
    auto inserter = std::back_inserter(ret);

    size_t nid_distance = distance(parent->get_nid(), nid);

    {
      auto& mutex_and_bucket = base[nid_distance];
      std::shared_lock lock{mutex_and_bucket.first};
      const auto& bucket = mutex_and_bucket.second;

      size_t limit = std::min(bucket.size(), alpha);
      auto bucket_iter = bucket.cbegin();

      for (size_t i = 0; i < limit; ++i) {
        if ((*bucket_iter).nid == nid) continue;
        *inserter++ = *bucket_iter++;
      }
    }

    if (ret.size() != alpha) {
      // We start at one so that we do not give ourselves as a node
      for (size_t i = 1; i < base.size(); ++i) {
        // Stop duplicates
        if (i == nid_distance) continue;

        auto& mutex_and_bucket = base[i];
        std::shared_lock lock{mutex_and_bucket.first};
        const auto& bucket = mutex_and_bucket.second;

        std::copy_n(bucket.begin(), std::min(bucket.size(), ret.size() - alpha), inserter);

        if (ret.size() == alpha)
          break;
      }
    }

    return ret;
  }


  void k_buckets::add(contact c) {
    if (c.nid == parent->get_nid())
      throw std::runtime_error("Tried to add self!");

    auto& mutex_and_bucket = get_bucket(c.nid);
    std::unique_lock lock{mutex_and_bucket.first};
    auto& bucket = mutex_and_bucket.second;


    if (std::find_if(bucket.cbegin(), bucket.cend(),
                     [&](const contact& i) { return i.nid == c.nid; }) != bucket.cend())
      return;

    bucket.push_front(c);
  }

  size_t k_buckets::count() const {
    size_t acc = 0;

    for (auto& i : base) {
      std::shared_lock lock{i.first};
      acc += i.second.size();
    }

    return acc;
  }

  std::vector<contact> k_buckets::get_all() const {
    std::vector<contact> ret;

    for (auto& i : base) {
      std::shared_lock lock{i.first};
      std::copy(i.second.begin(), i.second.end(), std::back_inserter(ret));
    }

    return ret;
  }
}
