#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace pbz::res {
// Generated from DT_InventoryData.ItemDetailType=Material and the four
// *UpgradeMaterial types. Excludes soul/quest/progression items and scripts.
// Source SHA-256: 00d60c37c6a8afb1ade2748f15b20ba3534f16561d0754a41b010cdc0168cad0
inline bool StackableMaterial(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    constexpr std::string_view ids[] = {
        "ac_material_1",
        "ac_material_4",
        "medi_material_1",
        "medi_material_2",
        "medi_material_3",
        "weapon_material_01",
        "weapon_material_02",
        "weapon_material_03",
        "weapon_material_04",
        "weapon_material_05",
        "weapon_material_06",
        "weapon_material_07",
        "weapon_material_08",
        "weapon_material_09",
        "weapon_material_10",
        "weapon_material_11",
        "weapon_material_12",
        "weapon_material_13",
        "weapon_material_14",
        "weapon_material_15",
        "weapon_material_16",
        "weapon_material_17",
        "weapon_material_18",
        "weapon_material_19",
        "weapon_material_20",
        "weapon_material_21",
        "weapon_material_22",
        "weapon_material_23",
        "weapon_material_24",
        "weapon_material_25",
        "weapon_material_26",
        "weapon_material_27",
    };
    return std::binary_search(std::begin(ids), std::end(ids), std::string_view(name));
}
}  // namespace pbz::res
