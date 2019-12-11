/*
 * tradingengine.h
 *
 *  Created on: 15. 11. 2019
 *      Author: ondra
 */

#ifndef SRC_SIMPLEFX_TRADINGENGINE_H_
#define SRC_SIMPLEFX_TRADINGENGINE_H_
#include <condition_variable>

#include "fndef.h"
#include <cstdint>
#include <mutex>
#include <vector>

#include "../brokers/api.h"
#include "../shared/refcnt.h"



class TradingEngine;

using PTradingEngine = ondra_shared::RefCntPtr<TradingEngine>;

class TradingEngine: public ondra_shared::RefCntObj {
public:
	TradingEngine(Command &&cmdFn);

	using UID = unsigned int;

	void start(RegisterPriceChangeEvent &&regFn);
	void stop();
	UID placeOrder(double price, double size, json::Value userId, const UID *replace = nullptr);
	void cancelOrder(UID id);

	UID readTrades(UID fromId, std::function<void(IStockApi::Trade)> &&cb);
	void readOrders(std::function<void(IStockApi::Order)> &&cb);
	IStockApi::Ticker getTicker() const;

	static PTradingEngine create(Command &&cmdIfc);

	static std::uint64_t now();


protected:

	Command cmdFn;


	struct Trade {
		UID id;
		double price;
		double size;
		std::uint64_t timestamp;
	};
	using Order = IStockApi::Order;

	double minPrice = 0;
	double maxPrice = 1e99;
	void updateMinMaxPrice();

	std::vector<Order> orders;
	std::vector<Trade> trades;
	mutable std::recursive_mutex lock;
	using Sync = std::unique_lock<std::recursive_mutex>;
	mutable std::condition_variable_any tickerWait;

	IStockApi::Ticker ticker;

	void onPriceChange(const IStockApi::Ticker &price);

	UID uidcnt;


	RegisterPriceChangeEvent starter;
	void startListenPrices() const;

	mutable std::uint64_t quoteStop = 0;
	mutable bool quotesStopped = true;
	void runQuotes() const;

};


#endif /* SRC_SIMPLEFX_TRADINGENGINE_H_ */