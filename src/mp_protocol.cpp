#include "mp_protocol.h"

#include <algorithm>
#include <exception>
#include <sstream>
#include <utility>

#include "flexbuffer_json.h"
#include "json.h"
#include "json_loader.h"

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

// --- Text validation --------------------------------------------------------

static bool is_control( unsigned char c )
{
    return c < 0x20 || c == 0x7F;
}

bool valid_player_name( const std::string &name )
{
    if( name.empty() || name.size() > max_name_bytes ) {
        return false;
    }
    bool has_visible = false;
    for( const char ch : name ) {
        const unsigned char c = static_cast<unsigned char>( ch );
        if( is_control( c ) ) {
            return false;
        }
        if( c != ' ' ) {
            has_visible = true;
        }
    }
    return has_visible;
}

std::string sanitize_text( const std::string &text, size_t max_bytes )
{
    std::string out;
    out.reserve( std::min( text.size(), max_bytes ) );
    for( const char ch : text ) {
        if( !is_control( static_cast<unsigned char>( ch ) ) ) {
            out.push_back( ch );
        }
    }
    if( out.size() > max_bytes ) {
        size_t cut = max_bytes;
        // Do not cut a UTF-8 sequence in half: step back over continuation
        // bytes (10xxxxxx) and the lead byte that starts them.
        while( cut > 0 && ( static_cast<unsigned char>( out[cut] ) & 0xC0U ) == 0x80U ) {
            --cut;
        }
        out.resize( cut );
    }
    return out;
}

std::optional<std::string> check_hello( const hello_msg &h, const std::string &game_version,
                                        const std::string &mods_hash )
{
    if( h.proto != protocol_version ) {
        return "protocol version mismatch: host " + std::to_string( protocol_version ) +
               ", client " + std::to_string( h.proto );
    }
    if( h.game_version != game_version ) {
        return "game version mismatch: host " + game_version + ", client " +
               sanitize_text( h.game_version, 64 );
    }
    if( h.mods_hash != mods_hash ) {
        return "mod list mismatch";
    }
    if( !valid_player_name( h.name ) ) {
        return "invalid player name";
    }
    if( h.player_uuid.empty() || h.player_uuid.size() > 64 ) {
        return "invalid player id";
    }
    for( const char ch : h.player_uuid ) {
        const bool ok = ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'z' ) ||
                        ( ch >= 'A' && ch <= 'Z' ) || ch == '-';
        if( !ok ) {
            return "invalid player id";
        }
    }
    return std::nullopt;
}

// --- Enums ------------------------------------------------------------------

std::string to_string( action_type a )
{
    switch( a ) {
        case action_type::move:
            return "move";
        case action_type::wait:
            return "wait";
        case action_type::pause:
            return "pause";
        case action_type::ready:
            return "ready";
        case action_type::unready:
            return "unready";
        case action_type::tactical_on:
            return "tactical_on";
        case action_type::tactical_off:
            return "tactical_off";
        case action_type::activity:
            return "activity";
    }
    return "unknown";
}

std::optional<action_type> action_from_string( const std::string &s )
{
    static const std::pair<const char *, action_type> table[] = {
        { "move", action_type::move },
        { "wait", action_type::wait },
        { "pause", action_type::pause },
        { "ready", action_type::ready },
        { "unready", action_type::unready },
        { "tactical_on", action_type::tactical_on },
        { "tactical_off", action_type::tactical_off },
        { "activity", action_type::activity },
    };
    for( const auto &e : table ) {
        if( s == e.first ) {
            return e.second;
        }
    }
    return std::nullopt;
}

static std::optional<tempo_mode> mode_from_string( const std::string &s )
{
    for( tempo_mode m : {
             tempo_mode::tactical, tempo_mode::explore, tempo_mode::fast_forward
         } ) {
        if( s == to_string( m ) ) {
            return m;
        }
    }
    return std::nullopt;
}

// --- Serialization ----------------------------------------------------------

namespace
{
// Writes one JSON object: {"t": type, ...fill...}.
template<typename F>
std::string write_message( const char *type, F &&fill )
{
    std::ostringstream os;
    JsonOut jsout( os );
    jsout.start_object();
    jsout.member( "t", std::string( type ) );
    fill( jsout );
    jsout.end_object();
    return os.str();
}

// Parses payload as an object of the given type and calls read( obj ).
// Unknown members are allowed, so newer peers can add fields.
template<typename F>
std::string read_message( const std::string &payload, const char *type, F &&read )
{
    try {
        JsonValue root = json_loader::from_string( payload );
        JsonObject jo = root.get_object();
        jo.allow_omitted_members();
        if( !jo.has_string( "t" ) || jo.get_string( "t" ) != type ) {
            return std::string( "expected message type " ) + type;
        }
        return read( jo );
    } catch( const std::exception &e ) {
        return std::string( "malformed " ) + type + " message: " + e.what();
    }
}

std::string read_int64( const JsonObject &jo, const char *key, std::int64_t &out )
{
    if( !jo.has_number( key ) ) {
        return std::string( "missing number '" ) + key + "'";
    }
    out = jo.get_int64( key );
    return std::string();
}

std::string read_string( const JsonObject &jo, const char *key, std::string &out,
                         size_t max_bytes )
{
    if( !jo.has_string( key ) ) {
        return std::string( "missing string '" ) + key + "'";
    }
    out = jo.get_string( key );
    if( out.size() > max_bytes ) {
        return std::string( "string '" ) + key + "' too long";
    }
    return std::string();
}
} // namespace

std::string serialize( const hello_msg &m )
{
    return write_message( "hello", [&]( JsonOut & j ) {
        j.member( "proto", m.proto );
        j.member( "game_version", m.game_version );
        j.member( "mods_hash", m.mods_hash );
        j.member( "name", m.name );
        j.member( "player_uuid", m.player_uuid );
    } );
}

std::string serialize( const welcome_msg &m )
{
    return write_message( "welcome", [&]( JsonOut & j ) {
        j.member( "player_id", static_cast<std::int64_t>( m.id ) );
        j.member( "world_name", m.world_name );
        j.member( "pace_ms", m.pace_ms );
        j.member( "tactical_grace_ms", m.tactical_grace_ms );
    } );
}

std::string serialize( const reject_msg &m )
{
    return write_message( "reject", [&]( JsonOut & j ) {
        j.member( "reason", m.reason );
    } );
}

std::string serialize( const command_msg &m )
{
    return write_message( "cmd", [&]( JsonOut & j ) {
        j.member( "seq", m.seq );
        j.member( "turn", m.turn );
        j.member( "action", to_string( m.action ) );
        if( m.action == action_type::move ) {
            j.member( "dx", m.dx );
            j.member( "dy", m.dy );
            j.member( "dz", m.dz );
        }
        if( m.action == action_type::activity ) {
            j.member( "activity", m.activity );
        }
    } );
}

std::string serialize( const command_ack_msg &m )
{
    return write_message( "cmd_ack", [&]( JsonOut & j ) {
        j.member( "seq", m.seq );
        j.member( "ok", m.ok );
        if( !m.reason.empty() ) {
            j.member( "reason", m.reason );
        }
    } );
}

std::string serialize( const tick_msg &m )
{
    return write_message( "tick", [&]( JsonOut & j ) {
        j.member( "turn", m.turn );
        j.member( "mode", to_string( m.mode ) );
        j.member( "waiting_for" );
        j.start_array();
        for( const player_id id : m.waiting_for ) {
            j.write( static_cast<std::int64_t>( id ) );
        }
        j.end_array();
    } );
}

std::string serialize( const chat_msg &m )
{
    return write_message( "chat", [&]( JsonOut & j ) {
        j.member( "from", static_cast<std::int64_t>( m.from ) );
        j.member( "text", m.text );
    } );
}

std::string serialize( const bye_msg &m )
{
    return write_message( "bye", [&]( JsonOut & j ) {
        j.member( "reason", m.reason );
    } );
}

// --- Parsing ----------------------------------------------------------------

std::optional<std::string> message_type( const std::string &payload )
{
    try {
        JsonValue root = json_loader::from_string( payload );
        JsonObject jo = root.get_object();
        jo.allow_omitted_members();
        if( !jo.has_string( "t" ) ) {
            return std::nullopt;
        }
        return jo.get_string( "t" );
    } catch( const std::exception & ) {
        return std::nullopt;
    }
}

static std::string read_player_id( const JsonObject &jo, const char *key, player_id &out )
{
    std::int64_t v = 0;
    std::string err = read_int64( jo, key, v );
    if( !err.empty() ) {
        return err;
    }
    if( v < 0 || v > static_cast<std::int64_t>( UINT32_MAX ) ) {
        return std::string( "'" ) + key + "' out of range";
    }
    out = static_cast<player_id>( v );
    return std::string();
}

std::string parse( const std::string &payload, hello_msg &out )
{
    return read_message( payload, "hello", [&]( const JsonObject & jo ) {
        std::int64_t proto = 0;
        std::string err = read_int64( jo, "proto", proto );
        if( err.empty() ) {
            out.proto = proto < 0 || proto > 1000000 ? -1 : static_cast<int>( proto );
            err = read_string( jo, "game_version", out.game_version, 128 );
        }
        if( err.empty() ) {
            err = read_string( jo, "mods_hash", out.mods_hash, 128 );
        }
        if( err.empty() ) {
            err = read_string( jo, "name", out.name, max_name_bytes );
        }
        if( err.empty() ) {
            err = read_string( jo, "player_uuid", out.player_uuid, 64 );
        }
        return err;
    } );
}

std::string parse( const std::string &payload, welcome_msg &out )
{
    return read_message( payload, "welcome", [&]( const JsonObject & jo ) {
        std::string err = read_player_id( jo, "player_id", out.id );
        if( err.empty() ) {
            err = read_string( jo, "world_name", out.world_name, 256 );
        }
        if( err.empty() ) {
            err = read_int64( jo, "pace_ms", out.pace_ms );
        }
        if( err.empty() ) {
            err = read_int64( jo, "tactical_grace_ms", out.tactical_grace_ms );
        }
        return err;
    } );
}

std::string parse( const std::string &payload, reject_msg &out )
{
    return read_message( payload, "reject", [&]( const JsonObject & jo ) {
        std::string err = read_string( jo, "reason", out.reason, max_frame_bytes );
        out.reason = sanitize_text( out.reason, max_reason_bytes );
        return err;
    } );
}

std::string parse( const std::string &payload, command_msg &out )
{
    return read_message( payload, "cmd", [&]( const JsonObject & jo ) {
        std::string err = read_int64( jo, "seq", out.seq );
        if( err.empty() ) {
            err = read_int64( jo, "turn", out.turn );
        }
        std::string action;
        if( err.empty() ) {
            err = read_string( jo, "action", action, 32 );
        }
        if( !err.empty() ) {
            return err;
        }
        std::optional<action_type> a = action_from_string( action );
        if( !a ) {
            return "unknown action '" + sanitize_text( action, 32 ) + "'";
        }
        out.action = *a;
        if( out.action == action_type::move ) {
            std::int64_t d[3] = { 0, 0, 0 };
            const char *keys[3] = { "dx", "dy", "dz" };
            for( int i = 0; i < 3; ++i ) {
                err = read_int64( jo, keys[i], d[i] );
                if( !err.empty() ) {
                    return err;
                }
                if( d[i] < -1 || d[i] > 1 ) {
                    return std::string( "'" ) + keys[i] + "' out of range";
                }
            }
            if( d[0] == 0 && d[1] == 0 && d[2] == 0 ) {
                return std::string( "move without direction" );
            }
            out.dx = static_cast<int>( d[0] );
            out.dy = static_cast<int>( d[1] );
            out.dz = static_cast<int>( d[2] );
        }
        if( out.action == action_type::activity ) {
            err = read_string( jo, "activity", out.activity, max_activity_bytes );
        }
        return err;
    } );
}

std::string parse( const std::string &payload, command_ack_msg &out )
{
    return read_message( payload, "cmd_ack", [&]( const JsonObject & jo ) {
        std::string err = read_int64( jo, "seq", out.seq );
        if( err.empty() ) {
            if( !jo.has_bool( "ok" ) ) {
                return std::string( "missing bool 'ok'" );
            }
            out.ok = jo.get_bool( "ok" );
            out.reason.clear();
            if( jo.has_member( "reason" ) ) {
                err = read_string( jo, "reason", out.reason, max_frame_bytes );
                out.reason = sanitize_text( out.reason, max_reason_bytes );
            }
        }
        return err;
    } );
}

std::string parse( const std::string &payload, tick_msg &out )
{
    return read_message( payload, "tick", [&]( const JsonObject & jo ) {
        std::string err = read_int64( jo, "turn", out.turn );
        std::string mode;
        if( err.empty() ) {
            err = read_string( jo, "mode", mode, 32 );
        }
        if( !err.empty() ) {
            return err;
        }
        std::optional<tempo_mode> m = mode_from_string( mode );
        if( !m ) {
            return std::string( "unknown tempo mode" );
        }
        out.mode = *m;
        out.waiting_for.clear();
        if( !jo.has_array( "waiting_for" ) ) {
            return std::string( "missing array 'waiting_for'" );
        }
        for( JsonValue v : jo.get_array( "waiting_for" ) ) {
            const std::int64_t id = v.get_int64();
            if( id < 0 || id > static_cast<std::int64_t>( UINT32_MAX ) ) {
                return std::string( "'waiting_for' id out of range" );
            }
            out.waiting_for.push_back( static_cast<player_id>( id ) );
        }
        return std::string();
    } );
}

std::string parse( const std::string &payload, chat_msg &out )
{
    return read_message( payload, "chat", [&]( const JsonObject & jo ) {
        std::string err = read_player_id( jo, "from", out.from );
        if( err.empty() ) {
            err = read_string( jo, "text", out.text, max_frame_bytes );
            out.text = sanitize_text( out.text, max_chat_bytes );
        }
        return err;
    } );
}

std::string parse( const std::string &payload, bye_msg &out )
{
    return read_message( payload, "bye", [&]( const JsonObject & jo ) {
        std::string err = read_string( jo, "reason", out.reason, max_frame_bytes );
        out.reason = sanitize_text( out.reason, max_reason_bytes );
        return err;
    } );
}

} // namespace mp
