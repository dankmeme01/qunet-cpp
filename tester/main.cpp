#include <iostream>
#include <qsox/UdpSocket.hpp>
#include <qsox/TcpStream.hpp>

#include <arc/prelude.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include <asp/time.hpp>
#include <fmt/color.h>
#include <fmt/ranges.h>

#ifdef QUNET_TLS_SUPPORT
# include <xtls/Backend.hpp>
#endif

using namespace qn;
using namespace asp::time;
using namespace arc;

arc::Future<> connLoop(Connection& conn) {
    constexpr static size_t TRIES = 32768;

    std::atomic<size_t> inFlight{0};
    std::vector<std::vector<uint8_t>> messages{1};
    auto [tx, rx] = arc::mpsc::channel<std::vector<uint8_t>>();

    conn.setDataCallback([&](std::vector<uint8_t> data) {
        tx.trySend(std::move(data)).unwrap();
    });

    auto receiver = arc::spawn([&] -> arc::Future<> {
        for (size_t i = 1; i < TRIES; i++) {
            auto val = (co_await rx.recv()).unwrap();
            inFlight--;

            if (val != messages[i]) {
                fmt::println("Received incorrect message {} ({} bytes)\n", i, val.size());
            } else {
                fmt::print("Received good message {} ({} bytes)\n", i, val.size());
            }
        }
    });

    for (size_t i = 1; i < TRIES; i++) {
        std::vector<uint8_t> msg(i);
        for (size_t j = 0; j < i; j++) {
            msg[j] = (uint8_t)rand();
        }
        messages.push_back(std::move(msg));
        log::debug("Send {} bytes", messages.back().size());
        conn.sendData(messages.back(), true);
        inFlight++;

        while (inFlight > 5) {
            co_await arc::sleep(asp::time::Duration::fromMillis(10));
        }

        co_await arc::sleep(asp::time::Duration::fromMillis(1));
    }

    co_await receiver;
}

arc::Future<int> amain(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <address>" << std::endl;
        co_return 1;
    }

    static Instant start = Instant::now();

    qn::log::setLogFunction([&](qn::log::Level level, const std::string& message) {
        auto timestr = fmt::format("{:.6f}", start.elapsed().seconds<double>());
        switch (level) {
#ifdef QUNET_DEBUG
            case qn::log::Level::Trace: fmt::println("[{}] [{}] {}", timestr, styled("TRACE", fg(fmt::color::gray)), message); break;
#endif
            case qn::log::Level::Debug: fmt::println("[{}] [{}] {}", timestr, styled("DEBUG", fg(fmt::color::gray)), message); break;
            case qn::log::Level::Info: fmt::println("[{}] [{}] {}", timestr, styled("INFO", fg(fmt::color::cyan)), message); break;
            case qn::log::Level::Warning: fmt::println("[{}] [{}] {}", timestr, styled("WARN", fg(fmt::color::yellow)), message); break;
            case qn::log::Level::Error: fmt::println("[{}] [{}] {}", timestr, styled("ERROR", fg(fmt::color::indian_red)), message); break;
        }
    });

    // if (auto err = (co_await testTLS()).err()) {
    //     std::cerr << "DOT test failed: " << err->message() << std::endl;
    //     co_return 1;
    // }
    // co_return 1;

    auto conn = co_await Connection::create();
    conn->setQdbFolder("./qdb-storage");
    conn->setDebugOptions(ConnectionDebugOptions {
        // .packetLossSimulation = 0.1f,
    });

#ifdef QUNET_QUIC_SUPPORT
    auto ctx = xtls::Backend::get().createContext(xtls::ContextType::Client1_3).unwrap();
    ctx->setCertVerification(false).unwrap();
    setupQuicContext(*ctx).unwrap();
    conn->setQuicTlsContext(ctx);
#endif

    auto res = co_await conn->connectWait(argv[1]);
    if (!res) {
        std::cerr << "Failed to connect: " << res.unwrapErr().message() << std::endl;
        co_return 1;
    }

    co_await arc::select(
        arc::selectee(connLoop(*conn)),
        arc::selectee(arc::ctrl_c())
    );

    conn->disconnect();

    while (!conn->disconnected()) {
        co_await arc::sleep(asp::time::Duration::fromMillis(10));
    }

    conn->destroy();

    // wait a bit for cleanup
    co_await arc::sleep(asp::time::Duration::fromMillis(100));

    co_return 0;
}

ARC_DEFINE_MAIN_NT(amain, 1);
