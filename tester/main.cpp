#include <iostream>
#include <qsox/UdpSocket.hpp>
#include <qsox/TcpStream.hpp>

#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include <asp/time.hpp>

using namespace qsox;
using namespace qn;
using namespace asp::time;

int main(int argc, const char** argv) {
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

    auto res = conn.connect(argv[1]);
    if (!res) {
        std::cerr << "Failed to connect: " << res.unwrapErr().message() << std::endl;
        return 1;
    }

    while (conn.connecting()) {
        asp::time::sleep(asp::time::Duration::fromMillis(100));
    }

    if (conn.connected()) {
        log::info("Connected!");
    } else {
        log::warn("Failed to connect: {}", conn.lastError().message());
    }

    while (true) {
        conn.sendKeepalive();
        asp::time::sleep(asp::time::Duration::fromMillis(100));
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