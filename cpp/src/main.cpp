#include "wbt/data.hpp"
#include "wbt/yield.hpp"
#include "wbt/strategy.hpp"
#include "wbt/gbs.hpp"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <chrono>

using namespace wbt;

static void usage(const char* argv0) {
  std::cerr <<
    "Usage: " << argv0 << " [options]\n"
    "  -i <options.csv>   Options chain CSV\n"
    "  -y <yield.csv>     Yield curve CSV\n"
    "  --short-dte N\n"
    "  --long-dte N\n"
    "  --short-delta X\n"
    "  --long-delta X\n"
    "  --units N\n"
    "  --balance X\n"
    "  --start-date YYYY-MM-DD\n"
    "  --end-date YYYY-MM-DD\n"
    "  --trade-every-n-days N\n"
    "  --no-gamma-scalp\n"
    "  --no-expected-pnl\n"
    "  --bench N           Repeat backtest N times for timing\n";
}

int main(int argc, char** argv) {
  std::string input, yield = "data/yield-curve-rates-1990-2024.csv";
  WParam param;
  param.enable_gamma_scalp = false;
  param.eval_expected_pnl = false;
  param.roll_dte = 0;
  Date start = 0, end = 0;
  int every_n = 1;
  int bench = 1;

  for (int i = 1; i < argc; ++i) {
    auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
      return argv[++i];
    };
    if (eq("-i")) input = need();
    else if (eq("-y")) yield = need();
    else if (eq("--short-dte")) param.short_dte = std::atoi(need());
    else if (eq("--long-dte")) param.long_dte = std::atoi(need());
    else if (eq("--short-delta")) param.short_delta = std::atof(need());
    else if (eq("--long-delta")) param.long_delta = std::atof(need());
    else if (eq("--units")) param.units = std::atoi(need());
    else if (eq("--balance")) param.fixed.initial_balance = std::atof(need());
    else if (eq("--m-call")) param.m_call = std::atoi(need());
    else if (eq("--m-put")) param.m_put = std::atoi(need());
    else if (eq("--roll-dte")) param.roll_dte = std::atoi(need());
    else if (eq("--start-date")) start = parse_date(need());
    else if (eq("--end-date")) end = parse_date(need());
    else if (eq("--trade-every-n-days")) every_n = std::atoi(need());
    else if (eq("--no-gamma-scalp")) param.enable_gamma_scalp = false;
    else if (eq("--gamma-scalp")) param.enable_gamma_scalp = true;
    else if (eq("--no-expected-pnl")) param.eval_expected_pnl = false;
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

  load_yield_data(yield);
  auto ds = load_options_csv(input, start, end, every_n);

  BacktestResult last;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < bench; ++i)
    last = w_strategy(ds, param, false);
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  std::cout << "annualReturn=" << last.annual_return
            << " maxDrawdown=" << last.max_drawdown
            << " finalNLV=" << last.final_nlv
            << " numRolls=" << last.num_rolls
            << " elapsed_ms=" << (ms / bench)
            << " (bench=" << bench << ")\n";
  return last.ok ? 0 : 1;
}
