#include <iostream>
#include <qsox/UdpSocket.hpp>
#include <qsox/TcpStream.hpp>

#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include <asp/time.hpp>

using namespace qsox;
using namespace qn;

int main(int argc, const char** argv) {
    qn::log::setLogFunction([](qn::log::Level level, const std::string& message) {
        switch (level) {
            case qn::log::Level::Debug: std::cout << "[DEBUG] " << message << std::endl; break;
            case qn::log::Level::Info: std::cout << "[INFO] " << message << std::endl; break;
            case qn::log::Level::Warning: std::cout << "[WARN] " << message << std::endl; break;
            case qn::log::Level::Error: std::cerr << "[ERROR] " << message << std::endl; break;
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