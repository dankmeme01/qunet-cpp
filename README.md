# Qunet

Advanced asynchronous C++ networking library, powered by [Arc](https://github.com/dankmeme01/arc) and used by [Globed](https://github.com/GlobedGD/globed2).

This library is available in Rust: [dankmeme01/qunet](https://github.com/dankmeme01/qunet)

## Features

* Support for three underlying transports: TCP, QUIC and Reliable UDP
* UDP transport allows for choosing reliability per-message, perfect for games and other low latency cases
* Handles UDP fragmentation at protocol level
* Full support of IPv4 and IPv6
* Tiny data overhead: one handshake and then just 1 byte per data message (if no extra headers are present)
* ZSTD and LZ4 compression of messages, applied per-message
* Extensive DNS lookups, including SRV queries
* Smart logic for connection & reconnection, including [happy eyeballs](https://datatracker.ietf.org/doc/html/rfc6555)
* Buffers for efficient data manipulation: `ByteWriter`, `HeapByteWriter`, `ArrayByteWriter`, `CircularByteBuffer`, `ByteReader`
* Statistics tracking (bytes/messages sent, compression ratios, etc.)
* Highly confgiruable, including being able to disable some features at compile time, e.g. QUIC or advanced DNS resolver.

## Usage

This section is incomplete as there's a ton to describe here :)

See [tester/main.cpp](./tester/main.cpp) for some example usage. Qunet is an Arc-based async library, and thus an Arc runtime is needed to create a connection. Other calls like `sendData()` are not required to be in the context of a runtime, only those that return a `Future` are.
