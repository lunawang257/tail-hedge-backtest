#include "wbt/data.hpp"
#include "wbt/yield.hpp"
#include "wbt/strategy.hpp"
#include "wbt/types.hpp"
#include "wbt/chain.hpp"
#include "wbt/verbose_log.hpp"

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
#include <cctype>
#include <filesystem>
#include <stdexcept>

using namespace wbt;

namespace {

struct SearchConfig {
  int short_dte = 0;
  int long_dte = 0;
  double short_delta = 0;
  double long_delta = 0;
  WingMode wing_mode = WingMode::Fixed;
  double m_call = 2, m_put = 2;  // Fixed: long/short ratio
  double credit_buffer = 0.02;
  double theta_threshold = 0.0;
  int max_wing_mult = 4;
  int units = 10;
  double hedge_ratio = 0;  // 0 = unused (size via units)
  double call_put_ratio_pct = 100;  // calls as % of puts (100 = equal)
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

WParam to_param(const SearchConfig& c, const std::string& symbol,
                double opt_comm, double opt_slip, double long_pos_mil,
                double lt_tax, double st_tax,
                const MergeSplitHistory& merge_split) {
  WParam p;
  p.fixed.symbol = symbol;
  p.fixed.opt_comm_per_contract = opt_comm;
  p.fixed.opt_slippage_percent = opt_slip;
  p.fixed.long_term_tax = lt_tax;
  p.fixed.short_term_tax = st_tax;
  p.fixed.merge_split = merge_split;
  p.short_dte = c.short_dte;
  p.long_dte = c.long_dte;
  p.short_delta = c.short_delta;
  p.long_delta = c.long_delta;
  if (c.wing_mode == WingMode::Theta) {
    p.n_call = kThetaShortPerUnit;
    p.n_put = kThetaShortPerUnit;
  } else {
    p.n_call = 1;
    p.n_put = 1;
  }
  p.m_call = c.m_call;
  p.m_put = c.m_put;
  p.units = c.units;
  p.long_pos_mil = long_pos_mil;
  p.hedge_ratio = c.hedge_ratio;
  p.call_put_ratio_pct = c.call_put_ratio_pct;
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
//   --wings fixed:2,fixed:1.3,credit
//   --roll-dtes 0,7,14,21
//   --profit-takes off,0.75
//   --loss-cuts off
// ---------------------------------------------------------------------------

struct WingSpec {
  WingMode mode = WingMode::Fixed;
  // Fixed: long/short contract ratio (e.g. 1.3 → 13 longs per 10 shorts).
  // Must be > 1. Credit/theta ignore this (size chosen at runtime).
  double long_short_ratio = 2.0;
  double credit_buffer = 0.02;
  double theta_threshold = 0.0;
  int max_wing_mult = 4;
};

struct GridSpec {
  std::vector<int> short_dtes = {7, 14, 30, 45};
  std::vector<int> long_dtes = {30, 60, 90};
  std::vector<double> short_deltas = {0.3, 0.4, 0.5};
  std::vector<double> long_deltas = {0.08, 0.12, 0.15, 0.20};
  // fixed(ratio>1), credit, theta — long wings must outnumber short body.
  std::vector<WingSpec> wings = {
    //{WingMode::Fixed, 1.6, 0.02, 0.0, 4},
    //{WingMode::Fixed, 2.0, 0.02, 0.0, 4},
    //{WingMode::Fixed, 2.4, 0.02, 0.0, 4},
    //{WingMode::Credit, 2.0, 0.02, 0.0, 4},
    {WingMode::Theta, 4.0, 0.02, 0.0, 4},
  };
  // -1 in profit_takes / loss_cuts means that exit path is disabled
  std::vector<int> roll_dtes = {0, 7, 14, 21};
  std::vector<double> profit_takes = {-1.0, 5};
  std::vector<double> loss_cuts = {-1.0};
  // Only used when long_pos_mil > 0; otherwise grid uses a single 0 entry.
  std::vector<double> hedge_ratios = {0.5, 1.0, 1.5, 2.0};
  // Calls as a percent of puts (100 = equal, 50 = half, 25 = quarter).
  std::vector<double> call_put_ratio_pcts = {100.0, 50.0, 25.0};
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

std::vector<double> parse_doubles(const std::string& s) {
  std::vector<double> out;
  for (auto& t : split_csv(s)) out.push_back(std::atof(t.c_str()));
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
    double ratio = colon == std::string::npos
                       ? 2.0
                       : std::atof(t.substr(colon + 1).c_str());
    if (mode == "fixed") {
      w.mode = WingMode::Fixed;
      w.long_short_ratio = ratio > 0 ? ratio : 2.0;
      if (w.long_short_ratio <= 1.0) {
        throw std::runtime_error(
            "fixed long_short_ratio must be > 1 (got fixed:" +
            std::to_string(w.long_short_ratio) + ")");
      }
    } else if (mode == "credit") {
      w.mode = WingMode::Credit;
      w.long_short_ratio = 2.0;  // unused; size chosen at runtime
    } else if (mode == "theta") {
      w.mode = WingMode::Theta;
      w.long_short_ratio = 2.0;  // unused; size chosen at runtime
    } else {
      throw std::runtime_error("bad --wings token '" + t +
                               "' (use fixed[:ratio], credit, theta)");
    }
    out.push_back(w);
  }
  return out;
}

std::vector<SearchConfig> build_grid(const GridSpec& g, int units,
                                     double long_pos_mil) {
  std::vector<SearchConfig> out;
  // When no long overlay, hedge_ratio is unused — single sentinel 0.
  std::vector<double> hedge_ratios =
      long_pos_mil > 0 ? g.hedge_ratios : std::vector<double>{0.0};
  if (hedge_ratios.empty()) hedge_ratios = {0.0};

  for (int sd : g.short_dtes) {
    for (int ld : g.long_dtes) {
      if (ld < sd) continue;
      for (double sdel : g.short_deltas) {
        for (double ldel : g.long_deltas) {
          if (!(0 < ldel && ldel < sdel && sdel < 1)) continue;
          for (auto& w : g.wings) {
            // Hard rule: long > short. Fixed ratio must be > 1.
            // Credit: ratio in (1, max_wing_mult]; theta sizes longs from n+1
            // (n = kThetaShortPerUnit) up to n * max_wing_mult.
            if (w.mode == WingMode::Fixed && w.long_short_ratio <= 1.0) continue;
            if (w.mode == WingMode::Credit && w.max_wing_mult <= 1) continue;
            if (w.mode == WingMode::Theta && w.max_wing_mult < 2) continue;
            // Theta: same-expiry shorts and longs net to a plain vertical with
            // no calendar theta edge — skip that degenerate combo.
            if (w.mode == WingMode::Theta && ld == sd) continue;
            for (double hr : hedge_ratios) {
              for (double cp : g.call_put_ratio_pcts) {
                if (!(cp > 0)) continue;
                for (int rd : g.roll_dtes) {
                  // Rolling at/after the short tenor would never hold the body.
                  if (rd >= sd) continue;
                  for (double pt : g.profit_takes) {
                    for (double lc : g.loss_cuts) {
                      SearchConfig c;
                      c.short_dte = sd;
                      c.long_dte = ld;
                      c.short_delta = sdel;
                      c.long_delta = ldel;
                      c.wing_mode = w.mode;
                      c.m_call = w.mode == WingMode::Fixed ? w.long_short_ratio : 2.0;
                      c.m_put = c.m_call;
                      c.credit_buffer = w.credit_buffer;
                      c.theta_threshold = w.theta_threshold;
                      c.max_wing_mult = w.max_wing_mult;
                      c.units = units;
                      c.hedge_ratio = hr;
                      c.call_put_ratio_pct = cp;
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
    }
  }
  return out;
}

std::string format_duration(double sec) {
  if (sec < 0) sec = 0;
  long total = static_cast<long>(sec + 0.5);
  long h = total / 3600;
  long m = (total % 3600) / 60;
  long s = total % 60;
  std::ostringstream os;
  if (h > 0)
    os << h << "h" << std::setfill('0') << std::setw(2) << m << "m"
       << std::setw(2) << s << "s";
  else if (m > 0)
    os << m << "m" << std::setfill('0') << std::setw(2) << s << "s";
  else
    os << s << "s";
  return os.str();
}

void print_grid(const GridSpec& g, size_t n, double long_pos_mil) {
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
    if (w.mode == WingMode::Fixed) std::cerr << "fixed:" << w.long_short_ratio;
    else std::cerr << wing_name(w.mode);
  }
  std::cerr << "}\n"
            << "  rollDTE={" << join_i(g.roll_dtes) << "}\n"
            << "  profitTake={" << join_d(g.profit_takes) << "}\n"
            << "  lossCut={" << join_d(g.loss_cuts) << "}\n"
            << "  callPutRatioPct={" << join_d(g.call_put_ratio_pcts) << "}\n";
  if (long_pos_mil > 0) {
    std::cerr << "  longPosMil=" << long_pos_mil
              << "  hedgeRatio={" << join_d(g.hedge_ratios) << "}\n";
  }
  std::cerr << "  (DTE roll + expiry always on)\n";
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

std::string fmt_trim(double x, int prec = 6) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(prec) << x;
  std::string s = os.str();
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") return "0";
  return s;
}

std::string csv_quote(const std::string& s) {
  bool need = false;
  for (char c : s) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') {
      need = true;
      break;
    }
  }
  if (!need) return s;
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out.push_back('"');
  return out;
}

void write_csv(const std::string& path, std::vector<SearchResult>& rows,
               const std::string& symbol, double opt_comm, double opt_slip,
               double long_pos_mil, double lt_tax, double st_tax,
               const MergeSplitHistory& merge_split,
               const VerboseRunInfo& run) {
  std::sort(rows.begin(), rows.end(),
            [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
  std::ofstream out(path);
  out << "id,hedgeRatio,annualReturn,maxDrawdown,shortDTE,longDTE,shortDelta,longDelta,"
         "wingMode,mCall,mPut,creditBuffer,thetaThreshold,maxWingMult,rollDTE,"
         "rollOnExpiry,enableDteRoll,enableProfitTake,profitTakePct,enableLossCut,"
         "lossCutPct,units,callPutRatioPct,longPosMil,score,finalNLV,totalTax,taxPct,"
         "baselineAnnualReturn,baselineMaxDrawdown,baselineFinalNLV,"
         "longPosShares,unitsUsed,numRolls,W-win,elapsedSec,command\n";
  for (size_t i = 0; i < rows.size(); ++i) {
    auto& r = rows[i];
    auto param = to_param(r.cfg, symbol, opt_comm, opt_slip, long_pos_mil,
                          lt_tax, st_tax, merge_split);
    VerboseRunInfo row_run = run;
    row_run.html_log = symbol + "-" + std::to_string(i) + ".html";
    out << i << ',' << fmt_trim(r.cfg.hedge_ratio) << ','
        << fmt_trim(r.bt.annual_return) << ','
        << fmt_trim(r.bt.max_drawdown) << ','
        << r.cfg.short_dte << ',' << r.cfg.long_dte << ','
        << fmt_trim(r.cfg.short_delta) << ',' << fmt_trim(r.cfg.long_delta) << ','
        << wing_name(r.cfg.wing_mode) << ','
        << fmt_trim(r.cfg.m_call) << ',' << fmt_trim(r.cfg.m_put) << ','
        << fmt_trim(r.cfg.credit_buffer) << ',' << fmt_trim(r.cfg.theta_threshold) << ','
        << r.cfg.max_wing_mult << ',' << r.cfg.roll_dte << ','
        << (r.cfg.roll_on_expiry ? 1 : 0) << ','
        << (r.cfg.enable_dte_roll ? 1 : 0) << ','
        << (r.cfg.enable_profit_take ? 1 : 0) << ','
        << fmt_trim(r.cfg.profit_take_pct) << ','
        << (r.cfg.enable_loss_cut ? 1 : 0) << ','
        << fmt_trim(r.cfg.loss_cut_pct) << ','
        << r.cfg.units << ','
        << fmt_trim(r.cfg.call_put_ratio_pct) << ','
        << fmt_trim(long_pos_mil) << ','
        << fmt_trim(r.score) << ','
        << fmt_trim(r.bt.final_nlv) << ','
        << fmt_trim(r.bt.total_tax) << ','
        << fmt_trim(tax_pct_of_nlv(r.bt.total_tax, r.bt.final_nlv)) << ','
        << fmt_trim(r.bt.baseline_annual_return) << ','
        << fmt_trim(r.bt.baseline_max_drawdown) << ','
        << fmt_trim(r.bt.baseline_final_nlv) << ','
        << fmt_trim(r.bt.long_pos_shares) << ','
        << r.bt.units_used << ',' << r.bt.num_rolls << ','
        << fmt_trim(r.bt.w_win_pct) << ','
        << fmt_trim(r.elapsed_sec, 2) << ','
        << csv_quote(reproduce_command(symbol, param, row_run, true)) << '\n';
  }
}

void usage(const char* argv0) {
  std::cerr
    << "Usage: " << argv0 << " --symbol SYM -i options.csv [options]\n"
    << "  -y yield.csv           Yield curve\n"
    << "  --cache path           Binary cache path\n"
    << "  --iv-smile path.csv    IV smile CSV (default: <data-dir>/<SYM>-ivsmile.csv;\n"
    << "                         build if missing, reuse if present)\n"
    << "  --merge-split path.csv Merge/split history (default: <data-dir>/<SYM>-merge-split.csv).\n"
    << "                         Scales option and stock commissions by remaining split factor:\n"
    << "                         before 2:1 then 7:1 → 1/14 of today's rate, between → 1/7\n"
    << "  --start-date YYYY-MM-DD\n"
    << "  --end-date YYYY-MM-DD\n"
    << "  --every-n N            Session thinning (default 5)\n"
    << "  --atm-band X           Strike band around spot (default 0.20)\n"
    << "  --units N              (default 10; ignored when --long-pos-mil > 0)\n"
    << "  --long-pos-mil X       Starting capital in $millions (default 0).\n"
    << "                         Starts with X*1e6 cash, buys that notional of\n"
    << "                         the symbol on day 1. Opening W credit is held\n"
    << "                         as cash. NLV/MaxDD are W-strategy + long stock.\n"
    << "                         Prints B&H baseline.\n"
    << "  --hedge-ratio X        Single hedge ratio (only with long-pos). Net long\n"
    << "                         puts protect X × long-pos contracts (shares/100).\n"
    << "  --hedge-ratios a,b,..  Grid of hedge ratios (default 0.5,1,1.5,2 when\n"
    << "                         long-pos-mil > 0)\n"
    << "  --call-put-ratio-pct X Calls as % of puts (100 = equal). Single value.\n"
    << "  --call-put-ratio-pcts a,b,..  Grid (default 100,50,25). E.g. 20 means\n"
    << "                         calls are 20% of puts (−100/+200 puts → −20/+40 calls).\n"
    << "  --commission X         Option commission $/contract (default 1.7125)\n"
    << "  --slippage-pct X       Option slippage % of mid; 5 = 5% (default 5)\n"
    << "  --lt-tax-pct X         Long-term capital-gain tax % (default 37.1)\n"
    << "  --st-tax-pct X         Short-term capital-gain tax % (default 54.1)\n"
    << "  --workers N            Threads (default hw concurrency)\n"
    << "  --out results.csv\n"
    << "  --html-log <path>      After the grid, re-run the best-scoring config\n"
    << "                         and write a collapsible HTML trade log\n"
    << "  --limit N              Debug: only first N configs\n"
    << "\n"
    << "Pin / shrink the search grid (comma lists; one value = fixed):\n"
    << "  --short-dtes 7,14,30,45\n"
    << "  --long-dtes 30,60,90\n"
    << "  --short-deltas 0.35,0.45\n"
    << "  --long-deltas 0.08,0.12\n"
    << "  --wings fixed:2,fixed:1.3,credit,theta\n"
    << "  --roll-dtes 0,7,14,21\n"
    << "  --profit-takes off,0.75\n"
    << "  --loss-cuts off\n"
    << "  --call-put-ratio-pcts 100,50,25\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string symbol = "SPY";
  std::string input, yield = "data/yield-curve-rates-1990-2024.csv";
  std::string cache_path, out_path, iv_smile_path, html_log_path;
  std::string merge_split_path;
  Date start = 20180101, end = 20191231;
  int every_n = 5;
  double atm_band = 0.20;
  int units = 10;
  double long_pos_mil = 0;
  bool hedge_ratios_set = false;
  double opt_comm = 1.7125;
  double opt_slip = 0.05;  // decimal; CLI --slippage-pct is percent (5 → 0.05)
  double lt_tax = FixedParam{}.long_term_tax;
  double st_tax = FixedParam{}.short_term_tax;
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
      else if (eq("--merge-split")) merge_split_path = need();
      else if (eq("--start-date")) start = parse_date(need());
      else if (eq("--end-date")) end = parse_date(need());
      else if (eq("--every-n")) every_n = std::atoi(need());
      else if (eq("--atm-band")) atm_band = std::atof(need());
      else if (eq("--units")) units = std::atoi(need());
      else if (eq("--long-pos-mil") || eq("--long-pos"))
        long_pos_mil = std::atof(need());
      else if (eq("--hedge-ratio")) {
        grid_spec.hedge_ratios = {std::atof(need())};
        hedge_ratios_set = true;
      } else if (eq("--hedge-ratios")) {
        grid_spec.hedge_ratios = parse_doubles(need());
        hedge_ratios_set = true;
      }
      else if (eq("--commission")) opt_comm = std::atof(need());
      else if (eq("--slippage-pct")) opt_slip = std::atof(need()) / 100.0;
      else if (eq("--lt-tax-pct") || eq("--long-term-tax-pct"))
        lt_tax = std::atof(need()) / 100.0;
      else if (eq("--st-tax-pct") || eq("--short-term-tax-pct"))
        st_tax = std::atof(need()) / 100.0;
      else if (eq("--workers")) workers = std::atoi(need());
      else if (eq("--out")) out_path = need();
      else if (eq("--html-log")) html_log_path = need();
      else if (eq("--limit")) limit = std::atoi(need());
      else if (eq("--short-dtes")) grid_spec.short_dtes = parse_ints(need());
      else if (eq("--long-dtes")) grid_spec.long_dtes = parse_ints(need());
      else if (eq("--short-deltas")) grid_spec.short_deltas = parse_doubles_or_off(need());
      else if (eq("--long-deltas")) grid_spec.long_deltas = parse_doubles_or_off(need());
      else if (eq("--wings")) grid_spec.wings = parse_wings(need());
      else if (eq("--roll-dtes")) grid_spec.roll_dtes = parse_ints(need());
      else if (eq("--profit-takes")) grid_spec.profit_takes = parse_doubles_or_off(need());
      else if (eq("--loss-cuts")) grid_spec.loss_cuts = parse_doubles_or_off(need());
      else if (eq("--call-put-ratio-pct") || eq("--call-put-ratio-pcts"))
        grid_spec.call_put_ratio_pcts = parse_doubles(need());
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
      grid_spec.profit_takes.empty() || grid_spec.loss_cuts.empty() ||
      grid_spec.call_put_ratio_pcts.empty()) {
    std::cerr << "Grid dimension cannot be empty\n";
    return 1;
  }
  for (double cp : grid_spec.call_put_ratio_pcts) {
    if (!(cp > 0)) {
      std::cerr << "call-put-ratio-pct values must be > 0\n";
      return 1;
    }
  }
  if (long_pos_mil > 0) {
    if (grid_spec.hedge_ratios.empty()) {
      std::cerr << "hedge_ratios cannot be empty when long-pos-mil > 0\n";
      return 1;
    }
    for (double hr : grid_spec.hedge_ratios) {
      if (!(hr > 0)) {
        std::cerr << "hedge_ratio values must be > 0\n";
        return 1;
      }
    }
  } else if (hedge_ratios_set) {
    std::cerr << "warning: --hedge-ratio(s) ignored when --long-pos-mil is 0\n";
  }
  if (workers <= 0) workers = 1;
  if (cache_path.empty()) {
    cache_path = "data/cache/" + symbol + "_" + std::to_string(start) + "_" +
                 std::to_string(end) + "_e" + std::to_string(every_n) + "_atm.bin";
  }
  if (out_path.empty()) out_path = "search-" + symbol + "-cpp.csv";
  if (iv_smile_path.empty()) iv_smile_path = default_iv_smile_path(input, symbol);

  MergeSplitHistory merge_split;
  try {
    merge_split = resolve_merge_split(input, merge_split_path, symbol);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  if (merge_split_path.empty())
    merge_split_path = default_merge_split_path(input, symbol);
  if (!std::filesystem::exists(merge_split_path)) merge_split_path.clear();

  load_yield_data(yield);
  auto ds = ensure_cache(input, cache_path, start, end, every_n, atm_band);
  if (ds.dates.empty()) {
    std::cerr << "No data in range\n";
    return 1;
  }
  // Always load or build IV smiles beside the options data (or --iv-smile override).
  ensure_iv_smile_csv(ds, iv_smile_path, 0.45);

  auto grid = build_grid(grid_spec, units, long_pos_mil);
  if (limit > 0 && static_cast<int>(grid.size()) > limit)
    grid.resize(static_cast<size_t>(limit));

  std::cerr << "=== " << symbol << " search: " << grid.size()
            << " configs, " << workers << " workers, days=" << ds.dates.size()
            << " rows=" << ds.quotes.size() << " ===\n";
  print_grid(grid_spec, grid.size(), long_pos_mil);
  std::cerr << std::fixed << std::setprecision(1)
            << "  tax lt=" << (lt_tax * 100.0) << "%  st=" << (st_tax * 100.0)
            << "%\n";

  // Print buy-and-hold baseline once (same for every config).
  if (long_pos_mil > 0) {
    BacktestResult base;
    FixedParam fp;
    fp.merge_split = merge_split;
    buy_and_hold_baseline(ds, long_pos_mil, fp, base);
    std::cerr << std::fixed << std::setprecision(2)
              << "Baseline buy-and-hold (" << long_pos_mil << "M "
              << symbol << "): annual=" << base.baseline_annual_return
              << "%  maxDD=" << base.baseline_max_drawdown
              << "%  finalNLV=" << base.baseline_final_nlv
              << "  shares=" << base.long_pos_shares << "\n";
  }

  std::vector<SearchResult> results(grid.size());
  std::atomic<size_t> next{0};
  std::atomic<size_t> completed{0};
  std::mutex log_mu;
  auto t0 = std::chrono::steady_clock::now();
  auto last_progress_print = t0 - std::chrono::seconds(2);

  auto worker = [&]() {
    while (true) {
      size_t i = next.fetch_add(1);
      if (i >= grid.size()) break;
      auto cfg = grid[i];
      auto param = to_param(cfg, symbol, opt_comm, opt_slip, long_pos_mil,
                            lt_tax, st_tax, merge_split);
      auto t1 = std::chrono::steady_clock::now();
      auto bt = w_strategy(ds, param, false);
      auto t2 = std::chrono::steady_clock::now();
      SearchResult r;
      r.cfg = cfg;
      r.bt = bt;
      r.score = bt.annual_return - 0.5 * bt.max_drawdown;
      r.elapsed_sec = std::chrono::duration<double>(t2 - t1).count();
      results[i] = r;

      size_t done = completed.fetch_add(1) + 1;
      std::lock_guard<std::mutex> lock(log_mu);
      const bool finished = done >= grid.size();
      if (!finished && t2 - last_progress_print < std::chrono::seconds(2))
        continue;
      last_progress_print = t2;
      double elapsed = std::chrono::duration<double>(t2 - t0).count();
      double eta =
          done > 0 ? elapsed * static_cast<double>(grid.size() - done) /
                         static_cast<double>(done)
                   : 0.0;
      double pct =
          100.0 * static_cast<double>(done) / static_cast<double>(grid.size());
      std::cerr << "\rProgress: " << std::fixed << std::setprecision(1) << pct
                << "% (" << done << "/" << grid.size()
                << ")  elapsed: " << format_duration(elapsed)
                << "  ETA: " << format_duration(eta) << "   " << std::flush;
    }
  };

  std::vector<std::thread> threads;
  for (int w = 0; w < workers; ++w) threads.emplace_back(worker);
  for (auto& th : threads) th.join();

  auto t_end = std::chrono::steady_clock::now();

  VerboseRunInfo run;
  run.argv0 = argv[0];
  run.search = true;
  run.input = input;
  run.yield = yield;
  run.cache = cache_path;
  run.iv_smile = iv_smile_path;
  run.merge_split = merge_split_path;
  run.start = start;
  run.end = end;
  run.every_n = every_n;
  run.atm_band = atm_band;
  write_csv(out_path, results, symbol, opt_comm, opt_slip, long_pos_mil, lt_tax,
            st_tax, merge_split, run);

  if (!html_log_path.empty() && !results.empty()) {
    auto best = std::max_element(
        results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b) {
          return a.score < b.score;
        });
    auto param = to_param(best->cfg, symbol, opt_comm, opt_slip,
                          long_pos_mil, lt_tax, st_tax, merge_split);
    std::cerr << "Re-running best config for HTML log...\n";
    auto bt = w_strategy(ds, param, true);
    run.html_log = html_log_path;
    if (write_verbose_html(html_log_path, symbol, param, bt, run))
      std::cerr << "Wrote HTML log (" << bt.verbose_log.size()
                << " days) to " << html_log_path << "\n";
    else
      std::cerr << "Failed to write HTML log: " << html_log_path
                << "\n";
  }

  double total_sec = std::chrono::duration<double>(t_end - t0).count();
  std::cerr << "\nDone in " << format_duration(total_sec) << "  wrote "
            << out_path << "\n";
  std::cerr << "=== TOP 10 " << symbol << " ===\n";
  for (size_t i = 0; i < results.size() && i < 10; ++i) {
    auto& r = results[i];
    std::cerr << "  id=" << i << " score=" << r.score << " ret=" << r.bt.annual_return
              << "% dd=" << r.bt.max_drawdown << "% tax="
              << tax_pct_of_nlv(r.bt.total_tax, r.bt.final_nlv) << "%NLV"
              << " PLc=" << r.bt.call_pnl << " PLp=" << r.bt.put_pnl
              << " W-win=" << r.bt.w_win_pct << "%"
              << " | sDTE=" << r.cfg.short_dte
              << " lDTE=" << r.cfg.long_dte << " sΔ=" << r.cfg.short_delta
              << " lΔ=" << r.cfg.long_delta << " wing=" << wing_name(r.cfg.wing_mode)
              << " M=" << r.cfg.m_call
              << " c/p=" << r.cfg.call_put_ratio_pct << "%";
    if (long_pos_mil > 0)
      std::cerr << " hedge=" << r.cfg.hedge_ratio
                << " unitsUsed=" << r.bt.units_used;
    std::cerr << " rollDTE=" << r.cfg.roll_dte
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
