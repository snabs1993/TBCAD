#include "tbcad/nettls/tls.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#include <chrono>
#include <cstdio>
#include <mutex>

namespace tbcad::nettls {

namespace {

using Clock = std::chrono::steady_clock;

std::string describe(int code) {
    char buf[160];
    mbedtls_strerror(code, buf, sizeof buf);
    return std::string(buf) + " (-0x" + [&] {
        char h[16];
        std::snprintf(h, sizeof h, "%04X", static_cast<unsigned>(-code));
        return std::string(h);
    }() + ")";
}

int sendCallback(void* ctx, const unsigned char* buf, std::size_t len) {
    auto h = *static_cast<net::Handle*>(ctx);
    return net::sendAll(h, buf, len) ? static_cast<int>(len) : MBEDTLS_ERR_SSL_CONN_EOF;
}

// Never waits: callers poll the socket first. Handshake uses handshakeWaitMs below.
struct Bio {
    net::Handle handle = net::kInvalid;
    int waitMs = 0;
};

int sendBio(void* ctx, const unsigned char* buf, std::size_t len) {
    return sendCallback(&static_cast<Bio*>(ctx)->handle, buf, len);
}

int recvBio(void* ctx, unsigned char* buf, std::size_t len) {
    auto* bio = static_cast<Bio*>(ctx);
    long n = net::receive(bio->handle, buf, len, bio->waitMs);
    if (n > 0)
        return static_cast<int>(n);
    if (n == net::kTimeout)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_CONN_EOF;
}

bool initPsa() {
    static const bool ok = psa_crypto_init() == PSA_SUCCESS;
    return ok;
}

class TlsChannel final : public net::Channel {
public:
    TlsChannel() {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_entropy_init(&entropy_);
    }
    ~TlsChannel() override {
        close();
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    bool start(bool server, net::Handle socket, const Psk& psk, int timeoutMs, std::string& error) {
        bio_.handle = socket;
        if (!initPsa()) {
            error = "psa_crypto_init failed";
            return false;
        }
        if (psk.key.size() != 32 || psk.identity.empty()) {
            error = "a TLS key must be 32 bytes and come with an identity";
            return false;
        }
        int rc = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, nullptr, 0);
        if (rc == 0)
            rc = mbedtls_ssl_config_defaults(
                &conf_, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) {
            error = describe(rc);
            return false;
        }
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_tls13_key_exchange_modes(
            &conf_, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
        rc = mbedtls_ssl_conf_psk(&conf_, psk.key.data(), psk.key.size(),
                                  reinterpret_cast<const unsigned char*>(psk.identity.data()),
                                  psk.identity.size());
        if (rc == 0)
            rc = mbedtls_ssl_setup(&ssl_, &conf_);
        if (rc != 0) {
            error = describe(rc);
            return false;
        }
        mbedtls_ssl_set_bio(&ssl_, &bio_, sendBio, recvBio, nullptr);

        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (left.count() <= 0) {
                error = "TLS handshake timed out";
                return false;
            }
            bio_.waitMs = static_cast<int>(left.count() < 250 ? left.count() : 250);
            rc = mbedtls_ssl_handshake(&ssl_);
            if (rc == 0)
                break;
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                error = "TLS handshake failed: " + describe(rc);
                return false;
            }
        }
        bio_.waitMs = 0;
        open_ = true;
        return true;
    }

    long read(void* buffer, std::size_t size, int timeoutMs) override {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!open_)
                    return net::kClosed;
                // Decrypted bytes may already be buffered; otherwise only touch the context when
                // the socket has something, so a blocked reader never holds up writers.
                if (mbedtls_ssl_get_bytes_avail(&ssl_) > 0 || pending_) {
                    pending_ = false;
                    long r = readLocked(buffer, size);
                    if (r != kRetry)
                        return r;
                }
            }
            auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (left.count() <= 0)
                return net::kTimeout;
            if (!net::waitReadable(bio_.handle,
                                   static_cast<int>(left.count() < 100 ? left.count() : 100)))
                continue;
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_)
                return net::kClosed;
            long r = readLocked(buffer, size);
            if (r != kRetry)
                return r;
        }
    }

    bool writeAll(const void* data, std::size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
            return false;
        const auto* p = static_cast<const unsigned char*>(data);
        bio_.waitMs = 1000;
        while (size > 0) {
            int n = mbedtls_ssl_write(&ssl_, p, size);
            if (n > 0) {
                p += n;
                size -= static_cast<std::size_t>(n);
            } else if (n != MBEDTLS_ERR_SSL_WANT_WRITE && n != MBEDTLS_ERR_SSL_WANT_READ) {
                bio_.waitMs = 0;
                return false;
            }
        }
        bio_.waitMs = 0;
        return true;
    }

    void shutdown() override {
        if (bio_.handle != net::kInvalid)
            net::shutdownSocket(bio_.handle);
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bio_.handle == net::kInvalid)
            return;
        if (open_) {
            bio_.waitMs = 0;
            mbedtls_ssl_close_notify(&ssl_);
        }
        open_ = false;
        net::closeSocket(bio_.handle);
        bio_.handle = net::kInvalid;
    }

    void abandon() {
        if (bio_.handle != net::kInvalid) {
            net::closeSocket(bio_.handle);
            bio_.handle = net::kInvalid;
        }
    }

private:
    static constexpr long kRetry = -1000;

    long readLocked(void* buffer, std::size_t size) {
        bio_.waitMs = 0;
        int n = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(buffer), size);
        if (n > 0) {
            pending_ = mbedtls_ssl_get_bytes_avail(&ssl_) > 0;
            return n;
        }
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
            return net::kClosed;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            return kRetry;
        return net::kError;
    }

    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_ctr_drbg_context drbg_;
    mbedtls_entropy_context entropy_;
    Bio bio_;
    std::mutex mutex_;
    bool open_ = false;
    bool pending_ = false;
};

std::unique_ptr<net::Channel> start(bool server, net::Handle socket, const Psk& psk, int timeoutMs,
                                    std::string& error) {
    auto ch = std::make_unique<TlsChannel>();
    if (!ch->start(server, socket, psk, timeoutMs, error)) {
        ch->abandon();
        return nullptr;
    }
    return ch;
}

}  // namespace

Psk derivePsk(const std::string& password, const std::string& identity,
              const net::Argon2Params& params) {
    std::string label = "TBCAD collab v1:" + identity;
    std::vector<std::uint8_t> salt = net::blake2b(label.data(), label.size(), 16);
    net::Argon2Params p = params;
    p.tagLength = 32;
    Psk psk;
    psk.identity = identity;
    psk.key = net::argon2id(password, salt, p);
    return psk;
}

std::unique_ptr<net::Channel> connectTls(net::Handle socket, const Psk& psk, int timeoutMs,
                                         std::string& error) {
    return start(false, socket, psk, timeoutMs, error);
}

std::unique_ptr<net::Channel> acceptTls(net::Handle socket, const Psk& psk, int timeoutMs,
                                        std::string& error) {
    return start(true, socket, psk, timeoutMs, error);
}

}  // namespace tbcad::nettls
