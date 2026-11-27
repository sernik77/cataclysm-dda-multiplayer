#pragma once
#ifndef CATA_SRC_MP_SCHEDULER_H
#define CATA_SRC_MP_SCHEDULER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Multiplayer turn scheduler.
//
// All players share one world clock (calendar::turn). The scheduler decides,
// in real time, when the current turn may end. It picks one of three tempo
// modes from the state of all players, then applies a per-player deadline.
// See doc/c++/MULTIPLAYER.md, section 4.
//
// This is pure logic: no game state and no I/O. The caller passes the time in
// milliseconds, so tests can use a fake clock.
namespace mp
{

using player_id = std::uint32_t;

enum class tempo_mode : int {
    // Somebody is in danger: strict turns, wait for every input player.
    tactical,
    // Nobody is in danger: soft real time, idle players auto-pause.
    explore,
    // Everybody is busy or ready: no waiting at all.
    fast_forward,
};

enum class player_state : int {
    // Idle; expected to choose an action this turn.
    input,
    // An activity, sleep, wait or auto-move spends their moves.
    busy,
    // Explicitly agreed to skip time with the group.
    ready,
};

std::string to_string( tempo_mode m );
std::string to_string( player_state s );

struct scheduler_config {
    // EXPLORE: the longest a turn waits for an input player.
    std::int64_t pace_ms = 250;
    // TACTICAL: the longest a turn waits for an input player (AFK guard).
    // A value <= 0 means: wait forever.
    std::int64_t tactical_grace_ms = 30000;
    // A danger report keeps the player in danger for this many turns, so a
    // monster that flickers in and out of view does not toggle the mode on
    // every turn.
    int danger_linger_turns = 5;
};

// Result of scheduler::poll().
struct turn_poll {
    tempo_mode mode = tempo_mode::fast_forward;
    // True when the turn may end now.
    bool can_end_turn = true;
    // Players the turn still waits for (deadline not reached yet).
    std::vector<player_id> waiting_for;
    // Players whose deadline has passed without an action. The host should
    // make them pause for this turn, then call mark_done() for each.
    std::vector<player_id> timed_out;
    // Milliseconds until the next deadline among waiting_for, or -1 when the
    // turn waits without a time limit. 0 when can_end_turn is true.
    std::int64_t wake_in_ms = 0;
};

class scheduler
{
    public:
        scheduler() = default;
        explicit scheduler( const scheduler_config &cfg ) : cfg_( cfg ) {}

        const scheduler_config &config() const {
            return cfg_;
        }
        void set_config( const scheduler_config &cfg ) {
            cfg_ = cfg;
        }

        // Returns false if the player already exists.
        bool add_player( player_id id );
        // Returns false if the player does not exist.
        bool remove_player( player_id id );
        bool has_player( player_id id ) const;
        size_t player_count() const {
            return players_.size();
        }

        // Disconnected players never hold the clock.
        void set_connected( player_id id, bool connected );
        void set_state( player_id id, player_state state );
        player_state state( player_id id ) const;

        // The player perceives a threat this turn. Keeps them in danger for
        // config().danger_linger_turns turns. When the group enters danger,
        // every `ready` vote is cancelled (becomes `input`).
        void report_danger( player_id id );
        // Manual tactical toggle; stays on until switched off.
        void set_force_tactical( player_id id, bool on );
        bool in_danger( player_id id ) const;

        // Starts a new turn at time now_ms. Clears the per-turn "done" flags.
        void begin_turn( std::int64_t now_ms );
        // The player has spent their moves (or paused) for this turn.
        void mark_done( player_id id );
        bool is_done( player_id id ) const;

        std::uint64_t turn_index() const {
            return turn_index_;
        }
        tempo_mode mode() const;
        turn_poll poll( std::int64_t now_ms ) const;

    private:
        struct slot {
            player_state state = player_state::input;
            bool connected = true;
            bool done = false;
            bool force_tactical = false;
            // Danger lasts while turn_index_ < danger_until.
            std::uint64_t danger_until = 0;
        };

        // Deadline in ms for one input player in the given mode; -1 = none.
        std::int64_t deadline_ms( tempo_mode m ) const;
        bool group_in_danger() const;

        scheduler_config cfg_;
        std::map<player_id, slot> players_;
        std::uint64_t turn_index_ = 0;
        std::int64_t turn_start_ms_ = 0;
};

} // namespace mp

#endif // CATA_SRC_MP_SCHEDULER_H
