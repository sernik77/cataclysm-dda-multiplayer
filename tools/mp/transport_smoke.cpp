// Standalone smoke test for mp_transport: loopback listen, connect, exchange
// framed messages, detect close. CI builds it natively on Windows (MSVC).
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "mp_transport.h"

static bool pump_until( mp::connection &want, mp::connection &other, std::string &out )
{
    const auto start = std::chrono::steady_clock::now();
    while( std::chrono::steady_clock::now() - start < std::chrono::seconds( 3 ) ) {
        other.flush();
        want.pump();
        if( std::optional<std::string> m = want.next_message() ) {
            out = *m;
            return true;
        }
        if( !want.is_open() ) {
            return false;
        }
        mp::connection *const conns[] = { &want };
        mp::wait_readable( conns, 1, nullptr, 20 );
    }
    return false;
}

int main()
{
    int fails = 0;
    const auto check = [&fails]( bool ok, const char *what ) {
        std::printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
        fails += ok ? 0 : 1;
    };
    check( mp::transport_supported(), "transport supported (WSAStartup)" );
    mp::listener lst;
    std::string err;
    check( lst.open( 0, true, err ), "listen on loopback" );
    std::printf( "  port %u %s\n", lst.port(), err.c_str() );
    std::unique_ptr<mp::connection> client = mp::connect_to( "127.0.0.1", lst.port(), 2000, err );
    check( client != nullptr, "connect" );
    if( !client ) {
        std::printf( "  %s\n", err.c_str() );
        return 1;
    }
    std::unique_ptr<mp::connection> server;
    const auto start = std::chrono::steady_clock::now();
    while( !server && std::chrono::steady_clock::now() - start < std::chrono::seconds( 3 ) ) {
        server = lst.accept();
        mp::wait_readable( nullptr, 0, &lst, 20 );
    }
    check( server != nullptr, "accept" );
    if( !server ) {
        return 1;
    }
    std::string got;
    check( client->send( "{\"t\":\"hello\"}" ), "send small" );
    check( pump_until( *server, *client, got ) && got == "{\"t\":\"hello\"}", "receive small" );
    const std::string big( 300000, 'z' );
    check( server->send( big ), "send 300 KB" );
    check( pump_until( *client, *server, got ) && got == big, "receive 300 KB whole" );
    server->close( "done" );
    check( !pump_until( *client, *server, got ) && !client->is_open(), "close detected" );
    std::printf( "  reason: %s\n", client->close_reason().c_str() );
    const std::uint16_t port = lst.port();
    lst.close();
    std::unique_ptr<mp::connection> none = mp::connect_to( "127.0.0.1", port, 500, err );
    check( none == nullptr && !err.empty(), "connect to closed port fails" );
    std::printf( "  %s\n", err.c_str() );
    std::printf( fails == 0 ? "ALL PASS\n" : "%d FAILED\n", fails );
    return fails == 0 ? 0 : 1;
}
