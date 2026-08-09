#pragma once
#include "types.hpp"
#include "data.hpp"
#include <vector>
#include <string>
#include <optional>
#include <tuple>

namespace wbt {

constexpr const char* LEG_SHORT_CALL = "short_call";
constexpr const char* LEG_SHORT_PUT = "short_put";
constexpr const char* LEG_LONG_CALL = "long_call";
constexpr const char* LEG_LONG_PUT = "long_put";

struct OptPosition {
  OptType opt_type = OptType::Call;
  double strike = 0;
  Date exp_date = 0;
  int num_contracts = 0;  // + long / - short
  Date open_date = 0;
  double open_price = 0;
  double org_price = 0;
  double per_contract_delta = 0;
  double per_contract_gamma = 0;
  double per_contract_theta = 0;
  double implied_vol = 0;
  std::string leg_role;
  double open_stock_price = 0;
  int structure_id = 0;

  double get_cost_basis() const {
    return open_price * num_contracts * CONTRACT_SCALE;
  }
};

struct StockPos {
  double num_shares = 0;
  double open_price = 0;
  Date open_date = 0;
};

struct PositionsAndStats {
  std::vector<OptPosition> opt_list;
  std::vector<StockPos> stocks;
  double cash = 0;
  double annual_capital_gain = 0;
  double total_tax = 0;
  double total_capital_gain = 0;
  double total_put_capital_gain = 0;
  double total_call_capital_gain = 0;
  double total_stock_capital_gain = 0;
  double total_commission = 0;
  int num_data_missing = 0;
  int structure_id = 0;
  double structure_entry_value = 0;
  double structure_entry_cash = 0;
  Date structure_open_date = 0;
  double structure_open_spot = 0;
  int num_rolls = 0;
  int num_gamma_hedges = 0;
  double last_expected_pnl = 0;
  int last_wing_m_call = 0, last_wing_m_put = 0;
  bool gamma_scalping_active = false;

  double initial_price = 0;
  double cur_peak = 0;
  double max_drawdown = 0;
  std::string cur_peak_date;
  std::string max_drawdown_peak_date;

  const OptionsDataset* all_data = nullptr;

  double total_shares() const;
  double mark_option(const OptPosition& opt, Date as_of, double spot) const;
  std::pair<double, double> get_balance(Date as_of, double spot) const;  // nlv_ex_cash, opt_nlv
  double option_nlv(Date as_of, double spot) const;
  double structure_pnl(Date as_of, double spot) const;
  double structure_pnl_pct(Date as_of, double spot) const;
  std::tuple<double, double, double> net_greeks() const;
  std::optional<int> short_min_dte(Date as_of) const;
  void open_stock(double shares, double px, Date d);
  std::pair<double, double> close_stock_positions(double shares, double px);
};

}  // namespace wbt
