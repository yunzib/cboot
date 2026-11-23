/**
 * @file jwt_smoke.cpp
 * @brief JWT 校验冒烟测试
 *
 * 覆盖：
 *   - HS256 签发（测试侧）/ 校验、claims 读取
 *   - RS256 公钥加载与验签
 *   - 过期、issuer/audience、algorithm 混淆、非法签名
 */
// GCC16 模块边界守卫：OpenSSL 头会拉入 stdio/stdlib 等 C 头，
// 与 import std 的同名声明产生语言链接冲突；与 web/server.cppm 同款。
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 1
#define _MM_MALLOC_H_INCLUDED
#define _X86INTRIN_H_INCLUDED
#define _IMMINTRIN_H_INCLUDED
#define _XMMINTRIN_H_INCLUDED
#define _EMMINTRIN_H_INCLUDED
#define _MMINTRIN_H_INCLUDED
#define _PMMINTRIN_H_INCLUDED
#define _CPUID_H_INCLUDED

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

import cboot.jwt;
import cboot.json;
import std;

namespace {

constexpr std::string_view b64_table =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(std::span<const unsigned char> in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const std::uint32_t v = (std::uint32_t(in[i]) << 16)
            | (std::uint32_t(in[i + 1]) << 8) | in[i + 2];
        out.push_back(b64_table[(v >> 18) & 63]);
        out.push_back(b64_table[(v >> 12) & 63]);
        out.push_back(b64_table[(v >> 6) & 63]);
        out.push_back(b64_table[v & 63]);
        i += 3;
    }
    if (i < in.size()) {
        const std::uint32_t v = (std::uint32_t(in[i]) << 16)
            | (i + 1 < in.size() ? (std::uint32_t(in[i + 1]) << 8) : 0);
        out.push_back(b64_table[(v >> 18) & 63]);
        out.push_back(b64_table[(v >> 12) & 63]);
        if (i + 1 < in.size()) out.push_back(b64_table[(v >> 6) & 63]);
    }
    return out;
}

std::string b64url(std::string_view s) {
    return b64url(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(s.data()), s.size()));
}

std::string hs256_token(const json::value& header, const json::value& payload,
                        std::string_view secret) {
    const std::string signing = b64url(json::dump(header)) + "."
        + b64url(json::dump(payload));
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), secret.data(), int(secret.size()),
         reinterpret_cast<const unsigned char*>(signing.data()),
         signing.size(), digest, &len);
    return signing + "." + b64url(std::span<const unsigned char>(digest, len));
}

json::value header_hs256() {
    json::object h;
    h.emplace_back("alg", json::value("HS256"));
    h.emplace_back("typ", json::value("JWT"));
    return json::value(std::move(h));
}

json::value payload(std::int64_t exp,
                    std::string_view iss = "issuer",
                    std::string_view aud = "api") {
    json::object p;
    p.emplace_back("sub", json::value("user-42"));
    p.emplace_back("iss", json::value(iss));
    p.emplace_back("aud", json::value(aud));
    p.emplace_back("exp", json::value(exp));
    json::array roles;
    roles.emplace_back(json::value("admin"));
    p.emplace_back("roles", json::value(std::move(roles)));
    p.emplace_back("scope", json::value("read write"));
    return json::value(std::move(p));
}

jwt::JwtProps hs_props() {
    jwt::JwtProps p;
    p.algorithm = "HS256";
    p.secret = "test-secret";
    p.issuer = "issuer";
    p.audience = "api";
    p.leeway_s = 0;
    return p;
}

} // namespace

int main() {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto props = hs_props();
    jwt::JwtVerifier verifier(props);

    // ── HS256 合法 token ────────────────────────────────────────────────
    auto token = hs256_token(header_hs256(), payload(now + 3600),
                             props.secret);
    auto claims = verifier.verify(token);
    if (claims.subject != "user-42" || claims.issuer != "issuer") return 1;
    if (claims.roles.size() != 1 || claims.roles[0] != "admin") return 2;
    if (claims.scopes.size() != 2 || claims.scopes[1] != "write") return 3;
    if (claims.audience.size() != 1 || claims.audience[0] != "api") return 4;

    // ── 过期 / issuer / audience / 签名 ────────────────────────────────
    auto expired = hs256_token(header_hs256(), payload(now - 10), props.secret);
    try { (void)verifier.verify(expired); return 5; } catch (const jwt::error&) {}

    auto wrong_iss = hs256_token(header_hs256(),
                                 payload(now + 3600, "other"), props.secret);
    try { (void)verifier.verify(wrong_iss); return 6; } catch (const jwt::error&) {}

    auto wrong_aud = hs256_token(header_hs256(),
                                 payload(now + 3600, "issuer", "other"),
                                 props.secret);
    try { (void)verifier.verify(wrong_aud); return 7; } catch (const jwt::error&) {}

    auto bad_sig = token;
    bad_sig.back() = bad_sig.back() == 'A' ? 'B' : 'A';
    try { (void)verifier.verify(bad_sig); return 8; } catch (const jwt::error&) {}

    // ── algorithm 混淆：header 声称 none / HS512 ───────────────────────
    json::object none_h;
    none_h.emplace_back("alg", json::value("none"));
    auto none_token = hs256_token(json::value(std::move(none_h)),
                                  payload(now + 3600), props.secret);
    try { (void)verifier.verify(none_token); return 9; } catch (const jwt::error&) {}

    // ── RS256：测试侧生成密钥并签发 ───────────────────────────────────
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kctx) return 10;
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen_init(kctx) != 1
        || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) != 1
        || EVP_PKEY_keygen(kctx, &key) != 1) {
        EVP_PKEY_CTX_free(kctx);
        return 11;
    }
    EVP_PKEY_CTX_free(kctx);

    BIO* pub = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(pub, key);
    char* pem_data = nullptr;
    const long pem_len = BIO_get_mem_data(pub, &pem_data);
    std::string public_pem(pem_data, std::size_t(pem_len));
    BIO_free(pub);

    json::object rs_h;
    rs_h.emplace_back("alg", json::value("RS256"));
    const std::string signing = b64url(json::dump(json::value(std::move(rs_h))))
        + "." + b64url(json::dump(payload(now + 3600)));
    EVP_MD_CTX* sctx = EVP_MD_CTX_new();
    std::size_t sig_len = 0;
    if (!sctx
        || EVP_DigestSignInit(sctx, nullptr, EVP_sha256(), nullptr, key) != 1
        || EVP_DigestSignUpdate(sctx, signing.data(), signing.size()) != 1
        || EVP_DigestSignFinal(sctx, nullptr, &sig_len) != 1) {
        if (sctx) EVP_MD_CTX_free(sctx);
        EVP_PKEY_free(key);
        return 12;
    }
    std::vector<unsigned char> sig(sig_len);
    if (EVP_DigestSignFinal(sctx, sig.data(), &sig_len) != 1) {
        EVP_MD_CTX_free(sctx);
        EVP_PKEY_free(key);
        return 13;
    }
    EVP_MD_CTX_free(sctx);
    EVP_PKEY_free(key);

    jwt::JwtProps rs_props;
    rs_props.algorithm = "RS256";
    rs_props.public_key = public_pem;
    rs_props.issuer = "issuer";
    rs_props.audience = "api";
    rs_props.leeway_s = 0;
    jwt::JwtVerifier rs_verifier(rs_props);
    auto rs_token = signing + "." + b64url(std::span<const unsigned char>(
        sig.data(), sig_len));
    auto rs_claims = rs_verifier.verify(rs_token);
    if (rs_claims.subject != "user-42") return 14;

    std::println("JWT SMOKE: PASS");
    return 0;
}
