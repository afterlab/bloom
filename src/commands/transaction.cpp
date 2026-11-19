#include <bloom/commands/transaction.hpp>

#include <limits>
#include <utility>

namespace bloom::commands {

bool Transaction::add(std::unique_ptr<Operation> operation) {
    if (!operation) {
        return false;
    }
    operations_.push_back(std::move(operation));
    return true;
}

std::size_t Transaction::workUnits() const noexcept {
    std::size_t total = 0;
    for (const auto& operation : operations_) {
        const auto units = operation->workUnits();
        if (units > std::numeric_limits<std::size_t>::max() - total)
            return std::numeric_limits<std::size_t>::max();
        total += units;
    }
    return total;
}

} // namespace bloom::commands
