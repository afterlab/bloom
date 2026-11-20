#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bloom::document {

// #744. A composition's authored records (animation curves, parameters, graph nodes, edges and
// layer records) are each held in their own immutable allocation. Published snapshots share one
// immutable list of record pointers; a draft copies that list of pointers when it first edits the
// table, and copies a record only when it edits that record. Publication copies the records a
// draft made (so a reference retained from the draft can never reach a snapshot) and freezes the
// list. Two tables therefore share every record neither edited, an edit copies only what it
// touches, and undo history holds each record once however many snapshots reach it.
//
// The table is read like a contiguous range of records: RecordView iterates them in order and
// indexes them, so `for (const auto& node : graph.nodes())`, `std::ranges::find` and `.size()`
// read as they did when the records were a vector.
template <typename T> class RecordView;

template <typename T> class RecordIterator final {
  public:
    using iterator_concept = std::random_access_iterator_tag;
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    RecordIterator() noexcept = default;
    explicit RecordIterator(const std::shared_ptr<T>* slot) noexcept : slot_(slot) {}

    [[nodiscard]] reference operator*() const noexcept { return **slot_; }
    [[nodiscard]] pointer operator->() const noexcept { return slot_->get(); }
    [[nodiscard]] reference operator[](const difference_type offset) const noexcept {
        return *slot_[offset];
    }
    RecordIterator& operator++() noexcept {
        ++slot_;
        return *this;
    }
    RecordIterator operator++(int) noexcept {
        auto copy = *this;
        ++slot_;
        return copy;
    }
    RecordIterator& operator--() noexcept {
        --slot_;
        return *this;
    }
    RecordIterator operator--(int) noexcept {
        auto copy = *this;
        --slot_;
        return copy;
    }
    RecordIterator& operator+=(const difference_type offset) noexcept {
        slot_ += offset;
        return *this;
    }
    RecordIterator& operator-=(const difference_type offset) noexcept {
        slot_ -= offset;
        return *this;
    }
    [[nodiscard]] friend RecordIterator operator+(RecordIterator it,
                                                  const difference_type offset) noexcept {
        return it += offset;
    }
    [[nodiscard]] friend RecordIterator operator+(const difference_type offset,
                                                  RecordIterator it) noexcept {
        return it += offset;
    }
    [[nodiscard]] friend RecordIterator operator-(RecordIterator it,
                                                  const difference_type offset) noexcept {
        return it -= offset;
    }
    [[nodiscard]] friend difference_type operator-(const RecordIterator& left,
                                                   const RecordIterator& right) noexcept {
        return left.slot_ - right.slot_;
    }
    [[nodiscard]] friend bool operator==(const RecordIterator&,
                                         const RecordIterator&) noexcept = default;
    [[nodiscard]] friend auto operator<=>(const RecordIterator&,
                                          const RecordIterator&) noexcept = default;

    // The shared allocation this position holds: two tables hold the same record exactly when
    // these compare equal.
    [[nodiscard]] const void* identity() const noexcept { return slot_->get(); }

  private:
    const std::shared_ptr<T>* slot_ = nullptr;
};

// A read-only, random-access view of a RecordTable's records, valid until the table changes.
template <typename T> class RecordView final {
  public:
    using value_type = T;
    using iterator = RecordIterator<T>;
    using const_iterator = RecordIterator<T>;
    using reverse_iterator = std::reverse_iterator<RecordIterator<T>>;
    using size_type = std::size_t;

    RecordView() noexcept = default;
    RecordView(const std::shared_ptr<T>* first, const std::size_t count) noexcept
        : first_(first), count_(count) {}

    [[nodiscard]] iterator begin() const noexcept { return iterator(first_); }
    [[nodiscard]] iterator end() const noexcept { return iterator(first_ + count_); }
    [[nodiscard]] std::reverse_iterator<iterator> rbegin() const noexcept {
        return std::reverse_iterator<iterator>(end());
    }
    [[nodiscard]] std::reverse_iterator<iterator> rend() const noexcept {
        return std::reverse_iterator<iterator>(begin());
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] const T& operator[](const std::size_t index) const noexcept {
        return *first_[index];
    }
    [[nodiscard]] const T& front() const noexcept { return *first_[0]; }
    [[nodiscard]] const T& back() const noexcept { return *first_[count_ - 1]; }
    // The shared allocation of record `index`.
    [[nodiscard]] const void* identity(const std::size_t index) const noexcept {
        return first_[index].get();
    }
    // A share of record `index`'s allocation, to keep it (and its identity) alive.
    [[nodiscard]] std::shared_ptr<const T> share(const std::size_t index) const noexcept {
        return first_[index];
    }

  private:
    const std::shared_ptr<T>* first_ = nullptr;
    std::size_t count_ = 0;
};

template <typename T> class RecordTable final {
  public:
    RecordTable() : frozen_(emptyFrozen()) {}
    RecordTable(const RecordTable& other) : frozen_(other.frozen_) {
        if (other.editable_) {
            editable_ = std::make_unique<Editable>(*other.editable_);
            // The records the other table made are its own: this copy gets its own copies, so
            // a reference into one can never write into the other.
            for (std::size_t index = 0; index < editable_->records.size(); ++index)
                if (editable_->owned[index] != 0)
                    editable_->records[index] = std::make_shared<T>(*editable_->records[index]);
        }
    }
    RecordTable& operator=(const RecordTable& other) {
        if (this != &other) {
            auto copy = other;
            *this = std::move(copy);
        }
        return *this;
    }
    RecordTable(RecordTable&&) noexcept = default;
    RecordTable& operator=(RecordTable&&) noexcept = default;
    ~RecordTable() = default;

    [[nodiscard]] RecordView<T> view() const noexcept {
        const auto& records = recordList();
        return {records.data(), records.size()};
    }
    [[nodiscard]] std::size_t size() const noexcept { return recordList().size(); }
    [[nodiscard]] bool empty() const noexcept { return recordList().empty(); }
    [[nodiscard]] const T& operator[](const std::size_t index) const noexcept {
        return *recordList()[index];
    }
    [[nodiscard]] const void* recordIdentity(const std::size_t index) const noexcept {
        return recordList()[index].get();
    }
    // Two tables share their whole record list exactly when neither edited it since they diverged.
    [[nodiscard]] const void* identity() const noexcept { return &recordList(); }

    // The record at `index`, made this table's own first when another table shares it.
    [[nodiscard]] T& edit(const std::size_t index) {
        auto& editable = detach();
        if (editable.owned[index] == 0) {
            editable.records[index] = std::make_shared<T>(*editable.records[index]);
            editable.owned[index] = 1;
        }
        return *editable.records[index];
    }
    // Puts `record` at `index` in place of the record there, without copying the old one.
    T& replace(const std::size_t index, T record) {
        auto& editable = detach();
        editable.records[index] = std::make_shared<T>(std::move(record));
        editable.owned[index] = 1;
        return *editable.records[index];
    }
    T& insert(const std::size_t index, T record) {
        auto& editable = detach();
        auto made = std::make_shared<T>(std::move(record));
        editable.records.insert(editable.records.begin() + static_cast<std::ptrdiff_t>(index),
                                made);
        editable.owned.insert(editable.owned.begin() + static_cast<std::ptrdiff_t>(index), 1);
        return *made;
    }
    T& push_back(T record) { return insert(size(), std::move(record)); }
    void erase(const std::size_t index) {
        auto& editable = detach();
        editable.records.erase(editable.records.begin() + static_cast<std::ptrdiff_t>(index));
        editable.owned.erase(editable.owned.begin() + static_cast<std::ptrdiff_t>(index));
    }
    // Erases every record `remove` selects; returns how many. A table nothing matches stays shared.
    template <typename Predicate> std::size_t eraseIf(Predicate&& remove) {
        const auto& records = recordList();
        if (std::none_of(records.begin(), records.end(),
                         [&](const auto& record) { return remove(std::as_const(*record)); }))
            return 0;
        auto& editable = detach();
        std::size_t kept = 0;
        const auto count = editable.records.size();
        for (std::size_t index = 0; index < count; ++index) {
            if (remove(std::as_const(*editable.records[index])))
                continue;
            if (kept != index) {
                editable.records[kept] = std::move(editable.records[index]);
                editable.owned[kept] = editable.owned[index];
            }
            ++kept;
        }
        editable.records.resize(kept);
        editable.owned.resize(kept);
        return count - kept;
    }

    // Called only on a publication copy, so a reference retained from the draft cannot reach it.
    void freeze() {
        if (editable_) {
            frozen_ = std::make_shared<const std::vector<std::shared_ptr<T>>>(
                std::move(editable_->records));
            editable_.reset();
        }
    }

  private:
    struct Editable final {
        std::vector<std::shared_ptr<T>> records;
        // 1 where this table made the record and nothing else holds it.
        std::vector<std::uint8_t> owned;
    };
    [[nodiscard]] static std::shared_ptr<const std::vector<std::shared_ptr<T>>> emptyFrozen() {
        static const auto empty = std::make_shared<const std::vector<std::shared_ptr<T>>>();
        return empty;
    }
    [[nodiscard]] const std::vector<std::shared_ptr<T>>& recordList() const noexcept {
        return editable_ ? editable_->records : *frozen_;
    }
    Editable& detach() {
        if (!editable_) {
            auto editable = std::make_unique<Editable>();
            editable->records = *frozen_;
            editable->owned.assign(editable->records.size(), 0);
            editable_ = std::move(editable);
        }
        return *editable_;
    }

    std::shared_ptr<const std::vector<std::shared_ptr<T>>> frozen_;
    std::unique_ptr<Editable> editable_;
};

// The records two versions of one table do not share: `removed` holds the earlier version's records
// the later one dropped or replaced, `added` the later version's records the earlier one did not
// hold. A replaced record appears in both. The cost is one pointer comparison per record, and
// nothing when the two versions still share their record list.
template <typename T> struct RecordDiff final {
    std::vector<const T*> removed;
    std::vector<const T*> added;
    [[nodiscard]] bool empty() const noexcept { return removed.empty() && added.empty(); }
};

template <typename T>
[[nodiscard]] RecordDiff<T> diffRecords(const RecordView<T> before, const RecordView<T> after) {
    RecordDiff<T> diff;
    if (before.size() == after.size()) {
        // The usual edit replaces records in place: compare position by position.
        for (std::size_t index = 0; index < before.size(); ++index)
            if (before.identity(index) != after.identity(index)) {
                diff.removed.push_back(&before[index]);
                diff.added.push_back(&after[index]);
            }
        return diff;
    }
    // Inserted or erased records: skip the shared prefix and suffix, then match the middle.
    std::size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() &&
           before.identity(prefix) == after.identity(prefix))
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before.identity(before.size() - 1 - suffix) == after.identity(after.size() - 1 - suffix))
        ++suffix;
    std::unordered_set<const void*> held;
    held.reserve(before.size() - prefix - suffix);
    for (std::size_t index = prefix; index < before.size() - suffix; ++index)
        held.insert(before.identity(index));
    std::unordered_set<const void*> kept;
    for (std::size_t index = prefix; index < after.size() - suffix; ++index) {
        if (held.contains(after.identity(index)))
            kept.insert(after.identity(index));
        else
            diff.added.push_back(&after[index]);
    }
    for (std::size_t index = prefix; index < before.size() - suffix; ++index)
        if (!kept.contains(before.identity(index)))
            diff.removed.push_back(&before[index]);
    return diff;
}

} // namespace bloom::document

template <typename T>
inline constexpr bool std::ranges::enable_borrowed_range<bloom::document::RecordView<T>> = true;
template <typename T>
inline constexpr bool std::ranges::enable_view<bloom::document::RecordView<T>> = true;
