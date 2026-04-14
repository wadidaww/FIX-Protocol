// =============================================================================
// FIX Protocol Engine - Reference OMS Application
// Shows NewOrderSingle / ExecutionReport flow
// =============================================================================
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <fix/core/constants.hpp>
#include <fix/engine.hpp>
#include <fix/parser/serializer.hpp>

using namespace fix;

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false);
}

// ---------------------------------------------------------------------------
// OMS – simple order management system demo
// ---------------------------------------------------------------------------
class OMS {
public:
    OMS() {
        EngineConfig ec;
        ec.store_dir = "./oms_store";
        ec.log_dir = "./oms_logs";
        ec.enable_audit = true;
        ec.use_file_store = false;
        engine_ = std::make_unique<Engine>(std::move(ec));

        // Load FIX 4.2 built-in dictionary
        engine_->load_builtin_dictionary(FixVersion::FIX_4_2);
    }

    void run(const std::string &host, std::uint16_t port) {
        // Create initiator session
        SessionConfig cfg;
        cfg.id.version = FixVersion::FIX_4_2;
        cfg.id.senderCompID = "OMS1";
        cfg.id.targetCompID = "BROKER1";
        cfg.initiator = true;
        cfg.heartbeat_interval = 30;
        cfg.reset_on_logon = true;
        cfg.username = "oms_user";
        cfg.password = "s3cr3t";

        SessionCallbacks cbs;
        cbs.on_logon = [this](const SessionID &sid) {
            std::cout << "[OMS] Logged on: " << sid.to_string() << "\n";
            send_test_order();
        };
        cbs.on_logout = [](const SessionID &sid, std::string_view reason) {
            std::cout << "[OMS] Logged out: " << sid.to_string() << " reason=" << reason << "\n";
        };
        cbs.on_message = [this](const SessionID &sid, const Message &msg) {
            handle_message(sid, msg);
        };
        cbs.on_sequence_gap = [](const SessionID &sid, SeqNum exp, SeqNum got) {
            std::cout << "[OMS] SeqGap on " << sid.to_string() << " expected=" << exp
                      << " got=" << got << "\n";
        };

        // Create TCP transport
        TcpTransportConfig tc;
        tc.host = host;
        tc.port = port;
        tc.initiator = true;

        auto transport = make_tcp_transport(std::move(tc));

        session_ = engine_->add_session(std::move(cfg), std::move(transport), std::move(cbs),
                                        engine_->dictionary(FixVersion::FIX_4_2));

        engine_->start();

        // Start the session
        session_->logon();

        std::cout << "[OMS] Running (press Ctrl-C to stop)\n";
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        session_->logout("OMS shutdown");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        engine_->stop();
    }

private:
    std::unique_ptr<Engine> engine_;
    Session *session_ = nullptr;
    std::uint64_t order_counter_ = 0;

    void send_test_order() {
        if (!session_ || !session_->is_active())
            return;

        ++order_counter_;
        std::string clordid = "ORD-" + std::to_string(order_counter_);

        Message order(msg_types::NewOrderSingle);
        order.set(tags::ClOrdID, clordid);
        order.set(tags::HandlInst, "1"); // automated
        order.set(tags::Symbol, "AAPL");
        order.set(tags::Side, "1"); // Buy
        order.set(tags::TransactTime, MessageBuilder::format_timestamp_now());
        order.set(tags::OrderQty, 100.0);
        order.set(tags::OrdType, "2"); // Limit
        order.set(tags::Price, 150.00);
        order.set(tags::TimeInForce, "0"); // Day
        order.set(tags::Account, "ACC-001");
        order.set(tags::Currency, "USD");

        auto r = session_->send(std::move(order));
        if (r.has_value()) {
            std::cout << "[OMS] Sent NewOrderSingle clOrdID=" << clordid << "\n";
        } else {
            std::cerr << "[OMS] Failed to send order: " << r.error().message() << "\n";
        }
    }

    void handle_message(const SessionID &sid, const Message &msg) {
        auto mt = msg.msg_type();

        if (mt == msg_types::ExecutionReport) {
            auto ordid = msg.get(tags::OrderID).value_or("?");
            auto clordid = msg.get(tags::ClOrdID).value_or("?");
            auto status = msg.get(tags::OrdStatus).value_or("?");
            auto exec_type = msg.get(tags::ExecType).value_or("?");
            auto leaves = msg.get_double(tags::LeavesQty).value_or(0.0);
            auto cum = msg.get_double(tags::CumQty).value_or(0.0);
            auto avg_px = msg.get_double(tags::AvgPx).value_or(0.0);

            std::cout << "[OMS] ExecReport ordID=" << ordid << " clOrdID=" << clordid
                      << " status=" << status << " execType=" << exec_type << " leaves=" << leaves
                      << " cum=" << cum << " avgPx=" << avg_px << "\n";

            // If filled, send another order
            if (status == "2") { // Filled
                send_test_order();
            }
        } else if (mt == msg_types::OrderCancelReject) {
            auto clordid = msg.get(tags::ClOrdID).value_or("?");
            auto reason = msg.get(tags::CxlRejReason).value_or("?");
            std::cout << "[OMS] CancelReject clOrdID=" << clordid << " reason=" << reason << "\n";
        } else if (mt == msg_types::BusinessMessageReject) {
            auto text = msg.get(tags::Text).value_or("?");
            std::cout << "[OMS] BusinessReject: " << text << "\n";
        } else {
            std::cout << "[OMS] Received: " << mt << "\n";
        }
    }
};

int main(int argc, char *argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string host = "127.0.0.1";
    std::uint16_t port = 9876;

    if (argc >= 2)
        host = argv[1];
    if (argc >= 3)
        port = static_cast<std::uint16_t>(std::stoi(argv[2]));

    std::cout << "FIX OMS Reference Application\n";
    std::cout << "Connecting to " << host << ":" << port << "\n";

    OMS oms;
    oms.run(host, port);

    return 0;
}
