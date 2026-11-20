// SPDX-License-Identifier: MPL-2.0
#include "foods.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/wait.h>
#include <unistd.h>
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
  int ticks = 0;     // how long it runs (GregTech), its energy is eut * ticks
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
map<string, vector<string>> sources;  // item key -> loot bags and quests that give it
map<string, string> fluid_names;  // texture-dump's fluids.json
map<string, string> machine_names;  // texture-dump's recipe_maps.json: GT recipe map -> its name in game
unordered_map<string, int> icon_of;  // texture-dump's item_icons.json: key -> cell in item_icons.png
// item -> the picture of any damage of it, for items whose damages are all the same thing (same name)
unordered_map<string, int> icon_of_item;
int icon_size = 16, icon_columns = 0;
string icons_png;
// a hash of the sheet: pages ask for icons.png?v=<it>, so a new sheet is a new address and no browser keeps
// cutting pictures out of an old one
string icons_version;

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
    // a damage without its own picture, of an item that's the same thing at every damage (an IC2 battery's charge)
    else if (auto g = icon_of_item.find(keys[k].substr(0, keys[k].rfind(':'))); g != icon_of_item.end()) icons[keys[k]] = g->second;
  }
  return {{"size", icon_size}, {"columns", icon_columns}, {"cells", icons}, {"version", icons_version}};
}
bool loaded = false;

vector<vector<int>> made_by;     // key -> recipes with it as an output
vector<int> used_in;             // key -> how many recipes take it
// per recipe: it takes something apart (its input is itself made from what it gives: melting a polyethylene rod,
// emptying a cell, macerating a machine). listed last, never a route's way to a held thing's worth
vector<bool> recycling;
// per recipe: a fingerprint that survives re-dumps (its machine, ingredients and outputs hashed), and back. a
// recipe turned off on the page is stored by it, its number changes whenever the dump does
vector<string> recipe_sig;
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
  set<int> banned;     // recipes the person asking turned off: never used, for routes or for worth
  vector<bool> held;   // per key: really in the group's storage (not just free: water, circuits)
  vector<bool> free;   // per key: costs nothing: water, the group's free fluids (steam with a boiler)
  // what the group's Logistics Pipes crafting modules make: result -> the ingredients they're set up with
  map<int, set<int>> automated;
  vector<int> dist, best, any;  // per key: steps from what's held, the recipe that does it. per item: best over damages
  vector<int> tier;             // per recipe type: the best voltage tier the group can run it at, -1 for none
  // per key: how much of the group's holdings one of it (one mB for a fluid) uses up on its cheapest route, and
  // that route's recipe (-1 when held). per item: the cheapest over damages. INFINITY when unreachable
  vector<double> cost, cost_any;
  vector<int> any_pick;  // concrete variant supplying each wildcard's complete cost/steps/setup tuple
  vector<int> cheapest;
  // per key: what one held unit is worth (0 for what isn't held). a held thing that can be made is worth what
  // making another would cost, so a route doesn't use up something valuable (recycle a held machine for its dust)
  vector<double> value;
  // per key: held, and the group can make more of it (a recipe that doesn't take something apart)
  vector<bool> replaceable;
  vector<int> value_by;  // per held key: the recipe its worth came from (making another), -1 for none
  // per key: how many recipe steps its thriftiest route takes (0 when held), per item the fewest over damages
  vector<int> steps, steps_any;
  // per key: what its thriftiest route costs once, however much is made (machines to build, tools to make)
  vector<double> setup, setup_any;
};

int cost_of(const Solved &s, int alt) { return wild[alt] ? s.any[name_of[alt]] : s.dist[alt]; }
bool single_use_tooling(int k) {
  const string &key = keys[k];
  if (!key.starts_with("ggfab:gt.ggfab.d1:")) return false;
  int meta = atoi(key.c_str() + key.rfind(':') + 1);
  return meta <= 7 || (meta >= 30 && meta <= 37);  // disposable tools and the molds made only for them
}
// a slot's alternative can be used: a circuit only as a setting, unless one is really held
bool usable(const Solved &s, const Slot &slot, int alt) {
  return !single_use_tooling(alt) && (!circuit_key[alt] || slot.count == 0 || s.held[alt]);
}
double raw_of(const Solved &s, int alt) { return wild[alt] ? s.cost_any[name_of[alt]] : s.cost[alt]; }

// what a held thing is worth: an item 1, a fluid's mB 1/144 (an ingot is 144 mB molten). every run of a recipe
// also costs a little, so of two equally thrifty routes the shorter wins
// no general cost per run: a recipe making more per run isn't better for it (8 ingots to a 16x wire to 16 wires
// isn't better than 1 ingot to 2 wires). Furnace smelting has a separate fuel and handling cost.
const double ITEM = 1, MB = 1.0 / 144, PER_RUN = 0, PRECIOUS = 16;
// A furnace batch uses fuel (roughly one coal per eight items) and needs handling. Without a per-item cost,
// hundreds of nugget smelts appear nearly free beside one powered machine run.
const double FURNACE_RUN = 0.25;
// energy counts too: 40,000 EU is worth an item (a few pieces of coal in a generator, a team running on steam has
// plenty). enough that the cutting machine's free water, slow and power hungry, doesn't win on being free alone,
// not so much that a machine saving material loses to wasting it by hand (a bending machine's plate from 1 ingot
// against crafting one from 2)
const double EU = 1.0 / 40000;
// what a route is scored by: what it uses up, plus a fixed cost per step (another machine to load and wait on,
// worth about an item, however much goes through it). so a small job takes the fewest steps (one copper ingot:
// smelt the crushed malachite), a big one the process that yields more (64: malachite dust in the blast furnace
// gives half as much again), and using something held beats a long way to an equivalent
const double STEP = 1;
double score(double cost, int steps) { return cost + STEP * steps; }

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
  double c = PER_RUN + (types[rc.type] == "smelting" ? FURNACE_RUN : 0) + (double)max(rc.eut, 0) * rc.ticks * EU;
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
bool gt_key(int k) { return keys[k].compare(0, 9, "gregtech:") == 0; }
int steps_of(const Solved &s, int alt) { return wild[alt] ? s.steps_any[name_of[alt]] : s.steps[alt]; }
// a recipe a plan may use: not scrapping, not turned off
bool allowed(const Solved &s, int r) { return plannable[r] && !s.banned.count(r); }

// a recipe the group's crafting modules run by themselves: it makes what a module makes, and every ingredient it
// uses up is one that module is set up with (an iron plate from iron ingots, not from a bending machine's ingot)
bool automated(const Solved &s, const Recipe &rc) {
  for (auto &o : rc.out) {
    auto a = s.automated.find(o.key);
    if (a == s.automated.end()) continue;
    bool all = true;
    for (auto &slot : rc.in) {
      if (slot.count == 0 || slot.alts.empty()) continue;
      bool any = false;
      for (int x : slot.alts) any |= a->second.count(x) > 0;
      all &= any;
    }
    if (all) return true;
  }
  return false;
}

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
  // a step the group's crafting modules do by themselves is no effort
  return min(st + (automated(s, rc) ? 0 : 1), 200);
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
Solved solve(const FoodGroup &g, bool build, const set<int> &banned = {}) {
  Solved s;
  s.build = build;
  s.banned = banned;
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
  for (auto &[out, ins] : g.automated)
    if (auto o = key_index.find(out); o != key_index.end())
      for (auto &in : ins)
        if (auto i = key_index.find(in); i != key_index.end()) s.automated[o->second].insert(i->second);
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
  // other mods' stations: a smeltery runs all of Tinkers' Construct's recipes
  for (size_t t = 0; t < types.size(); t++)
    if (types[t].compare(0, 11, "tconstruct.") == 0 && g.stations.count("tconstruct")) tier[t] = 15;
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
      if (!allowed(s, r)) continue;
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
    if (!allowed(s, r) || (!owns(s, rc) && machine_to_build(s, rc) < 0)) continue;
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
      int n = name_of[k];
      if (total(c, su, st, typical(k)) < total(s.cost_any[n], s.setup_any[n], s.steps_any[n], typical(k))) {
        s.cost_any[n] = c;
        s.steps_any[n] = st;
        s.setup_any[n] = su;
        s.any_pick[n] = k;
      }
    }
    return true;
  };
  auto relax = [&]() {
    s.cost.assign(keys.size(), INFINITY);
    s.cost_any.assign(name_index.size(), INFINITY);
    s.any_pick.assign(name_index.size(), -1);
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
        if (s.any_pick[n] < 0) s.steps_any[n] = 0;
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
  // then what held things that can be made are worth: exactly what making one more costs from the rest (no floor:
  // 86 lead dust is worth what the ore it came from is, so smelting it beats crushing more ore), or PRECIOUS when
  // it can't be made now (use it if needed, but not to save a little elsewhere). not counting round trips: a
  // recipe whose input is got through the thing itself (annealed copper wire, made from annealed copper ingots,
  // smelted back into one) says nothing about what the thing is worth
  function<bool(int, int, int)> through = [&](int a, int k, int depth) {
    if (a == k) return true;
    if (depth <= 0 || wild[a]) return false;
    int r = s.cheapest[a];
    // something held: follow how another would be made (the gold wire held is made from gold ingots, which are
    // made from the gold dust being valued: a loop through held stock is still a loop)
    if (r < 0 && s.dist[a] == 0 && !s.value_by.empty()) r = s.value_by[a];
    if (r < 0) return false;
    for (auto &slot : recipes[r].in) {
      if (slot.count == 0 || slot.alts.empty()) continue;
      int pick = -1;
      double b = INFINITY;
      for (int x : slot.alts)
        if (usable(s, slot, x) && raw_of(s, x) < b) b = raw_of(s, x), pick = x;
      if (pick >= 0 && through(pick, k, depth - 1)) return true;
    }
    return false;
  };
  // again until it settles: each round values held things from what the others are worth after the last one, so
  // the crushed ore's worth reaches the impure dust made from it, and the dust made from that
  vector<double> before;
  s.value_by.assign(keys.size(), -1);
  for (int round = 0; round < 4 && before != s.value; round++) {
  vector<int> by(keys.size(), -1);
  before = s.value;
  vector<double> value = s.value;
  s.replaceable.assign(keys.size(), false);
  for (int k = 0; k < (int)keys.size(); k++) {
    if (s.dist[k] != 0 || before[k] == 0 || made_by[k].empty()) continue;
    double best = INFINITY;
    for (int r : made_by[k]) {
      const Recipe &rc = recipes[r];
      if (!can_run(s, rc) || !allowed(s, r)) continue;
      bool loops = false;
      for (auto &slot : rc.in) {
        if (slot.count == 0 || slot.alts.empty() || loops) continue;
        int pick = -1;
        double b = INFINITY;
        for (int x : slot.alts)
          if (usable(s, slot, x) && raw_of(s, x) < b) b = raw_of(s, x), pick = x;
        loops = pick >= 0 && through(pick, k, 6);
      }
      if (loops) continue;
      double per = made_per_run(rc, k), c = run_cost(s, rc);
      if (per > 0 && c < INFINITY && c / per < best) best = c / per, by[k] = r;
    }
    value[k] = best == INFINITY ? PRECIOUS * base(k) : best;
    s.replaceable[k] = best < INFINITY;
  }
  s.value = value;
  s.value_by = by;
  relax();
  }
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
  json j = {{"t", types[rc.type]}, {"eut", rc.eut}, {"ticks", rc.ticks},
            {"tier", rc.eut > 0 ? tier_for(rc.eut) : -1}, {"in", in}, {"out", o}};
  if (rc.w > 0) j["w"] = rc.w, j["h"] = rc.h;
  if (auto m = machine_names.find(types[rc.type]); m != machine_names.end()) j["machine"] = m->second;
  // only what plans won't use (a breakdown machine scrapping something assembled), not every recipe the loose
  // recycling check flags (an assembler putting logistics pipes into a chassis)
  if (!plannable[r]) j["recycling"] = true;
  j["sig"] = recipe_sig[r];
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
  int building = 0;  // how many machines being built the current branch is inside
  // what the whole plan makes: never an ingredient inside its own plan (steel ingots from steel plates bent from
  // steel ingots), except held stock of it going into a machine the plan builds
  int target = -1;
  // the item currently being planned, including when a held ingredient needs topping up
  int root = -1;

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
  vector<int> candidates(int k, double qty) {
    if (!thrifty) return s.best[k] >= 0 ? vector<int>{s.best[k]} : vector<int>{};
    vector<pair<double, int>> c;
    for (int r : made_by[k]) {
      double per = made_per_run(recipes[r], k);
      if (per <= 0 || !allowed(s, r) || !machine_ok(s, recipes[r])) continue;
      // Score a child for what its parent needs, including the number of parent runs. A one-gear
      // subrecipe should not be ranked as if the plan needs a stack of gears.
      double sc = route_score(s, recipes[r], k, qty);
      if (sc < INFINITY) c.emplace_back(sc, r);
    }
    sort(c.begin(), c.end());
    // Circuit Assemblers automate the repeated hand placement of boards, wires and components. When one is
    // runnable, use its recipe before the crafting-grid version; a failed ingredient tree still falls through
    // to the other recipes. This also applies to circuits inside a larger machine plan.
    stable_partition(c.begin(), c.end(), [&](const auto &x) {
      return types[recipes[x.second].type] == "gt.recipe.circuitassembler";
    });
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
        // one that's held goes in before making another that would also do: they're interchangeable in this slot
        // (the held GregTech lead casing, not a freshly cast IC2 one). too few held is topped up or planned around
        bool hx = cost_of(s, x) == 0, hy = cost_of(s, y) == 0;
        if (hx != hy) return hx;
        double sx = sc(x), sy = sc(y);
        if (fabs(sx - sy) > 1e-9 * max(1.0, fabs(sx))) return sx < sy;
        if (cost_of(s, x) != cost_of(s, y)) return cost_of(s, x) < cost_of(s, y);
        // equal: GregTech's (GTNH unifies to it: a lead casing is GregTech's, IC2's is a leftover placeholder)
        return gt_key(x) && !gt_key(y);
      });
    else
      stable_sort(a.begin(), a.end(), [&](int x, int y) { return cost_of(s, x) < cost_of(s, y); });
    return a;
  }
  // make k with recipe r: every slot from its cheapest alternative that works
  bool with(int k, int r, int d, double qty) {
    size_t mark = added.size();
    json ju = json::array();
    double per = made_per_run(recipes[r], k);
    double runs = per > 0 && qty > 0 ? ceil(qty / per - 1e-9) : 1;
    for (auto &slot : recipes[r].in) {
      if (slot.alts.empty()) {
        ju.push_back(nullptr);
        continue;
      }
      bool ok = false;
      for (int a : alts(slot))
        if (add(a, -1, false, slot.count > 0 ? runs * slot.count : 1)) {
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
      building++;
      bool built = m >= 0 && add(m, -1, false, 1);
      building--;
      if (!built) {
        back_to(mark);
        return false;
      }
      node.push_back({{"machine", keys[m]}});
    }
    put(k, node);
    // A child may reuse an already planned node before this node exists. Adding it can close a cycle, such as
    // Steel Plates made from ingots that are smelted from those same plates.
    if (needs_stack(k)) {
      back_to(mark);
      return false;
    }
    if (!jr.contains(to_string(r))) jr[to_string(r)] = recipe_json(r, named);
    return true;
  }
  // give k a node, false if every way to it needs something being made higher up. make: k is held but not
  // enough, make it anyway (the node says how much is held, the plan makes the rest)
  // does the node already planned for k need any of what's being made higher up (directly or further down)
  bool needs_stack(int k) {
    set<int> visited;
    function<bool(int)> walk = [&](int x) {
      if (!visited.insert(x).second) return false;
      auto n = nodes.find(keys[x]);
      if (n == nodes.end() || n->size() < 3) return false;
      auto kids = (*n)[2];
      if (n->size() >= 4 && (*n)[3].contains("machine")) kids.push_back((*n)[3]["machine"]);
      for (auto &u : kids) {
        if (u.is_null()) continue;
        int c = key_index[u.get<string>()];
        // something being made higher up, made again down here: a loop. used from what's held there (a leaf), it
        // isn't one; nor is held stock of it going into a machine the plan builds (see add)
        if (stack.count(c)) {
          auto cn = nodes.find(keys[c]);
          bool made_here = cn != nodes.end() && cn->size() >= 3;
          if (made_here && !(building > 0 && cost_of(s, c) == 0 && !wild[c])) return true;
        }
        if (walk(c)) return true;
      }
      return false;
    };
    return walk(k);
  }
  bool add(int k, int force = -1, bool make = false, double qty = 0) {
    if (k == target && !stack.empty() && !(building > 0 && cost_of(s, k) == 0)) return false;
    // already planned: reuse it, unless it needs what's being made higher up (that would be a loop: more
    // annealed copper ingots from macerating the wire the plan makes out of annealed copper ingots)
    // (the fewest steps fallback can't loop, every step there is closer to what's held: no check, so it always
    // finds the plan there is)
    if (seen.count(k)) return !thrifty || !needs_stack(k);
    // something being made higher up: a loop. except when it's held and goes into a machine the plan builds (the
    // held vacuum tubes in the circuits of the extractor that makes what more vacuum tubes need). never into
    // making more of itself (annealed copper ingots to a block to dust to annealed copper ingots)
    if (stack.count(k)) return building > 0 && cost_of(s, k) == 0 && !wild[k];
    if (--budget < 0) return false;
    named.insert(k);
    int d = cost_of(s, k);
    if (d >= INF) {
      put(k, {nullptr});
      return true;
    }
    // An avoided item is excluded from held stock in the retry's solve. It may still need to be made when it
    // is an ingredient of the requested item (Crop Manager needs plates even when held plates run short).
    if (scarce && scarce->count(k) && k != root && d == 0) return false;
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
        if (add(k2, -1, false, qty)) {
          put(k, {d, -1, json::array({keys[k2]})});
          ok = true;
          break;
        }
    } else {
      for (int r : force >= 0 ? vector<int>{force} : candidates(k, qty))
        if ((ok = with(k, r, d, qty))) break;
    }
    // the plan's own target isn't partial: what's held of it doesn't count, it's being made
    if (ok && make && k != root) {
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
      // judged for what's missing, not a typical amount: making 4 more wires is one wiremill step, not 2x, 4x and
      // 8x wire crafted back down (cheaper per wire for 64, not for 4)
      b.root = kk;
      b.target = k;
      if (b.add(kk, -1, true, want - have[key])) {
        // This is a top-up of an ingredient, even though it is the root of this small retry.
        // Keep the stock already held instead of making the entire demand again.
        if (have[key] > 0) {
          json &made = nodes[key];
          if (made.size() < 4) made.push_back(json::object());
          made[3]["partial"] = true;
        }
        grew = true;
      } else {
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
    // Ore, crops and other gathered inputs have no recipe to plan around. The route can ask the player to
    // gather more; excluding them would erase otherwise workable routes.
    if (circuit_key[kk] || free || have[key] >= want || made_by[kk].empty()) continue;
    out.insert(kk);
  }
  return out;
}

// what a whole plan costs: everything it uses up of what's held (at what each is worth), made worse by its steps
double plan_total(const Solved &s, const FoodGroup *g, int k, double qty, const json &nodes, const json &jr) {
  map<string, int64_t> have;
  for (auto &[key, n] : nodes.items()) have[key] = held_of(g, key_index[key]);
  Needs p = plan_needs(nodes, jr, keys[k], qty, have);
  double used = 0;
  for (auto &[key, want] : p.need) {
    auto n = nodes.find(key);
    if (n == nodes.end() || p.tools.count(key)) continue;
    int kk = key_index[key];
    bool held = n->size() == 1 && !n->at(0).is_null() && n->at(0).get<int>() == 0;
    bool partial = n->size() >= 4 && (*n)[3].value("partial", false);
    if (held) {
      used += min<double>(want, have[key]) * s.value[kk];
      // more than there is, and the plan couldn't make the rest: it can't be carried out. any plan that can wins,
      // one that's short is only kept when there's nothing else
      if (want > have[key] && !s.free[kk] && !circuit_key[kk]) used += (want - have[key]) * 1e6;
    } else if (partial) used += min<double>(want, have[key]) * s.value[kk];
  }
  for (const string &key : p.order) {
    const json &n = nodes[key];
    int r = n[1].get<int>(), kk = key_index[key];
    double per = made_per_run(recipes[r], kk);
    double want = p.need[key];
    if (n.size() >= 4 && n[3].value("partial", false)) want = max(0.0, want - have[key]);
    if (per > 0 && want > 0)
      used += ceil(want / per - 1e-9) * ((types[recipes[r].type] == "smelting" ? FURNACE_RUN : 0) +
                                             max(recipes[r].eut, 0) * recipes[r].ticks * EU);
  }
  return score(used, (int)p.order.size());
}

// the plan for qty of k: the thriftiest tree, short things made where they can be, and where they can't, planned
// around (each round marks what's still short as scarce and plans again, keeping the cheapest plan found).
// something held that runs short and gets topped up is also tried without (86 lead dust beats making more lead
// nuggets once the 9 held ones run out), whichever whole plan costs less is kept
void build_plan(const Solved &s, const FoodGroup *g, int k, double qty, int force, json &nodes, json &jr, set<int> &named) {
  set<int> scarce;
  bool have_plan = false;
  double have_total = INFINITY;
  // planning around something: price everything again as if it weren't held, so the plan's own top recipe is
  // chosen afresh too (13 rubber pulp with only 4 rubber bars: not crafting small piles, whose cheap source was
  // the bars, from rubber sheets cut into casings and macerated, when the sheets macerate straight to pulp)
  shared_ptr<Solved> without;
  auto attempt = [&](const set<int> &avoid, json &n2, json &r2, set<int> &named2) {
    const Solved *use = &s;
    if (!avoid.empty() && g) {
      FoodGroup g2 = *g;
      for (int a : avoid) g2.held.erase(keys[a]);
      without = make_shared<Solved>(solve(g2, s.build, s.banned));
      use = without.get();
    }
    set<int> seen2;
    TreeBuild b{*use, n2, r2, named2, seen2};
    b.scarce = &avoid;
    b.root = k;
    b.target = k;
    // the target is made even when some is held: looking it up is wanting more of it. something nothing makes
    // stays held (or unreachable)
    if (!b.add(k, force, !made_by[k].empty(), qty)) return false;
    // planning around something can leave no way at all: that's no plan, not a plan that uses nothing up
    if (auto root = n2.find(keys[k]); root == n2.end() || root->at(0).is_null()) return false;
    fill_shortfalls(*use, g, k, qty, n2, r2, named2, seen2, &avoid);
    return true;
  };
  // each round avoids what the last attempt was short of or had to top up, and the cheapest whole plan seen is
  // kept (avoiding the plates can leave a plan short of bolts: avoid those too, rather than stop)
  for (int round = 0; round < 6; round++) {
    json n2 = json::object(), r2 = json::object();
    set<int> named2;
    bool ok = attempt(scarce, n2, r2, named2);
    if (!ok && !have_plan) {
      // nothing works around what's short (or the thriftiest picks loop): the old way, short or not
      n2 = json::object(), r2 = json::object(), named2.clear();
      set<int> seen2;
      add_tree(s, k, n2, r2, named2, seen2, force);
      ok = true;
    }
    if (!ok) break;
    double total = plan_total(s, g, k, qty, n2, r2);
    set<int> more = short_of(g, k, qty, n2, r2);
    // First resolve things the plan still lacks. Avoiding every top-up at once can discard a good route
    // (held soldering alloy plus one extracted ingot) while the retries repair unrelated shortages.
    if (more.empty())
      for (auto &[key, n] : n2.items())
        if (key_index[key] != k && n.size() >= 4 && n[3].value("partial", false)) more.insert(key_index[key]);
    // short: the odds and ends it reached for won't do either (4 steel plates, then 3 nuggets, then a chain
    // helmet...), a plan takes one source per ingredient. avoid every small held stock it used at once, so the
    // next round looks for a real source (iron dust into the blast furnace)
    if (!more.empty() && n2.size() <= 12)
      for (auto &[key, n] : n2.items()) {
        int kk = key_index[key];
        if (kk == k || n.size() != 1 || n[0].is_null() || n[0].get<int>() != 0 || s.free[kk] || circuit_key[kk]) continue;
        if (held_of(g, kk) < 16) more.insert(kk);
      }
    if (!have_plan || total < have_total) {
      nodes = move(n2), jr = move(r2);
      named.insert(named2.begin(), named2.end());
      have_plan = true;
      have_total = total;
    }
    size_t before = scarce.size();
    scarce.insert(more.begin(), more.end());
    if (scarce.size() == before) break;
  }
}

// Run the integer model in a separate process so a slow search cannot hold the exporter indefinitely.
json solve_full_problem(const json &problem) {
  const char *script = getenv("FOODS_OPTIMIZER");
  string path = script ? script : "full_plan.py";
  if (!filesystem::exists(path)) return nullptr;
  char filename[] = "/tmp/foods-plan-XXXXXX";
  int input = mkstemp(filename);
  if (input < 0) return nullptr;
  string body = problem.dump();
  size_t offset = 0;
  while (offset < body.size()) {
    ssize_t n = write(input, body.data() + offset, body.size() - offset);
    if (n <= 0) { close(input); unlink(filename); return nullptr; }
    offset += n;
  }
  lseek(input, 0, SEEK_SET);
  int output[2];
  if (pipe(output) != 0) { close(input); unlink(filename); return nullptr; }
  pid_t child = fork();
  if (child == 0) {
    dup2(input, STDIN_FILENO);
    dup2(output[1], STDOUT_FILENO);
    close(output[0]);
    close(output[1]);
    close(input);
    const char *python = getenv("FOODS_PYTHON");
    execlp(python ? python : "python3", python ? python : "python3", path.c_str(), (char *)nullptr);
    _exit(127);
  }
  close(input);
  close(output[1]);
  unlink(filename);
  if (child < 0) { close(output[0]); return nullptr; }
  string result;
  char buffer[8192];
  for (ssize_t n; (n = read(output[0], buffer, sizeof(buffer))) > 0;)
    if (result.size() + n < 10000000) result.append(buffer, n);
  close(output[0]);
  int status = 0;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return nullptr;
  try { return json::parse(result); } catch (...) { return nullptr; }
}

// Give the optimizer the tree's recipes, nearby alternatives, and the ingredients they lead to.
// Its result is exact for this finite candidate set. A candidate cap keeps page requests responsive.
json full_plan(const Solved &s, const FoodGroup *g, int target, double qty, int force,
               const json &nodes, set<int> &named) {
  if (nodes.empty() || cost_of(s, target) >= INF) return nullptr;
  json problem = {{"demand", {{keys[target], qty}}}, {"recipes", json::array()},
                  {"held", json::object()}, {"value", json::object()}, {"free", json::array()},
                  {"gather", json::object()}, {"time_limit", 8}, {"allow_missing", false}};
  struct Variant { int r; vector<int> uses; int made; };
  map<string, Variant> variants;
  vector<pair<int, int>> queue;
  set<int> visited;
  for (auto &[key, node] : nodes.items())
    if (auto f = key_index.find(key); f != key_index.end() && !wild[f->second]) queue.emplace_back(f->second, 0);
  if (queue.empty()) return nullptr;
  set<int> route_items;
  for (auto [item, depth] : queue) route_items.insert(item);
  auto available = [&](int a, const Slot &slot) {
    return !wild[a] && cost_of(s, a) < INF && usable(s, slot, a);
  };
  auto add_variant = [&](int r, int made, const vector<int> &uses) {
    if (variants.size() >= 1800) return;
    string id = to_string(r);
    for (int a : uses) id += ":" + to_string(a);
    if (variants.count(id)) return;
    const Recipe &rc = recipes[r];
    json in = json::object(), out = json::object(), tools = json::array();
    for (size_t i = 0; i < rc.in.size(); i++) {
      int a = uses[i];
      if (a < 0) continue;
      if (!route_items.count(a) && held_of(g, a) <= 0 && !s.free[a]) return;
      if (rc.in[i].count > 0) in[keys[a]] = in.value(keys[a], 0) + rc.in[i].count;
      else if (!circuit_key[a]) tools.push_back(keys[a]);
    }
    for (auto &o : rc.out)
      if (o.chance > 0) out[keys[o.key]] = out.value(keys[o.key], 0.0) + o.amount * o.chance / 10000.0;
    if (out.empty()) return;
    json v = {{"id", id}, {"in", in}, {"out", out}, {"tools", tools},
              {"run_cost", (types[rc.type] == "smelting" ? FURNACE_RUN : 0) + max(rc.eut, 0) * rc.ticks * EU},
              {"setup_cost", automated(s, rc) ? 0 : STEP}, {"max_runs", 100000}};
    if (!owns(s, rc)) {
      int m = machine_to_build(s, rc);
      if (m < 0) return;
      if (!route_items.count(m) && held_of(g, m) <= 0) return;
      v["machine"] = keys[m];
    }
    problem["recipes"].push_back(v);
    variants.emplace(id, Variant{r, uses, made});
  };
  for (size_t pos = 0; pos < queue.size() && visited.size() < 900 && variants.size() < 1800; pos++) {
    auto [item, depth] = queue[pos];
    if (!visited.insert(item).second) continue;
    if (auto n = nodes.find(keys[item]); n != nodes.end() && n->size() >= 3 && !(*n)[0].is_null() && (*n)[1].get<int>() >= 0) {
      int r = (*n)[1].get<int>();
      vector<int> uses;
      for (auto &u : (*n)[2]) {
        if (u.is_null()) { uses.push_back(-1); continue; }
        int a = key_index[u.get<string>()];
        if (wild[a]) {
          auto w = nodes.find(keys[a]);
          if (w != nodes.end() && w->size() >= 3 && (*w)[1].get<int>() == -1)
            a = key_index[(*w)[2][0].get<string>()];
        }
        uses.push_back(wild[a] ? -1 : a);
      }
      bool complete = uses.size() == recipes[r].in.size();
      if (complete)
        for (size_t i = 0; i < uses.size(); i++)
          if (uses[i] < 0 && !recipes[r].in[i].alts.empty()) complete = false;
      if (complete && allowed(s, r) && machine_ok(s, recipes[r])) add_variant(r, item, uses);
    }
    vector<pair<double, int>> choices;
    for (int r : made_by[item]) {
      if (!allowed(s, r) || !machine_ok(s, recipes[r])) continue;
      if (item == target && force >= 0 && r != force) continue;
      double c = route_score(s, recipes[r], item, item == target ? qty : 0);
      if (c < INFINITY) choices.emplace_back(c, r);
    }
    sort(choices.begin(), choices.end());
    int limit = 3;
    int chosen = 0;
    for (auto [cost, r] : choices) {
      if (chosen++ >= limit) break;
      const Recipe &rc = recipes[r];
      vector<int> uses;
      vector<pair<size_t, int>> substitutions;
      bool ok = true;
      for (size_t i = 0; i < rc.in.size(); i++) {
        const Slot &slot = rc.in[i];
        if (slot.alts.empty()) { uses.push_back(-1); continue; }
        vector<pair<double, int>> alts;
        for (int a : slot.alts) {
          if (available(a, slot)) alts.emplace_back(raw_of(s, a), a);
          else if (wild[a])
            for (int concrete : keys_of_name[name_of[a]])
              if (available(concrete, slot)) alts.emplace_back(raw_of(s, concrete), concrete);
        }
        sort(alts.begin(), alts.end());
        alts.erase(unique(alts.begin(), alts.end(), [](auto a, auto b) { return a.second == b.second; }), alts.end());
        if (alts.empty()) { ok = false; break; }
        uses.push_back(alts[0].second);
        if (alts.size() > 1) substitutions.emplace_back(i, alts[1].second);
      }
      if (!ok) continue;
      add_variant(r, item, uses);
      for (auto [slot, alt] : substitutions) {
        vector<int> changed = uses;
        changed[slot] = alt;
        add_variant(r, item, changed);
      }
    }
  }
  if (variants.empty()) return json{{"status", "no_candidates"}};
  set<int> involved{target};
  for (auto &[id, v] : variants) {
    const Recipe &rc = recipes[v.r];
    for (int a : v.uses) if (a >= 0) involved.insert(a);
    for (auto &o : rc.out) involved.insert(o.key);
    if (!owns(s, rc)) involved.insert(machine_to_build(s, rc));
  }
  for (int item : involved) {
    if (item < 0) continue;
    double held = item == target ? 0 : held_of(g, item);
    if (held > 0) problem["held"][keys[item]] = held;
    problem["value"][keys[item]] = s.value[item] > 0 ? s.value[item] :
                                      keys[item].starts_with("fluid:") ? MB : ITEM;
    if (s.free[item]) problem["free"].push_back(keys[item]);
    if (item != target && !s.free[item] && !circuit_key[item] && made_by[item].empty())
      problem["gather"][keys[item]] = PRECIOUS * problem["value"][keys[item]].get<double>();
  }
  json result = solve_full_problem(problem);
  if (!result.is_object()) return json{{"status", "unavailable"}};
  if (result.value("status", "") != "optimal") return json{{"status", result.value("status", "unavailable")}};
  json steps = json::array(), jr = json::object();
  for (auto &step : result["order"]) {
    string id = step["id"].get<string>();
    auto it = variants.find(id);
    if (it == variants.end()) return nullptr;
    const Variant &v = it->second;
    json uses = json::array();
    for (int a : v.uses) {
      uses.push_back(a < 0 ? json(nullptr) : json(keys[a]));
      if (a >= 0) named.insert(a);
    }
    steps.push_back({{"recipe", v.r}, {"uses", uses}, {"runs", step["runs"]}, {"made", keys[v.made]}});
    if (!jr.contains(to_string(v.r))) jr[to_string(v.r)] = recipe_json(v.r, named);
    named.insert(v.made);
  }
  json optimized = {{"status", "optimal"}, {"cost", result["cost"]}, {"steps", steps},
                    {"stock", result["stock"]}, {"gather", result["missing"]},
                    {"recipes", jr}, {"candidates", variants.size()}};
  return optimized;
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
      if (recycling[a] != recycling[b]) return recycling[a] < recycling[b];
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
    // and why that machine can't be built: the lowest tier one that would do, explained the same way
    int need = rc.eut > 0 ? tier_for(rc.eut) : 0, pick = -1, pick_tier = INT_MAX;
    for (auto &m : machine_items[rc.type])
      if (m.tier >= need && m.tier < pick_tier) pick = m.key, pick_tier = m.tier;
    if (pick >= 0 && depth > 0 && !stack.count(pick)) {
      stack.insert(k);
      out["machine"]["why"] = blockers(s, pick, depth - 1, stack);
      stack.erase(k);
    }
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

// An unreachable item still has useful recipes. Follow the closest ones as far as possible so the page can
// show a quantity-aware path, with unavailable ingredients as leaves, instead of only a nested blocker report.
struct MissingPlan {
  const Solved &s;
  json &nodes, &jr;
  set<int> &named;
  Missing missing;
  set<int> stack, seen;
  set<int> limited;
  vector<int> added;
  int budget = 800;
  int horizon = 5;
  int base_tier = 1;
  bool strict = true;

  bool raw_material(int k) const {
    const string &key = keys[k];
    if (key.starts_with("fluid:")) return true;
    if (key.starts_with("bartworks:gt.bwMetaGenerateddust")) return true;
    if (!key.starts_with("gregtech:gt.metaitem.01:")) return false;
    int meta = atoi(key.c_str() + key.rfind(':') + 1);
    return meta < 3000 || (meta >= 9000 && meta < 13000);
  }
  bool processed_form(int k) const {
    const string &key = keys[k];
    if (key.starts_with("gregtech:gt.metaitem.01:")) {
      int meta = atoi(key.c_str() + key.rfind(':') + 1);
      return meta >= 13000 && meta < 32000;
    }
    return key.starts_with("gregtech:gt.metaitem.02:") || key.starts_with("gregtech:gt.block");
  }
  // For an unavailable material, start from dusts and ingots before plates, tool parts or placed blocks.
  // This keeps a provisional path pointed toward primary production rather than melting a finished part.
  int source_rank(int k) const {
    if (cost_of(s, k) == 0) return -2;
    const string &key = keys[k];
    if (key.starts_with("bartworks:gt.bwMetaGenerateddust")) return 0;
    if (key.starts_with("gregtech:gt.metaitem.01:")) {
      int meta = atoi(key.c_str() + key.rfind(':') + 1);
      if ((meta >= 2000 && meta < 3000) || (meta >= 11000 && meta < 13000)) return 0;
      return meta < 32000 ? 2 : 3;
    }
    if (key.starts_with("fluid:")) return 1;
    if (key.starts_with("gregtech:gt.metaitem.02:")) return 3;
    if (key.starts_with("TGregworks:") || key.starts_with("gregtech:gt.block")) return 5;
    return 2;
  }
  int recipe_rank(int r, int target) const {
    const string &type = types[recipes[r].type];
    bool cools_plasma = type == "gt.recipe.vacuumfreezer" && any_of(recipes[r].in.begin(), recipes[r].in.end(), [](const Slot &slot) {
      return any_of(slot.alts.begin(), slot.alts.end(), [](int k) { return keys[k].starts_with("fluid:plasma."); });
    });
    int rank = type == "gt.recipe.polarizer" || type == "gt.recipe.vacuumfreezer" ? -10
             : type == "gt.recipe.blastfurnace" ? -8
             : type == "gt.recipe.mixer" ? (raw_material(target) ? -30 : -6)
             : type == "gt.recipe.metalbender" ? -5
             : type == "gt.recipe.extruder" ? -3
             : type == "gt.recipe.arcfurnace" || type == "gt.recipe.fluidcanner" ? 8
             : type == "gt.recipe.fluidsolidifier" || type == "gt.recipe.fluidextractor" || type == "smelting" ? 5 : 0;
    if (cools_plasma) rank += 50;
    for (auto &slot : recipes[r].in) {
      if (slot.count <= 0 || slot.alts.empty()) continue;
      int best = 10;
      for (int a : slot.alts) best = min(best, source_rank(a));
      rank += best;
    }
    if (recipes[r].eut > 0 && !owns(s, recipes[r]))
      rank += 8 * max(0, tier_for(recipes[r].eut) - base_tier);
    return rank;
  }

  void put(int k, json n) {
    nodes[keys[k]] = move(n);
    seen.insert(k);
    added.push_back(k);
    named.insert(k);
  }
  void back_to(size_t mark) {
    while (added.size() > mark) {
      int k = added.back();
      nodes.erase(keys[k]);
      seen.erase(k);
      added.pop_back();
    }
  }
  int distance(int k) {
    if (cost_of(s, k) < INF) return 0;
    missing.stack = stack;
    return missing.key(k, 3);
  }
  bool needs_stack(int k) const {
    set<int> visited;
    function<bool(int)> walk = [&](int x) {
      if (!visited.insert(x).second) return false;
      auto n = nodes.find(keys[x]);
      if (n == nodes.end() || n->size() < 3) return false;
      for (auto &u : (*n)[2]) {
        if (u.is_null()) continue;
        int child = key_index[u.get<string>()];
        if (stack.count(child) || walk(child)) return true;
      }
      if (n->size() >= 4 && (*n)[3].contains("machine")) {
        int machine = key_index[(*n)[3]["machine"].get<string>()];
        if (stack.count(machine) || walk(machine)) return true;
      }
      return false;
    };
    return walk(k);
  }
  bool add(int k, int depth = 0) {
    if (stack.count(k)) return false;
    if (seen.count(k)) return !needs_stack(k);
    if ((int)nodes.size() >= budget || depth >= 64) {
      limited.insert(k);
      if (strict) return false;
      put(k, {nullptr});
      return true;
    }
    if (made_by[k].empty() && !wild[k]) {
      put(k, {nullptr});
      return true;
    }
    if (cost_of(s, k) == 0) {
      put(k, {0});
      return true;
    }
    stack.insert(k);
    if (wild[k]) {
      vector<int> choices;
      for (int a : keys_of_name[name_of[k]]) if (!wild[a]) choices.push_back(a);
      stable_sort(choices.begin(), choices.end(), [&](int a, int b) { return distance(a) < distance(b); });
      for (int a : choices)
        if (add(a, depth + 1)) {
          put(k, {1, -1, json::array({keys[a]})});
          stack.erase(k);
          return true;
        }
    } else {
      vector<int> choices;
      bool cycle_failed = false;
      int beyond = 15;
      for (int r : made_by[k]) if (allowed(s, r)) {
        bool scraps_form = false;
        const string &type = types[recipes[r].type];
        if (raw_material(k) && (type == "gt.recipe.centrifuge" || type == "gt.recipe.electrolyzer"))
          for (auto &slot : recipes[r].in)
            if (slot.count > 0 && !slot.alts.empty() && all_of(slot.alts.begin(), slot.alts.end(), [&](int a) {
                  const string &input = keys[a];
                  return cost_of(s, a) >= INF &&
                         (input.starts_with("bartworks:gt.bwMetaGenerateddust") ||
                          input.starts_with("gregtech:gt.metaitem.01:2"));
                })) scraps_form = true;
        if (raw_material(k))
          for (auto &slot : recipes[r].in) {
            if (slot.count <= 0 || slot.alts.empty()) continue;
            bool can_use_raw = false;
            for (int a : slot.alts)
              if (!processed_form(a)) {
                bool same_material_ingot = false;
                if (keys[k].starts_with("gregtech:gt.metaitem.01:") && keys[a].starts_with("gregtech:gt.metaitem.01:")) {
                  int target_meta = atoi(keys[k].c_str() + keys[k].rfind(':') + 1);
                  int input_meta = atoi(keys[a].c_str() + keys[a].rfind(':') + 1);
                  same_material_ingot = target_meta >= 2000 && target_meta < 3000 && input_meta >= 10000 &&
                                        input_meta < 13000 && target_meta % 1000 == input_meta % 1000 && cost_of(s, a) >= INF;
                }
                if (!same_material_ingot) can_use_raw = true;
              }
            if (!can_use_raw) scraps_form = true;
          }
        if (scraps_form) continue;
        bool consumes_plasma = false;
        for (auto &slot : recipes[r].in)
          for (int a : slot.alts)
            if ((keys[a].starts_with("fluid:plasma.") || display(a).find("Plasma Cell") != string::npos) && cost_of(s, a) != 0)
              consumes_plasma = true;
        if (consumes_plasma && (types[recipes[r].type] == "gt.recipe.fluidcanner" || !keys[k].starts_with("fluid:plasma."))) continue;
        int tier = recipes[r].eut > 0 ? tier_for(recipes[r].eut) : 0;
        if (depth > 0 && tier > horizon) { beyond = min(beyond, tier); continue; }
        choices.push_back(r);
      }
      stable_sort(choices.begin(), choices.end(), [&](int a, int b) {
        int ra = recipe_rank(a, k), rb = recipe_rank(b, k);
        missing.stack = stack;
        int ca = missing.recipe(a, 3);
        missing.stack = stack;
        int cb = missing.recipe(b, 3);
        if (ra + 15 * ca != rb + 15 * cb) return ra + 15 * ca < rb + 15 * cb;
        return ra < rb;
      });
      if (choices.size() > 40) choices.resize(40);
      for (int r : choices) {
        size_t mark = added.size();
        json uses = json::array(), extra = json::object();
        bool ok = true;
        for (auto &slot : recipes[r].in) {
          if (slot.alts.empty()) { uses.push_back(nullptr); continue; }
          vector<int> alts = slot.alts;
          stable_sort(alts.begin(), alts.end(), [&](int a, int b) {
            bool held_a = cost_of(s, a) == 0, held_b = cost_of(s, b) == 0;
            if (held_a != held_b) return held_a;
            if (display(a) == display(b) && gt_key(a) != gt_key(b)) return gt_key(a);
            return distance(a) < distance(b);
          });
          bool found = false;
          for (int a : alts)
            if (usable(s, slot, a)) {
              bool duplicate_without_source = !gt_key(a) && cost_of(s, a) >= INF && made_by[a].empty() &&
                any_of(slot.alts.begin(), slot.alts.end(), [&](int other) {
                  return gt_key(other) && display(other) == display(a);
                });
              if (duplicate_without_source) continue;
              if (add(a, depth + 1)) { uses.push_back(keys[a]); found = true; break; }
            }
          if (!found) {
            for (int a : alts) if (stack.count(a)) cycle_failed = true;
            ok = false;
            break;
          }
        }
        if (ok && !owns(s, recipes[r])) {
          auto name = machine_names.find(types[recipes[r].type]);
          extra["missing_machine"] = name != machine_names.end() ? name->second : types[recipes[r].type];
          extra["missing_tier"] = recipes[r].eut > 0 ? tier_for(recipes[r].eut) : -1;
          extra["have_tier"] = s.tier[recipes[r].type];
          int machine = machine_to_build(s, recipes[r]);
          if (machine >= 0 && add(machine, depth + 1)) extra["machine"] = keys[machine];
          else {
            int need = max(0, extra["missing_tier"].get<int>()), pick = -1, pick_tier = INT_MAX;
            for (auto &m : machine_items[recipes[r].type])
              if (m.tier >= need && m.tier < pick_tier) pick = m.key, pick_tier = m.tier;
            if (pick >= 0) extra["missing_machine_key"] = keys[pick], named.insert(pick);
          }
        }
        if (ok) {
          json n = {1, r, uses};
          if (!extra.empty()) n.push_back(extra);
          put(k, move(n));
          if (!jr.contains(to_string(r))) jr[to_string(r)] = recipe_json(r, named);
          stack.erase(k);
          return true;
        }
        back_to(mark);
      }
      if (cycle_failed) {
        stack.erase(k);
        return false;
      }
      if (choices.empty() && beyond < 15) {
        limited.insert(k);
        stack.erase(k);
        put(k, {nullptr, {{"tier_limited", beyond}}});
        return true;
      }
    }
    stack.erase(k);
    if (strict) return false;
    put(k, {nullptr});
    return true;
  }
};

// a group's solve for the current snapshot, kept until the next one. call with state_lock held
// build: may plan building missing machines (the recipe pages). the food planner counts only what can be made now
// banned: recipes the person asking turned off. each set of them is its own solve, kept like the rest
shared_ptr<Solved> solved(const string &group, bool build, const set<int> &banned = {}) {
  string key = (build ? "build:" : "now:") + group;
  for (int r : banned) key += ":" + to_string(r);
  auto &c = solve_cache[key];
  if (!c.second || c.first != generation) {
    auto g = snapshot.groups.find(group);
    c = {generation, make_shared<Solved>(solve(g != snapshot.groups.end() ? g->second : FoodGroup{}, build, banned))};
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
        rc.ticks = e.value("ticks", 0);
        rc.w = e.value("w", 0);
        rc.h = e.value("h", 0);
        for (auto &slot : e["in"]) {
          Slot s;
          s.count = slot[0].get<int>();
          for (size_t i = 1; i < slot.size(); i++) s.alts.push_back(intern(slot[i].get<string>()));
          // a GregTech tool in a crafting grid (hammer, wire cutter, file) takes damage, it isn't used up. the dump
          // only spots tools that come back whole, GregTech's come back worn. a slot also taking GG Fab's single use
          // tool still only needs the reusable one
          if (rc.type == type_of("crafting")) {
            bool tool = false;
            for (int a : s.alts) tool |= keys[a].compare(0, 21, "gregtech:gt.metatool.") == 0;
            if (tool) s.count = 0;
          }
          rc.in.push_back(move(s));
        }
        for (auto &o : e["out"]) rc.out.push_back({intern(o[0].get<string>()), o[1].get<int>(), o[2].get<int>()});
        recipes.push_back(move(rc));
      }
    }
    // a bucket fills from any water source, no recipe has that
    if (key_index.count("minecraft:water_bucket:0"))
      recipes.push_back({type_of("fill"), 0, 0, 0, 0, {{1, {intern("minecraft:bucket:0")}}}, {{intern("minecraft:water_bucket:0"), 1, 10000}}});
    if (exists(dir / "fluids.json")) {
      // keep the document alive, items() only refers into it
      json fl = json::parse(readFile(dir / "fluids.json"));
      for (auto &[k, v] : fl.items()) fluid_names[k] = v.get<string>();
    }
    // Tinkers' Construct's stations, by their names in game
    machine_names["tconstruct.table"] = "Casting Table";
    machine_names["tconstruct.basin"] = "Casting Basin";
    machine_names["tconstruct.melting"] = "Smeltery";
    machine_names["tconstruct.alloy"] = "Smeltery (alloying)";
    if (exists(dir / "recipe_maps.json")) {
      json rm = json::parse(readFile(dir / "recipe_maps.json"));
      for (auto &[k, v] : rm.items()) machine_names[k] = v.get<string>();
    }
    if (exists(dir / "item_icons.json") && exists(dir / "item_icons.png")) {
      json ic = json::parse(readFile(dir / "item_icons.json"));
      icon_size = ic["size"].get<int>();
      icon_columns = ic["columns"].get<int>();
      for (auto &[k, v] : ic["icons"].items()) icon_of[k] = v.get<int>();
      // items whose pictured damages all have one name: any of their pictures stands for all of them
      map<string, set<string>> names_of;
      for (auto &[k, v] : icon_of)
        if (k.compare(0, 6, "fluid:") != 0) names_of[k.substr(0, k.rfind(':'))].insert(namer(k));
      for (auto &[k, v] : icon_of) {
        string item = k.substr(0, k.rfind(':'));
        if (k.compare(0, 6, "fluid:") != 0 && names_of[item].size() == 1) icon_of_item.emplace(item, v);
      }
      icons_png = readFile(dir / "item_icons.png");
      char v[17];
      snprintf(v, sizeof(v), "%016zx", hash<string>{}(icons_png));
      icons_version = v;
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
  recipe_sig.assign(recipes.size(), "");
  for (int r = 0; r < (int)recipes.size(); r++) {
    string text = types[recipes[r].type];
    for (auto &slot : recipes[r].in) {
      text += "|" + to_string(slot.count);
      for (int a : slot.alts) text += "," + keys[a];
    }
    for (auto &o : recipes[r].out) text += ">" + keys[o.key] + "x" + to_string(o.amount);
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    recipe_sig[r] = buf;
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
  // assembled: something put together from different things (a motor, a conveyor module, a machine), not one
  // material in another form. GregTech's material items (ingots, plates, rods, wires, dusts, a rubber sheet:
  // gt.metaitem.01 below 32000) are always forms; anything else is assembled if any recipe making it (that isn't
  // breaking something down) combines two or more different things. any, not every: somewhere in the pack a one
  // ingredient recipe gives LV motors, that doesn't make a motor raw material
  vector<signed char> assembled(keys.size(), -1);
  auto is_assembled = [&](int k) {
    if (assembled[k] >= 0) return (bool)assembled[k];
    const string &key = keys[k];
    // fluids are materials (a gas made from air isn't put together), so are GregTech's material items
    if (key.compare(0, 6, "fluid:") == 0 || key.starts_with("bartworks:gt.bwMetaGenerateddust") ||
        (key.compare(0, 23, "gregtech:gt.metaitem.01") == 0 && atoi(key.c_str() + key.rfind(':') + 1) < 32000))
      return (bool)(assembled[k] = 0);
    if (key.starts_with("gregtech:gt.metaitem.01:") && atoi(key.c_str() + key.rfind(':') + 1) >= 32000)
      return (bool)(assembled[k] = 1);
    // GregTech's springs, small springs and superdense plates are made components. Recovering their metal is scrapping.
    if (key.starts_with("gregtech:gt.metaitem.02:")) {
      int meta = atoi(key.c_str() + key.rfind(':') + 1);
      if (meta >= 23000 && meta < 25000) return (bool)(assembled[k] = 1);
    }
    if (key.starts_with("gregtech:gt.metaitem.03:")) {
      int meta = atoi(key.c_str() + key.rfind(':') + 1);
      if (meta >= 6000 && meta < 7000) return (bool)(assembled[k] = 1);
    }
    // The synthesized Gallium Arsenide crystals are material forms too: macerating one into dust is a way to
    // make more dust from gallium and arsenic, not dismantling a finished component.
    if (key == "gregtech:gt.metaitem.03:32190" || key == "gregtech:gt.metaitem.03:32191")
      return (bool)(assembled[k] = 0);
    // GregTech's placed frames, casings, pipes, cables and machines are built things even when made from one
    // material (macerating a steel frame back to dust is scrapping it).
    if (key.starts_with("gregtech:gt.blockmachines") || key.starts_with("gregtech:gt.blockframes") ||
        key.starts_with("gregtech:gt.blockcasings")) return (bool)(assembled[k] = 1);
    bool any = false;
    for (int r : made_by[k]) {
      // every way counts, even ones the loose recycling check flags (a cable is made from wire and rubber, cables
      // can be stripped back to wire so that looks like recycling: it still says a cable is put together)
      set<int> used;
      for (auto &slot : recipes[r].in)
        if (slot.count > 0 && !slot.alts.empty()) used.insert(slot.alts[0]);
      any |= used.size() >= 2;
    }
    return (bool)(assembled[k] = any);
  };
  // scrapping: taking in something assembled and giving back only material forms and fluids, in whatever machine
  // (macerating a conveyor module to rubber pulp and dusts, smelting or alloy smelting a motor back to a rubber bar).
  // building puts out something assembled, an assembler putting logistics pipes and a circuit into a chassis is
  // fine. emptying a cell in the fluid canner is fine too
  // vanilla raw materials (redstone, glowstone, ingots, coal, gems, dyes): materials however some modded recipe
  // also puts them together, as long as one way makes them from a single thing (smelting, crushing ore,
  // crafting from piles or nuggets). a spray can extracted to redstone dust and tin is scrapping
  auto vanilla_material = [&](int k) {
    if (keys[k].compare(0, 10, "minecraft:") != 0) return false;
    for (int r : made_by[k]) {
      set<int> used;
      for (auto &slot : recipes[r].in)
        if (slot.count > 0 && !slot.alts.empty()) used.insert(slot.alts[0]);
      if (used.size() == 1) return true;
    }
    return false;
  };
  plannable.assign(recipes.size(), true);
  for (int r = 0; r < (int)recipes.size(); r++) {
    if (!recipes[r].out.empty() && all_of(recipes[r].out.begin(), recipes[r].out.end(), [](const Out &o) { return single_use_tooling(o.key); })) {
      plannable[r] = false;
      continue;
    }
    if (types[recipes[r].type] == "gt.recipe.fluidcanner") continue;
    bool takes = false, gives = false;
    for (auto &slot : recipes[r].in)
      if (slot.count > 0 && !slot.alts.empty() && !vanilla_material(slot.alts[0])) {
        // An empty cell in a chemical recipe is packaging for the new fluid, not an assembled ingredient being
        // broken down (iron dust + acid + empty cells makes Iron III Chloride).
        if (types[recipes[r].type] == "gt.recipe.chemicalreactor" && keys[slot.alts[0]] == "IC2:itemCellEmpty:0") continue;
        takes |= is_assembled(slot.alts[0]);
      }
    if (!takes) continue;
    for (auto &o : recipes[r].out)
      if (keys[o.key].compare(0, 6, "fluid:") != 0 && !vanilla_material(o.key)) gives |= is_assembled(o.key);
    plannable[r] = gives;
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

void foods_set_sources(map<string, vector<string>> s) {
  lock_guard<mutex> l(state_lock);
  sources = move(s);
}
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

// Whether another unit can be made without consuming the stock of that same item. Valuation follows the cheapest
// path, which may loop through held stock even when a dearer independent path exists (Gallium Arsenide crystals
// from gallium and arsenic, then dust and small piles).
bool can_make_more(const Solved &s, int target) {
  set<int> stack;
  int budget = 10000;
  function<bool(int)> make = [&](int k) {
    if (k != target && cost_of(s, k) == 0) return true;
    if (budget-- <= 0 || !stack.insert(k).second) return false;
    bool ok = false;
    if (wild[k]) {
      for (int a : keys_of_name[name_of[k]])
        if (a != k && make(a)) { ok = true; break; }
    } else {
      vector<pair<double, int>> options;
      for (int r : made_by[k])
        if (allowed(s, r) && machine_ok(s, recipes[r]))
          options.emplace_back(route_score(s, recipes[r], k), r);
      sort(options.begin(), options.end());
      for (auto &option : options) {
        const Recipe &rc = recipes[option.second];
        if (!owns(s, rc)) {
          int m = machine_to_build(s, rc);
          if (m < 0 || !make(m)) continue;
        }
        bool inputs = true;
        for (auto &slot : rc.in) {
          if (slot.alts.empty()) continue;
          bool one = false;
          for (int a : slot.alts)
            if (usable(s, slot, a) && make(a)) { one = true; break; }
          if (!one) { inputs = false; break; }
        }
        if (inputs) { ok = true; break; }
      }
    }
    stack.erase(k);
    return ok;
  };
  return make(target);
}

string recipes_item_json(const string &key, const string &group, int force, double qty, const set<string> &banned_sigs) {
  // turned off recipes come as fingerprints: every recipe with one (identical recipes share it)
  set<int> banned;
  if (!banned_sigs.empty())
    for (int r = 0; r < (int)recipes.size(); r++)
      if (banned_sigs.count(recipe_sig[r])) banned.insert(r);
  lock_guard<mutex> l(state_lock);
  auto f = key_index.find(key);
  if (!loaded || f == key_index.end()) return "{\"error\":\"unknown item\"}";
  int k = f->second;
  if (qty <= 0) qty = wild[k] || key.compare(0, 6, "fluid:") != 0 ? 1 : 1000;
  // Compare a plan using current machines with one that may build machines. The machine's ingredients and
  // operating power count in the whole-plan score, so a small batch won't pay for an unnecessary upgrade.
  shared_ptr<Solved> now = solved(group, false, banned), build = solved(group, true, banned);
  shared_ptr<Solved> s = now->dist[k] < INF ? now : build;
  // a recipe picked to plan with that only works by building a machine somewhere along it: plan with building
  auto runs = [&](const Solved &v, int r) { return allowed(v, r) && machine_ok(v, recipes[r]) && run_cost(v, recipes[r]) < INFINITY; };
  bool pick = force >= 0 && force < (int)recipes.size() && find(made_by[k].begin(), made_by[k].end(), force) != made_by[k].end();
  if (pick && !runs(*s, force) && runs(*build, force)) s = build;
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
    order.emplace_back((ok ? 0 : machine_ok(*build, rc) ? 1 : 2) * 2 + !plannable[r], worst, rc.eut, r);
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
    // scored from what the plan uses (the machines the group has, when that works), otherwise with building
    // machines along the way, and then the card says so
    double c = state / 2 == 0 && allowed(*now, r) ? route_score(*now, recipes[r], k, qty) : INFINITY;
    if (banned.count(r)) rj["banned"] = true;
    if (c == INFINITY && state / 2 == 0 && allowed(*build, r)) {
      c = route_score(*build, recipes[r], k, qty);
      if (c < INFINITY) rj["needs_build"] = true;
    }
    rj["cost"] = c == INFINITY ? json(nullptr) : json(c);
    producers.push_back(rj);
  }
  out["producers"] = producers;
  out["producers_total"] = made_by[k].size();
  // the group's thriftiest route, as a tree, or the one through the recipe the page picked
  bool forced = false;
  if (pick && runs(*s, force)) forced = true;
  json nodes = json::object(), jr = json::object();
  set<int> seen;
  auto gi = snapshot.groups.find(group);
  const FoodGroup *gp = gi != snapshot.groups.end() ? &gi->second : nullptr;
  build_plan(*s, gp, k, qty, forced ? force : -1, nodes, jr, named);
  if (!forced && s == now && build != now) {
    json build_nodes = json::object(), build_jr = json::object();
    set<int> build_named;
    build_plan(*build, gp, k, qty, -1, build_nodes, build_jr, build_named);
    if (auto root = build_nodes.find(key); root != build_nodes.end() && root->size() >= 3 && !(*root)[0].is_null() &&
        plan_total(*now, gp, k, qty, build_nodes, build_jr) < plan_total(*now, gp, k, qty, nodes, jr)) {
      nodes = move(build_nodes), jr = move(build_jr);
      named.insert(build_named.begin(), build_named.end());
      s = build;
    }
  }
  if (s->dist[k] >= INF) {
    nodes = json::object(), jr = json::object();
    MissingPlan path{*s, nodes, jr, named, Missing{*s}};
    int best_tier = 0;
    for (size_t t = 0; t < types.size(); t++)
      if (!type_free[t] && types[t].starts_with("gt.recipe.") && s->tier[t] >= 0 && s->tier[t] < 9)
        best_tier = max(best_tier, s->tier[t]);
    int target_tier = 0;
    const vector<string> tiers = {"ULV", "LV", "MV", "HV", "EV", "IV", "LuV", "ZPM", "UV"};
    auto tier_hint = [&](int item) {
      const string name = display(item);
      for (int tier = 8; tier >= 1; tier--)
        if (name.find("(" + tiers[tier] + ")") != string::npos || name.starts_with(tiers[tier] + " ") ||
            name.find(" " + tiers[tier] + " ") != string::npos) return tier;
      return 0;
    };
    target_tier = tier_hint(k);
    for (int r : made_by[k])
      if (allowed(*s, r))
        for (auto &slot : recipes[r].in)
          for (int a : slot.alts) target_tier = max(target_tier, tier_hint(a));
    path.horizon = min(8, max(best_tier + 1, target_tier));
    path.base_tier = best_tier;
    if (!path.add(k)) {
      path.back_to(0);
      path.stack.clear();
      path.strict = false;
      path.add(k);
    }
    out["route_limits"] = json::array();
    for (int item : path.limited) if (nodes.contains(keys[item])) out["route_limits"].push_back(keys[item]);
    out["route_horizon"] = path.horizon;
  }
  if (s->dist[k] < INF && !forced) {
    json optimized = full_plan(*build, gp, k, qty, -1, nodes, named);
    if (optimized.is_object()) out["optimized"] = move(optimized);
  }
  out["qty"] = qty;
  // steps the team's crafting modules do by themselves
  for (auto &[key2, n] : nodes.items())
    if (n.size() >= 3 && !n[0].is_null() && n[1].get<int>() >= 0 && automated(*s, recipes[n[1].get<int>()])) {
      if (n.size() < 4) n.push_back(json::object());
      n[3]["auto"] = true;
    }
  out["route"] = {{"nodes", nodes}, {"recipes", jr}};
  if (s->dist[k] >= INF) {
    out["route"]["provisional"] = true;
    set<int> stack;
    json b = blockers(*s, k, 5, stack);
    // name everything in it, and the recipes it names
    function<void(const json &)> walk = [&](const json &x) {
      named.insert(key_index[x["key"].get<string>()]);
      if (x.contains("recipe") && !jr.contains(to_string(x["recipe"].get<int>()))) jr[to_string(x["recipe"].get<int>())] = recipe_json(x["recipe"].get<int>(), named);
      if (x.contains("missing"))
        for (auto &m : x["missing"]) walk(m);
      if (x.contains("machine") && x["machine"].contains("why")) walk(x["machine"]["why"]);
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
    if (allowed(*s, r) && machine_ok(*s, recipes[r])) best = min(best, route_score(*s, recipes[r], k, qty));
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
  // held things the plan starts from: can the group make more, and where else they come from
  json from = json::object(), replace = json::object();
  for (auto &[key2, node] : nodes.items()) {
    if (node.size() != 1 || node[0].is_null() || node[0].get<int>() != 0) continue;
    int kk = key_index[key2];
    if (circuit_key[kk] || s->free[kk]) continue;
    // nothing makes it at all (an ore, a crop, a mob drop): it's gathered, not something that runs out
    // gathered: nothing makes it at all (an ore, a crop, a mob drop). "build": more can be made once a machine is
    // built (an MV bending machine for the empty shape plate a mold is pressed from)
    replace[key2] = made_by[kk].empty() ? json("gathered")
                  : now->replaceable[kk] || can_make_more(*now, kk) ? json(true)
                  : build->replaceable[kk] || can_make_more(*build, kk) ? json("build") : json(false);
    string lookup = wild[kk] ? key2.substr(0, key2.rfind(':')) + ":0" : key2;
    if (auto f2 = sources.find(lookup); f2 != sources.end()) from[key2] = f2->second;
  }
  out["sources"] = from;
  out["replaceable"] = replace;
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
