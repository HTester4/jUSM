// futex.h - support for futex synchronization

#pragma once

#include <climits>
#include <functional>
#include <unordered_map>

#include "junction/base/arch.h"
#include "junction/base/error.h"
#include "junction/bindings/sync.h"

namespace junction {

namespace detail {

struct futex_waiter {
  thread_t *th;
  uint32_t bitset;
};

struct alignas(kCacheLineSize) futex_bucket {
  std::unordered_multimap<uint32_t *, futex_waiter> futexes;
  rt::Spin lock;
};

}  // namespace detail

constexpr uint32_t kFutexBitsetAny = 0xFFFFFFFF;

class alignas(kCacheLineSize) FutexTable {
 public:
  FutexTable() = default;
  ~FutexTable() = default;

  FutexTable(FutexTable &&) = delete;
  FutexTable &operator=(FutexTable &&) = delete;
  FutexTable(const FutexTable &) = delete;
  FutexTable &operator=(const FutexTable &) = delete;

  // Wait blocks on the address @key, returning true. However, it returns false
  // if @val doesn't match the value in the address.
  bool Wait(uint32_t *key, uint32_t val, uint32_t bitset = kFutexBitsetAny);

  // Wake unblocks up to @n threads waiting on the address @key.
  void Wake(uint32_t *key, int n = INT_MAX, uint32_t bitset = kFutexBitsetAny);

 private:
  static constexpr size_t kBuckets = 16;  // TODO(amb): allocate dynamically?

  // gets the right hash bucket for a key.
  detail::futex_bucket &get_bucket(uint32_t *key) {
    return buckets_[std::hash<uint32_t *>{}(key) % kBuckets];
  }

  detail::futex_bucket buckets_[kBuckets];
};

}  // namespace junction
