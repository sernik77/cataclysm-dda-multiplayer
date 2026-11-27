#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "mp_protocol.h"
#include "mp_scheduler.h"

TEST_CASE( "mp_frame_roundtrip_in_pieces", "[mp][nogame]" )
{
    const std::vector<std::string> payloads = { "{}", "", std::string( 70000, 'x' ), "{\"t\":\"a\"}" };
    std::string stream;
    for( const std::string &p : payloads ) {
        stream += mp::encode_frame( p );
    }

    SECTION( "one byte at a time" ) {
        mp::frame_decoder dec;
        std::vector<std::string> got;
        for( const char c : stream ) {
            REQUIRE( dec.feed( &c, 1 ) );
            while( std::optional<std::string> m = dec.next() ) {
                got.push_back( *m );
            }
        }
        CHECK( got == payloads );
        CHECK( dec.buffered() == 0 );
    }
    SECTION( "all at once" ) {
        mp::frame_decoder dec;
        REQUIRE( dec.feed( stream.data(), stream.size() ) );
        std::vector<std::string> got;
        while( std::optional<std::string> m = dec.next() ) {
            got.push_back( *m );
        }
        CHECK( got == payloads );
    }
}

TEST_CASE( "mp_frame_rejects_oversized", "[mp][nogame]" )
{
    mp::frame_decoder dec( 100 );
    const std::string ok = mp::encode_frame( std::string( 100, 'a' ) );
    const std::string bad = mp::encode_frame( std::string( 101, 'b' ) );
    REQUIRE( dec.feed( ok.data(), ok.size() ) );
    // Only the header of the bad frame: rejected before the body arrives.
    CHECK_FALSE( dec.feed( bad.data(), 4 ) );
    CHECK( dec.failed() );
    // The good frame before it is not delivered once the stream failed.
    CHECK_FALSE( dec.next().has_value() );
    CHECK_FALSE( dec.feed( "x", 1 ) );
}

TEST_CASE( "mp_messages_roundtrip", "[mp][nogame]" )
{
    mp::hello_msg h;
    h.game_version = "0.I-test";
    h.mods_hash = "abc123";
    h.name = "Ala \"the axe\"";
    h.player_uuid = "6f50-1198";
    mp::hello_msg h2;
    REQUIRE( mp::parse( mp::serialize( h ), h2 ).empty() );
    CHECK( h2.proto == mp::protocol_version );
    CHECK( h2.game_version == h.game_version );
    CHECK( h2.mods_hash == h.mods_hash );
    CHECK( h2.name == h.name );
    CHECK( h2.player_uuid == h.player_uuid );
    CHECK( mp::message_type( mp::serialize( h ) ) == std::optional<std::string>( "hello" ) );

    mp::welcome_msg w;
    w.id = 4000000000U;
    w.world_name = "Co-op";
    w.pace_ms = 250;
    w.tactical_grace_ms = 30000;
    mp::welcome_msg w2;
    REQUIRE( mp::parse( mp::serialize( w ), w2 ).empty() );
    CHECK( w2.id == w.id );
    CHECK( w2.world_name == w.world_name );
    CHECK( w2.pace_ms == 250 );
    CHECK( w2.tactical_grace_ms == 30000 );

    mp::command_msg c;
    c.seq = 7;
    c.turn = 123456789012LL;
    c.action = mp::action_type::move;
    c.dx = -1;
    c.dy = 1;
    mp::command_msg c2;
    REQUIRE( mp::parse( mp::serialize( c ), c2 ).empty() );
    CHECK( c2.seq == 7 );
    CHECK( c2.turn == c.turn );
    CHECK( c2.action == mp::action_type::move );
    CHECK( c2.dx == -1 );
    CHECK( c2.dy == 1 );
    CHECK( c2.dz == 0 );

    c.action = mp::action_type::activity;
    c.activity = "{\"type\":\"ACT_CHOP_TREE\",\"actor\":{\"moves\":2000}}";
    REQUIRE( mp::parse( mp::serialize( c ), c2 ).empty() );
    CHECK( c2.action == mp::action_type::activity );
    CHECK( c2.activity == c.activity );

    mp::tick_msg t;
    t.turn = 99;
    t.mode = mp::tempo_mode::tactical;
    t.waiting_for = { 1, 3 };
    mp::tick_msg t2;
    REQUIRE( mp::parse( mp::serialize( t ), t2 ).empty() );
    CHECK( t2.turn == 99 );
    CHECK( t2.mode == mp::tempo_mode::tactical );
    CHECK( t2.waiting_for == t.waiting_for );

    mp::command_ack_msg a;
    a.seq = 7;
    a.ok = false;
    a.reason = "target is gone";
    mp::command_ack_msg a2;
    REQUIRE( mp::parse( mp::serialize( a ), a2 ).empty() );
    CHECK( a2.seq == 7 );
    CHECK_FALSE( a2.ok );
    CHECK( a2.reason == a.reason );

    mp::chat_msg ch;
    ch.from = 2;
    ch.text = "zombie brute NE!";
    mp::chat_msg ch2;
    REQUIRE( mp::parse( mp::serialize( ch ), ch2 ).empty() );
    CHECK( ch2.from == 2 );
    CHECK( ch2.text == ch.text );

    mp::bye_msg b;
    b.reason = "host quit";
    mp::bye_msg b2;
    REQUIRE( mp::parse( mp::serialize( b ), b2 ).empty() );
    CHECK( b2.reason == b.reason );

    mp::reject_msg r;
    r.reason = "mod list mismatch";
    mp::reject_msg r2;
    REQUIRE( mp::parse( mp::serialize( r ), r2 ).empty() );
    CHECK( r2.reason == r.reason );
}

TEST_CASE( "mp_messages_reject_hostile_input", "[mp][nogame]" )
{
    mp::command_msg c;
    // Not JSON, not an object, wrong type, missing fields.
    CHECK_FALSE( mp::parse( "not json", c ).empty() );
    CHECK_FALSE( mp::parse( "[1,2,3]", c ).empty() );
    CHECK_FALSE( mp::parse( R"({"t":"hello"})", c ).empty() );
    CHECK_FALSE( mp::parse( R"({"t":"cmd","seq":1,"turn":1})", c ).empty() );
    CHECK_FALSE( mp::message_type( "garbage" ).has_value() );
    CHECK_FALSE( mp::message_type( R"({"t":5})" ).has_value() );

    // Unknown action.
    CHECK_FALSE( mp::parse( R"({"t":"cmd","seq":1,"turn":1,"action":"teleport"})", c ).empty() );
    // Moves are one tile at most, and must have a direction.
    CHECK_FALSE( mp::parse( R"({"t":"cmd","seq":1,"turn":1,"action":"move","dx":5,"dy":0,"dz":0})",
                            c ).empty() );
    CHECK_FALSE( mp::parse( R"({"t":"cmd","seq":1,"turn":1,"action":"move","dx":0,"dy":0,"dz":0})",
                            c ).empty() );
    CHECK_FALSE( mp::parse( R"({"t":"cmd","seq":1,"turn":1,"action":"move","dx":1})", c ).empty() );
    // Wrong value types.
    CHECK_FALSE( mp::parse( R"({"t":"cmd","seq":"1","turn":1,"action":"wait"})", c ).empty() );

    // Unknown extra members are fine (forward compatibility).
    REQUIRE( mp::parse( R"({"t":"cmd","seq":1,"turn":1,"action":"wait","future":true})", c ).empty() );
    CHECK( c.action == mp::action_type::wait );

    // Oversized activity payload.
    const std::string big = R"({"t":"cmd","seq":1,"turn":1,"action":"activity","activity":")" +
                            std::string( mp::max_activity_bytes + 1, 'a' ) + "\"}";
    CHECK_FALSE( mp::parse( big, c ).empty() );

    // Player ids out of range.
    mp::tick_msg t;
    CHECK_FALSE( mp::parse( R"({"t":"tick","turn":1,"mode":"explore","waiting_for":[-1]})",
                            t ).empty() );
    CHECK_FALSE( mp::parse( R"({"t":"tick","turn":1,"mode":"warp","waiting_for":[]})", t ).empty() );

    // Control characters are stripped from chat.
    mp::chat_msg ch;
    REQUIRE( mp::parse( "{\"t\":\"chat\",\"from\":1,\"text\":\"a\\u001b[2Jb\\nc\"}", ch ).empty() );
    CHECK( ch.text == "a[2Jbc" );
}

TEST_CASE( "mp_hello_validation", "[mp][nogame]" )
{
    mp::hello_msg h;
    h.game_version = "v1";
    h.mods_hash = "m";
    h.name = "Bartek";
    h.player_uuid = "abc-123";
    CHECK_FALSE( mp::check_hello( h, "v1", "m" ).has_value() );

    mp::hello_msg bad = h;
    bad.proto = mp::protocol_version + 1;
    CHECK( mp::check_hello( bad, "v1", "m" ).has_value() );
    bad = h;
    bad.game_version = "v2";
    CHECK( mp::check_hello( bad, "v1", "m" ).has_value() );
    bad = h;
    bad.mods_hash = "other";
    CHECK( mp::check_hello( bad, "v1",
                            "m" ) == std::optional<std::string>( "mod list mismatch: host m" ) );
    bad = h;
    bad.name = "   ";
    CHECK( mp::check_hello( bad, "v1", "m" ).has_value() );
    bad = h;
    bad.player_uuid = "../../etc";
    CHECK( mp::check_hello( bad, "v1", "m" ).has_value() );

    CHECK( mp::valid_player_name( "Zażółć" ) );
    CHECK_FALSE( mp::valid_player_name( "" ) );
    CHECK_FALSE( mp::valid_player_name( std::string( mp::max_name_bytes + 1, 'a' ) ) );
    CHECK_FALSE( mp::valid_player_name( "tab\tname" ) );
}

TEST_CASE( "mp_sanitize_text_keeps_utf8_whole", "[mp][nogame]" )
{
    // "ż" is two bytes (0xC5 0xBC). Cutting at 2 bytes would split it.
    CHECK( mp::sanitize_text( "aż", 2 ) == "a" );
    CHECK( mp::sanitize_text( "aż", 3 ) == "aż" );
    CHECK( mp::sanitize_text( "a\x01\x7f" "b", 10 ) == "ab" );
    CHECK( mp::sanitize_text( "", 5 ).empty() );
}

TEST_CASE( "mp_self_and_password", "[mp][nogame]" )
{
    mp::self_msg m;
    m.turn = 5260000;
    m.x = -1234;
    m.y = 99;
    m.z = -2;
    m.hp = 80;
    m.hp_max = 84;
    m.moves = -20;
    m.activity = "ACT_CHOP_TREE";
    m.danger = true;
    mp::self_msg m2;
    REQUIRE( mp::parse( mp::serialize( m ), m2 ).empty() );
    CHECK( m2.turn == m.turn );
    CHECK( m2.x == -1234 );
    CHECK( m2.y == 99 );
    CHECK( m2.z == -2 );
    CHECK( m2.hp == 80 );
    CHECK( m2.hp_max == 84 );
    CHECK( m2.moves == -20 );
    CHECK( m2.activity == m.activity );
    CHECK( m2.danger );

    mp::hello_msg h;
    h.game_version = "v";
    h.mods_hash = "m";
    h.name = "Ala";
    h.player_uuid = "u1";
    h.password = "sekret";
    mp::hello_msg h2;
    REQUIRE( mp::parse( mp::serialize( h ), h2 ).empty() );
    CHECK( h2.password == "sekret" );
    CHECK_FALSE( mp::check_hello( h2, "v", "m", "sekret" ).has_value() );
    CHECK( mp::check_hello( h2, "v", "m", "sekre" ) == std::optional<std::string>( "wrong password" ) );
    // No password on the host: any client password is accepted.
    CHECK_FALSE( mp::check_hello( h2, "v", "m" ).has_value() );
}

TEST_CASE( "mp_mods_fingerprint", "[mp][nogame]" )
{
    const std::string base = mp::mods_fingerprint( { "dda", "no_npc_food" } );
    CHECK( base.size() == 16 );
    CHECK( base == mp::mods_fingerprint( { "dda", "no_npc_food" } ) );
    // Order and boundaries matter.
    CHECK( base != mp::mods_fingerprint( { "no_npc_food", "dda" } ) );
    CHECK( mp::mods_fingerprint( { "ab", "c" } ) != mp::mods_fingerprint( { "a", "bc" } ) );
    // Known value: FNV-1a 64 of the empty input.
    CHECK( mp::mods_fingerprint( {} ) == "cbf29ce484222325" );
}
