//
// Created by qlu on 2019/5/21.
//

#include <map>
#include <utility>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <kungfu/wingchun/util/business_helper.h>
#include <kungfu/wingchun/util/instrument.h>
#include <kungfu/wingchun/portfolio/account_manager.h>
#include "storage.h"

using namespace kungfu::wingchun::msg::data;
using namespace kungfu::wingchun::msg::nanomsg;

namespace kungfu
{
    namespace wingchun
    {
        
        AccountManager::AccountManager(const std::string& account_id, Calendar& calendar, const std::string& commission_db_file) :
                calendar_(calendar), last_update_(0), trading_day_(""),
                position_manager_(account_id, calendar), commission_manager_(account_id, commission_db_file)
        {
            strcpy(account_.account_id, account_id.c_str());
            position_manager_.register_pos_callback(std::bind(&AccountManager::on_pos_callback, this, std::placeholders::_1));
        }

        Position AccountManager::get_long_pos(const std::string& instrument_id, const std::string& exchange_id) const
        {
            return position_manager_.get_long_pos(instrument_id, exchange_id);
        }

        Position AccountManager::get_short_pos(const std::string& instrument_id, const std::string& exchange_id) const
        {
            return position_manager_.get_short_pos(instrument_id, exchange_id);
        }

        std::vector<Instrument> AccountManager::get_all_pos_instruments() const
        {
            return position_manager_.get_all_pos_instruments();
        }

        double AccountManager::get_last_price(const std::string& instrument_id, const std::string& exchange_id) const
        {
            return position_manager_.get_last_price(instrument_id, exchange_id);
        }

        void AccountManager::on_quote(const Quote *quote)
        {
            last_update_ = quote->rcv_time;

            // 账户初次创建并登陆td时查询回来的pos如果没有成本价的话，则用现价对其赋值，忽略其被加入功夫系统前的pnl
            // 这样计算不会太离谱
            auto long_pos = position_manager_.get_long_pos(quote->instrument_id, quote->exchange_id);
            if (is_zero(long_pos.cost_price) && long_pos.volume > 0)
            {
                double base = quote->last_price * long_pos.volume;
                if (quote->instrument_type == InstrumentTypeFuture)
                {
                    auto *instrument = InstrumentManager::get_instrument_manager()->get_future_instrument(quote->instrument_id, quote->exchange_id);
                    base = base * instrument->contract_multiplier * instrument->long_margin_ratio;
                }
                account_.initial_equity += base;
                account_.static_equity += base;
            }
            auto short_pos = position_manager_.get_short_pos(quote->instrument_id, quote->exchange_id);
            if (is_zero(short_pos.cost_price) && short_pos.volume > 0)
            {
                double base = quote->last_price * short_pos.volume;
                if (quote->instrument_type == InstrumentTypeFuture)
                {
                    auto *instrument = InstrumentManager::get_instrument_manager()->get_future_instrument(quote->instrument_id, quote->exchange_id);
                    base = base * instrument->contract_multiplier * instrument->short_margin_ratio;
                }
                account_.initial_equity += base;
                account_.static_equity += base;
            }

            position_manager_.on_quote(quote);
            if (recalc_acc())
            {
                callback();
            }
        }

        void AccountManager::on_order_input(const OrderInput *input)
        {
            if (strcmp(input->account_id, account_.account_id) != 0)
            {
                return;
            }

            // 不处理卖出
            if (input->side == SideSell || input->volume <= 0)
            {
                return;
            }

            double buy_cost = 0;
            if (is_reverse_repurchase(input->instrument_id, input->exchange_id))
            {
                buy_cost = input->volume * (1 + get_reverse_repurchase_commission_rate(input->instrument_id));
            } else
            {
                buy_cost = input->volume * input->frozen_price + commission_manager_.calc_commission(input);
            }

            if (is_less(account_.avail, buy_cost))
            {
                SPDLOG_ERROR("Not enough available cash");
            }

            account_.avail -= buy_cost;
            account_.frozen_cash += buy_cost;
            frozen_map_[input->order_id] = buy_cost;

            callback();
        }

        void AccountManager::on_order(const Order *order)
        {
            if (strcmp(order->account_id, account_.account_id) != 0)
            {
                return;
            }

            last_update_ = order->rcv_time;
            if (!is_final_status(order->status) || frozen_map_.find(order->order_id) == frozen_map_.end())
            {
                return;
            }

            auto fro = frozen_map_[order->order_id];
            frozen_map_.erase(order->order_id);
            account_.avail += fro;
            account_.frozen_cash -= fro;
            account_.update_time = order->rcv_time;
            position_manager_.on_order(order);
            recalc_acc();
            callback();
        }

        void AccountManager::on_trade(const Trade *trade)
        {
            SPDLOG_TRACE("trade: {}", msg::data::to_string(*trade));
            SPDLOG_TRACE("acc before: {}", msg::data::to_string(account_));

            if (strcmp(trade->account_id, account_.account_id) != 0)
            {
                return;
            }

            last_update_ = trade->rcv_time;
            if (trade->volume == 0)
            {
                return;
            }

            double buy_cost =
                    (is_reverse_repurchase(trade->instrument_id, trade->exchange_id) ? trade->volume : trade->price * trade->volume) + trade->tax +
                    trade->commission;
            double sell_profit = trade->price * trade->volume - trade->tax - trade->commission;

            bool frozen = false;
            if (frozen_map_.find(trade->order_id) != frozen_map_.end())
            {
                frozen = true;
                frozen_map_[trade->order_id] -= buy_cost;
                account_.frozen_cash -= buy_cost;
            }

            account_.update_time = trade->rcv_time;
            account_.accumulated_fee += (trade->tax + trade->commission);
            account_.intraday_fee += (trade->tax + trade->commission);
            if (trade->side == SideBuy)
            {
                if (!frozen)
                {
                    account_.avail -= buy_cost;
                }
                if (is_reverse_repurchase(trade->instrument_id, trade->exchange_id))
                {
                    auto days = get_reverse_repurchase_expire_days(trade->instrument_id);
                    auto expire_date = calendar_.calculate_trading_day(std::atoi(yijinjing::time::strftime(trade->trade_time, "%Y%m%d").c_str()),
                            get_reverse_repurchase_expire_days(trade->instrument_id));
                    bond_map_[std::to_string(expire_date)] += trade->volume * trade->price / 100.0 * days / 360 + trade->volume;
                }
            } else
            {
                account_.avail += sell_profit;
            }

            position_manager_.on_trade(trade);
            recalc_acc();
            callback();

            SPDLOG_TRACE("acc after: {}", msg::data::to_string(account_));
        }

        void AccountManager::on_switch_day(const std::string &trading_day)
        {
            if (trading_day <= trading_day_)
            {
                return;
            }
            trading_day_ = trading_day;
            strcpy(account_.trading_day, trading_day.c_str());

            position_manager_.on_switch_day(trading_day);

            // 撤销所有冻结
            frozen_map_.clear();
            account_.avail += account_.frozen_cash;
            account_.frozen_cash = 0;

            // 到期债券
            for (auto iter = bond_map_.begin(); iter != bond_map_.end();)
            {
                if (iter->first <= trading_day_)
                {
                    account_.avail += iter->second;
                    iter = bond_map_.erase(iter);
                } else
                {
                    ++iter;
                }
            }

            // 重算pnl
            recalc_acc();
            account_.static_equity = account_.dynamic_equity;
            account_.intraday_pnl = 0;
            account_.intraday_pnl_ratio = 0;
            callback();
        }

        void AccountManager::on_positions(const std::vector<msg::data::Position> &positions)
        {
            std::vector<msg::data::Position> filtered;
            for (const auto &pos : positions)
            {
                if (strcmp(pos.account_id, account_.account_id) == 0)
                {
                    filtered.emplace_back(pos);
                }
            }
            position_manager_.on_positions(filtered);
            if (recalc_acc())
            {
                callback();
            }
        }

        void AccountManager::on_position_details(const std::vector<msg::data::Position> &details)
        {
            // 股票期权可能跟期货类似的逻辑, 目前不支持
            position_manager_.on_position_details(details);
        }

        void AccountManager::on_account(const msg::data::AccountInfo &account)
        {
            if (strcmp(account.account_id, account_.account_id) != 0)
            {
                return;
            }
            last_update_ = account.rcv_time;
            account_.rcv_time = account.rcv_time;
            account_.update_time = account.update_time;
            strcpy(account_.trading_day, account.trading_day);
            strcpy(account_.broker_id, account.broker_id);
            strcpy(account_.source_id, account.source_id);
            if (is_zero(account_.initial_equity))
            {
                account_.initial_equity = account.initial_equity;
            }
            if (is_zero(account_.static_equity))
            {
                account_.static_equity = account.static_equity;
            }
            account_.avail = account.avail;
            account_.margin = account.margin;
            recalc_acc();
            callback();
        }

        double AccountManager::calc_commission(const msg::data::Trade *trade) const
        {
            if (is_reverse_repurchase(trade->instrument_id, trade->exchange_id))
            {
                return get_reverse_repurchase_expire_days(trade->instrument_id) * trade->volume;
            } else
            {
                auto rate = commission_manager_.get_commission_rate(trade->instrument_id, trade->exchange_id);
                double base = rate.mode == CommissionRateModeByVolume ? trade->volume : trade->price * trade->volume;
                return std::max((trade->side == SideBuy ? rate.open_ratio : rate.close_ratio) * base, rate.min_commission);
            }
        }

        double AccountManager::calc_tax(const msg::data::Trade *trade) const
        {
            // 税费共有四种，经手费(券商收取)、证管费(证监会收取)、过户费(中登收取)、印花税(国税，卖方收取)
            // 前两种合称佣金，其中征管费由券商代收，本系统内称为commission
            // 后两种本系统内合称tax
            // 以中信建投为例
            // 经手费 0.0487‰，征管费 0.02‰，过户费 0.02‰，印花税 1‰
            return trade->volume * trade->price * ((trade->side == SideSell ? 0.001 : 0) + 0.00002);
        }

        void AccountManager::set_initial_equity(double equity)
        {
            account_.initial_equity = equity;
        }

        void AccountManager::set_static_equity(double equity)
        {
            account_.static_equity = equity;
        }

        void AccountManager::register_pos_callback(PositionCallback cb)
        {
            pos_cbs_.emplace_back(std::move(cb));
        }

        void AccountManager::register_acc_callback(AccountCallback cb)
        {
            cbs_.emplace_back(std::move(cb));
        }

        void AccountManager::register_pnl_callback(PnLCallback cb)
        {}

        void AccountManager::set_current_trading_day(const std::string &trading_day)
        {
            position_manager_.set_current_trading_day(trading_day);
            if (trading_day >= trading_day_)
            {
                if (!trading_day_.empty())
                {
                    on_switch_day(trading_day);
                } else
                {
                    trading_day_ = trading_day;
                    strcpy(account_.trading_day, trading_day.c_str());
                }
            }
        }

        bool AccountManager::recalc_acc()
        {
            auto old_dynamic = account_.dynamic_equity;
            account_.market_value = position_manager_.get_market_value();
            account_.dynamic_equity = account_.market_value + account_.avail + account_.frozen_cash;
            if (is_zero(account_.initial_equity))
            {
                account_.initial_equity = account_.dynamic_equity;
            }
            if (is_zero(account_.static_equity))
            {
                account_.static_equity = account_.dynamic_equity;
            }
            account_.accumulated_pnl = account_.dynamic_equity - account_.initial_equity;
            account_.accumulated_pnl_ratio = is_zero(account_.initial_equity) ? 0 : account_.accumulated_pnl / account_.initial_equity;
            account_.intraday_pnl = account_.dynamic_equity - account_.static_equity;
            account_.intraday_pnl_ratio = is_zero(account_.static_equity) ? 0 : account_.intraday_pnl / account_.static_equity;
            return !is_equal(old_dynamic, account_.dynamic_equity);
        }

        void AccountManager::callback() const
        {
            for (const auto &cb : cbs_)
            {
                cb(account_);
            }
        }

        void AccountManager::on_pos_callback(const msg::data::Position &pos) const
        {
            for (const auto &cb : pos_cbs_)
            {
                cb(pos);
            }
        }

        void AccountManager::dump_to_db(SQLite::Database &db, bool save_meta)
        {
            position_manager_.dump_to_db(db);

            SQLite::Statement delete_account(db, "DELETE FROM account WHERE account_id = ? and source_id = ?");
            delete_account.bind(1, account_.account_id);
            delete_account.bind(2, account_.source_id);
            delete_account.exec();

            SQLite::Statement delete_bond_expired(db, "DELETE FROM bond_expire where account_id = ?");
            delete_bond_expired.bind(1, account_.account_id);
            delete_bond_expired.exec();

            SQLite::Statement delete_acc_frozen(db, "DELETE FROM acc_frozen WHERE account_id = ?");
            delete_acc_frozen.bind(1, account_.account_id);

            SQLite::Statement save_account_info(db, "INSERT INTO account("
                                                    "rcv_time, update_time, trading_day, account_id, broker_id, source_id, "
                                                    "initial_equity, static_equity, dynamic_equity, accumulated_pnl, accumulated_pnl_ratio, "
                                                    "intraday_pnl, intraday_pnl_ratio, avail, market_value, margin, accumulated_fee, "
                                                    "intraday_fee, frozen_cash, frozen_margin, frozen_fee, position_pnl, close_pnl"
                                                    ") "
                                                    "VALUES(?, ?, ?, ?, ?,?, ?, ?,?, ?, ?, ?, ?,?,?,?,?,?,?,?,?, ?, ?, ?)");
            save_account_info.bind(1, account_.rcv_time);
            save_account_info.bind(2, account_.update_time);
            save_account_info.bind(3, account_.trading_day);
            save_account_info.bind(4, account_.account_id);
            save_account_info.bind(5, account_.broker_id);
            save_account_info.bind(6, account_.source_id);
            save_account_info.bind(7, account_.initial_equity);
            save_account_info.bind(8, account_.static_equity);
            save_account_info.bind(9, account_.dynamic_equity);
            save_account_info.bind(10, account_.accumulated_pnl);
            save_account_info.bind(11, account_.accumulated_pnl_ratio);
            save_account_info.bind(12, account_.intraday_pnl);
            save_account_info.bind(13, account_.intraday_pnl_ratio);
            save_account_info.bind(14, account_.avail);
            save_account_info.bind(15, account_.market_value);
            save_account_info.bind(16, account_.margin);
            save_account_info.bind(17, account_.accumulated_fee);
            save_account_info.bind(18, account_.intraday_fee);
            save_account_info.bind(19, account_.frozen_cash);
            save_account_info.bind(20, account_.frozen_margin);
            save_account_info.bind(21, account_.frozen_fee);
            save_account_info.bind(22, account_.position_pnl);
            save_account_info.bind(23, account_.close_pnl);
            save_account_info.exec();

            for (const auto &iter : bond_map_)
            {
                SQLite::Statement insert_bond_expired(db, "INSERT INTO bond_expire(date, account_id, amount) VALUES(?, ?, ?)");
                insert_bond_expired.bind(1, iter.first);
                insert_bond_expired.bind(2, account_.account_id);
                insert_bond_expired.bind(3, iter.second);
                insert_bond_expired.exec();
            }

            for (const auto &iter : frozen_map_)
            {
                SQLite::Statement insert_acc_frozen(db, "INSERT INTO acc_frozen(order_id, account_id, amount) VALUES(?, ?, ?)");
                insert_acc_frozen.bind(1, iter.first);
                insert_acc_frozen.bind(2, account_.account_id);
                insert_acc_frozen.bind(3, iter.second);
                insert_acc_frozen.exec();
            }

            if (save_meta)
            {
                portfolio_util::save_meta_inner(db, last_update_, trading_day_);
            }
        }

        void AccountManager::dump_to_db(const std::string& db_file, bool save_meta)
        {
            SQLite::Database db(db_file, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            SQLite::Transaction transaction(db);
            dump_to_db(db, save_meta);
            transaction.commit();
        }

        void AccountManager::load_from_db(SQLite::Database &db)
        {
            if (!db.tableExists("meta"))
            {
                return;
            }

            position_manager_.load_from_db(db);

            portfolio_util::load_meta_inner(db, last_update_, trading_day_);

            SQLite::Statement query_account(db, "SELECT * FROM account WHERE account_id = ?");
            query_account.bind(1, account_.account_id);
            if (query_account.executeStep())
            {
                account_.rcv_time = query_account.getColumn(0);
                account_.update_time = query_account.getColumn(1);
                strcpy(account_.trading_day, query_account.getColumn(2));
                strcpy(account_.account_id, query_account.getColumn(3));
                strcpy(account_.broker_id, query_account.getColumn(4));
                strcpy(account_.source_id, query_account.getColumn(5));
                account_.initial_equity = query_account.getColumn(6);
                account_.static_equity = query_account.getColumn(7);
                account_.dynamic_equity = query_account.getColumn(8);
                account_.accumulated_pnl = query_account.getColumn(9);
                account_.accumulated_pnl_ratio = query_account.getColumn(10);
                account_.intraday_pnl = query_account.getColumn(11);
                account_.intraday_pnl_ratio = query_account.getColumn(12);
                account_.avail = query_account.getColumn(13);
                account_.market_value = query_account.getColumn(14);
                account_.margin = query_account.getColumn(15);
                account_.accumulated_fee = query_account.getColumn(16);
                account_.intraday_fee = query_account.getColumn(17);
                account_.frozen_cash = query_account.getColumn(18);
                account_.frozen_margin = query_account.getColumn(19);
                account_.frozen_fee = query_account.getColumn(20);
                account_.position_pnl = query_account.getColumn(21);
                account_.close_pnl = query_account.getColumn(22);
            }

            bond_map_.clear();
            SQLite::Statement query_bond(db, "SELECT date, amount FROM bond_expire WHERE account_id = ?");
            query_bond.bind(1, account_.account_id);
            while (query_bond.executeStep())
            {
                bond_map_[query_bond.getColumn(0).getString()] = query_bond.getColumn(1);
            }

            frozen_map_.clear();
            SQLite::Statement query_frozen(db, "SELECT order_id, amount FROM acc_frozen WHERE account_id = ?");
            query_frozen.bind(1, account_.account_id);
            while (query_frozen.executeStep())
            {
                frozen_map_[query_frozen.getColumn(0)] = query_frozen.getColumn(1);
            }
        }
    }
}