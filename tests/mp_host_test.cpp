#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "coordinates.h"
#include "do_turn.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "mp_host.h"
#include "mp_protocol.h"
#include "mp_transport.h"
#include "npc.h"
#include "player_helpers.h"
#include "type_id.h"

static const efftype_id effect_npc_suspend( "npc_suspend" );

namespace
{

// Runs the host's network side and the client until `pred` holds.
template<typename Pred>
bool pump_until( mp::host &h, mp::connection &client, Pred pred, int timeout_ms = 3000 )
{
    const auto start = std::chrono::steady_clock::now();
    while( std::chrono::steady_clock::now() - start < std::chrono::milliseconds( timeout_ms ) ) {
        h.poll();
        client.flush();
        client.pump();
        if( pred() ) {
            return true;
        }
        mp::connection *const conns[] = { &client };
        mp::wait_readable( conns, 1, nullptr, 10 );
    }
    return false;
}

// Reads client messages until one of the given type arrives.
std::optional<std::string> wait_for_type( mp::host &h, mp::connection &client,
        const std::string &type )
{
    std::optional<std::string> found;
    pump_until( h, client, [&]() {
        while( std::optional<std::string> m = client.next_message() ) {
            if( mp::message_type( *m ) == std::optional<std::string>( type ) ) {
                found = m;
                return true;
            }
        }
        return false;
    } );
    return found;
}

mp::hello_msg make_hello( const mp::host &h )
{
    mp::hello_msg hello;
    hello.game_version = h.game_version();
    hello.mods_hash = h.mods_hash();
    hello.name = "Bartek";
    hello.player_uuid = "test-uuid-1";
    return hello;
}

} // namespace

TEST_CASE( "mp_host_puppet_follows_network_commands", "[mp]" )
{
    if( !mp::transport_supported() ) {
        WARN( "no network transport on this platform" );
        return;
    }
    clear_map();
    clear_avatar();
    clear_npcs();

    mp::host &h = mp::get_host();
    mp::host_options opts;
    opts.loopback_only = true;
    opts.sched.pace_ms = 60;
    opts.sched.tactical_grace_ms = 300;
    std::string err;
    REQUIRE( h.start( opts, err ) );
    on_out_of_scope stop_host( [&h]() {
        h.stop( "test finished" );
    } );

    std::unique_ptr<mp::connection> client = mp::connect_to( "127.0.0.1", h.port(), 2000, err );
    INFO( err );
    REQUIRE( client );

    // A lone host is never timed out, even with a client still in handshake.
    h.begin_turn();
    CHECK_FALSE( h.host_input_timed_out() );

    REQUIRE( client->send( mp::serialize( make_hello( h ) ) ) );
    std::optional<std::string> msg = wait_for_type( h, *client, "welcome" );
    REQUIRE( msg );
    mp::welcome_msg welcome;
    REQUIRE( mp::parse( *msg, welcome ).empty() );
    CHECK( welcome.id != mp::host_player_id );
    CHECK( welcome.pace_ms == 60 );
    CHECK( h.connected_remotes() == 1 );

    const std::optional<character_id> puppet_id = h.puppet_of( welcome.id );
    REQUIRE( puppet_id );
    npc *guy = g->find_npc( *puppet_id );
    REQUIRE( guy != nullptr );
    CHECK( guy->get_name() == "Bartek" );
    CHECK( h.is_puppet( *guy ) );

    SECTION( "a move command moves the puppet one tile" ) {
        h.begin_turn();
        const tripoint_bub_ms before = guy->pos_bub();
        mp::command_msg cmd;
        cmd.seq = 1;
        cmd.action = mp::action_type::move;
        cmd.dx = 1;
        REQUIRE( client->send( mp::serialize( cmd ) ) );
        process_npc_turn( get_map(), *guy );
        CHECK( guy->pos_bub() == before + tripoint_rel_ms( 1, 0, 0 ) );

        msg = wait_for_type( h, *client, "cmd_ack" );
        REQUIRE( msg );
        mp::command_ack_msg ack;
        REQUIRE( mp::parse( *msg, ack ).empty() );
        CHECK( ack.seq == 1 );
        CHECK( ack.ok );
    }

    SECTION( "an idle remote player pauses when the explore pace runs out" ) {
        h.begin_turn();
        const tripoint_bub_ms before = guy->pos_bub();
        const auto start = std::chrono::steady_clock::now();
        process_npc_turn( get_map(), *guy );
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK( guy->pos_bub() == before );
        CHECK( guy->get_moves() <= 0 );
        // It waited for the player, but not much longer than the pace.
        CHECK( took >= std::chrono::milliseconds( 40 ) );
        CHECK( took < std::chrono::milliseconds( 1000 ) );
    }

    SECTION( "the host times out in explore mode once a client is connected" ) {
        h.begin_turn();
        CHECK_FALSE( h.host_input_timed_out() );
        const auto start = std::chrono::steady_clock::now();
        bool timed_out = false;
        while( !timed_out && std::chrono::steady_clock::now() - start < std::chrono::seconds( 2 ) ) {
            timed_out = h.host_input_timed_out();
        }
        CHECK( timed_out );
    }

    SECTION( "a ready vote from everyone fast-forwards" ) {
        mp::command_msg cmd;
        cmd.seq = 2;
        cmd.action = mp::action_type::ready;
        REQUIRE( client->send( mp::serialize( cmd ) ) );
        REQUIRE( wait_for_type( h, *client, "cmd_ack" ) );
        CHECK( h.get_scheduler().state( welcome.id ) == mp::player_state::ready );
    }

    SECTION( "disconnecting suspends the puppet and frees the clock" ) {
        client.reset();
        const auto start = std::chrono::steady_clock::now();
        while( h.connected_remotes() != 0 &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds( 3 ) ) {
            h.poll();
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
        CHECK( h.connected_remotes() == 0 );
        CHECK( guy->has_effect( effect_npc_suspend ) );
        CHECK( h.is_puppet( *guy ) );
        h.begin_turn();
        CHECK_FALSE( h.host_input_timed_out() );
    }
}

TEST_CASE( "mp_host_rejects_bad_hello", "[mp]" )
{
    if( !mp::transport_supported() ) {
        WARN( "no network transport on this platform" );
        return;
    }
    clear_map();
    clear_avatar();
    clear_npcs();

    mp::host &h = mp::get_host();
    mp::host_options opts;
    opts.loopback_only = true;
    opts.password = "sekret";
    std::string err;
    REQUIRE( h.start( opts, err ) );
    on_out_of_scope stop_host( [&h]() {
        h.stop( "test finished" );
    } );

    std::unique_ptr<mp::connection> client = mp::connect_to( "127.0.0.1", h.port(), 2000, err );
    REQUIRE( client );
    mp::hello_msg hello = make_hello( h );
    hello.password = "wrong";
    REQUIRE( client->send( mp::serialize( hello ) ) );
    std::optional<std::string> msg = wait_for_type( h, *client, "reject" );
    REQUIRE( msg );
    mp::reject_msg rej;
    REQUIRE( mp::parse( *msg, rej ).empty() );
    CHECK( rej.reason == "wrong password" );
    CHECK( h.connected_remotes() == 0 );

    // A command before hello is a protocol violation: the host hangs up.
    std::unique_ptr<mp::connection> rude = mp::connect_to( "127.0.0.1", h.port(), 2000, err );
    REQUIRE( rude );
    mp::command_msg cmd;
    cmd.action = mp::action_type::wait;
    REQUIRE( rude->send( mp::serialize( cmd ) ) );
    CHECK( pump_until( h, *rude, [&rude]() {
        return !rude->is_open();
    } ) );
}
