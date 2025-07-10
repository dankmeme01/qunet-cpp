#pragma once
#include <stdint.h>
#include <stddef.h>

namespace qn {

constexpr inline uint16_t MAJOR_VERSION = 1;

constexpr inline uint8_t MSG_PING = 1;
constexpr inline uint8_t MSG_PONG = 2;
constexpr inline uint8_t MSG_KEEPALIVE = 3;
constexpr inline uint8_t MSG_KEEPALIVE_RESPONSE = 4;
constexpr inline uint8_t MSG_HANDSHAKE_START = 5;
constexpr inline uint8_t MSG_HANDSHAKE_FINISH = 6;
constexpr inline uint8_t MSG_HANDSHAKE_FAILURE = 7;
constexpr inline uint8_t MSG_CLIENT_CLOSE = 8;
constexpr inline uint8_t MSG_SERVER_CLOSE = 9;
constexpr inline uint8_t MSG_CLIENT_RECONNECT = 10;
constexpr inline uint8_t MSG_CONNECTION_ERROR = 11;
constexpr inline uint8_t MSG_QDB_CHUNK_REQUEST = 12;
constexpr inline uint8_t MSG_QDB_CHUNK_RESPONSE = 13;

constexpr inline uint8_t MSG_QDBG_TOGGLE = 64;
constexpr inline uint8_t MSG_QDBG_REPORT = 65;

constexpr inline uint8_t MSG_DATA = 0x80;
constexpr inline uint8_t MSG_DATA_START = 0x80;
constexpr inline uint8_t MSG_DATA_END = 0xff;
constexpr inline uint8_t MSG_DATA_MASK = MSG_DATA;

constexpr inline size_t MSG_DATA_BIT_COMPRESSION_1 = 0; // least significant bit
constexpr inline size_t MSG_DATA_BIT_COMPRESSION_2 = 1; // second least significant bit
constexpr inline size_t MSG_DATA_RELIABILITY_MASK = 1 << 4;
constexpr inline size_t MSG_DATA_FRAGMENTATION_MASK = 1 << 5;

constexpr inline uint16_t MSG_DATA_LAST_FRAGMENT_MASK = 0x8000; // most significant bit of fragment index

constexpr inline int MSG_ZSTD_COMPRESSION_LEVEL = 3;

constexpr inline uint8_t PROTO_TCP = 0x01;
constexpr inline uint8_t PROTO_UDP = 0x02;
constexpr inline uint8_t PROTO_QUIC = 0x03;
constexpr inline uint8_t PROTO_WEBSOCKET = 0x04;

constexpr inline uint16_t DEFAULT_PORT = 4340;

constexpr inline size_t UDP_PACKET_LIMIT = 1400;

constexpr inline size_t HANDSHAKE_START_SIZE = 1 + 2 + 2 + 16; // header, qunet major, frag limit, qdb hash
constexpr inline size_t HANDSHAKE_HEADER_SIZE = 9; // connection ID (u64) + qdb presence (bool)
constexpr inline size_t HANDSHAKE_HEADER_SIZE_WITH_QDB = HANDSHAKE_HEADER_SIZE + 16; // four u32s: uncompressed size, qdb size, chunk offset, chunk size

}
