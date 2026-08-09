#include "wbt/data.hpp"
#include "wbt/yield.hpp"
#include "wbt/strategy.hpp"
#include "wbt/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <filesystem>

using namespace wbt;

namespace {

struct SearchConfig {
  int short_dte = 0;
  int long_dte = 0;
  double short_delta = 0;
  double long_delta = 0;
  WingMode wing_mode = WingMode::Fixed;
  int m_call = 2, m_put = 2;
  double credit_buffer = 0.02;
  double theta_threshold = 0.0;
  int max_wing_mult = 4;
  int units = 10;
  int roll_dte = 0;
};

struct SearchResult {
  SearchConfig cfg;
  BacktestResult bt;
  double score = -1e100;
  double elapsed_sec = 0;
};

const char* wing_name(WingMode m) {
  switch (m) {
    case WingMode::Fixed: return "fixed";
    case WingMode::Credit: return "credit";
    case WingMode::Theta: return "theta";
  }
  return "?";
}

WParam to_param(const SearchConfig& c, double balance, const std::string& symbol) {
  WParam p;
  p.fixed.initial_balance = balance;
  p.fixed.symbol = symbol;
  p.short_dte = c.short_dte;
  p.long_dte = c.long_dte;
  p.short_delta = c.short_delta;
  p.long_delta = c.long_delta;
  p.n_call = 1;
  p.n_put = 1;
  p.m_call = c.m_call;
  p.m_put = c.m_put;
  p.units = c.units;
  p.wing_mode = c.wing_mode;
  p.credit_buffer = c.credit_buffer;
  p.theta_threshold = c.theta_threshold;
  p.max_wing_mult = c.max_wing_mult;
  p.roll_dte = c.roll_dte;
  p.roll_on_expiry = true;
  p.enable_profit_take = true;
  p.enable_loss_cut = true;
  p.enable_dte_roll = true;
  p.enable_gamma_scalp = false;
  p.eval_expected_pnl = false;
  return p;
}

std::vector<SearchConfig> build_grid(int units) {
  std::vector<int> short_dtes = {5, 7, 14, 30, 45};
  std::vector<int> long_dtes = {30, 60, 90};
  std::vector<double> short_deltas = {0.35, 0.45};
  std::vector<double> long_deltas = {0.08, 0.12};

  struct WingSpec {
    WingMode mode;
    int m;
    double buf;
    double thr;
    int cap;
  };
  std::vector<WingSpec> wings = {
    {WingMode::Fixed, 2, 0.02, 0.0, 4},
    {WingMode::Fixed, 3, 0.02, 0.0, 4},
    {WingMode::Credit, 1, 0.02, 0.0, 4},
    {WingMode::Theta, 1, 0.02, 0.0, 4},
  };

  std::vector<SearchConfig> out;
  for (int sd : short_dtes) {
    for (int ld : long_dtes) {
      if (ld < sd) continue;
      for (double sdel : short_deltas) {
        for (double ldel : long_deltas) {
          if (!(0 < ldel && ldel < sdel && sdel < 1)) continue;
          for (auto& w : wings) {
            SearchConfig c;
            c.short_dte = sd;
            c.long_dte = ld;
            c.short_delta = sdel;
            c.long_delta = ldel;
            c.wing_mode = w.mode;
            c.m_call = w.m;
            c.m_put = w.m;
            c.credit_buffer = w.buf;
            c.theta_threshold = w.thr;
            c.max_wing_mult = w.cap;
            c.units = units;
            c.roll_dte = 0;
            out.push_back(c);
          }
        }
      }
    }
  }
  return out;
}

OptionsDataset ensure_cache(const std::string& csv_path, const std::string& cache_path,
                            Date start, Date end, int every_n, double atm_band) {
  namespace fs = std::filesystem;
  if (fs::exists(cache_path)) {
    try {
      return load_dataset_cache(cache_path);
    } catch (const std::exception& e) {
      std::cerr << "Cache load failed (" << e.what() << "), rebuilding...\n";
    }
  }
  auto ds = load_options_csv(csv_path, start, end, every_n, atm_band);
  fs::create_directories(fs::path(cache_path).parent_path());
  save_dataset_cache(ds, cache_path);
  return ds;
}

void write_csv(const std::string& path, std::vector<SearchResult>& rows) {
  std::sort(rows.begin(), rows.end(),
            [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
  std::ofstream out(path);
  out << "shortDTE,longDTE,shortDelta,longDelta,wingMode,mCall,mPut,"
         "creditBuffer,thetaThreshold,maxWingMult,rollDTE,units,"
         "annualReturn,maxDrawdown,score,finalNLV,numRolls,elapsedSec\n";
  out << std::fixed << std::setprecision(6);
  for (auto& r : rows) {
    out << r.cfg.short_dte << ',' << r.cfg.long_dte << ','
        << r.cfg.short_delta << ',' << r.cfg.long_delta << ','
        << wing_name(r.cfg.wing_mode) << ','
        << r.cfg.m_call << ',' << r.cfg.m_put << ','
        << r.cfg.credit_buffer << ',' << r.cfg.theta_threshold << ','
        << r.cfg.max_wing_mult << ',' << r.cfg.roll_dte << ',' << r.cfg.units << ','
        << r.bt.annual_return << ',' << r.bt.max_drawdown << ',' << r.score << ','
        << r.bt.final_nlv << ',' << r.bt.num_rolls << ','
        << std::setprecision(2) << r.elapsed_sec << std::setprecision(6) << '\n';
  }
}

void usage(const char* argv0) {
  std::cerr
    << "Usage: " << argv0 << " --symbol SYM -i options.csv [options]\n"
    << "  -y yield.csv           Yield curve\n"
    << "  --cache path           Binary cache path\n"
    << "  --start-date YYYY-MM-DD\n"
    << "  --end-date YYYY-MM-DD\n"
    << "  --every-n N            Session thinning (default 5)\n"
    << "  --atm-band X           Strike band around spot (default 0.20)\n"
    << "  --units N              (default 10)\n"
    << "  --balance X\n"
    << "  --workers N            Threads (default hw)\n"
    << "  --out results.csv\n"
    << "  --limit N              Debug: only first N configs\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string symbol = "SPY";
  std::string input, yield = "data/yield-curve-rates-1990-2024.csv";
  std::string cache_path, out_path;
  Date start = 20180101, end = 20191231;
  int every_n = 5;
  double atm_band = 0.20;
  int units = 10;
  double balance = 1e6;
  int workers = static_cast<int>(std::thread::hardware_concurrency());
  if (workers <= 0) workers = 4;
  int limit = 0;

  for (int i = 1; i < argc; ++i) {
    auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
      return argv[++i];
    };
    if (eq("--symbol")) symbol = need();
    else if (eq("-i")) input = need();
    else if (eq("-y")) yield = need();
    else if (eq("--cache")) cache_path = need();
    else if (eq("--start-date")) start = parse_date(need());
    else if (eq("--end-date")) end = parse_date(need());
    else if (eq("--every-n")) every_n = std::atoi(need());
    else if (eq("--atm-band")) atm_band = std::atof(need());
    else if (eq("--units")) units = std::atoi(need());
    else if (eq("--balance")) balance = std::atof(need());
    else if (eq("--workers")) workers = std::atoi(need());
    else if (eq("--out")) out_path = need();
    else if (eq("--limit")) limit = std::atoi(need());
    else if (eq("-h") || eq("--help")) { usage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown arg " << argv[i] << "\n";
      usage(argv[0]);
      return 1;
    }
  }
  if (input.empty()) {
    std::cerr << "Need -i options.csv\n";
    return 1;
  }
  if (cache_path.empty()) {
    cache_path = "data/cache/" + symbol + "_" + std::to_string(start) + "_" +
                 std::to_string(end) + "_e" + std::to_string(every_n) + "_atm.bin";
  }
  if (out_path.empty()) out_path = "search-" + symbol + "-cpp.csv";

  load_yield_data(yield);
  auto ds = ensure_cache(input, cache_path, start, end, every_n, atm_band);
  if (ds.dates.empty()) {
    std::cerr << "No data in range\n";
    return 1;
  }

  auto grid = build_grid(units);
  if (limit > 0 && static_cast<int>(grid.size()) > limit)
    grid.resize(static_cast<size_t>(limit));

  std::cerr << "=== " << symbol << " search: " << grid.size()
            << " configs, " << workers << " workers, days=" << ds.dates.size()
            << " rows=" << ds.quotes.size() << " ===\n";

  std::vector<SearchResult> results(grid.size());
  std::atomic<size_t> next{0};
  std::mutex log_mu;
  auto t0 = std::chrono::steady_clock::now();

  auto worker = [&]() {
    while (true) {
      size_t i = next.fetch_add(1);
      if (i >= grid.size()) break;
      auto cfg = grid[i];
      auto param = to_param(cfg, balance, symbol);
      auto t1 = std::chrono::steady_clock::now();
      auto bt = w_strategy(ds, param, false);
      auto t2 = std::chrono::steady_clock::now();
      SearchResult r;
      r.cfg = cfg;
      r.bt = bt;
      r.score = bt.annual_return - 0.5 * bt.max_drawdown;
      r.elapsed_sec = std::chrono::duration<double>(t2 - t1).count();
      results[i] = r;
      std::lock_guard<std::mutex> lock(log_mu);
      std::cerr << "[" << (i + 1) << "/" << grid.size() << "] score="
                << std::fixed << std::setprecision(2) << r.score
                << " ret=" << r.bt.annual_return << "% dd=" << r.bt.max_drawdown
                << "% (" << r.elapsed_sec << "s) | sDTE=" << cfg.short_dte
                << " lDTE=" << cfg.long_dte << " sΔ=" << cfg.short_delta
                << " lΔ=" << cfg.long_delta << " wing=" << wing_name(cfg.wing_mode)
                << (cfg.wing_mode == WingMode::Fixed
                        ? (" M=" + std::to_string(cfg.m_call))
                        : "")
                << "\n";
    }
  };

  std::vector<std::thread> threads;
  for (int w = 0; w < workers; ++w) threads.emplace_back(worker);
  for (auto& th : threads) th.join();

  auto t_end = std::chrono::steady_clock::now();
  write_csv(out_path, results);

  std::cerr << "Done in "
            << std::chrono::duration<double>(t_end - t0).count()
            << "s  wrote " << out_path << "\n";
  std::cerr << "=== TOP 10 " << symbol << " ===\n";
  for (size_t i = 0; i < results.size() && i < 10; ++i) {
    auto& r = results[i];
    std::cerr << "  score=" << r.score << " ret=" << r.bt.annual_return
              << "% dd=" << r.bt.max_drawdown << "% | sDTE=" << r.cfg.short_dte
              << " lDTE=" << r.cfg.long_dte << " sΔ=" << r.cfg.short_delta
              << " lΔ=" << r.cfg.long_delta << " wing=" << wing_name(r.cfg.wing_mode)
              << " M=" << r.cfg.m_call << "\n";
  }
  return 0;
}
