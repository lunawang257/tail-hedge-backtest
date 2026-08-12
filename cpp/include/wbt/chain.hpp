#pragma once
#include "types.hpp"
#include "data.hpp"
#include <unordered_map>
#include <optional>
#include <utility>
#include <vector>
#include <cmath>
#include <string>
#include <memory>

namespace wbt {

// Lazy IV smile: mid prices stored up front; IVs solved on demand.
// When loaded from an IV-smile CSV, `iv` is fully populated and `mid` may be empty.
struct IvSmile {
  OptType type = OptType::Call;
  double spot = 0, t = 0, r = 0, q = 0, disc_s = 0, ert = 0;
  std::vector<double> moneyness;  // ascending
  std::vector<double> mid;
  mutable std::vector<double> iv;  // NaN until solved

  double ensure_iv(size_t i) const;
  double at_moneyness(double m) const;
};

using IvCache = std::unordered_map<long long, IvSmile>;

struct IvSmileKey {
  Date as_of = 0;
  Date exp = 0;
  char type = 'c';
  bool operator==(const IvSmileKey& o) const {
    return as_of == o.as_of && exp == o.exp && type == o.type;
  }
};

struct IvSmileKeyHash {
  size_t operator()(const IvSmileKey& k) const noexcept {
    size_t h = static_cast<size_t>(k.as_of);
    h ^= static_cast<size_t>(k.exp) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<size_t>(k.type) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

// Precomputed smiles keyed by (as_of, expiry, put/call). Thread-safe for concurrent reads.
struct IvSmileStore {
  std::unordered_map<IvSmileKey, IvSmile, IvSmileKeyHash> smiles;

  const IvSmile* find(Date as_of, OptType type, Date exp) const;
  size_t size() const { return smiles.size(); }
};

IvSmileStore build_iv_smile_store(const OptionsDataset& ds, double band = 0.45);
void save_iv_smile_csv(const IvSmileStore& store, const std::string& path);
IvSmileStore load_iv_smile_csv(const std::string& path);

// If path exists, load; otherwise build from `ds`, write CSV, and install the store.
// Installs a process-wide read-only smile store used by IV interpolation.
std::shared_ptr<const IvSmileStore> ensure_iv_smile_csv(const OptionsDataset& ds,
                                                        const std::string& path,
                                                        double band = 0.45);
void set_iv_smile_store(std::shared_ptr<const IvSmileStore> store);
const IvSmileStore* get_iv_smile_store();
void clear_iv_smile_store();

// Default: <dir(options_csv)>/<SYMBOL>-ivsmile.csv
// If symbol is empty, uses the options filename stem (strips .csv / .csv.gz).
std::string default_iv_smile_path(const std::string& options_csv_path,
                                  const std::string& symbol = {});

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
    double slip_pct = 0.05, bool allow_interpolate = true, IvCache* cache = nullptr);

}  // namespace wbt
