#pragma once
#ifndef CATA_SRC_MP_HOST_H
#define CATA_SRC_MP_HOST_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "character_id.h"
#include "mp_protocol.h"
#include "mp_scheduler.h"
#include "mp_transport.h"

class npc;

// Multiplayer host: the authoritative side of a co-op session.
//
// The host process runs the real world. Each remote player controls a puppet
// npc. The puppet does not run NPC AI; on its turn it executes the commands
// its player sent. The scheduler (mp_scheduler.h) decides how long a turn
// waits for each player. See doc/c++/MULTIPLAYER.md.
//
// Everything here is inactive unless hosting was requested (--mp-host), so
// single player does not change.
namespace mp
{

// The local (host) player always has this id in the scheduler.
constexpr player_id host_player_id = 0;

struct host_options {
    std::uint16_t port = 0;
    bool loopback_only = false;
    std::string password;
    size_t max_players = 8;
    size_t max_queued_commands = 16;
    // A client that does not say hello in time is dropped.
    std::int64_t hello_timeout_ms = 10000;
    scheduler_config sched;
};

class host
{
    public:
        bool start( const host_options &opts, std::string &err );
        void stop( const std::string &reason );
        bool active() const {
            return lst_.is_open();
        }
        std::uint16_t port() const {
            return lst_.port();
        }

        // Accepts clients, reads messages, runs handshakes and queues
        // commands. Never blocks.
        void poll();

        // Once per game turn, after the clock advanced: updates player states
        // and danger, starts the scheduler turn and tells the clients.
        void begin_turn();

        bool is_puppet( const npc &guy ) const;
        // Runs a puppet's turn in place of npc::move(): executes queued
        // commands while it has moves, and waits for its player as long as
        // the scheduler allows.
        void run_puppet_turn( npc &guy );

        // For the host's own input loop: true when the host player has used
        // up their time for this turn and should pause. Always false while no
        // remote player is connected, so a lone host plays like single player.
        bool host_input_timed_out();

        size_t connected_remotes() const;
        const scheduler &get_scheduler() const {
            return sched_;
        }
        // "explore - waiting for Bartek", for the status display.
        std::string status_line() const;
        // The puppet of a player, if it was spawned.
        std::optional<character_id> puppet_of( player_id id ) const;
        // What a client must send in hello to be accepted.
        const std::string &game_version() const {
            return game_version_;
        }
        const std::string &mods_hash() const {
            return mods_hash_;
        }

    private:
        struct remote {
            player_id id = 0;
            std::unique_ptr<connection> conn;
            bool welcomed = false;
            bool gone = false;
            std::int64_t connected_at_ms = 0;
            std::string name;
            std::string uuid;
            character_id puppet;
            std::deque<command_msg> queue;
        };

        remote *find_remote( player_id id );
        const remote *find_remote_by_puppet( const npc &guy ) const;
        void handle_message( remote &r, const std::string &payload );
        void handle_hello( remote &r, const std::string &payload );
        void handle_command( remote &r, const command_msg &cmd );
        // Executes one command. Returns an error, or an empty string.
        std::string execute( npc &guy, const command_msg &cmd );
        bool spawn_or_bind_puppet( remote &r );
        void drop( remote &r, const std::string &reason );
        void send_self( remote &r, const npc &guy );
        void send_tick();
        void ack( remote &r, std::int64_t seq, const std::string &err );

        host_options opts_;
        listener lst_;
        scheduler sched_;
        std::vector<std::unique_ptr<remote>> remotes_;
        // Player uuid -> puppet, so a player who reconnects gets the same
        // character back. (In memory only; saved in a later milestone.)
        std::map<std::string, character_id> bindings_;
        std::map<std::string, player_id> uuid_ids_;
        player_id next_id_ = 1;
        std::string game_version_;
        std::string mods_hash_;
};

host &get_host();

// Monotonic wall clock in milliseconds.
std::int64_t now_ms();

// Hosting requested on the command line; the host starts on the first turn
// of the loaded game.
void request_host( const host_options &opts );

// --- Engine hooks (cheap no-ops while multiplayer is off) -------------------

// From game::do_turn(), once per turn.
void on_turn_start();
// From process_npc_turn(). Returns true if guy is a puppet and its turn ran.
bool run_puppet_turn_hook( npc &guy );
// From the host's input loop timeout check.
bool host_turn_timed_out();

} // namespace mp

#endif // CATA_SRC_MP_HOST_H
