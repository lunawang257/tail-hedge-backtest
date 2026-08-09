#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace wbt {

using Date = int;  // YYYYMMDD

inline int ymd(int y, int m, int d) { return y * 10000 + m * 100 + d; }
inline int year_of(Date d) { return d / 10000; }
inline int month_of(Date d) { return (d / 100) % 100; }
inline int day_of(Date d) { return d % 100; }

Date parse_date(const std::string& s);  // YYYY-MM-DD or M/D/YYYY
std::string format_date(Date d);
int days_between(Date a, Date b);  // b - a in calendar days
Date add_days(Date d, int n);
Date third_friday(int year, int month);

constexpr double CONTRACT_SCALE = 100.0;
constexpr double YEAR_FRAC = 365.25;

enum class OptType : char { Call = 'c', Put = 'p' };
inline OptType parse_opt_type(char c) {
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c == 'c') return OptType::Call;
  if (c == 'p') return OptType::Put;
  throw std::runtime_error("bad PutCall");
}
inline char opt_type_char(OptType t) { return static_cast<char>(t); }

struct OptionQuote {
  Date data_date = 0;
  Date exp_date = 0;
  OptType put_call = OptType::Call;
  double strike = 0;
  double bid = 0;
  double ask = 0;
  double last = 0;
  double underlying = 0;
  double mid() const {
    double m = 0.5 * (bid + ask);
    if (m > 0 && std::isfinite(m)) return m;
    if (last > 0 && std::isfinite(last)) return last;
    return 0.0;
  }
};

struct Greeks {
  double value = 0, delta = 0, gamma = 0, theta = 0, vega = 0, rho = 0;
};

struct SyntheticQuote {
  double bid = 0, ask = 0, mid = 0;
  double strike = 0;
  Date exp_date = 0;
  Date data_date = 0;
  double underlying = 0;
  OptType put_call = OptType::Call;
  double implied_vol = 0;
  double delta = 0, gamma = 0, theta = 0;
  bool interpolated = false;
};

struct FixedParam {
  double initial_balance = 1e6;
  double capital_gain_tax = 0.371;
  double stock_comm_per_share = 0.0035;
  double opt_comm_per_contract = 0.065;
  double stock_slippage = 0;
  double opt_slippage_percent = 0.01;  // already decimal
  std::string symbol = "SPY";
};

enum class WingMode { Fixed, Credit, Theta };

struct WParam {
  FixedParam fixed;
  int short_dte = 30;
  int long_dte = 60;
  double short_delta = 0.40;
  double long_delta = 0.10;
  int n_call = 1, n_put = 1;
  int m_call = 2, m_put = 2;
  int units = 1;
  WingMode wing_mode = WingMode::Fixed;
  double credit_buffer = 0.02;
  double theta_threshold = 0.0;
  int max_wing_mult = 5;
  int roll_dte = 7;
  bool roll_on_expiry = true;
  double profit_take_pct = 0.50;
  double loss_cut_pct = 1.00;
  bool enable_profit_take = true;
  bool enable_loss_cut = true;
  bool enable_dte_roll = true;
  bool enable_gamma_scalp = false;
  double gamma_scalp_delta_thresh = 0.15;
  double gamma_scalp_stop_delta = 0.05;
  bool eval_expected_pnl = false;
  std::vector<double> expected_pnl_move_pcts = {
    -0.30, -0.20, -0.15, -0.10, -0.05, 0.0,
    0.05, 0.10, 0.15, 0.20, 0.30
  };
};

struct BacktestResult {
  double annual_return = 0;
  double max_drawdown = 0;
  double final_nlv = 0;
  int num_rolls = 0;
  int num_gamma_hedges = 0;
  double expected_pnl = 0;
  bool ok = true;
  std::string error;
};

}  // namespace wbt
