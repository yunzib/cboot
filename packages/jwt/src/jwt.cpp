/**
 * @file jwt.cpp
 * @brief JWT 校验实现 — OpenSSL HMAC/RSA + base64url + claims
 *
 * 只实现 resource server 的校验路径：
 *   - compact JWS 三段解析
 *   - HS256 / RS256 签名验证
 *   - exp / nbf / iss / aud 检查
 *
 * OpenSSL 细节只在本单元出现；错误信息不回显 token/密钥。
 */

module;

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

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>

module cboot.jwt;

import std;
import cboot.json;

namespace jwt {

namespace {

/** @brief base64url 字符表（无 padding；JWT 段允许末尾 '='，按填充忽略） */
int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/** @brief base64url → 原始字节；非法字符/长度抛 jwt::error */
std::vector<unsigned char> b64url_decode(std::string_view in) {
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    if (in.size() % 4 == 1)
        throw error("cboot.jwt: invalid base64url length");

    std::vector<unsigned char> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = b64_value(c);
        if (v < 0) throw error("cboot.jwt: invalid base64url character");
        acc = (acc << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xff));
        }
    }
    if (bits > 0 && (acc & ((std::uint32_t{1} << bits) - 1)) != 0)
        throw error("cboot.jwt: non-zero base64url padding bits");
    return out;
}

std::string decode_text(std::string_view in) {
    auto bytes = b64url_decode(in);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw error("cboot.jwt: cannot open public key file");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

std::string string_claim(const json::value& payload, std::string_view key) {
    const json::value* v = payload.find(key);
    if (!v) return {};
    if (!v->is_string())
        throw error("cboot.jwt: claim must be a string");
    return v->as_string();
}

std::vector<std::string> audience_claim(const json::value& payload) {
    const json::value* v = payload.find("aud");
    if (!v) return {};
    std::vector<std::string> out;
    if (v->is_string()) {
        out.push_back(v->as_string());
    } else if (v->is_array()) {
        for (const auto& item : v->as_array()) {
            if (!item.is_string())
                throw error("cboot.jwt: aud array must contain strings");
            out.push_back(item.as_string());
        }
    } else {
        throw error("cboot.jwt: aud must be string or array");
    }
    return out;
}

std::vector<std::string> string_array_claim(const json::value& payload,
                                            std::string_view key) {
    const json::value* v = payload.find(key);
    if (!v) return {};
    std::vector<std::string> out;
    if (v->is_string()) {
        out.push_back(v->as_string());
    } else if (v->is_array()) {
        for (const auto& item : v->as_array()) {
            if (!item.is_string())
                throw error("cboot.jwt: claim array must contain strings");
            out.push_back(item.as_string());
        }
    } else {
        throw error("cboot.jwt: claim must be string or array");
    }
    return out;
}

std::vector<std::string> scope_claim(const json::value& payload,
                                     std::string_view key) {
    const json::value* v = payload.find(key);
    if (!v) return {};
    std::vector<std::string> out;
    if (v->is_string()) {
        std::string_view s = v->as_string();
        while (!s.empty()) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            std::size_t end = s.find_first_of(" \t");
            if (end == std::string_view::npos) end = s.size();
            if (end) out.emplace_back(s.substr(0, end));
            s.remove_prefix(end);
        }
    } else if (v->is_array()) {
        for (const auto& item : v->as_array()) {
            if (!item.is_string())
                throw error("cboot.jwt: scope array must contain strings");
            out.push_back(item.as_string());
        }
    } else {
        throw error("cboot.jwt: scope must be string or array");
    }
    return out;
}

bool contains(const std::vector<std::string>& values, std::string_view want) {
    return std::find(values.begin(), values.end(), want) != values.end();
}

} // namespace

struct JwtVerifier::impl {
    std::string algorithm;
    std::string secret;
    std::string issuer;
    std::string audience;
    std::string roles_claim;
    std::string scope_claim;
    int leeway_s = 30;
    bool require_exp = true;
    EVP_PKEY* public_key = nullptr;

    ~impl() {
        if (public_key) EVP_PKEY_free(public_key);
    }

    void verify_signature(std::string_view signed_part,
                          std::span<const unsigned char> signature) const {
        if (algorithm == "HS256") {
            unsigned char digest[EVP_MAX_MD_SIZE]{};
            unsigned int len = 0;
            if (!HMAC(EVP_sha256(), secret.data(),
                      static_cast<int>(secret.size()),
                      reinterpret_cast<const unsigned char*>(signed_part.data()),
                      signed_part.size(), digest, &len))
                throw error("cboot.jwt: HMAC verification failed");
            if (signature.size() != len
                || CRYPTO_memcmp(signature.data(), digest, len) != 0)
                throw error("cboot.jwt: invalid signature");
            return;
        }

        if (algorithm == "RS256") {
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            if (!ctx) throw error("cboot.jwt: RSA context allocation failed");
            const bool ok =
                EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr,
                                     public_key) == 1
                && EVP_DigestVerifyUpdate(ctx, signed_part.data(),
                                          signed_part.size()) == 1
                && EVP_DigestVerifyFinal(ctx, signature.data(),
                                         signature.size()) == 1;
            EVP_MD_CTX_free(ctx);
            if (!ok) throw error("cboot.jwt: invalid signature");
            return;
        }

        throw error("cboot.jwt: unsupported algorithm");
    }
};

JwtVerifier::JwtVerifier(JwtProps& props) : impl_(std::make_unique<impl>()) {
    impl_->algorithm = props.algorithm;
    if (impl_->algorithm != "HS256" && impl_->algorithm != "RS256")
        throw error("cboot.jwt: algorithm must be HS256 or RS256");

    impl_->issuer = props.issuer;
    impl_->audience = props.audience;
    impl_->roles_claim = props.roles_claim.empty() ? "roles" : props.roles_claim;
    impl_->scope_claim = props.scope_claim.empty() ? "scope" : props.scope_claim;
    impl_->leeway_s = props.leeway_s > 0 ? props.leeway_s : 0;
    impl_->require_exp = props.require_exp;

    if (impl_->algorithm == "HS256") {
        std::string secret = props.secret;
        if (secret.empty() || secret.rfind("${", 0) == 0) {
            const char* env = std::getenv("JWT_SECRET");
            secret = env ? env : "";
        }
        if (secret.empty())
            throw error("cboot.jwt: HS256 secret is empty");
        impl_->secret = std::move(secret);
        return;
    }

    // RS256：优先文件，其次内联 PEM；只接受公钥
    std::string pem = props.public_key;
    if (!props.public_key_path.empty())
        pem = read_file(props.public_key_path);
    if (pem.empty())
        throw error("cboot.jwt: RS256 public key is empty");

    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) throw error("cboot.jwt: cannot allocate key buffer");
    impl_->public_key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!impl_->public_key)
        throw error("cboot.jwt: invalid RSA public key");
}

JwtVerifier::~JwtVerifier() = default;

claims JwtVerifier::verify(std::string_view token) const {
    const std::size_t dot1 = token.find('.');
    const std::size_t dot2 = dot1 == std::string_view::npos
        ? std::string_view::npos : token.find('.', dot1 + 1);
    if (dot1 == std::string_view::npos || dot2 == std::string_view::npos
        || token.find('.', dot2 + 1) != std::string_view::npos)
        throw error("cboot.jwt: malformed token");

    const std::string_view header_b64 = token.substr(0, dot1);
    const std::string_view payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    const std::string_view signature_b64 = token.substr(dot2 + 1);

    json::value header;
    json::value payload;
    try {
        header = json::parse(decode_text(header_b64));
        payload = json::parse(decode_text(payload_b64));
    } catch (const std::exception& e) {
        throw error("cboot.jwt: invalid JSON segment");
    }
    if (!header.is_object() || !payload.is_object())
        throw error("cboot.jwt: header/payload must be objects");

    const json::value* alg = header.find("alg");
    if (!alg || !alg->is_string() || alg->as_string() != impl_->algorithm)
        throw error("cboot.jwt: algorithm mismatch");
    if (header.find("crit"))
        throw error("cboot.jwt: crit header is not supported");

    const auto signature = b64url_decode(signature_b64);
    impl_->verify_signature(token.substr(0, dot2), signature);

    const std::int64_t now = now_seconds();
    const json::value* exp_v = payload.find("exp");
    if (!exp_v) {
        if (impl_->require_exp)
            throw error("cboot.jwt: exp claim is required");
    } else {
        if (!exp_v->is_int())
            throw error("cboot.jwt: exp must be integer");
        const std::int64_t exp = exp_v->as_int();
        if (now > exp + impl_->leeway_s)
            throw error("cboot.jwt: token expired");
    }

    if (const json::value* nbf_v = payload.find("nbf")) {
        if (!nbf_v->is_int())
            throw error("cboot.jwt: nbf must be integer");
        if (now + impl_->leeway_s < nbf_v->as_int())
            throw error("cboot.jwt: token not yet valid");
    }

    claims out;
    out.payload = payload;
    out.subject = string_claim(payload, "sub");
    out.issuer = string_claim(payload, "iss");
    out.audience = audience_claim(payload);
    out.roles = string_array_claim(payload, impl_->roles_claim);
    out.scopes = scope_claim(payload, impl_->scope_claim);
    if (exp_v) out.expires_at = exp_v->as_int();

    if (!impl_->issuer.empty() && out.issuer != impl_->issuer)
        throw error("cboot.jwt: issuer mismatch");
    if (!impl_->audience.empty() && !contains(out.audience, impl_->audience))
        throw error("cboot.jwt: audience mismatch");

    return out;
}

} // namespace jwt
