#include <benchmark/benchmark.h>
#include <iostream>
#include <cstdint>
#include <vector>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>

// ============================================================================
// 1. PRIMITIVES & DATA STRUCTURES
// ============================================================================
using Price = uint64_t;
using Quantity = uint32_t;
using OrderId = uint64_t;

enum class Side { BUY, SELL };

constexpr size_t NULL_INDEX = static_cast<size_t>(-1);

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    size_t prev_index = NULL_INDEX;
    size_t next_index = NULL_INDEX;
};

struct Trade {
    OrderId buyer_id;
    OrderId seller_id;
    Price price;
    Quantity quantity;
};

struct PriceLevelQueue {
    size_t head_index = NULL_INDEX;
    size_t tail_index = NULL_INDEX;
    Quantity total_qty = 0;
    
    bool empty() const { return head_index == NULL_INDEX; }
};

struct BBO {
    Price bid_price = 0; Quantity bid_qty = 0;
    Price ask_price = 0; Quantity ask_qty = 0;
    
    bool operator!=(const BBO& other) const {
        return bid_price != other.bid_price || bid_qty != other.bid_qty ||
               ask_price != other.ask_price || ask_qty != other.ask_qty;
    }
};

struct Level2Entry {
    Price price;
    Quantity quantity;
};

struct Level2Snapshot {
    std::vector<Level2Entry> bids;
    std::vector<Level2Entry> asks;
};

// ============================================================================
// 2. THE HFT MEMORY POOL
// ============================================================================
class OrderPool {
private:
    std::vector<Order> pool;
    std::vector<size_t> free_indices;

public:
    explicit OrderPool(size_t capacity = 100000) {
        pool.resize(capacity);
        reset(capacity);
    }

    void reset(size_t capacity) {
        free_indices.clear();
        free_indices.reserve(capacity);
        for (int i = static_cast<int>(capacity) - 1; i >= 0; --i) {
            free_indices.push_back(static_cast<size_t>(i));
        }
    }

    void clear() {
        reset(pool.size());
    }

    size_t allocate(OrderId id, Side side, Price price, Quantity qty) {
        if (free_indices.empty()) {
            throw std::runtime_error("CRITICAL: Pool Exhausted!");
        }
        size_t index = free_indices.back();
        free_indices.pop_back();
        pool[index] = {id, side, price, qty, NULL_INDEX, NULL_INDEX};
        return index;
    }

    void deallocate(size_t index) {
        free_indices.push_back(index);
    }

    Order& getOrder(size_t index) {
        return pool[index];
    }
};

// ============================================================================
// 3. THE MATCHING ENGINE CORE
// ============================================================================
class OrderBook {
private:
    OrderPool pool;
    std::map<Price, PriceLevelQueue, std::greater<Price>> bids;
    std::map<Price, PriceLevelQueue> asks;
    std::unordered_map<OrderId, size_t> order_map;

public:
    void clear() {
        bids.clear();
        asks.clear();
        order_map.clear();
        pool.clear();
    }

    // ------------------------------------------------------------------------
    // ADD ORDER (Matching + Intrusive Queue Insertion)
    // ------------------------------------------------------------------------
    std::vector<Trade> addOrder(OrderId id, Side side, Price price, Quantity qty) {
        std::vector<Trade> trades;
        Quantity remaining_qty = qty;

        if (side == Side::BUY) {
            while (remaining_qty > 0 && !asks.empty()) {
                auto best_ask_it = asks.begin();
                if (price < best_ask_it->first) break;

                PriceLevelQueue& queue = best_ask_it->second;
                size_t resting_idx = queue.head_index;
                Order& resting_seller = pool.getOrder(resting_idx);

                Quantity match_qty = std::min(remaining_qty, resting_seller.quantity);
                trades.push_back({id, resting_seller.id, best_ask_it->first, match_qty});

                remaining_qty -= match_qty;
                resting_seller.quantity -= match_qty;
                queue.total_qty -= match_qty;

                if (resting_seller.quantity == 0) {
                    queue.head_index = resting_seller.next_index;
                    if (queue.head_index == NULL_INDEX) {
                        queue.tail_index = NULL_INDEX;
                    } else {
                        pool.getOrder(queue.head_index).prev_index = NULL_INDEX;
                    }
                    order_map.erase(resting_seller.id);
                    pool.deallocate(resting_idx);
                }

                if (queue.empty()) asks.erase(best_ask_it);
            }
        } else {
            while (remaining_qty > 0 && !bids.empty()) {
                auto best_bid_it = bids.begin();
                if (price > best_bid_it->first) break;

                PriceLevelQueue& queue = best_bid_it->second;
                size_t resting_idx = queue.head_index;
                Order& resting_buyer = pool.getOrder(resting_idx);

                Quantity match_qty = std::min(remaining_qty, resting_buyer.quantity);
                trades.push_back({resting_buyer.id, id, best_bid_it->first, match_qty});

                remaining_qty -= match_qty;
                resting_buyer.quantity -= match_qty;
                queue.total_qty -= match_qty;

                if (resting_buyer.quantity == 0) {
                    queue.head_index = resting_buyer.next_index;
                    if (queue.head_index == NULL_INDEX) {
                        queue.tail_index = NULL_INDEX;
                    } else {
                        pool.getOrder(queue.head_index).prev_index = NULL_INDEX;
                    }
                    order_map.erase(resting_buyer.id);
                    pool.deallocate(resting_idx);
                }

                if (queue.empty()) bids.erase(best_bid_it);
            }
        }

        if (remaining_qty > 0) {
            size_t new_idx = pool.allocate(id, side, price, remaining_qty);
            PriceLevelQueue& queue = (side == Side::BUY) ? bids[price] : asks[price];

            if (queue.empty()) {
                queue.head_index = new_idx;
                queue.tail_index = new_idx;
            } else {
                Order& tail_order = pool.getOrder(queue.tail_index);
                tail_order.next_index = new_idx;
                pool.getOrder(new_idx).prev_index = queue.tail_index;
                queue.tail_index = new_idx;
            }

            queue.total_qty += remaining_qty;
            order_map[id] = new_idx;
        }

        return trades;
    }

    // ------------------------------------------------------------------------
    // O(1) CANCEL ORDER
    // ------------------------------------------------------------------------
    bool cancelOrder(OrderId id) {
        auto it = order_map.find(id);
        if (it == order_map.end()) return false;

        size_t target_idx = it->second;
        Order& target = pool.getOrder(target_idx);

        PriceLevelQueue& queue = (target.side == Side::BUY) ? bids[target.price] : asks[target.price];

        if (target.prev_index != NULL_INDEX) {
            pool.getOrder(target.prev_index).next_index = target.next_index;
        } else {
            queue.head_index = target.next_index;
        }

        if (target.next_index != NULL_INDEX) {
            pool.getOrder(target.next_index).prev_index = target.prev_index;
        } else {
            queue.tail_index = target.prev_index;
        }

        queue.total_qty -= target.quantity;

        if (queue.empty()) {
            if (target.side == Side::BUY) bids.erase(target.price);
            else asks.erase(target.price);
        }

        order_map.erase(it);
        pool.deallocate(target_idx);
        return true;
    }

    // ------------------------------------------------------------------------
    // LEVEL 1: BEST BID & OFFER (BBO)
    // ------------------------------------------------------------------------
    BBO getBBO() const {
        BBO bbo;
        if (!bids.empty()) {
            bbo.bid_price = bids.begin()->first;
            bbo.bid_qty = bids.begin()->second.total_qty;
        }
        if (!asks.empty()) {
            bbo.ask_price = asks.begin()->first;
            bbo.ask_qty = asks.begin()->second.total_qty;
        }
        return bbo;
    }

    // ------------------------------------------------------------------------
    // LEVEL 2: MARKET DEPTH SNAPSHOT
    // ------------------------------------------------------------------------
    Level2Snapshot getL2Snapshot(size_t depth = 5) const {
        Level2Snapshot snap;
        snap.bids.reserve(depth);
        snap.asks.reserve(depth);

        size_t count = 0;
        for (const auto& [price, queue] : bids) {
            if (count++ >= depth) break;
            snap.bids.push_back({price, queue.total_qty});
        }

        count = 0;
        for (const auto& [price, queue] : asks) {
            if (count++ >= depth) break;
            snap.asks.push_back({price, queue.total_qty});
        }

        return snap;
    }
};

// ============================================================================
// 4. GOOGLE BENCHMARK SUITE
// ============================================================================

// Benchmark 1: Resting Limit Orders
static void BM_AddOrderResting(benchmark::State& state) {
    OrderBook engine;
    OrderId id = 1;

    for (auto _ : state) {
        state.PauseTiming();
        engine.clear();
        state.ResumeTiming();

        for (int i = 0; i < 1000; ++i) {
            engine.addOrder(id++, Side::BUY, 100, 10);
        }
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_AddOrderResting);

// Benchmark 2: Continuous Trade Matching (Crosses)
static void BM_AddOrderMatching(benchmark::State& state) {
    OrderBook engine;
    OrderId id = 1;

    for (auto _ : state) {
        engine.addOrder(id++, Side::SELL, 100, 10);
        engine.addOrder(id++, Side::BUY, 100, 10);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddOrderMatching);

// Benchmark 3: O(1) Order Cancellations
static void BM_CancelOrder(benchmark::State& state) {
    OrderBook engine;
    OrderId id = 1;

    for (auto _ : state) {
        engine.addOrder(id, Side::BUY, 95, 10);
        engine.cancelOrder(id);
        id++;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CancelOrder);

// Benchmark 4: Level 1 BBO Queries
static void BM_GetBBO(benchmark::State& state) {
    OrderBook engine;
    engine.addOrder(1, Side::BUY, 99, 100);
    engine.addOrder(2, Side::SELL, 101, 100);

    for (auto _ : state) {
        auto bbo = engine.getBBO();
        benchmark::DoNotOptimize(bbo);
    }
}
BENCHMARK(BM_GetBBO);

// Benchmark 5: Level 2 Snapshot Generation (Top 5 Levels)
static void BM_GetL2Snapshot(benchmark::State& state) {
    OrderBook engine;
    for (int i = 0; i < 5; ++i) {
        engine.addOrder(100 + i, Side::BUY, 90 + i, 10);
        engine.addOrder(200 + i, Side::SELL, 110 + i, 10);
    }

    for (auto _ : state) {
        auto snap = engine.getL2Snapshot(5);
        benchmark::DoNotOptimize(snap);
    }
}
BENCHMARK(BM_GetL2Snapshot);

BENCHMARK_MAIN();