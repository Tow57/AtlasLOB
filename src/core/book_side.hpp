#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "price_level.hpp"

namespace atlaslob::core {

enum class BookSideError : std::uint8_t {
  none = 0,
  invalid_price = 1,
  level_not_found = 2,
  level_identity_mismatch = 3,
  level_not_empty = 4,
  level_invariant_violation = 5,
  prepared_level_empty = 6,
  prepared_level_conflict = 7,
};

[[nodiscard]] constexpr std::string_view to_string(BookSideError error) noexcept {
  switch (error) {
    case BookSideError::none:
      return "none";
    case BookSideError::invalid_price:
      return "invalid_price";
    case BookSideError::level_not_found:
      return "level_not_found";
    case BookSideError::level_identity_mismatch:
      return "level_identity_mismatch";
    case BookSideError::level_not_empty:
      return "level_not_empty";
    case BookSideError::level_invariant_violation:
      return "level_invariant_violation";
    case BookSideError::prepared_level_empty:
      return "prepared_level_empty";
    case BookSideError::prepared_level_conflict:
      return "prepared_level_conflict";
  }
  return "unknown";
}

enum class BookSideInvariantError : std::uint8_t {
  none = 0,
  invalid_price = 1,
  nonmonotonic_price = 2,
  null_level = 3,
  key_price_mismatch = 4,
  empty_level = 5,
  level_invariant_violation = 6,
};

[[nodiscard]] constexpr std::string_view to_string(BookSideInvariantError error) noexcept {
  switch (error) {
    case BookSideInvariantError::none:
      return "none";
    case BookSideInvariantError::invalid_price:
      return "invalid_price";
    case BookSideInvariantError::nonmonotonic_price:
      return "nonmonotonic_price";
    case BookSideInvariantError::null_level:
      return "null_level";
    case BookSideInvariantError::key_price_mismatch:
      return "key_price_mismatch";
    case BookSideInvariantError::empty_level:
      return "empty_level";
    case BookSideInvariantError::level_invariant_violation:
      return "level_invariant_violation";
  }
  return "unknown";
}

struct BookSideInvariantResult final {
  BookSideInvariantError error{BookSideInvariantError::none};
  domain::PriceTicks price{};
  const PriceLevel* level{};
  PriceLevelInvariantResult level_result{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return error == BookSideInvariantError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  bool operator==(const BookSideInvariantResult&) const = default;
};

namespace detail {

template <domain::Side RestingSide>
struct BestPriceFirst final {
  [[nodiscard]] constexpr bool operator()(domain::PriceTicks left,
                                          domain::PriceTicks right) const noexcept {
    if constexpr (RestingSide == domain::Side::buy) {
      return left > right;
    } else {
      return left < right;
    }
  }
};

}  // namespace detail

template <domain::Side RestingSide>
class BookSide final {
  static_assert(RestingSide == domain::Side::buy || RestingSide == domain::Side::sell);

 private:
  using Levels = std::map<domain::PriceTicks, std::unique_ptr<PriceLevel>,
                          detail::BestPriceFirst<RestingSide>>;

 public:
  class PreparedLevel final {
   public:
    PreparedLevel(const PreparedLevel&) = delete;
    PreparedLevel& operator=(const PreparedLevel&) = delete;

    PreparedLevel(PreparedLevel&& other) noexcept
        : owner_{std::exchange(other.owner_, nullptr)},
          level_{std::exchange(other.level_, nullptr)},
          created_{std::exchange(other.created_, false)},
          error_{std::exchange(other.error_, BookSideError::none)} {}

    PreparedLevel& operator=(PreparedLevel&& other) noexcept {
      if (this != &other) {
        rollback();
        owner_ = std::exchange(other.owner_, nullptr);
        level_ = std::exchange(other.level_, nullptr);
        created_ = std::exchange(other.created_, false);
        error_ = std::exchange(other.error_, BookSideError::none);
      }
      return *this;
    }

    ~PreparedLevel() noexcept { rollback(); }

    [[nodiscard]] PriceLevel* level() const noexcept { return level_; }
    [[nodiscard]] bool created() const noexcept { return created_; }
    [[nodiscard]] BookSideError error() const noexcept { return error_; }

    [[nodiscard]] bool has_value() const noexcept {
      return level_ != nullptr && error_ == BookSideError::none;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] BookSideError commit() noexcept {
      if (error_ != BookSideError::none) {
        return error_;
      }
      if (owner_ == nullptr || level_ == nullptr) {
        return BookSideError::level_invariant_violation;
      }
      if (level_->empty()) {
        return BookSideError::prepared_level_empty;
      }
      if (!level_->validate_invariants()) {
        return BookSideError::level_invariant_violation;
      }

      owner_ = nullptr;
      level_ = nullptr;
      created_ = false;
      return BookSideError::none;
    }

   private:
    friend class BookSide;

    PreparedLevel(BookSide* owner, PriceLevel* level, bool created, BookSideError error) noexcept
        : owner_{owner}, level_{level}, created_{created}, error_{error} {}

    void rollback() noexcept {
      if (owner_ != nullptr && created_) {
        if (level_ == nullptr || !level_->empty() ||
            owner_->erase_level(*level_) != BookSideError::none) {
          std::terminate();
        }
      }
      owner_ = nullptr;
      level_ = nullptr;
      created_ = false;
    }

    BookSide* owner_{};
    PriceLevel* level_{};
    bool created_{};
    BookSideError error_{BookSideError::none};
  };

  // Owns a fully allocated std::map node that is deliberately detached from the
  // visible side.  It is the allocation-free fallback used by a higher-level
  // transaction when the target price level does not exist at commit time.
  class DetachedLevel final {
   public:
    DetachedLevel(const DetachedLevel&) = delete;
    DetachedLevel& operator=(const DetachedLevel&) = delete;

    DetachedLevel(DetachedLevel&& other) noexcept
        : owner_{std::exchange(other.owner_, nullptr)},
          node_{std::move(other.node_)},
          error_{std::exchange(other.error_, BookSideError::none)} {}

    DetachedLevel& operator=(DetachedLevel&& other) noexcept {
      if (this != &other) {
        owner_ = std::exchange(other.owner_, nullptr);
        node_ = std::move(other.node_);
        error_ = std::exchange(other.error_, BookSideError::none);
      }
      return *this;
    }

    ~DetachedLevel() = default;

    [[nodiscard]] PriceLevel* level() const noexcept {
      return node_.empty() ? nullptr : node_.mapped().get();
    }

    [[nodiscard]] BookSideError error() const noexcept { return error_; }

    [[nodiscard]] bool has_value() const noexcept {
      return owner_ != nullptr && !node_.empty() && level() != nullptr &&
             error_ == BookSideError::none;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    // No allocation occurs here: the map node was allocated by
    // prepare_detached_level(). A conflict indicates that the caller violated
    // its commit-time preflight contract.
    [[nodiscard]] BookSideError publish() noexcept {
      if (error_ != BookSideError::none) {
        return error_;
      }
      if (owner_ == nullptr || node_.empty() || level() == nullptr) {
        return BookSideError::level_invariant_violation;
      }
      if (level()->empty()) {
        return BookSideError::prepared_level_empty;
      }
      if (!level()->validate_invariants()) {
        return BookSideError::level_invariant_violation;
      }
      if (owner_->levels_.contains(node_.key())) {
        return BookSideError::prepared_level_conflict;
      }

      auto inserted = owner_->levels_.insert(std::move(node_));
      if (!inserted.inserted) {
        node_ = std::move(inserted.node);
        return BookSideError::prepared_level_conflict;
      }

      owner_ = nullptr;
      return BookSideError::none;
    }

   private:
    friend class BookSide;

    DetachedLevel(BookSide* owner, typename Levels::node_type node, BookSideError error) noexcept
        : owner_{owner}, node_{std::move(node)}, error_{error} {}

    BookSide* owner_{};
    typename Levels::node_type node_;
    BookSideError error_{BookSideError::none};
  };

  class const_iterator final {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = PriceLevel;
    using difference_type = std::ptrdiff_t;
    using pointer = const PriceLevel*;
    using reference = const PriceLevel&;

    const_iterator() = default;

    [[nodiscard]] reference operator*() const noexcept { return *position_->second; }
    [[nodiscard]] pointer operator->() const noexcept { return position_->second.get(); }

    const_iterator& operator++() noexcept {
      ++position_;
      return *this;
    }

    const_iterator operator++(int) noexcept {
      auto previous = *this;
      ++(*this);
      return previous;
    }

    const_iterator& operator--() noexcept {
      --position_;
      return *this;
    }

    const_iterator operator--(int) noexcept {
      auto previous = *this;
      --(*this);
      return previous;
    }

    bool operator==(const const_iterator&) const = default;

   private:
    friend class BookSide;

    explicit const_iterator(typename Levels::const_iterator position) noexcept
        : position_{position} {}

    typename Levels::const_iterator position_{};
  };

  BookSide() = default;
  BookSide(const BookSide&) = delete;
  BookSide& operator=(const BookSide&) = delete;
  BookSide(BookSide&&) = delete;
  BookSide& operator=(BookSide&&) = delete;
  ~BookSide() = default;

  [[nodiscard]] static constexpr domain::Side side() noexcept { return RestingSide; }
  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
  [[nodiscard]] std::size_t level_count() const noexcept { return levels_.size(); }

  [[nodiscard]] PriceLevel* best_level() noexcept {
    return levels_.empty() ? nullptr : levels_.begin()->second.get();
  }

  [[nodiscard]] const PriceLevel* best_level() const noexcept {
    return levels_.empty() ? nullptr : levels_.begin()->second.get();
  }

  [[nodiscard]] PriceLevel* find_level(domain::PriceTicks price) noexcept {
    const auto position = levels_.find(price);
    return position == levels_.end() ? nullptr : position->second.get();
  }

  [[nodiscard]] const PriceLevel* find_level(domain::PriceTicks price) const noexcept {
    const auto position = levels_.find(price);
    return position == levels_.end() ? nullptr : position->second.get();
  }

  [[nodiscard]] PreparedLevel prepare_level(domain::PriceTicks price) {
    if (price.value() <= 0) {
      return PreparedLevel{nullptr, nullptr, false, BookSideError::invalid_price};
    }

    if (const auto position = levels_.find(price); position != levels_.end()) {
      if (position->second == nullptr || position->second->price() != price ||
          position->second->empty() || !position->second->validate_invariants()) {
        return PreparedLevel{nullptr, nullptr, false, BookSideError::level_invariant_violation};
      }
      return PreparedLevel{this, position->second.get(), false, BookSideError::none};
    }

    auto level = std::make_unique<PriceLevel>(price);
    auto* const address = level.get();
    const auto [position, inserted] = levels_.try_emplace(price, std::move(level));
    if (!inserted) {
      if (position->second == nullptr || position->second->price() != price ||
          position->second->empty() || !position->second->validate_invariants()) {
        return PreparedLevel{nullptr, nullptr, false, BookSideError::level_invariant_violation};
      }
      return PreparedLevel{this, position->second.get(), false, BookSideError::none};
    }
    return PreparedLevel{this, address, true, BookSideError::none};
  }

  [[nodiscard]] DetachedLevel prepare_detached_level(domain::PriceTicks price) {
    if (price.value() <= 0) {
      return DetachedLevel{nullptr, {}, BookSideError::invalid_price};
    }
    if (!validate_invariants()) {
      return DetachedLevel{nullptr, {}, BookSideError::level_invariant_violation};
    }

    Levels detached;
    auto level = std::make_unique<PriceLevel>(price);
    const auto [position, inserted] = detached.try_emplace(price, std::move(level));
    if (!inserted) {
      return DetachedLevel{nullptr, {}, BookSideError::prepared_level_conflict};
    }
    return DetachedLevel{this, detached.extract(position), BookSideError::none};
  }

  [[nodiscard]] BookSideError erase_level(PriceLevel& level) noexcept {
    if (level.price().value() <= 0) {
      return BookSideError::invalid_price;
    }

    const auto position = levels_.find(level.price());
    if (position == levels_.end()) {
      return BookSideError::level_not_found;
    }
    if (position->second.get() != &level) {
      return BookSideError::level_identity_mismatch;
    }
    if (!level.validate_invariants()) {
      return BookSideError::level_invariant_violation;
    }
    if (!level.empty()) {
      return BookSideError::level_not_empty;
    }

    levels_.erase(position);
    return BookSideError::none;
  }

  [[nodiscard]] const_iterator begin() const noexcept { return const_iterator{levels_.cbegin()}; }
  [[nodiscard]] const_iterator end() const noexcept { return const_iterator{levels_.cend()}; }
  [[nodiscard]] const_iterator cbegin() const noexcept { return const_iterator{levels_.cbegin()}; }
  [[nodiscard]] const_iterator cend() const noexcept { return const_iterator{levels_.cend()}; }

  [[nodiscard]] BookSideInvariantResult validate_invariants() const noexcept {
    BookSideInvariantResult result{};
    const auto fail = [&result](BookSideInvariantError error, domain::PriceTicks price,
                                const PriceLevel* level = nullptr,
                                PriceLevelInvariantResult level_result = {}) noexcept {
      result.error = error;
      result.price = price;
      result.level = level;
      result.level_result = level_result;
      return result;
    };

    domain::PriceTicks previous_price{};
    bool has_previous = false;
    for (const auto& [price, level] : levels_) {
      if (price.value() <= 0) {
        return fail(BookSideInvariantError::invalid_price, price, level.get());
      }
      if (has_previous && !detail::BestPriceFirst<RestingSide>{}(previous_price, price)) {
        return fail(BookSideInvariantError::nonmonotonic_price, price, level.get());
      }
      if (level == nullptr) {
        return fail(BookSideInvariantError::null_level, price);
      }
      if (level->price() != price) {
        return fail(BookSideInvariantError::key_price_mismatch, price, level.get());
      }
      if (level->empty()) {
        return fail(BookSideInvariantError::empty_level, price, level.get());
      }

      const auto level_result = level->validate_invariants();
      if (!level_result) {
        return fail(BookSideInvariantError::level_invariant_violation, price, level.get(),
                    level_result);
      }

      previous_price = price;
      has_previous = true;
    }

    return result;
  }

 private:
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  friend class test::CoreAccess;
#endif

  Levels levels_;
};

using BidBookSide = BookSide<domain::Side::buy>;
using AskBookSide = BookSide<domain::Side::sell>;

}  // namespace atlaslob::core
