/*
 * strategy_keepvalue.cpp
 *
 *  Created on: 20. 10. 2019
 *      Author: ondra
 */

#include "strategy_leveraged_base.h"

#include <chrono>
#include <imtjson/object.h>
#include "../shared/logOutput.h"
#include <cmath>

#include "../imtjson/src/imtjson/string.h"
#include "sgn.h"


using ondra_shared::logDebug;

template<typename Calc>
std::string_view Strategy_Leveraged<Calc>::id = Calc::id;

template<typename Calc>
Strategy_Leveraged<Calc>::Strategy_Leveraged(const PCalc &calc, const PConfig &cfg, State &&st)
:calc(calc),cfg(cfg), st(std::move(st)) {}
template<typename Calc>
Strategy_Leveraged<Calc>::Strategy_Leveraged(const PCalc &calc, const PConfig &cfg)
:calc(calc),cfg(cfg) {}


template<typename Calc>
bool Strategy_Leveraged<Calc>::isValid() const {
	return st.neutral_price > 0;
}

template<typename Calc>
void Strategy_Leveraged<Calc>::recalcNewState(const PCalc &calc, const PConfig &cfg, State &nwst) {
	double adjbalance = std::abs(nwst.bal + cfg->external_balance) * cfg->power;
	nwst.power = calc->calcPower(nwst.last_price, adjbalance, cfg->asym);
	recalcNeutral(calc,cfg,nwst);
	nwst.val = calc->calcPosValue(nwst.power, calcAsym(cfg,nwst), nwst.neutral_price, nwst.last_price);
}

template<typename Calc>
Strategy_Leveraged<Calc> Strategy_Leveraged<Calc>::init(const PCalc &calc, const PConfig &cfg, double price, double pos, double currency, const IStockApi::MarketInfo &minfo) {
	bool futures = minfo.leverage != 0;
	auto bal = getBalance(futures, price, pos, currency);
	State nwst {
		/*neutral_price:*/ price,
		/*last_price */ price,
		/*position */ pos - bal.second,
		/*bal */ bal.first,
		/* val */ 0,
		/* power */ 0,
		/* neutral_pos */bal.second
	};
	if (nwst.bal <= 0) {
		//we cannot calc with empty balance. In this case, use price for calculation (but this is  unreal, trading still impossible)
		nwst.bal = price;
	}
	PCalc newcalc = calc;
	if (!newcalc->isValid(minfo)) newcalc = std::make_shared<Calc>(calc->init(minfo));
	recalcNewState(newcalc, cfg,nwst);
	return Strategy_Leveraged(newcalc, cfg, std::move(nwst));
}



template<typename Calc>
double Strategy_Leveraged<Calc>::calcMult() const {
	return st.power;
}

template<typename Calc>
typename Strategy_Leveraged<Calc>::PosCalcRes Strategy_Leveraged<Calc>::calcPosition(double price) const {
	auto mm = calcRoots();
	bool lmt = false;
	if (price < mm.min) {price = mm.min; lmt = true;}
	if (price > mm.max) {price = mm.max; lmt = true;}
	if (lmt) {
		return {true,st.position};
	} else {

		double profit = st.position * (price - st.last_price);
		double new_neutral = cfg->reduction?calcNewNeutralFromProfit(profit, price):st.neutral_price;
		double pos = calc->calcPosition(calcMult(), calcAsym(), new_neutral, price);
		return {false,pos};
	}
}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::onIdle(
		const IStockApi::MarketInfo &minfo,
		const IStockApi::Ticker &ticker, double assets, double currency) const {
	if (isValid()) {
		if (st.power <= 0) {
			State nst = st;
			recalcNewState(calc, cfg, nst);
			return new Strategy_Leveraged<Calc>(calc, cfg, std::move(nst));
		} else {
			return this;
		}
	}
	else {
		return new Strategy_Leveraged<Calc>(init(calc, cfg,ticker.last, assets, currency, minfo));
	}
}

template<typename Calc>
double Strategy_Leveraged<Calc>::calcNewNeutralFromProfit(double profit, double price) const {
	if ((st.last_price - st.neutral_price) * (price - st.neutral_price) <= 0 || profit == 0)
			return st.neutral_price;

	double mult = calcMult();
	double asym = calcAsym();
	double middle = calc->calcPrice0(st.neutral_price, asym);
	double prev_val = st.val;
	double cur_val = calc->calcPosValue(mult, asym, st.neutral_price, price);
	double new_val;
	if (prev_val < 0 && (price - middle) * (st.neutral_price - middle)>0) {
		new_val = 2*cur_val - (prev_val - profit);
	} else {
		new_val = prev_val - profit;
	}

	double c_neutral = calc->calcNeutralFromValue(mult, asym, st.neutral_price, new_val, price);
	double new_neutral = st.neutral_price + (c_neutral - st.neutral_price)* 2 * (cfg->reduction+cfg->dynred*std::abs(st.position*st.last_price/st.bal));

	double pos1 = calc->calcPosition(mult, asym, st.neutral_price, price);
	double pos2 = calc->calcPosition(mult, asym, new_neutral, price);
	if ((pos1 - st.position) * (pos2 - st.position) < 0) {
		return calc->calcNeutral(mult, asym, st.position, price);
	} else {
		return new_neutral;

	}
}

template<typename Calc>
void Strategy_Leveraged<Calc>::recalcPower(const PCalc &calc, const PConfig &cfg, State &nwst) {
	double offset = calc->calcPosition(nwst.power, cfg->asym, nwst.neutral_price, nwst.neutral_price);
	double adjbalance = std::abs(nwst.bal + cfg->external_balance + nwst.neutral_price * std::abs(nwst.position - offset) * cfg->powadj) * cfg->power;
	double power = calc->calcPower(nwst.neutral_price, adjbalance, cfg->asym);
	if (std::isfinite(power)) {
		nwst.power = power;
	}
}

template<typename Calc>
void Strategy_Leveraged<Calc>::recalcNeutral(const PCalc &calc, const PConfig &cfg,State &nwst)  {
	double neutral_price = calc->calcNeutral(nwst.power, calcAsym(cfg,nwst), nwst.position,
			nwst.last_price);
	if (std::isfinite(neutral_price) && neutral_price > 0) {
		nwst.neutral_price = neutral_price;
	}
}

template<typename Calc>
std::pair<typename Strategy_Leveraged<Calc>::OnTradeResult, PStrategy> Strategy_Leveraged<Calc>::onTrade(
		const IStockApi::MarketInfo &minfo,
		double tradePrice, double tradeSize, double assetsLeft,
		double currencyLeft) const {


	if (!isValid()) {
		return init(calc, cfg,tradePrice, assetsLeft, currencyLeft, minfo)
				.onTrade(minfo, tradePrice, tradeSize, assetsLeft, currencyLeft);
	}

	State nwst = st;
	if (tradeSize * st.position < 0 && (st.position + tradeSize)/st.position > 0.5) {
		int chg = sgn(tradeSize);
		nwst.trend_cntr += chg - nwst.trend_cntr/1000;

	}
	double apos = assetsLeft - st.neutral_pos;
	auto cpos = calcPosition(tradePrice);
	double mult = calcMult();
	double profit = (apos - tradeSize) * (tradePrice - st.last_price);
	//double vprofit = (st.position) * (tradePrice - st.last_price);
	//store current position
	nwst.position = cpos.pos;
	//store last price
	nwst.last_price = tradePrice;

	recalcNeutral(calc, cfg, nwst);

	auto bal = getBalance(minfo.leverage, tradePrice, assetsLeft, currencyLeft);
	nwst.neutral_pos = bal.second;

	double val = calc->calcPosValue(mult, calcAsym(), nwst.neutral_price, tradePrice);
	//calculate extra profit - we get change of value and add profit. This shows how effective is strategy. If extra is positive, it generates
	//profit, if it is negative, is losses
	double extra = (val - st.val) + profit;
	//store val to calculate next profit (because strategy was adjusted)
	nwst.val = val;
	//store new balance
	nwst.bal = std::max(0.0, val) + bal.first;

	recalcPower(calc, cfg, nwst);

	return {
		OnTradeResult{extra,0,calc->calcPrice0(st.neutral_price, calcAsym()),0},
		new Strategy_Leveraged<Calc>(calc, cfg,  std::move(nwst))
	};

}

template<typename Calc>
json::Value Strategy_Leveraged<Calc>::storeCfgCmp() const {
	return json::Object("asym", static_cast<int>(cfg->asym * 1000))("ebal",
			static_cast<int>(cfg->external_balance * 1000))("power",
			static_cast<int>(cfg->power * 1000));
}

template<typename Calc>
json::Value Strategy_Leveraged<Calc>::exportState() const {
	return json::Object
			("neutral_price",st.neutral_price)
			("last_price",st.last_price)
			("position",st.position)
			("balance",st.bal)
			("val",st.val)
			("power",st.power)
			("neutral_pos",st.neutral_pos)
			("trend", st.trend_cntr)
			("cfg", storeCfgCmp());

}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::importState(json::Value src,const IStockApi::MarketInfo &minfo) const {
		State newst {
			src["neutral_price"].getNumber(),
			src["last_price"].getNumber(),
			src["position"].getNumber(),
			src["balance"].getNumber(),
			src["val"].getNumber(),
			src["power"].getNumber(),
			src["neutral_pos"].getNumber(),
			src["trend"].getInt()
		};
		json::Value cfgcmp = src["cfg"];
		json::Value cfgcmp2 = storeCfgCmp();
		if (cfgcmp != cfgcmp2) {
			recalcNewState(calc, cfg,newst);
		}
		PCalc newcalc = calc;
		if (!newcalc->isValid(minfo)) newcalc = std::make_shared<Calc>(newcalc->init(minfo));
		return new Strategy_Leveraged<Calc>(newcalc, cfg, std::move(newst));
}

template<typename Calc>
IStrategy::OrderData Strategy_Leveraged<Calc>::getNewOrder(
		const IStockApi::MarketInfo &minfo,
		double curPrice, double price, double dir, double assets, double currency) const {
	auto apos = assets - st.neutral_pos;
	auto mm = calcRoots();
	if (curPrice < mm.min || curPrice > mm.max) {
		auto testStat = onTrade(minfo,curPrice,0,assets,currency);
		auto mm2 = static_cast<const Strategy_Leveraged<Calc> *>((const IStrategy *)(testStat.second))->calcRoots();
		if (dir * apos < 0 && (curPrice < mm2.min || curPrice > mm2.max))
			return {curPrice,-apos,Alert::stoploss};
		else
			return {0,0,Alert::stoploss};
	} else {
		auto cps = calcPosition(price);
		double ch1 = cps.pos - st.position;
		double ch2 = cps.pos - apos;
		if (ch2 * dir < 0)
			ch2 = ch1 / 2.0;
		else if (ch2 * dir > 2 * ch1 * dir)
			ch2 = ch1 * 2;
		return {0, ch2};
	}
}

template<typename Calc>
typename Strategy_Leveraged<Calc>::MinMax Strategy_Leveraged<Calc>::calcSafeRange(
		const IStockApi::MarketInfo &minfo,
		double assets,
		double currencies) const {

	if (minfo.leverage) {
		return calcRoots();
	} else {
		auto r = calcRoots();
		double maxp = calc->calcPriceFromPosition(st.power, calcAsym(), st.neutral_price, -st.neutral_pos);
		double minp = calc->calcRoots(st.power, calcAsym(),st.neutral_price, currencies).min;
		return {std::max(r.min,minp),std::min(r.max,maxp)};
	}
}

template<typename Calc>
double Strategy_Leveraged<Calc>::getEquilibrium(double assets) const {
	return  calc->calcPriceFromPosition(st.power, calcAsym(), st.neutral_price, assets-st.neutral_pos);
}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::reset() const {
	return new Strategy_Leveraged<Calc>(calc, cfg,{});
}

template<typename Calc>
json::Value Strategy_Leveraged<Calc>::dumpStatePretty(
		const IStockApi::MarketInfo &minfo) const {

	return json::Object("Position", (minfo.invert_price?-1:1)*st.position)
				  ("Last price", minfo.invert_price?1/st.last_price:st.last_price)
				 ("Neutral price", minfo.invert_price?1/st.neutral_price:st.neutral_price)
				 ("Value", st.val)
				 ("Last balance", st.bal)
				 ("Multiplier", st.power)
				 ("Neutral pos", st.neutral_pos?json::Value(st.neutral_pos):json::Value())
	 	 	 	 ("Trend factor", json::String({
						json::Value((minfo.invert_price?-1:1)*trendFactor(st)*100).toString(),"%"}));


}



template<typename Calc>
double Strategy_Leveraged<Calc>::calcMaxLoss() const {
	double lmt;
	if (cfg->max_loss == 0)
		lmt = st.bal;
	else
		lmt = cfg->max_loss;

	if (st.val < 0)
		lmt += st.val;
	return lmt;
}

template<typename Calc>
typename Strategy_Leveraged<Calc>::MinMax Strategy_Leveraged<Calc>::calcRoots() const {
	if (!rootsCache.has_value()) {
		double lmt = calcMaxLoss();
		rootsCache = calc->calcRoots(calcMult(), calcAsym(),st.neutral_price,lmt);
	}
	return *rootsCache;
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcAsym(const PConfig &cfg, const State &st)  {
	if (cfg->detect_trend) {
		return cfg->asym * trendFactor(st);
	}
	else {
		return cfg->asym;
	}
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcAsym() const {
	return calcAsym(cfg,st);
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::trendFactor(const State &st) {
	return st.trend_cntr*0.001;
}

template<typename Calc>
std::pair<double,double> Strategy_Leveraged<Calc>::getBalance(bool leveraged, double price, double assets, double currency) {
	if (leveraged) return {currency, 0};
	double md = assets + currency / price;
	return {md * price, md / 2};
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcInitialPosition(const IStockApi::MarketInfo &minfo, double price, double assets, double currency) const {
	auto bal = getBalance(minfo.leverage != 0,price,assets,currency);
	double adjbalance = std::abs(bal.first + cfg->external_balance) * cfg->power;
	double asym = calcAsym(cfg, st);
	double power = calc->calcPower(price, adjbalance, calcAsym(cfg, st));
	return calc->calcPosition(power, asym, price, price)+bal.second;
}

