#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/socket/transport/QuicTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/database/QunetDatabase.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include <qunet/Connection.hpp>
#include <qunet/util/hash.hpp>

#include <fmt/ranges.h>
#include <asp/time/Instant.hpp>

using namespace arc;
using namespace asp::time;

namespace qn {

Future<TransportResult<std::pair<Socket, Duration>>> Socket::createSocket(const TransportOptions& options) {
    ARC_FRAME();

    auto startedAt = Instant::now();

    StatTracker tracker;
    tracker.onStartedConnection();
    tracker.setEnabled(options.connOptions->debug.recordStats);

    auto transport = ARC_CO_UNWRAP(co_await Socket::createTransport(options));

    if (startedAt.elapsed() > options.timeout) {
        (void) co_await transport->close();
        co_return Err(TransportError::ConnectionTimedOut);
    }

    tracker.onConnected();
    transport->m_tracker = std::move(tracker);

    Socket socket(std::move(transport), options.address);

    auto handshakeTimeout = options.timeout - startedAt.elapsed();
    if (handshakeTimeout.millis() <= 0) {
        (void) co_await transport->close();
        co_return Err(TransportError::ConnectionTimedOut);
    }

    co_return Ok(std::pair{std::move(socket), handshakeTimeout});
}

arc::Future<TransportResult<Socket>> Socket::connect(
    const TransportOptions& options,
    std::optional<std::filesystem::path> qdbFolder
) {
    ARC_FRAME();

    auto [socket, timeout] = ARC_CO_UNWRAP(co_await createSocket(options));

    if (qdbFolder) {
        socket.m_usedQdb = co_await arc::spawnBlocking<std::optional<QunetDatabase>>([&] {
            return qn::tryFindQdb(*qdbFolder, options.address);
        });
        socket.m_qdbFolder = std::move(*qdbFolder);
    }

    // this will be set to false at the very end if successful
    bool closeSocket = true;
    auto _dtor = scopeDtor([&] {
        if (closeSocket) {
            (void) socket.closeSync();
        }
    });

    std::array<uint8_t, 16> qdbHash = {};
    if (socket.m_usedQdb) {
        qdbHash = socket.m_usedQdb->getHash();
        log::debug("Using qunet database with hash {}", qn::hexEncode(qdbHash.data(), 16));
    }

    auto hmsg = HandshakeStartMessage {
        .majorVersion = MAJOR_VERSION,
        .fragLimit = UDP_PACKET_LIMIT,
        .qdbHash = qdbHash,
    };

    auto tres = co_await arc::timeout(
        timeout,
        socket.m_transport->performHandshake(std::move(hmsg))
    );
    if (!tres) {
        co_return Err(TransportError::ConnectionTimedOut);
    }

    auto msg = ARC_CO_UNWRAP(std::move(tres).unwrap());

    if (msg.is<HandshakeFinishMessage>()) {
        auto& hf = msg.as<HandshakeFinishMessage>();
        ARC_CO_UNWRAP(co_await socket.onHandshakeSuccess(hf));
    } else if (msg.is<HandshakeFailureMessage>()) {
        auto& hf = msg.as<HandshakeFailureMessage>();
        log::warn("Handshake failed: {}", hf.message());

        co_return Err(TransportError::HandshakeFailure(std::string(hf.message())));
    } else {
        co_return Err(TransportError::UnexpectedMessage);
    }

    closeSocket = false;
    co_return Ok(std::move(socket));
}

Future<TransportResult<Socket>> Socket::reconnect(const TransportOptions& options, Socket& prev) {
    ARC_FRAME();

    auto [socket, timeout] = ARC_CO_UNWRAP(co_await createSocket(options));

    // this will be set to false at the very end if successful
    bool closeSocket = true;
    auto _dtor = scopeDtor([&] {
        if (closeSocket) {
            (void) socket.closeSync();
        }
    });

    auto tres = co_await arc::timeout(
        timeout,
        socket.m_transport->performReconnect(prev.transport()->m_connectionId)
    );
    if (!tres) {
        co_return Err(TransportError::ConnectionTimedOut);
    }

    auto msg = ARC_CO_UNWRAP(std::move(tres).unwrap());

    if (msg.is<ReconnectSuccessMessage>()) {
        ARC_CO_UNWRAP(socket.onReconnectSuccess(prev));
    } else if (msg.is<ReconnectFailureMessage>()) {
        log::warn("Reconnect failed!");
        co_return Err(TransportError::ReconnectFailed);
    } else {
        co_return Err(TransportError::UnexpectedMessage);
    }

    closeSocket = false;
    co_return Ok(std::move(socket));
}

Future<TransportResult<>> Socket::onHandshakeSuccess(const HandshakeFinishMessage& msg) {
    ARC_FRAME();

    log::debug("Handshake finished, connection ID: {}, qdb size: {}", msg.connectionId, msg.qdbData ? msg.qdbData->uncompressedSize : 0);
    m_transport->setConnectionId(msg.connectionId);

    if (msg.qdbData) {
        // qdb data is also zstd compressed, decompress it first
        ZstdDecompressor dec;
        dec.init().unwrap();

        size_t realSize = msg.qdbData->uncompressedSize;
        std::vector<uint8_t> qdbData(realSize);

        ARC_CO_UNWRAP(dec.decompress(msg.qdbData->chunkData.data(), msg.qdbData->chunkData.size(), qdbData.data(), realSize));

        auto qdb = ARC_CO_UNWRAP(QunetDatabase::decode(qdbData).mapErr([&](const DatabaseDecodeError& err) {
            log::warn("Failed to decode Qunet database: {}", err);
            return TransportError::InvalidQunetDatabase;
        }));

        ARC_CO_UNWRAP(m_transport->initCompressors(&qdb));

        // save the qdb
        if (m_qdbFolder) {
            log::debug("Saving Qunet database to {:?}", *m_qdbFolder);

            auto res = co_await arc::spawnBlocking<geode::Result<>>([&] {
                return qn::saveQdb(qdbData, *m_qdbFolder, m_remoteAddress);
            });

            if (!res) {
                log::warn("Failed to save Qunet database: {}", res.unwrapErr());
            }
        }
    } else if (m_usedQdb) {
        ARC_CO_UNWRAP(m_transport->initCompressors(&*m_usedQdb));
        m_usedQdb.reset(); // no longer needed
    } else {
        ARC_CO_UNWRAP(m_transport->initCompressors(nullptr));
    }

    co_return Ok();
}

TransportResult<> Socket::onReconnectSuccess(Socket& older) {
    log::debug("Reconnect finished, connection ID: {}", older.m_transport->m_connectionId);
    m_transport->setConnectionId(older.m_transport->m_connectionId);
    m_transport->setMessageSizeLimit(older.m_transport->m_messageSizeLimit);
    m_transport->m_zstdCompressor = std::move(older.m_transport->m_zstdCompressor);
    m_transport->m_zstdDecompressor = std::move(older.m_transport->m_zstdDecompressor);
    m_transport->m_lz4Compressor = std::move(older.m_transport->m_lz4Compressor);
    m_transport->m_lz4Decompressor = std::move(older.m_transport->m_lz4Decompressor);

    return Ok();
}

Future<TransportResult<>> Socket::close() {
    return m_transport->close();
}

TransportResult<> Socket::closeSync() {
    return m_transport->closeSync();
}

bool Socket::isClosed() const {
    return m_transport->isClosed();
}

Future<TransportResult<>> Socket::sendMessage(OutgoingMessage outgoing) {
    ARC_FRAME();

    // determine if the message needs to be compressed
    CompressionType ctype = CompressionType::None;
    auto& message = outgoing.message;

    if (message.is<DataMessage>()) {
        auto& msg = message.as<DataMessage>();

        if (!outgoing.uncompressed) {
            ctype = this->shouldCompress(msg.data);
        }

        switch (ctype) {
            case CompressionType::Zstd: {
                ARC_CO_UNWRAP(this->doCompressZstd(msg, true));
            } break;

            case CompressionType::ZstdNoDict: {
                ARC_CO_UNWRAP(this->doCompressZstd(msg, false));
            } break;

            case CompressionType::Lz4: {
                ARC_CO_UNWRAP(this->doCompressLz4(msg));
            } break;

            default: break;
        }
    }

    log::debug(
        "Socket: sending message: {} (tag: {}) (reliable: {}, compressed: {})",
        message.typeStr(), outgoing.tag, outgoing.reliable, ctype != CompressionType::None
    );

    SentMessageContext ctx;
    ctx.reliable = outgoing.reliable;
    ctx.tag = std::move(outgoing.tag);
    auto headerByte = message.headerByte();

    if (message.is<DataMessage>()) {
        auto& msg = message.as<DataMessage>();
        if (msg.compHeader) {
            ctx.originalSize = msg.compHeader->uncompressedSize;
            ctx.compressedSize = msg.data.size();
        } else {
            ctx.originalSize = msg.data.size();
        }
    } else {
        ctx.originalSize = 1; // this does not include udp connection id
    }

    auto res = co_await m_transport->sendMessage(std::move(message), ctx);

    if (res) {
        m_transport->logOutgoingMessage(headerByte, ctx);
    }

    co_return res;
}

// similar to adaptive compression in qunet
CompressionType Socket::shouldCompress(std::span<const uint8_t> data) const {
    if (data.size() < 128) {
        return CompressionType::None;
    }

    // try compressing with lz4
    std::unique_ptr<uint8_t[]> heapPtr;
    uint8_t stackArr[4096];
    size_t destCap = Lz4Compressor::compressBound(data.size());
    uint8_t* ptr;

    if (destCap <= sizeof(stackArr)) {
        ptr = stackArr;
    } else {
        heapPtr = std::make_unique<uint8_t[]>(destCap);
        ptr = heapPtr.get();
    }

    size_t compSize = destCap;
    m_transport->m_lz4Compressor.compress(data.data(), data.size(), ptr, compSize).unwrap();

    // if lz4 is ineffective, don't compress until a certain point
    if (compSize >= data.size() && data.size() < 1024) {
        return CompressionType::None;
    }

    // use zstd for larger packets
    if (data.size() >= 512) {
        return CompressionType::Zstd;
    }

    // use zstd if the packet is slightly compressible
    if (compSize + compSize / 16 < data.size()) {
        return CompressionType::Zstd;
    }

    // use lz4 otherwise
    return CompressionType::Lz4;
}

static CompressorResult<> doCompress(auto& compressor, CompressionType type, DataMessage& message) {
    uint32_t uncSize = message.data.size();

    size_t outSize = compressor.compressBound(uncSize);
    std::vector<uint8_t> compressedData(outSize);

    if constexpr (std::is_same_v<std::decay_t<decltype(compressor)>, ZstdCompressor>) {
        GEODE_UNWRAP(compressor.compress(
            message.data.data(), uncSize,
            compressedData.data(), outSize,
            type == CompressionType::ZstdNoDict
        ));
    } else {
        GEODE_UNWRAP(compressor.compress(
            message.data.data(), uncSize,
            compressedData.data(), outSize
        ));
    }

    log::debug("Compressed outgoing message ({}): {} -> {} bytes", type, uncSize, outSize);

    // if compression did not help, leave the message uncompressed
    if (outSize >= uncSize) {
        log::debug("Compression did not reduce size, leaving message uncompressed");
        return Ok();
    }

    compressedData.resize(outSize);
    message.data = std::move(compressedData);

    message.compHeader = CompressionHeader {
        .type = type,
        .uncompressedSize = uncSize
    };

    return Ok();
}

CompressorResult<> Socket::doCompressZstd(DataMessage& message, bool useDict) const {
    return doCompress(
        m_transport->m_zstdCompressor,
        useDict ? CompressionType::Zstd : CompressionType::ZstdNoDict,
        message
    );
}

CompressorResult<> Socket::doCompressLz4(DataMessage& message) const {
    return doCompress(
        m_transport->m_lz4Compressor,
        CompressionType::Lz4,
        message
    );
}

Future<TransportResult<QunetMessage>> Socket::receiveMessage() {
    ARC_FRAME();
    auto msg = ARC_CO_UNWRAP(co_await m_transport->receiveMessage());
    m_transport->onIncomingMessage(msg);
    co_return Ok(std::move(msg));
}

Duration Socket::getLatency() const {
    return m_transport->getLatency();
}

Duration Socket::untilTimerExpiry() const {
    return m_transport->untilTimerExpiry();
}

Future<TransportResult<>> Socket::handleTimerExpiry() {
    return m_transport->handleTimerExpiry();
}

Future<TransportResult<std::shared_ptr<BaseTransport>>> Socket::createTransport(const TransportOptions& options) {
    switch (options.type) {
        case ConnectionType::Udp: {
            auto transport = ARC_CO_UNWRAP(co_await UdpTransport::connect(
                options.address,
                *options.connOptions
            ));
            auto ptr = std::make_shared<UdpTransport>(std::move(transport));
            co_return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

        case ConnectionType::Tcp: {
            auto transport = ARC_CO_UNWRAP(co_await TcpTransport::connect(
                options.address,
                options.timeout,
                *options.connOptions
            ));
            auto ptr = std::make_shared<TcpTransport>(std::move(transport));
            co_return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

#ifdef QUNET_QUIC_SUPPORT
        case ConnectionType::Quic: {
            auto transport = ARC_CO_UNWRAP(co_await QuicTransport::connect(
                options.address,
                options.timeout,
                std::static_pointer_cast<QuicTlsContext>(options.tlsContext),
                options.connOptions,
                options.hostname
            ));
            auto ptr = std::make_shared<QuicTransport>(std::move(transport));
            co_return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;
#endif

        default: {
            co_return Err(TransportError::NotImplemented);
        }
    }
}

std::shared_ptr<BaseTransport> Socket::transport() const {
    return m_transport;
}

}
