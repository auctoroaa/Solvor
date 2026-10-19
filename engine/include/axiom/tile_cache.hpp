#pragma once
#include "axiom/axiom_core.hpp"
#include <list>
#include <unordered_map>

namespace axiom {

// Bounded LRU cache for 4 KiB tiles. Pinned entries cannot be evicted.
// The backing store remains responsible for loading a miss and persisting dirty data.
class TileCache {
 public:
  explicit TileCache(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) throw std::invalid_argument("tile cache capacity must be nonzero");
  }
  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t size() const noexcept { return entries_.size(); }
  bool contains(std::uint64_t id) const { return entries_.find(id) != entries_.end(); }

  bool get(std::uint64_t id, Tile& out) {
    auto it=entries_.find(id);
    if(it==entries_.end()) return false;
    touch(it); out=it->second.tile; return true;
  }

  // Returns false only when every resident entry is pinned and capacity is full.
  bool put(std::uint64_t id, const Tile& tile) {
    auto found=entries_.find(id);
    if(found!=entries_.end()) {
      found->second.tile=tile; touch(found); return true;
    }
    if(entries_.size()==capacity_ && !evict_one()) return false;
    order_.push_front(id);
    entries_.emplace(id,Entry{tile,order_.begin(),0});
    return true;
  }

  bool pin(std::uint64_t id) {
    auto it=entries_.find(id); if(it==entries_.end()) return false;
    ++it->second.pins; touch(it); return true;
  }
  bool unpin(std::uint64_t id) {
    auto it=entries_.find(id);
    if(it==entries_.end() || it->second.pins==0) return false;
    --it->second.pins; return true;
  }
  std::size_t pin_count(std::uint64_t id) const {
    auto it=entries_.find(id); return it==entries_.end()?0:it->second.pins;
  }
  bool erase(std::uint64_t id) {
    auto it=entries_.find(id);
    if(it==entries_.end() || it->second.pins!=0) return false;
    order_.erase(it->second.position); entries_.erase(it); return true;
  }

 private:
  struct Entry { Tile tile; std::list<std::uint64_t>::iterator position; std::size_t pins; };
  using Iterator=std::unordered_map<std::uint64_t,Entry>::iterator;
  std::size_t capacity_;
  std::list<std::uint64_t> order_;
  std::unordered_map<std::uint64_t,Entry> entries_;

  void touch(Iterator it) {
    order_.erase(it->second.position);
    order_.push_front(it->first);
    it->second.position=order_.begin();
  }
  bool evict_one() {
    for(auto it=order_.rbegin();it!=order_.rend();++it) {
      auto entry=entries_.find(*it);
      if(entry!=entries_.end() && entry->second.pins==0) {
        auto erase_position=std::next(it).base();
        order_.erase(erase_position); entries_.erase(entry); return true;
      }
    }
    return false;
  }
};
} // namespace axiom
