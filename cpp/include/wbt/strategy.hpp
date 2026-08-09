#pragma once
#include "types.hpp"
#include "data.hpp"
#include "positions.hpp"

namespace wbt {

BacktestResult w_strategy(const OptionsDataset& ds, const WParam& param, bool debug = false);

}  // namespace wbt
