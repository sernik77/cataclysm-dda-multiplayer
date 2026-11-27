#include <cstdint>
#include <vector>

#include "cata_catch.h"
#include "mp_scheduler.h"

using mp::player_id;
using mp::player_state;
using mp::scheduler;
using mp::scheduler_config;
using mp::tempo_mode;

static constexpr player_id ala = 1;
static constexpr player_id bartek = 2;

static scheduler make_two_players()
{
    scheduler_config cfg;
    cfg.pace_ms = 250;
    cfg.tactical_grace_ms = 30000;
    cfg.danger_linger_turns = 3;
    scheduler s( cfg );
    REQUIRE( s.add_player( ala ) );
    REQUIRE( s.add_player( bartek ) );
    return s;
}

TEST_CASE( "mp_scheduler_mode_selection", "[mp][nogame]" )
{
    scheduler s = make_two_players();
    CHECK_FALSE( s.add_player( ala ) );

    // Both idle, nobody in danger.
    CHECK( s.mode() == tempo_mode::explore );

    // Both busy: skip time.
    s.set_state( ala, player_state::busy );
    s.set_state( bartek, player_state::busy );
    CHECK( s.mode() == tempo_mode::fast_forward );

    // Busy + ready also skips time.
    s.set_state( bartek, player_state::ready );
    CHECK( s.mode() == tempo_mode::fast_forward );

    // A disconnected idle player does not keep the group in explore.
    s.set_state( bartek, player_state::input );
    CHECK( s.mode() == tempo_mode::explore );
    s.set_connected( bartek, false );
    CHECK( s.mode() == tempo_mode::fast_forward );
    s.set_connected( bartek, true );

    // Danger to anyone means tactical, even if that player is busy.
    s.report_danger( ala );
    CHECK( s.mode() == tempo_mode::tactical );
}

TEST_CASE( "mp_scheduler_long_activity_does_not_block", "[mp][nogame]" )
{
    // Ala clears a forest (busy). Bartek explores (input).
    scheduler s = make_two_players();
    s.set_state( ala, player_state::busy );

    std::int64_t now = 0;
    s.begin_turn( now );
    mp::turn_poll p = s.poll( now );
    CHECK( p.mode == tempo_mode::explore );
    CHECK_FALSE( p.can_end_turn );
    REQUIRE( p.waiting_for == std::vector<player_id> { bartek } );
    CHECK( p.wake_in_ms == 250 );

    // Bartek acts after 40 ms: the turn ends at once, no pace delay.
    now += 40;
    s.mark_done( bartek );
    p = s.poll( now );
    CHECK( p.can_end_turn );
    CHECK( p.wake_in_ms == 0 );

    // Next turn Bartek reads a menu and does nothing.
    s.begin_turn( now );
    now += 249;
    p = s.poll( now );
    CHECK_FALSE( p.can_end_turn );
    CHECK( p.wake_in_ms == 1 );
    now += 1;
    p = s.poll( now );
    // Deadline reached: the host pauses Bartek and the clock goes on.
    REQUIRE( p.timed_out == std::vector<player_id> { bartek } );
    CHECK( p.waiting_for.empty() );
    CHECK_FALSE( p.can_end_turn );
    s.mark_done( bartek );
    CHECK( s.poll( now ).can_end_turn );
}

TEST_CASE( "mp_scheduler_tactical_waits_for_everyone", "[mp][nogame]" )
{
    scheduler s = make_two_players();
    s.begin_turn( 0 );
    s.report_danger( bartek );

    mp::turn_poll p = s.poll( 5000 );
    CHECK( p.mode == tempo_mode::tactical );
    // Both input players are waited for, not only the one in danger.
    CHECK( p.waiting_for.size() == 2 );
    CHECK( p.timed_out.empty() );
    CHECK( p.wake_in_ms == 25000 );

    s.mark_done( bartek );
    p = s.poll( 6000 );
    REQUIRE( p.waiting_for == std::vector<player_id> { ala } );
    CHECK_FALSE( p.can_end_turn );

    // AFK guard: after the grace period Ala auto-pauses.
    p = s.poll( 30000 );
    REQUIRE( p.timed_out == std::vector<player_id> { ala } );

    SECTION( "grace <= 0 waits forever" ) {
        scheduler_config cfg = s.config();
        cfg.tactical_grace_ms = 0;
        s.set_config( cfg );
        p = s.poll( 10000000 );
        CHECK( p.timed_out.empty() );
        REQUIRE( p.waiting_for == std::vector<player_id> { ala } );
        CHECK( p.wake_in_ms == -1 );
    }
}

TEST_CASE( "mp_scheduler_danger_lingers_then_expires", "[mp][nogame]" )
{
    scheduler s = make_two_players();
    s.begin_turn( 0 );
    s.report_danger( ala );
    CHECK( s.in_danger( ala ) );
    CHECK_FALSE( s.in_danger( bartek ) );

    // danger_linger_turns = 3: the turn of the report and two more.
    s.begin_turn( 1 );
    CHECK( s.mode() == tempo_mode::tactical );
    s.begin_turn( 2 );
    CHECK( s.mode() == tempo_mode::tactical );
    s.begin_turn( 3 );
    CHECK( s.mode() == tempo_mode::explore );

    // Manual toggle stays until switched off.
    s.set_force_tactical( bartek, true );
    for( int i = 0; i < 10; ++i ) {
        s.begin_turn( 4 + i );
    }
    CHECK( s.mode() == tempo_mode::tactical );
    s.set_force_tactical( bartek, false );
    CHECK( s.mode() == tempo_mode::explore );
}

TEST_CASE( "mp_scheduler_danger_cancels_ready_vote", "[mp][nogame]" )
{
    scheduler s = make_two_players();
    s.set_state( ala, player_state::ready );
    s.set_state( bartek, player_state::ready );
    CHECK( s.mode() == tempo_mode::fast_forward );

    s.begin_turn( 0 );
    CHECK( s.poll( 0 ).can_end_turn );

    s.report_danger( bartek );
    CHECK( s.state( ala ) == player_state::input );
    CHECK( s.state( bartek ) == player_state::input );
    CHECK( s.mode() == tempo_mode::tactical );

    // Voting ready while in danger is refused.
    s.set_state( ala, player_state::ready );
    CHECK( s.state( ala ) == player_state::input );
}

TEST_CASE( "mp_scheduler_edge_cases", "[mp][nogame]" )
{
    scheduler s;
    // No players: nothing to wait for.
    s.begin_turn( 0 );
    mp::turn_poll p = s.poll( 0 );
    CHECK( p.mode == tempo_mode::fast_forward );
    CHECK( p.can_end_turn );

    // Unknown ids are ignored.
    s.mark_done( 99 );
    s.report_danger( 99 );
    s.set_state( 99, player_state::busy );
    CHECK_FALSE( s.has_player( 99 ) );
    CHECK_FALSE( s.remove_player( 99 ) );
    CHECK( s.mode() == tempo_mode::fast_forward );

    // Clock going backwards does not produce a negative elapsed time.
    REQUIRE( s.add_player( ala ) );
    s.begin_turn( 1000 );
    p = s.poll( 500 );
    REQUIRE( p.waiting_for == std::vector<player_id> { ala } );
    CHECK( p.wake_in_ms == 250 );

    // A player removed mid-turn no longer holds it.
    CHECK( s.remove_player( ala ) );
    CHECK( s.poll( 600 ).can_end_turn );
}
