#include "wbt/gbs.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace wbt {

namespace {
constexpr double MIN_T = 1.0 / 1000.0;
constexpr double MIN_V = 0.005;
constexpr double MAX_V = 2.0;
}

double norm_pdf(double x) {
  static constexpr double inv_sqrt_2pi = 0.3989422804014327;
  return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

double norm_cdf(double x) {
  // Abramowitz & Stegun 26.2.17 approximation (matches scipy.norm.cdf well)
  // Use std::erfc for accuracy closer to scipy
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

Greeks gbs(OptType type, double fs, double x, double t, double r, double b, double v) {
  if (t < MIN_T) t = MIN_T;
  if (v < MIN_V) v = MIN_V;
  if (v > MAX_V) v = MAX_V;

  const double t_sqrt = std::sqrt(t);
  const double d1 = (std::log(fs / x) + (b + 0.5 * v * v) * t) / (v * t_sqrt);
  const double d2 = d1 - v * t_sqrt;
  Greeks g;

  if (type == OptType::Call) {
    g.value = fs * std::exp((b - r) * t) * norm_cdf(d1) - x * std::exp(-r * t) * norm_cdf(d2);
    g.delta = std::exp((b - r) * t) * norm_cdf(d1);
    g.gamma = std::exp((b - r) * t) * norm_pdf(d1) / (fs * v * t_sqrt);
    g.theta = -(fs * v * std::exp((b - r) * t) * norm_pdf(d1)) / (2.0 * t_sqrt)
              - (b - r) * fs * std::exp((b - r) * t) * norm_cdf(d1)
              - r * x * std::exp(-r * t) * norm_cdf(d2);
    g.vega = std::exp((b - r) * t) * fs * t_sqrt * norm_pdf(d1);
    g.rho = x * t * std::exp(-r * t) * norm_cdf(d2);
  } else {
    g.value = x * std::exp(-r * t) * norm_cdf(-d2) - fs * std::exp((b - r) * t) * norm_cdf(-d1);
    g.delta = -std::exp((b - r) * t) * norm_cdf(-d1);
    g.gamma = std::exp((b - r) * t) * norm_pdf(d1) / (fs * v * t_sqrt);
    g.theta = -(fs * v * std::exp((b - r) * t) * norm_pdf(d1)) / (2.0 * t_sqrt)
              + (b - r) * fs * std::exp((b - r) * t) * norm_cdf(-d1)
              + r * x * std::exp(-r * t) * norm_cdf(-d2);
    g.vega = std::exp((b - r) * t) * fs * t_sqrt * norm_pdf(d1);
    g.rho = -x * t * std::exp(-r * t) * norm_cdf(-d2);
  }
  return g;
}

Greeks merton(OptType type, double fs, double x, double t, double r, double q, double v) {
  return gbs(type, fs, x, t, r, r - q, v);
}

static double approx_implied_vol(OptType type, double fs, double x, double t, double r, double b, double cp) {
  const double ebrt = std::exp((b - r) * t);
  const double ert = std::exp(-r * t);
  const double a = std::sqrt(2.0 * M_PI) / (fs * ebrt + x * ert);
  const double payoff = (type == OptType::Call) ? (fs * ebrt - x * ert) : (x * ert - fs * ebrt);
  const double bb = cp - payoff / 2.0;
  const double c = (payoff * payoff) / M_PI;
  return (a * (bb + std::sqrt(bb * bb + c))) / std::sqrt(t);
}

static double bisection_implied_vol(OptType type, double fs, double x, double t, double r, double b,
                                    double cp, double precision, int max_steps) {
  double v_mid = approx_implied_vol(type, fs, x, t, r, b, cp);
  double v_low, v_high;
  if (v_mid <= MIN_V || v_mid >= MAX_V) {
    v_low = MIN_V;
    v_high = MAX_V;
    v_mid = 0.5 * (v_low + v_high);
  } else {
    v_low = std::max(MIN_V, v_mid * 0.5);
    v_high = std::min(MAX_V, v_mid * 1.5);
  }

  double cp_low = gbs(type, fs, x, t, r, b, v_low).value;
  double cp_high = gbs(type, fs, x, t, r, b, v_high).value;

  if (cp <= cp_low) return v_low;
  if (cp >= cp_high) return v_high;

  for (int i = 0; i < max_steps; ++i) {
    // Linear interpolation step (matches optlib bisection fallback style)
    if (std::abs(cp_high - cp_low) < 1e-14) break;
    v_mid = v_low + (cp - cp_low) * (v_high - v_low) / (cp_high - cp_low);
    v_mid = std::min(std::max(v_mid, MIN_V), MAX_V);
    double cp_mid = gbs(type, fs, x, t, r, b, v_mid).value;
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

double euro_implied_vol(OptType type, double fs, double x, double t, double r, double q, double cp) {
  const double b = r - q;
  if (t < MIN_T) t = MIN_T;
  constexpr double precision = 1e-5;
  constexpr int max_steps = 100;

  double v = approx_implied_vol(type, fs, x, t, r, b, cp);
  v = std::min(std::max(v, MIN_V), MAX_V);

  auto g = gbs(type, fs, x, t, r, b, v);
  double min_diff = std::abs(cp - g.value);
  int countr = 0;
  while (precision <= std::abs(cp - g.value) && std::abs(cp - g.value) <= min_diff && countr < max_steps) {
    if (std::abs(g.vega) < 1e-12) break;
    v = v - (g.value - cp) / g.vega;
    if (v > MAX_V || v < MIN_V) break;
    g = gbs(type, fs, x, t, r, b, v);
    min_diff = std::min(std::abs(cp - g.value), min_diff);
    ++countr;
  }
  if (std::abs(cp - g.value) < precision) return v;
  return bisection_implied_vol(type, fs, x, t, r, b, cp, precision, max_steps);
}

}  // namespace wbt
