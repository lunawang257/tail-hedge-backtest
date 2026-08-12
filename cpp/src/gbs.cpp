#include "wbt/gbs.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace wbt {

namespace {
constexpr double MIN_T = 1.0 / 1000.0;
constexpr double MIN_V = 0.005;
constexpr double MAX_V = 2.0;
constexpr double INV_SQRT_2PI = 0.3989422804014327;

// Abramowitz & Stegun 26.2.17 — ~1e-7 abs error vs erfc, much cheaper.
inline double norm_cdf_as(double x) noexcept {
  constexpr double b1 = 0.319381530;
  constexpr double b2 = -0.356563782;
  constexpr double b3 = 1.781477937;
  constexpr double b4 = -1.821255978;
  constexpr double b5 = 1.330274429;
  constexpr double p = 0.2316419;
  const double ax = std::fabs(x);
  const double t = 1.0 / (1.0 + p * ax);
  const double poly = ((((b5 * t + b4) * t + b3) * t + b2) * t + b1) * t;
  const double y = INV_SQRT_2PI * std::exp(-0.5 * ax * ax) * poly;
  return x >= 0.0 ? 1.0 - y : y;
}

// Same A&S form but reuses a caller-supplied exp(-0.5*x*x).
inline double norm_cdf_with_exp(double x, double exp_neg_half_x2) noexcept {
  constexpr double b1 = 0.319381530;
  constexpr double b2 = -0.356563782;
  constexpr double b3 = 1.781477937;
  constexpr double b4 = -1.821255978;
  constexpr double b5 = 1.330274429;
  constexpr double p = 0.2316419;
  const double ax = std::fabs(x);
  const double t = 1.0 / (1.0 + p * ax);
  const double poly = ((((b5 * t + b4) * t + b3) * t + b2) * t + b1) * t;
  const double y = INV_SQRT_2PI * exp_neg_half_x2 * poly;
  return x >= 0.0 ? 1.0 - y : y;
}

struct GbsFixed {
  OptType type;
  double fs = 0, x = 0, t = 0, r = 0, b = 0;
  double t_sqrt = 0, log_fx = 0, ebrt = 0, ert = 0;
};

inline GbsFixed make_fixed(OptType type, double fs, double x, double t, double r,
                           double b) noexcept {
  if (t < MIN_T) t = MIN_T;
  GbsFixed c;
  c.type = type;
  c.fs = fs;
  c.x = x;
  c.t = t;
  c.r = r;
  c.b = b;
  c.t_sqrt = std::sqrt(t);
  c.log_fx = std::log(fs / x);
  c.ebrt = std::exp((b - r) * t);
  c.ert = std::exp(-r * t);
  return c;
}

inline void value_vega_fixed(const GbsFixed& c, double v, double& value,
                             double& vega, double* d1_out = nullptr,
                             double* d2_out = nullptr) noexcept {
  if (v < MIN_V) v = MIN_V;
  if (v > MAX_V) v = MAX_V;
  const double d1 = (c.log_fx + (c.b + 0.5 * v * v) * c.t) / (v * c.t_sqrt);
  const double d2 = d1 - v * c.t_sqrt;
  if (d1_out) *d1_out = d1;
  if (d2_out) *d2_out = d2;
  const double e1 = std::exp(-0.5 * d1 * d1);
  const double pdf_d1 = INV_SQRT_2PI * e1;
  vega = c.ebrt * c.fs * c.t_sqrt * pdf_d1;
  const double nd1 = norm_cdf_with_exp(d1, e1);
  const double nd2 = norm_cdf_as(d2);
  if (c.type == OptType::Call) {
    value = c.fs * c.ebrt * nd1 - c.x * c.ert * nd2;
  } else {
    // Φ(-z) = 1 - Φ(z)
    value = c.x * c.ert * (1.0 - nd2) - c.fs * c.ebrt * (1.0 - nd1);
  }
}
}  // namespace

double norm_pdf(double x) {
  return INV_SQRT_2PI * std::exp(-0.5 * x * x);
}

double norm_cdf(double x) {
  return norm_cdf_as(x);
}

Greeks gbs(OptType type, double fs, double x, double t, double r, double b, double v) {
  if (t < MIN_T) t = MIN_T;
  if (v < MIN_V) v = MIN_V;
  if (v > MAX_V) v = MAX_V;

  const double t_sqrt = std::sqrt(t);
  const double d1 = (std::log(fs / x) + (b + 0.5 * v * v) * t) / (v * t_sqrt);
  const double d2 = d1 - v * t_sqrt;
  const double e1 = std::exp(-0.5 * d1 * d1);
  const double pdf_d1 = INV_SQRT_2PI * e1;
  const double ebrt = std::exp((b - r) * t);
  const double ert = std::exp(-r * t);
  const double nd1 = norm_cdf_with_exp(d1, e1);
  const double nd2 = norm_cdf_as(d2);
  Greeks g;

  if (type == OptType::Call) {
    g.value = fs * ebrt * nd1 - x * ert * nd2;
    g.delta = ebrt * nd1;
    g.gamma = ebrt * pdf_d1 / (fs * v * t_sqrt);
    g.theta = -(fs * v * ebrt * pdf_d1) / (2.0 * t_sqrt)
              - (b - r) * fs * ebrt * nd1
              - r * x * ert * nd2;
    g.vega = ebrt * fs * t_sqrt * pdf_d1;
    g.rho = x * t * ert * nd2;
  } else {
    const double nmd1 = 1.0 - nd1;
    const double nmd2 = 1.0 - nd2;
    g.value = x * ert * nmd2 - fs * ebrt * nmd1;
    g.delta = -ebrt * nmd1;
    g.gamma = ebrt * pdf_d1 / (fs * v * t_sqrt);
    g.theta = -(fs * v * ebrt * pdf_d1) / (2.0 * t_sqrt)
              + (b - r) * fs * ebrt * nmd1
              + r * x * ert * nmd2;
    g.vega = ebrt * fs * t_sqrt * pdf_d1;
    g.rho = -x * t * ert * nmd2;
  }
  return g;
}

Greeks merton(OptType type, double fs, double x, double t, double r, double q, double v) {
  return gbs(type, fs, x, t, r, r - q, v);
}

double merton_delta(OptType type, double fs, double x, double t, double r, double q, double v) {
  double b = r - q;
  if (t < MIN_T) t = MIN_T;
  if (v < MIN_V) v = MIN_V;
  if (v > MAX_V) v = MAX_V;
  const double t_sqrt = std::sqrt(t);
  const double d1 = (std::log(fs / x) + (b + 0.5 * v * v) * t) / (v * t_sqrt);
  const double disc = std::exp((b - r) * t);
  if (type == OptType::Call) return disc * norm_cdf_as(d1);
  return -disc * norm_cdf_as(-d1);
}

void gbs_value_vega(OptType type, double fs, double x, double t, double r, double b,
                    double v, double& value, double& vega) {
  auto c = make_fixed(type, fs, x, t, r, b);
  value_vega_fixed(c, v, value, vega);
}

static double approx_implied_vol(OptType type, double fs, double x, double t,
                                 double /*r*/, double /*b*/, double cp, double ebrt,
                                 double ert) {
  const double a = std::sqrt(2.0 * M_PI) / (fs * ebrt + x * ert);
  const double payoff =
      (type == OptType::Call) ? (fs * ebrt - x * ert) : (x * ert - fs * ebrt);
  const double bb = cp - payoff / 2.0;
  const double c = (payoff * payoff) / M_PI;
  return (a * (bb + std::sqrt(bb * bb + c))) / std::sqrt(t);
}

static double bisection_implied_vol(const GbsFixed& c, double cp, double precision,
                                    int max_steps) {
  double v_mid = approx_implied_vol(c.type, c.fs, c.x, c.t, c.r, c.b, cp, c.ebrt, c.ert);
  double v_low, v_high;
  if (v_mid <= MIN_V || v_mid >= MAX_V) {
    v_low = MIN_V;
    v_high = MAX_V;
    v_mid = 0.5 * (v_low + v_high);
  } else {
    v_low = std::max(MIN_V, v_mid * 0.5);
    v_high = std::min(MAX_V, v_mid * 1.5);
  }

  double cp_low, cp_high, unused;
  value_vega_fixed(c, v_low, cp_low, unused);
  value_vega_fixed(c, v_high, cp_high, unused);

  if (cp <= cp_low) return v_low;
  if (cp >= cp_high) return v_high;

  for (int i = 0; i < max_steps; ++i) {
    if (std::abs(cp_high - cp_low) < 1e-14) break;
    v_mid = v_low + (cp - cp_low) * (v_high - v_low) / (cp_high - cp_low);
    v_mid = std::min(std::max(v_mid, MIN_V), MAX_V);
    double cp_mid;
    value_vega_fixed(c, v_mid, cp_mid, unused);
    if (std::abs(cp - cp_mid) < precision) return v_mid;
    if (cp_mid < cp) {
      v_low = v_mid;
      cp_low = cp_mid;
    } else {
      v_high = v_mid;
      cp_high = cp_mid;
    }
  }
  return v_mid;
}

double euro_implied_vol(OptType type, double fs, double x, double t, double r, double q,
                        double cp) {
  const double b = r - q;
  auto c = make_fixed(type, fs, x, t, r, b);
  constexpr double precision = 1e-5;
  constexpr int max_steps = 100;

  double v = approx_implied_vol(type, fs, x, t, r, b, cp, c.ebrt, c.ert);
  v = std::min(std::max(v, MIN_V), MAX_V);

  double value, vega, d1, d2;
  value_vega_fixed(c, v, value, vega, &d1, &d2);
  double min_diff = std::abs(cp - value);
  int countr = 0;
  // Halley (cubic) when possible; falls back to Newton if vomma term is tiny.
  while (precision <= std::abs(cp - value) && std::abs(cp - value) <= min_diff &&
         countr < max_steps) {
    if (std::abs(vega) < 1e-12) break;
    const double diff = value - cp;
    double step = diff / vega;
    // Halley: denom = vega - ½ · diff · d1·d2 / v
    const double denom = vega - 0.5 * diff * d1 * d2 / v;
    if (std::abs(denom) > 1e-12) step = diff / denom;
    v = v - step;
    if (v > MAX_V || v < MIN_V) break;
    value_vega_fixed(c, v, value, vega, &d1, &d2);
    min_diff = std::min(std::abs(cp - value), min_diff);
    ++countr;
  }
  if (std::abs(cp - value) < precision) return v;
  return bisection_implied_vol(c, cp, precision, max_steps);
}

}  // namespace wbt
