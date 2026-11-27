#pragma once
#ifndef CATA_SRC_MP_TRANSPORT_H
#define CATA_SRC_MP_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mp_framing.h"

// Multiplayer TCP transport. Non-blocking sockets, polled from the main
// thread; the game is single-threaded, so nothing here takes a lock.
//
// Implemented with POSIX sockets and with Winsock 2 on Windows. On Emscripten
// every operation fails with an "unsupported" error, so the game still
// builds and single player is unaffected.
namespace mp
{

// True if this build has a working network transport.
bool transport_supported();

class connection
{
    public:
        explicit connection( std::intptr_t fd, std::string peer );
        ~connection();
        connection( const connection & ) = delete;
        connection &operator=( const connection & ) = delete;

        bool is_open() const {
            return fd_ >= 0;
        }
        const std::string &peer() const {
            return peer_;
        }
        // OS socket handle (a SOCKET on Windows), -1 when closed.
        std::intptr_t native_handle() const {
            return fd_;
        }
        // Why the connection closed; empty while open.
        const std::string &close_reason() const {
            return close_reason_;
        }

        // Queues one message (framed). Returns false if closed or if the
        // outgoing queue would exceed the limit (slow or stuck peer).
        bool send( const std::string &payload );
        // Writes as much of the queue as the socket accepts now.
        // Returns false if the connection closed.
        bool flush();
        // Reads everything available now into the frame decoder.
        // Returns false if the connection closed or the stream is corrupt.
        bool pump();
        // Next complete incoming message, if any.
        std::optional<std::string> next_message();
        size_t pending_output() const {
            return out_.size() - out_pos_;
        }

        void close( const std::string &reason );

        // Outgoing queue limit in bytes.
        static constexpr size_t max_output_bytes = 64U * 1024U * 1024U;

    private:
        std::intptr_t fd_;
        std::string peer_;
        std::string close_reason_;
        frame_decoder in_;
        std::string out_;
        size_t out_pos_ = 0;
};

class listener
{
    public:
        listener() = default;
        ~listener();
        listener( const listener & ) = delete;
        listener &operator=( const listener & ) = delete;

        // Binds and listens. port 0 picks a free port (see port()).
        // loopback_only binds 127.0.0.1 instead of all interfaces.
        bool open( std::uint16_t port, bool loopback_only, std::string &err );
        void close();
        bool is_open() const {
            return fd_ >= 0;
        }
        std::uint16_t port() const {
            return port_;
        }
        std::intptr_t native_handle() const {
            return fd_;
        }
        // Accepts one pending client, if any. Never blocks.
        std::unique_ptr<connection> accept();

    private:
        std::intptr_t fd_ = -1;
        std::uint16_t port_ = 0;
};

// Connects to host:port, waiting at most timeout_ms.
std::unique_ptr<connection> connect_to( const std::string &host, std::uint16_t port,
                                        int timeout_ms, std::string &err );

// Waits until one of the given connections (or the listener) has data, or
// timeout_ms passes. Returns false on timeout. Any pointer may be null.
bool wait_readable( connection *const *conns, size_t count, const listener *lst,
                    int timeout_ms );

} // namespace mp

#endif // CATA_SRC_MP_TRANSPORT_H
