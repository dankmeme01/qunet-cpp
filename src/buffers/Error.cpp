#include <qunet/buffers/Error.hpp>

namespace qn {

std::string_view ByteReaderError::message() const {
    // this crashes clang on macos/android
    // using enum Code;

    switch (m_code) {
        case Code::OutOfBoundsRead: return "ByteReader out of bounds read";
        case Code::VarintOverflow: return "Tried reading a varint that cannot fit into a 64-bit integer";
        case Code::StringTooLong: return "Tried decoding a string longer than the maximum permitted size";
    }
}

std::string_view ByteWriterError::message() const {
    // this crashes clang on macos/android
    // using enum Code;

    switch (m_code) {
        case Code::OutOfBoundsWrite: return "ByteWriter out of bounds write";
        case Code::StringTooLong: return "Tried encoding a string longer than the maximum permitted size";
    }
}

}