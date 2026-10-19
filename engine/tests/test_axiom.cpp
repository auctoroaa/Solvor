#include "axiom/axiom_core.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>\n#include <fstream>
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
  std::cout<<"AXIOM core tests passed\n";
}
