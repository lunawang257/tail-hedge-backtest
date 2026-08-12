#include "wbt/yield.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace wbt {
namespace {

struct YieldRow {
  Date date = 0;
  std::vector<double> rates;  // parallel to tenors_
  double div_yield = 0;
  bool has_div = false;
};

std::vector<std::pair<std::string, double>> tenors_;
std::vector<YieldRow> rows_;  // sorted by date ascending
// Per-thread: w_search workers call get_* concurrently; a shared
// unordered_map rehash races and can spin forever (seen at ~8063/8064).
thread_local std::unordered_map<long long, double> rf_cache_;
thread_local std::unordered_map<int, double> div_cache_;

long long rf_key(Date d, double t) {
  return (static_cast<long long>(d) << 20) ^ static_cast<long long>(std::llround(t * 1e8));
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_q = false;
  for (char c : line) {
    if (c == '"') { in_q = !in_q; continue; }
    if (c == ',' && !in_q) { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

}  // namespace

void load_yield_data(const std::string& path) {
  rows_.clear();
  tenors_.clear();
  rf_cache_.clear();
  div_cache_.clear();

  static const std::pair<const char*, double> kAll[] = {
    {"1 Mo", 1.0/12}, {"2 Mo", 2.0/12}, {"3 Mo", 0.25}, {"4 Mo", 4.0/12},
    {"6 Mo", 0.5}, {"1 Yr", 1}, {"2 Yr", 2}, {"3 Yr", 3}, {"5 Yr", 5},
    {"7 Yr", 7}, {"10 Yr", 10}, {"20 Yr", 20}, {"30 Yr", 30},
  };

  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open yield file: " + path);
  std::string header;
  std::getline(in, header);
  if (!header.empty() && header.back() == '\r') header.pop_back();
  auto cols = split_csv_line(header);
  std::vector<int> tenor_idx;
  int date_i = -1, div_i = -1;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i] == "Date") date_i = static_cast<int>(i);
    if (cols[i] == "Div Yield") div_i = static_cast<int>(i);
    for (auto& [name, yrs] : kAll) {
      if (cols[i] == name) {
        tenors_.push_back({name, yrs});
        tenor_idx.push_back(static_cast<int>(i));
      }
    }
  }
  if (date_i < 0 || tenors_.empty()) throw std::runtime_error("bad yield header");

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto f = split_csv_line(line);
    if (static_cast<int>(f.size()) <= date_i) continue;
    YieldRow row;
    try { row.date = parse_date(f[date_i]); } catch (...) { continue; }
    row.rates.resize(tenors_.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < tenor_idx.size(); ++i) {
      int ci = tenor_idx[i];
      if (ci < static_cast<int>(f.size()) && !f[ci].empty()) {
        try { row.rates[i] = std::stod(f[ci]) / 100.0; } catch (...) {}
      }
    }
    if (div_i >= 0 && div_i < static_cast<int>(f.size()) && !f[div_i].empty()) {
      try {
        row.div_yield = std::stod(f[div_i]) / 100.0;
        row.has_div = true;
      } catch (...) {}
    }
    rows_.push_back(row);
  }
  std::sort(rows_.begin(), rows_.end(),
            [](const YieldRow& a, const YieldRow& b) { return a.date < b.date; });
  std::cerr << "Loaded " << rows_.size() << " yield curve records from " << path << "\n";
}

double get_risk_free_rate(Date date, double time_to_expire_years) {
  if (rows_.empty()) throw std::runtime_error("Yield data not loaded");
  double target = time_to_expire_years < 0 ? 0.0 : std::max(time_to_expire_years, 0.0);
  auto key = rf_key(date, target);
  auto it = rf_cache_.find(key);
  if (it != rf_cache_.end()) return it->second;

  // nearest prior date
  auto lb = std::upper_bound(rows_.begin(), rows_.end(), date,
    [](Date d, const YieldRow& r) { return d < r.date; });
  const YieldRow* row = nullptr;
  if (lb == rows_.begin()) row = &rows_.front();
  else row = &*(lb - 1);

  // sort tenors by (abs(yrs-target), yrs < target)
  size_t best = 0;
  double best_score = 1e99;
  for (size_t i = 0; i < tenors_.size(); ++i) {
    if (!std::isfinite(row->rates[i])) continue;
    double yrs = tenors_[i].second;
    double score = std::abs(yrs - target) + (yrs < target ? 1e-6 : 0.0);
    // Prefer closer; tie-break prefer yrs >= target (smaller secondary key when yrs>=target)
    double primary = std::abs(yrs - target);
    double secondary = (yrs < target) ? 1.0 : 0.0;
    score = primary * 1000 + secondary;
    if (score < best_score) { best_score = score; best = i; }
  }
  double rate = row->rates[best];
  if (!std::isfinite(rate)) rate = 0.0;
  rf_cache_[key] = rate;
  return rate;
}

double get_dividend_yield(Date date) {
  auto it = div_cache_.find(date);
  if (it != div_cache_.end()) return it->second;
  if (rows_.empty()) return 0.0;
  auto lb = std::upper_bound(rows_.begin(), rows_.end(), date,
    [](Date d, const YieldRow& r) { return d < r.date; });
  // walk back for has_div
  auto i = (lb == rows_.begin()) ? rows_.begin() : (lb - 1);
  while (true) {
    if (i->has_div) {
      div_cache_[date] = i->div_yield;
      return i->div_yield;
    }
    if (i == rows_.begin()) break;
    --i;
  }
  div_cache_[date] = 0.0;
  return 0.0;
}

std::optional<double> sanitize_option_price_for_iv(
    OptType type, double stock, double strike, double t, double r, double q, double price) {
  double disc_s = stock * std::exp(-q * t);
  double disc_k = strike * std::exp(-r * t);
  double lo, hi;
  if (type == OptType::Put) {
    lo = std::max(disc_k - disc_s, 0.0);
    hi = disc_k;
  } else {
    lo = std::max(disc_s - disc_k, 0.0);
    hi = disc_s;
  }
  if (hi <= lo) return std::nullopt;
  double eps = std::max(1e-6, hi * 1e-6);
  return std::min(std::max(price, lo + eps), hi - eps);
}

}  // namespace wbt
