#include "wbt/positions.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace wbt {

double PositionsAndStats::total_shares() const {
  double s = 0;
  for (auto& p : stocks) s += p.num_shares;
  return s;
}

double PositionsAndStats::mark_option(const OptPosition& opt, Date as_of, double spot) const {
  if (opt.exp_date <= as_of) {
    if (opt.opt_type == OptType::Put) return std::max(0.0, opt.strike - spot);
    return std::max(0.0, spot - opt.strike);
  }
  if (opt.implied_vol > 0) {
    int days = days_between(as_of, opt.exp_date);
    double t = std::max(days / YEAR_FRAC, 1e-6);
    try {
      double r = get_risk_free_rate(as_of, t);
      double q = get_dividend_yield(as_of);
      auto g = merton(opt.opt_type, spot, opt.strike, t, r, q, opt.implied_vol);
      if (std::isfinite(g.value)) return g.value;
    } catch (...) {}
  }
  // Fallback: chain mid
  if (all_data) {
    auto b = all_data->begin(as_of), e = all_data->end(as_of);
    if (b) {
      for (auto p = b; p != e; ++p) {
        if (p->put_call == opt.opt_type && p->exp_date == opt.exp_date &&
            std::abs(p->strike - opt.strike) < 1e-6) {
          return p->mid();
        }
      }
    }
  }
  return 0.0;
}

std::pair<double, double> PositionsAndStats::get_balance(Date as_of, double spot) const {
  double nlv = total_shares() * spot;
  double opt_nlv = 0;
  for (auto& opt : opt_list) {
    opt_nlv += mark_option(opt, as_of, spot) * opt.num_contracts * CONTRACT_SCALE;
  }
  nlv += opt_nlv;
  return {nlv, opt_nlv};
}

double PositionsAndStats::option_nlv(Date as_of, double spot) const {
  return get_balance(as_of, spot).second;
}

double PositionsAndStats::structure_pnl(Date as_of, double spot) const {
  if (opt_list.empty()) return 0;
  return option_nlv(as_of, spot) - structure_entry_cash;
}

double PositionsAndStats::structure_pnl_pct(Date as_of, double spot) const {
  double denom = std::abs(structure_entry_cash);
  if (denom < 1e-9) denom = 1.0;
  return structure_pnl(as_of, spot) / denom;
}

std::tuple<double, double, double> PositionsAndStats::net_greeks() const {
  double d = total_shares(), g = 0, th = 0;
  for (auto& opt : opt_list) {
    d += opt.num_contracts * opt.per_contract_delta * CONTRACT_SCALE;
    g += opt.num_contracts * opt.per_contract_gamma * CONTRACT_SCALE;
    th += opt.num_contracts * opt.per_contract_theta * CONTRACT_SCALE;
  }
  return {d, g, th / YEAR_FRAC};  // daily theta like Python
}

std::optional<int> PositionsAndStats::short_min_dte(Date as_of) const {
  std::optional<int> best;
  for (auto& opt : opt_list) {
    if (opt.num_contracts >= 0) continue;
    int dte = days_between(as_of, opt.exp_date);
    if (!best || dte < *best) best = dte;
  }
  return best;
}

void PositionsAndStats::open_stock(double shares, double px, Date d,
                                   bool buy_and_hold) {
  stocks.push_back(StockPos{shares, px, d, buy_and_hold});
}

double PositionsAndStats::buy_and_hold_shares() const {
  double s = 0;
  for (auto& p : stocks) {
    if (p.buy_and_hold) s += p.num_shares;
  }
  return s;
}

void PositionsAndStats::add_realized_gain(double gain, Date open_date,
                                          Date close_date, bool is_stock,
                                          bool is_put) {
  total_capital_gain += gain;
  if (is_stock) total_stock_capital_gain += gain;
  else if (is_put) total_put_capital_gain += gain;
  else total_call_capital_gain += gain;
  if (is_long_term_hold(open_date, close_date))
    annual_lt_capital_gain += gain;
  else
    annual_st_capital_gain += gain;
}

void PositionsAndStats::fill_trade_snapshot(TradeRecord& tr, Date date,
                                            double spot) const {
  tr.bh_shares = buy_and_hold_shares();
  tr.total_tax = total_tax;
  tr.st_loss_carryover = st_loss_carryover;
  tr.lt_loss_carryover = lt_loss_carryover;
  tr.st_capital_gain = annual_st_capital_gain;
  tr.lt_capital_gain = annual_lt_capital_gain;
  tr.cash = cash;
  tr.stock_nlv = total_shares() * spot;
  tr.total_nlv = tr.cash + tr.stock_nlv + option_nlv(date, spot);
}

void PositionsAndStats::log_stock_trade(std::vector<TradeRecord>* log, Date date,
                                        double spot, const char* action,
                                        const char* reason,
                                        double signed_shares, double open_px,
                                        double close_px, double cost_basis,
                                        double st_gain, double lt_gain) {
  if (!log) return;
  TradeRecord tr;
  tr.date = date;
  tr.action = action ? action : "";
  tr.structure_id = structure_id;
  tr.leg_role = "stock";
  tr.put_call = 's';
  tr.shares = signed_shares;
  tr.open_price = open_px;
  tr.close_price = close_px;
  tr.pnl = st_gain + lt_gain;
  tr.reason = reason ? reason : "";
  tr.spot = spot;
  tr.cost_basis = cost_basis;
  tr.trade_st_gain = st_gain;
  tr.trade_lt_gain = lt_gain;
  fill_trade_snapshot(tr, date, spot);
  log->push_back(std::move(tr));
}

static void split_hold_gain(Date open_date, Date close_date, double gain,
                            double& st_gain, double& lt_gain) {
  if (is_long_term_hold(open_date, close_date)) {
    st_gain = 0;
    lt_gain = gain;
  } else {
    st_gain = gain;
    lt_gain = 0;
  }
}

std::pair<double, double> PositionsAndStats::close_stock_positions(
    double shares, double px, Date as_of, bool include_buy_and_hold,
    double stock_comm, std::vector<TradeRecord>* log, const char* reason,
    double spot) {
  // Close long shares FIFO; returns (gain, avg_open).
  // By default skips buy-and-hold overlay lots (roll / gamma flatten).
  double remaining = shares;
  double gain = 0, closed = 0, cost = 0;
  for (auto it = stocks.begin(); it != stocks.end() && remaining > 1e-9;) {
    if (it->num_shares <= 0) { ++it; continue; }
    if (it->buy_and_hold && !include_buy_and_hold) { ++it; continue; }
    double take = std::min(it->num_shares, remaining);
    double lot_cost = it->open_price * take;
    double lot_gain = (px - it->open_price) * take;
    add_realized_gain(lot_gain, it->open_date, as_of, /*is_stock=*/true);
    gain += lot_gain;
    cost += lot_cost;
    closed += take;
    Date lot_open = it->open_date;
    double lot_px = it->open_price;
    it->num_shares -= take;
    remaining -= take;
    cash += px * take;
    total_commission += stock_comm * take;
    if (it->num_shares < 1e-9) it = stocks.erase(it);
    else ++it;
    double st_g = 0, lt_g = 0;
    split_hold_gain(lot_open, as_of, lot_gain, st_g, lt_g);
    log_stock_trade(log, as_of, spot > 0 ? spot : px, "sell", reason, -take,
                    lot_px, px, lot_cost, st_g, lt_g);
  }
  double avg = closed > 0 ? cost / closed : 0;
  return {gain, avg};
}

double PositionsAndStats::sell_overlay_hifo(double cash_needed, double spot,
                                            Date d, double stock_comm,
                                            std::vector<TradeRecord>* log,
                                            const char* reason) {
  if (cash_needed <= 1e-9 || spot <= 0) return 0;
  double net_px = spot - stock_comm;
  if (net_px <= 1e-12) return 0;

  std::vector<size_t> idx;
  idx.reserve(stocks.size());
  for (size_t i = 0; i < stocks.size(); ++i) {
    if (stocks[i].buy_and_hold && stocks[i].num_shares > 1e-9)
      idx.push_back(i);
  }
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    if (stocks[a].open_price != stocks[b].open_price)
      return stocks[a].open_price > stocks[b].open_price;
    return stocks[a].open_date > stocks[b].open_date;
  });

  double raised = 0;
  double remaining_need = cash_needed;
  for (size_t i : idx) {
    if (remaining_need <= 1e-9) break;
    auto& lot = stocks[i];
    if (lot.num_shares <= 1e-9) continue;
    double shares_need = remaining_need / net_px;
    double take = std::min(lot.num_shares, shares_need);
    double proceeds = take * net_px;
    double lot_cost = lot.open_price * take;
    double lot_gain = (net_px - lot.open_price) * take;
    Date lot_open = lot.open_date;
    double lot_px = lot.open_price;
    add_realized_gain(lot_gain, lot_open, d, /*is_stock=*/true);
    lot.num_shares -= take;
    cash += proceeds;
    raised += proceeds;
    remaining_need -= proceeds;
    total_commission += stock_comm * take;
    double st_g = 0, lt_g = 0;
    split_hold_gain(lot_open, d, lot_gain, st_g, lt_g);
    log_stock_trade(log, d, spot, "sell", reason, -take, lot_px, net_px,
                    lot_cost, st_g, lt_g);
  }
  stocks.erase(std::remove_if(stocks.begin(), stocks.end(),
                              [](const StockPos& p) {
                                return std::abs(p.num_shares) < 1e-9;
                              }),
               stocks.end());
  return raised;
}

double PositionsAndStats::buy_overlay(double cash_budget, double spot, Date d,
                                      double stock_comm,
                                      std::vector<TradeRecord>* log,
                                      const char* reason) {
  double px = spot + stock_comm;
  if (px <= 1e-12 || cash_budget <= 0) return 0;
  double shares = std::floor(cash_budget / px);
  if (shares < 1.0) return 0;
  open_stock(shares, px, d, /*buy_and_hold=*/true);
  double spent = px * shares;
  cash -= spent;
  total_commission += stock_comm * shares;
  log_stock_trade(log, d, spot, "buy", reason, shares, px, 0, 0, 0, 0);
  return spent;
}

}  // namespace wbt
