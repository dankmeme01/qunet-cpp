#pragma once

#include <qunet/socket/message/meta.hpp>
#include <qunet/socket/transport/Error.hpp>
#include <asp/time/Instant.hpp>
#include <unordered_map>

namespace qn {

class FragmentStore {
public:
    FragmentStore();
    ~FragmentStore();

    /// Accepts incoming message fragment. If a whole message is ready, it will be reassembled and returned, otherwise nullopt is returned.
    TransportResult<std::optional<QunetMessageMeta>> processFragment(
        QunetMessageMeta&& message
    );

    // Returns the next message ID for a new fragmented message to be sent.
    uint16_t nextMessageId();

private:
    struct Fragment {
        uint16_t index;
        std::vector<uint8_t> data;
    };

    struct Message {
        uint16_t messageId;
        asp::time::Instant receivedAt;
        size_t totalFragmentCount = -1; // -1 means unknown
        std::vector<Fragment> fragments;
        std::optional<CompressionHeader> compressionHeader;
        std::optional<ReliabilityHeader> reliabilityHeader;
    };

    std::unordered_map<uint16_t, Message> m_messages;
    uint16_t m_nextMessageId = 1;

    TransportResult<std::optional<QunetMessageMeta>> processStored();
    TransportResult<std::optional<QunetMessageMeta>> reassemble(Message&&);
};

}