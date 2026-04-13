# FIX Protocol Engine

A production-ready, ultra-reliable, high-performance **Financial Information Exchange (FIX) protocol engine** written in modern **C++23**, designed for institutional trading environments.

## Features

### Protocol Coverage
- **FIX 4.2, 4.4, 5.0 SP2, and FIXT 1.1** with automatic `ApplVerID` routing
- All administrative session messages: Logon, Logout, Heartbeat, TestRequest, ResendRequest, SequenceReset, Reject
- Business messages: NewOrderSingle, ExecutionReport, OrderCancelRequest, OrderCancelReplaceRequest, MarketDataRequest/Snapshot/Incremental, BusinessMessageReject, and more

### Architecture
- **Layered, event-driven, modular** design separating transport, session, dictionary, and application layers
- **Reactor pattern** for network I/O using `epoll` on Linux (non-blocking, edge-triggered)
- **Session FSM** with initiator and acceptor roles, configurable per session
- **Asynchronous, non-blocking** message flow throughout

### Parser / Serializer
- **Streaming zero-copy SOH-delimited parser** decoding raw `Tag=Value\x01` bytes directly from network buffers
- Handles partial frames across TCP reads with no intermediate allocations
- Configurable checksum and body-length validation
- **Incremental message builder** with correct BodyLength + CheckSum generation

### Session Layer
- Full **FIX FSM** per session (NotConnected → LogonSent/WaitingLogon → Active → LogoutSent → Disconnected)
- **Sequence number management** with lock-free atomic counters
- **Gap detection** and automatic `ResendRequest` generation
- **GapFill / SequenceReset** processing
- **Heartbeat / TestRequest / timeout management**

### Data Dictionary
- Built-in dictionaries for FIX 4.2, 4.4, 5.0 SP2
- **Hot-reloadable** – atomic dictionary swap without recompilation
- Field type validation, enumerated value checking
- `ApplVerID`-aware version routing for FIXT 1.1
- Runtime registration of custom tags and venue extensions

### Message Store
- **MemoryStore** – lock-free atomic counters, in-memory message map
- **FileStore** – append-only log with index, crash-safe sequence persistence

### Transport
- **TCP with epoll** (Linux), portable fallback for other platforms
- TCP_NODELAY, non-blocking edge-triggered I/O
- TLS 1.3 ready for OpenSSL/BoringSSL integration

### Audit Log
- Async file-based audit log with rotation and retention policies
- MiFID II / SEC Rule 605/606 compliance

## Building

### Prerequisites
- C++23 compiler: GCC ≥ 13 or Clang ≥ 17 or MSVC 2022
- CMake ≥ 3.25
- Ninja (recommended) or Make

### Quick Start
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && ctest --output-on-failure
```

### Build Options
| Option              | Default | Description                              |
|---------------------|---------|------------------------------------------|
| `FIX_BUILD_TESTS`   | ON      | Build Google Test unit tests             |
| `FIX_BUILD_APPS`    | ON      | Build reference OMS application          |
| `FIX_BUILD_SHARED`  | OFF     | Build shared library                     |
| `FIX_ENABLE_ASAN`   | OFF     | Enable AddressSanitizer                  |
| `FIX_ENABLE_TSAN`   | OFF     | Enable ThreadSanitizer                   |
| `FIX_ENABLE_UBSAN`  | OFF     | Enable UBSanitizer                       |
| `FIX_ENABLE_LTO`    | OFF     | Enable Link-Time Optimisation            |
| `FIX_NO_EXCEPTIONS` | OFF     | Build with `-fno-exceptions`             |

## Usage

### Minimal Example
```cpp
#include <fix/engine.hpp>
#include <fix/core/constants.hpp>

fix::Engine engine;
engine.load_builtin_dictionary(fix::FixVersion::FIX_4_2);

fix::SessionConfig cfg;
cfg.id.version       = fix::FixVersion::FIX_4_2;
cfg.id.senderCompID  = "FIRM1";
cfg.id.targetCompID  = "BROKER";
cfg.initiator        = true;
cfg.heartbeat_interval = 30;

fix::SessionCallbacks cbs;
cbs.on_logon = [](const fix::SessionID& sid) {
    std::cout << "Logged on: " << sid.to_string() << "\n";
};
cbs.on_message = [](const fix::SessionID& sid, const fix::Message& msg) {
    if (msg.msg_type() == fix::msg_types::ExecutionReport) {
        auto ordid = msg.get(fix::tags::OrderID).value_or("?");
        std::cout << "ExecutionReport ordID=" << ordid << "\n";
    }
};

fix::TcpTransportConfig tc;
tc.host = "broker.example.com";
tc.port = 9876;
auto transport = fix::make_tcp_transport(tc);
auto* session = engine.add_session(cfg, std::move(transport), cbs);

engine.start();
session->logon();
```

### Sending Orders
```cpp
fix::Message order(fix::msg_types::NewOrderSingle);
order.set(fix::tags::ClOrdID,      "ORD-001");
order.set(fix::tags::Symbol,       "AAPL");
order.set(fix::tags::Side,         "1");   // Buy
order.set(fix::tags::OrderQty,     100.0);
order.set(fix::tags::OrdType,      "2");   // Limit
order.set(fix::tags::Price,        150.00);
order.set(fix::tags::TransactTime, fix::MessageBuilder::format_timestamp_now());

session->send(order);
```

## Project Structure
```
include/fix/
  core/         types.hpp, field.hpp, message.hpp, constants.hpp
  parser/       parser.hpp (StreamParser), serializer.hpp (MessageBuilder)
  dictionary/   data_dictionary.hpp
  session/      session.hpp, session_manager.hpp
  store/        message_store.hpp, memory_store.hpp, file_store.hpp
  transport/    transport.hpp, tcp_transport.hpp
  log/          message_log.hpp
  engine.hpp
src/            Implementation files
tests/          Google Test unit tests (56 tests)
apps/oms/       Reference OMS application
.github/workflows/ci.yml   GitHub Actions CI
```

## CI
GitHub Actions pipeline runs on every push:
- Linux (GCC 13 + Clang 18), Release and Debug
- macOS (AppleClang)
- Windows (MSVC 2022)
- AddressSanitizer + UBSanitizer
- ThreadSanitizer

## License
See [LICENSE](LICENSE).
