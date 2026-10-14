#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include "wbt/data.hpp"
#include "wbt/strategy.hpp"
#include "wbt/chain.hpp"
#include "wbt/positions.hpp"
#include "wbt/verbose_log.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <map>

using namespace wbt;

// Minimal JSON value getters (reference file is simple / well-formed)
static std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss; ss << in.rdbuf();
  return ss.str();
}

static double json_num_after(const std::string& s, const std::string& key) {
  auto pos = s.find("\"" + key + "\"");
  if (pos == std::string::npos) throw std::runtime_error("missing key " + key);
  pos = s.find(':', pos);
  return std::stod(s.substr(pos + 1));
}

static std::string json_str_after(const std::string& s, const std::string& key) {
  auto pos = s.find("\"" + key + "\"");
  if (pos == std::string::npos) throw std::runtime_error("missing key " + key);
  pos = s.find(':', pos);
  pos = s.find('"', pos + 1);
  auto end = s.find('"', pos + 1);
  return s.substr(pos + 1, end - pos - 1);
}

static int g_fail = 0;
static void check(const char* name, double got, double exp, double tol) {
  double err = std::abs(got - exp);
  double rel = err / std::max(1.0, std::abs(exp));
  bool ok = err <= tol || rel <= tol;
  std::cout << (ok ? "PASS" : "FAIL") << " " << name
            << " got=" << got << " exp=" << exp
            << " abserr=" << err << "\n";
  if (!ok) ++g_fail;
}

static void check_contains(const char* name, const std::string& text,
                           const std::string& needle) {
  bool ok = text.find(needle) != std::string::npos;
  std::cout << (ok ? "PASS" : "FAIL") << " " << name << "\n";
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "Usage: w_tests <yield.csv> <fixture.csv> <python_reference.json>\n";
    return 2;
  }
  std::string yield_path = argv[1];
  std::string fixture = argv[2];
  std::string ref_path = argv[3];

  load_yield_data(yield_path);
  auto ref = read_file(ref_path);

  // Realized option P/L is classified independently by option type and
  // long/short leg, and all four cumulative values appear in the HTML stats.
  {
    PositionsAndStats stat;
    const Date opened = parse_date("2020-01-02");
    const Date closed = parse_date("2020-01-03");
    stat.add_realized_gain(120, opened, closed, false, false, false);
    stat.add_realized_gain(-30, opened, closed, false, true, false);
    stat.add_realized_gain(45, opened, closed, false, false, true);
    stat.add_realized_gain(-15, opened, closed, false, true, true);
    check("pl_call", stat.total_call_capital_gain, 165, 0);
    check("pl_put", stat.total_put_capital_gain, -45, 0);
    check("pl_short", stat.total_short_option_capital_gain, 90, 0);
    check("pl_long", stat.total_long_option_capital_gain, 30, 0);

    BacktestResult html_result;
    VerboseDay day;
    day.date = closed;
    day.call_pnl = stat.total_call_capital_gain;
    day.put_pnl = stat.total_put_capital_gain;
    day.short_pnl = stat.total_short_option_capital_gain;
    day.long_pnl = stat.total_long_option_capital_gain;
    html_result.verbose_log.push_back(day);
    const char* html_path = "/tmp/wbt-leg-pnl-test.html";
    check("html_written", write_verbose_html(html_path, "TEST", WParam{},
                                              html_result),
          1, 0);
    const auto html = read_file(html_path);
    check_contains("html_leg_pnl_headers", html,
                   "PLcall</th><th class=\"num\">PLput</th>"
                   "<th class=\"num\">PLshort</th>"
                   "<th class=\"num\">PLlong</th>");
    check_contains("html_leg_pnl_values", html,
                   "$165</td><td class=\"num\">-$45</td>"
                   "<td class=\"num\">$90</td>"
                   "<td class=\"num\">$30</td>");
    std::remove(html_path);
  }

  // --- GBS parity ---
  {
    // First case: c,100,100,0.5,0.05,0.02,0.20
    auto g = merton(OptType::Call, 100, 100, 0.5, 0.05, 0.02, 0.20);
    // Parse first gbs block values from JSON loosely by searching
    auto pos = ref.find("\"gbs\"");
    auto block = ref.substr(pos, 800);
    check("gbs_call_value", g.value, json_num_after(block, "value"), 1e-4);
    check("gbs_call_delta", g.delta, json_num_after(block, "delta"), 1e-4);
    double iv = euro_implied_vol(OptType::Call, 100, 100, 0.5, 0.05, 0.02, g.value);
    check("gbs_call_iv_rt", iv, json_num_after(block, "iv_roundtrip"), 1e-3);
  }
  {
    auto g = merton(OptType::Put, 100, 100, 0.5, 0.05, 0.02, 0.20);
    // second object in gbs array
    auto pos = ref.find("\"type\": \"p\"");
    auto block = ref.substr(pos, 600);
    check("gbs_put_value", g.value, json_num_after(block, "value"), 1e-4);
    check("gbs_put_delta", g.delta, json_num_after(block, "delta"), 1e-4);
  }

  // --- Backtest parity ---
  auto ds = load_options_csv(fixture);
  WParam param;
  param.fixed.initial_balance = json_num_after(ref, "balance");
  // Pin legacy costs so parity stays aligned with python_reference.json.
  param.fixed.opt_comm_per_contract = 0.065;
  param.fixed.opt_slippage_percent = 0.01;
  param.short_dte = 30;
  param.long_dte = 60;
  param.short_delta = 0.40;
  param.long_delta = 0.10;
  param.m_call = 2;
  param.m_put = 2;
  param.units = 1;
  param.call_put_ratio_pct = 100;
  param.roll_dte = 0;
  param.enable_gamma_scalp = false;
  param.eval_expected_pnl = false;
  param.enable_profit_take = true;
  param.enable_loss_cut = true;

  // Delta pick parity on first day
  Date as_of = parse_date(json_str_after(ref, "as_of"));
  double spot = ds.spot(as_of);
  check("spot", spot, json_num_after(ref, "spot"), 1e-4);

  check("cp_ratio_20_short", scale_calls_from_puts(100, 20), 20, 0);
  check("cp_ratio_20_long", scale_calls_from_puts(200, 20), 40, 0);
  check("cp_ratio_100", scale_calls_from_puts(100, 100), 100, 0);
  check("cp_ratio_50", scale_calls_from_puts(100, 50), 50, 0);
  check("cp_ratio_25", scale_calls_from_puts(100, 25), 25, 0);

  {
    MergeSplitHistory h;
    h.events.push_back({parse_date("2010-01-01"), 2.0});
    h.events.push_back({parse_date("2014-06-09"), 7.0});
    check("ms_before_both", h.scale_commission(1.0, parse_date("2009-12-31")),
          1.0 / 14.0, 1e-12);
    check("ms_between", h.scale_commission(1.0, parse_date("2012-06-01")),
          1.0 / 7.0, 1e-12);
    check("ms_on_second_split", h.scale_commission(1.0, parse_date("2014-06-09")),
          1.0, 1e-12);
    check("ms_after", h.scale_commission(1.0, parse_date("2015-01-01")), 1.0,
          1e-12);
    check("ms_remaining_before", h.remaining_factor(parse_date("2009-12-31")),
          14.0, 1e-12);

    FixedParam fp;
    fp.merge_split = h;
    fp.stock_comm_per_share = 0.0035;
    fp.opt_comm_per_contract = 1.7125;
    check("stock_comm_before", fp.stock_commission(parse_date("2009-12-31")),
          0.0035 / 14.0, 1e-12);
    check("stock_comm_between", fp.stock_commission(parse_date("2012-06-01")),
          0.0035 / 7.0, 1e-12);
    check("stock_comm_after", fp.stock_commission(parse_date("2015-01-01")),
          0.0035, 1e-12);
    check("opt_comm_before", fp.option_commission(parse_date("2009-12-31")),
          1.7125 / 14.0, 1e-12);

    const char* tmp = "/tmp/wbt-merge-split-test.csv";
    {
      std::ofstream out(tmp);
      out << "date,symbol,event_type,ratio,numerator,denominator,factor,notes,source\n"
          << "2010-01-01,TEST,split,2:1,2,1,2,n,s\n"
          << "2014-06-09,TEST,split,7:1,7,1,7,n,s\n";
    }
    auto loaded = load_merge_split_csv(tmp);
    check("ms_csv_n", static_cast<double>(loaded.events.size()), 2.0, 0);
    check("ms_csv_before", loaded.scale_commission(14.0, parse_date("2009-01-01")),
          1.0, 1e-12);
    check("ms_csv_between", loaded.scale_commission(14.0, parse_date("2012-01-01")),
          2.0, 1e-12);
    std::remove(tmp);
  }

  auto sc = find_option_by_delta(ds, OptType::Call, as_of, spot, 0.40, 30,
                                 true, 0.01);
  if (!sc) { std::cout << "FAIL short_call missing\n"; ++g_fail; }
  else {
    auto pos = ref.find("\"short_call\"");
    auto block = ref.substr(pos, 400);
    check("short_call_strike", sc->strike, json_num_after(block, "strike"), 1.01);
    // Delta targeting can land on adjacent strikes; allow modest absolute error
    check("short_call_delta", std::abs(sc->delta), std::abs(json_num_after(block, "delta")), 0.08);
  }

  // Theta must be feasible on EACH side after hedge sizing and rounding.
  // Exercise both overlay and explicit units, including a disabled call side.
  for (double overlay : {0.0, 1.0}) {
    for (double pct : {0.0, 25.0, 100.0}) {
      WParam theta = param;
      theta.wing_mode = WingMode::Theta;
      theta.long_pos_mil = overlay;
      theta.units = 9;
      theta.call_put_ratio_pct = pct;
      theta.long_delta = 0.08;
      auto result = w_strategy(ds, theta, true);
      int opens = 0;
      for (const auto& day : result.verbose_log) {
        bool opened = false;
        for (const auto& action : day.actions)
          opened |= action.kind == VerboseActionKind::OpenW;
        if (!opened) continue;
        ++opens;
        double ct = 0, pt = 0;
        int cs = 0, cl = 0, ps = 0, pl = 0;
        for (const auto& row : day.positions) {
          if (row.type == "Call") {
            ct += row.theta;
            if (row.contracts < 0) cs -= row.contracts;
            else cl += row.contracts;
          } else if (row.type == "Put") {
            pt += row.theta;
            if (row.contracts < 0) ps -= row.contracts;
            else pl += row.contracts;
          }
        }
        check("theta_entry_call_nonnegative", ct >= -1e-7, 1, 0);
        check("theta_entry_put_nonnegative", pt >= -1e-7, 1, 0);
        check("theta_put_longs_exceed_shorts", pl > ps && ps > 0, 1, 0);
        check("theta_call_short_exposure", cs, scale_calls_from_puts(ps, pct), 0);
        check("theta_call_longs_exceed_shorts", cs ? cl > cs : cl == 0, 1, 0);
      }
      check("theta_fixture_opens", opens > 0, 1, 0);
      // No feasible minimum must skip entry rather than force six longs.
      theta.theta_threshold = 1e12;
      auto rejected = w_strategy(ds, theta, true);
      int rejected_opens = 0;
      for (const auto& trade : rejected.trades)
        rejected_opens += trade.action == "open";
      check("theta_infeasible_skips", rejected_opens, 0, 0);
      theta.theta_threshold = 0;
      theta.max_wing_mult = 1;
      rejected = w_strategy(ds, theta, true);
      rejected_opens = 0;
      for (const auto& trade : rejected.trades)
        rejected_opens += trade.action == "open";
      check("theta_respects_wing_cap", rejected_opens, 0, 0);
    }
  }

  auto t0 = std::chrono::steady_clock::now();
  constexpr int N = 20;
  BacktestResult last;
  for (int i = 0; i < N; ++i) last = w_strategy(ds, param, false);
  auto t1 = std::chrono::steady_clock::now();
  double cpp_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / N;

  auto bt_pos = ref.find("\"backtest\"");
  auto bt = ref.substr(bt_pos, 500);
  check("annualReturn", last.annual_return, json_num_after(bt, "annualReturn"), 0.25);  // within 0.25 pp
  check("maxDrawdown", last.max_drawdown, json_num_after(bt, "maxDrawdown"), 0.5);
  check("finalNLV", last.final_nlv, json_num_after(bt, "finalNLV"),
        std::max(50.0, std::abs(json_num_after(bt, "finalNLV")) * 0.01));  // 1% or $50
  check("numRolls", last.num_rolls, json_num_after(bt, "numRolls"), 0.0);

  double py_ms = json_num_after(bt, "elapsed_ms_python");
  std::cout << "PERF python_ms=" << py_ms << " cpp_ms=" << cpp_ms
            << " speedup=" << (py_ms / std::max(cpp_ms, 1e-9)) << "x\n";

  if (g_fail) {
    std::cout << g_fail << " checks failed\n";
    return 1;
  }
  std::cout << "All parity checks passed.\n";
  return 0;
}
