#ifdef QUNET_WS_SUPPORT

#include <qunet/socket/transport/WsTransport.hpp>
#include <dbuf/ByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>

#include <arc/time/Timeout.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](auto e) { return WsError{std::move(e)}; }))
#define MAP_CO_UNWRAP(x) GEODE_CO_UNWRAP((x).mapErr([](auto e) { return WsError{std::move(e)}; }))

using namespace arc;
using namespace asp::time;

namespace qn {

WsTransport::WsTransport(wsx::AsyncClient ws) : m_ws(std::move(ws)) {}

WsTransport::~WsTransport() {}

Future<TransportResult<WsTransport>> WsTransport::connect(
    const Duration& timeout,
    const ConnectionOptions& connOptions,
    const wsx::ClientConnectOptions& opts
) {
    auto res = co_await arc::timeout(
        timeout,
        wsx::AsyncClient::connect(opts)
    );
    if (!res) {
        co_return Err(TransportError::ConnectionTimedOut);
    }

    auto client = MAP_CO_UNWRAP(std::move(res).unwrap());

    WsTransport ret(std::move(client));
    ret.m_activeKeepaliveInterval = connOptions.activeKeepaliveInterval;

    co_return Ok(std::move(ret));
}

Future<TransportResult<>> WsTransport::close() {
    if (m_ws.isConnected()) {
        MAP_CO_UNWRAP(co_await m_ws.closeNoAck());
    }
    co_return Ok();
}

TransportResult<> WsTransport::closeSync() {
    if (m_ws.isConnected()) {
        MAP_UNWRAP(m_ws.closeSync());
    }
    return Ok();
}

bool WsTransport::isClosed() const {
    return !m_ws.isConnected();
}

Future<TransportResult<>> WsTransport::sendMessage(QunetMessage message, SentMessageContext& ctx) {
    this->updateLastActivity();

    if (message.is<KeepaliveMessage>()) {
        message.as<KeepaliveMessage>().timestamp = this->getKeepaliveTimestamp();
        this->updateLastSentKeepalive();
    }

    dbuf::ByteWriter writer;

    if (!message.is<DataMessage>()) {
        // non-data message
        ARC_CO_UNWRAP(message.encodeControlMsg(writer, 0));
    } else {
        // data message
        auto& msg = message.as<DataMessage>();
        message.encodeDataHeader(writer, 0, false).unwrap();
        writer.writeBytes(msg.data);
    }

    auto data = writer.written();
    MAP_CO_UNWRAP(co_await m_ws.send(wsx::Message{data}));

    this->_tracker().onUpPacket(data.size());

    co_return Ok();
}

Future<TransportResult<>> WsTransport::poll() {
    co_return Err(TransportError::NotImplemented);
}

Future<TransportResult<QunetMessage>> WsTransport::receiveMessage() {
    auto msg = MAP_CO_UNWRAP(co_await m_ws.recv());
    if (msg.isClose()) {
        log::info("WebSocket closed by peer: {}, {}", msg.closeCode(), msg.closeReason());
        co_return Err(WsError{fmt::format("WebSocket closed by peer: {} (code {})", msg.closeReason(), msg.closeCode())});
    }

    if (!msg.isBinary()) {
        // enby :3
        log::warn("Received non-binary message over WebSocket!");
        co_return Err(TransportError::UnexpectedMessage);
    }

    auto data = msg.binary();
    dbuf::ByteReader reader{data};
    auto meta = GEODE_CO_UNWRAP(QunetMessage::decodeMeta(reader));

    if (meta.type != MSG_DATA) {
        auto msg = GEODE_CO_UNWRAP(QunetMessage::decodeWithMeta(std::move(meta)));
        this->_tracker().onDownMessage(data[0], data.size());
        co_return Ok(std::move(msg));
    }

    co_return this->decodePreFinalDataMessage(std::move(meta));
}

asp::time::Duration WsTransport::untilTimerExpiry() const {
    return this->untilKeepalive();
}

Future<TransportResult<>> WsTransport::handleTimerExpiry() {
    ARC_CO_UNWRAP(co_await BaseTransport::handleTimerExpiry());
    co_return co_await this->sendMessage(KeepaliveMessage{});
}

Duration WsTransport::untilKeepalive() const {
    // similar to UDP, we send more keepalives at the start to figure out the latency
    if (m_unackedKeepalives > 0) {
        return Duration::fromSecs(3) - this->sinceLastActivity();
    }

    auto orActive = [&](const Duration& dur) {
        if (m_activeKeepaliveInterval) {
            return std::min(dur, *m_activeKeepaliveInterval) - this->sinceLastKeepalive();
        } else {
            return dur - this->sinceLastKeepalive();
        }
    };

    switch (m_totalRttEstimates) {
        case 0:
        case 1:
            return orActive(Duration::fromSecs(3));
        case 2:
            return orActive(Duration::fromSecs(10));
        case 3:
            return orActive(Duration::fromSecs(25));
        default: {
            if (m_activeKeepaliveInterval) {
                return std::min(
                    Duration::fromSecs(45) - this->sinceLastActivity(),
                    *m_activeKeepaliveInterval - this->sinceLastKeepalive()
                );
            } else {
                return Duration::fromSecs(45) - this->sinceLastActivity();
            }
        }
    }
}

}

#endif
