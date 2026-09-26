#pragma once
// =============================================================================
// FIX Protocol Engine - Session State Machine
// =============================================================================
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "../core/constants.hpp"
#include "../core/message.hpp"
#include "../core/types.hpp"
#include "../dictionary/data_dictionary.hpp"
#include "../parser/parser.hpp"
#include "../parser/serializer.hpp"
#include "../store/message_store.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// Session configuration
// ---------------------------------------------------------------------------
struct SessionConfig {
    SessionID id;
    bool initiator = true;       // false = acceptor
    int heartbeat_interval = 30; // seconds
    int reconnect_delay = 5;     // seconds (for initiators)
    int logon_timeout = 10;      // seconds
    int logout_timeout = 5;      // seconds
    bool reset_on_logon = false;
    bool reset_on_disconnect = false;
    bool validate_fields = true;
    std::string username;
    std::string password;
    SeqNum reset_seq_num = 0; // 0 = no override

    // FIXT: default ApplVerID to use for FIX 5.0 SP2 sessions
    std::string default_appl_ver_id;
};

// ---------------------------------------------------------------------------
// Session FSM states
// ---------------------------------------------------------------------------
enum class SessionState : std::uint8_t {
    NotConnected,
    WaitingLogon,   // acceptor waiting for logon
    LogonSent,      // initiator sent logon
    Active,         // session established
    LogoutSent,     // we sent logout
    LogoutReceived, // peer sent logout, we're draining
    Reconnecting,   // backing off before retry
    Disconnected,   // terminal
};

std::string_view to_string(SessionState s) noexcept;

// ---------------------------------------------------------------------------
// Application callbacks
// ---------------------------------------------------------------------------
struct SessionCallbacks {
    // Session lifecycle
    std::function<void(const SessionID &)> on_logon;
    std::function<void(const SessionID &, std::string_view reason)> on_logout;
    std::function<void(const SessionID &, SeqNum expected, SeqNum received)> on_sequence_gap;
    std::function<void(const SessionID &)> on_heartbeat_timeout;
    std::function<void(const SessionID &, SeqNum, SeqNum)> on_resend_request;

    // Message received (called for application-level messages)
    std::function<void(const SessionID &, const Message &)> on_message;

    // Transport send callback (engine calls this to write bytes to the wire)
    std::function<void(const std::string &)> do_send;
    std::function<void()> do_disconnect; // engine → ITransport::disconnect()
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
class Session {
public:
    explicit Session(SessionConfig cfg, std::unique_ptr<IMessageStore> store,
                     const DataDictionary *dict = nullptr, SessionCallbacks cbs = {});

    ~Session();

    // Called by transport layer when bytes arrive
    void on_data(const char *data, std::size_t len);

    // Called by the engine when the underlying transport connection has been
    // established (initiator: reset any stale state and send Logon;
    // acceptor: enter WaitingLogon so an incoming Logon is accepted).
    // Safe to call on every (re)connect.
    void on_transport_connected();

    // Send an application message (seq assigned automatically)
    Result<void> send(Message msg);

    // Send a raw string (already serialized) – bypasses seq mgmt
    Result<void> send_raw(const std::string &raw);

    // Trigger logon (for initiators)
    Result<void> logon();

    // Initiate graceful logout
    Result<void> logout(std::string_view reason = "");

    // Called periodically by the engine (e.g., from a timer thread)
    void on_timer();

    // Forcibly disconnect (no logout sent)
    void disconnect();

    // Resets session sequence numbers and state
    Result<void> reset();

    // Getters
    [[nodiscard]] const SessionID &id() const noexcept { return cfg_.id; }
    [[nodiscard]] SessionState state() const noexcept { return state_.load(); }
    [[nodiscard]] bool is_active() const noexcept { return state_.load() == SessionState::Active; }
    [[nodiscard]] const IMessageStore *store() const noexcept { return store_.get(); }

    // For metrics
    [[nodiscard]] std::uint64_t msgs_sent() const noexcept { return msgs_sent_.load(); }
    [[nodiscard]] std::uint64_t msgs_received() const noexcept { return msgs_received_.load(); }

private:
    SessionConfig cfg_;
    std::unique_ptr<IMessageStore> store_;
    const DataDictionary *dict_;
    SessionCallbacks cbs_;
    StreamParser parser_;
    MessageBuilder builder_;

    std::atomic<SessionState> state_{SessionState::NotConnected};
    std::atomic<std::uint64_t> msgs_sent_{0};
    std::atomic<std::uint64_t> msgs_received_{0};

    mutable std::mutex send_mutex_;

    // -- Shared timing state (F2) -------------------------------------------
    // Epoch milliseconds, stored atomically: written on IO/user threads
    // (process_message/send_message/send_logon) and read by the engine's
    // timer thread (on_timer). Plain TimePoint members raced under TSAN.
    using Millis = std::chrono::milliseconds::rep;
    static Millis now_ms() noexcept;
    std::atomic<Millis> last_send_time_ms_{0};
    std::atomic<Millis> last_recv_time_ms_{0};
    std::atomic<Millis> logon_sent_time_ms_{0};

    // -- TestRequest round-trip bookkeeping (F2) ----------------------------
    // The flag is written by the timer thread (send_test_request) and IO
    // thread (handle_heartbeat / on_transport_connected / disconnect); the
    // ID string is guarded by test_req_mtx_ — the mutex is only ever held
    // for the string itself, never across sends or user callbacks.
    mutable std::mutex test_req_mtx_;
    std::string pending_test_req_id_;
    std::atomic<bool> test_req_pending_{false};

    // -- Heartbeat interval mirror (F2) -------------------------------------
    // cfg_.heartbeat_interval is written by the IO thread (handle_logon) and
    // read by the timer thread (on_timer) and by send_logon (any thread).
    // SessionConfig must stay copyable, so the shared view lives here as an
    // atomic mirror; construction and handle_logon keep both in sync.
    std::atomic<int> heartbeat_sec_{30};

    // -- Parser guard (F10/F14) ---------------------------------------------
    // StreamParser is single-thread-affine (feed/next/reset must not overlap).
    // on_data holds this only across feed()/next() — never across
    // process_message() — so disconnect() (which may run on the timer or IO
    // thread and resets the parser) can never deadlock against it.
    mutable std::mutex parser_mtx_;

    // -- Message dispatch ----------------------------------------------------
    void process_message(const Message &msg);
    void process_admin(const Message &msg);
    void process_app(const Message &msg);

    // -- Admin message handlers ---------------------------------------------
    void handle_logon(const Message &msg);
    void handle_logout(const Message &msg);
    void handle_heartbeat(const Message &msg);
    void handle_test_request(const Message &msg);
    void handle_resend_request(const Message &msg);
    void handle_sequence_reset(const Message &msg);
    void handle_reject(const Message &msg);

    // -- Internal send helpers -----------------------------------------------
    void send_logon();
    void send_logout(std::string_view reason);
    void send_heartbeat(std::string_view test_req_id = "");
    void send_test_request();
    void send_resend_request(SeqNum begin, SeqNum end);
    void send_sequence_reset(SeqNum new_seq, bool gap_fill);
    void send_reject(SeqNum ref_seq, SessionRejectReason reason, std::string_view ref_msg_type = "",
                     TagNum ref_tag = 0, std::string_view text = "");

    Result<void> send_message(Message &msg); // internal: sets header, stores, sends
    std::string build_header_fields(const Message &msg, SeqNum seq);

    // -- Sequence validation -------------------------------------------------
    bool validate_seq_num(const Message &msg);

    // -- Helpers -------------------------------------------------------------
    bool is_admin_msg(std::string_view msg_type) const noexcept;
    std::string sending_time_str() const;
};

} // namespace fix
