#include <benchmark/benchmark.h>
#include <iostream>
#include <cstdint>
#include <vector>
#include <array>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// ============================================================================
// 1. PRIMITIVES & DATA STRUCTURES
// ============================================================================
using Price = uint64_t;
using Quantity = uint32_t;
using OrderId = uint64_t;

enum class Side { BUY, SELL };
enum class TimeInForce { GTC, IOC }; // Good-Til-Cancelled, Immediate-Or-Cancel

constexpr size_t NULL_INDEX = static_cast<size_t>(-1);
constexpr Price MAX_PRICE = 100000; 

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

struct TradeResult {
    const Trade* trades;
    size_t count;
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

    void clear() { reset(pool.size()); }

    size_t allocate(OrderId id, Side side, Price price, Quantity qty) {
        if (free_indices.empty()) throw std::runtime_error("CRITICAL: Pool Exhausted!");
        size_t index = free_indices.back();
        free_indices.pop_back();
        pool[index] = {id, side, price, qty, NULL_INDEX, NULL_INDEX};
        return index;
    }

    void deallocate(size_t index) { free_indices.push_back(index); }
    Order& getOrder(size_t index) { return pool[index]; }
};

// ============================================================================
// 3. THE MATCHING ENGINE CORE (OPTIMIZED)
// ============================================================================
class OrderBook {
private:
    OrderPool pool;
    
    std::array<PriceLevelQueue, MAX_PRICE> bids{};
    std::array<PriceLevelQueue, MAX_PRICE> asks{};
    
    Price best_bid = 0;
    Price best_ask = MAX_PRICE;

    std::unordered_map<OrderId, size_t> order_map;
    std::array<Trade, 1024> trade_buffer{};

    void updateBestBid() {
        while (best_bid > 0 && bids[best_bid].empty()) best_bid--;
    }

    void updateBestAsk() {
        while (best_ask < MAX_PRICE - 1 && asks[best_ask].empty()) best_ask++;
    }

public:
    void clear() {
        bids.fill({});
        asks.fill({});
        order_map.clear();
        best_bid = 0;
        best_ask = MAX_PRICE;
        pool.clear();
    }

    TradeResult addOrder(OrderId id, Side side, Price price, Quantity qty, TimeInForce tif = TimeInForce::GTC) {
        size_t trade_count = 0;
        Quantity remaining_qty = qty;

        if (price >= MAX_PRICE) throw std::out_of_range("Price exceeds MAX_PRICE limit");

        if (side == Side::BUY) {
            while (remaining_qty > 0 && best_ask <= price) {
                PriceLevelQueue& queue = asks[best_ask];
                if (queue.empty()) {
                    best_ask++;
                    continue;
                }

                size_t resting_idx = queue.head_index;
                Order& resting_seller = pool.getOrder(resting_idx);

                Quantity match_qty = std::min(remaining_qty, resting_seller.quantity);
                trade_buffer[trade_count++] = {id, resting_seller.id, best_ask, match_qty};

                remaining_qty -= match_qty;
                resting_seller.quantity -= match_qty;
                queue.total_qty -= match_qty;

                if (resting_seller.quantity == 0) {
                    queue.head_index = resting_seller.next_index;
                    if (queue.head_index == NULL_INDEX) queue.tail_index = NULL_INDEX;
                    else pool.getOrder(queue.head_index).prev_index = NULL_INDEX;
                    
                    order_map.erase(resting_seller.id);
                    pool.deallocate(resting_idx);
                }
            }
            if (remaining_qty > 0 && price > best_bid) best_bid = price;

        } else {
            while (remaining_qty > 0 && best_bid >= price && best_bid > 0) {
                PriceLevelQueue& queue = bids[best_bid];
                if (queue.empty()) {
                    best_bid--;
                    continue;
                }

                size_t resting_idx = queue.head_index;
                Order& resting_buyer = pool.getOrder(resting_idx);

                Quantity match_qty = std::min(remaining_qty, resting_buyer.quantity);
                trade_buffer[trade_count++] = {resting_buyer.id, id, best_bid, match_qty};

                remaining_qty -= match_qty;
                resting_buyer.quantity -= match_qty;
                queue.total_qty -= match_qty;

                if (resting_buyer.quantity == 0) {
                    queue.head_index = resting_buyer.next_index;
                    if (queue.head_index == NULL_INDEX) queue.tail_index = NULL_INDEX;
                    else pool.getOrder(queue.head_index).prev_index = NULL_INDEX;
                    
                    order_map.erase(resting_buyer.id);
                    pool.deallocate(resting_idx);
                }
            }
            if (remaining_qty > 0 && price < best_ask) best_ask = price;
        }

        if (remaining_qty > 0) {
            if (tif == TimeInForce::IOC) {
                // IOC: Unexecuted remainder is cancelled immediately, does not rest on book
                return {trade_buffer.data(), trade_count};
            }

            // GTC: Rest remaining quantity on the book
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

        return {trade_buffer.data(), trade_count};
    }

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
            if (target.side == Side::BUY && target.price == best_bid) updateBestBid();
            else if (target.side == Side::SELL && target.price == best_ask) updateBestAsk();
        }

        order_map.erase(it);
        pool.deallocate(target_idx);
        return true;
    }

    // Order Amendment (Amend): 
    // - Decreasing qty preserves time priority (in-place O(1)).
    // - Increasing qty or changing price acts as cancel + re-add (loses priority, triggers matching if crossing).
    TradeResult amendOrder(OrderId id, Price new_price, Quantity new_qty) {
        auto it = order_map.find(id);
        if (it == order_map.end()) {
            return {nullptr, 0};
        }

        size_t target_idx = it->second;
        Order& target = pool.getOrder(target_idx);

        if (new_price == target.price) {
            if (new_qty < target.quantity) {
                // In-place decrease: preserves time priority
                Quantity diff = target.quantity - new_qty;
                target.quantity = new_qty;
                PriceLevelQueue& queue = (target.side == Side::BUY) ? bids[target.price] : asks[target.price];
                queue.total_qty -= diff;
                return {nullptr, 0};
            } else if (new_qty == target.quantity) {
                return {nullptr, 0}; // No change
            }
        }

        // Price change or quantity increase: cancel old and re-add (loses priority, handles crossing)
        Side side = target.side;
        cancelOrder(id);
        return addOrder(id, side, new_price, new_qty, TimeInForce::GTC);
    }

    BBO getBBO() const {
        BBO bbo;
        if (best_bid > 0) { bbo.bid_price = best_bid; bbo.bid_qty = bids[best_bid].total_qty; }
        if (best_ask < MAX_PRICE) { bbo.ask_price = best_ask; bbo.ask_qty = asks[best_ask].total_qty; }
        return bbo;
    }

    Level2Snapshot getL2Snapshot(size_t depth = 5) const {
        Level2Snapshot snap;
        snap.bids.reserve(depth);
        snap.asks.reserve(depth);

        size_t count = 0;
        for (Price p = best_bid; p > 0 && count < depth; --p) {
            if (!bids[p].empty()) {
                snap.bids.push_back({p, bids[p].total_qty});
                count++;
            }
        }

        count = 0;
        for (Price p = best_ask; p < MAX_PRICE && count < depth; ++p) {
            if (!asks[p].empty()) {
                snap.asks.push_back({p, asks[p].total_qty});
                count++;
            }
        }
        return snap;
    }
};

// ============================================================================
// 4. GOOGLE BENCHMARK SUITE
// ============================================================================
static void BM_AddOrderResting(benchmark::State& state) {
    auto engine_ptr = std::make_unique<OrderBook>();
    OrderBook& engine = *engine_ptr;
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

static void BM_AddOrderMatching(benchmark::State& state) {
    auto engine_ptr = std::make_unique<OrderBook>();
    OrderBook& engine = *engine_ptr;
    OrderId id = 1;

    for (auto _ : state) {
        engine.addOrder(id++, Side::SELL, 100, 10);
        engine.addOrder(id++, Side::BUY, 100, 10);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddOrderMatching);

static void BM_CancelOrder(benchmark::State& state) {
    auto engine_ptr = std::make_unique<OrderBook>();
    OrderBook& engine = *engine_ptr;
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

static void BM_AmendOrder(benchmark::State& state) {
    auto engine_ptr = std::make_unique<OrderBook>();
    OrderBook& engine = *engine_ptr;
    OrderId id = 1;

    for (auto _ : state) {
        engine.addOrder(id, Side::BUY, 95, 10);
        engine.amendOrder(id, 95, 5); // Quantity decrease (in-place priority preserve)
        engine.cancelOrder(id);
        id++;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AmendOrder);

static void BM_GetBBO(benchmark::State& state) {
    auto engine_ptr = std::make_unique<OrderBook>();
    OrderBook& engine = *engine_ptr;
    engine.addOrder(1, Side::BUY, 99, 100);
    engine.addOrder(2, Side::SELL, 101, 100);

    for (auto _ : state) {
        auto bbo = engine.getBBO();
        benchmark::DoNotOptimize(bbo);
    }
}
BENCHMARK(BM_GetBBO);

// ============================================================================
// 5. TAIL LATENCY HARNESS (Using __rdtsc) - ALL OPERATIONS
// ============================================================================
static double calibrateCpuNsPerCycle() {
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t c1 = __rdtsc();
    
    volatile int dummy = 0;
    auto target = t1 + std::chrono::milliseconds(5);
    while (std::chrono::high_resolution_clock::now() < target) {
        dummy++;
    }
    
    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t c2 = __rdtsc();
    
    auto ns_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    uint64_t cycles_elapsed = c2 - c1;
    
    return static_cast<double>(ns_elapsed) / static_cast<double>(cycles_elapsed);
}

static void printLatencyReport(const std::string& label, std::vector<uint64_t>& samples_cycles, double ns_per_cycle) {
    std::sort(samples_cycles.begin(), samples_cycles.end());
    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * (samples_cycles.size() - 1));
        return samples_cycles[idx];
    };
    
    auto to_ns = [&](uint64_t cycles) { return static_cast<uint64_t>(cycles * ns_per_cycle); };

    std::cout << "\n--- " << label << " (" << samples_cycles.size() << " samples) ---\n";
    std::cout << "  p50:    " << to_ns(pct(0.50)) << " ns (" << pct(0.50) << " cycles)\n";
    std::cout << "  p90:    " << to_ns(pct(0.90)) << " ns\n";
    std::cout << "  p99:    " << to_ns(pct(0.99)) << " ns\n";
    std::cout << "  p99.9:  " << to_ns(pct(0.999)) << " ns\n";
    std::cout << "  max:    " << to_ns(samples_cycles.back()) << " ns\n";
}

static void runLatencyPercentileHarness() {
    double ns_per_cycle = calibrateCpuNsPerCycle();
    constexpr int WARMUP = 5000;
    constexpr int SAMPLES = 50000;

    // 1. addOrder - resting (no match)
    {
        auto book = std::make_unique<OrderBook>();
        std::vector<uint64_t> samples;
        samples.reserve(SAMPLES);
        OrderId id = 1;

        for (int i = 0; i < WARMUP; ++i) {
            book->addOrder(id++, Side::BUY, 50, 10);
        }
        book->clear();
        id = 1;

        for (int i = 0; i < SAMPLES; ++i) {
            uint64_t t0 = __rdtsc();
            book->addOrder(id++, Side::BUY, 50, 10);
            uint64_t t1 = __rdtsc();
            samples.push_back(t1 - t0);
        }
        printLatencyReport("addOrder - resting (no match)", samples, ns_per_cycle);
    }

    // 2. addOrder - matching (taker)
    {
        auto book = std::make_unique<OrderBook>();
        std::vector<uint64_t> samples;
        samples.reserve(SAMPLES);
        OrderId id = 1;

        for (int i = 0; i < WARMUP; ++i) {
            book->addOrder(id++, Side::SELL, 100, 10);
            book->addOrder(id++, Side::BUY, 100, 10);
        }

        for (int i = 0; i < SAMPLES; ++i) {
            book->addOrder(id++, Side::SELL, 100, 10);
            uint64_t t0 = __rdtsc();
            book->addOrder(id++, Side::BUY, 100, 10);
            uint64_t t1 = __rdtsc();
            samples.push_back(t1 - t0);
        }
        printLatencyReport("addOrder - matching (taker)", samples, ns_per_cycle);
    }

    // 3. cancelOrder
    {
        auto book = std::make_unique<OrderBook>();
        std::vector<uint64_t> samples;
        samples.reserve(SAMPLES);
        OrderId id = 1;

        for (int i = 0; i < WARMUP; ++i) {
            book->addOrder(id, Side::BUY, 50, 10);
            book->cancelOrder(id);
            id++;
        }

        for (int i = 0; i < SAMPLES; ++i) {
            book->addOrder(id, Side::BUY, 50, 10);
            uint64_t t0 = __rdtsc();
            book->cancelOrder(id);
            uint64_t t1 = __rdtsc();
            samples.push_back(t1 - t0);
            id++;
        }
        printLatencyReport("cancelOrder", samples, ns_per_cycle);
    }

    // 4. amendOrder (qty decrease - in-place)
    {
        auto book = std::make_unique<OrderBook>();
        std::vector<uint64_t> samples;
        samples.reserve(SAMPLES);
        OrderId id = 1;

        for (int i = 0; i < WARMUP; ++i) {
            book->addOrder(id, Side::BUY, 50, 10);
            book->amendOrder(id, 50, 5);
            book->cancelOrder(id);
            id++;
        }

        for (int i = 0; i < SAMPLES; ++i) {
            book->addOrder(id, Side::BUY, 50, 10);
            uint64_t t0 = __rdtsc();
            book->amendOrder(id, 50, 5);
            uint64_t t1 = __rdtsc();
            samples.push_back(t1 - t0);
            book->cancelOrder(id);
            id++;
        }
        printLatencyReport("amendOrder (qty decrease - in-place)", samples, ns_per_cycle);
    }
}

// ============================================================================
// 6. MAIN
// ============================================================================
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    std::cout << "\n\n============ Tail Latency Report (single-op, __rdtsc) ============\n";
    runLatencyPercentileHarness();

    return 0;
}