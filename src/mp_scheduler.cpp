#include "mp_scheduler.h"

#include <algorithm>

namespace mp
{

std::string to_string( tempo_mode m )
{
    switch( m ) {
        case tempo_mode::tactical:
            return "tactical";
        case tempo_mode::explore:
            return "explore";
        case tempo_mode::fast_forward:
            return "fast_forward";
    }
    return "unknown";
}

std::string to_string( player_state s )
{
    switch( s ) {
        case player_state::input:
            return "input";
        case player_state::busy:
            return "busy";
        case player_state::ready:
            return "ready";
    }
    return "unknown";
}

bool scheduler::add_player( player_id id )
{
    return players_.emplace( id, slot() ).second;
}

bool scheduler::remove_player( player_id id )
{
    return players_.erase( id ) > 0;
}

bool scheduler::has_player( player_id id ) const
{
    return players_.count( id ) > 0;
}

void scheduler::set_connected( player_id id, bool connected )
{
    auto it = players_.find( id );
    if( it != players_.end() ) {
        it->second.connected = connected;
    }
}

void scheduler::set_state( player_id id, player_state state )
{
    auto it = players_.find( id );
    if( it == players_.end() ) {
        return;
    }
    // A ready vote is meaningless while the group is in danger.
    if( state == player_state::ready && group_in_danger() ) {
        state = player_state::input;
    }
    it->second.state = state;
}

player_state scheduler::state( player_id id ) const
{
    auto it = players_.find( id );
    return it == players_.end() ? player_state::input : it->second.state;
}

bool scheduler::group_in_danger() const
{
    return std::any_of( players_.begin(), players_.end(), [this]( const auto & p ) {
        return p.second.connected && in_danger( p.first );
    } );
}

void scheduler::report_danger( player_id id )
{
    auto it = players_.find( id );
    if( it == players_.end() ) {
        return;
    }
    const bool was_in_danger = group_in_danger();
    it->second.danger_until = std::max<std::uint64_t>( it->second.danger_until,
                              turn_index_ + static_cast<std::uint64_t>( std::max( 1, cfg_.danger_linger_turns ) ) );
    if( !was_in_danger ) {
        for( auto &p : players_ ) {
            if( p.second.state == player_state::ready ) {
                p.second.state = player_state::input;
            }
        }
    }
}

void scheduler::set_force_tactical( player_id id, bool on )
{
    auto it = players_.find( id );
    if( it != players_.end() ) {
        it->second.force_tactical = on;
    }
}

bool scheduler::in_danger( player_id id ) const
{
    auto it = players_.find( id );
    if( it == players_.end() ) {
        return false;
    }
    return it->second.force_tactical || turn_index_ < it->second.danger_until;
}

void scheduler::begin_turn( std::int64_t now_ms )
{
    ++turn_index_;
    turn_start_ms_ = now_ms;
    for( auto &p : players_ ) {
        p.second.done = false;
    }
}

void scheduler::mark_done( player_id id )
{
    auto it = players_.find( id );
    if( it != players_.end() ) {
        it->second.done = true;
    }
}

bool scheduler::is_done( player_id id ) const
{
    auto it = players_.find( id );
    return it != players_.end() && it->second.done;
}

tempo_mode scheduler::mode() const
{
    if( group_in_danger() ) {
        return tempo_mode::tactical;
    }
    const bool anyone_input = std::any_of( players_.begin(), players_.end(),
    []( const auto & p ) {
        return p.second.connected && p.second.state == player_state::input;
    } );
    return anyone_input ? tempo_mode::explore : tempo_mode::fast_forward;
}

std::int64_t scheduler::deadline_ms( tempo_mode m ) const
{
    switch( m ) {
        case tempo_mode::tactical:
            return cfg_.tactical_grace_ms > 0 ? cfg_.tactical_grace_ms : -1;
        case tempo_mode::explore:
            return std::max<std::int64_t>( 0, cfg_.pace_ms );
        case tempo_mode::fast_forward:
            return 0;
    }
    return 0;
}

turn_poll scheduler::poll( std::int64_t now_ms ) const
{
    turn_poll res;
    res.mode = mode();
    const std::int64_t deadline = deadline_ms( res.mode );
    const std::int64_t elapsed = std::max<std::int64_t>( 0, now_ms - turn_start_ms_ );
    std::int64_t wake = -1;
    for( const auto &p : players_ ) {
        const slot &s = p.second;
        if( !s.connected || s.done || s.state != player_state::input ) {
            continue;
        }
        if( deadline >= 0 && elapsed >= deadline ) {
            res.timed_out.push_back( p.first );
            continue;
        }
        res.waiting_for.push_back( p.first );
        if( deadline >= 0 ) {
            const std::int64_t left = deadline - elapsed;
            wake = wake < 0 ? left : std::min( wake, left );
        }
    }
    res.can_end_turn = res.waiting_for.empty() && res.timed_out.empty();
    if( res.can_end_turn ) {
        res.wake_in_ms = 0;
    } else if( !res.waiting_for.empty() ) {
        res.wake_in_ms = wake;
    } else {
        // Only timed-out players left: the host must pause them now.
        res.wake_in_ms = 0;
    }
    return res;
}

} // namespace mp
