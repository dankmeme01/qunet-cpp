#include <qunet/socket/transport/udp/FragmentStore.hpp>
#include <qunet/Log.hpp>
#include <asp/time/Duration.hpp>
#include <algorithm>

using namespace asp::time;

namespace qn {

FragmentStore::FragmentStore() {}

FragmentStore::~FragmentStore() {}

TransportResult<std::optional<QunetMessageMeta>> FragmentStore::processFragment(
    QunetMessageMeta&& message
) {
    QN_ASSERT(message.fragmentationHeader.has_value());
    auto& frh = *message.fragmentationHeader;

    auto& fmsg = m_messages[frh.messageId];

    // check if it's a duplicate
    auto it = std::find_if(fmsg.fragments.begin(), fmsg.fragments.end(), [&](const Fragment& f) {
        return f.index == frh.fragmentIndex;
    });

    if (it != fmsg.fragments.end()) {
        // duplicate fragment, ignore it
        return Ok(std::nullopt);
    }

    // if this is the last fragment, we can set the fragment count
    if (frh.lastFragment) {
        fmsg.totalFragmentCount = frh.fragmentIndex + 1;
    }

    Fragment fragment {
        .index = frh.fragmentIndex,
        .data = std::move(message.data),
    };

    fmsg.fragments.push_back(std::move(fragment));

    // if some headers are present, store them
    if (message.compressionHeader.has_value()) {
        fmsg.compressionHeader = message.compressionHeader;
    }

    if (message.reliabilityHeader.has_value()) {
        fmsg.reliabilityHeader = message.reliabilityHeader;
    }

    // processStored internally will handle two things:
    // 1. If a message is complete, it will reassemble and return it
    // 2. If there are any messages that have been stale for some period of time, they will be removed

    // it is *slightly* inefficient to call this every time, but it is a lot simpler than maintaining some separate timer

    return this->processStored();
}

TransportResult<std::optional<QunetMessageMeta>> FragmentStore::processStored() {
    constexpr static Duration EXPIRY = Duration::fromSecs(5);

    for (auto it = m_messages.begin(); it != m_messages.end();) {
        auto& msg = it->second;

        if (msg.totalFragmentCount == msg.fragments.size()) {
            // retrieve the message and remove it from the store
            auto msg = std::move(it->second);
            it = m_messages.erase(it);

            return this->reassemble(std::move(msg));
        } else if (msg.receivedAt.elapsed() > EXPIRY) {
            // message is stale, remove it
            it = m_messages.erase(it);
        } else {
            ++it;
        }
    }

    return Ok(std::nullopt);
}

TransportResult<std::optional<QunetMessageMeta>> FragmentStore::reassemble(Message&& msg) {
    std::sort(msg.fragments.begin(), msg.fragments.end(), [](const Fragment& a, const Fragment& b) {
        return a.index < b.index;
    });

    // calculate total size and see if there's any skips
    size_t totalSize = 0;
    size_t expectedIndex = 0;

    for (auto& fragment : msg.fragments) {
        if (fragment.index != expectedIndex) {
            log::warn("Error defragmenting message, expected index {}, got {}", expectedIndex, fragment.index);
            return Err(TransportError::DefragmentationError);
        }

        expectedIndex++;
        totalSize += fragment.data.size();
    }

    QunetMessageMeta out{};
    out.data = std::vector<uint8_t>(totalSize);
    out.compressionHeader = msg.compressionHeader;
    out.reliabilityHeader = msg.reliabilityHeader;
    out.fragmentationHeader = std::nullopt;

    auto curdata = out.data.data();

    for (auto& fragment : msg.fragments) {
        QN_ASSERT(curdata + fragment.data.size() <= out.data.data() + out.data.size());

        std::memcpy(curdata, fragment.data.data(), fragment.data.size());
        curdata += fragment.data.size();
    }

    QN_ASSERT(curdata == out.data.data() + out.data.size());

    return Ok(std::move(out));
}

uint16_t FragmentStore::nextMessageId() {
    return m_nextMessageId++;
}

}