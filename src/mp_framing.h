#pragma once
#ifndef CATA_SRC_MP_FRAMING_H
#define CATA_SRC_MP_FRAMING_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Multiplayer stream framing: a 4-byte big-endian payload length followed by
// the payload. Independent of JSON, so the transport can use it alone.
namespace mp
{

constexpr std::uint32_t max_frame_bytes = 16U * 1024U * 1024U;

std::string encode_frame( const std::string &payload );

// Splits a byte stream into frames. Handles frames that arrive in pieces.
class frame_decoder
{
    public:
        explicit frame_decoder( std::uint32_t max_frame = max_frame_bytes ) : max_frame_( max_frame ) {}

        // Appends bytes. Returns false once the stream is corrupt (a frame
        // longer than the limit); the connection must then be closed.
        bool feed( const char *data, size_t len );
        // Returns the next complete payload, if any.
        std::optional<std::string> next();

        bool failed() const {
            return !error_.empty();
        }
        const std::string &error() const {
            return error_;
        }
        size_t buffered() const {
            return buf_.size() - pos_;
        }

    private:
        std::uint32_t max_frame_;
        std::string buf_;
        size_t pos_ = 0;
        std::string error_;
};

} // namespace mp

#endif // CATA_SRC_MP_FRAMING_H
