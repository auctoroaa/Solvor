# AXIOM Reality Engine Core

Consolidated source file for the current C++20 implementation in `auctoroaa/Solvor`.

> Status: C++ core with WAL recovery and a tested bounded-cache primitive. The cache is not yet integrated as the engine's backing store. WAL segment rotation, full StatePager integration, and optional io_uring remain unfinished. This document does not claim production completeness.

## README

```text
# AXIOM C++20 Engine Core

## Build and test

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --parallel
ctest --test-dir engine/build --output-on-failure
```

## Implemented and tested

- Deterministic DAG layering and stable task ordering.
- Declared tile-access conflict checks and transactional tick updates.
- Rollback when a task fails before commit.
- SHA-256 state digests and a deterministic hash-chain verifier.
- CRC32C-protected binary write-ahead log records with fsync before a tick is accepted.
- CRC32C-protected, atomically replaced genesis state for initial tiles.
- Restart recovery that validates tick order, before/after state digests, record CRCs, and truncates an incomplete or invalid WAL tail.
- Bounded LRU `TileCache` primitive with pin-aware eviction and tests.

## Remaining integration work

The cache is currently a tested primitive, not yet the engine's backing store. The engine still keeps its active state in an in-memory map. WAL segment rotation, a complete StatePager that pages state through the bounded cache, optional io_uring I/O, and broader fault-injection/concurrency testing remain unfinished. Do not treat this as the complete production AXIOM Reality Engine yet.

```

## CMake build

```cmake
cmake_minimum_required(VERSION 3.20)
project(axiom_core VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(OpenSSL REQUIRED)
add_library(axiom_core src/axiom_core.cpp)
target_include_directories(axiom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(axiom_core PUBLIC OpenSSL::Crypto)
target_compile_options(axiom_core PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
add_executable(test_axiom tests/test_axiom.cpp)
target_link_libraries(test_axiom PRIVATE axiom_core)
enable_testing()
add_test(NAME axiom_core_tests COMMAND test_axiom)

```

## Core API

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace axiom {
constexpr std::size_t TILE_BYTES = 4096;
using Digest = std::array<std::uint8_t, 32>;
using Tile = std::array<std::uint8_t, TILE_BYTES>;
using State = std::map<std::uint64_t, Tile>;

struct Task {
  std::uint64_t id{};
  std::vector<std::uint64_t> dependencies;
  std::set<std::uint64_t> reads;
  std::set<std::uint64_t> writes;
  std::function<void(const State&, State&)> execute;
};
struct TickResult { bool ok{}; std::uint64_t tick{}; std::string error; Digest digest{}; };

Digest sha256(const std::uint8_t* data, std::size_t size);
Digest state_digest(const State& state);
std::string hex(const Digest& digest);

class ZeroVerifier {
 public:
  bool record(std::uint64_t tick, const Digest& state_hash);
  bool verify() const;
  std::uint64_t last_tick() const noexcept { return last_tick_; }
 private:
  std::vector<std::pair<std::uint64_t, Digest>> records_;
  std::vector<Digest> chain_;
  std::uint64_t last_tick_{};
};

class AxiomEngine {
 public:
  explicit AxiomEngine(std::filesystem::path storage_root = {});
  void set_tile(std::uint64_t id, const Tile& tile);
  bool get_tile(std::uint64_t id, Tile& out) const;
  bool build_graph(const std::vector<Task>& tasks, std::string& error);
  TickResult execute_tick();
  std::uint64_t current_tick() const noexcept { return tick_; }
  const State& state() const noexcept { return state_; }
  bool verify() const { return verifier_.verify(); }
 private:
  std::filesystem::path root_;
  State state_;
  std::vector<Task> tasks_;
  std::vector<std::vector<std::size_t>> layers_;
  std::uint64_t tick_{};
  Digest last_digest_{};
  ZeroVerifier verifier_;
  bool recover(std::string& error);
  bool persist_genesis(std::string& error) const;
  bool append_commit(std::uint64_t tick, const State& before_state, const State& after_state,
                     const Digest& before, const Digest& after, std::string& error);
};
} // namespace axiom

```

## Bounded tile cache

```cpp
#pragma once
#include "axiom/axiom_core.hpp"
#include <list>
#include <stdexcept>
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

```

## Core implementation

```cpp
#include "axiom/axiom_core.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace axiom {
namespace {
constexpr std::array<std::uint8_t,8> WAL_MAGIC{'A','X','W','A','L','0','0','1'};
constexpr std::array<std::uint8_t,8> GEN_MAGIC{'A','X','G','E','N','0','0','1'};
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i=0;i<8;++i) out.push_back(static_cast<std::uint8_t>(v>>(i*8)));
}
std::uint64_t get_u64(const std::uint8_t* p) {
  std::uint64_t v=0; for(int i=0;i<8;++i) v|=std::uint64_t(p[i])<<(i*8); return v;
}
std::uint32_t crc32c(const std::uint8_t* data, std::size_t n) {
  std::uint32_t crc=~std::uint32_t{0};
  for(std::size_t i=0;i<n;++i) {
    crc^=data[i];
    for(int b=0;b<8;++b) crc=(crc>>1)^(0x82f63b78u & (0u-(crc&1u)));
  }
  return ~crc;
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for(int i=0;i<4;++i) out.push_back(static_cast<std::uint8_t>(v>>(i*8)));
}
bool write_all(int fd,const std::uint8_t* p,std::size_t n) {
  std::size_t done=0;
  while(done<n) {
    auto w=::write(fd,p+done,n-done);
    if(w<0) { if(errno==EINTR) continue; return false; }
    if(w==0) return false;
    done+=static_cast<std::size_t>(w);
  }
  return true;
}
}
Digest sha256(const std::uint8_t* data, std::size_t size) {
  Digest out{}; SHA256(data, size, out.data()); return out;
}
Digest state_digest(const State& state) {
  SHA256_CTX ctx; SHA256_Init(&ctx);
  for (const auto& [id, tile] : state) {
    std::uint8_t key[8];
    for (int i=0;i<8;++i) key[i] = static_cast<std::uint8_t>(id >> (i*8));
    SHA256_Update(&ctx, key, sizeof key);
    SHA256_Update(&ctx, tile.data(), tile.size());
  }
  Digest out{}; SHA256_Final(out.data(), &ctx); return out;
}
std::string hex(const Digest& digest) {
  std::ostringstream s; s << std::hex << std::setfill('0');
  for (auto b : digest) s << std::setw(2) << static_cast<unsigned>(b);
  return s.str();
}
bool ZeroVerifier::record(std::uint64_t tick, const Digest& state_hash) {
  if (tick != last_tick_ + 1) return false;
  std::array<std::uint8_t, 72> material{};
  for (int i=0;i<8;++i) material[i] = static_cast<std::uint8_t>(tick >> (i*8));
  if (!chain_.empty()) std::copy(chain_.back().begin(), chain_.back().end(), material.begin()+8);
  std::copy(state_hash.begin(), state_hash.end(), material.begin()+40);
  chain_.push_back(sha256(material.data(), material.size()));
  records_.emplace_back(tick, state_hash); last_tick_ = tick; return true;
}
bool ZeroVerifier::verify() const {
  if (records_.size() != chain_.size()) return false;
  Digest previous{};
  for (std::size_t n=0; n<records_.size(); ++n) {
    std::array<std::uint8_t,72> material{}; auto tick=records_[n].first;
    for(int i=0;i<8;++i) material[i]=static_cast<std::uint8_t>(tick>>(i*8));
    std::copy(previous.begin(),previous.end(),material.begin()+8);
    std::copy(records_[n].second.begin(),records_[n].second.end(),material.begin()+40);
    previous=sha256(material.data(),material.size()); if(previous!=chain_[n]) return false;
  }
  return true;
}
AxiomEngine::AxiomEngine(std::filesystem::path storage_root) : root_(std::move(storage_root)) {
  if (!root_.empty()) {
    std::error_code ec; std::filesystem::create_directories(root_, ec);
    if(ec) throw std::runtime_error("cannot create storage root: "+ec.message());
    std::string error;
    if(!recover(error)) throw std::runtime_error("AXIOM WAL recovery failed: "+error);
  }
  last_digest_=state_digest(state_);
}
void AxiomEngine::set_tile(std::uint64_t id, const Tile& tile) {
  auto previous=state_.find(id);
  const bool existed=previous!=state_.end();
  Tile old{}; if(existed) old=previous->second;
  state_[id]=tile;
  if(!root_.empty() && tick_==0) {
    std::string error;
    if(!persist_genesis(error)) {
      if(existed) state_[id]=old; else state_.erase(id);
      throw std::runtime_error("persist genesis failed: "+error);
    }
  }
  last_digest_=state_digest(state_);
}
bool AxiomEngine::get_tile(std::uint64_t id, Tile& out) const {
  auto it=state_.find(id); if(it==state_.end()) return false; out=it->second; return true;
}
bool AxiomEngine::build_graph(const std::vector<Task>& tasks, std::string& error) {
  tasks_.clear(); layers_.clear(); error.clear();
  std::map<std::uint64_t,std::size_t> index;
  for(std::size_t i=0;i<tasks.size();++i) {
    if(!tasks[i].execute) { error="task has no execute function"; return false; }
    if(!index.emplace(tasks[i].id,i).second) { error="duplicate task id"; return false; }
  }
  std::vector<std::size_t> indegree(tasks.size());
  std::vector<std::vector<std::size_t>> children(tasks.size());
  for(std::size_t i=0;i<tasks.size();++i) for(auto dep:tasks[i].dependencies) {
    auto it=index.find(dep);
    if(it==index.end()) { error="dependency references unknown task"; return false; }
    if(it->second==i) { error="task depends on itself"; return false; }
    ++indegree[i]; children[it->second].push_back(i);
  }
  std::vector<bool> used(tasks.size()); std::size_t processed=0;
  while(processed<tasks.size()) {
    std::vector<std::size_t> layer;
    for(std::size_t i=0;i<tasks.size();++i) if(!used[i] && indegree[i]==0) layer.push_back(i);
    if(layer.empty()) { error="dependency cycle detected"; tasks_.clear(); layers_.clear(); return false; }
    std::sort(layer.begin(),layer.end(),[&](auto a,auto b){return tasks[a].id<tasks[b].id;});
    for(std::size_t x=0;x<layer.size();++x) for(std::size_t y=x+1;y<layer.size();++y) {
      const auto& a=tasks[layer[x]]; const auto& b=tasks[layer[y]];
      for(auto tile:a.writes) if(b.writes.contains(tile) || b.reads.contains(tile)) {
        error="conflicting tile access within a DAG layer"; tasks_.clear(); layers_.clear(); return false;
      }
      for(auto tile:a.reads) if(b.writes.contains(tile)) {
        error="conflicting tile access within a DAG layer"; tasks_.clear(); layers_.clear(); return false;
      }
    }
    layers_.push_back(layer);
    for(auto i:layer) { used[i]=true; ++processed; for(auto child:children[i]) --indegree[child]; }
  }
  tasks_=tasks; return true;
}
bool AxiomEngine::persist_genesis(std::string& error) const {
  error.clear();
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(),GEN_MAGIC.begin(),GEN_MAGIC.end()); put_u64(bytes,state_.size());
  for(const auto& [id,tile]:state_) {
    put_u64(bytes,id); bytes.insert(bytes.end(),tile.begin(),tile.end());
  }
  put_u32(bytes,crc32c(bytes.data(),bytes.size()));
  const auto path=root_/"axiom.genesis";
  const auto temp=root_/"axiom.genesis.tmp";
  int fd=::open(temp.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0644);
  if(fd<0) { error="open genesis temp failed"; return false; }
  const bool wrote=write_all(fd,bytes.data(),bytes.size());
  const bool synced=wrote && ::fsync(fd)==0;
  const int closed=::close(fd);
  if(!wrote || !synced || closed!=0) { error="write/fsync genesis failed"; return false; }
  std::error_code ec; std::filesystem::rename(temp,path,ec);
  if(ec) { error="rename genesis failed: "+ec.message(); return false; }
  int dirfd=::open(root_.c_str(),O_RDONLY|O_DIRECTORY);
  if(dirfd>=0) { const bool ok=::fsync(dirfd)==0; ::close(dirfd); if(!ok) { error="fsync genesis directory failed"; return false; } }
  return true;
}
bool AxiomEngine::recover(std::string& error) {
  error.clear();
  State genesis;
  const auto genesis_path=root_/"axiom.genesis";
  if(std::filesystem::exists(genesis_path)) {
    std::ifstream gen(genesis_path,std::ios::binary);
    if(!gen) { error="cannot open genesis state"; return false; }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(gen)),{});
    constexpr std::size_t header=16, entry=8+TILE_BYTES, trailer=4;
    if(data.size()<header+trailer || !std::equal(GEN_MAGIC.begin(),GEN_MAGIC.end(),data.begin())) {
      error="invalid genesis header"; return false;
    }
    const auto count=get_u64(data.data()+8);
    if(count>(std::numeric_limits<std::size_t>::max()-header-trailer)/entry ||
       data.size()!=header+static_cast<std::size_t>(count)*entry+trailer) {
      error="invalid genesis length"; return false;
    }
    const auto n=data.size();
    const auto stored=static_cast<std::uint32_t>(data[n-4]) |
      (static_cast<std::uint32_t>(data[n-3])<<8) |
      (static_cast<std::uint32_t>(data[n-2])<<16) |
      (static_cast<std::uint32_t>(data[n-1])<<24);
    if(crc32c(data.data(),n-4)!=stored) { error="genesis CRC32C mismatch"; return false; }
    std::size_t off=header;
    for(std::uint64_t i=0;i<count;++i) {
      const auto id=get_u64(data.data()+off); off+=8;
      Tile tile{}; std::copy(data.begin()+static_cast<std::ptrdiff_t>(off),
                             data.begin()+static_cast<std::ptrdiff_t>(off+TILE_BYTES),tile.begin());
      off+=TILE_BYTES;
      if(!genesis.emplace(id,tile).second) { error="duplicate tile in genesis"; return false; }
    }
  }
  const auto path=root_/"axiom.wal";
  std::ifstream in(path,std::ios::binary);
  std::vector<std::uint8_t> bytes;
  if(in) bytes.assign(std::istreambuf_iterator<char>(in),{});
  else if(std::filesystem::exists(path)) { error="cannot open WAL"; return false; }
  std::size_t pos=0,valid_end=0;
  State recovered=genesis;
  std::uint64_t recovered_tick=0;
  ZeroVerifier recovered_verifier;
  while(pos<bytes.size()) {
    constexpr std::size_t fixed=8+8+32+32+8;
    if(bytes.size()-pos<fixed) break;
    const auto* p=bytes.data()+pos;
    if(!std::equal(WAL_MAGIC.begin(),WAL_MAGIC.end(),p)) break;
    const auto tick=get_u64(p+8);
    Digest before{},after{};
    std::copy(p+16,p+48,before.begin()); std::copy(p+48,p+80,after.begin());
    const auto count=get_u64(p+80);
    if(count>(std::numeric_limits<std::size_t>::max()-fixed-4)/(8+TILE_BYTES)) break;
    const std::size_t record_size=fixed+static_cast<std::size_t>(count)*(8+TILE_BYTES)+4;
    if(bytes.size()-pos<record_size) break;
    const auto stored_crc=static_cast<std::uint32_t>(bytes[pos+record_size-4]) |
      (static_cast<std::uint32_t>(bytes[pos+record_size-3])<<8) |
      (static_cast<std::uint32_t>(bytes[pos+record_size-2])<<16) |
      (static_cast<std::uint32_t>(bytes[pos+record_size-1])<<24);
    if(crc32c(p,record_size-4)!=stored_crc || tick!=recovered_tick+1 ||
       state_digest(recovered)!=before) break;
    State next=recovered;
    std::size_t off=pos+fixed;
    for(std::uint64_t i=0;i<count;++i) {
      const auto id=get_u64(bytes.data()+off); off+=8;
      Tile tile{}; std::copy(bytes.begin()+static_cast<std::ptrdiff_t>(off),
                             bytes.begin()+static_cast<std::ptrdiff_t>(off+TILE_BYTES),tile.begin());
      off+=TILE_BYTES; next[id]=tile;
    }
    if(state_digest(next)!=after || !recovered_verifier.record(tick,after)) break;
    recovered=std::move(next); recovered_tick=tick; pos+=record_size; valid_end=pos;
  }
  if(valid_end<bytes.size()) {
    int fd=::open(path.c_str(),O_WRONLY);
    if(fd<0) { error="cannot open torn WAL for truncation"; return false; }
    const bool ok=::ftruncate(fd,static_cast<off_t>(valid_end))==0 && ::fsync(fd)==0;
    const int close_result=::close(fd);
    if(!ok || close_result!=0) { error="failed to truncate torn WAL tail"; return false; }
  }
  state_=std::move(recovered); tick_=recovered_tick; verifier_=std::move(recovered_verifier);
  last_digest_=state_digest(state_);
  return true;
}
bool AxiomEngine::append_commit(std::uint64_t tick,const State& before_state,const State& after_state,
                                const Digest& before,const Digest& after,std::string& error) {
  if(root_.empty()) return true;
  std::vector<std::uint8_t> record;
  record.insert(record.end(),WAL_MAGIC.begin(),WAL_MAGIC.end()); put_u64(record,tick);
  record.insert(record.end(),before.begin(),before.end()); record.insert(record.end(),after.begin(),after.end());
  std::vector<std::pair<std::uint64_t,const Tile*>> changed;
  for(const auto& [id,tile]:after_state) {
    auto old=before_state.find(id);
    if(old==before_state.end() || old->second!=tile) changed.emplace_back(id,&tile);
  }
  put_u64(record,changed.size());
  for(const auto& [id,tile]:changed) {
    put_u64(record,id); record.insert(record.end(),tile->begin(),tile->end());
  }
  put_u32(record,crc32c(record.data(),record.size()));
  const auto path=root_/"axiom.wal";
  int fd=::open(path.c_str(),O_CREAT|O_WRONLY|O_APPEND,0644);
  if(fd<0) { error="open WAL failed"; return false; }
  const bool wrote=write_all(fd,record.data(),record.size());
  const bool synced=wrote && ::fsync(fd)==0;
  const int closed=::close(fd);
  if(!wrote || !synced || closed!=0) { error="WAL write/fsync/close failed"; return false; }
  return true;
}
TickResult AxiomEngine::execute_tick() {
  TickResult result{}; result.tick=tick_+1;
  const State original=state_; const auto before=state_digest(original); State working=original;
  try {
    for(const auto& layer:layers_) {
      const State snapshot=working; State writes;
      for(auto idx:layer) {
        State produced; tasks_[idx].execute(snapshot,produced);
        for(const auto& [tile,data]:produced) {
          if(!tasks_[idx].writes.contains(tile)) throw std::runtime_error("task wrote an undeclared tile");
          if(writes.contains(tile)) throw std::runtime_error("two tasks wrote the same tile");
          writes.emplace(tile,data);
        }
      }
      for(auto& [tile,data]:writes) working[tile]=data;
    }
    const auto after=state_digest(working); std::string error;
    if(!append_commit(tick_+1,original,working,before,after,error)) throw std::runtime_error(error);
    if(!verifier_.record(tick_+1,after)) throw std::runtime_error("verifier rejected tick sequence");
    state_=std::move(working); ++tick_; last_digest_=after;
    result.ok=true; result.tick=tick_; result.digest=after; return result;
  } catch(const std::exception& e) { result.error=e.what(); result.digest=before; return result; }
}
} // namespace axiom

```

## Tests

```cpp
#include "axiom/axiom_core.hpp"
#include "axiom/tile_cache.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace axiom;
int main() {
  auto root=std::filesystem::temp_directory_path()/"axiom-core-test";
  std::filesystem::remove_all(root);
  AxiomEngine engine(root);
  Tile initial{}; initial[0]=3; engine.set_tile(1,initial);
  Task a{10,{}, {}, {1}, [](const State& in, State& out){ auto t=in.at(1); t[0]++; out[1]=t; }};
  Task b{20,{10}, {1}, {2}, [](const State& in, State& out){ Tile t{}; t[0]=static_cast<std::uint8_t>(in.at(1)[0]*2); out[2]=t; }};
  std::string error; assert(engine.build_graph({b,a},error));
  auto r=engine.execute_tick(); assert(r.ok && r.tick==1 && engine.verify());
  Tile got{}; assert(engine.get_tile(1,got) && got[0]==4); assert(engine.get_tile(2,got) && got[0]==8);
  {
    AxiomEngine restarted(root);
    assert(restarted.current_tick()==1 && restarted.verify());
    Tile persisted{}; assert(restarted.get_tile(1,persisted) && persisted[0]==4);
    assert(restarted.get_tile(2,persisted) && persisted[0]==8);
  }
  {
    std::ofstream torn(root/"axiom.wal",std::ios::binary|std::ios::app);
    torn.write("AXW",3);
  }
  {
    AxiomEngine repaired(root);
    assert(repaired.current_tick()==1 && repaired.verify());
  }
  Task bad{30,{}, {}, {1}, [](const State&, State&){ throw std::runtime_error("intentional failure"); }};
  assert(engine.build_graph({bad},error)); auto before=engine.state().at(1); auto failed=engine.execute_tick();
  assert(!failed.ok && engine.current_tick()==1 && engine.state().at(1)==before);
  Task c{40,{41},{},{},[](const State&,State&){}}; Task d{41,{40},{},{},[](const State&,State&){ }};
  assert(!engine.build_graph({c,d},error)); assert(error.find("cycle")!=std::string::npos);
  TileCache cache(2); Tile x{},y{},z{}; x[0]=1; y[0]=2; z[0]=3;
  assert(cache.put(1,x) && cache.put(2,y));
  assert(cache.pin(1)); assert(cache.get(2,got) && got[0]==2);
  assert(cache.put(3,z)); assert(!cache.contains(2) && cache.contains(1) && cache.contains(3));
  assert(cache.pin(3)); assert(!cache.put(4,x));
  assert(cache.unpin(1)); assert(cache.put(4,x));
  assert(cache.size()==2 && cache.contains(3) && cache.contains(4));
  std::cout<<"AXIOM core tests passed\\n";
}

```

## GitHub Actions workflow

```yaml
name: AXIOM C++20 Build
on:
  push:
    branches: [main]
    paths:
      - 'engine/**'
      - '.github/workflows/axiom-cpp.yml'
  pull_request:
    paths:
      - 'engine/**'
      - '.github/workflows/axiom-cpp.yml'
  workflow_dispatch:
permissions:
  contents: read
jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install C++ build dependencies
        run: sudo apt-get update && sudo apt-get install --yes cmake g++ libssl-dev
      - name: Configure
        run: cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
      - name: Compile
        run: cmake --build engine/build --parallel 2
      - name: Test
        run: ctest --test-dir engine/build --output-on-failure
      - name: Upload test executable
        uses: actions/upload-artifact@v4
        with:
          name: axiom-cpp-core-linux
          path: engine/build/test_axiom

```

