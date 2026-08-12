#include "wbt/chain.hpp"
#include "wbt/gbs.hpp"
#include "wbt/yield.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <algorithm>
#include <limits>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <iterator>

namespace wbt {

bool is_monthly_expiration(Date exp) {
  return exp == third_friday(year_of(exp), month_of(exp));
}

std::vector<Date> available_expirations(const OptionsDataset& ds, Date as_of) {
  auto it = ds.day_exps.find(as_of);
  if (it == ds.day_exps.end()) return {};
  std::vector<Date> out;
  out.reserve(it->second.size());
  for (Date e : it->second) if (e >= as_of) out.push_back(e);
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
  auto it = ds.day_exps.find(as_of);
  if (it == ds.day_exps.end()) return true;
  for (Date e : it->second) {
    if (e < as_of) continue;
    if (std::abs(days_between(as_of, e) - target_dte) <= tol) return false;
  }
  return true;
}

static long long iv_key(OptType t, Date exp) {
  return (static_cast<long long>(exp) << 8) | static_cast<long long>(opt_type_char(t));
}

double IvSmile::ensure_iv(size_t i) const {
  double& slot = iv[i];
  if (std::isfinite(slot) && slot > 0) return slot;
  // Precomputed smiles may omit mid; treat missing mid as unsolvable.
  if (i >= mid.size() || !std::isfinite(mid[i])) {
    slot = std::numeric_limits<double>::quiet_NaN();
    return slot;
  }
  const double disc_k = moneyness[i] * spot * ert;
  double lo, hi;
  if (type == OptType::Put) {
    lo = std::max(disc_k - disc_s, 0.0);
    hi = disc_k;
  } else {
    lo = std::max(disc_s - disc_k, 0.0);
    hi = disc_s;
  }
  if (hi <= lo) {
    slot = std::numeric_limits<double>::quiet_NaN();
    return slot;
  }
  const double eps = std::max(1e-6, hi * 1e-6);
  const double san = std::min(std::max(mid[i], lo + eps), hi - eps);
  const double solved = euro_implied_vol(type, spot, moneyness[i] * spot, t, r, q, san);
  slot = (solved > 0 && std::isfinite(solved))
             ? solved
             : std::numeric_limits<double>::quiet_NaN();
  return slot;
}

double IvSmile::at_moneyness(double m) const {
  if (moneyness.empty()) return std::numeric_limits<double>::quiet_NaN();
  if (m <= moneyness.front()) return ensure_iv(0);
  if (m >= moneyness.back()) return ensure_iv(moneyness.size() - 1);
  auto it = std::lower_bound(moneyness.begin(), moneyness.end(), m);
  size_t hi = static_cast<size_t>(it - moneyness.begin());
  if (hi == 0) return ensure_iv(0);
  size_t lo = hi - 1;
  // Walk outward if a bracket IV fails to solve.
  while (lo > 0 && !std::isfinite(ensure_iv(lo))) --lo;
  while (hi + 1 < moneyness.size() && !std::isfinite(ensure_iv(hi))) ++hi;
  double iv0 = ensure_iv(lo), iv1 = ensure_iv(hi);
  if (!std::isfinite(iv0)) return iv1;
  if (!std::isfinite(iv1)) return iv0;
  double m0 = moneyness[lo], m1 = moneyness[hi];
  if (m1 == m0) return iv0;
  double w = (m - m0) / (m1 - m0);
  return iv0 * (1 - w) + iv1 * w;
}

static IvSmile build_iv_smile(const OptionsDataset& ds, OptType type, Date exp,
                              double spot, Date as_of, double band = 0.45) {
  IvSmile smile;
  smile.type = type;
  smile.spot = spot;
  int days = std::max(days_between(as_of, exp), 1);
  smile.t = days / YEAR_FRAC;
  smile.r = get_risk_free_rate(as_of, smile.t);
  smile.q = get_dividend_yield(as_of);
  smile.disc_s = spot * std::exp(-smile.q * smile.t);
  smile.ert = std::exp(-smile.r * smile.t);
  double lo_k = spot * (1.0 - band), hi_k = spot * (1.0 + band);
  auto [b, e] = ds.quotes_for(as_of, exp, type);
  if (!b) return smile;
  const size_t n = static_cast<size_t>(e - b);
  smile.moneyness.reserve(n);
  smile.mid.reserve(n);
  const double inv_spot = 1.0 / spot;
  for (auto p = b; p != e; ++p) {
    if (p->strike < lo_k || p->strike > hi_k) continue;
    double m = p->mid();
    if (m <= 0 || !std::isfinite(m)) continue;
    smile.moneyness.push_back(p->strike * inv_spot);
    smile.mid.push_back(m);
  }
  smile.iv.assign(smile.moneyness.size(),
                  std::numeric_limits<double>::quiet_NaN());
  return smile;
}

// Keep only successfully solved (moneyness, iv) points — matches old eager smile.
static IvSmile compact_solved_smile(IvSmile smile) {
  IvSmile out;
  out.type = smile.type;
  out.spot = smile.spot;
  out.t = smile.t;
  out.r = smile.r;
  out.q = smile.q;
  out.disc_s = smile.disc_s;
  out.ert = smile.ert;
  out.moneyness.reserve(smile.moneyness.size());
  out.iv.reserve(smile.moneyness.size());
  for (size_t i = 0; i < smile.moneyness.size(); ++i) {
    double v = smile.ensure_iv(i);
    if (std::isfinite(v) && v > 0) {
      out.moneyness.push_back(smile.moneyness[i]);
      out.iv.push_back(v);
    }
  }
  return out;
}

namespace {
std::shared_ptr<const IvSmileStore> g_iv_smile_store;
}  // namespace

void set_iv_smile_store(std::shared_ptr<const IvSmileStore> store) {
  g_iv_smile_store = std::move(store);
}

const IvSmileStore* get_iv_smile_store() {
  return g_iv_smile_store.get();
}

void clear_iv_smile_store() {
  g_iv_smile_store.reset();
}

const IvSmile* IvSmileStore::find(Date as_of, OptType type, Date exp) const {
  IvSmileKey k{as_of, exp, opt_type_char(type)};
  auto it = smiles.find(k);
  return it == smiles.end() ? nullptr : &it->second;
}

IvSmileStore build_iv_smile_store(const OptionsDataset& ds, double band) {
  IvSmileStore store;
  for (Date as_of : ds.dates) {
    double spot = ds.spot(as_of);
    if (!(spot > 0) || !std::isfinite(spot)) continue;
    auto exps = available_expirations(ds, as_of);
    for (Date exp : exps) {
      for (OptType type : {OptType::Call, OptType::Put}) {
        auto smile = compact_solved_smile(
            build_iv_smile(ds, type, exp, spot, as_of, band));
        if (smile.moneyness.empty()) continue;
        IvSmileKey key{as_of, exp, opt_type_char(type)};
        store.smiles.emplace(key, std::move(smile));
      }
    }
  }
  return store;
}

void save_iv_smile_csv(const IvSmileStore& store, const std::string& path) {
  namespace fs = std::filesystem;
  if (auto parent = fs::path(path).parent_path(); !parent.empty())
    fs::create_directories(parent);
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write iv smile csv " + path);
  out << "as_of,exp,type,spot,moneyness,iv\n";
  out << std::fixed << std::setprecision(10);
  // Stable order for diffs / reproducibility.
  std::vector<const std::pair<const IvSmileKey, IvSmile>*> rows;
  rows.reserve(store.smiles.size());
  for (auto& kv : store.smiles) rows.push_back(&kv);
  std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
    if (a->first.as_of != b->first.as_of) return a->first.as_of < b->first.as_of;
    if (a->first.exp != b->first.exp) return a->first.exp < b->first.exp;
    return a->first.type < b->first.type;
  });
  size_t n_pts = 0;
  for (auto* kv : rows) {
    const auto& key = kv->first;
    const auto& smile = kv->second;
    for (size_t i = 0; i < smile.moneyness.size(); ++i) {
      out << format_date(key.as_of) << ','
          << format_date(key.exp) << ','
          << key.type << ','
          << smile.spot << ','
          << smile.moneyness[i] << ','
          << smile.iv[i] << '\n';
      ++n_pts;
    }
  }
  std::cerr << "Wrote IV smile CSV " << path
            << " smiles=" << store.smiles.size()
            << " points=" << n_pts << "\n";
}

namespace {

inline bool parse_ymd10(const char* b, Date& out) {
  if (b[4] != '-' || b[7] != '-') return false;
  auto dig = [](char c) -> int { return (c >= '0' && c <= '9') ? (c - '0') : -1; };
  int y0 = dig(b[0]), y1 = dig(b[1]), y2 = dig(b[2]), y3 = dig(b[3]);
  int m0 = dig(b[5]), m1 = dig(b[6]);
  int d0 = dig(b[8]), d1 = dig(b[9]);
  if ((y0 | y1 | y2 | y3 | m0 | m1 | d0 | d1) < 0) return false;
  out = ymd(y0 * 1000 + y1 * 100 + y2 * 10 + y3, m0 * 10 + m1, d0 * 10 + d1);
  return true;
}

inline bool parse_dec(const char* b, const char* e, double& out) {
  if (b >= e) return false;
  bool neg = false;
  if (*b == '-') { neg = true; ++b; if (b >= e) return false; }
  uint64_t ip = 0;
  const char* p = b;
  while (p < e && *p >= '0' && *p <= '9') {
    ip = ip * 10u + static_cast<uint64_t>(*p - '0');
    ++p;
  }
  double val = static_cast<double>(ip);
  if (p < e && *p == '.') {
    ++p;
    uint64_t fp = 0;
    int n = 0;
    while (p < e && *p >= '0' && *p <= '9') {
      fp = fp * 10u + static_cast<uint64_t>(*p - '0');
      ++n;
      ++p;
    }
    if (n > 0) {
      static const double scale[] = {
          1.0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12};
      val += n < 13 ? static_cast<double>(fp) / scale[n]
                    : static_cast<double>(fp) / std::pow(10.0, n);
    }
  }
  if (p != e) return false;
  out = neg ? -val : val;
  return true;
}

inline const char* find_comma(const char* p, const char* end) {
  while (p < end && *p != ',') ++p;
  return p;
}

std::string iv_smile_bin_path(const std::string& csv_path) {
  // QQQ-ivsmile.csv → QQQ-ivsmile.bin
  if (csv_path.size() >= 4 &&
      (csv_path.compare(csv_path.size() - 4, 4, ".csv") == 0 ||
       csv_path.compare(csv_path.size() - 4, 4, ".CSV") == 0)) {
    return csv_path.substr(0, csv_path.size() - 4) + ".bin";
  }
  return csv_path + ".bin";
}

void save_iv_smile_bin(const IvSmileStore& store, const std::string& path) {
  namespace fs = std::filesystem;
  if (auto parent = fs::path(path).parent_path(); !parent.empty())
    fs::create_directories(parent);
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot write iv smile bin " + path);
  const char magic[8] = {'W', 'B', 'T', 'I', 'V', 'S', 'M', '1'};
  out.write(magic, 8);
  uint64_t n = store.smiles.size();
  out.write(reinterpret_cast<const char*>(&n), sizeof(n));
  size_t n_pts = 0;
  for (const auto& kv : store.smiles) {
    const auto& key = kv.first;
    const auto& smile = kv.second;
    int32_t as_of = key.as_of, exp = key.exp;
    char type = key.type;
    double spot = smile.spot;
    uint32_t npt = static_cast<uint32_t>(smile.moneyness.size());
    out.write(reinterpret_cast<const char*>(&as_of), sizeof(as_of));
    out.write(reinterpret_cast<const char*>(&exp), sizeof(exp));
    out.write(&type, 1);
    out.write(reinterpret_cast<const char*>(&spot), sizeof(spot));
    out.write(reinterpret_cast<const char*>(&npt), sizeof(npt));
    if (npt) {
      out.write(reinterpret_cast<const char*>(smile.moneyness.data()),
                static_cast<std::streamsize>(npt * sizeof(double)));
      out.write(reinterpret_cast<const char*>(smile.iv.data()),
                static_cast<std::streamsize>(npt * sizeof(double)));
      n_pts += npt;
    }
  }
  std::cerr << "Wrote IV smile bin " << path
            << " smiles=" << n << " points=" << n_pts << "\n";
}

IvSmileStore load_iv_smile_bin(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read iv smile bin " + path);
  char magic[8];
  in.read(magic, 8);
  if (std::string(magic, magic + 8) != "WBTIVSM1")
    throw std::runtime_error("bad iv smile bin magic");
  uint64_t n = 0;
  in.read(reinterpret_cast<char*>(&n), sizeof(n));
  IvSmileStore store;
  store.smiles.reserve(static_cast<size_t>(n) * 2);
  size_t n_pts = 0;
  for (uint64_t i = 0; i < n; ++i) {
    int32_t as_of = 0, exp = 0;
    char type = 'c';
    double spot = 0;
    uint32_t npt = 0;
    in.read(reinterpret_cast<char*>(&as_of), sizeof(as_of));
    in.read(reinterpret_cast<char*>(&exp), sizeof(exp));
    in.read(&type, 1);
    in.read(reinterpret_cast<char*>(&spot), sizeof(spot));
    in.read(reinterpret_cast<char*>(&npt), sizeof(npt));
    IvSmileKey key{as_of, exp, type};
    IvSmile smile;
    smile.type = parse_opt_type(type);
    smile.spot = spot;
    smile.moneyness.resize(npt);
    smile.iv.resize(npt);
    if (npt) {
      in.read(reinterpret_cast<char*>(smile.moneyness.data()),
              static_cast<std::streamsize>(npt * sizeof(double)));
      in.read(reinterpret_cast<char*>(smile.iv.data()),
              static_cast<std::streamsize>(npt * sizeof(double)));
      n_pts += npt;
    }
    store.smiles.emplace(key, std::move(smile));
  }
  std::cerr << "Loaded IV smile bin " << path
            << " smiles=" << store.smiles.size()
            << " points=" << n_pts << "\n";
  return store;
}

}  // namespace

IvSmileStore load_iv_smile_csv(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read iv smile csv " + path);
  std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (buf.empty()) throw std::runtime_error("empty iv smile csv " + path);

  const char* p = buf.data();
  const char* end = p + buf.size();
  // skip header
  while (p < end && *p != '\n') ++p;
  if (p < end) ++p;

  IvSmileStore store;
  IvSmileKey cur{};
  IvSmile* smile = nullptr;
  size_t n_pts = 0;
  while (p < end) {
    const char* lb = p;
    while (p < end && *p != '\n') ++p;
    const char* le = p;
    if (p < end) ++p;
    if (le > lb && le[-1] == '\r') --le;
    if (lb == le) continue;

    // as_of,exp,type,spot,moneyness,iv
    const char* c0 = find_comma(lb, le);
    if (c0 >= le || c0 - lb < 10) continue;
    const char* c1 = find_comma(c0 + 1, le);
    if (c1 >= le || c1 - (c0 + 1) < 10) continue;
    const char* c2 = find_comma(c1 + 1, le);
    if (c2 >= le) continue;
    const char* c3 = find_comma(c2 + 1, le);
    if (c3 >= le) continue;
    const char* c4 = find_comma(c3 + 1, le);
    if (c4 >= le) continue;

    Date as_of = 0, exp = 0;
    if (!parse_ymd10(lb, as_of) || !parse_ymd10(c0 + 1, exp)) continue;
    char type = static_cast<char>(std::tolower(static_cast<unsigned char>(*(c1 + 1))));
    double spot = 0, m = 0, iv = 0;
    if (!parse_dec(c2 + 1, c3, spot)) continue;
    if (!parse_dec(c3 + 1, c4, m)) continue;
    if (!parse_dec(c4 + 1, le, iv)) continue;
    if (!(iv > 0) || !std::isfinite(iv) || !std::isfinite(m)) continue;

    IvSmileKey key{as_of, exp, type};
    if (!smile || key.as_of != cur.as_of || key.exp != cur.exp || key.type != cur.type) {
      cur = key;
      auto [it, inserted] = store.smiles.try_emplace(key);
      smile = &it->second;
      if (inserted) {
        smile->type = parse_opt_type(type);
        smile->spot = spot;
        smile->moneyness.reserve(64);
        smile->iv.reserve(64);
      }
    }
    smile->moneyness.push_back(m);
    smile->iv.push_back(iv);
    ++n_pts;
  }
  std::cerr << "Loaded IV smile CSV " << path
            << " smiles=" << store.smiles.size()
            << " points=" << n_pts << "\n";
  return store;
}

std::shared_ptr<const IvSmileStore> ensure_iv_smile_csv(const OptionsDataset& ds,
                                                        const std::string& path,
                                                        double band) {
  namespace fs = std::filesystem;
  if (path.empty()) {
    clear_iv_smile_store();
    return nullptr;
  }
  const std::string bin_path = iv_smile_bin_path(path);
  std::shared_ptr<const IvSmileStore> store;

  // Prefer binary sidecar (much faster than 100MB+ CSV).
  if (fs::exists(bin_path)) {
    try {
      store = std::make_shared<const IvSmileStore>(load_iv_smile_bin(bin_path));
      set_iv_smile_store(store);
      return store;
    } catch (const std::exception& e) {
      std::cerr << "IV smile bin load failed (" << e.what() << "), trying CSV...\n";
    }
  }

  if (fs::exists(path)) {
    try {
      auto loaded = load_iv_smile_csv(path);
      try { save_iv_smile_bin(loaded, bin_path); } catch (...) {}
      store = std::make_shared<const IvSmileStore>(std::move(loaded));
      set_iv_smile_store(store);
      return store;
    } catch (const std::exception& e) {
      std::cerr << "IV smile load failed (" << e.what() << "), rebuilding...\n";
    }
  }
  auto t0 = std::chrono::steady_clock::now();
  auto built = build_iv_smile_store(ds, band);
  auto t1 = std::chrono::steady_clock::now();
  std::cerr << "Built IV smile store in "
            << std::chrono::duration<double>(t1 - t0).count() << "s\n";
  save_iv_smile_csv(built, path);
  try { save_iv_smile_bin(built, bin_path); } catch (...) {}
  store = std::make_shared<const IvSmileStore>(std::move(built));
  set_iv_smile_store(store);
  return store;
}

std::string default_iv_smile_path(const std::string& options_csv_path,
                                  const std::string& symbol) {
  namespace fs = std::filesystem;
  fs::path in(options_csv_path);
  fs::path dir = in.parent_path();
  if (dir.empty()) dir = ".";

  std::string base = symbol;
  if (base.empty()) {
    std::string name = in.filename().string();
    auto ends_with = [](const std::string& s, const char* suf) {
      const size_t n = std::strlen(suf);
      return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    if (ends_with(name, ".gz")) name.resize(name.size() - 3);
    if (ends_with(name, ".csv") || ends_with(name, ".CSV")) name.resize(name.size() - 4);
    base = name.empty() ? "options" : name;
  }
  return (dir / (base + "-ivsmile.csv")).string();
}

static double anchor_iv(const OptionsDataset& ds, OptType type, Date exp,
                        double spot, Date as_of) {
  int days = std::max(days_between(as_of, exp), 1);
  double t = days / YEAR_FRAC;
  double r = get_risk_free_rate(as_of, t);
  double q = get_dividend_yield(as_of);
  auto [b, e] = ds.quotes_for(as_of, exp, type);
  if (!b) return 0.20;

  std::vector<std::pair<double, const OptionQuote*>> ranked;
  ranked.reserve(static_cast<size_t>(e - b));
  for (auto p = b; p != e; ++p)
    ranked.push_back({std::abs(p->strike - spot), p});
  size_t lim = std::min(ranked.size(), size_t(16));
  std::partial_sort(
      ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(lim), ranked.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < lim; ++i) {
    auto* row = ranked[i].second;
    double mid = row->mid();
    if (mid <= 0) continue;
    auto san = sanitize_option_price_for_iv(type, spot, row->strike, t, r, q, mid);
    if (!san) continue;
    double iv = euro_implied_vol(type, spot, row->strike, t, r, q, *san);
    if (iv > 0 && std::isfinite(iv)) return iv;
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

  IvCache local_iv;
  IvCache* use_cache = cache ? cache : &local_iv;
  const IvSmileStore* smile_store = get_iv_smile_store();
  auto get_smile = [&](Date exp) -> const IvSmile& {
    long long k = iv_key(type, exp);
    auto it = use_cache->find(k);
    if (it != use_cache->end()) return it->second;
    if (smile_store) {
      if (const IvSmile* pre = smile_store->find(as_of, type, exp)) {
        return *pre;
      }
    }
    it = use_cache->emplace(k, build_iv_smile(ds, type, exp, spot, as_of)).first;
    return it->second;
  };

  double moneyness = strike / spot;
  double t_target = std::max(target_dte, 1) / YEAR_FRAC;
  double iv_lo = get_smile(lo).at_moneyness(moneyness);
  if (!std::isfinite(iv_lo)) return std::nullopt;

  if (!hi || hi == lo) {
    double t_lo = std::max(days_between(as_of, lo), 1) / YEAR_FRAC;
    double var = iv_lo * iv_lo * t_lo;
    double iv = std::sqrt(std::max(var / t_target, 1e-12));
    if (t_target < t_lo) iv = std::max(iv_lo * 0.85, std::min(iv, iv_lo * 1.35));
    return iv;
  }

  double iv_hi = get_smile(hi).at_moneyness(moneyness);
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

  auto [b, e] = ds.quotes_for(as_of, target_exp, type);
  if (b) {
    auto it = std::lower_bound(
        b, e, strike, [](const OptionQuote& row, double k) { return row.strike < k; });
    if (it != e && std::abs(it->strike - strike) < 1e-6) {
      double mid = it->mid();
      SyntheticQuote qte;
      qte.bid = it->bid; qte.ask = it->ask; qte.mid = mid;
      qte.strike = strike; qte.exp_date = target_exp; qte.data_date = as_of;
      qte.underlying = spot; qte.put_call = type; qte.interpolated = false;
      auto san = sanitize_option_price_for_iv(type, spot, strike, t, r, q, mid);
      double iv = san ? euro_implied_vol(type, spot, strike, t, r, q, *san) : 0.2;
      auto g = merton(type, spot, strike, t, r, q, iv);
      qte.implied_vol = iv; qte.delta = g.delta; qte.gamma = g.gamma; qte.theta = g.theta;
      return qte;
    }
  }

  auto iv = interpolate_iv_for_dte(ds, type, as_of, spot, strike, target_dte, cache);
  if (!iv) return std::nullopt;
  auto g = merton(type, spot, strike, t, r, q, *iv);
  if (!std::isfinite(g.value)) return std::nullopt;
  SyntheticQuote qte;
  qte.mid = g.value;
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
  auto [b, e] = ds.quotes_for(as_of, nearest, type);
  if (!b) return std::nullopt;
  ranked.reserve(static_cast<size_t>(e - b));

  auto consider = [&](const OptionQuote* p, bool enforce_otm) {
    if (p->strike < lo_k || p->strike > hi_k) return;
    double mid = p->mid();
    if (mid <= 0) return;
    if (enforce_otm) {
      if (type == OptType::Call && p->strike < spot * 0.97) return;
      if (type == OptType::Put && p->strike > spot * 1.03) return;
    }
    double delta = merton_delta(type, spot, p->strike, t, r, q, anchor);
    if (!std::isfinite(delta)) return;
    ranked.push_back({std::abs(delta - target_delta), p->strike, mid});
  };

  for (auto p = b; p != e; ++p) consider(p, prefer_otm);
  if (ranked.empty()) {
    for (auto p = b; p != e; ++p) {
      double mid = p->mid();
      if (mid <= 0) continue;
      double delta = merton_delta(type, spot, p->strike, t, r, q, anchor);
      ranked.push_back({std::abs(delta - target_delta), p->strike, mid});
    }
  }
  if (ranked.empty()) return std::nullopt;

  size_t n = std::min(ranked.size(), size_t(8));
  std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(n),
                    ranked.end(),
                    [](const Cand& a, const Cand& b) { return a.err < b.err; });

  IvCache local;
  IvCache* use_cache = cache ? cache : &local;
  std::optional<SyntheticQuote> best;
  double best_err = 1e99;
  for (size_t i = 0; i < n; ++i) {
    std::optional<SyntheticQuote> quote;
    if (need_interp) {
      quote = synthetic_option_quote(ds, type, as_of, spot, ranked[i].k, use_dte,
                                     slip_pct, use_cache);
    } else {
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
    }
    if (!quote) continue;
    double err = std::abs(quote->delta - target_delta);
    if (err < best_err) { best_err = err; best = quote; }
  }
  return best;
}

}  // namespace wbt
