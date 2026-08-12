#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace wbt {

struct OptionsDataset {
  std::vector<OptionQuote> quotes;                 // all rows
  std::vector<Date> dates;                         // sorted unique DataDates
  std::unordered_map<Date, std::pair<size_t, size_t>> day_range;  // [begin,end)
  // Unique expirations per session (ascending), built in rebuild_index.
  std::unordered_map<Date, std::vector<Date>> day_exps;

  const OptionQuote* begin(Date d) const;
  const OptionQuote* end(Date d) const;
  double spot(Date d) const;
  std::vector<Date> expirations(Date d) const;
  // Quotes for (session, expiry, put/call). Relies on sort order from loader:
  // (data_date, exp_date, put_call, strike).
  std::pair<const OptionQuote*, const OptionQuote*>
  quotes_for(Date d, Date exp, OptType type) const;

  void rebuild_index();
};

OptionsDataset load_options_csv(const std::string& path,
                                Date start = 0, Date end = 0,
                                int trade_every_n_days = 1,
                                double atm_band = 0.0);

void adjust_prices_for_splits_and_merges(std::vector<OptionQuote>& quotes);

// Simple binary cache (quotes only + rebuild index)
void save_dataset_cache(const OptionsDataset& ds, const std::string& path);
OptionsDataset load_dataset_cache(const std::string& path);

}  // namespace wbt
