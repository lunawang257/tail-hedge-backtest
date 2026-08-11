#pragma once
#include "types.hpp"
#include "data.hpp"
#include "positions.hpp"

namespace wbt {

// When verbose=true, fills result.trades with every option open/close.
BacktestResult w_strategy(const OptionsDataset& ds, const WParam& param,
                          bool verbose = false);

}  // namespace wbt
