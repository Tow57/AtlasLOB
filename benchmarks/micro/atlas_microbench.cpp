#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/types.hpp"
#include "atlaslob/matching_engine.hpp"
#include "atlaslob/multi_instrument_engine.hpp"

namespace {

using atlaslob::EngineResult;
using atlaslob::MatchingEngine;
using atlaslob::MultiInstrumentEngine;
using atlaslob::domain::CancelOrder;
using atlaslob::domain::ClientId;
using atlaslob::domain::InstrumentId;
using atlaslob::domain::NewOrder;
using atlaslob::domain::OrderId;
using atlaslob::domain::OrderType;
using atlaslob::domain::PriceTicks;
using atlaslob::domain::Quantity;
using atlaslob::domain::ReplaceOrder;
using atlaslob::domain::Side;
using atlaslob::domain::TimeInForce;

constexpr ClientId client_id{11U};
constexpr InstrumentId instrument_id{7U};
constexpr PriceTicks bid_price{10'000};
constexpr PriceTicks ask_price{10'001};
constexpr Quantity unit_quantity{1U};

[[nodiscard]] NewOrder limit_order(OrderId order_id, Side side, PriceTicks price) {
  return NewOrder{
      client_id,        order_id,         instrument_id, side,
      OrderType::limit, TimeInForce::gtc, price,         unit_quantity,
  };
}

[[nodiscard]] NewOrder market_ioc(OrderId order_id, InstrumentId routed_instrument) {
  return NewOrder{
      client_id,         order_id,         routed_instrument, Side::buy,
      OrderType::market, TimeInForce::ioc, std::nullopt,      unit_quantity,
  };
}

[[nodiscard]] bool observe_committed(benchmark::State& state, const EngineResult& result) {
  if (!result.committed()) {
    state.SkipWithError("public engine command did not commit");
    return false;
  }
  benchmark::DoNotOptimize(result.batch());
  return true;
}

void PublicEngineHotLevelAddCancel(benchmark::State& state) {
  std::int64_t completed_items = 0;
  std::optional<MatchingEngine> engine;
  std::uint64_t next_order_id = 2U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (!engine.has_value()) {
      state.PauseTiming();
      engine.emplace(instrument_id);
      auto anchor = engine->execute(limit_order(OrderId{1U}, Side::buy, bid_price));
      const bool initialized = observe_committed(state, anchor);
      state.ResumeTiming();
      if (!initialized) {
        break;
      }
    }

    const OrderId order_id{next_order_id++};
    auto added = engine->execute(limit_order(order_id, Side::buy, bid_price));
    if (!observe_committed(state, added)) {
      break;
    }
    ++completed_items;

    auto canceled = engine->execute(CancelOrder{client_id, order_id, instrument_id});
    if (!observe_committed(state, canceled)) {
      break;
    }
    ++completed_items;
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(completed_items);
}

void PublicEngineCrossOneLevel(benchmark::State& state) {
  std::int64_t completed_items = 0;
  std::optional<MatchingEngine> engine;
  std::uint64_t next_order_id = 1U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (!engine.has_value()) {
      state.PauseTiming();
      engine.emplace(instrument_id);
      state.ResumeTiming();
    }

    auto rested = engine->execute(limit_order(OrderId{next_order_id++}, Side::sell, ask_price));
    if (!observe_committed(state, rested)) {
      break;
    }
    ++completed_items;

    auto crossed = engine->execute(market_ioc(OrderId{next_order_id++}, instrument_id));
    if (!observe_committed(state, crossed)) {
      break;
    }
    ++completed_items;
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(completed_items);
}

void PublicEngineReplacePriority(benchmark::State& state) {
  std::int64_t completed_items = 0;
  std::optional<MatchingEngine> engine;
  OrderId active_order_id{1U};
  std::uint64_t next_order_id = 2U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (!engine.has_value()) {
      state.PauseTiming();
      engine.emplace(instrument_id);
      auto initial = engine->execute(limit_order(active_order_id, Side::buy, bid_price));
      const bool initialized = observe_committed(state, initial);
      state.ResumeTiming();
      if (!initialized) {
        break;
      }
    }

    const OrderId replacement_id{next_order_id++};
    auto replaced = engine->execute(ReplaceOrder{
        client_id,
        active_order_id,
        replacement_id,
        instrument_id,
        bid_price,
        unit_quantity,
    });
    if (!observe_committed(state, replaced)) {
      break;
    }
    ++completed_items;
    active_order_id = replacement_id;
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(completed_items);
}

void PublicEngineMultiInstrumentRoute(benchmark::State& state) {
  std::int64_t completed_items = 0;
  constexpr std::uint32_t instrument_count = 16U;
  std::optional<MultiInstrumentEngine> engine;
  std::uint64_t next_order_id = 1U;
  std::uint32_t next_instrument = 1U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (!engine.has_value()) {
      state.PauseTiming();
      std::vector<atlaslob::InstrumentConfig> catalog;
      catalog.reserve(instrument_count);
      for (std::uint32_t value = 1U; value <= instrument_count; ++value) {
        catalog.push_back(atlaslob::InstrumentConfig{InstrumentId{value}, {}});
      }
      engine.emplace(catalog);
      state.ResumeTiming();
    }

    auto routed =
        engine->execute(market_ioc(OrderId{next_order_id++}, InstrumentId{next_instrument}));
    if (!observe_committed(state, routed)) {
      break;
    }
    ++completed_items;
    next_instrument = next_instrument == instrument_count ? 1U : next_instrument + 1U;
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(completed_items);
}

BENCHMARK(PublicEngineHotLevelAddCancel)->UseRealTime()->Unit(benchmark::kNanosecond);
BENCHMARK(PublicEngineCrossOneLevel)->UseRealTime()->Unit(benchmark::kNanosecond);
BENCHMARK(PublicEngineReplacePriority)->UseRealTime()->Unit(benchmark::kNanosecond);
BENCHMARK(PublicEngineMultiInstrumentRoute)->UseRealTime()->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
