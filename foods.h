// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// Spice of Life food planner: which foods each player hasn't eaten yet but could make right now from what
// their team holds, with the machines their team owns, and in how many steps. needs texture-dump's foods.json,
// recipes.json and gt_machine_maps.json. item keys are "mod:name:damage", fluids "fluid:name"

// a holder of items: a team, or "player:<name>" for a player in no team
struct FoodGroup {
  std::map<std::string, int64_t> held;  // what's in its claimed chunks' containers and its members' inventories
  std::vector<int> machines;            // GregTech mIDs of its machines, in its claimed chunks
};

struct FoodPlayer {
  std::string name, team, group;
  std::set<std::string> eaten;  // Spice of Life's FullHistory, what /foodlist counts
};

struct FoodSnapshot {
  std::map<std::string, FoodGroup> groups;
  std::vector<FoodPlayer> players;
};

// display name of an item key that isn't a food, from the exporter's items.json
using FoodNamer = std::function<std::string(const std::string &key)>;

bool foods_load(const std::filesystem::path &dump_dir, FoodNamer namer);
bool foods_loaded();
size_t foods_total();
// called by every container scrape, the solve runs only when the page asks
void foods_update(FoodSnapshot s);
// the page's data, solved for the latest snapshot (cached until the next one)
std::string foods_state_json();
extern const char foods_html[];

// the recipe search page: items matching a name, every recipe making one, and a group's cheapest route to it
std::string recipes_search_json(const std::string &query);
std::string recipes_groups_json();
std::string recipes_item_json(const std::string &key, const std::string &group);
extern const char recipes_html[];
