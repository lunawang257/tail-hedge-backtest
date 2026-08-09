#pragma once
#include "types.hpp"

namespace wbt {

// Generalized Black–Scholes / Merton (matches optlib/gbs.py)
Greeks gbs(OptType type, double fs, double x, double t, double r, double b, double v);
Greeks merton(OptType type, double fs, double x, double t, double r, double q, double v);
double euro_implied_vol(OptType type, double fs, double x, double t, double r, double q, double cp);

double norm_cdf(double x);
double norm_pdf(double x);

}  // namespace wbt
