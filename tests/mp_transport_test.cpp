#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "cata_catch.h"
#include "mp_protocol.h"
#include "mp_transport.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/socket.h>
#endif

namespace
{
// Pumps both ends until `want` has a message or the time runs out.
std::optional<std::string> receive( mp::connection &want, mp::connection &other,
                                    int timeout_ms = 2000 )
{
    const auto start = std::chrono::steady_clock::now();
    while( std::chrono::steady_clock::now() - start < std::chrono::milliseconds( timeout_ms ) ) {
        other.flush();
        want.flush();
        want.pump();
        if( std::optional<std::string> m = want.next_message() ) {
            return m;
        }
        if( !want.is_open() ) {
            return std::nullopt;
        }
        mp::connection *const conns[] = { &want };
        mp::wait_readable( conns, 1, nullptr, 20 );
    }
    return std::nullopt;
}

std::unique_ptr<mp::connection> accept_one( mp::listener &lst, int timeout_ms = 2000 )
{
    const auto start = std::chrono::steady_clock::now();
    while( std::chrono::steady_clock::now() - start < std::chrono::milliseconds( timeout_ms ) ) {
        if( std::unique_ptr<mp::connection> c = lst.accept() ) {
            return c;
        }
        mp::wait_readable( nullptr, 0, &lst, 20 );
    }
    return nullptr;
}
} // namespace

TEST_CASE( "mp_transport_loopback_handshake", "[mp][nogame]" )
{
    if( !mp::transport_supported() ) {
        WARN( "no network transport on this platform" );
        return;
    }
    mp::listener lst;
    std::string err;
    REQUIRE( lst.open( 0, true, err ) );
    INFO( err );
    REQUIRE( lst.port() != 0 );

    std::unique_ptr<mp::connection> client = mp::connect_to( "127.0.0.1", lst.port(), 2000, err );
    INFO( err );
    REQUIRE( client );
    std::unique_ptr<mp::connection> server = accept_one( lst );
    REQUIRE( server );

    // Client says hello, host validates and answers.
    mp::hello_msg h;
    h.game_version = "test";
    h.mods_hash = "none";
    h.name = "Ala";
    h.player_uuid = "uuid-1";
    REQUIRE( client->send( mp::serialize( h ) ) );

    std::optional<std::string> got = receive( *server, *client );
    REQUIRE( got );
    REQUIRE( mp::message_type( *got ) == std::optional<std::string>( "hello" ) );
    mp::hello_msg h2;
    REQUIRE( mp::parse( *got, h2 ).empty() );
    CHECK_FALSE( mp::check_hello( h2, "test", "none" ).has_value() );

    mp::welcome_msg w;
    w.id = 2;
    w.pace_ms = 250;
    REQUIRE( server->send( mp::serialize( w ) ) );
    got = receive( *client, *server );
    REQUIRE( got );
    mp::welcome_msg w2;
    REQUIRE( mp::parse( *got, w2 ).empty() );
    CHECK( w2.id == 2 );

    // A large message (a submap snapshot is tens of KiB) arrives whole.
    const std::string big = "{\"t\":\"chat\",\"from\":2,\"text\":\"" + std::string( 300000, 'z' ) + "\"}";
    REQUIRE( server->send( big ) );
    got = receive( *client, *server );
    REQUIRE( got );
    CHECK( got->size() == big.size() );

    // Closing one side is detected by the other.
    server->close( "test done" );
    CHECK_FALSE( receive( *client, *server, 500 ).has_value() );
    CHECK_FALSE( client->is_open() );
    CHECK( client->close_reason() == "peer closed the connection" );
}

TEST_CASE( "mp_transport_drops_corrupt_stream", "[mp][nogame]" )
{
    if( !mp::transport_supported() ) {
        WARN( "no network transport on this platform" );
        return;
    }
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    mp::listener lst;
    std::string err;
    REQUIRE( lst.open( 0, true, err ) );
    std::unique_ptr<mp::connection> client = mp::connect_to( "127.0.0.1", lst.port(), 2000, err );
    REQUIRE( client );
    std::unique_ptr<mp::connection> server = accept_one( lst );
    REQUIRE( server );

    // Bypass the framing and write a header that announces a 4 GiB frame.
    // The host must hang up instead of trying to buffer it.
    const char bogus[4] = { '\xff', '\xff', '\xff', '\xff' };
    REQUIRE( ::send( client->native_handle(), bogus, sizeof( bogus ), 0 ) == 4 );
    CHECK_FALSE( receive( *server, *client, 1000 ).has_value() );
    CHECK_FALSE( server->is_open() );
    CHECK( server->close_reason().rfind( "protocol error", 0 ) == 0 );

    // Connecting to a closed port fails cleanly.
    const std::uint16_t port = lst.port();
    lst.close();
    server.reset();
    client.reset();
    std::unique_ptr<mp::connection> none = mp::connect_to( "127.0.0.1", port, 500, err );
    CHECK_FALSE( none );
    CHECK_FALSE( err.empty() );
#endif
}
