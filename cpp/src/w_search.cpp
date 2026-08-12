#include "wbt/data.hpp"
#include "wbt/yield.hpp"
#include "wbt/strategy.hpp"
#include "wbt/types.hpp"
#include "wbt/chain.hpp"

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
  bool roll_on_expiry = true;
  bool enable_dte_roll = true;
  bool enable_profit_take = true;
  double profit_take_pct = 0.50;
  bool enable_loss_cut = true;
  double loss_cut_pct = 1.00;
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

WParam to_param(const SearchConfig& c, double balance, const std::string& symbol,
                double opt_comm, double opt_slip) {
  WParam p;
  p.fixed.initial_balance = balance;
  p.fixed.symbol = symbol;
  p.fixed.opt_comm_per_contract = opt_comm;
  p.fixed.opt_slippage_percent = opt_slip;
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
  p.roll_on_expiry = c.roll_on_expiry;
  p.enable_dte_roll = c.enable_dte_roll;
  p.enable_profit_take = c.enable_profit_take;
  p.profit_take_pct = c.profit_take_pct;
  p.enable_loss_cut = c.enable_loss_cut;
  p.loss_cut_pct = c.loss_cut_pct;
  p.enable_gamma_scalp = false;
  p.eval_expected_pnl = false;
  return p;
}

// ---------------------------------------------------------------------------
// Search parameter space.
//
// Edit the defaults below, or override any dimension from the CLI without
// rebuilding (comma-separated lists; a single value pins that dimension):
//   --short-dtes 30
//   --long-dtes 60,90
//   --short-deltas 0.45
//   --long-deltas 0.08,0.12
//   --wings fixed:2,credit
//   --roll-dtes 0,7
//   --profit-takes off,0.50
//   --loss-cuts off,1.00
// ---------------------------------------------------------------------------

struct WingSpec {
  WingMode mode = WingMode::Fixed;
  int m = 2;                 // used when mode == Fixed (m_call = m_put = m)
  double credit_buffer = 0.02;
  double theta_threshold = 0.0;
  int max_wing_mult = 4;
};

struct GridSpec {
  std::vector<int> short_dtes = {5, 7, 14, 30, 45};
  std::vector<int> long_dtes = {30, 60, 90};
  std::vector<double> short_deltas = {0.35, 0.45};
  std::vector<double> long_deltas = {0.08, 0.12};
  // fixed(M>=2), credit, theta — long wings must outnumber short body (n=1).
  std::vector<WingSpec> wings = {
    {WingMode::Fixed, 2, 0.02, 0.0, 4},
    {WingMode::Fixed, 3, 0.02, 0.0, 4},
    {WingMode::Credit, 2, 0.02, 0.0, 4},
    {WingMode::Theta, 2, 0.02, 0.0, 4},
  };
  // -1 in profit_takes / loss_cuts means that exit path is disabled
  std::vector<int> roll_dtes = {0, 3, 7};
  std::vector<double> profit_takes = {-1.0, 0.25, 0.50, 0.75};
  std::vector<double> loss_cuts = {-1.0, 0.50, 1.00};
};

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<int> parse_ints(const std::string& s) {
  std::vector<int> out;
  for (auto& t : split_csv(s)) out.push_back(std::atoi(t.c_str()));
  return out;
}

std::vector<double> parse_doubles_or_off(const std::string& s) {
  std::vector<double> out;
  for (auto& t : split_csv(s)) {
    if (t == "off" || t == "-" || t == "none") out.push_back(-1.0);
    else out.push_back(std::atof(t.c_str()));
  }
  return out;
}

std::vector<WingSpec> parse_wings(const std::string& s) {
  std::vector<WingSpec> out;
  for (auto& t : split_csv(s)) {
    WingSpec w;
    auto colon = t.find(':');
    std::string mode = colon == std::string::npos ? t : t.substr(0, colon);
    int m = colon == std::string::npos ? 2 : std::atoi(t.substr(colon + 1).c_str());
    if (mode == "fixed") {
      w.mode = WingMode::Fixed;
      w.m = m > 0 ? m : 2;
      if (w.m <= 1) {
        throw std::runtime_error(
            "fixed wing M must be > short body (got fixed:" +
            std::to_string(w.m) + "; need M>=2)");
      }
    } else if (mode == "credit") {
      w.mode = WingMode::Credit;
      w.m = 2;  // floor marker; actual size chosen at runtime (>= 2)
    } else if (mode == "theta") {
      w.mode = WingMode::Theta;
      w.m = 2;  // floor marker; actual size chosen at runtime (>= 2)
    } else {
      throw std::runtime_error("bad --wings token '" + t +
                               "' (use fixed[:M], credit, theta)");
    }
    out.push_back(w);
  }
  return out;
}

std::vector<SearchConfig> build_grid(const GridSpec& g, int units) {
  std::vector<SearchConfig> out;
  for (int sd : g.short_dtes) {
    for (int ld : g.long_dtes) {
      if (ld < sd) continue;
      for (double sdel : g.short_deltas) {
        for (double ldel : g.long_deltas) {
          if (!(0 < ldel && ldel < sdel && sdel < 1)) continue;
          for (auto& w : g.wings) {
            // Hard rule: long > short. Search body is n_call=n_put=1, so
            // fixed M must be >= 2; credit/theta enforce the floor at runtime.
            if (w.mode == WingMode::Fixed && w.m <= 1) continue;
            if (w.max_wing_mult < 2 &&
                (w.mode == WingMode::Credit || w.mode == WingMode::Theta)) {
              continue;
            }
            for (int rd : g.roll_dtes) {
              for (double pt : g.profit_takes) {
                for (double lc : g.loss_cuts) {
                  SearchConfig c;
                  c.short_dte = sd;
                  c.long_dte = ld;
                  c.short_delta = sdel;
                  c.long_delta = ldel;
                  c.wing_mode = w.mode;
                  c.m_call = w.mode == WingMode::Fixed ? w.m : std::max(w.m, 2);
                  c.m_put = c.m_call;
                  c.credit_buffer = w.credit_buffer;
                  c.theta_threshold = w.theta_threshold;
                  c.max_wing_mult = w.max_wing_mult;
                  c.units = units;
                  c.roll_dte = rd;
                  c.roll_on_expiry = true;
                  c.enable_dte_roll = true;
                  c.enable_profit_take = pt > 0;
                  c.profit_take_pct = pt > 0 ? pt : 0.50;
                  c.enable_loss_cut = lc > 0;
                  c.loss_cut_pct = lc > 0 ? lc : 1.00;
                  out.push_back(c);
                }
              }
            }
          }
        }
      }
    }
  }
  return out;
}

void print_grid(const GridSpec& g, size_t n) {
  auto join_i = [](const std::vector<int>& v) {
    std::ostringstream os;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) os << ',';
      os << v[i];
    }
    return os.str();
  };
  auto join_d = [](const std::vector<double>& v) {
    std::ostringstream os;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) os << ',';
      if (v[i] < 0) os << "off";
      else os << v[i];
    }
    return os.str();
  };
  std::cerr << "Grid (" << n << " configs after filters):\n"
            << "  shortDTE={" << join_i(g.short_dtes) << "}\n"
            << "  longDTE={" << join_i(g.long_dtes) << "} (>= short)\n"
            << "  shortDelta={" << join_d(g.short_deltas) << "}\n"
            << "  longDelta={" << join_d(g.long_deltas) << "} (< short)\n"
            << "  wings={";
  for (size_t i = 0; i < g.wings.size(); ++i) {
    if (i) std::cerr << ',';
    auto& w = g.wings[i];
    if (w.mode == WingMode::Fixed) std::cerr << "fixed:" << w.m;
    else std::cerr << wing_name(w.mode);
  }
  std::cerr << "}\n"
            << "  rollDTE={" << join_i(g.roll_dtes) << "}\n"
            << "  profitTake={" << join_d(g.profit_takes) << "}\n"
            << "  lossCut={" << join_d(g.loss_cuts) << "}\n"
            << "  (DTE roll + expiry always on)\n";
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
         "creditBuffer,thetaThreshold,maxWingMult,rollDTE,rollOnExpiry,"
         "enableDteRoll,enableProfitTake,profitTakePct,enableLossCut,lossCutPct,"
         "units,annualReturn,maxDrawdown,score,finalNLV,numRolls,elapsedSec\n";
  out << std::fixed << std::setprecision(6);
  for (auto& r : rows) {
    out << r.cfg.short_dte << ',' << r.cfg.long_dte << ','
        << r.cfg.short_delta << ',' << r.cfg.long_delta << ','
        << wing_name(r.cfg.wing_mode) << ','
        << r.cfg.m_call << ',' << r.cfg.m_put << ','
        << r.cfg.credit_buffer << ',' << r.cfg.theta_threshold << ','
        << r.cfg.max_wing_mult << ',' << r.cfg.roll_dte << ','
        << (r.cfg.roll_on_expiry ? 1 : 0) << ','
        << (r.cfg.enable_dte_roll ? 1 : 0) << ','
        << (r.cfg.enable_profit_take ? 1 : 0) << ',' << r.cfg.profit_take_pct << ','
        << (r.cfg.enable_loss_cut ? 1 : 0) << ',' << r.cfg.loss_cut_pct << ','
        << r.cfg.units << ','
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
    << "  --iv-smile path.csv    IV smile CSV (default: <data-dir>/<SYM>-ivsmile.csv;\n"
    << "                         build if missing, reuse if present)\n"
    << "  --start-date YYYY-MM-DD\n"
    << "  --end-date YYYY-MM-DD\n"
    << "  --every-n N            Session thinning (default 5)\n"
    << "  --atm-band X           Strike band around spot (default 0.20)\n"
    << "  --units N              (default 10)\n"
    << "  --balance X\n"
    << "  --commission X         Option commission $/contract (default 1.7125)\n"
    << "  --slippage-pct X       Option slippage % of mid; 5 = 5% (default 5)\n"
    << "  --workers N            Threads (default hw concurrency)\n"
    << "  --out results.csv\n"
    << "  --limit N              Debug: only first N configs\n"
    << "\n"
    << "Pin / shrink the search grid (comma lists; one value = fixed):\n"
    << "  --short-dtes 5,7,14,30,45\n"
    << "  --long-dtes 30,60,90\n"
    << "  --short-deltas 0.35,0.45\n"
    << "  --long-deltas 0.08,0.12\n"
    << "  --wings fixed:2,fixed:3,credit,theta\n"
    << "  --roll-dtes 0,3,7\n"
    << "  --profit-takes off,0.25,0.50,0.75\n"
    << "  --loss-cuts off,0.50,1.00\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string symbol = "SPY";
  std::string input, yield = "data/yield-curve-rates-1990-2024.csv";
  std::string cache_path, out_path, iv_smile_path;
  Date start = 20180101, end = 20191231;
  int every_n = 5;
  double atm_band = 0.20;
  int units = 10;
  double balance = 1e6;
  double opt_comm = 1.7125;
  double opt_slip = 0.05;  // decimal; CLI --slippage-pct is percent (5 → 0.05)
  int workers = static_cast<int>(std::thread::hardware_concurrency());
  if (workers <= 0) workers = 4;
  int limit = 0;
  GridSpec grid_spec;

  for (int i = 1; i < argc; ++i) {
    auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
      return argv[++i];
    };
    try {
      if (eq("--symbol")) symbol = need();
      else if (eq("-i")) input = need();
      else if (eq("-y")) yield = need();
      else if (eq("--cache")) cache_path = need();
      else if (eq("--iv-smile")) iv_smile_path = need();
      else if (eq("--start-date")) start = parse_date(need());
      else if (eq("--end-date")) end = parse_date(need());
      else if (eq("--every-n")) every_n = std::atoi(need());
      else if (eq("--atm-band")) atm_band = std::atof(need());
      else if (eq("--units")) units = std::atoi(need());
      else if (eq("--balance")) balance = std::atof(need());
      else if (eq("--commission")) opt_comm = std::atof(need());
      else if (eq("--slippage-pct")) opt_slip = std::atof(need()) / 100.0;
      else if (eq("--workers")) workers = std::atoi(need());
      else if (eq("--out")) out_path = need();
      else if (eq("--limit")) limit = std::atoi(need());
      else if (eq("--short-dtes")) grid_spec.short_dtes = parse_ints(need());
      else if (eq("--long-dtes")) grid_spec.long_dtes = parse_ints(need());
      else if (eq("--short-deltas")) grid_spec.short_deltas = parse_doubles_or_off(need());
      else if (eq("--long-deltas")) grid_spec.long_deltas = parse_doubles_or_off(need());
      else if (eq("--wings")) grid_spec.wings = parse_wings(need());
      else if (eq("--roll-dtes")) grid_spec.roll_dtes = parse_ints(need());
      else if (eq("--profit-takes")) grid_spec.profit_takes = parse_doubles_or_off(need());
      else if (eq("--loss-cuts")) grid_spec.loss_cuts = parse_doubles_or_off(need());
      else if (eq("-h") || eq("--help")) { usage(argv[0]); return 0; }
      else {
        std::cerr << "Unknown arg " << argv[i] << "\n";
        usage(argv[0]);
        return 1;
      }
    } catch (const std::exception& e) {
      std::cerr << "Bad arg near " << argv[i] << ": " << e.what() << "\n";
      return 1;
    }
  }
  if (input.empty()) {
    std::cerr << "Need -i options.csv\n";
    return 1;
  }
  if (grid_spec.short_dtes.empty() || grid_spec.long_dtes.empty() ||
      grid_spec.short_deltas.empty() || grid_spec.long_deltas.empty() ||
      grid_spec.wings.empty() || grid_spec.roll_dtes.empty() ||
      grid_spec.profit_takes.empty() || grid_spec.loss_cuts.empty()) {
    std::cerr << "Grid dimension cannot be empty\n";
    return 1;
  }
  if (workers <= 0) workers = 1;
  if (cache_path.empty()) {
    cache_path = "data/cache/" + symbol + "_" + std::to_string(start) + "_" +
                 std::to_string(end) + "_e" + std::to_string(every_n) + "_atm.bin";
  }
  if (out_path.empty()) out_path = "search-" + symbol + "-cpp.csv";
  if (iv_smile_path.empty()) iv_smile_path = default_iv_smile_path(input, symbol);

  load_yield_data(yield);
  auto ds = ensure_cache(input, cache_path, start, end, every_n, atm_band);
  if (ds.dates.empty()) {
    std::cerr << "No data in range\n";
    return 1;
  }
  // Always load or build IV smiles beside the options data (or --iv-smile override).
  ensure_iv_smile_csv(ds, iv_smile_path, 0.45);

  auto grid = build_grid(grid_spec, units);
  if (limit > 0 && static_cast<int>(grid.size()) > limit)
    grid.resize(static_cast<size_t>(limit));

  std::cerr << "=== " << symbol << " search: " << grid.size()
            << " configs, " << workers << " workers, days=" << ds.dates.size()
            << " rows=" << ds.quotes.size() << " ===\n";
  print_grid(grid_spec, grid.size());

  std::vector<SearchResult> results(grid.size());
  std::atomic<size_t> next{0};
  std::mutex log_mu;
  auto t0 = std::chrono::steady_clock::now();

  auto worker = [&]() {
    while (true) {
      size_t i = next.fetch_add(1);
      if (i >= grid.size()) break;
      auto cfg = grid[i];
      auto param = to_param(cfg, balance, symbol, opt_comm, opt_slip);
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
                << " rollDTE=" << cfg.roll_dte
                << " pt=" << (cfg.enable_profit_take
                                  ? std::to_string(cfg.profit_take_pct)
                                  : "off")
                << " lc=" << (cfg.enable_loss_cut
                                  ? std::to_string(cfg.loss_cut_pct)
                                  : "off")
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
              << " M=" << r.cfg.m_call
              << " rollDTE=" << r.cfg.roll_dte
              << " pt=" << (r.cfg.enable_profit_take
                                ? std::to_string(r.cfg.profit_take_pct)
                                : "off")
              << " lc=" << (r.cfg.enable_loss_cut
                                ? std::to_string(r.cfg.loss_cut_pct)
                                : "off")
              << "\n";
  }
  return 0;
}
