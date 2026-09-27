// Ext.Stats' submodules: spell sets, equipment sets, treasure tables and
// categories, item combinations and their previews and properties, item and
// name groups. src/vendor/stats_sets.cpp reads and writes the engine's
// managers; lua_host.cpp turns the results into upstream's Lua shapes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bg3le::stats_sets {

enum class Kind {
    SpellSet,
    EquipmentSet,
    TreasureTable,
    TreasureCategory,
    ItemCombo,
    ItemComboPreview,
    ItemComboProperty,
    ItemGroup,
    NameGroup,
};

// The vendored type an element is read as, for PointedObject.
char const* type_name(Kind kind);

std::vector<void*> all(Kind kind);
void* get(Kind kind, char const* name);
// Upstream's Create*: a new, empty element under that name.
void* create(Kind kind, char const* name, std::string* why);

// Upstream's LuaSerializer shapes for TreasureTable and TreasureCategory.
struct CategoryRef {
    int Frequency = 1;
    bool IsTable = false;
    std::string Name;  // the table or category it draws from
    std::uint16_t Frequencies[7] = {};
};
struct DropCount {
    int Chance = 0;
    int Amount = 0;
};
struct SubTable {
    int TotalCount = 0;
    int StartLevel = 0;
    int EndLevel = 0;
    std::vector<CategoryRef> Categories;
    std::vector<DropCount> DropCounts;
};
struct Table {
    std::string Name;
    int MinLevel = 0;
    int MaxLevel = 0;
    bool IgnoreLevelDiff = false;
    bool UseTreasureGroupContainers = false;
    bool CanMerge = false;
    std::vector<SubTable> SubTables;
};
struct CategoryItem {
    std::string Name;
    int Priority = 1;
    int MinAmount = 1;
    int MaxAmount = 1;
    int ActPart = 0;
    int Unique = 0;
    int MinLevel = 0;
    int MaxLevel = 0;
};
struct Category {
    std::string Name;
    std::vector<CategoryItem> Items;
};

// RPGStats::TreasureRarities, the frequency keys of a CategoryRef.
std::vector<std::string> rarities();

bool read_table(char const* name, Table* out);
bool read_category(char const* name, Category* out);
bool update_table(Table const& table, std::string* why);
bool update_category(char const* name, Category const& category, std::string* why);

}  // namespace bg3le::stats_sets
