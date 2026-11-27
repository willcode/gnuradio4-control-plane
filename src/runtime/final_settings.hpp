#pragma once

#include <expected>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "gr4cp/runtime/block_settings.hpp"

#include <gnuradio-4.0/SettingValue.hpp>

namespace gr4cp::runtime::detail {

// Returns the entries of a block's refusal keyed as core keys the entries of a graph's: `<block>.<key>`, and the block
// alone for an entry that names no key.
inline gr::settings::Rejected keyed_by_block(gr::settings::Rejected refused, std::string_view block) {
    for (auto& entry : refused) {
        entry.key = entry.key.empty() ? std::string(block) : std::format("{}.{}", block, entry.key);
    }
    return refused;
}

// Returns the block's name, else its unique name. Core names a block inside a definition `<outer>.<inner>`, and each of
// two blocks that share a name by its unique name; this name does neither.
template <typename Block>
std::string entry_name(const Block& block) {
    const auto name = std::string(block.name());
    return name.empty() ? std::string(block.uniqueName()) : name;
}

// Returns the values a read holds, or a map as given; throws core's text for a refused read of `block`, each entry
// keyed by the block.
inline gr::property_map values_in_force(gr::property_map values, std::string_view /*block*/) { return values; }
inline gr::property_map values_in_force(const std::expected<gr::settings::Values, gr::settings::Rejected>& read,
                                        std::string_view block) {
    if (!read.has_value()) {
        throw std::runtime_error(gr::settings::describeRejected(keyed_by_block(read.error(), block)));
    }
    return gr::settings::heldValues(read);
}

// Returns each key of a settings result that failed, with its reason. A pending key and a key that holds a value are no
// failure.
inline gr::settings::Rejected failed_keys(const gr::settings::Values& values) {
    gr::settings::Rejected failed;
    for (const auto& [key, entry] : values) {
        if (entry.isFailed()) {
            failed.push_back(gr::settings::Rejection{.key = key, .reason = entry.reason});
        }
    }
    return failed;
}

// An ended block's values and lookup identity, without a handle keeping the block alive.
struct CapturedBlock {
    std::string unique_name;
    std::string block_name;
    std::string type_name;
    gr::property_map settings;
    std::optional<std::string> capture_error;

    std::string_view uniqueName() const { return unique_name; }
    std::string_view name() const { return block_name; }
    std::string_view typeName() const { return type_name; }
    std::expected<gr::settings::Value, gr::settings::Rejected> get(const std::string& key) const {
        const auto it = settings.find(key);
        if (it == settings.end()) {
            return std::unexpected(gr::settings::Rejected{{.key = key, .reason = "the ended run holds no value"}});
        }
        return gr::settings::Value{.value = it->second};
    }
    gr::property_map get() const {
        if (capture_error.has_value()) {
            throw std::runtime_error(*capture_error);
        }
        return settings;
    }
};

// Called only after the runtime has joined its workers and stop hooks. Capture failures remain explicit read
// errors for their blocks; other blocks still retain all their values.
template <typename Block>
auto capture_settings(const std::map<std::string, Block, std::less<>>& blocks) {
    std::map<std::string, CapturedBlock, std::less<>> captured;
    for (const auto& [unique_name, block] : blocks) {
        CapturedBlock snapshot{unique_name, std::string(block.name()), std::string(block.typeName()), {}, {}};
        try {
            const auto& entry = snapshot.block_name.empty() ? unique_name : snapshot.block_name;
            snapshot.settings = values_in_force(block.get(), entry);
        } catch (const std::exception& error) {
            snapshot.capture_error = std::format("final settings capture failed for block {}: {}", unique_name, error.what());
        }
        captured.emplace(unique_name, std::move(snapshot));
    }
    return captured;
}

}  // namespace gr4cp::runtime::detail
