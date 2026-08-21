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
// IRS long-term: holding period strictly more than one year.
inline bool is_long_term_hold(Date open_date, Date close_date) {
  return days_between(open_date, close_date) > 365;
}

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

// Corporate action used to scale today's option and stock commissions back
// in time. `factor` is new-shares / old-shares (2:1 split → 2; reverse 1:10 → 0.1).
struct MergeSplitEvent {
  Date date = 0;
  double factor = 1.0;
};

struct MergeSplitHistory {
  std::vector<MergeSplitEvent> events;  // sorted by date

  // Product of factors for events strictly after `as_of`. After all splits
  // this is 1; before a 2:1 then 7:1 it is 14.
  double remaining_factor(Date as_of) const {
    double p = 1.0;
    for (const auto& e : events) {
      if (e.date > as_of && e.factor > 0.0 && std::isfinite(e.factor)) p *= e.factor;
    }
    return p;
  }

  // Historical commission = today / remaining_factor(as_of).
  double scale_commission(double today_comm, Date as_of) const {
    if (!as_of || events.empty()) return today_comm;
    double f = remaining_factor(as_of);
    if (!(f > 0.0) || !std::isfinite(f)) return today_comm;
    return today_comm / f;
  }
};

struct FixedParam {
  // Options-only starting cash when long_pos_mil == 0. Overlay mode uses
  // long_pos_mil * 1e6 as the sole starting capital (no extra cash book).
  double initial_balance = 1e6;
  // Defaults: CA + federal + NIIT stacked rates (percent via CLI).
  double long_term_tax = 0.371;   // holding > 1 year
  double short_term_tax = 0.541;  // holding <= 1 year
  double stock_comm_per_share = 0.0035;
  double opt_comm_per_contract = 1.7125;
  double stock_slippage = 0;
  double opt_slippage_percent = 0.05;  // already decimal (5% of mid)
  std::string symbol = "SPY";
  MergeSplitHistory merge_split;

  double option_commission(Date as_of) const {
    return merge_split.scale_commission(opt_comm_per_contract, as_of);
  }

  double stock_commission(Date as_of) const {
    return merge_split.scale_commission(stock_comm_per_share, as_of);
  }
};

enum class WingMode { Fixed, Credit, Theta };

struct WParam {
  FixedParam fixed;
  int short_dte = 30;
  int long_dte = 60;
  double short_delta = 0.40;
  double long_delta = 0.10;
  int n_call = 1, n_put = 1;
  // Fixed wing mode: long/short contract ratio (e.g. 1.3 → 13 longs per 10 shorts).
  // Credit/theta: unused as an input (wing size chosen at runtime).
  double m_call = 2, m_put = 2;
  int units = 1;
  // Call contracts as a percent of the put side (100 = equal). After puts are
  // sized, short/long calls = round(put contracts × pct / 100).
  // E.g. 20 with puts −100/+200 → calls −20/+40.
  double call_put_ratio_pct = 100.0;
  WingMode wing_mode = WingMode::Fixed;
  double credit_buffer = 0.02;
  double theta_threshold = 0.0;
  // Credit: max long/short contract ratio (e.g. 4 → up to 4:1). The chosen
  // ratio can be any real value in (1, max_wing_mult] that is a credit after
  // slippage and commission; there is no 2:1 floor.
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
  // Starting capital in $millions. 0 = options-only (uses initial_balance).
  // When > 0: this is the entire book — buy that notional of underlying on
  // day 1. Opening W credit is held as cash (not swept into overlay).
  // NLV/MaxDD include W-strategy + stock. Structure size uses hedge_ratio.
  double long_pos_mil = 0;
  // Net long puts (long − short contracts) as a multiple of long-pos contracts
  // (shares/100). E.g. 1000 shares → 10 contracts; hedge_ratio=1.5 → 15 net
  // long puts. Ignored when long_pos_mil == 0.
  double hedge_ratio = 1.0;
};

// Scale put contract count onto the call side. `call_put_ratio_pct` 20 with
// 100 puts → 20 calls. Round to nearest; floor at 0.
inline int scale_calls_from_puts(int put_contracts, double call_put_ratio_pct) {
  if (put_contracts <= 0 || !(call_put_ratio_pct > 0)) return 0;
  int n = static_cast<int>(std::llround(
      static_cast<double>(put_contracts) * call_put_ratio_pct / 100.0));
  return n < 0 ? 0 : n;
}

struct TradeRecord {
  Date date = 0;
  std::string action;       // "open" | "close" | "buy" | "sell" | "tax"
  int structure_id = 0;
  std::string leg_role;
  char put_call = 'c';      // 'c'/'p' options, 's' stock, 't' tax
  double strike = 0;
  Date exp_date = 0;
  int num_contracts = 0;
  double shares = 0;        // stock: signed shares (+buy / -sell); 0 for options
  double open_price = 0;    // per-share fill incl. commission allocation
  double close_price = 0;   // per-share exit fill (0 on opens/buys)
  double pnl = 0;           // capital gain on close/sell (0 on opens/buys)
  std::string reason;       // open | dte_roll | expire | profit_take | loss_cut | final
                            // | overlay_open | overlay_buy | overlay_sell | tax
  double spot = 0;
  // Book snapshot after this fill (verbose).
  double bh_shares = 0;            // buy-and-hold overlay shares
  double total_tax = 0;            // cumulative tax paid
  double st_loss_carryover = 0;    // unused ST loss carried forward (>0)
  double lt_loss_carryover = 0;
  double st_capital_gain = 0;      // YTD realized ST gain (neg = loss)
  double lt_capital_gain = 0;      // YTD realized LT gain (neg = loss)
  double cash = 0;
  double stock_nlv = 0;            // shares * spot
  double total_nlv = 0;            // cash + stock NLV + option MTM
  // Stock-trade specifics (0 on option/tax rows).
  double cost_basis = 0;           // dollar cost of shares sold
  double trade_st_gain = 0;        // this fill's ST gain (neg = loss)
  double trade_lt_gain = 0;
};

enum class VerboseActionKind { OpenW, CloseW, BuyStock, SellStock, Tax };

struct VerbosePosRow {
  std::string type;  // Call, Put, Stock, Stocks NLV, Cash, Options NLV, NLV
  double contracts = 0;  // option contracts or stock shares
  double strike = 0;
  Date exp_date = 0;
  double price = 0;
  double cost_basis = 0;  // per-share stock lot cost; unused for options
  double total = 0;
  bool has_contracts = false;
  bool has_strike = false;
  bool has_exp = false;
  bool has_price = false;
  bool has_cost_basis = false;
};

struct VerboseActionLeg {
  std::string type;  // Call / Put
  int contracts = 0;
  double strike = 0;
  Date exp_date = 0;
  double cost_basis = 0;
  double price = 0;
  double pnl_per = 0;
  double total = 0;
};

struct VerboseAction {
  VerboseActionKind kind = VerboseActionKind::OpenW;
  std::string reason;
  double pnl = 0;
  double shares = 0;
  double price = 0;
  std::vector<VerboseActionLeg> legs;
};

struct VerboseDay {
  Date date = 0;
  double spot = 0;
  double nlv = 0;
  double commission = 0;  // cumulative commission spent through this day
  double peak = 0;
  double max_dd = 0;
  Date max_dd_date = 0;
  double st_loss_carry = 0;
  double lt_loss_carry = 0;
  double st_gain = 0;
  double lt_gain = 0;
  double bh_nlv = 0;
  double bh_max_dd = 0;
  Date bh_max_dd_date = 0;
  std::vector<VerbosePosRow> positions;
  std::vector<VerboseAction> actions;
};

struct BacktestResult {
  double annual_return = 0;
  double max_drawdown = 0;
  double final_nlv = 0;
  double total_tax = 0;
  int num_rolls = 0;
  int num_gamma_hedges = 0;
  double expected_pnl = 0;
  // Buy-and-hold-only baseline (populated when long_pos_mil > 0).
  double baseline_annual_return = 0;
  double baseline_max_drawdown = 0;
  double baseline_final_nlv = 0;
  double long_pos_shares = 0;  // shares bought for the overlay
  int units_used = 0;          // effective structure multiplier (may be hedge-sized)
  bool ok = true;
  std::string error;
  std::vector<TradeRecord> trades;  // populated when verbose=true
  std::vector<VerboseDay> verbose_log;  // HTML verbose log (when verbose=true)
};

// Cumulative tax paid as a percent of terminal NLV (0 if NLV is not positive).
inline double tax_pct_of_nlv(double total_tax, double final_nlv) {
  if (!(final_nlv > 1e-9)) return 0.0;
  return 100.0 * total_tax / final_nlv;
}

}  // namespace wbt
