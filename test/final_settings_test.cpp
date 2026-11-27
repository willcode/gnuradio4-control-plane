#include "../src/runtime/final_settings.hpp"

#include <gtest/gtest.h>

namespace {
struct SnapshotBlock {
    bool fails;
    std::string_view name() const { return "alias"; }
    std::string_view typeName() const { return "test::Block"; }
    gr::property_map get() const {
        if (fails) throw std::runtime_error("capture refused");
        return {{"value", 42}};
    }
};

TEST(FinalSettingsTest, CaptureFailureRemainsAReadErrorWithoutHidingOtherBlocks) {
    const std::map<std::string, SnapshotBlock, std::less<>> blocks{{"failed", {true}}, {"healthy", {false}}};
    const auto captured = gr4cp::runtime::detail::capture_settings(blocks);
    ASSERT_EQ(captured.size(), 2U);
    const auto& failed = captured.at("failed");
    EXPECT_EQ(failed.uniqueName(), "failed");
    EXPECT_EQ(failed.name(), "alias");
    EXPECT_EQ(failed.typeName(), "test::Block");
    EXPECT_FALSE(failed.get("value").has_value());
    try {
        (void)failed.get();
        FAIL() << "a failed capture must not read as empty settings";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string(error.what()), "final settings capture failed for block failed: capture refused");
    }
    EXPECT_EQ(captured.at("healthy").get().at("value").value_or(0), 42);
}

// A refused read of a block's values throws core's text with each entry keyed by the block, `<block>.<key>`, and the
// block alone for an entry that names no key.
TEST(FinalSettingsTest, RefusedReadNamesEachEntryByItsBlock) {
    const std::expected<gr::settings::Values, gr::settings::Rejected> refused = std::unexpected(gr::settings::Rejected{
        {.key = "gain", .reason = "out of range [0, 1]: 2"}, {.key = "", .reason = "the block is gone"}});
    try {
        (void)gr4cp::runtime::detail::values_in_force(refused, "lim0");
        FAIL() << "a refused read must not read as values";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string(error.what()), "lim0.gain: out of range [0, 1]: 2; lim0: the block is gone");
    }
}
}  // namespace
