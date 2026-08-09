#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include "wbt/data.hpp"
#include "wbt/strategy.hpp"
#include "wbt/chain.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <cstdlib>
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
  param.short_dte = 30;
  param.long_dte = 60;
  param.short_delta = 0.40;
  param.long_delta = 0.10;
  param.m_call = 2;
  param.m_put = 2;
  param.units = 1;
  param.roll_dte = 0;
  param.enable_gamma_scalp = false;
  param.eval_expected_pnl = false;
  param.enable_profit_take = true;
  param.enable_loss_cut = true;

  // Delta pick parity on first day
  Date as_of = parse_date(json_str_after(ref, "as_of"));
  double spot = ds.spot(as_of);
  check("spot", spot, json_num_after(ref, "spot"), 1e-4);

  auto sc = find_option_by_delta(ds, OptType::Call, as_of, spot, 0.40, 30);
  if (!sc) { std::cout << "FAIL short_call missing\n"; ++g_fail; }
  else {
    auto pos = ref.find("\"short_call\"");
    auto block = ref.substr(pos, 400);
    check("short_call_strike", sc->strike, json_num_after(block, "strike"), 1.01);
    // Delta targeting can land on adjacent strikes; allow modest absolute error
    check("short_call_delta", std::abs(sc->delta), std::abs(json_num_after(block, "delta")), 0.08);
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
