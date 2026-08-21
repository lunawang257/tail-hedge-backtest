#pragma once
#include "types.hpp"
#include <string>

namespace wbt {

struct WParam;

// Paths and calendar used for this HTML log, so the page can print a
// copy-paste command that regenerates the same file.
struct VerboseRunInfo {
  std::string argv0;       // binary as invoked (e.g. ./cpp/build/w_search)
  bool search = false;     // true → w_search flags; false → w_backtest
  std::string input;
  std::string yield;
  std::string cache;
  std::string iv_smile;
  std::string merge_split;
  std::string verbose_log;
  Date start = 0;
  Date end = 0;
  int every_n = 1;
  double atm_band = 0;
};

// Write a collapsible HTML trade log from result.verbose_log.
// Returns false if the file could not be opened.
bool write_verbose_html(const std::string& path, const std::string& symbol,
                        const WParam& param, const BacktestResult& result,
                        const VerboseRunInfo& run = {});

}  // namespace wbt
