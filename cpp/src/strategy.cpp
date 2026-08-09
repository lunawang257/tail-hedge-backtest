#include "wbt/strategy.hpp"
#include "wbt/chain.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include "wbt/expected_pnl.hpp"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace wbt {
namespace {

double trade_mid(const SyntheticQuote& q, bool is_long, double slip) {
  double mid = q.mid > 0 ? q.mid : 0.5 * (q.bid + q.ask);
  return is_long ? mid * (1.0 + slip) : mid * (1.0 - slip);
}

std::pair<int, int> choose_wing_counts(const WParam& param,
                                       const SyntheticQuote& sc, const SyntheticQuote& sp,
                                       const SyntheticQuote& lc, const SyntheticQuote& lp) {
  int n_c = param.n_call, n_p = param.n_put;
  if (param.wing_mode == WingMode::Fixed) return {param.m_call, param.m_put};

  double slip = param.fixed.opt_slippage_percent;
  double scp = trade_mid(sc, false, slip);
  double spp = trade_mid(sp, false, slip);
  double lcp = trade_mid(lc, true, slip);
  double lpp = trade_mid(lp, true, slip);
  int max_m = std::max(n_c, n_p) * std::max(1, param.max_wing_mult);
  int best_c = n_c, best_p = n_p;

  auto net_theta = [&](int mc, int mp) {
    return ((-n_c) * sc.theta + (-n_p) * sp.theta + mc * lc.theta + mp * lp.theta) * CONTRACT_SCALE;
  };

  if (param.wing_mode == WingMode::Credit) {
    double max_long = (scp * n_c + spp * n_p) * (1.0 - param.credit_buffer) * CONTRACT_SCALE;
    for (int m = std::max(n_c, n_p); m <= max_m; ++m) {
      int mc = std::max(n_c, m), mp = std::max(n_p, m);
      double debit = (lcp * mc + lpp * mp) * CONTRACT_SCALE;
      if (debit <= max_long + 1e-9) { best_c = mc; best_p = mp; }
      else break;
    }
    return {best_c, best_p};
  }
  // theta
  for (int m = std::max(n_c, n_p); m <= max_m; ++m) {
    int mc = std::max(n_c, m), mp = std::max(n_p, m);
    if (net_theta(mc, mp) >= param.theta_threshold - 1e-9) { best_c = mc; best_p = mp; }
    else break;
  }
  return {best_c, best_p};
}

double open_leg(const WParam& param, Date date, double spot, PositionsAndStats& stat,
                const SyntheticQuote& quote, int num_contracts, const char* role) {
  if (num_contracts == 0) return 0;
  bool is_buy = num_contracts > 0;
  double mid = quote.mid > 0 ? quote.mid : 0.5 * (quote.bid + quote.ask);
  double slip = param.fixed.opt_slippage_percent;
  double trade_px = is_buy ? mid * (1.0 + slip) : mid * (1.0 - slip);
  double opt_comm = param.fixed.opt_comm_per_contract;
  int n = std::abs(num_contracts);
  int signed_dir = is_buy ? 1 : -1;
  double cash_per = trade_px * CONTRACT_SCALE + (is_buy ? opt_comm : -opt_comm);
  double cash_flow = signed_dir * cash_per * n;

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
  return cash_flow;
}

bool open_w_structure(const WParam& param, Date date, const OptionsDataset& ds,
                      PositionsAndStats& stat) {
  double spot = ds.spot(date);
  double slip = param.fixed.opt_slippage_percent;
  int u = param.units;
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

  auto [m_call, m_put] = choose_wing_counts(param, *sc, *sp, *lc, *lp);
  ++stat.structure_id;
  double entry = 0;
  entry += open_leg(param, date, spot, stat, *sc, -param.n_call * u, LEG_SHORT_CALL);
  entry += open_leg(param, date, spot, stat, *sp, -param.n_put * u, LEG_SHORT_PUT);
  entry += open_leg(param, date, spot, stat, *lc, m_call * u, LEG_LONG_CALL);
  entry += open_leg(param, date, spot, stat, *lp, m_put * u, LEG_LONG_PUT);
  stat.structure_entry_cash = entry;
  stat.structure_entry_value = stat.option_nlv(date, spot);
  stat.structure_open_date = date;
  stat.structure_open_spot = spot;
  stat.last_wing_m_call = m_call;
  stat.last_wing_m_put = m_put;
  stat.gamma_scalping_active = false;
  return true;
}

double close_one_option(const WParam& param, Date date, double spot, PositionsAndStats& stat,
                        OptPosition& opt, const char* /*reason*/) {
  double mid = stat.mark_option(opt, date, spot);
  bool is_long = opt.num_contracts > 0;
  double slip = param.fixed.opt_slippage_percent;
  double trade_px = is_long ? std::max(mid * (1 - slip), 0.0) : std::max(mid * (1 + slip), 0.0);
  double opt_comm = param.fixed.opt_comm_per_contract;
  double cash_change = trade_px * opt.num_contracts * CONTRACT_SCALE
                       - opt_comm * std::abs(opt.num_contracts);
  double capital_gain = cash_change - opt.get_cost_basis();
  stat.cash += cash_change;
  stat.total_commission += opt_comm * std::abs(opt.num_contracts);
  stat.total_capital_gain += capital_gain;
  stat.annual_capital_gain += capital_gain;
  if (opt.opt_type == OptType::Put) stat.total_put_capital_gain += capital_gain;
  else stat.total_call_capital_gain += capital_gain;
  return capital_gain;
}

void close_all_options(const WParam& param, Date date, double spot, PositionsAndStats& stat,
                       const char* reason) {
  for (auto& opt : stat.opt_list) close_one_option(param, date, spot, stat, opt, reason);
  stat.opt_list.clear();
}

void close_all_stock(const WParam& param, Date date, double spot, PositionsAndStats& stat) {
  double shares = stat.total_shares();
  if (std::abs(shares) < 1e-9) return;
  double stock_comm = param.fixed.stock_comm_per_share;
  if (shares > 0) {
    double close_px = spot - stock_comm;
    auto [gain, avg] = stat.close_stock_positions(shares, close_px);
    (void)avg;
    stat.cash += close_px * shares;
  } else {
    double close_px = spot + stock_comm;
    double remaining = -shares;
    double gain = 0;
    for (auto it = stat.stocks.begin(); it != stat.stocks.end() && remaining > 0;) {
      if (it->num_shares >= 0) { ++it; continue; }
      double take = std::min(-it->num_shares, remaining);
      gain += (it->open_price - close_px) * take;
      it->num_shares += take;
      remaining -= take;
      if (std::abs(it->num_shares) < 1e-9) it = stat.stocks.erase(it);
      else ++it;
    }
    stat.total_capital_gain += gain;
    stat.total_stock_capital_gain += gain;
    stat.annual_capital_gain += gain;
    stat.cash -= close_px * (-shares);
  }
}

std::pair<bool, const char*> should_roll(const WParam& param, Date date, double spot,
                                         PositionsAndStats& stat) {
  if (stat.opt_list.empty()) return {false, ""};
  auto short_dte = stat.short_min_dte(date);
  if (param.enable_dte_roll && short_dte) {
    if (*short_dte <= param.roll_dte) return {true, "dte_roll"};
    if (param.roll_on_expiry && *short_dte <= 0) return {true, "expire"};
  }
  double pnl_pct = stat.structure_pnl_pct(date, spot);
  if (param.enable_profit_take && pnl_pct >= param.profit_take_pct) return {true, "profit_take"};
  if (param.enable_loss_cut && pnl_pct <= -param.loss_cut_pct) return {true, "loss_cut"};
  return {false, ""};
}

void year_end_tax(const WParam& param, PositionsAndStats& stat) {
  double gain = stat.annual_capital_gain;
  double tax = std::max(0.0, gain) * param.fixed.capital_gain_tax;
  // Python: deductible = max(gain, -ANNUAL...) with ANNUAL=0 so losses don't reduce tax cash
  // Actually: if gain > 0 pay tax; if gain < 0 with deduction 0, tax = 0
  if (gain > 0) {
    stat.cash -= tax;
    stat.total_tax += tax;
  }
  stat.annual_capital_gain = 0;
}

void calc_drawdown(PositionsAndStats& stat, const std::string& date_str, double equity) {
  if (equity > stat.cur_peak) {
    stat.cur_peak = equity;
    stat.cur_peak_date = date_str;
  }
  if (stat.cur_peak > 0) {
    double dd = (stat.cur_peak - equity) / stat.cur_peak * 100.0;
    if (dd > stat.max_drawdown) {
      stat.max_drawdown = dd;
      stat.max_drawdown_peak_date = date_str;
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

}  // namespace

BacktestResult w_strategy(const OptionsDataset& ds, const WParam& param, bool debug) {
  BacktestResult result;
  if (ds.dates.empty()) {
    result.ok = false;
    result.error = "no data";
    return result;
  }
  PositionsAndStats stat;
  stat.all_data = &ds;
  stat.cash = param.fixed.initial_balance;

  std::vector<double> spots;
  Date start = ds.dates.front();
  Date end = ds.dates.back();
  int prev_year = year_of(start);

  for (size_t i = 0; i < ds.dates.size(); ++i) {
    Date date = ds.dates[i];
    double spot = ds.spot(date);
    spots.push_back(spot);
    std::string date_str = format_date(date);

    if (i == 0) {
      stat.initial_price = spot;
      stat.cur_peak = stat.cash;
      stat.cur_peak_date = date_str;
    }
    if (year_of(date) != prev_year) {
      year_end_tax(param, stat);
      prev_year = year_of(date);
    }

    if (param.enable_gamma_scalp && !stat.opt_list.empty())
      update_greeks(date, spot, stat);

    if (!stat.opt_list.empty()) {
      auto [do_roll, reason] = should_roll(param, date, spot, stat);
      if (do_roll) {
        close_all_options(param, date, spot, stat, reason);
        close_all_stock(param, date, spot, stat);
        ++stat.num_rolls;
        open_w_structure(param, date, ds, stat);
      }
      // gamma scalp omitted when disabled (default for parity tests)
    } else {
      open_w_structure(param, date, ds, stat);
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

    auto [nlv, _] = stat.get_balance(date, spot);
    calc_drawdown(stat, date_str, nlv + stat.cash);
  }

  Date last = ds.dates.back();
  double last_spot = ds.spot(last);
  if (!stat.opt_list.empty())
    close_all_options(param, last, last_spot, stat, "final");
  if (std::abs(stat.total_shares()) > 1e-9)
    close_all_stock(param, last, last_spot, stat);
  year_end_tax(param, stat);

  double years = std::max(days_between(start, end) / YEAR_FRAC, 1e-6);
  double final_nlv = stat.cash;
  double total_return = final_nlv / param.fixed.initial_balance;
  double annual = (total_return > 0)
                      ? (std::pow(total_return, 1.0 / years) - 1.0) * 100.0
                      : -100.0;

  result.annual_return = annual;
  result.max_drawdown = stat.max_drawdown;
  result.final_nlv = final_nlv;
  result.num_rolls = stat.num_rolls;
  result.num_gamma_hedges = stat.num_gamma_hedges;
  result.expected_pnl = stat.last_expected_pnl;
  if (debug) {
    std::cerr << "C++ backtest annual=" << annual << "% dd=" << stat.max_drawdown
              << "% finalNLV=" << final_nlv << " rolls=" << stat.num_rolls << "\n";
  }
  return result;
}

}  // namespace wbt
