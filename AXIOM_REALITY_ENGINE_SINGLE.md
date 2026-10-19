# AXIOM Reality Engine: Single-File Source Bundle

This Markdown file consolidates the current AXIOM C++20 core source, build configuration, test source, and CI workflow from `auctoroaa/Solvor`.

## Build status

GitHub Actions run: https://github.com/auctoroaa/Solvor/actions/runs/39340220134

The C++ target compiled and its current test suite passed in that run. The artifact is available from the run's **Artifacts** section as `axiom-cpp-core-linux`.

## Scope

This is the currently compiled core, not the entire originally specified production engine. Implemented in this bundle: deterministic DAG layering, stable task order, declared tile-access conflict checks, transactional in-memory tick updates, rollback on task exceptions, SHA-256 state digests, hash-chain verification, and fsync-backed commit records.

Still not implemented in this bundle: bounded LRU/pinning TileCache, full event WAL and segment rotation, replay/torn-write repair, persisted tile state and clean-tick recovery, and the optional liburing backend. Those are not claimed as completed.

## Build and test

Requires a Linux environment with C++20 compiler, CMake 3.20+, and OpenSSL development headers.

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --parallel 2
ctest --test-dir engine/build --output-on-failure
```

When extracting the sections below, save each code block to its stated repository path. This is one Markdown document containing the current source, not a binary executable.

## Build configuration

**Repository path:** `engine/CMakeLists.txt`

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

## Public API

**Repository path:** `engine/include/axiom/axiom_core.hpp`

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
  bool append_commit(std::uint64_t tick, const Digest& before, const Digest& after, std::string& error);
};
} // namespace axiom
```

## Implementation

**Repository path:** `engine/src/axiom_core.cpp`

```cpp
#include "axiom/axiom_core.hpp"
#include <algorithm>
#include <fcntl.h>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace axiom {
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
  }
  last_digest_=state_digest(state_);
}
void AxiomEngine::set_tile(std::uint64_t id, const Tile& tile) { state_[id]=tile; last_digest_=state_digest(state_); }
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
bool AxiomEngine::append_commit(std::uint64_t tick,const Digest& before,const Digest& after,std::string& error) {
  if(root_.empty()) return true;
  const auto path=root_/"commits.log";
  int fd=::open(path.c_str(),O_CREAT|O_WRONLY|O_APPEND,0644);
  if(fd<0) { error="open commit log failed"; return false; }
  std::ostringstream line; line<<tick<<' '<<hex(before)<<' '<<hex(after)<<'\n'; auto s=line.str();
  std::size_t done=0;
  while(done<s.size()) {
    auto n=::write(fd,s.data()+done,s.size()-done);
    if(n<0) { ::close(fd); error="write commit log failed"; return false; }
    done+=static_cast<std::size_t>(n);
  }
  if(::fsync(fd)!=0) { ::close(fd); error="fsync commit log failed"; return false; }
  if(::close(fd)!=0) { error="close commit log failed"; return false; }
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
    if(!append_commit(tick_+1,before,after,error)) throw std::runtime_error(error);
    if(!verifier_.record(tick_+1,after)) throw std::runtime_error("verifier rejected tick sequence");
    state_=std::move(working); ++tick_; last_digest_=after;
    result.ok=true; result.tick=tick_; result.digest=after; return result;
  } catch(const std::exception& e) { result.error=e.what(); result.digest=before; return result; }
}
} // namespace axiom
```

## Tests

**Repository path:** `engine/tests/test_axiom.cpp`

```cpp
#include "axiom/axiom_core.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
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
  Task bad{30,{}, {}, {1}, [](const State&, State&){ throw std::runtime_error("intentional failure"); }};
  assert(engine.build_graph({bad},error)); auto before=engine.state().at(1); auto failed=engine.execute_tick();
  assert(!failed.ok && engine.current_tick()==1 && engine.state().at(1)==before);
  Task c{40,{41},{},{},[](const State&,State&){}}; Task d{41,{40},{},{},[](const State&,State&){ }};
  assert(!engine.build_graph({c,d},error)); assert(error.find("cycle")!=std::string::npos);
  std::cout<<"AXIOM core tests passed\n";
}
```

## GitHub Actions build

**Repository path:** `.github/workflows/axiom-cpp.yml`

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
