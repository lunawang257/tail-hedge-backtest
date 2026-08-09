#include "wbt/positions.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include <cmath>
#include <algorithm>

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

void PositionsAndStats::open_stock(double shares, double px, Date d) {
  stocks.push_back(StockPos{shares, px, d});
}

std::pair<double, double> PositionsAndStats::close_stock_positions(double shares, double px) {
  // Close long shares FIFO; returns (gain, avg_open)
  double remaining = shares;
  double gain = 0, closed = 0, cost = 0;
  for (auto it = stocks.begin(); it != stocks.end() && remaining > 1e-9;) {
    if (it->num_shares <= 0) { ++it; continue; }
    double take = std::min(it->num_shares, remaining);
    gain += (px - it->open_price) * take;
    cost += it->open_price * take;
    closed += take;
    it->num_shares -= take;
    remaining -= take;
    if (it->num_shares < 1e-9) it = stocks.erase(it);
    else ++it;
  }
  double avg = closed > 0 ? cost / closed : 0;
  total_capital_gain += gain;
  total_stock_capital_gain += gain;
  annual_capital_gain += gain;
  return {gain, avg};
}

}  // namespace wbt
