#include <qunet/compression/Error.hpp>

namespace qn {

std::string_view CompressorError::message() const {
    switch (m_code) {
        case NotInitialized: return "Compressor not initialized";
        case AlreadyInitialized: return "Compressor already initialized";
        case InvalidDictionary: return "Invalid dictionary for compressor";
        case CompressionFailed: return "Compression failed";
    }

    qn::unreachable();
}

std::string_view DecompressorError::message() const {
    switch (m_code) {
        case NotInitialized: return "Decompressor not initialized";
        case AlreadyInitialized: return "Decompressor already initialized";
        case InvalidDictionary: return "Invalid dictionary for decompressor";
        case DecompressionFailed: return "Decompression failed";
        case SizeMismatch: return "Decompressed size does not match expected size";
    }

    qn::unreachable();
}

}