#pragma once
#ifndef CATA_SRC_MP_PROTOCOL_H
#define CATA_SRC_MP_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mp_scheduler.h"

// Multiplayer wire protocol: framing, message types and validation.
// See doc/c++/MULTIPLAYER.md, section 5.
//
// A frame is a 4-byte big-endian payload length followed by a UTF-8 JSON
// object. Every object has a "t" member with the message type. The peer is
// never trusted: every parse function validates ranges and sizes and returns
// an error string instead of throwing.
namespace mp
{

// Bump when the wire format changes incompatibly.
constexpr int protocol_version = 1;
constexpr std::uint32_t max_frame_bytes = 16U * 1024U * 1024U;
constexpr size_t max_name_bytes = 32;
constexpr size_t max_chat_bytes = 512;
constexpr size_t max_activity_bytes = 64U * 1024U;
constexpr size_t max_reason_bytes = 256;

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

// --- Messages ---------------------------------------------------------------

struct hello_msg {
    int proto = protocol_version;
    std::string game_version;
    std::string mods_hash;
    std::string name;
    // Stable id kept by the client, used to rebind to the same character.
    std::string player_uuid;
    // Shared secret; empty when the host has none.
    std::string password;
};

struct welcome_msg {
    player_id id = 0;
    std::string world_name;
    std::int64_t pace_ms = 0;
    std::int64_t tactical_grace_ms = 0;
};

struct reject_msg {
    std::string reason;
};

enum class action_type : int {
    move,          // dx, dy, dz in [-1, 1]; also attacks, opens doors
    wait,          // spend the turn
    pause,         // spend the turn without the "waiting" activity
    ready,         // vote to skip time
    unready,
    tactical_on,   // manual tactical toggle
    tactical_off,
    activity,      // serialized activity_actor in `activity`
};

std::string to_string( action_type a );
std::optional<action_type> action_from_string( const std::string &s );

struct command_msg {
    std::int64_t seq = 0;
    // The turn the client saw when it issued the command.
    std::int64_t turn = 0;
    action_type action = action_type::wait;
    int dx = 0;
    int dy = 0;
    int dz = 0;
    std::string activity;
};

struct command_ack_msg {
    std::int64_t seq = 0;
    bool ok = true;
    std::string reason;
};

struct tick_msg {
    std::int64_t turn = 0;
    tempo_mode mode = tempo_mode::explore;
    std::vector<player_id> waiting_for;
};

struct chat_msg {
    player_id from = 0;
    std::string text;
};

struct bye_msg {
    std::string reason;
};

// Short status of the player's own character. The full mirror (character and
// submap JSON) replaces this in the thin-client milestone.
struct self_msg {
    std::int64_t turn = 0;
    // Absolute map-square position.
    int x = 0;
    int y = 0;
    int z = 0;
    int hp = 0;
    int hp_max = 0;
    int moves = 0;
    std::string activity;
    bool danger = false;
};

std::string serialize( const hello_msg &m );
std::string serialize( const welcome_msg &m );
std::string serialize( const reject_msg &m );
std::string serialize( const command_msg &m );
std::string serialize( const command_ack_msg &m );
std::string serialize( const tick_msg &m );
std::string serialize( const chat_msg &m );
std::string serialize( const bye_msg &m );
std::string serialize( const self_msg &m );

// Returns the "t" member of a payload, or nullopt if it is not a JSON object
// with a string "t".
std::optional<std::string> message_type( const std::string &payload );

// Each parse returns an empty string on success, else the error.
std::string parse( const std::string &payload, hello_msg &out );
std::string parse( const std::string &payload, welcome_msg &out );
std::string parse( const std::string &payload, reject_msg &out );
std::string parse( const std::string &payload, command_msg &out );
std::string parse( const std::string &payload, command_ack_msg &out );
std::string parse( const std::string &payload, tick_msg &out );
std::string parse( const std::string &payload, chat_msg &out );
std::string parse( const std::string &payload, bye_msg &out );
std::string parse( const std::string &payload, self_msg &out );

// --- Validation -------------------------------------------------------------

// 1..max_name_bytes bytes, no control characters, not only spaces.
bool valid_player_name( const std::string &name );
// Removes control characters (keeps valid UTF-8 bytes) and truncates.
std::string sanitize_text( const std::string &text, size_t max_bytes );

// Stable fingerprint of a mod list (FNV-1a 64, hex). Order matters, because
// load order changes the game data.
std::string mods_fingerprint( const std::vector<std::string> &mod_ids );

// What the host requires from a joining client. Returns the reason to reject,
// or nullopt to accept.
std::optional<std::string> check_hello( const hello_msg &h, const std::string &game_version,
                                        const std::string &mods_hash,
                                        const std::string &password = std::string() );

} // namespace mp

#endif // CATA_SRC_MP_PROTOCOL_H
