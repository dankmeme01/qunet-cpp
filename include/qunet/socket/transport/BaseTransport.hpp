#pragma once

#include "Error.hpp"
#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/compression/ZstdDecompressor.hpp>
#include <qunet/compression/ZstdCompressor.hpp>

#include <qsox/Error.hpp>
#include <asp/time/Duration.hpp>
#include <stdint.h>
#include <queue>

namespace qn {

class BaseTransport {
public:
    BaseTransport() = default;
    BaseTransport(BaseTransport&&) = default;
    BaseTransport& operator=(BaseTransport&&) = default;

    virtual ~BaseTransport() = default;
    virtual TransportResult<> sendMessage(QunetMessage message) = 0;

    /// Sends the qunet hadnshake to the server and waits for a response.
    /// The default implementation should only be used in reliable and ordered transports,
    /// it will return the first message as soon as it is received.
    virtual TransportResult<QunetMessage> performHandshake(
        HandshakeStartMessage handshakeStart,
        const std::optional<asp::time::Duration>& timeout
    );

    /// Polls until any kind of data is available to be read.
    virtual TransportResult<bool> poll(const std::optional<asp::time::Duration>& dur) = 0;

    /// Processes incoming data from the transport. This function may block until data is available,
    /// but it will only block for a single read call, rather than until a whole message is available.
    /// Returns whether an entire message is available to be read with `receiveMessage()`.
    virtual TransportResult<bool> processIncomingData() = 0;

    /// Returns whether there is a message available to be read from the transport.
    virtual bool messageAvailable();

    /// Receives a message from the transport. If no message is available, this will block until a message is received or an error occurs.
    virtual TransportResult<QunetMessage> receiveMessage();

    // Closes the transport. This method may or may not block until the transport is fully closed.
    // This does not send a `ClientClose` message.
    // After invoking, keep calling `isClosed()` to check if the transport is fully closed.
    virtual TransportResult<> close() = 0;
    virtual bool isClosed() const = 0;

    virtual void setConnectionId(uint64_t connectionId);
    virtual void setMessageSizeLimit(size_t limit);

protected:
    std::queue<QunetMessage> m_recvMsgQueue;
    uint64_t m_connectionId = 0;
    size_t m_messageSizeLimit = -1;

    // compressors
    ZstdCompressor m_zstdCompressor;
    ZstdDecompressor m_zstdDecompressor;
};

}
