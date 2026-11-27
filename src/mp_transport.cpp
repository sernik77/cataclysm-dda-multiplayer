#include "mp_transport.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#define CATA_MP_NO_SOCKETS 1
#endif

#if !defined(CATA_MP_NO_SOCKETS)
#if defined(_WIN32)
#include "platform_win.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#endif

namespace mp
{

#if defined(CATA_MP_NO_SOCKETS)

bool transport_supported()
{
    return false;
}

connection::connection( std::intptr_t fd, std::string peer ) : fd_( fd ),
    peer_( std::move( peer ) ) {}
connection::~connection() = default;
bool connection::send( const std::string & )
{
    return false;
}
bool connection::flush()
{
    return false;
}
bool connection::pump()
{
    return false;
}
std::optional<std::string> connection::next_message()
{
    return std::nullopt;
}
void connection::close( const std::string &reason )
{
    fd_ = -1;
    close_reason_ = reason;
}

listener::~listener() = default;
bool listener::open( std::uint16_t, bool, std::string &err )
{
    err = "multiplayer is not supported on this platform";
    return false;
}
void listener::close() {}
std::unique_ptr<connection> listener::accept()
{
    return nullptr;
}

std::unique_ptr<connection> connect_to( const std::string &, std::uint16_t, int,
                                        std::string &err )
{
    err = "multiplayer is not supported on this platform";
    return nullptr;
}

bool wait_readable( connection *const *, size_t, const listener *, int )
{
    return false;
}

#else

// --- Platform layer -----------------------------------------------------------
// Everything after this block is written once for both socket APIs.

namespace
{

#if defined(_WIN32)

using sock_t = SOCKET;
using poll_entry = WSAPOLLFD;
using io_len = int;

bool net_init()
{
    // Winsock must be started once per process before any socket call.
    static const bool ok = []() {
        WSADATA data;
        return WSAStartup( MAKEWORD( 2, 2 ), &data ) == 0;
    }
    ();
    return ok;
}

int last_error()
{
    return WSAGetLastError();
}

bool would_block( int err )
{
    return err == WSAEWOULDBLOCK;
}

bool connect_pending( int err )
{
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

bool interrupted( int err )
{
    return err == WSAEINTR;
}

std::string error_text( int err )
{
    char buf[256] = {};
    const DWORD n = FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    nullptr, static_cast<DWORD>( err ), 0, buf, sizeof( buf ), nullptr );
    std::string text = n > 0 ? std::string( buf, n ) : std::string();
    while( !text.empty() && ( text.back() == '\n' || text.back() == '\r' || text.back() == '.' ) ) {
        text.pop_back();
    }
    return text.empty() ? "error " + std::to_string( err ) : text;
}

void close_socket( sock_t s )
{
    closesocket( s );
}

bool set_nonblocking( sock_t s )
{
    u_long on = 1;
    return ioctlsocket( s, FIONBIO, &on ) == 0;
}

int poll_sockets( poll_entry *fds, size_t n, int timeout_ms )
{
    return WSAPoll( fds, static_cast<ULONG>( n ), timeout_ms );
}

constexpr int send_flags = 0;
constexpr int timed_out_error = WSAETIMEDOUT;

bool valid( sock_t s )
{
    return s != INVALID_SOCKET;
}

#else

using sock_t = int;
using poll_entry = pollfd;
using io_len = size_t;

bool net_init()
{
    return true;
}

int last_error()
{
    return errno;
}

bool would_block( int err )
{
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    return err == EAGAIN || err == EWOULDBLOCK;
#else
    return err == EAGAIN;
#endif
}

bool connect_pending( int err )
{
    return err == EINPROGRESS;
}

bool interrupted( int err )
{
    return err == EINTR;
}

std::string error_text( int err )
{
    return std::strerror( err );
}

void close_socket( sock_t s )
{
    ::close( s );
}

bool set_nonblocking( sock_t s )
{
    const int flags = fcntl( s, F_GETFL, 0 );
    return flags >= 0 && fcntl( s, F_SETFL, flags | O_NONBLOCK ) == 0;
}

int poll_sockets( poll_entry *fds, size_t n, int timeout_ms )
{
    return ::poll( fds, static_cast<nfds_t>( n ), timeout_ms );
}

#if defined(MSG_NOSIGNAL)
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif
constexpr int timed_out_error = ETIMEDOUT;

bool valid( sock_t s )
{
    return s >= 0;
}

#endif

constexpr std::intptr_t no_socket = -1;

sock_t to_sock( std::intptr_t h )
{
    return static_cast<sock_t>( h );
}

std::intptr_t to_handle( sock_t s )
{
    return static_cast<std::intptr_t>( s );
}

void set_option( sock_t s, int level, int name )
{
    int one = 1;
    setsockopt( s, level, name, reinterpret_cast<const char *>( &one ), sizeof( one ) );
}

void tune_socket( sock_t s )
{
    // Turn-based traffic is many small messages: do not batch them.
    set_option( s, IPPROTO_TCP, TCP_NODELAY );
#if defined(SO_NOSIGPIPE)
    set_option( s, SOL_SOCKET, SO_NOSIGPIPE );
#endif
}

std::string describe_peer( const sockaddr *addr, socklen_t len )
{
    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    if( getnameinfo( addr, len, host, sizeof( host ), serv, sizeof( serv ),
                     NI_NUMERICHOST | NI_NUMERICSERV ) != 0 ) {
        return "unknown";
    }
    return std::string( host ) + ":" + serv;
}

} // namespace

// --- Shared implementation ------------------------------------------------------

bool transport_supported()
{
    return net_init();
}

connection::connection( std::intptr_t fd, std::string peer ) : fd_( fd ),
    peer_( std::move( peer ) ) {}

connection::~connection()
{
    close( "destroyed" );
}

void connection::close( const std::string &reason )
{
    if( fd_ != no_socket ) {
        close_socket( to_sock( fd_ ) );
        fd_ = no_socket;
        close_reason_ = reason;
    }
}

bool connection::send( const std::string &payload )
{
    if( !is_open() ) {
        return false;
    }
    if( payload.size() > max_frame_bytes ) {
        return false;
    }
    if( pending_output() + payload.size() + 4 > max_output_bytes ) {
        close( "output queue overflow" );
        return false;
    }
    if( out_pos_ > 0 && out_pos_ == out_.size() ) {
        out_.clear();
        out_pos_ = 0;
    }
    out_ += encode_frame( payload );
    return flush();
}

bool connection::flush()
{
    while( is_open() && out_pos_ < out_.size() ) {
        const size_t chunk = std::min<size_t>( out_.size() - out_pos_, 1U << 20 );
        const auto n = ::send( to_sock( fd_ ), out_.data() + out_pos_, static_cast<io_len>( chunk ),
                               send_flags );
        if( n > 0 ) {
            out_pos_ += static_cast<size_t>( n );
            continue;
        }
        const int err = last_error();
        if( n < 0 && would_block( err ) ) {
            return true;
        }
        if( n < 0 && interrupted( err ) ) {
            continue;
        }
        close( "send failed: " + error_text( err ) );
        return false;
    }
    if( out_pos_ == out_.size() ) {
        out_.clear();
        out_pos_ = 0;
    }
    return is_open();
}

bool connection::pump()
{
    char buf[16384];
    while( is_open() ) {
        const auto n = ::recv( to_sock( fd_ ), buf, static_cast<io_len>( sizeof( buf ) ), 0 );
        if( n > 0 ) {
            if( !in_.feed( buf, static_cast<size_t>( n ) ) ) {
                close( "protocol error: " + in_.error() );
                return false;
            }
            continue;
        }
        if( n == 0 ) {
            close( "peer closed the connection" );
            return false;
        }
        const int err = last_error();
        if( would_block( err ) ) {
            return true;
        }
        if( interrupted( err ) ) {
            continue;
        }
        close( "recv failed: " + error_text( err ) );
        return false;
    }
    return false;
}

std::optional<std::string> connection::next_message()
{
    std::optional<std::string> msg = in_.next();
    if( !msg && in_.failed() && is_open() ) {
        close( "protocol error: " + in_.error() );
    }
    return msg;
}

listener::~listener()
{
    close();
}

void listener::close()
{
    if( fd_ != no_socket ) {
        close_socket( to_sock( fd_ ) );
        fd_ = no_socket;
    }
}

bool listener::open( std::uint16_t port, bool loopback_only, std::string &err )
{
    close();
    if( !net_init() ) {
        err = "cannot start the network stack";
        return false;
    }
    const sock_t s = ::socket( AF_INET, SOCK_STREAM, 0 );
    if( !valid( s ) ) {
        err = "socket failed: " + error_text( last_error() );
        return false;
    }
#if defined(_WIN32)
    // On Windows SO_REUSEADDR lets another process steal the port; exclusive
    // use is the safe equivalent of the POSIX behaviour.
    set_option( s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE );
#else
    set_option( s, SOL_SOCKET, SO_REUSEADDR );
#endif
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );
    addr.sin_addr.s_addr = htonl( loopback_only ? INADDR_LOOPBACK : INADDR_ANY );
    if( ::bind( s, reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) ) != 0 ) {
        err = "bind failed: " + error_text( last_error() );
        close_socket( s );
        return false;
    }
    if( ::listen( s, 8 ) != 0 || !set_nonblocking( s ) ) {
        err = "listen failed: " + error_text( last_error() );
        close_socket( s );
        return false;
    }
    socklen_t len = sizeof( addr );
    if( getsockname( s, reinterpret_cast<sockaddr *>( &addr ), &len ) != 0 ) {
        err = "getsockname failed: " + error_text( last_error() );
        close_socket( s );
        return false;
    }
    fd_ = to_handle( s );
    port_ = ntohs( addr.sin_port );
    return true;
}

std::unique_ptr<connection> listener::accept()
{
    if( fd_ == no_socket ) {
        return nullptr;
    }
    sockaddr_storage addr {};
    socklen_t len = sizeof( addr );
    const sock_t s = ::accept( to_sock( fd_ ), reinterpret_cast<sockaddr *>( &addr ), &len );
    if( !valid( s ) ) {
        return nullptr;
    }
    if( !set_nonblocking( s ) ) {
        close_socket( s );
        return nullptr;
    }
    tune_socket( s );
    return std::make_unique<connection>( to_handle( s ),
                                         describe_peer( reinterpret_cast<const sockaddr *>( &addr ), len ) );
}

std::unique_ptr<connection> connect_to( const std::string &host, std::uint16_t port,
                                        int timeout_ms, std::string &err )
{
    if( !net_init() ) {
        err = "cannot start the network stack";
        return nullptr;
    }
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const std::string service = std::to_string( port );
    if( getaddrinfo( host.c_str(), service.c_str(), &hints, &res ) != 0 ) {
        err = "cannot resolve " + host;
        return nullptr;
    }
    err = "no address for " + host;
    for( addrinfo *ai = res; ai != nullptr; ai = ai->ai_next ) {
        const sock_t s = ::socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
        if( !valid( s ) ) {
            err = "socket failed: " + error_text( last_error() );
            continue;
        }
        if( !set_nonblocking( s ) ) {
            err = "cannot make the socket non-blocking: " + error_text( last_error() );
            close_socket( s );
            continue;
        }
        int failure = 0;
        if( ::connect( s, ai->ai_addr, static_cast<socklen_t>( ai->ai_addrlen ) ) != 0 ) {
            failure = last_error();
            if( connect_pending( failure ) ) {
                poll_entry pfd {};
                pfd.fd = s;
                pfd.events = POLLOUT;
                const int rc = poll_sockets( &pfd, 1, timeout_ms );
                if( rc == 1 ) {
                    int so_error = 0;
                    socklen_t len = sizeof( so_error );
                    getsockopt( s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>( &so_error ), &len );
                    failure = so_error;
                } else {
                    failure = rc == 0 ? timed_out_error : last_error();
                }
            }
        }
        if( failure != 0 ) {
            err = "cannot connect to " + host + ":" + service + ": " + error_text( failure );
            close_socket( s );
            continue;
        }
        tune_socket( s );
        const std::string peer = describe_peer( ai->ai_addr, static_cast<socklen_t>( ai->ai_addrlen ) );
        freeaddrinfo( res );
        err.clear();
        return std::make_unique<connection>( to_handle( s ), peer );
    }
    freeaddrinfo( res );
    return nullptr;
}

bool wait_readable( connection *const *conns, size_t count, const listener *lst,
                    int timeout_ms )
{
    std::vector<poll_entry> fds;
    fds.reserve( count + 1 );
    const auto add = [&fds]( std::intptr_t h ) {
        poll_entry e {};
        e.fd = to_sock( h );
        e.events = POLLIN;
        fds.push_back( e );
    };
    for( size_t i = 0; i < count; ++i ) {
        if( conns[i] != nullptr && conns[i]->is_open() ) {
            add( conns[i]->native_handle() );
        }
    }
    if( lst != nullptr && lst->is_open() ) {
        add( lst->native_handle() );
    }
    if( fds.empty() ) {
        return false;
    }
    return poll_sockets( fds.data(), fds.size(), timeout_ms ) > 0;
}

#endif

} // namespace mp
