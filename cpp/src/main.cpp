#include "wbt/data.hpp"
#include "wbt/yield.hpp"
#include "wbt/strategy.hpp"
#include "wbt/gbs.hpp"
#include "wbt/chain.hpp"
#include "wbt/verbose_log.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>

using namespace wbt;

static void usage(const char* argv0) {
  std::cerr <<
    "Usage: " << argv0 << " [options]\n"
    "\n"
    "Inputs:\n"
    "  -i <options.csv>         Options chain CSV (.csv or .csv.gz)\n"
    "  -y <yield.csv>           Yield curve CSV\n"
    "  --start-date YYYY-MM-DD  Inclusive start (default: first day in file)\n"
    "  --end-date YYYY-MM-DD    Inclusive end (default: last day in file)\n"
    "  --trade-every-n-days N   Keep every Nth session (default 1)\n"
    "  --iv-smile <path.csv>    IV smile CSV (default: <data-dir>/<stem>-ivsmile.csv;\n"
    "                           build if missing, reuse if present)\n"
    "  --merge-split <path.csv> Merge/split history (default: <data-dir>/<stem>-merge-split.csv).\n"
    "                           Scales option commission by remaining split factor:\n"
    "                           before 2:1 then 7:1 → 1/14 of --commission, between → 1/7\n"
    "  --cache <path.bin>       Binary options cache (load if present, else build+save)\n"
    "  --atm-band X             Keep strikes within ±X of spot when loading (default 0)\n"
    "\n"
    "Structure:\n"
    "  --short-dte N            Target DTE for short (body) legs (default 30)\n"
    "  --long-dte N             Target DTE for long (wing) legs (default 60)\n"
    "  --short-delta X          |delta| target for short legs (default 0.40)\n"
    "  --long-delta X           |delta| target for long wings (default 0.10)\n"
    "  --units N                Structure multiplier (default 1;\n"
    "                           ignored when --long-pos-mil > 0)\n"
    "  --m-call N               Fixed-mode long-call wing count (default 2)\n"
    "  --m-put N                Fixed-mode long-put wing count (default 2)\n"
    "  --balance X              Options-book starting cash (default 1e6)\n"
    "  --long-pos-mil X         Buy-and-hold overlay in $millions (default 0).\n"
    "                           Buys X*1e6 notional of the symbol on day 1;\n"
    "                           NLV/MaxDD = W-strategy + long stock.\n"
    "  --hedge-ratio X          Net long puts protect X × long-pos contracts\n"
    "                           (shares/100). Only with long-pos-mil > 0.\n"
    "  --call-put-ratio-pct X   Call contracts as % of puts (default 100).\n"
    "                           20 → calls are 20% of puts (−100/+200 puts\n"
    "                           → −20/+40 calls).\n"
    "  --commission X           Option commission $/contract (default 1.7125)\n"
    "  --slippage-pct X         Option slippage as percent of mid;\n"
    "                           e.g. 5 means 5% (default 5)\n"
    "  --lt-tax-pct X           Long-term capital-gain tax % (holding > 1y).\n"
    "                           Default 37.1\n"
    "  --st-tax-pct X           Short-term capital-gain tax % (holding <= 1y).\n"
    "                           Default 54.1\n"
    "\n"
    "Exit / roll:\n"
    "  --roll-dte N             Roll when short DTE <= N (default 0)\n"
    "  --dte-roll / --no-dte-roll\n"
    "  --roll-on-expiry / --no-roll-on-expiry\n"
    "  --profit-take-pct X      Take profit at +X of |entry cash| (default 0.50)\n"
    "  --profit-take / --no-profit-take\n"
    "  --loss-cut-pct X         Cut loss at -X of |entry cash| (default 1.00)\n"
    "  --loss-cut / --no-loss-cut\n"
    "\n"
    "Other:\n"
    "  --gamma-scalp / --no-gamma-scalp\n"
    "  --no-expected-pnl\n"
    "  --verbose                Record every option/stock fill to CSV\n"
    "  --verbose-out <path>     Verbose CSV path (implies --verbose;\n"
    "                           default transactions.csv)\n"
    "  --verbose-log <path>     Write a collapsible HTML trade log (implies\n"
    "                           recording fills; year/day fold)\n"
    "  --bench N                Repeat backtest N times for timing\n"
    "  -h, --help\n";
}

static void write_trades_csv(const std::string& path,
                             const std::vector<TradeRecord>& trades,
                             double hedge_ratio, double call_put_ratio_pct) {
  std::ofstream out(path);
  if (!out) {
    std::cerr << "Failed to write verbose CSV: " << path << "\n";
    return;
  }
  out << "date,action,structureId,leg,type,strike,expiry,contracts,shares,"
         "openPrice,closePrice,pnl,reason,spot,hedgeRatio,callPutRatioPct,"
         "bhShares,totalTax,stLossCarry,ltLossCarry,stGain,ltGain,"
         "cash,stockNlv,totalNlv,costBasis,tradeStGain,tradeLtGain\n";
  out << std::fixed << std::setprecision(6);
  for (const auto& t : trades) {
    out << format_date(t.date) << ','
        << t.action << ','
        << t.structure_id << ','
        << t.leg_role << ','
        << t.put_call << ','
        << t.strike << ',';
    if (t.exp_date) out << format_date(t.exp_date);
    out << ','
        << t.num_contracts << ','
        << t.shares << ','
        << t.open_price << ','
        << t.close_price << ','
        << t.pnl << ','
        << t.reason << ','
        << t.spot << ','
        << hedge_ratio << ','
        << call_put_ratio_pct << ','
        << t.bh_shares << ','
        << t.total_tax << ','
        << t.st_loss_carryover << ','
        << t.lt_loss_carryover << ','
        << t.st_capital_gain << ','
        << t.lt_capital_gain << ','
        << t.cash << ','
        << t.stock_nlv << ','
        << t.total_nlv << ','
        << t.cost_basis << ','
        << t.trade_st_gain << ','
        << t.trade_lt_gain << '\n';
  }
}

int main(int argc, char** argv) {
  std::string input, yield = "data/yield-curve-rates-1990-2024.csv";
  std::string iv_smile_path, cache_path;
  std::string merge_split_path;
  WParam param;
  param.enable_gamma_scalp = false;
  param.eval_expected_pnl = false;
  param.roll_dte = 0;
  Date start = 0, end = 0;
  int every_n = 1;
  double atm_band = 0.0;
  int bench = 1;
  bool verbose = false;
  std::string verbose_out = "transactions.csv";
  std::string verbose_log;

  for (int i = 1; i < argc; ++i) {
    auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
      return argv[++i];
    };
    if (eq("-i")) input = need();
    else if (eq("-y")) yield = need();
    else if (eq("--iv-smile")) iv_smile_path = need();
    else if (eq("--merge-split")) merge_split_path = need();
    else if (eq("--cache")) cache_path = need();
    else if (eq("--atm-band")) atm_band = std::atof(need());
    else if (eq("--short-dte")) param.short_dte = std::atoi(need());
    else if (eq("--long-dte")) param.long_dte = std::atoi(need());
    else if (eq("--short-delta")) param.short_delta = std::atof(need());
    else if (eq("--long-delta")) param.long_delta = std::atof(need());
    else if (eq("--units")) param.units = std::atoi(need());
    else if (eq("--balance")) param.fixed.initial_balance = std::atof(need());
    else if (eq("--long-pos-mil") || eq("--long-pos"))
      param.long_pos_mil = std::atof(need());
    else if (eq("--hedge-ratio")) param.hedge_ratio = std::atof(need());
    else if (eq("--call-put-ratio-pct"))
      param.call_put_ratio_pct = std::atof(need());
    else if (eq("--commission"))
      param.fixed.opt_comm_per_contract = std::atof(need());
    else if (eq("--slippage-pct"))
      param.fixed.opt_slippage_percent = std::atof(need()) / 100.0;
    else if (eq("--lt-tax-pct") || eq("--long-term-tax-pct"))
      param.fixed.long_term_tax = std::atof(need()) / 100.0;
    else if (eq("--st-tax-pct") || eq("--short-term-tax-pct"))
      param.fixed.short_term_tax = std::atof(need()) / 100.0;
    else if (eq("--m-call")) param.m_call = std::atof(need());
    else if (eq("--m-put")) param.m_put = std::atof(need());
    else if (eq("--roll-dte")) param.roll_dte = std::atoi(need());
    else if (eq("--dte-roll")) param.enable_dte_roll = true;
    else if (eq("--no-dte-roll")) param.enable_dte_roll = false;
    else if (eq("--roll-on-expiry")) param.roll_on_expiry = true;
    else if (eq("--no-roll-on-expiry")) param.roll_on_expiry = false;
    else if (eq("--profit-take-pct")) {
      param.profit_take_pct = std::atof(need());
      param.enable_profit_take = true;
    }
    else if (eq("--profit-take")) param.enable_profit_take = true;
    else if (eq("--no-profit-take")) param.enable_profit_take = false;
    else if (eq("--loss-cut-pct")) {
      param.loss_cut_pct = std::atof(need());
      param.enable_loss_cut = true;
    }
    else if (eq("--loss-cut")) param.enable_loss_cut = true;
    else if (eq("--no-loss-cut")) param.enable_loss_cut = false;
    else if (eq("--start-date")) start = parse_date(need());
    else if (eq("--end-date")) end = parse_date(need());
    else if (eq("--trade-every-n-days")) every_n = std::atoi(need());
    else if (eq("--no-gamma-scalp")) param.enable_gamma_scalp = false;
    else if (eq("--gamma-scalp")) param.enable_gamma_scalp = true;
    else if (eq("--no-expected-pnl")) param.eval_expected_pnl = false;
    else if (eq("--verbose") || eq("-v")) verbose = true;
    else if (eq("--verbose-out")) {
      verbose_out = need();
      verbose = true;
    }
    else if (eq("--verbose-log")) verbose_log = need();
    else if (eq("--bench")) bench = std::atoi(need());
    else if (eq("-h") || eq("--help")) { usage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown arg: " << argv[i] << "\n";
      usage(argv[0]);
      return 1;
    }
  }
  if (input.empty()) {
    input = "data/SPY_fixture_small.csv";
  }
  if (iv_smile_path.empty()) {
    // Beside the options file: <stem>-ivsmile.csv (e.g. QQQ.csv → QQQ-ivsmile.csv).
    iv_smile_path = default_iv_smile_path(input);
  }
  try {
    param.fixed.merge_split = resolve_merge_split(input, merge_split_path);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  if (merge_split_path.empty())
    merge_split_path = default_merge_split_path(input);
  if (!std::filesystem::exists(merge_split_path)) merge_split_path.clear();

  load_yield_data(yield);
  OptionsDataset ds;
  if (!cache_path.empty()) {
    namespace fs = std::filesystem;
    bool loaded = false;
    if (fs::exists(cache_path)) {
      try {
        ds = load_dataset_cache(cache_path);
        loaded = true;
      } catch (const std::exception& e) {
        std::cerr << "Cache load failed (" << e.what() << "), rebuilding...\n";
      }
    }
    if (!loaded) {
      ds = load_options_csv(input, start, end, every_n, atm_band);
      try {
        if (auto parent = fs::path(cache_path).parent_path(); !parent.empty())
          fs::create_directories(parent);
        save_dataset_cache(ds, cache_path);
      } catch (const std::exception& e) {
        std::cerr << "Cache save failed (" << e.what() << ")\n";
      }
    }
  } else {
    ds = load_options_csv(input, start, end, every_n, atm_band);
  }
  ensure_iv_smile_csv(ds, iv_smile_path, 0.45);

  if (param.long_pos_mil > 0 && !(param.hedge_ratio > 0)) {
    std::cerr << "error: --hedge-ratio must be > 0 when --long-pos-mil > 0\n";
    return 1;
  }
  if (!(param.call_put_ratio_pct > 0)) {
    std::cerr << "error: --call-put-ratio-pct must be > 0\n";
    return 1;
  }

  // Verbose trade log is only needed on the last (or sole) run.
  const bool record = verbose || !verbose_log.empty();
  BacktestResult last;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < bench; ++i) {
    bool rec = record && (i == bench - 1);
    last = w_strategy(ds, param, rec);
  }
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (verbose) {
    const double hr = param.long_pos_mil > 0 ? param.hedge_ratio : 0.0;
    write_trades_csv(verbose_out, last.trades, hr, param.call_put_ratio_pct);
    std::cerr << "Wrote " << last.trades.size() << " trades to " << verbose_out << "\n";
  }
  if (!verbose_log.empty()) {
    VerboseRunInfo run;
    run.argv0 = argv[0];
    run.search = false;
    run.input = input;
    run.yield = yield;
    run.cache = cache_path;
    run.iv_smile = iv_smile_path;
    run.merge_split = merge_split_path;
    run.verbose_log = verbose_log;
    run.start = start;
    run.end = end;
    run.every_n = every_n;
    run.atm_band = atm_band;
    if (write_verbose_html(verbose_log, param.fixed.symbol, param, last, run))
      std::cerr << "Wrote HTML verbose log (" << last.verbose_log.size()
                << " days) to " << verbose_log << "\n";
    else
      std::cerr << "Failed to write HTML verbose log: " << verbose_log << "\n";
  }

  if (param.long_pos_mil > 0) {
    std::cout << "baselineAnnualReturn=" << last.baseline_annual_return
              << " baselineMaxDrawdown=" << last.baseline_max_drawdown
              << " baselineFinalNLV=" << last.baseline_final_nlv
              << " longPosShares=" << last.long_pos_shares
              << " hedgeRatio=" << param.hedge_ratio
              << " unitsUsed=" << last.units_used << "\n";
  }
  std::cout << "annualReturn=" << last.annual_return
            << " maxDrawdown=" << last.max_drawdown
            << " finalNLV=" << last.final_nlv
            << " totalTax=" << last.total_tax
            << " taxPct=" << tax_pct_of_nlv(last.total_tax, last.final_nlv)
            << " numRolls=" << last.num_rolls
            << " elapsed_ms=" << (ms / bench)
            << " (bench=" << bench << ")\n";
  return last.ok ? 0 : 1;
}
