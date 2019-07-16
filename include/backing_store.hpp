#pragma once
#include "base.hpp"

#include <shared_mutex>
#include <vector>
#include <optional>
#include <map>

#include <functional>
#include <thread>
#include <atomic>

namespace c3::kademlia {
  class backing_store {
  public:
    struct value_t {
      std::vector<uint8_t> dat;
      age_t age;
    };
    struct stats_t {
      size_t bytes_used = 0;
      size_t bytes_max = 0;
      size_t keys_used = 0;
      size_t keys_max = 0;
    };

  public:
    virtual bool store(span<const uint8_t>, age_t age = age_t{0}) noexcept = 0;
    virtual std::optional<value_t> retrieve(nid_t) noexcept = 0;
    virtual std::vector<nid_t> get_all_keys() noexcept = 0;
    virtual stats_t get_stats() noexcept = 0;

  public:
    virtual ~backing_store() = default;

  public:
    class simple;
  };

  class backing_store::simple : public backing_store {
  private:
    struct value_data {
      std::chrono::steady_clock::time_point birth;
      std::vector<uint8_t> data;
      std::thread expire_thread;

      value_data(decltype(birth) birth_, decltype(data) data_, decltype(expire_thread) expire_thread_) :
        birth{std::move(birth_)},
        data{std::move(data_)},
        expire_thread{std::move(expire_thread_)} {}
    };

  private:
    size_t max_size;
    size_t max_keys;

    //
    std::shared_mutex values_mutex;
    std::map<nid_t, value_data> values;
    std::atomic<size_t> values_total_size = 0;
    //

    //
    std::mutex die_mutex;
    std::condition_variable die_condvar;
    std::atomic<bool> die_value = false;
    //

  private:
    void expire(nid_t nid) {
      std::unique_lock lock{die_mutex};
      // We don't really care if we time out, the process is basically the same
      die_condvar.wait_for(lock, tExpire, [&]() -> bool { return die_value; });
      std::unique_lock values_lock{values_mutex};
      auto iter = values.find(nid);
      // Sp00ky
      if (iter == values.end())
        abort();
      values_total_size -= iter->second.data.size();
      --max_keys;
      iter->second.expire_thread.detach();
      values.erase(nid);
    }

  public:
    inline bool store(span<const uint8_t> s, age_t age) noexcept override final {
      try {
        // This is expensive and independent of obj state, so we do this outside the mutex
        auto nid = compute_nid(s);
        // This is also independent of obj state, and may be expensive (depending on implementation)
        auto birth = std::chrono::steady_clock::now();

        std::unique_lock lock{values_mutex};

        if (values.size() == max_keys)
          return false;

        // Check to see if we already have it
        if (values.find(nid) != values.end())
          return true;

        if (values_total_size + s.size() > max_size)
          return false;

        values_total_size += s.size();
        ++max_keys;

        values.emplace(nid, value_data{birth - age, {s.begin(), s.end()},
                                       std::thread{&simple::expire, this, nid}});


        return true;
      }
      catch (...) {
        return false;
      }
    }

    inline std::optional<value_t> retrieve(nid_t nid) noexcept override final {
      // This is also independent of obj state, and may be expensive (depending on implementation)
      auto now = std::chrono::steady_clock::now();
      std::shared_lock lock{values_mutex};

      if (auto i = values.find(nid); i != values.end())
        return value_t{ i->second.data, std::chrono::duration_cast<age_t>(now - i->second.birth) };
      else
        return std::nullopt;
    }

    inline std::vector<nid_t> get_all_keys() noexcept override final {
      std::vector<nid_t> ret;

      try {
        std::shared_lock lock{values_mutex};

        for (auto& i : values)
          ret.push_back(i.first);
      }
      catch (...) {}

      return ret;
    }

    inline stats_t get_stats() noexcept override final {
      return {
            .bytes_used = values_total_size,
            .bytes_max = max_size,
            .keys_used = values.size(),
            .keys_max = max_keys
      };
    }

  public:
    inline simple(size_t max_size = 16 * 1024 * 1024, size_t max_keys = 1024) :
      max_size{max_size}, max_keys{max_keys} {}
  };
}
