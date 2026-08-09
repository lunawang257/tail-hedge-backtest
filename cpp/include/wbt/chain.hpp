#pragma once
#include "types.hpp"
#include "data.hpp"
#include <unordered_map>
#include <optional>
#include <utility>

namespace wbt {

using IvCache = std::unordered_map<long long, std::vector<std::pair<double, double>>>;

bool is_monthly_expiration(Date exp);
std::vector<Date> available_expirations(const OptionsDataset& ds, Date as_of);
std::vector<Date> monthly_expirations(const OptionsDataset& ds, Date as_of);
std::pair<Date, Date> bracketing_expirations(const OptionsDataset& ds, Date as_of,
                                              int target_dte, bool prefer_monthly);
bool needs_synthetic_tenor(const OptionsDataset& ds, Date as_of, int target_dte, int tol = 1);

std::optional<double> interpolate_iv_for_dte(const OptionsDataset& ds, OptType type,
                                             Date as_of, double spot, double strike,
                                             int target_dte, IvCache* cache);

std::optional<SyntheticQuote> synthetic_option_quote(
    const OptionsDataset& ds, OptType type, Date as_of, double spot, double strike,
    int target_dte, double slip_pct, IvCache* cache);

std::optional<SyntheticQuote> find_option_by_delta(
    const OptionsDataset& ds, OptType type, Date as_of, double spot,
    double target_abs_delta, int target_dte, bool prefer_otm = true,
    double slip_pct = 0.01, bool allow_interpolate = true, IvCache* cache = nullptr);

}  // namespace wbt
