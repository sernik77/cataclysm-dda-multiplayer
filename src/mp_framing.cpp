#include "mp_framing.h"

namespace mp
{

// --- Framing ----------------------------------------------------------------

static std::uint32_t read_be32( const char *data )
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>( data );
    return ( static_cast<std::uint32_t>( p[0] ) << 24 ) |
           ( static_cast<std::uint32_t>( p[1] ) << 16 ) |
           ( static_cast<std::uint32_t>( p[2] ) << 8 ) |
           static_cast<std::uint32_t>( p[3] );
}

static std::string oversize_error( std::uint32_t len, std::uint32_t limit )
{
    return "frame of " + std::to_string( len ) + " bytes exceeds limit of " +
           std::to_string( limit );
}

std::string encode_frame( const std::string &payload )
{
    const std::uint32_t len = static_cast<std::uint32_t>( payload.size() );
    std::string out;
    out.reserve( payload.size() + 4 );
    out.push_back( static_cast<char>( ( len >> 24 ) & 0xFFU ) );
    out.push_back( static_cast<char>( ( len >> 16 ) & 0xFFU ) );
    out.push_back( static_cast<char>( ( len >> 8 ) & 0xFFU ) );
    out.push_back( static_cast<char>( len & 0xFFU ) );
    out += payload;
    return out;
}

bool frame_decoder::feed( const char *data, size_t len )
{
    if( failed() ) {
        return false;
    }
    // Drop consumed bytes before growing the buffer.
    if( pos_ > 0 && pos_ * 2 >= buf_.size() ) {
        buf_.erase( 0, pos_ );
        pos_ = 0;
    }
    buf_.append( data, len );
    // Validate every header that is already buffered, so an oversized frame
    // is rejected before its body is buffered, even behind valid frames.
    size_t at = pos_;
    while( at + 4 <= buf_.size() ) {
        const std::uint32_t flen = read_be32( buf_.data() + at );
        if( flen > max_frame_ ) {
            error_ = oversize_error( flen, max_frame_ );
            return false;
        }
        at += 4 + static_cast<size_t>( flen );
    }
    return true;
}

std::optional<std::string> frame_decoder::next()
{
    if( failed() || buffered() < 4 ) {
        return std::nullopt;
    }
    const std::uint32_t flen = read_be32( buf_.data() + pos_ );
    if( flen > max_frame_ ) {
        error_ = oversize_error( flen, max_frame_ );
        return std::nullopt;
    }
    if( buffered() < 4 + static_cast<size_t>( flen ) ) {
        return std::nullopt;
    }
    std::string payload = buf_.substr( pos_ + 4, flen );
    pos_ += 4 + flen;
    if( pos_ == buf_.size() ) {
        buf_.clear();
        pos_ = 0;
    }
    return payload;
}

} // namespace mp
