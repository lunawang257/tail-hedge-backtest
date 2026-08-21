#include "wbt/strategy.hpp"
#include "wbt/chain.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include "wbt/expected_pnl.hpp"
#include <cmath>
#include <climits>
#include <iostream>
#include <algorithm>
#include <vector>
#include <tuple>

namespace wbt {
namespace {

using TradeLog = std::vector<TradeRecord>;

double quote_mid(const SyntheticQuote& q) {
  return q.mid > 0 ? q.mid : 0.5 * (q.bid + q.ask);
}

// Signed cash to open `num_contracts` (+ long / − short). Matches `open_leg`:
// positive = cash paid (debit), negative = cash received (credit). Includes
// slippage and per-contract commission (split-adjusted to the quote date).
double preview_leg_cash(const WParam& param, const SyntheticQuote& quote,
                        int num_contracts) {
  if (num_contracts == 0) return 0;
  bool is_buy = num_contracts > 0;
  double slip = param.fixed.opt_slippage_percent;
  double trade_px = is_buy ? quote_mid(quote) * (1.0 + slip)
                           : quote_mid(quote) * (1.0 - slip);
  double opt_comm = param.fixed.option_commission(quote.data_date);
  int n = std::abs(num_contracts);
  int signed_dir = is_buy ? 1 : -1;
  double cash_per = trade_px * CONTRACT_SCALE + (is_buy ? opt_comm : -opt_comm);
  return signed_dir * cash_per * n;
}

void call_counts_from_puts(int short_p, int long_p, double call_put_ratio_pct,
                           int& short_c, int& long_c) {
  short_c = scale_calls_from_puts(short_p, call_put_ratio_pct);
  long_c = scale_calls_from_puts(long_p, call_put_ratio_pct);
  if (short_c > 0) long_c = std::max(long_c, short_c + 1);
}

// True iff the structure is a net credit after slippage and commission, with
// long debit ≤ short credit × (1 − credit_buffer). Longs must outnumber shorts.
bool is_credit_after_costs(const WParam& param,
                           const SyntheticQuote& qsc, const SyntheticQuote& qsp,
                           const SyntheticQuote& qlc, const SyntheticQuote& qlp,
                           int short_c, int short_p, int long_c, int long_p) {
  if (short_p <= 0 || long_p <= short_p) return false;
  if (short_c > 0 && long_c <= short_c) return false;
  double short_credit =
      -(preview_leg_cash(param, qsc, -short_c) +
        preview_leg_cash(param, qsp, -short_p));
  double long_debit =
      preview_leg_cash(param, qlc, long_c) + preview_leg_cash(param, qlp, long_p);
  if (!(short_credit > 0)) return false;
  double haircut = std::max(0.0, param.credit_buffer);
  double cap = short_credit * (1.0 - haircut);
  return long_debit + 1e-9 < cap;
}

// Largest long/short put ratio in (1, max_wing_mult] that is still a credit
// after slip+commission. Ratio can be 1.1, 2.25, etc. (integer contract counts).
bool choose_credit_wings_for_shorts(
    const WParam& param, const SyntheticQuote& qsc, const SyntheticQuote& qsp,
    const SyntheticQuote& qlc, const SyntheticQuote& qlp, int short_p,
    int& long_p, int& long_c) {
  int max_mult = std::max(1, param.max_wing_mult);
  int min_lp = short_p + 1;
  int max_lp = short_p * max_mult;
  if (max_lp < min_lp) return false;
  auto ok = [&](int n_lp) {
    int n_sc = 0, n_lc = 0;
    call_counts_from_puts(short_p, n_lp, param.call_put_ratio_pct, n_sc, n_lc);
    return is_credit_after_costs(param, qsc, qsp, qlc, qlp, n_sc, short_p, n_lc,
                                 n_lp);
  };
  if (!ok(min_lp)) return false;
  int lo = min_lp, hi = max_lp;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (ok(mid)) lo = mid;
    else hi = mid - 1;
  }
  long_p = lo;
  int n_sc = 0;
  call_counts_from_puts(short_p, long_p, param.call_put_ratio_pct, n_sc, long_c);
  return true;
}

bool size_credit_structure(const WParam& param,
                           const SyntheticQuote& qsc, const SyntheticQuote& qsp,
                           const SyntheticQuote& qlc, const SyntheticQuote& qlp,
                           bool hedge_sized, double target_net_puts,
                           int& short_c, int& short_p, int& long_c, int& long_p,
                           int& units_used) {
  const double pct = param.call_put_ratio_pct;
  int max_mult = std::max(1, param.max_wing_mult);
  if (max_mult <= 1) return false;

  if (!hedge_sized) {
    int u = std::max(1, param.units);
    short_p = param.n_put * u;
    if (!choose_credit_wings_for_shorts(param, qsc, qsp, qlc, qlp, short_p,
                                        long_p, long_c))
      return false;
    call_counts_from_puts(short_p, long_p, pct, short_c, long_c);
    units_used = u;
    return true;
  }

  // Overlay: keep net long puts ≈ target, pick the largest ratio (fewest
  // shorts) that is still a credit after slip+commission.
  int net = std::max(1, static_cast<int>(std::llround(target_net_puts)));
  int min_sp = static_cast<int>(
      std::ceil(static_cast<double>(net) / static_cast<double>(max_mult - 1)));
  min_sp = std::max(1, min_sp);

  auto ok_sp = [&](int n_sp) {
    if (n_sp <= 0 || net > INT_MAX - n_sp) return false;
    int n_lp = n_sp + net;
    if (n_lp > n_sp * max_mult || n_lp <= n_sp) return false;
    int n_sc = 0, n_lc = 0;
    call_counts_from_puts(n_sp, n_lp, pct, n_sc, n_lc);
    return is_credit_after_costs(param, qsc, qsp, qlc, qlp, n_sc, n_sp, n_lc,
                                 n_lp);
  };

  // Bound the search: if even 1:1 all-in is a debit, no finite size works.
  int probe = std::max(net, 1000);
  int psc = 0, plc = 0;
  call_counts_from_puts(probe, probe, pct, psc, plc);
  double sproc = -(preview_leg_cash(param, qsc, -psc) +
                   preview_leg_cash(param, qsp, -probe));
  double lcost =
      preview_leg_cash(param, qlc, plc) + preview_leg_cash(param, qlp, probe);
  double haircut = std::max(0.0, param.credit_buffer);
  double denom = sproc * (1.0 - haircut) - lcost;
  if (!(denom > 1e-9)) return false;
  double need = static_cast<double>(net) * (lcost / probe) / (denom / probe);
  if (!(need > 0) || need > static_cast<double>(INT_MAX / 4)) return false;
  int max_sp = std::max(min_sp, static_cast<int>(std::ceil(need)) + 16);
  if (max_sp > INT_MAX - net) max_sp = INT_MAX - net;
  if (!ok_sp(max_sp)) {
    bool found = false;
    int cap = (INT_MAX - net - max_sp > 64) ? max_sp + 64 : INT_MAX - net;
    for (int sp = max_sp + 1; sp <= cap; ++sp) {
      if (ok_sp(sp)) {
        max_sp = sp;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }

  int lo = min_sp, hi = max_sp;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (ok_sp(mid)) hi = mid;
    else lo = mid + 1;
  }
  if (!ok_sp(lo)) return false;
  short_p = lo;
  long_p = short_p + net;
  call_counts_from_puts(short_p, long_p, pct, short_c, long_c);
  units_used = short_p;
  return true;
}

std::pair<int, int> choose_wing_counts(const WParam& param,
                                       const SyntheticQuote& sc, const SyntheticQuote& sp,
                                       const SyntheticQuote& lc, const SyntheticQuote& lp) {
  int n_c = param.n_call, n_p = param.n_put;
  // Theta: integer wing multiple from n+1 up to max_wing_mult.
  // Credit sizing is in size_credit_structure (fractional ratio, all-in credit).
  int min_m = std::max(n_c, n_p) + 1;
  int max_m = std::max(min_m, std::max(n_c, n_p) * std::max(1, param.max_wing_mult));
  int best_c = std::max(n_c, min_m), best_p = std::max(n_p, min_m);

  auto net_theta = [&](int mc, int mp) {
    return ((-n_c) * sc.theta + (-n_p) * sp.theta + mc * lc.theta + mp * lp.theta) * CONTRACT_SCALE;
  };

  for (int m = min_m; m <= max_m; ++m) {
    int mc = std::max(n_c, m), mp = std::max(n_p, m);
    if (net_theta(mc, mp) >= param.theta_threshold - 1e-9) { best_c = mc; best_p = mp; }
    else break;
  }
  return {best_c, best_p};
}

double open_leg(const WParam& param, Date date, double spot, PositionsAndStats& stat,
                const SyntheticQuote& quote, int num_contracts, const char* role,
                TradeLog* log) {
  if (num_contracts == 0) return 0;
  bool is_buy = num_contracts > 0;
  double mid = quote_mid(quote);
  double slip = param.fixed.opt_slippage_percent;
  double trade_px = is_buy ? mid * (1.0 + slip) : mid * (1.0 - slip);
  double opt_comm = param.fixed.option_commission(date);
  int n = std::abs(num_contracts);
  double cash_flow = preview_leg_cash(param, quote, num_contracts);

  OptPosition pos;
  pos.opt_type = quote.put_call;
  pos.strike = quote.strike;
  pos.exp_date = quote.exp_date;
  pos.num_contracts = num_contracts;
  pos.open_date = date;
  pos.open_price = trade_px + (opt_comm / CONTRACT_SCALE) * (is_buy ? 1 : -1);
  pos.org_price = mid;
  pos.per_contract_delta = quote.delta;
  pos.per_contract_gamma = quote.gamma;
  pos.per_contract_theta = quote.theta;
  pos.implied_vol = quote.implied_vol;
  pos.leg_role = role;
  pos.open_stock_price = spot;
  pos.structure_id = stat.structure_id;
  stat.opt_list.push_back(pos);
  stat.cash -= cash_flow;
  stat.total_commission += opt_comm * n;

  if (log) {
    TradeRecord tr;
    tr.date = date;
    tr.action = "open";
    tr.structure_id = pos.structure_id;
    tr.leg_role = role;
    tr.put_call = opt_type_char(pos.opt_type);
    tr.strike = pos.strike;
    tr.exp_date = pos.exp_date;
    tr.num_contracts = pos.num_contracts;
    tr.open_price = pos.open_price;
    tr.close_price = 0;
    tr.pnl = 0;
    tr.reason = "open";
    tr.spot = spot;
    stat.fill_trade_snapshot(tr, date, spot);
    log->push_back(std::move(tr));
  }
  return cash_flow;
}

bool open_w_structure(const WParam& param, Date date, const OptionsDataset& ds,
                      PositionsAndStats& stat, TradeLog* log) {
  double spot = ds.spot(date);
  double slip = param.fixed.opt_slippage_percent;
  IvCache iv_cache;

  auto sc = find_option_by_delta(ds, OptType::Call, date, spot, param.short_delta,
                                 param.short_dte, true, slip, true, &iv_cache);
  auto sp = find_option_by_delta(ds, OptType::Put, date, spot, param.short_delta,
                                 param.short_dte, true, slip, true, &iv_cache);
  auto lc = find_option_by_delta(ds, OptType::Call, date, spot, param.long_delta,
                                 param.long_dte, true, slip, true, &iv_cache);
  auto lp = find_option_by_delta(ds, OptType::Put, date, spot, param.long_delta,
                                 param.long_dte, true, slip, true, &iv_cache);
  if (!sc || !sp || !lc || !lp) return false;

  // Overlay mode sizes the W off remaining long stock. If the book has been
  // fully sold down, do not fall back to param.units (that would re-lever).
  const bool overlay_mode =
      param.long_pos_mil > 0 && param.hedge_ratio > 0;
  if (overlay_mode && stat.buy_and_hold_shares() <= 0) return false;

  // When a long buy-and-hold overlay is active, size the W so net long puts
  // (long − short put contracts) ≈ hedge_ratio × (bh_shares / 100).
  // Otherwise use param.units as before.
  const bool hedge_sized = overlay_mode;
  const double target_net_puts =
      hedge_sized
          ? param.hedge_ratio * stat.buy_and_hold_shares() / CONTRACT_SCALE
          : 0.0;

  int short_c = 0, short_p = 0, long_c = 0, long_p = 0;
  if (param.wing_mode == WingMode::Fixed) {
    auto size_fixed = [&](int u) {
      int scn = param.n_call * u;
      int spn = param.n_put * u;
      int lcn = static_cast<int>(
          std::llround(static_cast<double>(scn) * param.m_call));
      int lpn = static_cast<int>(
          std::llround(static_cast<double>(spn) * param.m_put));
      lcn = std::max(lcn, scn + 1);
      lpn = std::max(lpn, spn + 1);
      return std::tuple<int, int, int, int>{scn, spn, lcn, lpn};
    };
    int u = std::max(1, param.units);
    if (hedge_sized) {
      // net ≈ short_p * (m_put - 1) → u ≈ target / (n_put * (m_put - 1))
      double denom = static_cast<double>(param.n_put) *
                     std::max(param.m_put - 1.0, 1e-9);
      u = std::max(1, static_cast<int>(std::llround(target_net_puts / denom)));
      // Nudge so realized net put contracts are as close as possible to target.
      auto net_of = [&](int uu) {
        auto [scn, spn, lcn, lpn] = size_fixed(uu);
        (void)scn; (void)lcn;
        return lpn - spn;
      };
      int best_u = u;
      double best_err = std::abs(net_of(u) - target_net_puts);
      for (int cand : {u - 1, u, u + 1, u + 2}) {
        if (cand < 1) continue;
        double err = std::abs(net_of(cand) - target_net_puts);
        if (err < best_err) {
          best_err = err;
          best_u = cand;
        }
      }
      u = best_u;
    }
    std::tie(short_c, short_p, long_c, long_p) = size_fixed(u);
    stat.last_units_used = u;
    // Call side is a percent of the (already sized) put side.
    short_c = scale_calls_from_puts(short_p, param.call_put_ratio_pct);
    long_c = scale_calls_from_puts(long_p, param.call_put_ratio_pct);
    if (short_c > 0) long_c = std::max(long_c, short_c + 1);
  } else if (param.wing_mode == WingMode::Credit) {
    int u = 0;
    if (!size_credit_structure(param, *sc, *sp, *lc, *lp, hedge_sized,
                               target_net_puts, short_c, short_p, long_c,
                               long_p, u))
      return false;
    stat.last_units_used = u;
  } else {
    auto [mc, mp] = choose_wing_counts(param, *sc, *sp, *lc, *lp);
    int net_per_unit = mp - param.n_put;
    if (net_per_unit < 1) net_per_unit = 1;
    int u = std::max(1, param.units);
    if (hedge_sized) {
      u = std::max(
          1, static_cast<int>(std::llround(target_net_puts /
                                           static_cast<double>(net_per_unit))));
    }
    short_c = param.n_call * u;
    short_p = param.n_put * u;
    long_c = mc * u;
    long_p = mp * u;
    stat.last_units_used = u;
    short_c = scale_calls_from_puts(short_p, param.call_put_ratio_pct);
    long_c = scale_calls_from_puts(long_p, param.call_put_ratio_pct);
    if (short_c > 0) long_c = std::max(long_c, short_c + 1);
  }

  ++stat.structure_id;
  double entry = 0;
  entry += open_leg(param, date, spot, stat, *sc, -short_c, LEG_SHORT_CALL, log);
  entry += open_leg(param, date, spot, stat, *sp, -short_p, LEG_SHORT_PUT, log);
  entry += open_leg(param, date, spot, stat, *lc, long_c, LEG_LONG_CALL, log);
  entry += open_leg(param, date, spot, stat, *lp, long_p, LEG_LONG_PUT, log);
  stat.structure_entry_cash = entry;
  stat.structure_entry_value = stat.option_nlv(date, spot);
  stat.structure_open_date = date;
  stat.structure_open_spot = spot;
  // Store opened wing contract counts for diagnostics.
  stat.last_wing_m_call = long_c;
  stat.last_wing_m_put = long_p;
  stat.gamma_scalping_active = false;
  return true;
}

double close_one_option(const WParam& param, Date date, double spot, PositionsAndStats& stat,
                        OptPosition& opt, const char* reason, TradeLog* log) {
  double mid = stat.mark_option(opt, date, spot);
  bool is_long = opt.num_contracts > 0;
  double slip = param.fixed.opt_slippage_percent;
  double trade_px = is_long ? std::max(mid * (1 - slip), 0.0) : std::max(mid * (1 + slip), 0.0);
  double opt_comm = param.fixed.option_commission(date);
  double cash_change = trade_px * opt.num_contracts * CONTRACT_SCALE
                       - opt_comm * std::abs(opt.num_contracts);
  // Close fill incl. commission, signed like open_price for P/L consistency.
  double close_price = trade_px - (opt_comm / CONTRACT_SCALE) *
                       (is_long ? 1.0 : -1.0);
  double capital_gain = cash_change - opt.get_cost_basis();
  stat.cash += cash_change;
  stat.total_commission += opt_comm * std::abs(opt.num_contracts);
  stat.add_realized_gain(capital_gain, opt.open_date, date, /*is_stock=*/false,
                         opt.opt_type == OptType::Put);
  if (log) {
    TradeRecord tr;
    tr.date = date;
    tr.action = "close";
    tr.structure_id = opt.structure_id;
    tr.leg_role = opt.leg_role;
    tr.put_call = opt_type_char(opt.opt_type);
    tr.strike = opt.strike;
    tr.exp_date = opt.exp_date;
    tr.num_contracts = opt.num_contracts;
    tr.open_price = opt.open_price;
    tr.close_price = close_price;
    tr.pnl = capital_gain;
    tr.reason = reason ? reason : "";
    tr.spot = spot;
    if (is_long_term_hold(opt.open_date, date)) tr.trade_lt_gain = capital_gain;
    else tr.trade_st_gain = capital_gain;
    stat.fill_trade_snapshot(tr, date, spot);
    log->push_back(std::move(tr));
  }
  return capital_gain;
}

void close_all_options(const WParam& param, Date date, double spot, PositionsAndStats& stat,
                       const char* reason, TradeLog* log) {
  for (auto& opt : stat.opt_list) close_one_option(param, date, spot, stat, opt, reason, log);
  stat.opt_list.clear();
}

void close_all_stock(const WParam& param, Date date, double spot,
                     PositionsAndStats& stat, bool include_buy_and_hold,
                     const char* reason, TradeLog* log) {
  double shares = 0;
  for (const auto& p : stat.stocks) {
    if (p.buy_and_hold && !include_buy_and_hold) continue;
    shares += p.num_shares;
  }
  if (std::abs(shares) < 1e-9) return;
  double stock_comm = param.fixed.stock_comm_per_share;
  if (shares > 0) {
    double close_px = spot - stock_comm;
    auto [gain, avg] = stat.close_stock_positions(
        shares, close_px, date, include_buy_and_hold, stock_comm, log, reason,
        spot);
    (void)avg;
    (void)gain;
  } else {
    double close_px = spot + stock_comm;
    double remaining = -shares;
    for (auto it = stat.stocks.begin(); it != stat.stocks.end() && remaining > 0;) {
      if (it->num_shares >= 0) { ++it; continue; }
      if (it->buy_and_hold && !include_buy_and_hold) { ++it; continue; }
      double take = std::min(-it->num_shares, remaining);
      double lot_cost = it->open_price * take;
      double lot_gain = (it->open_price - close_px) * take;
      Date lot_open = it->open_date;
      double lot_px = it->open_price;
      stat.add_realized_gain(lot_gain, lot_open, date, /*is_stock=*/true);
      it->num_shares += take;
      remaining -= take;
      stat.cash -= close_px * take;
      stat.total_commission += stock_comm * take;
      double st_g = 0, lt_g = 0;
      if (is_long_term_hold(lot_open, date)) lt_g = lot_gain;
      else st_g = lot_gain;
      if (std::abs(it->num_shares) < 1e-9) it = stat.stocks.erase(it);
      else ++it;
      stat.log_stock_trade(log, date, spot, "buy", reason, take, lot_px,
                           close_px, lot_cost, st_g, lt_g);
    }
  }
}

void open_buy_and_hold(const WParam& param, Date date, double spot,
                       PositionsAndStats& stat, TradeLog* log) {
  double notional = param.long_pos_mil * 1e6;
  if (notional <= 0 || spot <= 0) return;
  double stock_comm = param.fixed.stock_comm_per_share;
  // Spend ~notional on shares (commission included in cash outlay).
  double shares = std::floor(notional / (spot + stock_comm));
  if (shares < 1.0) return;
  double px = spot + stock_comm;
  stat.open_stock(shares, px, date, /*buy_and_hold=*/true);
  stat.cash -= px * shares;
  stat.total_commission += stock_comm * shares;
  stat.log_stock_trade(log, date, spot, "buy", "overlay_open", shares, px, 0, 0,
                       0, 0);
}

// Pure buy-and-hold of the underlying (no W-strategy) for baseline metrics.
void buy_and_hold_baseline_impl(const OptionsDataset& ds, double long_pos_mil,
                                double stock_comm_per_share,
                                BacktestResult& result) {
  double notional = long_pos_mil * 1e6;
  if (notional <= 0 || ds.dates.empty()) return;
  Date start = ds.dates.front();
  Date end = ds.dates.back();
  double spot0 = ds.spot(start);
  double shares = std::floor(notional / (spot0 + stock_comm_per_share));
  if (shares < 1.0) return;
  double cash = notional - (spot0 + stock_comm_per_share) * shares;
  double peak = notional;
  double max_dd = 0;
  for (Date date : ds.dates) {
    double spot = ds.spot(date);
    double equity = cash + shares * spot;
    if (equity > peak) peak = equity;
    if (peak > 0) {
      double dd = (peak - equity) / peak * 100.0;
      if (dd > max_dd) max_dd = dd;
    }
  }
  double last_spot = ds.spot(end);
  // Mark-to-market final, net of exit commission.
  double final_nlv = cash + shares * last_spot - stock_comm_per_share * shares;
  double years = std::max(days_between(start, end) / YEAR_FRAC, 1e-6);
  double total_return = final_nlv / notional;
  double annual = (total_return > 0)
                      ? (std::pow(total_return, 1.0 / years) - 1.0) * 100.0
                      : -100.0;
  result.baseline_annual_return = annual;
  result.baseline_max_drawdown = max_dd;
  result.baseline_final_nlv = final_nlv;
  result.long_pos_shares = shares;
}

std::pair<bool, const char*> should_roll(const WParam& param, Date date,
                                         PositionsAndStats& stat, double pnl_pct) {
  if (stat.opt_list.empty()) return {false, ""};
  auto short_dte = stat.short_min_dte(date);
  if (param.enable_dte_roll && short_dte) {
    if (*short_dte <= param.roll_dte) return {true, "dte_roll"};
    if (param.roll_on_expiry && *short_dte <= 0) return {true, "expire"};
  }
  if (param.enable_profit_take && pnl_pct >= param.profit_take_pct) return {true, "profit_take"};
  if (param.enable_loss_cut && pnl_pct <= -param.loss_cut_pct) return {true, "loss_cut"};
  return {false, ""};
}

struct NetGains {
  double st = 0;
  double lt = 0;
};

NetGains net_with_carry(double st, double lt, double st_carry, double lt_carry) {
  st -= st_carry;
  lt -= lt_carry;
  if (st > 0 && lt < 0) {
    double n = std::min(st, -lt);
    st -= n;
    lt += n;
  } else if (st < 0 && lt > 0) {
    double n = std::min(-st, lt);
    st += n;
    lt -= n;
  }
  return {st, lt};
}

double tax_on_net(const NetGains& n, const FixedParam& fp) {
  return std::max(0.0, n.st) * fp.short_term_tax +
         std::max(0.0, n.lt) * fp.long_term_tax;
}

// After each W open/close, option cash has already been applied (debit
// deducted, credit added). Convert leftover cash into overlay shares, or sell
// highest-cost lots if cash is short. Avoids a silent margin loan (which is
// what made MaxDrawdown exceed 100%).
void rebalance_overlay_cash(const WParam& param, Date date, double spot,
                            PositionsAndStats& stat, TradeLog* log) {
  if (param.long_pos_mil <= 0) return;
  double comm = param.fixed.stock_comm_per_share;
  if (stat.cash < -1e-9)
    stat.sell_overlay_hifo(-stat.cash, spot, date, comm, log, "overlay_sell");
  else if (stat.cash > 0)
    stat.buy_overlay(stat.cash, spot, date, comm, log, "overlay_buy");
}

void year_end_tax(const WParam& param, Date date, double spot,
                  PositionsAndStats& stat, bool invest_leftover, TradeLog* log) {
  const double comm = param.fixed.stock_comm_per_share;
  const bool overlay = param.long_pos_mil > 0;

  // Selling stock to fund tax realizes more gain/loss and can change the bill.
  for (int i = 0; i < 64; ++i) {
    auto net = net_with_carry(stat.annual_st_capital_gain,
                              stat.annual_lt_capital_gain,
                              stat.st_loss_carryover, stat.lt_loss_carryover);
    double tax = tax_on_net(net, param.fixed);
    if (tax <= stat.cash + 1e-6) break;
    if (!overlay) break;
    double need = tax - stat.cash;
    double raised = stat.sell_overlay_hifo(need, spot, date, comm, log, "tax");
    if (raised < 1e-6) break;
  }

  auto net = net_with_carry(stat.annual_st_capital_gain,
                            stat.annual_lt_capital_gain,
                            stat.st_loss_carryover, stat.lt_loss_carryover);
  double tax = tax_on_net(net, param.fixed);
  if (tax > 0) {
    stat.cash -= tax;
    stat.total_tax += tax;
  }
  stat.st_loss_carryover = net.st < 0 ? -net.st : 0;
  stat.lt_loss_carryover = net.lt < 0 ? -net.lt : 0;

  if (log && tax > 0) {
    TradeRecord tr;
    tr.date = date;
    tr.action = "tax";
    tr.structure_id = stat.structure_id;
    tr.leg_role = "tax";
    tr.put_call = 't';
    tr.reason = "tax";
    tr.spot = spot;
    tr.pnl = -tax;
    // Snapshot before zeroing YTD gains so the row shows the year just taxed.
    stat.fill_trade_snapshot(tr, date, spot);
    log->push_back(std::move(tr));
  }

  stat.annual_st_capital_gain = 0;
  stat.annual_lt_capital_gain = 0;

  if (invest_leftover && overlay && stat.cash > 0)
    stat.buy_overlay(stat.cash, spot, date, comm, log, "overlay_buy");
}

void calc_drawdown(PositionsAndStats& stat, Date date, double equity) {
  // 100% drawdown = account wiped out; never report more than that.
  if (equity < 0) equity = 0;
  if (equity > stat.cur_peak) {
    stat.cur_peak = equity;
    stat.cur_peak_date = format_date(date);
    stat.peak_date = date;
  }
  if (stat.cur_peak > 0) {
    double dd = (stat.cur_peak - equity) / stat.cur_peak * 100.0;
    if (dd > 100.0) dd = 100.0;
    if (dd > stat.max_drawdown) {
      stat.max_drawdown = dd;
      stat.max_drawdown_peak_date = format_date(date);
      stat.max_dd_date = date;
    }
  }
}

void update_greeks(Date date, double spot, PositionsAndStats& stat) {
  for (auto& opt : stat.opt_list) {
    if (opt.exp_date <= date) {
      if (opt.opt_type == OptType::Put)
        opt.per_contract_delta = (spot < opt.strike) ? -1.0 : 0.0;
      else
        opt.per_contract_delta = (spot > opt.strike) ? 1.0 : 0.0;
      opt.per_contract_gamma = 0;
      opt.per_contract_theta = 0;
      continue;
    }
    int days = days_between(date, opt.exp_date);
    double t = std::max(days / YEAR_FRAC, 1e-6);
    double r = get_risk_free_rate(date, t);
    double q = get_dividend_yield(date);
    double iv = opt.implied_vol > 0 ? opt.implied_vol : 0.20;
    try {
      auto g = merton(opt.opt_type, spot, opt.strike, t, r, q, iv);
      opt.per_contract_delta = g.delta;
      opt.per_contract_gamma = g.gamma;
      opt.per_contract_theta = g.theta;
    } catch (...) {}
  }
}

std::vector<VerbosePosRow> build_pos_rows(const PositionsAndStats& stat, Date date,
                                          double spot) {
  std::vector<VerbosePosRow> rows;
  double opt_total = 0;
  for (const auto& opt : stat.opt_list) {
    double mark = stat.mark_option(opt, date, spot);
    double tot = static_cast<double>(opt.num_contracts) * mark * CONTRACT_SCALE;
    opt_total += tot;
    VerbosePosRow r;
    r.type = opt.opt_type == OptType::Call ? "Call" : "Put";
    r.contracts = opt.num_contracts;
    r.has_contracts = true;
    r.strike = opt.strike;
    r.has_strike = true;
    r.exp_date = opt.exp_date;
    r.has_exp = true;
    r.price = mark;
    r.has_price = true;
    r.total = tot;
    rows.push_back(r);
  }
  VerbosePosRow opt_sum;
  opt_sum.type = "Options";
  opt_sum.total = opt_total;
  rows.push_back(opt_sum);

  double stock_mtm = 0;
  int n_lots = 0;
  for (const auto& p : stat.stocks) {
    if (std::abs(p.num_shares) < 1e-9) continue;
    ++n_lots;
    VerbosePosRow stk;
    stk.type = "Stock";
    stk.contracts = p.num_shares;
    stk.has_contracts = true;
    stk.price = spot;
    stk.has_price = true;
    stk.cost_basis = p.open_price;
    stk.has_cost_basis = true;
    stk.total = p.num_shares * spot;
    stock_mtm += stk.total;
    rows.push_back(stk);
  }
  if (n_lots == 0) {
    VerbosePosRow stk;
    stk.type = "Stock";
    stk.contracts = 0;
    stk.has_contracts = true;
    stk.price = spot;
    stk.has_price = true;
    stk.total = 0;
    rows.push_back(stk);
  } else if (n_lots > 1) {
    VerbosePosRow stk_sum;
    stk_sum.type = "Stock";
    stk_sum.contracts = stat.total_shares();
    stk_sum.has_contracts = true;
    stk_sum.total = stock_mtm;
    rows.push_back(stk_sum);
  }

  VerbosePosRow cash;
  cash.type = "Cash";
  cash.total = stat.cash;
  rows.push_back(cash);

  VerbosePosRow nlv;
  nlv.type = "NLV";
  nlv.total = opt_total + stock_mtm + stat.cash;
  rows.push_back(nlv);
  return rows;
}

struct BhTracker {
  bool on = false;
  double shares = 0;
  double cash = 0;
  double peak = 0;
  double max_dd = 0;
  Date max_dd_date = 0;

  void init(Date d, double spot, double notional, double comm) {
    on = notional > 0 && spot > 0;
    if (!on) return;
    shares = std::floor(notional / (spot + comm));
    if (shares < 1.0) {
      cash = notional;
      shares = 0;
    } else {
      cash = notional - (spot + comm) * shares;
    }
    peak = notional;
    max_dd = 0;
    max_dd_date = d;
  }
  void mark(Date d, double spot) {
    if (!on) return;
    double nlv = cash + shares * spot;
    if (nlv > peak) peak = nlv;
    if (peak > 0) {
      double dd = (peak - std::max(nlv, 0.0)) / peak * 100.0;
      if (dd > 100.0) dd = 100.0;
      if (dd > max_dd) {
        max_dd = dd;
        max_dd_date = d;
      }
    }
  }
  double nlv(double spot) const { return on ? cash + shares * spot : 0; }
};

struct VerboseRecorder {
  std::vector<VerboseDay> days;
  VerboseDay cur;
  bool has_cur = false;
  bool snapped = false;
  bool snap_has_opts = false;
  BhTracker bh;

  void ensure_day(Date d, double spot) {
    if (has_cur && cur.date != d) flush();
    if (!has_cur) {
      cur = VerboseDay{};
      cur.date = d;
      cur.spot = spot;
      has_cur = true;
      snapped = false;
      snap_has_opts = false;
    }
  }

  void snapshot(const PositionsAndStats& stat, Date date, double spot, bool force) {
    ensure_day(date, spot);
    bool has_opts = !stat.opt_list.empty();
    if (snapped && !force && !(has_opts && !snap_has_opts)) return;
    cur.spot = spot;
    cur.positions = build_pos_rows(stat, date, spot);
    cur.nlv = stat.cash + stat.total_shares() * spot + stat.option_nlv(date, spot);
    snapped = true;
    snap_has_opts = has_opts;
  }

  void capture_end_stats(const PositionsAndStats& stat, Date date, double spot) {
    if (!has_cur) return;
    cur.peak = stat.cur_peak;
    cur.max_dd = stat.max_drawdown;
    cur.max_dd_date = stat.max_dd_date ? stat.max_dd_date : date;
    cur.st_loss_carry = stat.st_loss_carryover;
    cur.lt_loss_carry = stat.lt_loss_carryover;
    cur.st_gain = stat.annual_st_capital_gain;
    cur.lt_gain = stat.annual_lt_capital_gain;
    cur.bh_nlv = bh.nlv(spot);
    cur.bh_max_dd = bh.max_dd;
    cur.bh_max_dd_date = bh.max_dd_date;
    if (!snapped) snapshot(stat, date, spot, true);
  }

  void flush() {
    if (!has_cur) return;
    if (!cur.actions.empty()) days.push_back(std::move(cur));
    has_cur = false;
    snapped = false;
    snap_has_opts = false;
  }

  void add_open_w(const PositionsAndStats& stat) {
    VerboseAction a;
    a.kind = VerboseActionKind::OpenW;
    a.reason = "open";
    for (const auto& opt : stat.opt_list) {
      VerboseActionLeg L;
      L.type = opt.opt_type == OptType::Call ? "Call" : "Put";
      L.contracts = opt.num_contracts;
      L.strike = opt.strike;
      L.exp_date = opt.exp_date;
      L.price = opt.open_price;
      // Cash-flow sign: short credit is +, long debit is −.
      L.total = -opt.get_cost_basis();
      a.pnl += L.total;
      a.legs.push_back(L);
    }
    if (!a.legs.empty()) cur.actions.push_back(std::move(a));
  }

  void add_close_from_log(const TradeLog* log, size_t from, const char* reason) {
    if (!log) return;
    VerboseAction a;
    a.kind = VerboseActionKind::CloseW;
    a.reason = reason ? reason : "";
    for (size_t i = from; i < log->size(); ++i) {
      const auto& t = (*log)[i];
      if (t.action != "close") continue;
      VerboseActionLeg L;
      L.type = (t.put_call == 'p' || t.put_call == 'P') ? "Put" : "Call";
      L.contracts = t.num_contracts;
      L.strike = t.strike;
      L.exp_date = t.exp_date;
      L.cost_basis = t.open_price;
      L.price = t.close_price;
      L.total = t.pnl;
      double denom = std::abs(t.num_contracts) * CONTRACT_SCALE;
      L.pnl_per = denom > 0 ? t.pnl / denom : 0;
      a.pnl += t.pnl;
      a.legs.push_back(L);
    }
    if (!a.legs.empty()) cur.actions.push_back(std::move(a));
  }

  void add_other_from_log(const TradeLog* log, size_t from) {
    if (!log) return;
    size_t i = from;
    while (i < log->size()) {
      const auto& t = (*log)[i];
      if (t.action == "tax") {
        VerboseAction a;
        a.kind = VerboseActionKind::Tax;
        a.reason = t.reason;
        a.pnl = std::abs(t.pnl);
        cur.actions.push_back(std::move(a));
        ++i;
        continue;
      }
      if (t.action == "buy" || t.action == "sell") {
        const std::string act = t.action;
        const std::string reason = t.reason;
        double shares = 0, notional = 0;
        while (i < log->size() && (*log)[i].action == act &&
               (*log)[i].reason == reason) {
          const auto& s = (*log)[i];
          double sh = std::abs(s.shares);
          double px = (act == "buy")
                          ? s.open_price
                          : (s.close_price != 0 ? s.close_price : s.open_price);
          shares += sh;
          notional += sh * px;
          ++i;
        }
        VerboseAction a;
        a.kind = (act == "buy") ? VerboseActionKind::BuyStock
                                : VerboseActionKind::SellStock;
        a.reason = reason;
        a.shares = shares;
        a.price = shares > 0 ? notional / shares : 0;
        cur.actions.push_back(std::move(a));
        continue;
      }
      ++i;
    }
  }
};

}  // namespace

void buy_and_hold_baseline(const OptionsDataset& ds, double long_pos_mil,
                           double stock_comm_per_share, BacktestResult& result) {
  buy_and_hold_baseline_impl(ds, long_pos_mil, stock_comm_per_share, result);
}

BacktestResult w_strategy(const OptionsDataset& ds, const WParam& param, bool verbose) {
  BacktestResult result;
  if (ds.dates.empty()) {
    result.ok = false;
    result.error = "no data";
    return result;
  }
  const double long_pos_dollars = std::max(0.0, param.long_pos_mil) * 1e6;
  const double initial_capital = param.fixed.initial_balance + long_pos_dollars;

  if (long_pos_dollars > 0) {
    buy_and_hold_baseline(ds, param.long_pos_mil,
                          param.fixed.stock_comm_per_share, result);
  }

  PositionsAndStats stat;
  stat.all_data = &ds;
  stat.cash = initial_capital;
  TradeLog* log = verbose ? &result.trades : nullptr;
  VerboseRecorder rec;
  VerboseRecorder* recp = verbose ? &rec : nullptr;
  if (recp) {
    double bh_notional =
        long_pos_dollars > 0 ? long_pos_dollars : param.fixed.initial_balance;
    rec.bh.init(ds.dates.front(), ds.spot(ds.dates.front()), bh_notional,
                param.fixed.stock_comm_per_share);
  }
  auto log_mark = [&]() -> size_t { return log ? log->size() : 0; };
  auto rec_others = [&](size_t m, Date date, double spot) {
    if (!recp) return;
    recp->ensure_day(date, spot);
    recp->add_other_from_log(log, m);
  };
  auto rec_close = [&](size_t m, Date date, double spot, const char* reason) {
    if (!recp) return;
    recp->ensure_day(date, spot);
    recp->add_close_from_log(log, m, reason);
  };
  auto rec_open = [&](Date date, double spot) {
    if (!recp || stat.opt_list.empty()) return;
    recp->ensure_day(date, spot);
    recp->add_open_w(stat);
  };
  auto rec_snap_book = [&](Date date, double spot, bool force) {
    if (!recp) return;
    recp->snapshot(stat, date, spot, force);
  };

  std::vector<double> spots;
  if (param.eval_expected_pnl) spots.reserve(ds.dates.size());
  Date start = ds.dates.front();
  Date end = ds.dates.back();
  int prev_year = year_of(start);

  for (size_t i = 0; i < ds.dates.size(); ++i) {
    Date date = ds.dates[i];
    double spot = ds.spot(date);
    if (param.eval_expected_pnl) spots.push_back(spot);

    if (i == 0) {
      stat.initial_price = spot;
      if (long_pos_dollars > 0) {
        size_t m = log_mark();
        open_buy_and_hold(param, date, spot, stat, log);
        rec_others(m, date, spot);
        if (recp) recp->snapshot(stat, date, spot, /*force=*/false);
      }
      result.long_pos_shares = stat.buy_and_hold_shares();
      // Peak at full starting capital (cash + stock mark ≈ initial_capital).
      stat.cur_peak = initial_capital;
      stat.cur_peak_date = format_date(date);
      stat.peak_date = date;
    }
    if (year_of(date) != prev_year) {
      size_t m = log_mark();
      year_end_tax(param, date, spot, stat, /*invest_leftover=*/true, log);
      rec_others(m, date, spot);
      prev_year = year_of(date);
    }

    if (param.enable_gamma_scalp && !stat.opt_list.empty())
      update_greeks(date, spot, stat);

    // Mark options once per day for roll check + equity (was duplicated).
    double opt_nlv = 0.0;
    if (!stat.opt_list.empty()) {
      for (const auto& opt : stat.opt_list)
        opt_nlv += stat.mark_option(opt, date, spot) * opt.num_contracts * CONTRACT_SCALE;
      double denom = std::abs(stat.structure_entry_cash);
      if (denom < 1e-9) denom = 1.0;
      double pnl_pct = (opt_nlv - stat.structure_entry_cash) / denom;
      auto [do_roll, reason] = should_roll(param, date, stat, pnl_pct);
      if (do_roll) {
        if (recp) recp->snapshot(stat, date, spot, /*force=*/false);
        size_t m_close = log_mark();
        close_all_options(param, date, spot, stat, reason, log);
        rec_close(m_close, date, spot, reason);
        // Flatten gamma-scalp stock only; keep buy-and-hold overlay.
        size_t m_stk = log_mark();
        close_all_stock(param, date, spot, stat, /*include_buy_and_hold=*/false,
                        reason, log);
        rec_others(m_stk, date, spot);
        ++stat.num_rolls;
        // If overlay was sold down to zero, put close proceeds back into stock
        // before sizing the replacement W.
        if (long_pos_dollars > 0 && stat.buy_and_hold_shares() <= 0) {
          size_t m = log_mark();
          rebalance_overlay_cash(param, date, spot, stat, log);
          rec_others(m, date, spot);
        }
        open_w_structure(param, date, ds, stat, log);
        rec_open(date, spot);
        {
          size_t m = log_mark();
          rebalance_overlay_cash(param, date, spot, stat, log);
          rec_others(m, date, spot);
        }
        rec_snap_book(date, spot, /*force=*/true);
        opt_nlv = stat.option_nlv(date, spot);
      }
    } else {
      if (long_pos_dollars > 0 && stat.buy_and_hold_shares() <= 0) {
        size_t m = log_mark();
        rebalance_overlay_cash(param, date, spot, stat, log);
        rec_others(m, date, spot);
      }
      open_w_structure(param, date, ds, stat, log);
      rec_open(date, spot);
      {
        size_t m = log_mark();
        rebalance_overlay_cash(param, date, spot, stat, log);
        rec_others(m, date, spot);
      }
      rec_snap_book(date, spot, /*force=*/true);
      if (!stat.opt_list.empty())
        opt_nlv = stat.option_nlv(date, spot);
    }

    if (param.eval_expected_pnl && i % 5 == 0 && !stat.opt_list.empty()) {
      update_greeks(date, spot, stat);
      auto [exp, pnls, probs] = expected_structure_pnl(
          stat.opt_list, spot, date, stat.structure_entry_cash,
          param.expected_pnl_move_pcts, spots, std::max(param.roll_dte, 5),
          spots.size() > 40);
      (void)pnls; (void)probs;
      stat.last_expected_pnl = exp;
    }

    // Combined NLV: W-strategy (cash + options + scalp stock) + B&H shares.
    double nlv = stat.total_shares() * spot + opt_nlv;
    double equity = nlv + stat.cash;
    calc_drawdown(stat, date, equity);
    if (recp) {
      recp->bh.mark(date, spot);
      if (recp->has_cur && recp->cur.date == date)
        recp->capture_end_stats(stat, date, spot);
    }
    if (equity <= 0) {
      // Account wiped out: stop, report 100% DD and zero terminal NLV.
      stat.max_drawdown = 100.0;
      stat.max_dd_date = date;
      stat.cash = 0;
      stat.opt_list.clear();
      stat.stocks.clear();
      result.annual_return = -100.0;
      result.max_drawdown = 100.0;
      result.final_nlv = 0;
      result.total_tax = stat.total_tax;
      result.num_rolls = stat.num_rolls;
      result.num_gamma_hedges = stat.num_gamma_hedges;
      result.expected_pnl = stat.last_expected_pnl;
      result.units_used = stat.last_units_used;
      if (verbose) {
        if (recp) {
          recp->flush();
          result.verbose_log = std::move(rec.days);
        }
        std::cerr << "C++ backtest WIPED OUT on " << format_date(date)
                  << " dd=100% rolls=" << stat.num_rolls << "\n";
      }
      return result;
    }
  }

  Date last = ds.dates.back();
  double last_spot = ds.spot(last);
  if (!stat.opt_list.empty()) {
    if (recp) recp->snapshot(stat, last, last_spot, /*force=*/false);
    size_t m = log_mark();
    close_all_options(param, last, last_spot, stat, "final", log);
    rec_close(m, last, last_spot, "final");
  }
  // Close everything including buy-and-hold for a cash final NLV.
  if (std::abs(stat.total_shares()) > 1e-9) {
    size_t m = log_mark();
    close_all_stock(param, last, last_spot, stat, /*include_buy_and_hold=*/true,
                    "final", log);
    rec_others(m, last, last_spot);
  }
  {
    size_t m = log_mark();
    year_end_tax(param, last, last_spot, stat, /*invest_leftover=*/false, log);
    rec_others(m, last, last_spot);
  }
  if (recp) {
    recp->capture_end_stats(stat, last, last_spot);
    recp->flush();
    result.verbose_log = std::move(rec.days);
  }

  double years = std::max(days_between(start, end) / YEAR_FRAC, 1e-6);
  double final_nlv = stat.cash;
  double total_return = final_nlv / initial_capital;
  double annual = (total_return > 0)
                      ? (std::pow(total_return, 1.0 / years) - 1.0) * 100.0
                      : -100.0;

  result.annual_return = annual;
  result.max_drawdown = stat.max_drawdown;
  result.final_nlv = final_nlv;
  result.total_tax = stat.total_tax;
  result.num_rolls = stat.num_rolls;
  result.num_gamma_hedges = stat.num_gamma_hedges;
  result.expected_pnl = stat.last_expected_pnl;
  result.units_used = stat.last_units_used;
  if (verbose) {
    std::cerr << "C++ backtest annual=" << annual << "% dd=" << stat.max_drawdown
              << "% finalNLV=" << final_nlv
              << " tax=" << tax_pct_of_nlv(stat.total_tax, final_nlv) << "%NLV"
              << " rolls=" << stat.num_rolls
              << " trades=" << result.trades.size();
    if (long_pos_dollars > 0) {
      std::cerr << " | baseline B&H annual=" << result.baseline_annual_return
                << "% dd=" << result.baseline_max_drawdown
                << "% finalNLV=" << result.baseline_final_nlv
                << " shares=" << result.long_pos_shares
                << " hedgeRatio=" << param.hedge_ratio
                << " unitsUsed=" << result.units_used;
    }
    std::cerr << "\n";
  }
  return result;
}

}  // namespace wbt
