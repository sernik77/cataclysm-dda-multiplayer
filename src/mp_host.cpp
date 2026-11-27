#include "mp_host.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "debug.h"
#include "game.h"
#include "get_version.h"
#include "map.h"
#include "messages.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "player_activity.h"
#include "translations.h"
#include "type_id.h"
#include "worldfactory.h"

static const efftype_id effect_npc_suspend( "npc_suspend" );
static const faction_id faction_your_followers( "your_followers" );

namespace mp
{

// How far a player looks for threats when the scheduler decides on danger.
static constexpr int danger_range = 60;
// Safety cap on actions per puppet turn (each should spend moves).
static constexpr int max_actions_per_turn = 32;

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() ).count();
}

host &get_host()
{
    static host instance;
    return instance;
}

static std::optional<host_options> &requested_options()
{
    static std::optional<host_options> opts;
    return opts;
}

void request_host( const host_options &opts )
{
    requested_options() = opts;
}

static std::string current_mods_hash()
{
    std::vector<std::string> ids;
    if( world_generator && world_generator->active_world ) {
        for( const mod_id &m : world_generator->active_world->active_mod_order ) {
            ids.push_back( m.str() );
        }
    }
    return mods_fingerprint( ids );
}

static bool contains( const std::vector<player_id> &v, player_id id )
{
    return std::find( v.begin(), v.end(), id ) != v.end();
}

static player_state derive_state( const Character &ch, player_state previous )
{
    if( ch.activity || ch.in_sleep_state() || ch.has_destination() ) {
        return player_state::busy;
    }
    return previous == player_state::ready ? player_state::ready : player_state::input;
}

// --- Lifecycle ----------------------------------------------------------------

bool host::start( const host_options &opts, std::string &err )
{
    if( active() ) {
        err = "already hosting";
        return false;
    }
    if( !lst_.open( opts.port, opts.loopback_only, err ) ) {
        return false;
    }
    opts_ = opts;
    sched_ = scheduler( opts.sched );
    sched_.add_player( host_player_id );
    // A new session: puppets of an older session are ordinary NPCs now.
    remotes_.clear();
    bindings_.clear();
    uuid_ids_.clear();
    next_id_ = 1;
    game_version_ = getVersionString();
    mods_hash_ = current_mods_hash();
    return true;
}

void host::stop( const std::string &reason )
{
    for( std::unique_ptr<remote> &r : remotes_ ) {
        if( !r->gone ) {
            drop( *r, reason );
        }
    }
    remotes_.clear();
    lst_.close();
    sched_ = scheduler();
}

size_t host::connected_remotes() const
{
    return static_cast<size_t>( std::count_if( remotes_.begin(), remotes_.end(),
    []( const std::unique_ptr<remote> &r ) {
        return r->welcomed && !r->gone;
    } ) );
}

host::remote *host::find_remote( player_id id )
{
    for( std::unique_ptr<remote> &r : remotes_ ) {
        if( r->id == id && !r->gone ) {
            return r.get();
        }
    }
    return nullptr;
}

const host::remote *host::find_remote_by_puppet( const npc &guy ) const
{
    for( const std::unique_ptr<remote> &r : remotes_ ) {
        if( r->welcomed && !r->gone && r->puppet == guy.getID() ) {
            return r.get();
        }
    }
    return nullptr;
}

bool host::is_puppet( const npc &guy ) const
{
    for( const auto &b : bindings_ ) {
        if( b.second == guy.getID() ) {
            return true;
        }
    }
    return false;
}

std::optional<character_id> host::puppet_of( player_id id ) const
{
    for( const std::unique_ptr<remote> &r : remotes_ ) {
        if( r->id == id && r->puppet.is_valid() ) {
            return r->puppet;
        }
    }
    return std::nullopt;
}

void host::drop( remote &r, const std::string &reason )
{
    if( r.gone ) {
        return;
    }
    if( r.conn && r.conn->is_open() ) {
        bye_msg bye;
        bye.reason = reason;
        r.conn->send( serialize( bye ) );
        r.conn->flush();
        r.conn->close( reason );
    }
    r.gone = true;
    r.queue.clear();
    sched_.remove_player( r.id );
    if( r.welcomed ) {
        // The puppet stays in the world, inert, until its player returns.
        if( npc *guy = g->find_npc( r.puppet ) ) {
            guy->add_effect( effect_npc_suspend, 1_turns, true );
        }
        add_msg( m_info, _( "%s left the game (%s)." ), r.name, reason );
    }
}

// --- Network ------------------------------------------------------------------

void host::poll()
{
    if( !active() ) {
        return;
    }
    while( std::unique_ptr<connection> c = lst_.accept() ) {
        std::unique_ptr<remote> r = std::make_unique<remote>();
        r->conn = std::move( c );
        r->connected_at_ms = now_ms();
        remotes_.push_back( std::move( r ) );
    }
    const std::int64_t now = now_ms();
    // Index loop: handle_message never adds remotes, but stay safe anyway.
    for( size_t i = 0; i < remotes_.size(); ++i ) {
        remote &r = *remotes_[i];
        if( r.gone ) {
            continue;
        }
        r.conn->flush();
        r.conn->pump();
        while( !r.gone ) {
            std::optional<std::string> msg = r.conn->next_message();
            if( !msg ) {
                break;
            }
            handle_message( r, *msg );
        }
        if( !r.gone && !r.conn->is_open() ) {
            drop( r, r.conn->close_reason() );
        }
        if( !r.gone && !r.welcomed && now - r.connected_at_ms > opts_.hello_timeout_ms ) {
            drop( r, "no hello received" );
        }
    }
}

void host::handle_message( remote &r, const std::string &payload )
{
    const std::optional<std::string> type = message_type( payload );
    if( !type ) {
        drop( r, "malformed message" );
        return;
    }
    if( !r.welcomed ) {
        if( *type == "hello" ) {
            handle_hello( r, payload );
        } else {
            drop( r, "expected hello" );
        }
        return;
    }
    if( *type == "cmd" ) {
        command_msg cmd;
        const std::string err = parse( payload, cmd );
        if( !err.empty() ) {
            ack( r, cmd.seq, err );
            return;
        }
        handle_command( r, cmd );
    } else if( *type == "chat" ) {
        chat_msg chat;
        if( parse( payload, chat ).empty() ) {
            // Never trust the sender field: the host knows who sent it.
            chat.from = r.id;
            add_msg( m_info, "<%s> %s", r.name, chat.text );
            const std::string out = serialize( chat );
            for( std::unique_ptr<remote> &other : remotes_ ) {
                if( other->welcomed && !other->gone ) {
                    other->conn->send( out );
                }
            }
        }
    } else if( *type == "bye" ) {
        drop( r, "quit" );
    }
    // Unknown types are ignored, so newer clients can talk to older hosts.
}

void host::handle_hello( remote &r, const std::string &payload )
{
    hello_msg hello;
    std::string err = parse( payload, hello );
    if( err.empty() ) {
        if( std::optional<std::string> why = check_hello( hello, game_version_, mods_hash_,
                                             opts_.password ) ) {
            err = *why;
        }
    }
    if( err.empty() && connected_remotes() >= opts_.max_players ) {
        err = "the game is full";
    }
    if( err.empty() ) {
        for( const std::unique_ptr<remote> &other : remotes_ ) {
            if( other.get() != &r && other->welcomed && !other->gone && other->uuid == hello.player_uuid ) {
                err = "this player is already connected";
            }
        }
    }
    if( !err.empty() ) {
        reject_msg rej;
        rej.reason = err;
        r.conn->send( serialize( rej ) );
        r.conn->flush();
        r.conn->close( err );
        r.gone = true;
        return;
    }
    r.name = hello.name;
    r.uuid = hello.player_uuid;
    // A returning player keeps their id.
    auto known = uuid_ids_.find( r.uuid );
    if( known != uuid_ids_.end() ) {
        r.id = known->second;
    } else {
        r.id = next_id_++;
        uuid_ids_.emplace( r.uuid, r.id );
    }
    if( !spawn_or_bind_puppet( r ) ) {
        reject_msg rej;
        rej.reason = "no room to place your character";
        r.conn->send( serialize( rej ) );
        r.conn->flush();
        r.conn->close( rej.reason );
        r.gone = true;
        return;
    }
    r.welcomed = true;
    sched_.add_player( r.id );

    welcome_msg w;
    w.id = r.id;
    if( world_generator && world_generator->active_world ) {
        w.world_name = world_generator->active_world->world_name;
    }
    w.pace_ms = opts_.sched.pace_ms;
    w.tactical_grace_ms = opts_.sched.tactical_grace_ms;
    r.conn->send( serialize( w ) );
    add_msg( m_good, _( "%s joined the game." ), r.name );
    if( npc *guy = g->find_npc( r.puppet ) ) {
        send_self( r, *guy );
    }
}

bool host::spawn_or_bind_puppet( remote &r )
{
    auto bound = bindings_.find( r.uuid );
    if( bound != bindings_.end() ) {
        if( npc *guy = g->find_npc( bound->second ) ) {
            if( !guy->is_dead() ) {
                guy->remove_effect( effect_npc_suspend );
                r.puppet = guy->getID();
                return true;
            }
        }
        bindings_.erase( bound );
    }

    // Find a free tile next to the host avatar.
    map &here = get_map();
    avatar &u = get_avatar();
    creature_tracker &creatures = get_creature_tracker();
    std::optional<tripoint_bub_ms> spot;
    for( int radius = 1; radius <= 4 && !spot; ++radius ) {
        for( int dx = -radius; dx <= radius && !spot; ++dx ) {
            for( int dy = -radius; dy <= radius && !spot; ++dy ) {
                const tripoint_bub_ms p = u.pos_bub() + tripoint_rel_ms( dx, dy, 0 );
                if( here.inbounds( p ) && here.passable( p ) &&
                    creatures.creature_at<Creature>( p ) == nullptr ) {
                    spot = p;
                }
            }
        }
    }
    if( !spot ) {
        return false;
    }

    shared_ptr_fast<npc> guy = make_shared_fast<npc>();
    guy->normalize();
    guy->randomize();
    guy->name = r.name;
    guy->spawn_at_precise( here.get_abs( *spot ) );
    overmap_buffer.insert_npc( guy );
    guy->set_attitude( NPCATT_FOLLOW );
    guy->set_fac( faction_your_followers );
    guy->mission = NPC_MISSION_NULL;
    g->load_npcs();
    r.puppet = guy->getID();
    bindings_[r.uuid] = r.puppet;
    return true;
}

void host::ack( remote &r, std::int64_t seq, const std::string &err )
{
    command_ack_msg a;
    a.seq = seq;
    a.ok = err.empty();
    a.reason = err;
    r.conn->send( serialize( a ) );
}

void host::handle_command( remote &r, const command_msg &cmd )
{
    // Votes and toggles cost no moves: apply them at once.
    switch( cmd.action ) {
        case action_type::ready:
            sched_.set_state( r.id, player_state::ready );
            ack( r, cmd.seq, sched_.state( r.id ) == player_state::ready ? std::string() :
                 std::string( "cannot skip time while someone is in danger" ) );
            return;
        case action_type::unready:
            sched_.set_state( r.id, player_state::input );
            ack( r, cmd.seq, std::string() );
            return;
        case action_type::tactical_on:
        case action_type::tactical_off:
            sched_.set_force_tactical( r.id, cmd.action == action_type::tactical_on );
            ack( r, cmd.seq, std::string() );
            return;
        case action_type::activity:
            ack( r, cmd.seq, "activity commands are not supported yet" );
            return;
        case action_type::move:
        case action_type::wait:
        case action_type::pause:
            break;
    }
    if( r.queue.size() >= opts_.max_queued_commands ) {
        ack( r, cmd.seq, "too many queued commands" );
        return;
    }
    r.queue.push_back( cmd );
}

// --- Turns ----------------------------------------------------------------------

void host::send_self( remote &r, const npc &guy )
{
    self_msg s;
    s.turn = to_turns<std::int64_t>( calendar::turn - calendar::turn_zero );
    const tripoint_abs_ms pos = guy.pos_abs();
    s.x = pos.x();
    s.y = pos.y();
    s.z = pos.z();
    s.hp = guy.get_hp();
    s.hp_max = guy.get_hp_max();
    s.moves = guy.get_moves();
    s.activity = guy.activity ? guy.activity.id().str() : std::string();
    s.danger = sched_.in_danger( r.id );
    r.conn->send( serialize( s ) );
}

void host::send_tick()
{
    const turn_poll p = sched_.poll( now_ms() );
    tick_msg t;
    t.turn = to_turns<std::int64_t>( calendar::turn - calendar::turn_zero );
    t.mode = p.mode;
    t.waiting_for = p.waiting_for;
    const std::string out = serialize( t );
    for( std::unique_ptr<remote> &r : remotes_ ) {
        if( r->welcomed && !r->gone ) {
            r->conn->send( out );
        }
    }
}

void host::begin_turn()
{
    if( !active() ) {
        return;
    }
    poll();
    // Forget closed connections; their puppets stay suspended.
    remotes_.erase( std::remove_if( remotes_.begin(), remotes_.end(),
    []( const std::unique_ptr<remote> &r ) {
        return r->gone;
    } ), remotes_.end() );

    sched_.begin_turn( now_ms() );

    avatar &u = get_avatar();
    sched_.set_state( host_player_id, derive_state( u, sched_.state( host_player_id ) ) );
    if( !u.get_hostile_creatures( danger_range ).empty() ) {
        sched_.report_danger( host_player_id );
    }
    for( std::unique_ptr<remote> &r : remotes_ ) {
        if( !r->welcomed || r->gone ) {
            continue;
        }
        npc *guy = g->find_npc( r->puppet );
        if( guy == nullptr || guy->is_dead() ) {
            bindings_.erase( r->uuid );
            drop( *r, "your character died" );
            continue;
        }
        sched_.set_state( r->id, derive_state( *guy, sched_.state( r->id ) ) );
        if( !guy->get_hostile_creatures( danger_range ).empty() ) {
            sched_.report_danger( r->id );
        }
    }
    send_tick();
    for( std::unique_ptr<remote> &r : remotes_ ) {
        if( r->welcomed && !r->gone ) {
            if( npc *guy = g->find_npc( r->puppet ) ) {
                send_self( *r, *guy );
            }
        }
    }
}

std::string host::execute( npc &guy, const command_msg &cmd )
{
    switch( cmd.action ) {
        case action_type::wait:
        case action_type::pause:
            guy.pause();
            return std::string();
        case action_type::move: {
            if( cmd.dz != 0 ) {
                return "changing z-level is not supported yet";
            }
            const tripoint_bub_ms dest = guy.pos_bub() + tripoint_rel_ms( cmd.dx, cmd.dy, 0 );
            if( !get_map().inbounds( dest ) ) {
                return "you can't leave your group behind";
            }
            if( Creature *c = get_creature_tracker().creature_at<Creature>( dest ) ) {
                if( c->attitude_to( guy ) == Creature::Attitude::FRIENDLY ) {
                    return "the way is blocked";
                }
                guy.melee_attack( *c, true );
                return std::string();
            }
            const int before = guy.get_moves();
            const tripoint_bub_ms from = guy.pos_bub();
            guy.move_to( dest, true );
            if( guy.get_moves() == before && guy.pos_bub() == from ) {
                return "you can't move there";
            }
            return std::string();
        }
        case action_type::ready:
        case action_type::unready:
        case action_type::tactical_on:
        case action_type::tactical_off:
        case action_type::activity:
            break;
    }
    return "unexpected command";
}

void host::run_puppet_turn( npc &guy )
{
    // The host avatar acts before NPCs in game::do_turn(), so by now it is
    // done for this turn.
    sched_.mark_done( host_player_id );
    const remote *found = find_remote_by_puppet( guy );
    if( found == nullptr ) {
        // Owner disconnected: stay put and do not hold the clock.
        guy.set_moves( 0 );
        return;
    }
    const player_id id = found->id;
    int actions = 0;
    while( guy.get_moves() > 0 && !guy.is_dead() && actions < max_actions_per_turn ) {
        poll();
        // poll() may have dropped the player; look them up again.
        remote *r = find_remote( id );
        if( r == nullptr ) {
            guy.set_moves( 0 );
            return;
        }
        if( !r->queue.empty() ) {
            const command_msg cmd = r->queue.front();
            r->queue.pop_front();
            // A new order interrupts a long activity.
            if( guy.activity ) {
                guy.cancel_activity();
            }
            ack( *r, cmd.seq, execute( guy, cmd ) );
            ++actions;
            continue;
        }
        if( guy.activity ) {
            guy.process_activity();
            ++actions;
            continue;
        }
        const turn_poll p = sched_.poll( now_ms() );
        if( contains( p.timed_out, id ) || !contains( p.waiting_for, id ) ) {
            // Out of time (or not waited for): pause for this turn.
            guy.pause();
            break;
        }
        // Wait for the player's next message, but never past the deadline.
        const int wait = p.wake_in_ms < 0 ? 50 : static_cast<int>( std::min<std::int64_t>( 50,
                         std::max<std::int64_t>( 1, p.wake_in_ms ) ) );
        connection *const conns[] = { r->conn.get() };
        wait_readable( conns, 1, nullptr, wait );
    }
    if( actions >= max_actions_per_turn && guy.get_moves() > 0 ) {
        guy.set_moves( 0 );
    }
    sched_.mark_done( id );
    if( remote *r = find_remote( id ) ) {
        send_self( *r, guy );
    }
}

bool host::host_input_timed_out()
{
    if( !active() ) {
        return false;
    }
    poll();
    if( connected_remotes() == 0 ) {
        return false;
    }
    return contains( sched_.poll( now_ms() ).timed_out, host_player_id );
}

std::string host::status_line() const
{
    const turn_poll p = sched_.poll( now_ms() );
    std::string out = to_string( p.mode );
    if( !p.waiting_for.empty() ) {
        out += " - waiting for ";
        bool first = true;
        for( const player_id id : p.waiting_for ) {
            std::string name = id == host_player_id ? std::string( "host" ) : std::to_string( id );
            for( const std::unique_ptr<remote> &r : remotes_ ) {
                if( r->id == id ) {
                    name = r->name;
                }
            }
            out += ( first ? "" : ", " ) + name;
            first = false;
        }
    }
    return out;
}

// --- Engine hooks ---------------------------------------------------------------

void on_turn_start()
{
    host &h = get_host();
    std::optional<host_options> &req = requested_options();
    if( req && !h.active() ) {
        std::string err;
        if( h.start( *req, err ) ) {
            add_msg( m_good, _( "Hosting a co-op game on port %d." ), h.port() );
        } else {
            add_msg( m_bad, _( "Cannot host a co-op game: %s" ), err );
        }
        // Try once; the player can restart the game to retry.
        req.reset();
    }
    h.begin_turn();
}

bool run_puppet_turn_hook( npc &guy )
{
    host &h = get_host();
    if( !h.active() || !h.is_puppet( guy ) ) {
        return false;
    }
    h.run_puppet_turn( guy );
    return true;
}

bool host_turn_timed_out()
{
    host &h = get_host();
    return h.active() && h.host_input_timed_out();
}

} // namespace mp
