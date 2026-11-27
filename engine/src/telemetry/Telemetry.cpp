#include "Telemetry.h"

#include <mutex>

namespace bv::telemetry {

StageStats& Telemetry::stage(std::string_view name) {
    {
        std::shared_lock read(mutex_);
        if (auto it = stages_.find(std::string(name)); it != stages_.end()) return *it->second;
    }
    std::unique_lock write(mutex_);
    auto& slot = stages_[std::string(name)];
    if (!slot) slot = std::make_unique<StageStats>();
    return *slot;
}

const StageStats* Telemetry::find(std::string_view name) const {
    std::shared_lock read(mutex_);
    auto it = stages_.find(std::string(name));
    return it == stages_.end() ? nullptr : it->second.get();
}

void Telemetry::resetAll() {
    std::shared_lock read(mutex_);
    for (auto& [_, stats] : stages_) stats->reset();
}

} // namespace bv::telemetry
