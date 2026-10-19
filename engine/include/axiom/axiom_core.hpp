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
  bool append_commit(std::uint64_t tick, const State& before_state, const State& after_state,
                     const Digest& before, const Digest& after, std::string& error);
};
} // namespace axiom
