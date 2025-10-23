#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/socket/transport/QuicTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/database/QunetDatabase.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include <qunet/Connection.hpp>

#include <asp/time/Instant.hpp>

using namespace qsox;
using namespace asp::time;

namespace qn {

TransportResult<std::pair<Socket, Duration>> Socket::createSocket(const TransportOptions& options) {
    auto startedAt = Instant::now();

    StatTracker tracker;
    tracker.setEnabled(options.connOptions->debug.recordStats);

    auto transport = GEODE_UNWRAP(Socket::createTransport(options));

    if (startedAt.elapsed() > options.timeout) {
        return Err(TransportError::ConnectionTimedOut);
    }

    tracker.onConnected();
    transport->m_tracker = std::move(tracker);

    Socket socket(std::move(transport));

    auto handshakeTimeout = options.timeout - startedAt.elapsed();
    if (handshakeTimeout.millis() <= 0) {
        return Err(TransportError::ConnectionTimedOut);
    }

    return Ok(std::pair{std::move(socket), handshakeTimeout});
}

TransportResult<Socket> Socket::connect(const TransportOptions& options) {
    auto [socket, timeout] = GEODE_UNWRAP(createSocket(options));

    auto msg = GEODE_UNWRAP(socket.m_transport->performHandshake(HandshakeStartMessage {
        .majorVersion = MAJOR_VERSION,
        .fragLimit = UDP_PACKET_LIMIT,
        // TODO: qdb hash
        .qdbHash = std::array<uint8_t, 16>{}
    }, timeout));

    if (msg.is<HandshakeFinishMessage>()) {
        auto& hf = msg.as<HandshakeFinishMessage>();
        GEODE_UNWRAP(socket.onHandshakeSuccess(hf));
    } else if (msg.is<HandshakeFailureMessage>()) {
        auto& hf = msg.as<HandshakeFailureMessage>();
        log::warn("Handshake failed: {}", hf.message());

        return Err(TransportError::HandshakeFailure(std::string(hf.message())));
    } else {
        return Err(TransportError::UnexpectedMessage);
    }

    return Ok(std::move(socket));
}

TransportResult<Socket> Socket::reconnect(const TransportOptions& options, Socket& prev) {
    auto [socket, timeout] = GEODE_UNWRAP(createSocket(options));

    auto msg = GEODE_UNWRAP(socket.m_transport->performReconnect(prev.transport()->m_connectionId, timeout));

    if (msg.is<ReconnectSuccessMessage>()) {
        GEODE_UNWRAP(socket.onReconnectSuccess(prev));
    } else if (msg.is<ReconnectFailureMessage>()) {
        log::warn("Reconnect failed!");
        return Err(TransportError::ReconnectFailed);
    } else {
        return Err(TransportError::UnexpectedMessage);
    }

    return Ok(std::move(socket));
}

TransportResult<> Socket::onHandshakeSuccess(const HandshakeFinishMessage& msg) {
    log::debug("Handshake finished, connection ID: {}, qdb size: {}", msg.connectionId, msg.qdbData ? msg.qdbData->uncompressedSize : 0);
    m_transport->setConnectionId(msg.connectionId);

    if (msg.qdbData) {
        // qdb data is also zstd compressed, decompress it first
        ZstdDecompressor dec;
        dec.init().unwrap();

        size_t realSize = msg.qdbData->uncompressedSize;
        std::vector<uint8_t> qdbData(realSize);

        GEODE_UNWRAP(dec.decompress(msg.qdbData->chunkData.data(), msg.qdbData->chunkData.size(), qdbData.data(), realSize));
        qdbData.resize(realSize);

        auto qdb = GEODE_UNWRAP(QunetDatabase::decode(qdbData).mapErr([&](const DatabaseDecodeError& err) {
            log::warn("Failed to decode Qunet database: {}", err.message());
            return TransportError::InvalidQunetDatabase;
        }));

        GEODE_UNWRAP(m_transport->initCompressors(&qdb));
    } else {
        GEODE_UNWRAP(m_transport->initCompressors());
    }

    return Ok();
}

TransportResult<> Socket::onReconnectSuccess(Socket& older) {
    log::debug("Reconnect finished, connection ID: {}", older.m_transport->m_connectionId);
    m_transport->setConnectionId(older.m_transport->m_connectionId);
    m_transport->setMessageSizeLimit(older.m_transport->m_messageSizeLimit);
    m_transport->m_zstdCompressor = std::move(older.m_transport->m_zstdCompressor);
    m_transport->m_zstdDecompressor = std::move(older.m_transport->m_zstdDecompressor);

    return Ok();
}

TransportResult<> Socket::close() {
    return m_transport->close();
}

bool Socket::isClosed() const {
    return m_transport->isClosed();
}

TransportResult<> Socket::sendMessage(QunetMessage&& message, bool reliable) {
    log::debug("Socket: sending message: {}", message.typeStr());

    // determine if the message needs to be compressed
    if (message.is<DataMessage>()) {
        auto& msg = message.as<DataMessage>();

        uint32_t uncSize = msg.data.size();
        auto ctype = this->shouldCompress(uncSize);

        switch (ctype) {
            case CompressionType::Zstd: {
                GEODE_UNWRAP(this->doCompressZstd(msg));
            } break;

            case CompressionType::Lz4: {
                return Err(TransportError::NotImplemented);
            } break;

            default: break;
        }
    }

    return m_transport->sendMessage(std::move(message), reliable);
}

CompressionType Socket::shouldCompress(size_t size) const {
    if (size > 1024) {
        return CompressionType::Zstd;
    } else {
        return CompressionType::None;
    }
}

CompressorResult<> Socket::doCompressZstd(DataMessage& message) const {
    uint32_t uncSize = message.data.size();

    size_t outSize = m_transport->m_zstdCompressor.compressBound(uncSize);
    std::vector<uint8_t> compressedData(outSize);

    GEODE_UNWRAP(m_transport->m_zstdCompressor.compress(
        message.data.data(), uncSize,
        compressedData.data(), outSize
    ));

    compressedData.resize(outSize);
    message.data = std::move(compressedData);

    message.compHeader = CompressionHeader {
        .type = CompressionType::Zstd,
        .uncompressedSize = uncSize
    };

    log::debug("Zstd compressed outgoing message: {} -> {} bytes", uncSize, outSize);

    return Ok();
}

CompressorResult<> Socket::doCompressLz4(DataMessage& message) const {
    return Err(CompressorError::NotInitialized);
}

TransportResult<QunetMessage> Socket::receiveMessage(const std::optional<Duration>& timeout) {
    bool available = this->messageAvailable();

    auto started = Instant::now();

    while (!available) {
        std::optional<Duration> remaining = timeout ? std::optional(*timeout - started.elapsed()) : std::nullopt;
        auto pollRes = GEODE_UNWRAP(m_transport->poll(remaining));

        if (!pollRes) {
            return Err(TransportError::TimedOut);
        }

        available = GEODE_UNWRAP(this->processIncomingData());
    }

    return m_transport->receiveMessage();
}

TransportResult<bool> Socket::processIncomingData() {
    // check if there's any data available to read
    bool hasData = GEODE_UNWRAP(m_transport->poll(Duration{}));

    return hasData ? m_transport->processIncomingData() : Ok(this->messageAvailable());
}

bool Socket::messageAvailable() {
    return m_transport->messageAvailable();
}

Duration Socket::getLatency() const {
    return m_transport->getLatency();
}

Duration Socket::untilTimerExpiry() const {
    return m_transport->untilTimerExpiry();
}

TransportResult<> Socket::handleTimerExpiry() {
    return m_transport->handleTimerExpiry();
}

TransportResult<std::shared_ptr<BaseTransport>> Socket::createTransport(const TransportOptions& options) {
    switch (options.type) {
        case ConnectionType::Udp: {
            auto transport = GEODE_UNWRAP(UdpTransport::connect(options.address, *options.connOptions));
            auto ptr = std::make_shared<UdpTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

        case ConnectionType::Tcp: {
            auto transport = GEODE_UNWRAP(TcpTransport::connect(
                options.address,
                options.timeout,
                *options.connOptions
            ));
            auto ptr = std::make_shared<TcpTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

#ifdef QUNET_QUIC_SUPPORT
        case ConnectionType::Quic: {
            auto transport = GEODE_UNWRAP(QuicTransport::connect(
                options.address,
                options.timeout,
                options.tlsContext,
                options.connOptions
            ));
            auto ptr = std::make_shared<QuicTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;
#endif

        default: {
            return Err(TransportError::NotImplemented);
        }
    }
}

std::shared_ptr<BaseTransport> Socket::transport() const {
    return m_transport;
}

}
