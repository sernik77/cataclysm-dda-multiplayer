#include "mp_transport.h"

#include <algorithm>
#include <utility>
#include <vector>

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define CATA_MP_NO_SOCKETS 1
#endif

#if !defined(CATA_MP_NO_SOCKETS)
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mp
{

#if defined(CATA_MP_NO_SOCKETS)

bool transport_supported()
{
    return false;
}

connection::connection( int fd, std::string peer ) : fd_( fd ), peer_( std::move( peer ) ) {}
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

bool transport_supported()
{
    return true;
}

static bool would_block( int err )
{
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    return err == EAGAIN || err == EWOULDBLOCK;
#else
    return err == EAGAIN;
#endif
}

static bool set_nonblocking( int fd )
{
    const int flags = fcntl( fd, F_GETFL, 0 );
    return flags >= 0 && fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0;
}

static void tune_socket( int fd )
{
    // Turn-based traffic is many small messages: do not batch them.
    int one = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
#if defined(SO_NOSIGPIPE)
    setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof( one ) );
#endif
}

static std::string describe_peer( const sockaddr_storage &addr )
{
    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    if( getnameinfo( reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ), host,
                     sizeof( host ), serv, sizeof( serv ), NI_NUMERICHOST | NI_NUMERICSERV ) != 0 ) {
        return "unknown";
    }
    return std::string( host ) + ":" + serv;
}

connection::connection( int fd, std::string peer ) : fd_( fd ), peer_( std::move( peer ) ) {}

connection::~connection()
{
    close( "destroyed" );
}

void connection::close( const std::string &reason )
{
    if( fd_ >= 0 ) {
        ::close( fd_ );
        fd_ = -1;
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
#if defined(MSG_NOSIGNAL)
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t n = ::send( fd_, out_.data() + out_pos_, out_.size() - out_pos_, flags );
        if( n > 0 ) {
            out_pos_ += static_cast<size_t>( n );
            continue;
        }
        if( n < 0 && would_block( errno ) ) {
            return true;
        }
        if( n < 0 && errno == EINTR ) {
            continue;
        }
        close( std::string( "send failed: " ) + std::strerror( errno ) );
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
        const ssize_t n = ::recv( fd_, buf, sizeof( buf ), 0 );
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
        if( would_block( errno ) ) {
            return true;
        }
        if( errno == EINTR ) {
            continue;
        }
        close( std::string( "recv failed: " ) + std::strerror( errno ) );
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
    if( fd_ >= 0 ) {
        ::close( fd_ );
        fd_ = -1;
    }
}

bool listener::open( std::uint16_t port, bool loopback_only, std::string &err )
{
    close();
    const int fd = ::socket( AF_INET, SOCK_STREAM, 0 );
    if( fd < 0 ) {
        err = std::string( "socket failed: " ) + std::strerror( errno );
        return false;
    }
    int one = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );
    addr.sin_addr.s_addr = htonl( loopback_only ? INADDR_LOOPBACK : INADDR_ANY );
    if( ::bind( fd, reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) ) != 0 ) {
        err = std::string( "bind failed: " ) + std::strerror( errno );
        ::close( fd );
        return false;
    }
    if( ::listen( fd, 8 ) != 0 || !set_nonblocking( fd ) ) {
        err = std::string( "listen failed: " ) + std::strerror( errno );
        ::close( fd );
        return false;
    }
    socklen_t len = sizeof( addr );
    if( getsockname( fd, reinterpret_cast<sockaddr *>( &addr ), &len ) != 0 ) {
        err = std::string( "getsockname failed: " ) + std::strerror( errno );
        ::close( fd );
        return false;
    }
    fd_ = fd;
    port_ = ntohs( addr.sin_port );
    return true;
}

std::unique_ptr<connection> listener::accept()
{
    if( fd_ < 0 ) {
        return nullptr;
    }
    sockaddr_storage addr {};
    socklen_t len = sizeof( addr );
    const int fd = ::accept( fd_, reinterpret_cast<sockaddr *>( &addr ), &len );
    if( fd < 0 ) {
        return nullptr;
    }
    if( !set_nonblocking( fd ) ) {
        ::close( fd );
        return nullptr;
    }
    tune_socket( fd );
    return std::make_unique<connection>( fd, describe_peer( addr ) );
}

std::unique_ptr<connection> connect_to( const std::string &host, std::uint16_t port,
                                        int timeout_ms, std::string &err )
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const std::string service = std::to_string( port );
    const int gai = getaddrinfo( host.c_str(), service.c_str(), &hints, &res );
    if( gai != 0 ) {
        err = std::string( "cannot resolve " ) + host + ": " + gai_strerror( gai );
        return nullptr;
    }
    err = "no address for " + host;
    for( addrinfo *ai = res; ai != nullptr; ai = ai->ai_next ) {
        const int fd = ::socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
        if( fd < 0 ) {
            err = std::string( "socket failed: " ) + std::strerror( errno );
            continue;
        }
        if( !set_nonblocking( fd ) ) {
            err = std::string( "fcntl failed: " ) + std::strerror( errno );
            ::close( fd );
            continue;
        }
        int rc = ::connect( fd, ai->ai_addr, ai->ai_addrlen );
        if( rc != 0 && errno == EINPROGRESS ) {
            pollfd pfd { fd, POLLOUT, 0 };
            rc = ::poll( &pfd, 1, timeout_ms );
            if( rc == 1 ) {
                int so_error = 0;
                socklen_t len = sizeof( so_error );
                getsockopt( fd, SOL_SOCKET, SO_ERROR, &so_error, &len );
                rc = so_error == 0 ? 0 : -1;
                errno = so_error;
            } else {
                errno = rc == 0 ? ETIMEDOUT : errno;
                rc = -1;
            }
        }
        if( rc != 0 ) {
            err = "cannot connect to " + host + ":" + service + ": " + std::strerror( errno );
            ::close( fd );
            continue;
        }
        tune_socket( fd );
        sockaddr_storage peer {};
        std::memcpy( &peer, ai->ai_addr, std::min<size_t>( ai->ai_addrlen, sizeof( peer ) ) );
        freeaddrinfo( res );
        err.clear();
        return std::make_unique<connection>( fd, describe_peer( peer ) );
    }
    freeaddrinfo( res );
    return nullptr;
}

bool wait_readable( connection *const *conns, size_t count, const listener *lst,
                    int timeout_ms )
{
    std::vector<pollfd> fds;
    fds.reserve( count + 1 );
    for( size_t i = 0; i < count; ++i ) {
        if( conns[i] != nullptr && conns[i]->is_open() ) {
            fds.push_back( pollfd { conns[i]->native_handle(), POLLIN, 0 } );
        }
    }
    if( lst != nullptr && lst->is_open() ) {
        fds.push_back( pollfd { lst->native_handle(), POLLIN, 0 } );
    }
    if( fds.empty() ) {
        return false;
    }
    const int rc = ::poll( fds.data(), static_cast<nfds_t>( fds.size() ), timeout_ms );
    return rc > 0;
}

#endif

} // namespace mp
