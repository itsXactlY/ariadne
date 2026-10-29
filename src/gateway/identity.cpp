// src/gateway/identity.cpp — impl of dd/gateway/identity.hpp.

#include "dd/gateway/identity.hpp"

#include "dd/gateway/crypto.hpp"

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <sys/stat.h>

#include <fstream>
#include <memory>
#include <sstream>

namespace dd {
namespace gateway {

namespace fs = std::filesystem;

namespace {

template <class T, void (*F)(T*)>
struct Del {
    void operator()(T* p) const { F(p); }
};
using Bio = std::unique_ptr<BIO, Del<BIO, BIO_free_all>>;
using Pkey = std::unique_ptr<EVP_PKEY, Del<EVP_PKEY, EVP_PKEY_free>>;
using Cert = std::unique_ptr<X509, Del<X509, X509_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Del<EVP_MD_CTX, EVP_MD_CTX_free>>;

std::string openssl_error() {
    const unsigned long e = ERR_get_error();
    if (!e) return "unknown OpenSSL error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

bool write_file(const fs::path& p, const std::string& data, mode_t mode, std::string& error) {
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { error = "cannot write " + tmp.string(); return false; }
        f << data;
        if (!f) { error = "cannot write " + tmp.string(); return false; }
    }
    ::chmod(tmp.c_str(), mode);
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { error = "cannot place " + p.string() + ": " + ec.message(); return false; }
    return true;
}

Cert parse_cert(const std::string& pem) {
    Bio b(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return Cert(b ? PEM_read_bio_X509(b.get(), nullptr, nullptr, nullptr) : nullptr);
}

Pkey parse_key(const std::string& pem) {
    Bio b(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return Pkey(b ? PEM_read_bio_PrivateKey(b.get(), nullptr, nullptr, nullptr) : nullptr);
}

std::string spki_of(X509* cert) {
    EVP_PKEY* pub = X509_get0_pubkey(cert);
    if (!pub) return {};
    unsigned char* der = nullptr;
    const int n = i2d_PUBKEY(pub, &der);   // SubjectPublicKeyInfo, DER
    if (n <= 0) return {};
    std::string spki(reinterpret_cast<char*>(der), static_cast<size_t>(n));
    OPENSSL_free(der);
    return crypto::sha256(spki);
}

std::optional<Identity> from_pems(std::string cert_pem, std::string key_pem, std::string& error) {
    Cert c = parse_cert(cert_pem);
    Pkey k = parse_key(key_pem);
    if (!c) { error = "certificate does not parse: " + openssl_error(); return std::nullopt; }
    if (!k) { error = "private key does not parse: " + openssl_error(); return std::nullopt; }
    if (X509_check_private_key(c.get(), k.get()) != 1) { error = "certificate and key do not belong together"; return std::nullopt; }
    Identity id;
    id.spki_sha256 = spki_of(c.get());
    if (id.spki_sha256.empty()) { error = "cannot read the certificate's public key"; return std::nullopt; }
    id.cert_pem = std::move(cert_pem);
    id.key_pem = std::move(key_pem);
    return id;
}

std::optional<Identity> create(const fs::path& dir, std::string& error) {
    Pkey key(EVP_EC_gen("P-256"));
    if (!key) { error = "EC key generation failed: " + openssl_error(); return std::nullopt; }
    Cert cert(X509_new());
    X509_set_version(cert.get(), 2);
    const std::string serial = crypto::random_bytes(16);
    BIGNUM* bn = BN_bin2bn(reinterpret_cast<const unsigned char*>(serial.data()), 16, nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 365 * 10);   // the pin is the identity, not the dates
    X509_set_pubkey(cert.get(), key.get());
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("ariadne-gateway"), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) { error = "self-signing failed: " + openssl_error(); return std::nullopt; }

    Bio cb(BIO_new(BIO_s_mem())), kb(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509(cb.get(), cert.get());
    PEM_write_bio_PrivateKey(kb.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    char* p = nullptr;
    const long cn = BIO_get_mem_data(cb.get(), &p);
    std::string cert_pem(p, static_cast<size_t>(cn));
    const long kn = BIO_get_mem_data(kb.get(), &p);
    std::string key_pem(p, static_cast<size_t>(kn));

    std::error_code ec;
    fs::create_directories(dir, ec);
    ::chmod(dir.c_str(), 0700);
    if (!write_file(dir / "key.pem", key_pem, 0600, error) || !write_file(dir / "cert.pem", cert_pem, 0644, error))
        return std::nullopt;
    return from_pems(std::move(cert_pem), std::move(key_pem), error);
}

}  // namespace

std::string Identity::fp_b64() const { return crypto::b64(spki_sha256); }
std::string Identity::fp_hex() const { return crypto::hex(spki_sha256); }

std::string Identity::cert_der() const {
    Cert c = parse_cert(cert_pem);
    if (!c) return {};
    unsigned char* der = nullptr;
    const int n = i2d_X509(c.get(), &der);
    if (n <= 0) return {};
    std::string out(reinterpret_cast<char*>(der), static_cast<size_t>(n));
    OPENSSL_free(der);
    return out;
}

std::optional<Identity> load(const fs::path& cert, const fs::path& key, std::string& error) {
    std::error_code ec;
    if (!fs::exists(cert, ec) || !fs::exists(key, ec)) { error = "no " + cert.string() + " / " + key.string(); return std::nullopt; }
    return from_pems(slurp(cert), slurp(key), error);
}

std::optional<Identity> load_or_create(const fs::path& dir, std::string& error) {
    std::error_code ec;
    if (fs::exists(dir / "cert.pem", ec) || fs::exists(dir / "key.pem", ec)) return load(dir / "cert.pem", dir / "key.pem", error);
    return create(dir, error);
}

bool adopt(const fs::path& cert, const fs::path& key, const fs::path& dir, std::string& error) {
    std::error_code ec;
    if (fs::exists(dir / "cert.pem", ec) || fs::exists(dir / "key.pem", ec)) {
        error = dir.string() + " already holds an identity; remove it first to adopt another";
        return false;
    }
    auto id = load(cert, key, error);
    if (!id) return false;
    fs::create_directories(dir, ec);
    ::chmod(dir.c_str(), 0700);
    return write_file(dir / "key.pem", id->key_pem, 0600, error) && write_file(dir / "cert.pem", id->cert_pem, 0644, error);
}

void* make_server_ctx(const Identity& id, std::string& error) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { error = openssl_error(); return nullptr; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    Cert c = parse_cert(id.cert_pem);
    Pkey k = parse_key(id.key_pem);
    if (!c || !k || SSL_CTX_use_certificate(ctx, c.get()) != 1 || SSL_CTX_use_PrivateKey(ctx, k.get()) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        error = "TLS identity rejected: " + openssl_error();
        SSL_CTX_free(ctx);
        return nullptr;
    }
    return ctx;
}

void free_server_ctx(void* ctx) { SSL_CTX_free(static_cast<SSL_CTX*>(ctx)); }

std::string sign_sha256(const Identity& id, std::string_view message) {
    Pkey k = parse_key(id.key_pem);
    MdCtx md(EVP_MD_CTX_new());
    if (!k || !md || EVP_DigestSignInit(md.get(), nullptr, EVP_sha256(), nullptr, k.get()) != 1) return {};
    size_t len = 0;
    const auto* m = reinterpret_cast<const unsigned char*>(message.data());
    if (EVP_DigestSign(md.get(), nullptr, &len, m, message.size()) != 1) return {};
    std::string sig(len, '\0');
    if (EVP_DigestSign(md.get(), reinterpret_cast<unsigned char*>(sig.data()), &len, m, message.size()) != 1) return {};
    sig.resize(len);
    return sig;
}

}  // namespace gateway
}  // namespace dd
