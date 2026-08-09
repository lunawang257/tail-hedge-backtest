#pragma once
#include "types.hpp"
#include "positions.hpp"
#include <vector>
#include <map>
#include <utility>

namespace wbt {

std::tuple<double, std::map<double, double>, std::map<double, double>>
expected_structure_pnl(const std::vector<OptPosition>& opts, double spot, Date as_of,
                       double entry_cash, const std::vector<double>& move_pcts,
                       const std::vector<double>& spots_history, int horizon_days,
                       bool use_empirical = false);

}  // namespace wbt
