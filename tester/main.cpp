#include <iostream>
#include <qsox/UdpSocket.hpp>
#include <qsox/TcpStream.hpp>

#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include <asp/time.hpp>

#include <csignal>

using namespace qsox;
using namespace qn;
using namespace asp::time;

std::atomic_bool g_running = true;

void signalHandler(int signal) {
    g_running = false;
}

int main(int argc, const char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <address>" << std::endl;
        return 1;
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

    qn::Connection conn;
    conn.setTlsCertVerification(false);
    conn.setDebugOptions(ConnectionDebugOptions {
        .packetLossSimulation = 0.0f,
    });

    auto res = conn.connect(argv[1]);
    if (!res) {
        std::cerr << "Failed to connect: " << res.unwrapErr().message() << std::endl;
        return 1;
    }

    while (conn.connecting() && g_running) {
        asp::time::sleep(asp::time::Duration::fromMillis(100));
    }

    if (conn.connected()) {
        log::info("Connected!");
    } else if (g_running) {
        log::warn("Failed to connect: {}", conn.lastError().message());
        return 1;
    } else {
        log::info("Aborted");
        return 0;
    }

    while (g_running) {
        conn.sendKeepalive();
        // std::vector megabyte(1024 * 1023, (uint8_t) 0x42); // 1 MB of data
        std::vector megabyte(1024, (uint8_t) 0x42); // 1 MB of data
        conn.sendData(megabyte);
        asp::time::sleep(asp::time::Duration::fromMillis(1000));
    }

    res = conn.disconnect();
    if (!res) {
        log::warn("Failed to disconnect: {}", res.unwrapErr().message());
    } else {
        log::info("Disconnected successfully");
    }

    while (!conn.disconnected()) {
        asp::time::sleep(asp::time::Duration::fromMillis(10));
    }
}


// int main(int argc, const char** argv) {
//     if (argc < 2) {
//         std::cerr << "Usage: " << argv[0] << " <address>" << std::endl;
//         return 1;
//     }

//     auto addr = SocketAddress::parse(argv[1]);
//     if (!addr) {
//         std::cerr << "Failed to parse address " << argv[1] << ": " << addr.unwrapErr().message() << std::endl;
//         return 1;
//     }

//     auto s = addr.unwrap();
//     std::cout << "Connecting to " << s.toString() << std::endl;

//     // auto sockR = UdpSocket::bindAny();
//     // if (!sockR) {
//     //     std::cerr << "Failed to bind socket: " << sockR.unwrapErr().message() << std::endl;
//     //     return 1;
//     // }

//     // auto sock = std::move(sockR).unwrap();
//     // sock.connect(s).unwrap();
//     auto sock = TcpStream::connect(s).unwrap();


//     // char buffer[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
//     char buffer[] = {5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
//     sock.send(buffer, sizeof(buffer)).unwrap();

//     char response[1024];
//     size_t bytes = sock.receive(response, sizeof(response)).unwrap();

//     std::cout << "Received response of size " << bytes << " bytes." << std::endl;
// }