// SPDX-License-Identifier: MPL-2.0
#include "foods.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "utils.h"

using namespace std;
using json = nlohmann::json;

namespace {

const int INF = INT_MAX / 2;

struct Slot {
  int count;  // 0: not used up (a HarvestCraft tool, a GT circuit or mold), for a fluid it's mB
  vector<int> alts;
};
struct Out {
  int key, amount, chance;  // chance in 1/10000
};
struct Recipe {
  int type;  // index into types
  int eut;
  int w = 0, h = 0;  // a shaped crafting grid (its slots row by row, empty cells have no alternatives), 0 if shapeless
  vector<Slot> in;
  vector<Out> out;
};
struct Food {
  string name;
  int hunger;
  double saturation;
};

// every key the recipes or foods mention. a key "mod:name:32767" stands for any damage of mod:name, so every
// key also knows its item (name_of, -1 for fluids) and whether it's that wildcard
unordered_map<string, int> key_index;
vector<string> keys;
vector<int> name_of;
vector<bool> wild;
unordered_map<string, int> name_index;

vector<string> types;
vector<bool> type_free;  // crafting table, furnace: needs no machine
vector<Recipe> recipes;
map<int, Food> foods;  // key -> food
map<int, pair<int, vector<int>>> machine_maps;  // GT mID -> (tier, recipe map types), tier -1 for multiblocks
FoodNamer namer;
map<string, string> fluid_names;  // texture-dump's fluids.json
map<string, string> machine_names;  // texture-dump's recipe_maps.json: GT recipe map -> its name in game
unordered_map<string, int> icon_of;  // texture-dump's item_icons.json: key -> cell in item_icons.png
int icon_size = 16, icon_columns = 0;
string icons_png;

// per key: a GregTech programmed circuit. free as a machine's setting (a slot that isn't used up), but a recipe
// that uses one up needs a real one
vector<bool> circuit_key;

// the icons of the keys a response names, and where they are in the sheet
json icons_json(const set<int> &named) {
  json icons = json::object();
  for (int k : named) {
    auto f = icon_of.find(keys[k]);
    // "any damage": the plain damage 0 picture
    // "any damage": the plain damage 0 picture (not for a circuit, circuit 0's picture would show the wrong number)
    if (f == icon_of.end() && wild[k]) f = icon_of.find(keys[k].substr(0, keys[k].rfind(':')) + ":0");
    if (f != icon_of.end()) icons[keys[k]] = f->second;
  }
  return {{"size", icon_size}, {"columns", icon_columns}, {"cells", icons}};
}
bool loaded = false;

vector<vector<int>> made_by;     // key -> recipes with it as an output
vector<int> used_in;             // key -> how many recipes take it
// per recipe: it takes something apart (its input is itself made from what it gives: melting a polyethylene rod,
// emptying a cell, macerating a machine). listed last, never a route's way to a held thing's worth
vector<bool> recycling;
// per recipe: a plan may use it. not one scrapping something assembled (macerating a machine or a conveyor: it
// destroys what took several things to make). turning a material from one form into another is fine (melting
// an ingot, macerating a rod), and so is emptying a container (a cell, can, capsule in the fluid canner)
vector<bool> plannable;
// per recipe type: the GT machines that run it, as items (key, tier, multiblock), so a plan can build one
struct MachineItem { int key, tier; bool multi; };
vector<vector<MachineItem>> machine_items;
set<int> multi_keys;
vector<vector<int>> keys_of_name;
vector<string> search_names;     // key -> lowercase display name, built on the first search

mutex state_lock;
map<string, pair<uint64_t, shared_ptr<struct Solved>>> solve_cache;  // group -> (generation, solve)
FoodSnapshot snapshot;
uint64_t generation = 0, cached_generation = ~0ull;
string cached;

int intern(const string &k) {
  auto f = key_index.find(k);
  if (f != key_index.end()) return f->second;
  int i = keys.size();
  key_index.emplace(k, i);
  keys.push_back(k);
  int name = -1;
  bool w = false;
  if (k.compare(0, 6, "fluid:") != 0) {
    size_t c = k.rfind(':');
    string n = k.substr(0, c);
    auto ni = name_index.find(n);
    if (ni == name_index.end()) ni = name_index.emplace(n, (int)name_index.size()).first;
    name = ni->second;
    w = k.compare(c + 1, string::npos, "32767") == 0;
  }
  name_of.push_back(name);
  wild.push_back(w);
  return i;
}

int type_of(const string &t) {
  for (int i = 0; i < (int)types.size(); i++)
    if (types[i] == t) return i;
  types.push_back(t);
  type_free.push_back(t == "crafting" || t == "smelting" || t == "fill");
  return types.size() - 1;
}

// the voltage tier a recipe's EU/t needs: ULV 8, LV 32, MV 128... (V = 8 * 4^tier)
int tier_for(int eut) {
  int t = 0;
  while (t < 15 && ((int64_t)8 << (2 * t)) < eut) t++;
  return t;
}

string display(int k) {
  auto f = foods.find(k);
  if (f != foods.end()) return f->second.name;
  const string &key = keys[k];
  if (key.compare(0, 6, "fluid:") == 0) {
    if (auto f = fluid_names.find(key.substr(6)); f != fluid_names.end()) return f->second;
    // no fluids.json: "potion.vodka" -> "Vodka", "molten.steel" -> "Molten Steel"
    string n = key.substr(6);
    if (n.compare(0, 7, "potion.") == 0) n = n.substr(7);
    for (size_t i = 0; i < n.size(); i++) {
      if (n[i] == '.' || n[i] == '_') n[i] = ' ';
      if (i == 0 || n[i - 1] == ' ') n[i] = toupper(n[i]);
    }
    return n;
  }
  if (wild[k]) return "any " + namer(key.substr(0, key.rfind(':')) + ":0");
  return namer(key);
}

struct Solved {
  bool build = false;  // may plan building machines the group doesn't have
  vector<bool> held;   // per key: really in the group's storage (not just free: water, circuits)
  vector<bool> free;   // per key: costs nothing: water, the group's free fluids (steam with a boiler)
  vector<int> dist, best, any;  // per key: steps from what's held, the recipe that does it. per item: best over damages
  vector<int> tier;             // per recipe type: the best voltage tier the group can run it at, -1 for none
  // per key: how much of the group's holdings one of it (one mB for a fluid) uses up on its cheapest route, and
  // that route's recipe (-1 when held). per item: the cheapest over damages. INFINITY when unreachable
  vector<double> cost, cost_any;
  vector<int> cheapest;
  // per key: what one held unit is worth (0 for what isn't held). a held thing that can be made is worth what
  // making another would cost, so a route doesn't use up something valuable (recycle a held machine for its dust)
  vector<double> value;
  // per key: how many recipe steps its thriftiest route takes (0 when held), per item the fewest over damages
  vector<int> steps, steps_any;
  // per key: what its thriftiest route costs once, however much is made (machines to build, tools to make)
  vector<double> setup, setup_any;
};

int cost_of(const Solved &s, int alt) { return wild[alt] ? s.any[name_of[alt]] : s.dist[alt]; }
// a slot's alternative can be used: a circuit only as a setting, unless one is really held
bool usable(const Solved &s, const Slot &slot, int alt) { return !circuit_key[alt] || slot.count == 0 || s.held[alt]; }
double raw_of(const Solved &s, int alt) { return wild[alt] ? s.cost_any[name_of[alt]] : s.cost[alt]; }

// what a held thing is worth: an item 1, a fluid's mB 1/144 (an ingot is 144 mB molten). every run of a recipe
// also costs a little, so of two equally thrifty routes the shorter wins
const double ITEM = 1, MB = 1.0 / 144, PER_RUN = 0.05, PRECIOUS = 16;
// what a route is scored by: what it uses up, made worse by how many steps it takes (each step 3%), so using
// something held beats a long way to an equivalent, and a much longer route has to save a lot to win
const double STEP = 0.03;
double score(double cost, int steps) { return cost * (1 + STEP * steps); }

bool owns(const Solved &s, const Recipe &rc) {
  return s.tier[rc.type] >= 0 && (rc.eut <= 0 || tier_for(rc.eut) <= s.tier[rc.type]);
}
// a multiblock controller is only part of the machine, the structure around it costs a lot more
const double MULTIBLOCK = 4;
// the machine to build for a recipe the group can't run: the cheapest reachable one of the right voltage, -1 if
// none (or not building). by step count when the costs aren't known yet
int machine_to_build(const Solved &s, const Recipe &rc) {
  if (!s.build) return -1;
  int need = rc.eut > 0 ? tier_for(rc.eut) : 0, best = -1;
  double bc = INFINITY;
  for (auto &m : machine_items[rc.type]) {
    if (m.tier < need || s.dist[m.key] >= INF) continue;
    double c = s.cost.empty() || s.setup.empty() ? s.dist[m.key]
             : s.dist[m.key] == 0 ? 0 : (s.cost[m.key] + s.setup[m.key]) * (m.multi ? MULTIBLOCK : 1);
    if (c < bc) bc = c, best = m.key;
  }
  return best;
}
bool can_run(const Solved &s, const Recipe &rc) { return owns(s, rc) || machine_to_build(s, rc) >= 0; }
bool is_multi(int key) { return multi_keys.count(key) > 0; }

// what one run of recipe r uses up, in held-material terms, INFINITY if a slot can't be had. a tool (a slot
// that isn't used up) costs nothing when held, otherwise what making one costs: charged every run, which only
// matters to the ranking (a plan makes it once)
double run_cost(const Solved &s, const Recipe &rc) {
  double c = PER_RUN;
  for (auto &slot : rc.in) {
    if (slot.alts.empty()) continue;
    double b = INFINITY;
    for (int a : slot.alts)
      if (usable(s, slot, a)) b = min(b, slot.count == 0 ? (cost_of(s, a) < INF ? 0.0 : INFINITY) : raw_of(s, a));
    if (b == INFINITY) return INFINITY;
    c += slot.count * b;  // a tool (count 0) is setup, not per run
  }
  // a machine to build is setup too, it just has to be buildable
  if (!owns(s, rc) && machine_to_build(s, rc) < 0) return INFINITY;
  return c;
}
double setup_of(const Solved &s, int alt) { return wild[alt] ? s.setup_any[name_of[alt]] : s.setup[alt]; }
// what one route through rc costs once: the setup of what its inputs are made with, a tool that isn't held
// (making one), a machine the group doesn't have (building one, a multiblock's controller counted MULTIBLOCK
// times for its structure)
double run_setup(const Solved &s, const Recipe &rc) {
  double su = 0;
  for (auto &slot : rc.in) {
    if (slot.alts.empty()) continue;
    double b = INFINITY, bs = 0;
    for (int a : slot.alts) {
      if (!usable(s, slot, a) || cost_of(s, a) >= INF) continue;
      double c = slot.count == 0 ? (cost_of(s, a) == 0 ? 0.0 : raw_of(s, a) + setup_of(s, a)) : raw_of(s, a);
      if (c < b) b = c, bs = slot.count == 0 ? c : (cost_of(s, a) == 0 ? 0.0 : setup_of(s, a));
    }
    su += bs;
  }
  if (!owns(s, rc))
    if (int m = machine_to_build(s, rc); m >= 0 && s.dist[m] != 0) su += (s.cost[m] + s.setup[m]) * (is_multi(m) ? MULTIBLOCK : 1);
  return su;
}
int steps_of(const Solved &s, int alt) { return wild[alt] ? s.steps_any[name_of[alt]] : s.steps[alt]; }
// how many steps a route through rc takes: one, plus what each input takes (from the alternative run_cost picks),
// plus building its machine. added up, a plan's effort is all of its steps, not its longest branch (something
// used twice counts twice, it only ranks routes). capped, so a deep tree can't run away
int run_steps(const Solved &s, const Recipe &rc) {
  int st = 0;
  for (auto &slot : rc.in) {
    if (slot.alts.empty()) continue;
    double b = INFINITY;
    int bs = 0;
    for (int a : slot.alts) {
      if (!usable(s, slot, a)) continue;
      double c = slot.count == 0 && cost_of(s, a) == 0 ? 0.0 : raw_of(s, a);
      if (c < b) b = c, bs = cost_of(s, a) == 0 ? 0 : steps_of(s, a);
    }
    st += bs;
  }
  if (!owns(s, rc))
    if (int m = machine_to_build(s, rc); m >= 0 && s.dist[m] != 0) st += s.steps[m];
  return min(st + 1, 200);
}
// how much of key k one run of rc makes, on average
double made_per_run(const Recipe &rc, int k) {
  double n = 0;
  for (auto &o : rc.out)
    if (o.key == k) n += o.amount * o.chance / 10000.0;
  return n;
}

// what a route through rc scores, per unit of k
// how much a route is judged for when the amount isn't known (deeper in a plan): a stack, 64 items or 64 ingots
// of fluid. so a one time cost (a machine) counts against a fair amount, not just one
double typical(int k) { return keys[k].compare(0, 6, "fluid:") == 0 ? 64 * 144 : 64; }
double total(double unit, double setup, int steps, double qty) { return score(unit * qty + setup, steps); }
// what a route through rc scores for qty of k (typical(k) when 0): what it uses up for that much, plus its setup
// once, made worse by its steps
double route_score(const Solved &s, const Recipe &rc, int k, double qty = 0) {
  double per = made_per_run(rc, k), c = run_cost(s, rc);
  if (per <= 0 || c == INFINITY) return INFINITY;
  return total(c / per, run_setup(s, rc), run_steps(s, rc), qty > 0 ? qty : typical(k));
}

// how many recipe steps each key is from the group's holdings. a recipe works when its machine is owned (with
// the voltage) and every slot has an alternative that's reachable, its outputs are one step past its slowest
// slot. counts are ignored: holding one of an ingredient is enough, so "craftable" can mean "once"
Solved solve(const FoodGroup &g, bool build) {
  Solved s;
  s.build = build;
  s.dist.assign(keys.size(), INF);
  s.best.assign(keys.size(), -1);
  s.any.assign(name_index.size(), INF);
  auto reach = [&](int k, int d, int r) {
    if (d >= s.dist[k]) return false;
    s.dist[k] = d;
    s.best[k] = r;
    if (name_of[k] >= 0 && d < s.any[name_of[k]]) s.any[name_of[k]] = d;
    return true;
  };
  s.held.assign(keys.size(), false);
  for (auto &[key, count] : g.held) {
    if (count <= 0) continue;
    auto k = key_index.find(key);
    if (k != key_index.end()) {
      reach(k->second, 0, -1);
      s.held[k->second] = true;
    }
    // a held damage no recipe names exactly still satisfies "any damage"
    auto n = name_index.find(key.substr(0, key.rfind(':')));
    if (n != name_index.end()) s.any[n->second] = 0;
  }
  // water is free: a pump, a water source next to the machine
  s.free.assign(keys.size(), false);
  set<string> free_fluids = g.free_fluids;
  free_fluids.insert("water");
  for (auto &f : free_fluids)
    if (auto w = key_index.find("fluid:" + f); w != key_index.end()) {
      reach(w->second, 0, -1);
      s.free[w->second] = true;
    }
  // so is a GregTech programmed circuit: a machine's circuit slot is set from its GUI, no item needed
  if (auto c = name_index.find("gregtech:gt.integrated_circuit"); c != name_index.end()) {
    s.any[c->second] = 0;
    for (int k = 0; k < (int)keys.size(); k++)
      if (name_of[k] == c->second) reach(k, 0, -1);
  }

  // the best tier owned per recipe map
  vector<int> &tier = s.tier;
  tier.assign(types.size(), -1);
  for (size_t t = 0; t < types.size(); t++)
    if (type_free[t]) tier[t] = 15;
  for (int mid : g.machines) {
    auto m = machine_maps.find(mid);
    if (m == machine_maps.end()) continue;
    int t = m->second.first < 0 ? 15 : m->second.first;
    for (int type : m->second.second) tier[type] = max(tier[type], t);
  }

  for (int pass = 0; pass < 256; pass++) {
    bool changed = false;
    for (int r = 0; r < (int)recipes.size(); r++) {
      const Recipe &rc = recipes[r];
      if (!plannable[r]) continue;
      int worst = 0;
      if (!owns(s, rc)) {
        int m = machine_to_build(s, rc);
        if (m < 0) continue;
        worst = s.dist[m];
      }
      for (auto &slot : rc.in) {
        if (slot.alts.empty()) continue;  // an empty grid cell
        int best = INF;
        for (int a : slot.alts)
          if (usable(s, slot, a)) best = min(best, cost_of(s, a));
        worst = max(worst, best);
        if (worst >= INF) break;
      }
      if (worst >= INF) continue;
      for (auto &o : rc.out) changed |= reach(o.key, worst + 1, r);
    }
    if (!changed) break;
  }

  // then the thriftiest route to each reachable key: keep relaxing "a run's inputs / what it makes" until
  // nothing gets cheaper. what's held stays a leaf at its value, use what you have before making more of it
  auto circuit = name_index.find("gregtech:gt.integrated_circuit");
  auto base = [&](int k) {
    // water and programmed circuits cost nothing (see above), an item 1, a fluid's mB 1/144
    if (s.free[k] || (circuit != name_index.end() && name_of[k] == circuit->second)) return 0.0;
    return keys[k].compare(0, 6, "fluid:") == 0 ? MB : ITEM;
  };
  vector<int> runnable;
  for (int r = 0; r < (int)recipes.size(); r++) {
    const Recipe &rc = recipes[r];
    if (!plannable[r] || (!owns(s, rc) && machine_to_build(s, rc) < 0)) continue;
    bool useful = false;
    for (auto &o : rc.out) useful |= s.dist[o.key] > 0 && s.dist[o.key] < INF;
    if (useful) runnable.push_back(r);
  }
  auto cheaper = [&](int k, double c, int r, int st, double su) {
    // judged as a whole for a typical amount: a cheap unit cost behind a big setup doesn't win a small job
    if (s.cost[k] < INFINITY && !(total(c, su, st, typical(k)) < total(s.cost[k], s.setup[k], s.steps[k], typical(k)) * (1 - 1e-9))) return false;
    s.cost[k] = c;
    s.cheapest[k] = r;
    s.steps[k] = st;
    s.setup[k] = su;
    if (name_of[k] >= 0) {
      s.cost_any[name_of[k]] = min(s.cost_any[name_of[k]], c);
      s.steps_any[name_of[k]] = min(s.steps_any[name_of[k]], st);
      s.setup_any[name_of[k]] = min(s.setup_any[name_of[k]], su);
    }
    return true;
  };
  auto relax = [&]() {
    s.cost.assign(keys.size(), INFINITY);
    s.cost_any.assign(name_index.size(), INFINITY);
    s.cheapest.assign(keys.size(), -1);
    s.steps.assign(keys.size(), 1 << 20);
    s.steps_any.assign(name_index.size(), 1 << 20);
    s.setup.assign(keys.size(), 0);
    s.setup_any.assign(name_index.size(), 0);
    for (int k = 0; k < (int)keys.size(); k++)
      if (s.dist[k] == 0) cheaper(k, s.value[k], -1, 0, 0);
    for (size_t n = 0; n < name_index.size(); n++)
      if (s.any[n] == 0) {
        if (s.cost_any[n] == INFINITY) s.cost_any[n] = ITEM;  // held under a damage no recipe names
        s.steps_any[n] = 0;
      }
    for (int pass = 0; pass < 200; pass++) {
      bool changed = false;
      for (int r : runnable) {
        const Recipe &rc = recipes[r];
        double c = run_cost(s, rc);
        if (c == INFINITY) continue;
        for (auto &o : rc.out) {
          if (s.dist[o.key] == 0) continue;
          double per = made_per_run(rc, o.key);
          if (per > 0) changed |= cheaper(o.key, c / per, r, run_steps(s, rc), run_setup(s, rc));
        }
      }
      if (!changed) break;
    }
  };
  // first everything held at its plain worth
  s.value.assign(keys.size(), 0);
  for (int k = 0; k < (int)keys.size(); k++)
    if (s.dist[k] == 0) s.value[k] = base(k);
  relax();
  // then what held things that can be made are worth: what making one more costs from the rest (never less than
  // plain), or PRECIOUS when it can't be made now (use it if needed, but not to save a little elsewhere)
  vector<double> value = s.value;
  for (int k = 0; k < (int)keys.size(); k++) {
    if (s.dist[k] != 0 || value[k] == 0 || made_by[k].empty()) continue;
    double best = INFINITY;
    for (int r : made_by[k]) {
      const Recipe &rc = recipes[r];
      if (!can_run(s, rc) || recycling[r]) continue;
      double per = made_per_run(rc, k), c = run_cost(s, rc);
      if (per > 0 && c < INFINITY) best = min(best, c / per);
    }
    value[k] = best == INFINITY ? PRECIOUS * base(k) : max(base(k), best);
  }
  s.value = value;
  relax();
  return s;
}

bool machine_ok(const Solved &s, const Recipe &rc) { return can_run(s, rc); }

json recipe_json(int r, set<int> &named) {
  const Recipe &rc = recipes[r];
  json in = json::array(), o = json::array();
  for (auto &slot : rc.in) in.push_back(slot.count);
  for (auto &x : rc.out) {
    o.push_back({keys[x.key], x.amount, x.chance});
    named.insert(x.key);
  }
  json j = {{"t", types[rc.type]}, {"eut", rc.eut}, {"tier", rc.eut > 0 ? tier_for(rc.eut) : -1}, {"in", in}, {"out", o}};
  if (rc.w > 0) j["w"] = rc.w, j["h"] = rc.h;
  if (auto m = machine_names.find(types[rc.type]); m != machine_names.end()) j["machine"] = m->second;
  if (recycling[r]) j["recycling"] = true;
  return j;
}

// the thriftiest way to key k: the recipe, and which alternative each slot uses, then the same for those, into
// nodes (key -> [steps] when held, [steps, recipe, [used keys]] when made, [steps, -1, [key]] for "any damage"
// made as a specific one). the thriftiest picks can loop (x is cheapest made from y, y from x), so this tries the
// options cheapest first and backs out of one that would need something it's still making higher up
struct TreeBuild {
  const Solved &s;
  json &nodes, &jr;
  set<int> &named, &seen;
  set<int> stack;
  vector<int> added;  // keys given a node, in order, to back out
  bool thrifty = true;
  int budget = 50000;
  // held things there's too little of and no way to make more: not used, so the plan backs out to another way
  const set<int> *scarce = nullptr;
  // what the plan is for: its top recipe is judged for this amount, not a typical one
  int root = -1;
  double root_qty = 0;

  void put(int k, json node) {
    nodes[keys[k]] = move(node);
    seen.insert(k);
    added.push_back(k);
  }
  void back_to(size_t n) {
    while (added.size() > n) {
      nodes.erase(keys[added.back()]);
      seen.erase(added.back());
      added.pop_back();
    }
  }
  // the recipes worth trying for k, cheapest first (fewest steps: just the one the step solve found)
  vector<int> candidates(int k) {
    if (!thrifty) return s.best[k] >= 0 ? vector<int>{s.best[k]} : vector<int>{};
    vector<pair<double, int>> c;
    for (int r : made_by[k]) {
      double per = made_per_run(recipes[r], k);
      if (per <= 0 || !plannable[r] || !machine_ok(s, recipes[r])) continue;
      // the top of the plan is judged for the amount asked, deeper ones for a typical amount
      double sc = route_score(s, recipes[r], k, k == root ? root_qty : 0);
      if (sc < INFINITY) c.emplace_back(sc, r);
    }
    sort(c.begin(), c.end());
    vector<int> out;
    for (auto &[cost, r] : c) out.push_back(r);
    return out;
  }
  vector<int> alts(const Slot &slot) {
    vector<int> a;
    for (int x : slot.alts)
      if (cost_of(s, x) < INF && usable(s, slot, x)) a.push_back(x);
    if (thrifty)
      stable_sort(a.begin(), a.end(), [&](int x, int y) {
        // held counts as no steps: use what there is before making an equivalent
        auto sc = [&](int a) {
          bool held = cost_of(s, a) == 0;
          return total(raw_of(s, a), held ? 0 : setup_of(s, a), held ? 0 : steps_of(s, a), wild[a] ? 64 : typical(a));
        };
        double sx = sc(x), sy = sc(y);
        return sx < sy || (sx == sy && cost_of(s, x) < cost_of(s, y));
      });
    else
      stable_sort(a.begin(), a.end(), [&](int x, int y) { return cost_of(s, x) < cost_of(s, y); });
    return a;
  }
  // make k with recipe r: every slot from its cheapest alternative that works
  bool with(int k, int r, int d) {
    size_t mark = added.size();
    json ju = json::array();
    for (auto &slot : recipes[r].in) {
      if (slot.alts.empty()) {
        ju.push_back(nullptr);
        continue;
      }
      bool ok = false;
      for (int a : alts(slot))
        if (add(a)) {
          ju.push_back(keys[a]);
          ok = true;
          break;
        }
      if (!ok) {
        back_to(mark);
        return false;
      }
    }
    json node = {max(d, 1), r, ju};
    // a machine the group has to build first: made like a tool, the step says it runs on it
    if (!owns(s, recipes[r])) {
      int m = machine_to_build(s, recipes[r]);
      if (m < 0 || !add(m)) {
        back_to(mark);
        return false;
      }
      node.push_back({{"machine", keys[m]}});
    }
    put(k, node);
    if (!jr.contains(to_string(r))) jr[to_string(r)] = recipe_json(r, named);
    return true;
  }
  // give k a node, false if every way to it needs something being made higher up. make: k is held but not
  // enough, make it anyway (the node says how much is held, the plan makes the rest)
  bool add(int k, int force = -1, bool make = false) {
    if (seen.count(k)) return true;
    if (stack.count(k) || --budget < 0) return false;
    named.insert(k);
    int d = cost_of(s, k);
    if (d >= INF) {
      put(k, {nullptr});
      return true;
    }
    if (d == 0 && force < 0 && !make && scarce && scarce->count(k)) return false;
    if (d == 0 && force < 0 && !make) {
      put(k, {0});
      return true;
    }
    stack.insert(k);
    bool ok = false;
    if (wild[k]) {
      vector<int> ks;
      for (int k2 : keys_of_name[name_of[k]])
        if (!wild[k2] && s.dist[k2] < INF) ks.push_back(k2);
      stable_sort(ks.begin(), ks.end(), [&](int x, int y) {
        auto sc = [&](int a) { return s.dist[a] == 0 ? total(s.cost[a], 0, 0, typical(a)) : total(s.cost[a], s.setup[a], s.steps[a], typical(a)); };
        return thrifty ? sc(x) < sc(y) : s.dist[x] < s.dist[y];
      });
      for (int k2 : ks)
        if (add(k2)) {
          put(k, {d, -1, json::array({keys[k2]})});
          ok = true;
          break;
        }
    } else {
      for (int r : force >= 0 ? vector<int>{force} : candidates(k))
        if ((ok = with(k, r, d))) break;
    }
    if (ok && make) {
      json &n = nodes[keys[k]];
      if (n.size() < 4) n.push_back(json::object());
      n[3]["partial"] = true;
    }
    stack.erase(k);
    return ok;
  }
};

void add_tree(const Solved &s, int k, json &nodes, json &jr, set<int> &named, set<int> &seen, int force = -1) {
  TreeBuild b{s, nodes, jr, named, seen};
  size_t mark = 0;
  if (b.add(k, force)) return;
  // gave up (out of budget, or nothing works without looping): the fewest steps never loops
  b.back_to(mark);
  b.thrifty = false;
  b.budget = INT_MAX;
  b.stack.clear();
  b.add(k, force);
}

// how much of everything a plan needs, the same sums the page does: the made things in the order to make them
// (each after what it needs), and per key how much is needed. a node made because too little is held
// ("partial") uses all that's held, the rest is made
struct Needs {
  vector<string> order;
  map<string, double> need;
  set<string> tools;
};
Needs plan_needs(const json &nodes, const json &jr, const string &target, double qty, const map<string, int64_t> &have) {
  Needs p;
  auto canon = [&](string k) {
    for (int i = 0; i < 64; i++) {
      auto n = nodes.find(k);
      if (n == nodes.end() || n->size() < 3 || (*n)[1].get<int>() != -1) break;
      k = (*n)[2][0].get<string>();
    }
    return k;
  };
  auto made = [&](const string &k) {
    auto n = nodes.find(k);
    return n != nodes.end() && n->size() >= 3 && !(*n)[0].is_null() && (*n)[1].get<int>() >= 0;
  };
  set<string> seen;
  auto machine = [&](const json &n) { return n.size() >= 4 && n[3].contains("machine") ? n[3]["machine"].get<string>() : string(); };
  function<void(const string &)> dfs = [&](const string &k0) {
    string k = canon(k0);
    if (!seen.insert(k).second || !made(k)) return;
    const json &n = nodes[k];
    if (string m = machine(n); !m.empty()) dfs(m);
    for (auto &u : n[2])
      if (!u.is_null()) dfs(u.get<string>());
    p.order.push_back(k);
  };
  dfs(target);
  p.need[canon(target)] = qty;
  for (auto it = p.order.rbegin(); it != p.order.rend(); ++it) {
    const json &n = nodes[*it];
    const json &rc = jr[to_string(n[1].get<int>())];
    double per = 0;
    for (auto &o : rc["out"])
      if (o[0].get<string>() == *it) per += o[1].get<double>() * o[2].get<double>() / 10000;
    double want = p.need[*it];
    if (n.size() >= 4 && n[3].value("partial", false)) {  // partial: what's held first
      auto h = have.find(*it);
      want = max(0.0, want - (h != have.end() ? h->second : 0));
    }
    double runs = per > 0 ? ceil(want / per - 1e-9) : 0;
    for (size_t i = 0; i < n[2].size(); i++) {
      if (n[2][i].is_null()) continue;
      string c = canon(n[2][i].get<string>());
      int count = rc["in"][i].get<int>();
      if (count == 0) {
        p.tools.insert(c);
        p.need[c] = max(p.need[c], 1.0);
      } else p.need[c] += runs * count;
    }
    if (string m = machine(n); !m.empty()) {
      p.tools.insert(m);
      p.need[m] = max(p.need[m], 1.0);
    }
  }
  return p;
}

// what the group holds of k: all damages together for "any damage"
int64_t held_of(const FoodGroup *g, int k) {
  if (!g) return 0;
  if (wild[k]) {
    int64_t n = 0;
    string prefix = keys[k].substr(0, keys[k].rfind(':') + 1);
    for (auto it = g->held.lower_bound(prefix); it != g->held.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) n += it->second;
    return n;
  }
  auto h = g->held.find(keys[k]);
  return h != g->held.end() ? h->second : 0;
}

// a plan that needs more of a held thing than there is: make the rest, if there's a way to that doesn't need it.
// repeated, since the new steps can come up short too
void fill_shortfalls(const Solved &s, const FoodGroup *g, int k, double qty, json &nodes, json &jr, set<int> &named, set<int> &seen,
                     const set<int> *scarce = nullptr) {
  map<string, int64_t> have;
  set<int> tried;
  for (int round = 0; round < 6; round++) {
    have.clear();
    for (auto &[key, n] : nodes.items())
      if (auto ki = key_index.find(key); ki != key_index.end()) have[key] = held_of(g, ki->second);
    Needs p = plan_needs(nodes, jr, keys[k], qty, have);
    bool grew = false;
    for (auto &[key, want] : p.need) {
      auto n = nodes.find(key);
      if (n == nodes.end() || n->size() != 1 || n->at(0).is_null() || n->at(0).get<int>() != 0) continue;
      int kk = key_index[key];
      if (wild[kk] || s.value[kk] == 0 || have[key] >= want || made_by[kk].empty() || !tried.insert(kk).second) continue;
      nodes.erase(key);
      seen.erase(kk);
      TreeBuild b{s, nodes, jr, named, seen};
      b.scarce = scarce;
      if (b.add(kk, -1, true)) grew = true;
      else {
        b.back_to(0);
        nodes[key] = {0};
        seen.insert(kk);
      }
    }
    if (!grew) break;
  }
}

// held things a plan needs more of than there is (after making what can be made)
set<int> short_of(const FoodGroup *g, int k, double qty, const json &nodes, const json &jr) {
  map<string, int64_t> have;
  for (auto &[key, n] : nodes.items())
    if (auto ki = key_index.find(key); ki != key_index.end()) have[key] = held_of(g, ki->second);
  Needs p = plan_needs(nodes, jr, keys[k], qty, have);
  set<int> out;
  for (auto &[key, want] : p.need) {
    auto n = nodes.find(key);
    if (n == nodes.end() || n->size() != 1 || n->at(0).is_null() || n->at(0).get<int>() != 0) continue;
    int kk = key_index[key];
    bool free = key == "fluid:water" || (g && key.compare(0, 6, "fluid:") == 0 && g->free_fluids.count(key.substr(6)));
    if (circuit_key[kk] || free || have[key] >= want) continue;
    out.insert(kk);
  }
  return out;
}

// the plan for qty of k: the thriftiest tree, short things made where they can be, and where they can't, planned
// around (each round marks what's still short as scarce and plans again, keeping the last plan that worked)
void build_plan(const Solved &s, const FoodGroup *g, int k, double qty, int force, json &nodes, json &jr, set<int> &named) {
  set<int> scarce;
  bool have_plan = false;
  for (int round = 0; round < 4; round++) {
    json n2 = json::object(), r2 = json::object();
    set<int> seen2, named2;
    TreeBuild b{s, n2, r2, named2, seen2};
    b.scarce = &scarce;
    b.root = k;
    b.root_qty = qty;
    bool ok = b.add(k, force);
    if (!ok && !have_plan) {
      // nothing works around what's short (or the thriftiest picks loop): the old way, short or not
      n2 = json::object(), r2 = json::object(), seen2.clear(), named2.clear();
      add_tree(s, k, n2, r2, named2, seen2, force);
      ok = true;
    }
    if (!ok) break;
    fill_shortfalls(s, g, k, qty, n2, r2, named2, seen2, &scarce);
    nodes = move(n2), jr = move(r2);
    named.insert(named2.begin(), named2.end());
    have_plan = true;
    set<int> more = short_of(g, k, qty, nodes, jr);
    size_t before = scarce.size();
    scarce.insert(more.begin(), more.end());
    if (scarce.size() == before) break;
  }
}

// how far k is from being makeable: 0 when it is, otherwise roughly how many things are missing underneath its
// closest recipe, a few levels down (a recipe needing what it's making counts as hopeless)
struct Missing {
  const Solved &s;
  map<pair<int, int>, int> memo;
  set<int> stack;
  int key(int k, int depth) {
    if (cost_of(s, k) < INF) return 0;
    if (made_by[k].empty() || depth <= 0) return 1;
    if (stack.count(k)) return 1000;
    if (auto m = memo.find({k, depth}); m != memo.end()) return m->second;
    stack.insert(k);
    int best = 1000;
    for (int r : order(k)) best = min(best, recipe(r, depth));
    stack.erase(k);
    return memo[{k, depth}] = best;
  }
  // a key's recipes worth looking at: ones a plan would use first, then the fewest ingredients
  vector<int> order(int k) {
    vector<int> rs = made_by[k];
    stable_sort(rs.begin(), rs.end(), [](int a, int b) {
      if (plannable[a] != plannable[b]) return plannable[a] > plannable[b];
      return recipes[a].in.size() < recipes[b].in.size();
    });
    if (rs.size() > 40) rs.resize(40);
    return rs;
  }
  int recipe(int r, int depth) {
    const Recipe &rc = recipes[r];
    int n = (can_run(s, rc) ? 0 : 1) + (plannable[r] ? 0 : 3);
    for (auto &slot : rc.in) {
      if (slot.alts.empty()) continue;
      int b = 1000;
      for (size_t i = 0; i < slot.alts.size() && i < 3 && b > 0; i++)
        b = min(b, usable(s, slot, slot.alts[i]) ? key(slot.alts[i], depth - 1) : 1000);
      n += b;
      if (n >= 1000) return 1000;
    }
    return n;
  }
};

// why k can't be made: its closest recipe (by what's missing underneath), the machine it lacks, and for each
// missing ingredient the same, a few levels down. machine: the recipe map's name, the tier it needs, the best
// the group has
json blockers(const Solved &s, int k, int depth, set<int> &stack) {
  json out = {{"key", keys[k]}};
  if (made_by[k].empty()) {
    out["gathered"] = true;
    return out;
  }
  Missing m{s};
  int bestr = -1, bests = INT_MAX;
  for (int r : m.order(k)) {
    m.stack = stack;
    m.stack.insert(k);
    int sc = m.recipe(r, 3);
    if (sc < bests) bests = sc, bestr = r;
  }
  if (bestr < 0) bestr = made_by[k][0];
  const Recipe &rc = recipes[bestr];
  out["recipe"] = bestr;
  if (!can_run(s, rc)) {
    auto mn = machine_names.find(types[rc.type]);
    out["machine"] = {{"name", mn != machine_names.end() ? mn->second : types[rc.type]}, {"tier", rc.eut > 0 ? tier_for(rc.eut) : -1},
                      {"have", s.tier[rc.type]}};
  }
  json missing = json::array();
  stack.insert(k);
  for (auto &slot : rc.in) {
    if (slot.alts.empty()) continue;
    bool any = false;
    for (int a : slot.alts) any |= cost_of(s, a) < INF && usable(s, slot, a);
    if (any) continue;
    // the alternative closest to makeable
    int a = slot.alts[0], as = INT_MAX;
    for (size_t i = 0; i < slot.alts.size() && i < 6; i++) {
      m.stack = stack;
      int sc = m.key(slot.alts[i], 2) + wild[slot.alts[i]];
      if (sc < as) as = sc, a = slot.alts[i];
    }
    if (depth <= 0 || stack.count(a)) missing.push_back({{"key", keys[a]}, {"more", true}});
    else missing.push_back(blockers(s, a, depth - 1, stack));
  }
  stack.erase(k);
  out["missing"] = missing;
  return out;
}

// a group's solve for the current snapshot, kept until the next one. call with state_lock held
// build: may plan building missing machines (the recipe pages). the food planner counts only what can be made now
shared_ptr<Solved> solved(const string &group, bool build) {
  auto &c = solve_cache[(build ? "build:" : "now:") + group];
  if (!c.second || c.first != generation) {
    auto g = snapshot.groups.find(group);
    c = {generation, make_shared<Solved>(solve(g != snapshot.groups.end() ? g->second : FoodGroup{}, build))};
  }
  return c.second;
}

}  // namespace

bool foods_load(const filesystem::path &dir, FoodNamer n) {
  namer = n;
  try {
    json f = json::parse(readFile(dir / "foods.json"));
    for (auto &[key, v] : f.items())
      foods[intern(key)] = Food{v["name"].get<string>(), v["hunger"].get<int>(), v["saturation"].get<double>()};
    {
      // the unfiltered set when there is one, so the recipe search finds everything (the solve doesn't mind)
      filesystem::path file = exists(dir / "all_recipes.json") ? dir / "all_recipes.json" : dir / "recipes.json";
      json r = json::parse(readFile(file));
      recipes.reserve(r.size() + 1);
      for (auto &e : r) {
        Recipe rc;
        rc.type = type_of(e["t"].get<string>());
        rc.eut = e.value("eut", 0);
        rc.w = e.value("w", 0);
        rc.h = e.value("h", 0);
        for (auto &slot : e["in"]) {
          Slot s;
          s.count = slot[0].get<int>();
          for (size_t i = 1; i < slot.size(); i++) s.alts.push_back(intern(slot[i].get<string>()));
          rc.in.push_back(move(s));
        }
        for (auto &o : e["out"]) rc.out.push_back({intern(o[0].get<string>()), o[1].get<int>(), o[2].get<int>()});
        recipes.push_back(move(rc));
      }
    }
    // a bucket fills from any water source, no recipe has that
    if (key_index.count("minecraft:water_bucket:0"))
      recipes.push_back({type_of("fill"), 0, 0, 0, {{1, {intern("minecraft:bucket:0")}}}, {{intern("minecraft:water_bucket:0"), 1, 10000}}});
    if (exists(dir / "fluids.json")) {
      // keep the document alive, items() only refers into it
      json fl = json::parse(readFile(dir / "fluids.json"));
      for (auto &[k, v] : fl.items()) fluid_names[k] = v.get<string>();
    }
    if (exists(dir / "recipe_maps.json")) {
      json rm = json::parse(readFile(dir / "recipe_maps.json"));
      for (auto &[k, v] : rm.items()) machine_names[k] = v.get<string>();
    }
    if (exists(dir / "item_icons.json") && exists(dir / "item_icons.png")) {
      json ic = json::parse(readFile(dir / "item_icons.json"));
      icon_size = ic["size"].get<int>();
      icon_columns = ic["columns"].get<int>();
      for (auto &[k, v] : ic["icons"].items()) icon_of[k] = v.get<int>();
      icons_png = readFile(dir / "item_icons.png");
    }
    if (exists(dir / "gt_machine_maps.json")) {
      json m = json::parse(readFile(dir / "gt_machine_maps.json"));
      for (auto &[id, v] : m.items()) {
        vector<int> maps;
        for (auto &name : v["maps"]) maps.push_back(type_of(name.get<string>()));
        machine_maps[atoi(id.c_str())] = {v["tier"].get<int>(), maps};
      }
    }
  } catch (const exception &e) {
    fprintf(stderr, "foods: failed to load from %s: %s\n", dir.c_str(), e.what());
    return false;
  }
  made_by.assign(keys.size(), {});
  used_in.assign(keys.size(), 0);
  for (int r = 0; r < (int)recipes.size(); r++) {
    for (auto &o : recipes[r].out) made_by[o.key].push_back(r);
    set<int> uses;
    for (auto &slot : recipes[r].in) uses.insert(slot.alts.begin(), slot.alts.end());
    for (int k : uses) used_in[k]++;
  }
  // containers: what the fluid canner gives back emptying something (an empty cell, can, capsule). a recipe
  // returning one (a cell of ethylene in, polyethylene and the empty cell out) isn't taking anything apart
  set<int> containers;
  for (auto &rc : recipes) {
    if (types[rc.type] != "gt.recipe.fluidcanner" || rc.in.size() != 1 || rc.out.size() != 2) continue;
    bool fluid0 = keys[rc.out[0].key].compare(0, 6, "fluid:") == 0, fluid1 = keys[rc.out[1].key].compare(0, 6, "fluid:") == 0;
    if (fluid0 != fluid1) containers.insert(fluid0 ? rc.out[1].key : rc.out[0].key);
  }
  recycling.assign(recipes.size(), false);
  for (int r = 0; r < (int)recipes.size(); r++) {
    set<int> outs;
    for (auto &o : recipes[r].out)
      if (!containers.count(o.key)) outs.insert(o.key);
    for (auto &slot : recipes[r].in) {
      if (slot.count == 0 || recycling[r]) continue;
      for (size_t i = 0; i < slot.alts.size() && i < 4 && !recycling[r]; i++) {
        const auto &makers = made_by[slot.alts[i]];
        for (size_t m = 0; m < makers.size() && m < 64 && !recycling[r]; m++)
          for (auto &s2 : recipes[makers[m]].in) {
            if (s2.count == 0) continue;
            bool hit = false;
            for (int x : s2.alts) hit |= outs.count(x) > 0;
            if (hit) {
              recycling[r] = true;
              break;
            }
          }
      }
    }
  }
  // assembled: every way to make it (not counting taking things apart) puts together two or more different things.
  // an ingot (molten metal and a mold that's kept), a rod, a wire, a dust are one material in another form
  vector<signed char> assembled(keys.size(), -1);
  auto is_assembled = [&](int k) {
    if (assembled[k] >= 0) return (bool)assembled[k];
    bool any = false, all = true;
    for (int r : made_by[k]) {
      if (recycling[r]) continue;
      set<int> used;
      for (auto &slot : recipes[r].in)
        if (slot.count > 0 && !slot.alts.empty()) used.insert(slot.alts[0]);
      any = true;
      all &= used.size() >= 2;
    }
    return (bool)(assembled[k] = any && all);
  };
  plannable.assign(recipes.size(), true);
  for (int r = 0; r < (int)recipes.size(); r++) {
    if (!recycling[r] || types[recipes[r].type] == "gt.recipe.fluidcanner") continue;
    bool scraps = false;
    for (auto &slot : recipes[r].in)
      if (slot.count > 0 && !slot.alts.empty()) scraps |= is_assembled(slot.alts[0]);
    plannable[r] = !scraps;
  }
  circuit_key.assign(keys.size(), false);
  if (auto c = name_index.find("gregtech:gt.integrated_circuit"); c != name_index.end())
    for (int k = 0; k < (int)keys.size(); k++) circuit_key[k] = name_of[k] == c->second;
  machine_items.assign(types.size(), {});
  for (auto &[mid, tm] : machine_maps) {
    auto k = key_index.find("gregtech:gt.blockmachines:" + to_string(mid));
    if (k == key_index.end() || made_by[k->second].empty()) continue;  // nothing makes it, can't plan building it
    for (int type : tm.second) machine_items[type].push_back({k->second, tm.first < 0 ? 15 : tm.first, tm.first < 0});
    if (tm.first < 0) multi_keys.insert(k->second);
  }
  keys_of_name.assign(name_index.size(), {});
  for (int k = 0; k < (int)keys.size(); k++)
    if (name_of[k] >= 0) keys_of_name[name_of[k]].push_back(k);
  loaded = true;
  printf("foods: %zu foods, %zu recipes, %zu items, %zu GregTech machines\n", foods.size(), recipes.size(), keys.size(),
         machine_maps.size());
  return true;
}

bool foods_loaded() { return loaded; }
size_t foods_total() { return foods.size(); }

void foods_update(FoodSnapshot s) {
  lock_guard<mutex> l(state_lock);
  snapshot = move(s);
  generation++;
}

string foods_state_json() {
  lock_guard<mutex> l(state_lock);
  if (!loaded) return "{\"error\":\"no texture-dump foods.json/recipes.json loaded\"}";
  if (cached_generation == generation) return cached;

  json out;
  out["generated"] = (int64_t)time(nullptr);
  set<int> named;
  json &jf = out["foods"] = json::object();
  for (auto &[k, f] : foods) jf[keys[k]] = {{"name", f.name}, {"hunger", f.hunger}, {"saturation", f.saturation}};
  json &jg = out["groups"] = json::object();
  for (auto &[name, g] : snapshot.groups) {
    const Solved &s = *solved(name, false);
    json &craftable = jg[name]["craftable"] = json::object();
    for (auto &[k, f] : foods)
      if (s.dist[k] < INF) {
        // steps: how many recipes the plan behind "how to make it" has, for one, made the same way
        json nodes = json::object(), jr = json::object();
        set<int> plan_named;
        build_plan(s, &g, k, 1, -1, nodes, jr, plan_named);
        map<string, int64_t> have;
        for (auto &[key, n] : nodes.items()) have[key] = held_of(&g, key_index[key]);
        craftable[keys[k]] = (int)plan_needs(nodes, jr, keys[k], 1, have).order.size();
        named.insert(k);
      }
  }
  json &jp = out["players"] = json::array();
  for (auto &p : snapshot.players) {
    json eaten = json::array();
    for (auto &e : p.eaten) {
      eaten.push_back(e);
      if (auto k = key_index.find(e); k != key_index.end()) named.insert(k->second);
    }
    jp.push_back({{"name", p.name}, {"team", p.team}, {"group", p.group}, {"eaten", eaten}});
  }
  json &jn = out["names"] = json::object();
  for (int k : named) jn[keys[k]] = display(k);
  out["icons"] = icons_json(named);
  // eaten items the dump doesn't know
  for (auto &p : snapshot.players)
    for (auto &e : p.eaten)
      if (!key_index.count(e)) jn[e] = namer(e);
  cached = out.dump();
  cached_generation = generation;
  return cached;
}

string recipes_search_json(const string &query) {
  lock_guard<mutex> l(state_lock);
  if (!loaded) return "{\"error\":\"no recipes loaded\"}";
  if (search_names.empty()) {
    search_names.resize(keys.size());
    for (size_t k = 0; k < keys.size(); k++) {
      search_names[k] = display(k);
      for (char &c : search_names[k]) c = tolower(c);
    }
  }
  string q = query;
  for (char &c : q) c = tolower(c);
  // exact name first, then names starting with it, then containing it, the most used first within each
  vector<tuple<int, int, int>> hits;  // rank, -recipes, key
  for (size_t k = 0; k < keys.size() && !q.empty(); k++) {
    const string &n = search_names[k];
    size_t at = n.find(q);
    bool keyhit = keys[k].find(query) != string::npos;
    if (at == string::npos && !keyhit) continue;
    if (made_by[k].empty() && used_in[k] == 0) continue;
    int rank = n == q ? 0 : at == 0 ? 1 : at != string::npos ? 2 : 3;
    hits.emplace_back(rank, -(int)(made_by[k].size() + used_in[k]), k);
  }
  sort(hits.begin(), hits.end());
  json out = json::array();
  set<int> shown;
  for (size_t i = 0; i < hits.size() && i < 60; i++) {
    int k = get<2>(hits[i]);
    out.push_back({{"key", keys[k]}, {"name", display(k)}, {"makes", made_by[k].size()}, {"uses", used_in[k]}});
    shown.insert(k);
  }
  return json{{"results", out}, {"total", hits.size()}, {"icons", icons_json(shown)}}.dump();
}

const string &recipes_icons_png() { return icons_png; }

string recipes_groups_json() {
  lock_guard<mutex> l(state_lock);
  json g = json::array();
  for (auto &[name, group] : snapshot.groups) g.push_back(name);
  json players = json::object();
  for (auto &p : snapshot.players) players[p.name] = p.group;
  return json{{"groups", g}, {"players", players}}.dump();
}

string recipes_item_json(const string &key, const string &group, int force, double qty) {
  lock_guard<mutex> l(state_lock);
  auto f = key_index.find(key);
  if (!loaded || f == key_index.end()) return "{\"error\":\"unknown item\"}";
  int k = f->second;
  if (qty <= 0) qty = wild[k] || key.compare(0, 6, "fluid:") != 0 ? 1 : 1000;
  // what can be made with the machines the group has comes first (the same plan the food planner counts), only
  // when there's no way does the plan build machines
  shared_ptr<Solved> now = solved(group, false), build = solved(group, true);
  shared_ptr<Solved> s = now->dist[k] < INF ? now : build;
  set<int> named{k};
  json out;
  out["key"] = key;
  out["uses"] = used_in[k];
  // every recipe making it: can the group run it now (machine and voltage, every ingredient reachable), how
  // many steps its slowest ingredient is, and each slot's alternatives
  vector<tuple<int, int, int, int>> order;  // runnable first, steps, EU/t, recipe
  for (int r : made_by[k]) {
    const Recipe &rc = recipes[r];
    int worst = 0;
    for (auto &slot : rc.in) {
      if (slot.alts.empty()) continue;
      int b = INF;
      for (int a : slot.alts)
        if (usable(*build, slot, a)) b = min(b, cost_of(*build, a));
      worst = max(worst, b);
    }
    bool ok = machine_ok(*build, rc) && worst < INF;
    // real recipes before ones taking something apart
    order.emplace_back((ok ? 0 : machine_ok(*build, rc) ? 1 : 2) * 2 + recycling[r], worst, rc.eut, r);
  }
  sort(order.begin(), order.end());
  json producers = json::array();
  for (size_t i = 0; i < order.size() && i < 400; i++) {
    auto [state, worst, eut, r] = order[i];
    json rj = recipe_json(r, named);
    json slots = json::array();
    for (auto &slot : recipes[r].in) {
      json alts = json::array();
      for (size_t a = 0; a < slot.alts.size() && a < 12; a++) {
        int ak = slot.alts[a], d = cost_of(*build, ak);
        alts.push_back({keys[ak], d >= INF ? json(nullptr) : json(d)});
        named.insert(ak);
      }
      slots.push_back({{"count", slot.count}, {"alts", alts}, {"more", (int)slot.alts.size() - (int)alts.size()}});
    }
    rj["slots"] = slots;
    rj["state"] = state / 2 == 0 ? "ready" : state / 2 == 1 ? "missing ingredients" : "no machine";
    if (!owns(*build, recipes[r])) {
      int m = machine_to_build(*build, recipes[r]);
      if (m >= 0) {
        rj["build"] = keys[m];
        named.insert(m);
      }
    }
    rj["steps"] = worst >= INF ? json(nullptr) : json(worst + 1);
    rj["id"] = r;
    // what one of it uses up of the group's holdings going this way, all the way down
    // the same score the plan picks by, from the same solve (the machines the group has, when that works)
    const Solved &by = machine_ok(*s, recipes[r]) ? *s : *build;
    double c = state / 2 == 0 && plannable[r] ? route_score(by, recipes[r], k, qty) : INFINITY;
    rj["cost"] = c == INFINITY ? json(nullptr) : json(c);
    producers.push_back(rj);
  }
  out["producers"] = producers;
  out["producers_total"] = made_by[k].size();
  // the group's thriftiest route, as a tree, or the one through the recipe the page picked
  bool forced = false;
  if (force >= 0 && force < (int)recipes.size() && find(made_by[k].begin(), made_by[k].end(), force) != made_by[k].end() &&
      machine_ok(*s, recipes[force]) && run_cost(*s, recipes[force]) < INFINITY)
    forced = true;
  json nodes = json::object(), jr = json::object();
  set<int> seen;
  auto gi = snapshot.groups.find(group);
  const FoodGroup *gp = gi != snapshot.groups.end() ? &gi->second : nullptr;
  build_plan(*s, gp, k, qty, forced ? force : -1, nodes, jr, named);
  out["qty"] = qty;
  out["route"] = {{"nodes", nodes}, {"recipes", jr}};
  if (s->dist[k] >= INF) {
    set<int> stack;
    json b = blockers(*s, k, 5, stack);
    // name everything in it, and the recipes it names
    function<void(const json &)> walk = [&](const json &x) {
      named.insert(key_index[x["key"].get<string>()]);
      if (x.contains("recipe") && !jr.contains(to_string(x["recipe"].get<int>()))) jr[to_string(x["recipe"].get<int>())] = recipe_json(x["recipe"].get<int>(), named);
      if (x.contains("missing"))
        for (auto &m : x["missing"]) walk(m);
    };
    walk(b);
    out["route"]["recipes"] = jr;
    out["blockers"] = b;
  }
  if (forced) out["route"]["forced"] = force;
  // the score of the recipe the plan took. one scoring better wasn't usable: the group would run short of
  // something along it (or it was the one picked, then this is the thriftiest)
  double took = INFINITY, best = INFINITY;
  for (int r : made_by[k])
    if (plannable[r] && machine_ok(*s, recipes[r])) best = min(best, route_score(*s, recipes[r], k, qty));
  if (auto root = nodes.find(key); root != nodes.end() && root->size() >= 3 && !(*root)[0].is_null() && (*root)[1].get<int>() >= 0)
    took = route_score(*s, recipes[(*root)[1].get<int>()], k, qty);
  out["route"]["cost"] = forced || took == INFINITY ? (best == INFINITY ? json(nullptr) : json(best)) : json(took);
  // how much of each thing the route starts from the group has ("any damage": all damages together)
  json have = json::object();
  for (auto &[key, node] : nodes.items())
    if (int64_t n = held_of(gp, key_index[key]); n > 0 || (node.size() == 1 && !node[0].is_null())) have[key] = n;
  out["have"] = have;
  // what the group has for free besides water (steam from its boilers)
  json free = json::array();
  if (gp)
    for (auto &f : gp->free_fluids) free.push_back("fluid:" + f);
  out["free"] = free;
  json &jn = out["names"] = json::object();
  for (int n : named) jn[keys[n]] = display(n);
  out["icons"] = icons_json(named);
  return out.dump();
}

const char recipes_html[] =
#include "recipes.html.inc"
;

const char foods_html[] =
#include "foods.html.inc"
;
