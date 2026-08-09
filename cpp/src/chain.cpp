#include "wbt/chain.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include <cmath>
#include <algorithm>
#include <limits>

namespace wbt {

bool is_monthly_expiration(Date exp) {
  return exp == third_friday(year_of(exp), month_of(exp));
}

std::vector<Date> available_expirations(const OptionsDataset& ds, Date as_of) {
  auto exps = ds.expirations(as_of);
  std::vector<Date> out;
  for (Date e : exps) if (e >= as_of) out.push_back(e);
  return out;
}

std::vector<Date> monthly_expirations(const OptionsDataset& ds, Date as_of) {
  auto all = available_expirations(ds, as_of);
  std::vector<Date> m;
  for (Date e : all) if (is_monthly_expiration(e)) m.push_back(e);
  return m.empty() ? all : m;
}

std::pair<Date, Date> bracketing_expirations(const OptionsDataset& ds, Date as_of,
                                              int target_dte, bool prefer_monthly) {
  auto exps = prefer_monthly ? monthly_expirations(ds, as_of)
                             : available_expirations(ds, as_of);
  if (exps.empty()) return {0, 0};
  Date lo = 0, hi = 0;
  int best_below = -1, best_above = 1e9;
  Date nearest = exps[0];
  int best_abs = 1e9;
  for (Date e : exps) {
    int dte = days_between(as_of, e);
    int ad = std::abs(dte - target_dte);
    if (ad < best_abs) { best_abs = ad; nearest = e; }
    if (dte <= target_dte && dte >= best_below) { best_below = dte; lo = e; }
    if (dte >= target_dte && dte <= best_above) { best_above = dte; hi = e; }
  }
  if (lo && hi) return {lo, hi};
  return {nearest, 0};
}

bool needs_synthetic_tenor(const OptionsDataset& ds, Date as_of, int target_dte, int tol) {
  for (Date e : available_expirations(ds, as_of)) {
    if (std::abs(days_between(as_of, e) - target_dte) <= tol) return false;
  }
  return true;
}

static long long iv_key(OptType t, Date exp) {
  return (static_cast<long long>(exp) << 8) | static_cast<long long>(opt_type_char(t));
}

static std::vector<std::pair<double, double>> chain_iv_by_moneyness(
    const OptionsDataset& ds, OptType type, Date exp, double spot, Date as_of,
    double band = 0.45) {
  std::vector<std::pair<double, double>> pts;
  int days = std::max(days_between(as_of, exp), 1);
  double t = days / YEAR_FRAC;
  double r = get_risk_free_rate(as_of, t);
  double q = get_dividend_yield(as_of);
  double lo_k = spot * (1.0 - band), hi_k = spot * (1.0 + band);
  auto b = ds.begin(as_of), e = ds.end(as_of);
  if (!b) return pts;
  for (auto p = b; p != e; ++p) {
    if (p->put_call != type || p->exp_date != exp) continue;
    if (p->strike < lo_k || p->strike > hi_k) continue;
    double mid = p->mid();
    if (mid <= 0 || !std::isfinite(mid)) continue;
    auto san = sanitize_option_price_for_iv(type, spot, p->strike, t, r, q, mid);
    if (!san) continue;
    try {
      double iv = euro_implied_vol(type, spot, p->strike, t, r, q, *san);
      if (iv > 0 && std::isfinite(iv)) pts.push_back({p->strike / spot, iv});
    } catch (...) {}
  }
  std::sort(pts.begin(), pts.end());
  return pts;
}

static double interp_iv_at_moneyness(const std::vector<std::pair<double, double>>& pts,
                                     double m) {
  if (pts.empty()) return std::numeric_limits<double>::quiet_NaN();
  if (m <= pts.front().first) return pts.front().second;
  if (m >= pts.back().first) return pts.back().second;
  for (size_t i = 1; i < pts.size(); ++i) {
    if (pts[i - 1].first <= m && m <= pts[i].first) {
      double m0 = pts[i - 1].first, m1 = pts[i].first;
      double iv0 = pts[i - 1].second, iv1 = pts[i].second;
      if (m1 == m0) return iv0;
      double w = (m - m0) / (m1 - m0);
      return iv0 * (1 - w) + iv1 * w;
    }
  }
  return pts.back().second;
}

static double anchor_iv(const OptionsDataset& ds, OptType type, Date exp,
                        double spot, Date as_of) {
  int days = std::max(days_between(as_of, exp), 1);
  double t = days / YEAR_FRAC;
  double r = get_risk_free_rate(as_of, t);
  double q = get_dividend_yield(as_of);
  auto b = ds.begin(as_of), e = ds.end(as_of);
  if (!b) return 0.20;
  std::vector<std::pair<double, const OptionQuote*>> ranked;
  for (auto p = b; p != e; ++p) {
    if (p->put_call != type || p->exp_date != exp) continue;
    ranked.push_back({std::abs(p->strike - spot), p});
  }
  std::sort(ranked.begin(), ranked.end(),
            [](auto& a, auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < ranked.size() && i < 16; ++i) {
    auto* row = ranked[i].second;
    double mid = row->mid();
    if (mid <= 0) continue;
    auto san = sanitize_option_price_for_iv(type, spot, row->strike, t, r, q, mid);
    if (!san) continue;
    try {
      double iv = euro_implied_vol(type, spot, row->strike, t, r, q, *san);
      if (iv > 0 && std::isfinite(iv)) return iv;
    } catch (...) {}
  }
  return 0.20;
}

std::optional<double> interpolate_iv_for_dte(const OptionsDataset& ds, OptType type,
                                             Date as_of, double spot, double strike,
                                             int target_dte, IvCache* cache) {
  bool prefer_monthly = needs_synthetic_tenor(ds, as_of, target_dte);
  auto [lo, hi] = bracketing_expirations(ds, as_of, target_dte, prefer_monthly);
  if (!lo) {
    auto p = bracketing_expirations(ds, as_of, target_dte, false);
    lo = p.first; hi = p.second;
  }
  if (!lo) return std::nullopt;

  auto get_pts = [&](Date exp) {
    long long k = iv_key(type, exp);
    if (cache) {
      auto it = cache->find(k);
      if (it != cache->end()) return it->second;
    }
    auto pts = chain_iv_by_moneyness(ds, type, exp, spot, as_of);
    if (cache) (*cache)[k] = pts;
    return pts;
  };

  double moneyness = strike / spot;
  double t_target = std::max(target_dte, 1) / YEAR_FRAC;
  auto lo_pts = get_pts(lo);
  double iv_lo = interp_iv_at_moneyness(lo_pts, moneyness);
  if (!std::isfinite(iv_lo)) return std::nullopt;

  if (!hi || hi == lo) {
    double t_lo = std::max(days_between(as_of, lo), 1) / YEAR_FRAC;
    double var = iv_lo * iv_lo * t_lo;
    double iv = std::sqrt(std::max(var / t_target, 1e-12));
    if (t_target < t_lo) iv = std::max(iv_lo * 0.85, std::min(iv, iv_lo * 1.35));
    return iv;
  }

  auto hi_pts = get_pts(hi);
  double iv_hi = interp_iv_at_moneyness(hi_pts, moneyness);
  if (!std::isfinite(iv_hi)) return iv_lo;

  double t_lo = std::max(days_between(as_of, lo), 1) / YEAR_FRAC;
  double t_hi = std::max(days_between(as_of, hi), 1) / YEAR_FRAC;
  double var_lo = iv_lo * iv_lo * t_lo;
  double var_hi = iv_hi * iv_hi * t_hi;
  double iv;
  if (t_hi == t_lo) iv = iv_lo;
  else {
    double w = (t_target - t_lo) / (t_hi - t_lo);
    w = std::min(std::max(w, -0.5), 1.5);
    double var = var_lo * (1 - w) + var_hi * w;
    iv = std::sqrt(std::max(var / t_target, 1e-12));
  }
  return iv;
}

std::optional<SyntheticQuote> synthetic_option_quote(
    const OptionsDataset& ds, OptType type, Date as_of, double spot, double strike,
    int target_dte, double slip_pct, IvCache* cache) {
  Date target_exp = add_days(as_of, target_dte);
  double t = std::max(target_dte, 1) / YEAR_FRAC;
  double r = get_risk_free_rate(as_of, t);
  double q = get_dividend_yield(as_of);

  // Exact listed
  auto b = ds.begin(as_of), e = ds.end(as_of);
  if (b) {
    for (auto p = b; p != e; ++p) {
      if (p->put_call == type && p->exp_date == target_exp &&
          std::abs(p->strike - strike) < 1e-6) {
        double mid = p->mid();
        SyntheticQuote qte;
        qte.bid = p->bid; qte.ask = p->ask; qte.mid = mid;
        qte.strike = strike; qte.exp_date = target_exp; qte.data_date = as_of;
        qte.underlying = spot; qte.put_call = type; qte.interpolated = false;
        try {
          auto san = sanitize_option_price_for_iv(type, spot, strike, t, r, q, mid);
          double iv = san ? euro_implied_vol(type, spot, strike, t, r, q, *san) : 0.2;
          auto g = merton(type, spot, strike, t, r, q, iv);
          qte.implied_vol = iv; qte.delta = g.delta; qte.gamma = g.gamma; qte.theta = g.theta;
        } catch (...) {}
        return qte;
      }
    }
  }

  auto iv = interpolate_iv_for_dte(ds, type, as_of, spot, strike, target_dte, cache);
  if (!iv) return std::nullopt;
  auto g = merton(type, spot, strike, t, r, q, *iv);
  if (!std::isfinite(g.value)) return std::nullopt;
  SyntheticQuote qte;
  qte.mid = g.value;
  qte.bid = std::max(g.value * (1.0 - slip_pct), 0.0);
  qte.ask = g.value * (1.0 + slip_pct);
  // match Python: half = mid * slip; bid = mid - half
  double half = g.value * slip_pct;
  qte.bid = std::max(g.value - half, 0.0);
  qte.ask = g.value + half;
  qte.strike = strike; qte.exp_date = target_exp; qte.data_date = as_of;
  qte.underlying = spot; qte.put_call = type;
  qte.implied_vol = *iv; qte.delta = g.delta; qte.gamma = g.gamma; qte.theta = g.theta;
  qte.interpolated = true;
  return qte;
}

std::optional<SyntheticQuote> find_option_by_delta(
    const OptionsDataset& ds, OptType type, Date as_of, double spot,
    double target_abs_delta, int target_dte, bool prefer_otm,
    double slip_pct, bool allow_interpolate, IvCache* cache) {
  double target_delta = (type == OptType::Put ? -1.0 : 1.0) * std::abs(target_abs_delta);
  bool synthetic = allow_interpolate && needs_synthetic_tenor(ds, as_of, target_dte);
  auto [lo, hi] = bracketing_expirations(ds, as_of, target_dte, synthetic);
  if (!lo) {
    auto p = bracketing_expirations(ds, as_of, target_dte, false);
    lo = p.first; hi = p.second;
  }
  if (!lo) return std::nullopt;

  Date nearest = lo;
  if (hi) {
    int dlo = std::abs(days_between(as_of, lo) - target_dte);
    int dhi = std::abs(days_between(as_of, hi) - target_dte);
    if (dhi < dlo) nearest = hi;
  }
  int listed_dte = std::max(days_between(as_of, nearest), 1);
  bool need_interp = allow_interpolate && (synthetic || std::abs(listed_dte - target_dte) >= 2);
  int use_dte = need_interp ? target_dte : listed_dte;

  double width = 0.08 + 0.55 * (1.0 - std::abs(target_abs_delta));
  double lo_k = spot * (1.0 - width), hi_k = spot * (1.0 + width);

  double t = std::max(use_dte, 1) / YEAR_FRAC;
  double r = get_risk_free_rate(as_of, t);
  double q = get_dividend_yield(as_of);
  double anchor = anchor_iv(ds, type, nearest, spot, as_of);

  struct Cand { double err; double k; double mid; };
  std::vector<Cand> ranked;
  auto b = ds.begin(as_of), e = ds.end(as_of);
  if (!b) return std::nullopt;
  for (auto p = b; p != e; ++p) {
    if (p->put_call != type || p->exp_date != nearest) continue;
    if (p->strike < lo_k || p->strike > hi_k) continue;
    double mid = p->mid();
    if (mid <= 0) continue;
    if (prefer_otm) {
      if (type == OptType::Call && p->strike < spot * 0.97) continue;
      if (type == OptType::Put && p->strike > spot * 1.03) continue;
    }
    try {
      auto g = merton(type, spot, p->strike, t, r, q, anchor);
      if (!std::isfinite(g.delta)) continue;
      ranked.push_back({std::abs(g.delta - target_delta), p->strike, mid});
    } catch (...) {}
  }
  if (ranked.empty()) {
    for (auto p = b; p != e; ++p) {
      if (p->put_call != type || p->exp_date != nearest) continue;
      double mid = p->mid();
      if (mid <= 0) continue;
      try {
        auto g = merton(type, spot, p->strike, t, r, q, anchor);
        ranked.push_back({std::abs(g.delta - target_delta), p->strike, mid});
      } catch (...) {}
    }
  }
  if (ranked.empty()) return std::nullopt;
  std::sort(ranked.begin(), ranked.end(),
            [](const Cand& a, const Cand& b) { return a.err < b.err; });

  IvCache local;
  IvCache* use_cache = cache ? cache : &local;
  std::optional<SyntheticQuote> best;
  double best_err = 1e99;
  size_t n = std::min(ranked.size(), size_t(8));
  for (size_t i = 0; i < n; ++i) {
    std::optional<SyntheticQuote> quote;
    if (need_interp) {
      quote = synthetic_option_quote(ds, type, as_of, spot, ranked[i].k, use_dte,
                                     slip_pct, use_cache);
    } else {
      try {
        auto san = sanitize_option_price_for_iv(
            type, spot, ranked[i].k, t, r, q, ranked[i].mid);
        double iv = san ? euro_implied_vol(type, spot, ranked[i].k, t, r, q, *san)
                        : anchor;
        auto g = merton(type, spot, ranked[i].k, t, r, q, iv);
        SyntheticQuote qte;
        double half = ranked[i].mid * slip_pct;
        qte.bid = std::max(ranked[i].mid - half, 0.0);
        qte.ask = ranked[i].mid + half;
        qte.mid = ranked[i].mid;
        qte.strike = ranked[i].k;
        qte.exp_date = nearest;
        qte.data_date = as_of;
        qte.underlying = spot;
        qte.put_call = type;
        qte.implied_vol = iv;
        qte.delta = g.delta; qte.gamma = g.gamma; qte.theta = g.theta;
        qte.interpolated = false;
        quote = qte;
      } catch (...) { continue; }
    }
    if (!quote) continue;
    double err = std::abs(quote->delta - target_delta);
    if (err < best_err) { best_err = err; best = quote; }
  }
  return best;
}

}  // namespace wbt
