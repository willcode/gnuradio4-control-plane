#include "gr4cp/api/http_server.hpp"
#include "gr4cp/api/wasm_api.hpp"

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gr4cp/runtime/stub_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

using Json = nlohmann::json;

class EmptyBlockCatalogProvider final : public gr4cp::catalog::BlockCatalogProvider {
public:
    std::vector<gr4cp::domain::BlockDescriptor> list() const override { return {}; }
};

class EmptySchedulerCatalogProvider final : public gr4cp::catalog::SchedulerCatalogProvider {
public:
    std::vector<gr4cp::domain::SchedulerDescriptor> list() const override { return {}; }
};

// A runtime whose every start stops on a setting.
class SettingRefusingRuntimeManager final : public gr4cp::runtime::RuntimeManager {
public:
    void prepare(const gr4cp::domain::Session&) override {}
    void start(const gr4cp::domain::Session&) override {
        throw gr4cp::runtime::StartSettingsError("source0.gain: out of range [0, 1]: 2");
    }
    std::optional<std::string> stop(const gr4cp::domain::Session&) override { return std::nullopt; }
    void destroy(const gr4cp::domain::Session&) override {}
    std::optional<gr4cp::domain::StreamRuntimePlan> active_stream_plan(const gr4cp::domain::Session&) override {
        return std::nullopt;
    }
    gr4cp::runtime::HttpStreamResponse fetch_http_stream(const gr4cp::domain::Session&, const std::string&) override {
        return {};
    }
    gr4cp::runtime::WebSocketStreamRoute resolve_websocket_stream(const gr4cp::domain::Session&,
                                                                  const std::string&) override {
        throw gr4cp::runtime::SessionNotRunningError("no run");
    }
    void set_block_settings(const gr4cp::domain::Session&,
                            const std::string&,
                            const gr::property_map&,
                            gr4cp::runtime::BlockSettingsMode) override {}
    gr::property_map get_block_settings(const gr4cp::domain::Session&, const std::string&) override { return {}; }
    gr4cp::runtime::RunObservation observe(const gr4cp::domain::Session&) override { return {}; }
};

class WasmApiTest : public ::testing::Test {
protected:
    void TearDown() override {
        http_server.stop();
        if (http_thread.joinable()) {
            http_thread.join();
        }
    }

    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::runtime::StubRuntimeManager runtime;
    EmptyBlockCatalogProvider blocks;
    EmptySchedulerCatalogProvider schedulers;
    gr4cp::app::SessionService sessions{repository, runtime};
    gr4cp::app::SessionStreamService streams;
    gr4cp::app::BlockCatalogService block_catalog{blocks};
    gr4cp::app::SchedulerCatalogService scheduler_catalog{schedulers};
    gr4cp::app::BlockSettingsService settings{sessions, runtime};
    gr4cp::api::WasmApi api{sessions, streams, block_catalog, scheduler_catalog, settings};
    gr4cp::api::HttpServer http_server{sessions, streams, block_catalog, scheduler_catalog, settings};
    std::jthread http_thread;
};

TEST_F(WasmApiTest, HealthIdentityIsAStableUuidAcrossDispatchers) {
    const auto health = api.dispatch("GET", "/healthz", "");
    ASSERT_EQ(health.status, 200);
    const auto body = Json::parse(health.body);
    EXPECT_EQ(body.at("ok"), true);
    const auto identity = body.at("server_instance_id").get<std::string>();
    EXPECT_TRUE(std::regex_match(identity, std::regex{
        "[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"}));
    EXPECT_EQ(api.dispatch("GET", "/healthz", "").body, health.body);

    gr4cp::api::WasmApi another{sessions, streams, block_catalog, scheduler_catalog, settings};
    EXPECT_EQ(another.dispatch("GET", "/healthz", "").body, health.body);
}

TEST_F(WasmApiTest, HealthAndEverySessionResponseShareHttpIdentityAndSerialization) {
    const auto port = http_server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    http_thread = std::jthread([this]() { http_server.listen_after_bind(); });
    httplib::Client client("127.0.0.1", port);
    const auto http_health = client.Get("/healthz");
    ASSERT_TRUE(http_health);
    ASSERT_EQ(http_health->status, 200);
    const auto health = Json::parse(api.dispatch("GET", "/healthz", "").body);
    EXPECT_EQ(Json::parse(http_health->body), health);

    const auto created = api.dispatch("POST", "/sessions", R"({"name":"identity-check","grc":"graph"})");
    ASSERT_EQ(created.status, 201);
    auto session = Json::parse(created.body);
    EXPECT_FALSE(session.contains("completion_reason"));
    EXPECT_FALSE(session.contains("execution_id"));
    const auto id = session.at("id").get<std::string>();
    const auto path = "/sessions/" + id;
    EXPECT_EQ(session.at("server_instance_id"), health.at("server_instance_id"));

    const auto expect_matching_session = [&]() {
        const auto http_session = client.Get(path);
        ASSERT_TRUE(http_session);
        ASSERT_EQ(http_session->status, 200);
        const auto detail = api.dispatch("GET", path, "");
        ASSERT_EQ(detail.status, 200);
        EXPECT_EQ(Json::parse(detail.body), session);
        EXPECT_EQ(Json::parse(http_session->body), session);
        const auto listed = api.dispatch("GET", "/sessions", "");
        ASSERT_EQ(listed.status, 200);
        EXPECT_EQ(Json::parse(listed.body), Json::array({session}));
    };
    expect_matching_session();

    std::string previous_execution;
    for (const auto* action : {"start", "stop", "restart"}) {
        const auto response = api.dispatch("POST", path + "/" + action, "");
        ASSERT_EQ(response.status, 200) << response.body;
        session = Json::parse(response.body);
        EXPECT_EQ(session.at("server_instance_id"), health.at("server_instance_id"));
        const bool stopped = std::string_view(action) == "stop";
        EXPECT_EQ(session.contains("completion_reason"), stopped);
        const auto execution_id = session.at("execution_id").get<std::string>();
        EXPECT_FALSE(execution_id.empty());
        if (stopped) {
            EXPECT_EQ(session.at("completion_reason"), "stopped");
            EXPECT_EQ(execution_id, previous_execution);
        } else {
            EXPECT_NE(execution_id, previous_execution);
        }
        previous_execution = execution_id;
        expect_matching_session();
    }
}

// A start or a restart that stops on a setting is answered 409 with the code `settings_refused` and the key, and the
// session reads that error.
TEST_F(WasmApiTest, StartThatStopsOnASettingAnswers409WithTheKey) {
    SettingRefusingRuntimeManager refusing;
    gr4cp::app::SessionService refusing_sessions{repository, refusing};
    gr4cp::app::BlockSettingsService refusing_settings{refusing_sessions, refusing};
    gr4cp::api::WasmApi refusing_api{refusing_sessions, streams, block_catalog, scheduler_catalog, refusing_settings};

    const auto created = refusing_api.dispatch("POST", "/sessions", R"({"name":"refused","grc":"graph"})");
    ASSERT_EQ(created.status, 201) << created.body;
    const auto path = "/sessions/" + Json::parse(created.body).at("id").get<std::string>();

    for (const auto* action : {"start", "restart"}) {
        const auto response = refusing_api.dispatch("POST", path + "/" + action, "");
        EXPECT_EQ(response.status, 409) << action << ": " << response.body;
        const auto error = Json::parse(response.body).at("error");
        EXPECT_EQ(error.at("code"), "settings_refused") << response.body;
        const auto message = error.at("message").get<std::string>();
        EXPECT_NE(message.find("source0.gain: out of range [0, 1]: 2"), std::string::npos) << response.body;

        const auto read = refusing_api.dispatch("GET", path, "");
        ASSERT_EQ(read.status, 200) << read.body;
        const auto session = Json::parse(read.body);
        EXPECT_EQ(session.at("state"), "error") << read.body;
        EXPECT_EQ(session.at("last_error"), message) << read.body;
    }
}

}  // namespace
