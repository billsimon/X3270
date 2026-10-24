#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <openssl/ssl.h>

namespace x3270 {

/// Low-level transport: wraps a TCP socket with optional OpenSSL TLS.
///
/// Threading: one reader thread calls connect(), recv() and finally close();
/// any thread may call send() and disconnect() concurrently with it.
/// OpenSSL forbids concurrent use of one SSL object, so every SSL_* call is
/// made under mutex_; the reader waits for data in poll() without the lock.
class TLSTransport {
public:
    TLSTransport();
    ~TLSTransport();

    // Non-copyable
    TLSTransport(const TLSTransport&) = delete;
    TLSTransport& operator=(const TLSTransport&) = delete;

    /// Connect to host:port.  If useTLS=true the TLS handshake is performed
    /// immediately after the TCP connection (implicit TLS, not STARTTLS).
    /// caBundle: optional path to a PEM CA bundle; empty = use system defaults.
    /// Returns true on success; on failure errorMsg is populated.
    bool connect(const std::string& host, uint16_t port,
                 bool useTLS,
                 bool verifyCert,
                 const std::string& caBundle,
                 std::string& errorMsg);

    /// Thread-safe: wakes a blocked recv(), makes further I/O fail and aborts
    /// a connect() in progress.  Does not free anything — see close().
    /// A transport that has been disconnected cannot connect again.
    void disconnect();

    /// Release the socket and TLS objects.  Call from the reader thread once
    /// recv() has returned (or when no reader exists); waits for any send()
    /// in progress on another thread.
    void close();

    bool isConnected() const { return connected_; }

    /// Send bytes.  Returns false if the connection dropped.
    bool send(const uint8_t* data, size_t len);
    bool send(const std::vector<uint8_t>& data);

    /// Blocking read up to bufLen bytes into buf.
    /// Returns number of bytes read, 0 on clean close, -1 on error.
    int recv(uint8_t* buf, size_t bufLen);

private:
    int               sock_      { -1 };
    std::atomic<bool> connected_ { false };
    bool              aborted_   { false };   // disconnect() was called
    bool              useTLS_    { false };
    SSL_CTX*          ctx_       { nullptr };
    SSL*              ssl_       { nullptr };

    std::mutex        mutex_;      // guards sock_/ssl_/ctx_/aborted_ and every SSL_* call
    std::mutex        sendMutex_;  // keeps concurrent send() calls from interleaving
};

} // namespace x3270
