#pragma once
#include "types.hpp"
#include "data.hpp"
#include "positions.hpp"

namespace wbt {

// When verbose=true, fills result.trades with every option and stock fill.
BacktestResult w_strategy(const OptionsDataset& ds, const WParam& param,
                          bool verbose = false);

// Pure buy-and-hold of the underlying (no W-strategy). Fills baseline_* and
// long_pos_shares on result. no-op when long_pos_mil <= 0.
void buy_and_hold_baseline(const OptionsDataset& ds, double long_pos_mil,
                           const FixedParam& fp, BacktestResult& result);

}  // namespace wbt
