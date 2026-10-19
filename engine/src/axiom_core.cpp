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
