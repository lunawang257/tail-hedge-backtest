#include "wbt/expected_pnl.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace wbt {
namespace {

double realized_vol(const std::vector<double>& spots, int lookback = 20) {
  if (spots.size() < 3) return 0.20;
  size_t n = spots.size();
  size_t start = n > static_cast<size_t>(lookback) ? n - lookback : 0;
  std::vector<double> rets;
  for (size_t i = start + 1; i < n; ++i)
    rets.push_back(std::log(spots[i] / spots[i - 1]));
  if (rets.empty()) return 0.20;
  double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
  double var = 0;
  for (double r : rets) var += (r - mean) * (r - mean);
  var /= std::max<size_t>(1, rets.size() - 1);
  return std::sqrt(var) * std::sqrt(252.0);
}

double norm_cdf_local(double x) {
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

std::map<double, double> lognormal_probs(double /*spot*/, const std::vector<double>& move_pcts,
                                         double vol, int days, double r) {
  std::vector<double> moves = move_pcts;
  std::sort(moves.begin(), moves.end());
  double t = std::max(days, 1) / YEAR_FRAC;
  std::map<double, double> probs;
  if (vol <= 0 || t <= 0) {
    for (double m : moves) probs[m] = (std::abs(m) < 1e-12) ? 1.0 : 0.0;
    return probs;
  }
  double mu = (r - 0.5 * vol * vol) * t;
  double sig = vol * std::sqrt(t);
  std::vector<double> edges;
  edges.push_back(-1e300);
  for (size_t i = 0; i + 1 < moves.size(); ++i)
    edges.push_back(0.5 * (moves[i] + moves[i + 1]));
  edges.push_back(1e300);

  auto to_z = [&](double ret) {
    if (ret <= -0.999999) return -1e300;
    return (std::log(1.0 + ret) - mu) / sig;
  };
  double total = 0;
  for (size_t i = 0; i < moves.size(); ++i) {
    double p = norm_cdf_local(to_z(edges[i + 1])) - norm_cdf_local(to_z(edges[i]));
    probs[moves[i]] = p;
    total += p;
  }
  if (total <= 0) {
    for (double m : moves) probs[m] = 1.0 / moves.size();
  } else {
    for (auto& kv : probs) kv.second /= total;
  }
  return probs;
}

std::map<double, double> empirical_probs(const std::vector<double>& spots,
                                         const std::vector<double>& move_pcts, int horizon) {
  std::vector<double> moves = move_pcts;
  std::sort(moves.begin(), moves.end());
  std::map<double, double> probs;
  if (static_cast<int>(spots.size()) <= horizon + 1) {
    for (double m : moves) probs[m] = 1.0 / moves.size();
    return probs;
  }
  std::vector<double> edges;
  edges.push_back(-1e300);
  for (size_t i = 0; i + 1 < moves.size(); ++i)
    edges.push_back(0.5 * (moves[i] + moves[i + 1]));
  edges.push_back(1e300);
  std::map<double, int> counts;
  for (double m : moves) counts[m] = 0;
  for (size_t i = 0; i + horizon < spots.size(); ++i) {
    double ret = spots[i + horizon] / spots[i] - 1.0;
    for (size_t j = 0; j < moves.size(); ++j) {
      if (edges[j] <= ret && ret < edges[j + 1]) { counts[moves[j]]++; break; }
    }
  }
  int total = 0;
  for (auto& kv : counts) total += kv.second;
  if (total == 0) total = 1;
  for (double m : moves) probs[m] = counts[m] / double(total);
  return probs;
}

double structure_pnl_at_spot(const std::vector<OptPosition>& opts, double new_spot,
                             Date as_of, double entry_cash, int days_to_horizon) {
  Date mark = add_days(as_of, days_to_horizon);
  double opt_nlv = 0;
  for (auto& opt : opts) {
    int days = days_between(mark, opt.exp_date);
    double mid = 0;
    if (days <= 0) {
      mid = (opt.opt_type == OptType::Put)
                ? std::max(0.0, opt.strike - new_spot)
                : std::max(0.0, new_spot - opt.strike);
    } else {
      double t = days / YEAR_FRAC;
      double r = get_risk_free_rate(as_of, t);
      double q = get_dividend_yield(as_of);
      double iv = opt.implied_vol > 0 ? opt.implied_vol : 0.20;
      try {
        mid = merton(opt.opt_type, new_spot, opt.strike, t, r, q, iv).value;
        if (!std::isfinite(mid)) mid = 0;
      } catch (...) { mid = 0; }
    }
    opt_nlv += mid * opt.num_contracts * CONTRACT_SCALE;
  }
  return opt_nlv - entry_cash;
}

}  // namespace

std::tuple<double, std::map<double, double>, std::map<double, double>>
expected_structure_pnl(const std::vector<OptPosition>& opts, double spot, Date as_of,
                       double entry_cash, const std::vector<double>& move_pcts,
                       const std::vector<double>& spots_history, int horizon_days,
                       bool use_empirical) {
  if (opts.empty()) return {0.0, {}, {}};
  double vol = realized_vol(spots_history.empty() ? std::vector<double>{spot} : spots_history);
  double r = get_risk_free_rate(as_of, horizon_days / YEAR_FRAC);
  auto probs = (use_empirical && spots_history.size() > 40)
                   ? empirical_probs(spots_history, move_pcts, horizon_days)
                   : lognormal_probs(spot, move_pcts, vol, horizon_days, r);
  std::map<double, double> pnls;
  double expected = 0;
  for (auto& [m, p] : probs) {
    double pnl = structure_pnl_at_spot(opts, spot * (1.0 + m), as_of, entry_cash, horizon_days);
    pnls[m] = pnl;
    expected += pnl * p;
  }
  return {expected, pnls, probs};
}

}  // namespace wbt
