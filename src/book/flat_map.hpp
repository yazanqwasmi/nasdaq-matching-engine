// Minimal open-addressing hash map (linear probing, backward-shift
// deletion). Purpose-built for the FastBook's order-id index: erases leave
// no tombstones, so a size-bounded steady state never rehashes and the hot
// path never allocates. Grows (allocates) only when load exceeds ~70%.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nsq {

template <typename K, typename V>
class FlatMap {
 public:
  explicit FlatMap(std::size_t expected = 16) {
    std::size_t cap = 16;
    while (cap * 7 < expected * 10) cap <<= 1;  // keep load under 70%
    slots_.resize(cap);
  }

  std::size_t size() const { return size_; }

  bool contains(K key) const { return find_slot(key) != kNpos; }

  V* find(K key) {
    const std::size_t i = find_slot(key);
    return i == kNpos ? nullptr : &slots_[i].val;
  }

  const V* find(K key) const {
    const std::size_t i = find_slot(key);
    return i == kNpos ? nullptr : &slots_[i].val;
  }

  // Inserts or overwrites.
  void insert(K key, V val) {
    if ((size_ + 1) * 10 > slots_.size() * 7) grow();
    std::size_t i = ideal(key);
    for (;;) {
      Slot& s = slots_[i];
      if (!s.used) {
        s = {key, val, true};
        ++size_;
        return;
      }
      if (s.key == key) {
        s.val = val;
        return;
      }
      i = next(i);
    }
  }

  bool erase(K key) {
    std::size_t i = find_slot(key);
    if (i == kNpos) return false;
    // Backward-shift deletion: pull displaced entries into the hole.
    std::size_t hole = i;
    std::size_t j = i;
    for (;;) {
      j = next(j);
      if (!slots_[j].used) break;
      const std::size_t home = ideal(slots_[j].key);
      // Move j into the hole if its home position does not lie in the
      // (cyclic) range (hole, j].
      const bool movable = (hole <= j) ? (home <= hole || home > j)
                                       : (home <= hole && home > j);
      if (movable) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].used = false;
    --size_;
    return true;
  }

  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (const Slot& s : slots_)
      if (s.used) fn(s.key, s.val);
  }

 private:
  struct Slot {
    K key{};
    V val{};
    bool used = false;
  };
  static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

  std::size_t mask() const { return slots_.size() - 1; }
  std::size_t next(std::size_t i) const { return (i + 1) & mask(); }
  std::size_t ideal(K key) const {
    // Fibonacci hashing spreads sequential order ids well.
    return static_cast<std::size_t>(
               (static_cast<std::uint64_t>(key) * 11400714819323198485ULL) >>
               32) &
           mask();
  }

  std::size_t find_slot(K key) const {
    std::size_t i = ideal(key);
    for (;;) {
      const Slot& s = slots_[i];
      if (!s.used) return kNpos;
      if (s.key == key) return i;
      i = next(i);
    }
  }

  void grow() {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(old.size() * 2, Slot{});
    size_ = 0;
    for (const Slot& s : old)
      if (s.used) insert(s.key, s.val);
  }

  std::vector<Slot> slots_;
  std::size_t size_ = 0;
};

}  // namespace nsq
