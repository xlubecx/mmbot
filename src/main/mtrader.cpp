#include <imtjson/value.h>
#include <imtjson/string.h>
#include "istockapi.h"
#include "mtrader.h"

#include <chrono>
#include <cmath>
#include <shared/logOutput.h>
#include <imtjson/object.h>
#include <imtjson/array.h>
#include <numeric>

#include "emulator.h"
#include "sgn.h"

using ondra_shared::logNote;
using ondra_shared::StrViewA;

json::NamedEnum<Dynmult_mode> strDynmult_mode  ({
	{Dynmult_mode::independent, "independent"},
	{Dynmult_mode::together, "together"},
	{Dynmult_mode::alternate, "alternate"},
	{Dynmult_mode::half_alternate, "half_alternate"}
});

json::NamedEnum<MTrader::Config::NeutralPosType> strNeutralPosType ({
	{MTrader::Config::assets, "assets"},
	{MTrader::Config::currency, "currency"},
	{MTrader::Config::center, "center"},
	{MTrader::Config::disabled, "disabled"}
});


std::string_view MTrader::vtradePrefix = "__vt__";

MTrader::MTrader(IStockSelector &stock_selector,
		StoragePtr &&storage,
		PStatSvc &&statsvc,
		Config config)
:stock(selectStock(stock_selector,config,ownedStock))
,cfg(std::move(config))
,storage(std::move(storage))
,statsvc(std::move(statsvc))
{
	//probe that broker is valid configured
	stock.testBroker();
	magic = this->statsvc->getHash() & 0xFFFFFFFF;
}


void MTrader::Config::parse_neutral_pos(StrViewA txt) {
	if (txt.empty()) {
		neutral_pos = 0;
		neutralPosType = disabled;
	} else {
		auto splt = txt.split(" ",2);
		StrViewA type = splt();
		StrViewA value = splt();

		if (value.empty()) {
			neutralPosType = assets;
			neutral_pos = strtod(type.data,nullptr);
		} else {
			neutralPosType = strNeutralPosType[type];
			neutral_pos =strtod(value.data,nullptr);
		}
	}

}

void unsupported(const ondra_shared::IniConfig::Section& ini,
		const std::initializer_list<std::string_view> &options,
		std::string_view desc) {

	for (auto &&x: options) {
		if (ini[x].defined()) {
			throw std::runtime_error(std::string(x).append(" - option is no longer supported. ").append(desc));
		}
	}

}

MTrader::Config MTrader::load(const ondra_shared::IniConfig::Section& ini, bool force_dry_run) {
	Config cfg;


	cfg.broker = ini.mandatory["broker"].getString();
	cfg.spread_calc_mins = ini["spread_calc_hours"].getUInt(24*5)*60;
	cfg.spread_calc_min_trades = ini["spread_calc_min_trades"].getUInt(8);
	cfg.spread_calc_max_trades = ini["spread_calc_max_trades"].getUInt(24);
	cfg.pairsymb = ini.mandatory["pair_symbol"].getString();

	cfg.buy_mult = ini["buy_mult"].getNumber(1.0);
	cfg.sell_mult = ini["sell_mult"].getNumber(1.0);

	cfg.buy_step_mult = ini["buy_step_mult"].getNumber(1.0);
	cfg.sell_step_mult = ini["sell_step_mult"].getNumber(1.0);
	cfg.external_assets = ini["external_assets"].getNumber(0);
	cfg.min_size = ini["min_size"].getNumber(0);


	cfg.dry_run = force_dry_run?true:ini["dry_run"].getBool(false);
	cfg.internal_balance = cfg.dry_run?true:ini["internal_balance"].getBool(false);
	cfg.detect_manual_trades = ini["detect_manual_trades"].getBool(true);
	cfg.enabled = ini["enable"].getBool(true);

	unsupported(ini, {
			"sliding_pos.change",
			"sliding_pos.acum",
			"sliding_pos.assets",
			"sliding_pos.currency",
			"sliding_pos.center",
			"sliding_pos.max_pos",
			"sliding_pos.giveup",
			"sliding_pos.recoil",
	}, "Check manual for new sliding_pos options");


	StrViewA neutral_pos_str = ini["neutral_pos"].getString("");
	cfg.parse_neutral_pos(neutral_pos_str);


	double default_accum = ini["acum_factor"].getNumber(0.5);
	cfg.acm_factor_buy = ini["acum_factor_buy"].getNumber(default_accum);
	cfg.acm_factor_sell = ini["acum_factor_sell"].getNumber(default_accum);


	cfg.dynmult_raise = ini["dynmult_raise"].getNumber(200);
	cfg.dynmult_fall = ini["dynmult_fall"].getNumber(1);
	cfg.dynmult_mode = strDynmult_mode[ini["dynmult_mode"].getString(strDynmult_mode[Dynmult_mode::independent])];
	cfg.emulated_currency = ini["emulated_currency"].getNumber(0);
	cfg.force_spread = ini["force_spread"].getNumber(0);
	cfg.force_margin = ini["force_margin"].getBool();

	cfg.accept_loss = ini["accept_loss"].getUInt(0);
	cfg.max_pos = ini["max_pos"].getNumber(0);

	cfg.sliding_pos_hours = ini["sliding_pos.hours"].getNumber(0);
	cfg.sliding_pos_weaken = ini["sliding_pos.weaken"].getNumber(0);

	cfg.title = ini["title"].getString();


	cfg.start_time = ini["start_time"].getUInt(0);

	if (cfg.spread_calc_mins > 1000000) throw std::runtime_error("spread_calc_hours is too big");
	if (cfg.spread_calc_min_trades > cfg.spread_calc_max_trades) throw std::runtime_error("'spread_calc_min_trades' must bee less then 'spread_calc_max_trades'");
	if (cfg.spread_calc_max_trades > 24*60) throw std::runtime_error("'spread_calc_max_trades' is too big");
	if (cfg.acm_factor_buy > 50) throw std::runtime_error("'acum_factor_buy' is too big");
	if (cfg.acm_factor_buy < -50) throw std::runtime_error("'acum_factor_buy' is too small");
	if (cfg.acm_factor_sell > 50) throw std::runtime_error("'acum_factor_sell' is too big");
	if (cfg.acm_factor_sell < -50) throw std::runtime_error("'acum_factor_sell' is too small");
	if (cfg.dynmult_raise > 1e6) throw std::runtime_error("'dynmult_raise' is too big");
	if (cfg.dynmult_raise < 0) throw std::runtime_error("'dynmult_raise' is too small");
	if (cfg.dynmult_fall > 100) throw std::runtime_error("'dynmult_fall' must be below 100");
	if (cfg.dynmult_fall <= 0) throw std::runtime_error("'dynmult_fall' must not be negative or zero");
	if (cfg.max_pos <0) throw std::runtime_error("'max_pos' must not be negative");
	if ((cfg.max_pos || cfg.sliding_pos_hours) && cfg.neutralPosType == Config::disabled) {
		throw std::runtime_error("Some option needs to define neutral_pos");
	}

	return cfg;
}

bool MTrader::Order::isSimilarTo(const Order& other, double step) {
	return std::fabs(price - other.price) < step && size * other.size > 0;
}


IStockApi &MTrader::selectStock(IStockSelector &stock_selector, const Config &conf,	std::unique_ptr<IStockApi> &ownedStock) {
	IStockApi *s = stock_selector.getStock(conf.broker);
	if (s == nullptr) throw std::runtime_error(std::string("Unknown stock market name: ")+std::string(conf.broker));
	if (conf.dry_run) {
		ownedStock = std::make_unique<EmulatorAPI>(*s, conf.emulated_currency);
		return *ownedStock;
	} else {
		return *s;
	}
}

double MTrader::raise_fall(double v, bool raise) const {
	if (raise) {
		double rr = (1.0+cfg.dynmult_raise/100.0);
		return v * rr;
	} else {
		double ff = (1.0-cfg.dynmult_fall/100.0);
		return std::max(1.0,v * ff);
	}
}

static auto calc_margin_range(double A, double D, double P) {
	double x1 = (A*P - 2*sqrt(A*D*P) + D)/A;
	double x2 = (A*P + 2*sqrt(A*D*P) + D)/A;
	return std::make_pair(x1,x2);
}


void MTrader::init() {
	if (need_load){
		loadState();
		need_load = false;
	}
}
int MTrader::perform() {

	try {

		init();

	double begbal = internal_balance + cfg.external_assets;

	//Get opened orders
	auto orders = getOrders();
	//get current status
	auto status = getMarketStatus();

	std::string buy_order_error;
	std::string sell_order_error;


	//update market fees
	minfo.fees = status.new_fees;
	//process all new trades
	auto ptres =processTrades(status, first_order);
	//merge trades on same price
	mergeTrades(trades.size() - status.new_trades.size());

	double lastTradePrice = trades.empty()?status.curPrice:trades.back().eff_price;

	bool calcadj = false;
	double neutral_pos=0;

	//if calculator is not valid, update it using current price and assets
	if (!calculator.isValid()) {
		calculator.update(lastTradePrice, status.assetBalance);
		calcadj = true;
	}
	if (!calculator.isValid()) {
		ondra_shared::logError("No asset balance is available. Buy some assets, use 'external_assets=' or use command achieve to invoke automatic initial trade");
	} else {

		switch (cfg.neutralPosType) {
		case Config::center: {
			double a = 1.0/(cfg.neutral_pos+1.0);
			neutral_pos = ((status.assetBalance-cfg.external_assets) * status.curPrice + currency_balance_cache)*a/status.curPrice+cfg.external_assets;
		}break;
		case Config::currency:
			neutral_pos = (currency_balance_cache-cfg.neutral_pos)/status.curPrice+status.assetBalance;
			break;
		case Config::assets:
			neutral_pos = cfg.neutral_pos + cfg.external_assets;
			break;
		default:
			neutral_pos = 0;
			break;
		}
		ondra_shared::logDebug("Neutral pos: $1", neutral_pos);



		double acm_buy, acm_sell;
/*		if (cfg.sliding_pos_acm) {
			double f = sgn(neutral_pos-status.assetBalance);
			acm_buy = cfg.acm_factor_buy * f;
			acm_sell = cfg.acm_factor_sell * f;
			ondra_shared::logDebug("Sliding pos: acum_factor_buy=$1, acum_factor_sell=$2", acm_buy, acm_sell);
		} else {*/
			acm_buy = cfg.acm_factor_buy;
			acm_sell = cfg.acm_factor_sell;
		/*}*/


		//only create orders, if there are no trades from previous run
		if (status.new_trades.empty()) {


			ondra_shared::logDebug("internal_balance=$1, external_balance=$2",status.internalBalance,status.assetBalance);
			if ( !similar(status.internalBalance ,status.assetBalance,1e-5)) {
				//when balance changes, we need to update calculator
				ondra_shared::logWarning("Detected balance change: $1 => $2", status.internalBalance, status.assetBalance);
				calculator.update(lastTradePrice, status.assetBalance);
				calcadj = true;
				internal_balance=status.assetBalance - cfg.external_assets;
			}




			//calculate buy order
			auto buyorder = calculateOrder(lastTradePrice,
										  -status.curStep*buy_dynmult*cfg.buy_step_mult,
										   status.curPrice, status.assetBalance, acm_buy,
										   neutral_pos);
			//calculate sell order
			auto sellorder = calculateOrder(lastTradePrice,
					                       status.curStep*sell_dynmult*cfg.sell_step_mult,
										   status.curPrice, status.assetBalance, acm_sell,
										   neutral_pos);

			try {
				setOrderCheckMaxPos(orders.buy, buyorder,status.assetBalance, neutral_pos);
			} catch (std::exception &e) {
				buy_order_error = e.what();
				orders.buy = buyorder;
				acceptLoss(buyorder, lastTradePrice, status);
			}

			try {
				setOrderCheckMaxPos(orders.sell, sellorder, status.assetBalance, neutral_pos);
			} catch (std::exception &e) {
				sell_order_error = e.what();
				orders.sell = sellorder;
				acceptLoss(sellorder, lastTradePrice, status);
			}
			//replace order on stockmarket
			//remember the orders (keep previous orders as well)
			std::swap(lastOrders[0],lastOrders[1]);
			lastOrders[0] = orders;

			update_dynmult(false,false);

		} else {
			const auto &lastTrade = trades.back();
			//update after trade
			if (!ptres.manual_trades) {
				calculator.update_after_trade(lastTrade.eff_price,  status.assetBalance,
						begbal, lastTrade.eff_size<0?acm_sell:acm_buy);
				calcadj = true;
			}

			currency_balance_cache = stock.getBalance(minfo.currency_symbol);


			if (cfg.sliding_pos_hours && trades.size()>1) {
				const auto & pt = trades[trades.size()-2];
				const auto & ct = lastTrade;
				double tdf = ct.time - pt.time;
				if (tdf > 0) {
					double tot = cfg.sliding_pos_hours * 3600 * 1000;
					double pos = ct.balance - neutral_pos;
					double pldiff = pos * (ct.eff_price - pt.eff_price);
					double eq = calculator.balance2price(neutral_pos);
					double neq = pldiff*sgn(tot)>0? eq + (ct.price - eq) * (tdf/fabs(tot)):eq;
					calculator = Calculator(neq, neutral_pos, false);
					ondra_shared::logDebug("sliding_pos.hours: tdf=$1 pos=$2 pldiff=$3 eq=$4 neq=$5",
							tdf, pos, pldiff, eq, neq);

				}
			}



			update_dynmult(!orders.buy.has_value() && lastTrade.size > 0,
						   !orders.sell.has_value() && lastTrade.size < 0);
		}




		if (calcadj) {
			double c = calculator.balance2price(1.0);
			ondra_shared::logNote("Calculator adjusted: $1 at $2, ref_price=$3 ($4)", calculator.getBalance(), calculator.getPrice(), c, c - prev_calc_ref);
			prev_calc_ref = c;
		}

	}

	//report orders to UI
	statsvc->reportOrders(orders.buy,orders.sell);
	//report order errors to UI
	statsvc->reportError(IStatSvc::ErrorObj(buy_order_error, sell_order_error));
	//report trades to UI
	statsvc->reportTrades(trades, minfo.leverage || cfg.force_margin);
	//report price to UI
	statsvc->reportPrice(status.curPrice);
	//report misc
	{
		double value = status.assetBalance * status.curPrice;
		double max_price = pow2((status.assetBalance * sqrt(status.curPrice))/cfg.external_assets);
		double S = value - currency_balance_cache;
		double min_price = S<=0?0:pow2(S/(status.assetBalance*sqrt(status.curPrice)));
		double b1 = isfinite(max_price)?calculator.price2balance(sqrt(max_price*min_price)):1;
		double boost = b1/(b1-cfg.external_assets);

		if (minfo.leverage && cfg.external_assets > 0) {
			double start_price = calculator.balance2price(cfg.external_assets);
			double cur_price = calculator.balance2price(status.assetBalance);
			double colateral = (currency_balance_cache+(start_price-sqrt(start_price*cur_price))*internal_balance )* (1 - 1 / minfo.leverage);
			auto range = calc_margin_range(cfg.external_assets, colateral, start_price);
			max_price = range.second;
			min_price = range.first;
			boost = cfg.external_assets*start_price / colateral;
		}

		statsvc->reportMisc(IStatSvc::MiscData{
			status.new_trades.empty()?0:sgn(status.new_trades.back().size),
			calculator.isAchieveMode(),
			calculator.balance2price(status.assetBalance),
			status.curPrice * (exp(status.curStep) - 1),
			buy_dynmult,
			sell_dynmult,
			2 * value,
			boost,
			min_price,
			max_price,
			trades.size(),
			trades.empty()?0:(trades.back().time-trades[0].time)
		});

	}

	//store current price (to build chart)
	chart.push_back(status.chartItem);
	//delete very old data from chart
	if (chart.size() > cfg.spread_calc_mins)
		chart.erase(chart.begin(),chart.end()-cfg.spread_calc_mins);


	//if this was first order, the next will not first order
	first_order = false;

	//save state
	saveState();

	return 0;
	} catch (std::exception &e) {
		statsvc->reportError(IStatSvc::ErrorObj(e.what()));
		throw;
	}
}


MTrader::OrderPair MTrader::getOrders() {
	OrderPair ret;
	auto data = stock.getOpenOrders(cfg.pairsymb);
	for (auto &&x: data) {
		try {
			if (x.client_id == magic) {
				Order o(x);
				if (o.size<0) {
					if (ret.sell.has_value()) {
						ondra_shared::logWarning("Multiple sell orders (trying to cancel)");
						stock.placeOrder(cfg.pairsymb,0,0,json::Value(),x.id);
					} else {
						ret.sell = o;
					}
				} else {
					if (ret.buy.has_value()) {
						ondra_shared::logWarning("Multiple buy orders (trying to cancel)");
						stock.placeOrder(cfg.pairsymb,0,0,json::Value(),x.id);
					} else {
						ret.buy = o;
					}
				}
			}
		} catch (std::exception &e) {
			ondra_shared::logError("$1", e.what());
		}
	}
	return ret;
}

void MTrader::setOrderCheckMaxPos(std::optional<Order> &orig, Order neworder, double balance, double neutral_pos) {
	if (cfg.max_pos) {
		double final_pos = balance + neworder.size;
		if (final_pos > neutral_pos + cfg.max_pos)
			throw std::runtime_error("Max position reached");
		if (final_pos < neutral_pos - cfg.max_pos)
			throw std::runtime_error("Min position reached");
	}
	if (cfg.enabled) {
		setOrder(orig, neworder);
	} else {
		if (orig.has_value()) {
			stock.placeOrder(cfg.pairsymb, 0, 0, 0, orig->id, 0);
		}
		throw std::runtime_error("Disabled (enable=off)");
	}

}

void MTrader::setOrder(std::optional<Order> &orig, Order neworder) {
	try {
		if (neworder.price < 0 || neworder.size == 0) return;
		neworder.client_id = magic;
		json::Value replaceid;
		double replaceSize = 0;
		if (orig.has_value()) {
			if (orig->isSimilarTo(neworder, minfo.currency_step)) return;
			replaceid = orig->id;
			replaceSize = std::fabs(orig->size);
		}
		json::Value placeid = stock.placeOrder(
					cfg.pairsymb,
					neworder.size,
					neworder.price,
					neworder.client_id,
					replaceid,
					replaceSize);
		if (placeid.isNull() || !placeid.defined()) {
			orig.reset();
		} else if (placeid != replaceid) {
			orig = neworder;
		}
	} catch (...) {
		orig.reset();
		throw;
	}
}



json::Value MTrader::getTradeLastId() const{
	json::Value res;
	if (!trades.empty()) {
		auto i = std::find_if_not(trades.rbegin(), trades.rend(), [&](auto &&x) {
			return json::StrViewA(x.id.toString()).begins(vtradePrefix);
		});
		if (i != trades.rend()) res = i->id;
	}
	return res;
}

MTrader::Status MTrader::getMarketStatus() const {

	Status res;



//	if (!initial_price) initial_price = res.curPrice;

	json::Value lastId;

	if (!trades.empty()) lastId = getTradeLastId();
	res.new_trades = stock.getTrades(lastId, cfg.start_time, cfg.pairsymb);

	{
		double balance = 0;
		for (auto &&t:res.new_trades) balance+=t.eff_size;
		res.internalBalance = internal_balance + balance + cfg.external_assets;
	}


	if (cfg.internal_balance) {
		res.assetBalance = res.internalBalance;
	} else{
		res.assetBalance = stock.getBalance(minfo.asset_symbol)+ cfg.external_assets;
	}



	auto step = cfg.force_spread>0?cfg.force_spread:statsvc->calcSpread(chart,cfg,minfo,res.assetBalance,prev_spread);
	res.curStep = step;
	prev_spread = step;


	res.new_fees = stock.getFees(cfg.pairsymb);

	auto ticker = stock.getTicker(cfg.pairsymb);
	res.curPrice = std::sqrt(ticker.ask*ticker.bid);

	res.chartItem.time = ticker.time;
	res.chartItem.bid = ticker.bid;
	res.chartItem.ask = ticker.ask;
	res.chartItem.last = ticker.last;

	return res;
}


MTrader::Order MTrader::calculateOrderFeeLess(
		double prevPrice,
		double step,
		double curPrice,
		double balance,
		double acm) const {
	Order order;

	double newPrice = prevPrice * exp(step);
	double fact = acm;
	double mult;

	if (step < 0) {
		//if price is lower than old, check whether current price is above
		//otherwise lower the price more
		if (newPrice > curPrice) newPrice = curPrice;
		mult = cfg.buy_mult;
	} else {
		//if price is higher then old, check whether current price is below
		//otherwise highter the newPrice more

		if (newPrice < curPrice) newPrice = curPrice;
		mult = cfg.sell_mult;
	}



	double newBalance = calculator.price2balance(newPrice);
	double base = (newBalance - balance);
	double extra = calculator.calcExtra(prevPrice, newPrice);
	double size = base +extra*fact;

	ondra_shared::logDebug("Set order: step=$1, base_price=$6, price=$2, base=$3, extra=$4, total=$5",step, newPrice, base, extra, size, prevPrice);

	if (size * step > 0) size = 0;
	//fill order
	order.size = size * mult;
	order.price = newPrice;


	return order;

}

MTrader::Order MTrader::calculateOrder(
		double lastTradePrice,
		double step,
		double curPrice,
		double balance,
		double acm,
		double neutral_pos) const {

	Order order(calculateOrderFeeLess(lastTradePrice, step,curPrice,balance,acm));

	if (cfg.neutralPosType != Config::disabled && cfg.sliding_pos_weaken ) {
		double maxpos = cfg.external_assets* cfg.sliding_pos_weaken * 0.01;
		double curpos = balance - neutral_pos;
		double mult = (maxpos - fabs(curpos))/maxpos;
		if (mult < 1e-10) mult = 1e-10;
		order.size *= mult;

		ondra_shared::logDebug("sliding_pos.weaken: maxpos=$1 curpos=$2 mult=$3",
				maxpos, curpos, mult);

	}


	if (std::fabs(order.size) < cfg.min_size) {
		order.size = cfg.min_size*sgn(order.size);
	}
	if (std::fabs(order.size) < minfo.min_size) {
		order.size = minfo.min_size*sgn(order.size);
	}
	if (minfo.min_volume) {
		double vol = std::fabs(order.size * order.price);
		if (vol < minfo.min_volume) {
			order.size = minfo.min_volume/order.price*sgn(order.size);
		}
	}
	//apply fees
	minfo.addFees(order.size, order.price);

	//order here
	return order;

}




void MTrader::loadState() {
	minfo = stock.getMarketInfo(cfg.pairsymb);
	this->statsvc->setInfo(
			IStatSvc::Info {
				cfg.title,
				minfo.asset_symbol,
				minfo.currency_symbol,
				minfo.invert_price?minfo.inverted_symbol:minfo.currency_symbol,
				minfo.invert_price,
				minfo.leverage || cfg.force_margin,
				stock.isTest()
			});
	currency_balance_cache = stock.getBalance(minfo.currency_symbol);

	if (storage == nullptr) return;
	auto st = storage->load();
	need_load = false;

	bool wastest = false;

	auto curtest = stock.isTest();
	bool drop_calc = false;

	bool recalc_trades = false;
	bool recalc_position = true;

	if (st.defined()) {
		json::Value tst = st["testStartTime"];
		wastest = tst.defined();

		drop_calc = drop_calc || (wastest != curtest);

		testStartTime = tst.getUInt();
		auto state = st["state"];
		if (state.defined()) {
			if (!curtest) {
				buy_dynmult = state["buy_dynmult"].getNumber();
				sell_dynmult = state["sell_dynmult"].getNumber();
			}
			prev_spread = state["lnspread"].getNumber();
			internal_balance = state["internal_balance"].getNumber();
			double ext_ass = state["external_assets"].getNumber();
			if (ext_ass != cfg.external_assets) drop_calc = true;

		}
		auto chartSect = st["chart"];
		if (chartSect.defined()) {
			chart.clear();
			for (json::Value v: chartSect) {
				double ask = v["ask"].getNumber();
				double bid = v["bid"].getNumber();
				json::Value vlast = v["last"];
				double last = vlast.defined()?vlast.getNumber():sqrt(ask*bid);

				chart.push_back({
					v["time"].getUInt(),ask,bid,last
				});
			}
		}
		{
			auto trSect = st["trades"];
			if (trSect.defined()) {
				trades.clear();
				for (json::Value v: trSect) {
					TWBItem itm = TWBItem::fromJSON(v);
					if (wastest && !curtest && itm.time > testStartTime ) {
						continue;
					} else {
						trades.push_back(itm);
						recalc_trades = recalc_trades || itm.balance == TWBItem::no_balance;
						recalc_position = recalc_position || itm.position == TWBItem::no_balance;
					}
				}
			}
			mergeTrades(0);
		}
		if (!drop_calc) {
			calculator = Calculator::fromJSON(st["calc"]);
			lastOrders[0] = OrderPair::fromJSON(st["orders"][0]);
			lastOrders[1] = OrderPair::fromJSON(st["orders"][1]);
		}
	}
	if (curtest && testStartTime == 0) {
		testStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()
				).count();
	}
	if (recalc_trades) {
			double endBal = stock.getBalance(minfo.asset_symbol) + cfg.external_assets;
		double chng = std::accumulate(trades.begin(), trades.end(),0.0,[](auto &&a, auto &&b) {
			return a + b.eff_size;
		});
		double begBal = endBal-chng;
		for (auto &&t:trades) {
			if (t.balance < 0) t.balance = begBal+t.eff_size;
			begBal = t.balance;
		}
	}
	if (recalc_position) {
		double endPos = trades.back().balance - cfg.external_assets;
		double chng = std::accumulate(trades.begin(), trades.end(),0.0,[](auto &&a, auto &&b) {
			return a + b.eff_size;
		});
		double begPos = endPos-chng;
		for (auto &&t:trades) {
			t.position = begPos+t.eff_size;
			begPos = t.position;
		}
	}
	if (internal_balance == 0) {
		if (!trades.empty()) internal_balance = trades.back().balance- cfg.external_assets;
	}
}

void MTrader::saveState() {
	if (storage == nullptr) return;
	json::Object obj;

	obj.set("version",2);
	if (stock.isTest()) {
		obj.set("testStartTime", testStartTime);;
	}

	{
		auto st = obj.object("state");
		st.set("buy_dynmult", buy_dynmult);
		st.set("sell_dynmult", sell_dynmult);
		st.set("lnspread", prev_spread);
		st.set("internal_balance", internal_balance);
		st.set("external_assets", cfg.external_assets);
	}
	{
		auto ch = obj.array("chart");
		for (auto &&itm: chart) {
			ch.push_back(json::Object("time", itm.time)
				  ("ask",itm.ask)
				  ("bid",itm.bid)
				  ("last",itm.last));
		}
	}
	{
		auto tr = obj.array("trades");
		for (auto &&itm:trades) {
			tr.push_back(itm.toJSON());
		}
	}
	obj.set("calc", calculator.toJSON());
	obj.set("orders", {lastOrders[0].toJSON(),lastOrders[1].toJSON()});
	storage->store(obj);
}

MTrader::CalcRes MTrader::calc_min_max_range() {

	CalcRes res {};
	loadState();


	res.avail_assets = stock.getBalance(minfo.asset_symbol);
	res.avail_money = stock.getBalance(minfo.currency_symbol);
	res.cur_price = stock.getTicker(cfg.pairsymb).last;
	res.assets = res.avail_assets+cfg.external_assets;
	res.value = res.assets * res.cur_price;
	res.max_price = pow2((res.assets * sqrt(res.cur_price))/(res.assets -res.avail_assets));
	double S = res.value - res.avail_money;
	res.min_price = S<=0?0:pow2(S/(res.assets*sqrt(res.cur_price)));
	return res;


}

void MTrader::mergeTrades(std::size_t fromPos) {
	if (fromPos) --fromPos;
	auto wr = trades.begin()+fromPos;
	auto rd = wr;
	auto end = trades.end();

	if (rd == end) return ;
	++rd;
	while (rd != end) {
		if (rd->price == wr->price && rd->size * wr->size > 0) {
			wr->size+=rd->size;
			wr->balance = rd->balance;
			wr->eff_price = rd->eff_price;
			wr->eff_size+=rd->eff_size;
			wr->time = rd->time;
			wr->id = rd->id;
		} else {
			++wr;
			if (wr != rd) *wr = *rd;
		}
		++rd;
	}
	++wr;
	if (wr != rd) trades.erase(wr,end);
}



bool MTrader::eraseTrade(std::string_view id, bool trunc) {
	if (need_load) loadState();
	auto iter = std::find_if(trades.begin(), trades.end(), [&](const IStockApi::Trade &tr) {
		json::String s = tr.id.toString();
		return s.str() == id;
	});
	if (iter == trades.end()) return false;
	if (trunc) {
		trades.erase(iter, trades.end());
	} else {
		trades.erase(iter);
	}
	saveState();
	return true;
}


MTrader::OrderPair MTrader::OrderPair::fromJSON(json::Value json) {
	json::Value bj = json["bj"];
	json::Value sj = json["sj"];
	std::optional<Order> nullorder;
	return 	OrderPair {
		bj.defined()?std::optional<Order>(Order::fromJSON(bj)):nullorder,
		sj.defined()?std::optional<Order>(Order::fromJSON(sj)):nullorder
	};
}

json::Value MTrader::OrderPair::toJSON() const {
	return json::Object
			("buy",buy.has_value()?buy->toJSON():json::Value())
			("sell",sell.has_value()?sell->toJSON():json::Value());
}

MTrader::PTResult MTrader::processTrades(Status &st,bool first_trade) {

	bool buy_trade = false;
	bool sell_trade = false;
	bool was_manual = false;

	for (auto &&t : st.new_trades) {
		bool manual = false;
		manual = true;
		Order fkord(t.size, t.price);
		for (auto &lo : lastOrders) {
			if (t.eff_size < 0) {
				if (lo.sell.has_value() && t.eff_size > lo.sell->size*1.1) {
					manual = false;
				}
			}
			if (t.eff_size > 0) {
				if (lo.buy.has_value() && t.eff_size < lo.buy->size*1.1) {
					manual = false;
				}
			}
			if (!manual) break;
		}
		if (manual) {
			for (auto &lo : lastOrders) {
				ondra_shared::logNote("Detected manual trade: $1 $2 $3",
						!lo.buy.has_value()?0.0:lo.buy->price, fkord.price, !lo.sell.has_value()?0.0:lo.sell->price);
			}
		}

		was_manual = was_manual || manual;


		if (!cfg.detect_manual_trades || first_trade)
			manual = false;

		if (!manual) {
			internal_balance += t.eff_size;
		}

		buy_trade = buy_trade || t.eff_size > 0;
		sell_trade = sell_trade || t.eff_size < 0;

		trades.push_back(TWBItem(t, st.assetBalance,
				st.assetBalance - cfg.external_assets,
				manual || calculator.isAchieveMode()));
	}


	st.internalBalance = internal_balance + cfg.external_assets;
	prev_calc_ref = calculator.balance2price(1);
	return {was_manual};
}

void MTrader::update_dynmult(bool buy_trade,bool sell_trade) {

	switch (cfg.dynmult_mode) {
	case Dynmult_mode::independent:
		break;
	case Dynmult_mode::together:
		buy_trade = buy_trade || sell_trade;
		sell_trade = buy_trade;
		break;
	case Dynmult_mode::alternate:
		if (buy_trade) this->sell_dynmult = 0;
		else if (sell_trade) this->buy_dynmult = 0;
		break;
	case Dynmult_mode::half_alternate:
		if (buy_trade) this->sell_dynmult = ((this->sell_dynmult-1) * 0.5) + 1;
		else if (sell_trade) this->buy_dynmult = ((this->buy_dynmult-1) * 0.5) + 1;
		break;
	}
	this->buy_dynmult= raise_fall(this->buy_dynmult, buy_trade);
	this->sell_dynmult= raise_fall(this->sell_dynmult, sell_trade);
}

void MTrader::reset() {
	if (need_load) loadState();
	if (trades.size() > 1) {
		trades.erase(trades.begin(), trades.end()-1);
	}
	saveState();
}

void MTrader::achieve_balance(double price, double balance) {
	if (need_load) loadState();
	if (minfo.leverage>0) {
		balance += cfg.external_assets;
	}
	if (balance > 0) {
		calculator.achieve(price, balance);
		saveState();
	} else {
		throw std::runtime_error("can't set negative balance");
	}
}

void MTrader::repair() {
	if (need_load) loadState();
	buy_dynmult = 1;
	sell_dynmult = 1;
	if (cfg.internal_balance) {
		if (!trades.empty())
			internal_balance = trades.back().balance- cfg.external_assets;
	} else {
		internal_balance = 0;
	}
	prev_spread = 0;
	currency_balance_cache =0;
	prev_calc_ref = 0;
	saveState();
}

ondra_shared::StringView<IStatSvc::ChartItem> MTrader::getChart() const {
	return chart;
}

double MTrader::getLastSpread() const {
	return prev_spread;
}

double MTrader::getInternalBalance() const {
	return internal_balance;
}
void MTrader::setInternalBalance(double v) {
	internal_balance = v;
}

void MTrader::acceptLoss(const Order &order, double lastTradePrice,  const Status &st) {

	if (cfg.accept_loss && cfg.enabled && !trades.empty()) {
		std::size_t ttm = trades.back().time;

		std::size_t e = st.chartItem.time>ttm?(st.chartItem.time-ttm)/(3600000):0;
		if (e > cfg.accept_loss && buy_dynmult <= 1.0 && sell_dynmult <= 1.0) {
			auto reford = calculateOrder(lastTradePrice, 2 * st.curStep * sgn(-order.size),lastTradePrice, st.assetBalance, 0, st.assetBalance);
			double df = (st.curPrice - reford.price)* sgn(-order.size);
			if (df > 0) {
				ondra_shared::logWarning("Accept loss in effect: price=$1, balance=$2", st.curPrice, st.assetBalance);
				trades.push_back(IStockApi::TradeWithBalance (
						IStockApi::Trade {
							json::Value(json::String({vtradePrefix,"loss_", std::to_string(st.chartItem.time)})),
							st.chartItem.time,
							0,
							reford.price,
							0,
							reford.price,
						}, st.assetBalance, st.assetBalance - cfg.external_assets,false));
				update_dynmult(order.size>0, order.size<0);
				calculator.update(reford.price, st.assetBalance);
			}
		}
	}

}

