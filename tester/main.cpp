#include <iostream>
#include <qsox/UdpSocket.hpp>
#include <qsox/TcpStream.hpp>

#include <arc/prelude.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include <asp/time.hpp>

using namespace qn;
using namespace asp::time;
using namespace arc;

arc::Future<> connLoop(Connection& conn) {
    while (true) {
        log::debug("Sending keepalive");
        conn.sendKeepalive();
        std::vector megabyte(1024 * 1023, (uint8_t) 0x42); // 1 MB of data
        // std::vector megabyte(256, (uint8_t)0);
        // fill with stuff
        for (size_t i = 0, u = 0; i < megabyte.size(); i++, u++) {
            megabyte[i] = rand() % 256;
        }

        // conn.sendData(megabyte);
        co_await arc::sleep(asp::time::Duration::fromMillis(1000));
    }
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
            case qn::log::Level::Debug: fmt::println("[{}] [DEBUG] {}", timestr, message); break;
            case qn::log::Level::Info: fmt::println("[{}] [INFO] {}", timestr, message); break;
            case qn::log::Level::Warning: fmt::println("[{}] [WARN] {}", timestr, message); break;
            case qn::log::Level::Error: fmt::println("[{}] [ERROR] {}", timestr, message); break;
        }
    });

    auto conn = co_await Connection::create();
    conn->setTlsCertVerification(false);
    conn->setDebugOptions(ConnectionDebugOptions {
        // .packetLossSimulation = 0.1f,
    });

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
