#include "wbt/data.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <cstdint>
#include <cstring>
#include <zlib.h>

namespace wbt {
namespace {

std::vector<std::string> split_csv(const std::string& line) {
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

int col_index(const std::vector<std::string>& cols, const std::string& name) {
  for (size_t i = 0; i < cols.size(); ++i)
    if (cols[i] == name) return static_cast<int>(i);
  return -1;
}

bool ends_with_gz(const std::string& path) {
  return path.size() >= 3 &&
         (path.compare(path.size() - 3, 3, ".gz") == 0 ||
          path.compare(path.size() - 3, 3, ".GZ") == 0);
}

// Line reader for plain or gzip-compressed text files.
class LineReader {
 public:
  explicit LineReader(const std::string& path) : path_(path), gzip_(ends_with_gz(path)) {
    if (gzip_) {
      gz_ = gzopen(path.c_str(), "rb");
      if (!gz_) throw std::runtime_error("cannot open gzip file: " + path);
      gzbuffer(gz_, 1 << 20);
    } else {
      plain_.open(path);
      if (!plain_) throw std::runtime_error("cannot open " + path);
    }
  }
  ~LineReader() {
    if (gz_) gzclose(gz_);
  }
  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;

  bool getline(std::string& out) {
    out.clear();
    if (gzip_) {
      char buf[8192];
      if (!gzgets(gz_, buf, sizeof(buf))) return false;
      out.assign(buf);
      // gzgets may truncate long lines; keep reading until newline or EOF
      while (!out.empty() && out.back() != '\n' && !gzeof(gz_)) {
        if (!gzgets(gz_, buf, sizeof(buf))) break;
        out.append(buf);
      }
      if (!out.empty() && out.back() == '\n') out.pop_back();
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (!std::getline(plain_, out)) return false;
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
  }

 private:
  std::string path_;
  bool gzip_ = false;
  gzFile gz_ = nullptr;
  std::ifstream plain_;
};

}  // namespace

void adjust_prices_for_splits_and_merges(std::vector<OptionQuote>& quotes) {
  if (quotes.empty()) return;
  std::vector<Date> dates;
  for (auto& q : quotes) dates.push_back(q.data_date);
  std::sort(dates.begin(), dates.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());

  std::unordered_map<Date, double> spot;
  for (auto& q : quotes) {
    if (!spot.count(q.data_date)) spot[q.data_date] = q.underlying;
  }

  constexpr double GAP = 1.7;
  double cumulative = 1.0;
  std::unordered_map<Date, double> adj;
  adj[dates.back()] = 1.0;
  for (int i = static_cast<int>(dates.size()) - 2; i >= 0; --i) {
    double older = spot[dates[i]];
    double newer = spot[dates[i + 1]];
    if (older > 0 && newer > 0) {
      double gap = std::max(older, newer) / std::min(older, newer);
      if (gap >= GAP) {
        int ratio = std::max(2, static_cast<int>(std::lround(gap)));
        if (older > newer) cumulative *= (1.0 / ratio);  // split
        else cumulative *= static_cast<double>(ratio);     // merge
      }
    }
    adj[dates[i]] = cumulative;
  }

  for (auto& q : quotes) {
    double f = adj.count(q.data_date) ? adj[q.data_date] : 1.0;
    if (std::abs(f - 1.0) < 1e-15) continue;
    q.underlying *= f;
    q.strike *= f;
    q.bid *= f;
    q.ask *= f;
    q.last *= f;
  }
}

OptionsDataset load_options_csv(const std::string& path, Date start, Date end,
                                int trade_every_n_days, double atm_band) {
  LineReader in(path);
  std::string header;
  if (!in.getline(header)) throw std::runtime_error("empty options file: " + path);
  auto cols = split_csv(header);
  int i_exp = col_index(cols, "ExpirationDate");
  int i_ask = col_index(cols, "AskPrice");
  int i_bid = col_index(cols, "BidPrice");
  int i_last = col_index(cols, "LastPrice");
  int i_pc = col_index(cols, "PutCall");
  int i_k = col_index(cols, "StrikePrice");
  int i_u = col_index(cols, "UnderlyingPrice");
  int i_d = col_index(cols, "DataDate");
  if (i_exp < 0 || i_ask < 0 || i_bid < 0 || i_pc < 0 || i_k < 0 || i_u < 0 || i_d < 0)
    throw std::runtime_error("missing required options columns");

  OptionsDataset ds;
  std::string line;
  while (in.getline(line)) {
    if (line.empty()) continue;
    auto f = split_csv(line);
    auto need = std::max({i_exp, i_ask, i_bid, i_pc, i_k, i_u, i_d});
    if (static_cast<int>(f.size()) <= need) continue;
    OptionQuote q;
    try {
      q.data_date = parse_date(f[i_d]);
      q.exp_date = parse_date(f[i_exp]);
      q.put_call = parse_opt_type(f[i_pc].empty() ? 'c' : f[i_pc][0]);
      q.strike = std::stod(f[i_k]);
      q.ask = std::stod(f[i_ask]);
      q.bid = std::stod(f[i_bid]);
      q.underlying = std::stod(f[i_u]);
      if (i_last >= 0 && i_last < static_cast<int>(f.size()) && !f[i_last].empty())
        q.last = std::stod(f[i_last]);
    } catch (...) { continue; }
    if (start && q.data_date < start) continue;
    if (end && q.data_date > end) continue;
    ds.quotes.push_back(q);
  }

  adjust_prices_for_splits_and_merges(ds.quotes);

  std::sort(ds.quotes.begin(), ds.quotes.end(),
            [](const OptionQuote& a, const OptionQuote& b) {
              if (a.data_date != b.data_date) return a.data_date < b.data_date;
              if (a.exp_date != b.exp_date) return a.exp_date < b.exp_date;
              if (a.put_call != b.put_call) return a.put_call < b.put_call;
              return a.strike < b.strike;
            });

  // Thin sessions
  std::vector<Date> all_dates;
  for (auto& q : ds.quotes) {
    if (all_dates.empty() || all_dates.back() != q.data_date) all_dates.push_back(q.data_date);
  }
  std::unordered_set<Date> keep;
  if (trade_every_n_days <= 1) {
    for (Date d : all_dates) keep.insert(d);
  } else {
    if (!all_dates.empty()) keep.insert(all_dates.front());
    for (size_t i = 0; i < all_dates.size(); ++i) {
      if (i % static_cast<size_t>(trade_every_n_days) == 0) keep.insert(all_dates[i]);
    }
    if (!all_dates.empty()) keep.insert(all_dates.back());
  }
  if (trade_every_n_days > 1) {
    std::vector<OptionQuote> filtered;
    filtered.reserve(ds.quotes.size());
    for (auto& q : ds.quotes) if (keep.count(q.data_date)) filtered.push_back(q);
    ds.quotes.swap(filtered);
  }

  // ATM strike band
  if (atm_band > 0) {
    std::unordered_map<Date, double> spot_map;
    for (auto& q : ds.quotes) {
      if (!spot_map.count(q.data_date)) spot_map[q.data_date] = q.underlying;
    }
    std::vector<OptionQuote> filtered;
    filtered.reserve(ds.quotes.size());
    for (auto& q : ds.quotes) {
      double s = spot_map[q.data_date];
      if (q.strike >= s * (1.0 - atm_band) && q.strike <= s * (1.0 + atm_band))
        filtered.push_back(q);
    }
    ds.quotes.swap(filtered);
  }

  ds.rebuild_index();
  std::cerr << "Loaded options rows=" << ds.quotes.size()
            << " days=" << ds.dates.size() << " from " << path << "\n";
  return ds;
}

void OptionsDataset::rebuild_index() {
  dates.clear();
  day_range.clear();
  for (size_t i = 0; i < quotes.size();) {
    Date d = quotes[i].data_date;
    size_t j = i;
    while (j < quotes.size() && quotes[j].data_date == d) ++j;
    day_range[d] = {i, j};
    dates.push_back(d);
    i = j;
  }
}

void save_dataset_cache(const OptionsDataset& ds, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot write cache " + path);
  const char magic[8] = {'W', 'B', 'T', 'C', 'A', 'C', 'H', '1'};
  out.write(magic, 8);
  uint64_t n = ds.quotes.size();
  out.write(reinterpret_cast<const char*>(&n), sizeof(n));
  out.write(reinterpret_cast<const char*>(ds.quotes.data()),
            static_cast<std::streamsize>(n * sizeof(OptionQuote)));
  std::cerr << "Wrote cache " << path << " rows=" << n << "\n";
}

OptionsDataset load_dataset_cache(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read cache " + path);
  char magic[8];
  in.read(magic, 8);
  if (std::string(magic, magic + 8) != "WBTCACH1")
    throw std::runtime_error("bad cache magic");
  uint64_t n = 0;
  in.read(reinterpret_cast<char*>(&n), sizeof(n));
  OptionsDataset ds;
  ds.quotes.resize(static_cast<size_t>(n));
  in.read(reinterpret_cast<char*>(ds.quotes.data()),
          static_cast<std::streamsize>(n * sizeof(OptionQuote)));
  ds.rebuild_index();
  std::cerr << "Loaded cache " << path << " rows=" << n
            << " days=" << ds.dates.size() << "\n";
  return ds;
}

const OptionQuote* OptionsDataset::begin(Date d) const {
  auto it = day_range.find(d);
  if (it == day_range.end()) return nullptr;
  return quotes.data() + it->second.first;
}
const OptionQuote* OptionsDataset::end(Date d) const {
  auto it = day_range.find(d);
  if (it == day_range.end()) return nullptr;
  return quotes.data() + it->second.second;
}
double OptionsDataset::spot(Date d) const {
  auto b = begin(d);
  if (!b) return 0;
  return b->underlying;
}
std::vector<Date> OptionsDataset::expirations(Date d) const {
  std::vector<Date> exps;
  auto b = begin(d), e = end(d);
  if (!b) return exps;
  for (auto p = b; p != e; ++p) {
    if (std::find(exps.begin(), exps.end(), p->exp_date) == exps.end())
      exps.push_back(p->exp_date);
  }
  std::sort(exps.begin(), exps.end());
  return exps;
}

}  // namespace wbt
