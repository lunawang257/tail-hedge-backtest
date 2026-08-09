#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <cmath>

namespace wbt {

void load_yield_data(const std::string& path);
double get_risk_free_rate(Date date, double time_to_expire_years = -1.0);
double get_dividend_yield(Date date);

std::optional<double> sanitize_option_price_for_iv(
    OptType type, double stock, double strike, double t, double r, double q, double price);

}  // namespace wbt
