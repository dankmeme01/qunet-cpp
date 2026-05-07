#include <gtest/gtest.h>
#include <qunet/socket/message/QunetMessage.hpp>

using namespace qn;

template <typename T>
T expectValue(std::optional<T> opt, std::string_view what) {
    EXPECT_TRUE(opt.has_value()) << "Expected value, got nullopt (" << what << ")";
    return std::move(opt.value());
}

template <typename T, typename U>
T expectValue(Result<T, U> res, std::string_view what) {
    EXPECT_TRUE(res.isOk()) << fmt::format("expected Ok, got Err: {} ({})", res.unwrapErr(), what);
    return std::move(res.unwrap());
}

TEST(QunetMessage, MultiMsgIterator) {
    uint8_t data[] = {
        // keepalive response
        0x4,
        0x63, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
        0x0, 0x0,
        // data (6 bytes)
        0x88, 0x06, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        // connection control (SetMTU = 0x578 aka 1400)
        0x10, 0x01, 0x00, 0x78, 0x05,
        // data (4 bytes)
        0x80, 0x01, 0x02, 0x03, 0x04,
    };

    QunetUdpMessageIter iter{{data, sizeof(data)}, false};

    auto first = QunetMessage::decodeWithMeta(expectValue(expectValue(iter.next(), "first message"), "first message")).unwrap();
    EXPECT_TRUE(first.is<KeepaliveResponseMessage>());
    EXPECT_EQ(first.as<KeepaliveResponseMessage>().timestamp, 0x63);

    auto second = expectValue(expectValue(iter.next(), "second message"), "second message");
    EXPECT_EQ(second.type, MSG_DATA);
    auto secondData = second.data;
    EXPECT_EQ(secondData, std::vector<uint8_t>({1, 2, 3, 4, 5, 6}));

    auto third = QunetMessage::decodeWithMeta(expectValue(expectValue(iter.next(), "third message"), "third message")).unwrap();
    EXPECT_TRUE(third.is<ConnectionControlMessage>());
    auto& ccmsg = third.as<ConnectionControlMessage>();
    EXPECT_TRUE(ccmsg.is<ConnectionControlMessage::SetMTU>());
    auto& setMtuMsg = ccmsg.as<ConnectionControlMessage::SetMTU>();
    EXPECT_EQ(setMtuMsg.mtu, 1400);

    auto fourth = expectValue(expectValue(iter.next(), "fourth message"), "fourth message");
    EXPECT_EQ(fourth.type, MSG_DATA);
    auto fourthData = fourth.data;
    EXPECT_EQ(fourthData, std::vector<uint8_t>({1, 2, 3, 4}));

    EXPECT_FALSE(iter.next().has_value()) << "Expected no more messages, but got one";
}


TEST(QunetMessage, Padding) {
    uint8_t data[] = {
        // keepalive header
        0x3,
        // connection id
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        // keepalive data
        0x63, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
        // padding (header + 3 bytes, pads to 22 in total)
        0x7f, 0x67, 0x67, 0x67,
    };

    QunetUdpMessageIter iter{{data, sizeof(data)}, true};

    auto first = QunetMessage::decodeWithMeta(expectValue(expectValue(iter.next(), "first message"), "first message")).unwrap();
    EXPECT_TRUE(first.is<KeepaliveMessage>());
    EXPECT_EQ(first.as<KeepaliveMessage>().timestamp, 0x63);

    auto second = QunetMessage::decodeWithMeta(expectValue(expectValue(iter.next(), "second message"), "second message")).unwrap();
    EXPECT_TRUE(second.is<PaddingMessage>());

    EXPECT_FALSE(iter.next().has_value()) << "Expected no more messages, but got one";
}
