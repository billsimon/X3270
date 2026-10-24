#include "TLSTransport.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <mutex>

#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace x3270 {

// ── OpenSSL one-time initialisation ──────────────────────────────────────────
static std::once_flag g_sslInitFlag;
static void opensslInit() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

// ── Helpers ──────────────────────────────────────────────────────────────────
static std::string sslLastError() {
    char buf[256];
    unsigned long err = ERR_get_error();
    if (err == 0) return "unknown SSL error";
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

static int tcpConnect(const std::string& host, uint16_t port, std::string& errorMsg) {
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string portStr = std::to_string(port);
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        errorMsg = std::string("DNS lookup failed: ") + gai_strerror(rc);
        return -1;
    }

    int sock = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        // A write to a socket the peer has reset must fail with EPIPE, not
        // raise SIGPIPE and kill the app (this covers OpenSSL's writes too).
        int one = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(sock);
        sock = -1;
    }
    ::freeaddrinfo(res);

    if (sock < 0) {
        errorMsg = std::string("TCP connect failed: ") + strerror(errno);
        return -1;
    }
    return sock;
}

/// Wait until fd is ready for `events`.  Returns false only if the fd is
/// unusable; readiness, hang-up and timeout all return true so the caller
/// retries the I/O call and lets it report the real outcome.
static bool waitFd(int fd, short events, int timeoutMs = -1) {
    struct pollfd p { fd, events, 0 };
    int r;
    do { r = ::poll(&p, 1, timeoutMs); } while (r < 0 && errno == EINTR);
    return r >= 0 && !(p.revents & POLLNVAL);
}

// ── TLSTransport ─────────────────────────────────────────────────────────────
TLSTransport::TLSTransport() {
    std::call_once(g_sslInitFlag, opensslInit);
}

TLSTransport::~TLSTransport() {
    close();
}

bool TLSTransport::connect(const std::string& host, uint16_t port,
                           bool useTLS,
                           bool verifyCert,
                           const std::string& caBundle,
                           std::string& errorMsg) {
    close();

    int sock = tcpConnect(host, port, errorMsg);
    if (sock < 0) return false;
    {
        // Publish the socket so disconnect() can abort the TLS handshake.
        std::lock_guard<std::mutex> lk(mutex_);
        if (aborted_) {
            ::close(sock);
            errorMsg = "Connection cancelled";
            return false;
        }
        sock_ = sock;
    }

    useTLS_ = useTLS;

    if (useTLS_) {
        // Build the SSL objects locally and publish them only once the
        // handshake is done: disconnect() may run concurrently with it.
        SSL_CTX* ctx = nullptr;
        SSL*     ssl = nullptr;
        auto fail = [&](const std::string& msg) {
            errorMsg = msg;
            if (ssl) SSL_free(ssl);
            if (ctx) SSL_CTX_free(ctx);
            close();
            return false;
        };

        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return fail("SSL_CTX_new failed: " + sslLastError());

        // Certificate verification
        if (verifyCert) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        } else {
        // Disable certificate verification (equivalent to the -noverifycert flag)
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }

        if (!caBundle.empty()) {
            if (SSL_CTX_load_verify_locations(ctx, caBundle.c_str(), nullptr) != 1)
                return fail("Failed to load CA bundle: " + sslLastError());
        } else {
            // Use macOS system certificates
            SSL_CTX_set_default_verify_paths(ctx);
        }

        // Minimum TLS 1.2
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#ifdef SSL_OP_NO_RENEGOTIATION
        // Renegotiation would let SSL_write consume inbound records behind
        // the reader thread's back.
        SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
#endif

        ssl = SSL_new(ctx);
        if (!ssl) return fail("SSL_new failed: " + sslLastError());

        SSL_set_fd(ssl, sock);
        // SNI
        SSL_set_tlsext_host_name(ssl, host.c_str());
        // Hostname verification
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!SSL_set1_host(ssl, host.c_str()))
            return fail("SSL_set1_host failed");

        if (SSL_connect(ssl) != 1)
            return fail("TLS handshake failed: " + sslLastError());

        // From here on reads and writes come from different threads and are
        // serialised by mutex_, so the socket must never block inside SSL_*.
        ::fcntl(sock, F_SETFL, ::fcntl(sock, F_GETFL) | O_NONBLOCK);

        std::lock_guard<std::mutex> lk(mutex_);
        ctx_ = ctx;
        ssl_ = ssl;
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!aborted_) {
            connected_ = true;
            return true;
        }
    }
    errorMsg = "Connection cancelled";
    close();
    return false;
}

void TLSTransport::disconnect() {
    std::lock_guard<std::mutex> lk(mutex_);
    aborted_   = true;
    connected_ = false;
    if (ssl_) { ERR_clear_error(); SSL_shutdown(ssl_); }   // best-effort close_notify
    if (sock_ >= 0) ::shutdown(sock_, SHUT_RDWR);           // wakes the reader
}

void TLSTransport::close() {
    {
        // Wake any send() blocked in poll() so it releases sendMutex_.
        std::lock_guard<std::mutex> lk(mutex_);
        connected_ = false;
        if (ssl_) { ERR_clear_error(); SSL_shutdown(ssl_); }
        if (sock_ >= 0) ::shutdown(sock_, SHUT_RDWR);
    }
    std::lock_guard<std::mutex> sendLk(sendMutex_);
    std::lock_guard<std::mutex> lk(mutex_);
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool TLSTransport::send(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> sendLk(sendMutex_);

    size_t sent = 0;
    while (sent < len) {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!connected_ || sock_ < 0) return false;
        int fd = sock_;
        if (useTLS_) {
            ERR_clear_error();
            int n = SSL_write(ssl_, data + sent, static_cast<int>(len - sent));
            if (n > 0) { sent += static_cast<size_t>(n); continue; }
            int err = SSL_get_error(ssl_, n);
            lk.unlock();
            // Retry with the same arguments, as SSL_write requires.
            if (err == SSL_ERROR_WANT_WRITE && waitFd(fd, POLLOUT)) continue;
            if (err == SSL_ERROR_WANT_READ  && waitFd(fd, POLLIN, 50)) continue;
        } else {
            // close() holds sendMutex_ while closing, so fd stays valid here.
            lk.unlock();
            ssize_t n = ::write(fd, data + sent, len - sent);
            if (n > 0) { sent += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
        }
        connected_ = false;
        return false;
    }
    return true;
}

bool TLSTransport::send(const std::vector<uint8_t>& data) {
    return send(data.data(), data.size());
}

int TLSTransport::recv(uint8_t* buf, size_t bufLen) {
    for (;;) {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!connected_ || sock_ < 0) return -1;
        int fd = sock_;

        if (!useTLS_) {
            // Plain sockets may be read and written concurrently; the read
            // blocks without the lock and disconnect() wakes it via shutdown().
            lk.unlock();
            ssize_t n = ::read(fd, buf, bufLen);
            if (n > 0) return static_cast<int>(n);
            if (n == 0) return 0;
            if (errno == EINTR) continue;
            connected_ = false;
            return -1;
        }

        ERR_clear_error();
        int n = SSL_read(ssl_, buf, static_cast<int>(bufLen));
        if (n > 0) return n;
        int err = SSL_get_error(ssl_, n);
        lk.unlock();
        if (err == SSL_ERROR_ZERO_RETURN) return 0; // clean close
        if (err == SSL_ERROR_WANT_READ  && waitFd(fd, POLLIN))  continue;
        if (err == SSL_ERROR_WANT_WRITE && waitFd(fd, POLLOUT)) continue;
        connected_ = false;
        return -1;
    }
}

} // namespace x3270
