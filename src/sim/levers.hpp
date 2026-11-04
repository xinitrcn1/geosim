// Lever metadata for the player's controls (client tabs): name, unit, legal range, the
// agent kind that owns the lever, and the tab it belongs to. The AI and the player move
// the same levers (systems/ai.cpp `settle`, `apply_override`); bills pass the legislature
// (institutions.cpp `enact`). NPI levers (one per npi_indicator) come first at runtime and
// are described by their indicator; the fixed levers below follow at
// derived_indices::lever_base + lever.

#pragma once

#include "sim/model.hpp"

#include <array>
#include <cstdint>

namespace geosim::sim {

enum class lever_tab : uint8_t { policies = 0, fiscal = 1, monetary = 2, firm = 3, labour = 4, regional = 5 };

struct lever_meta {
    lever id;
    char const* name;
    char const* unit;
    float lo, hi, step;
    agent_kind owner;
    lever_tab tab;
    bool bill; // passes the legislature where it is competitive
};

inline constexpr std::array<lever_meta, static_cast<std::size_t>(lever::count)> lever_table{{
    {lever::fiscal, "Fiscal stimulus (cumulative)", "% of GDP", 0.0f, 30.0f, 0.5f, agent_kind::executive, lever_tab::fiscal, false},
    {lever::stw_coverage, "Short-time work coverage", "share of workers", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::fiscal, false},
    {lever::stw_replacement, "Short-time work replacement", "share of wage", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::fiscal, false},
    {lever::tti, "Test, trace and isolate", "capacity 0-1", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::policy_rate, "Policy rate", "%", -1.0f, 25.0f, 0.25f, agent_kind::central_bank, lever_tab::monetary, false},
    {lever::oil_target, "Oil production target", "mb/d (model units)", 0.0f, 30.0f, 0.1f, agent_kind::executive, lever_tab::policies, false},
    {lever::directed_credit, "Directed state-bank credit", "% of GDP a year", 0.0f, 10.0f, 0.25f, agent_kind::executive, lever_tab::fiscal, false},
    {lever::info_control, "Online media restrictions", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::asset_purchases, "Asset purchases (QE)", "% of GDP a year", 0.0f, 30.0f, 0.5f, agent_kind::central_bank, lever_tab::monetary, false},
    {lever::net_filtering, "Internet filtering", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::tariff, "Average import tariff", "ad valorem", 0.0f, 1.0f, 0.01f, agent_kind::executive, lever_tab::policies, true},
    {lever::hoarding, "Labour hoarding", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::board, lever_tab::firm, false},
    {lever::investment, "Investment drive", "relative to baseline", -1.0f, 1.0f, 0.05f, agent_kind::board, lever_tab::firm, false},
    {lever::tax_scale, "Household tax rate", "x start", 0.0f, 3.0f, 0.02f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::public_investment, "Public investment", "% of GDP above start", -5.0f, 15.0f, 0.25f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::min_wage, "Minimum wage", "x start", 0.0f, 3.0f, 0.02f, agent_kind::executive, lever_tab::policies, true},
    {lever::defense, "Defence spending", "% of GDP", 0.0f, 30.0f, 0.1f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::wage_push, "Real-wage demand", "annual log change", -0.2f, 0.3f, 0.01f, agent_kind::union_federation, lever_tab::labour, false},
    {lever::carbon_price, "Carbon price", "USD per tonne", 0.0f, 1000.0f, 5.0f, agent_kind::executive, lever_tab::policies, true},
    {lever::repression, "Repression of protest", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::immigration, "Immigration openness", "x start", 0.0f, 3.0f, 0.05f, agent_kind::executive, lever_tab::policies, true},
    {lever::retirement_age, "Retirement age change", "years", -5.0f, 10.0f, 0.5f, agent_kind::executive, lever_tab::policies, true},
    {lever::education, "Education spending", "% of GDP above start", -3.0f, 5.0f, 0.1f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::health, "Health spending", "% of GDP above start", -3.0f, 5.0f, 0.1f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::capital_controls, "Capital controls", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::regional_npi, "Regional restrictions", "x national level", 0.0f, 2.0f, 0.05f, agent_kind::governor, lever_tab::regional, false},
    {lever::police, "Police spending", "% of GDP above start", -1.0f, 3.0f, 0.05f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::asylum, "Refugee admission", "x 2019", 0.0f, 3.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::vat, "VAT rate", "pp change", -10.0f, 15.0f, 0.5f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::corporate_tax, "Corporate tax rate", "pp change", -20.0f, 20.0f, 0.5f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::wealth_tax, "Wealth tax", "% a year", 0.0f, 5.0f, 0.1f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::rd, "Public R&D", "% of GDP above start", -1.0f, 3.0f, 0.05f, agent_kind::executive, lever_tab::fiscal, true},
    {lever::subsidy, "Fuel and food price support", "share of rise absorbed", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::fiscal, false},
    {lever::epl, "Employment protection", "OECD index points", -2.0f, 2.0f, 0.1f, agent_kind::executive, lever_tab::policies, true},
    {lever::energy_mandate, "Renewables mandate", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, true},
    {lever::vaccine_priority, "Vaccines to the old first", "share of doses", 0.0f, 1.0f, 0.05f, agent_kind::executive, lever_tab::policies, false},
    {lever::fx_intervention, "FX intervention", "0-1", 0.0f, 1.0f, 0.05f, agent_kind::central_bank, lever_tab::monetary, false},
    {lever::macroprudential, "Countercyclical capital buffer", "pp", 0.0f, 5.0f, 0.25f, agent_kind::central_bank, lever_tab::monetary, false},
}};

static_assert([] {
    for(std::size_t i = 0; i < lever_table.size(); ++i) {
        if(static_cast<std::size_t>(lever_table[i].id) != i) return false;
    }
    return true;
}(), "lever_table must list every lever in enum order");

} // namespace geosim::sim
