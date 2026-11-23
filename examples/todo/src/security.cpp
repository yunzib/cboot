/**
 * @file security.cpp
 * @brief PBKDF2-HMAC-SHA256 实现（OpenSSL）
 */
module;

// GCC16 模块边界守卫：OpenSSL 头会拉入 stdio/stdlib 等 C 头，
// 与 import std 的同名声明产生语言链接冲突。
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 1
#define _MM_MALLOC_H_INCLUDED
#define _X86INTRIN_H_INCLUDED
#define _IMMINTRIN_H_INCLUDED
#define _XMMINTRIN_H_INCLUDED
#define _EMMINTRIN_H_INCLUDED
#define _MM_INTRIN_H_INCLUDED
#define _PMMINTRIN_H_INCLUDED
#define _CPUID_H_INCLUDED

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

module todo.security;

import std;

namespace todo::security {

namespace {

constexpr int iterations = 120'000;
constexpr std::size_t salt_bytes = 16;
constexpr std::size_t hash_bytes = 32;

std::string to_hex(std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
}

std::optional<std::vector<unsigned char>> from_hex(std::string_view text) {
    if (text.empty() || text.size() % 2 != 0) return std::nullopt;

    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::vector<unsigned char> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(text[i * 2]);
        const int lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return out;
}

std::optional<std::vector<unsigned char>> derive(std::string_view password,
                                                 std::span<const unsigned char> salt,
                                                 int rounds) {
    std::vector<unsigned char> out(hash_bytes);
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          rounds, EVP_sha256(), static_cast<int>(out.size()),
                          out.data()) != 1)
        return std::nullopt;
    return out;
}

} // namespace

std::string hash_password(std::string_view password) {
    std::array<unsigned char, salt_bytes> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
        throw std::runtime_error("todo.security: random salt failed");

    auto hash = derive(password, salt, iterations);
    if (!hash)
        throw std::runtime_error("todo.security: password hash failed");

    return "pbkdf2-sha256$" + std::to_string(iterations) + "$"
         + to_hex(salt) + "$" + to_hex(*hash);
}

bool verify_password(std::string_view password, std::string_view encoded) {
    constexpr std::string_view prefix = "pbkdf2-sha256$";
    if (encoded.substr(0, prefix.size()) != prefix) return false;
    encoded.remove_prefix(prefix.size());

    const std::size_t first = encoded.find('$');
    if (first == std::string_view::npos) return false;
    const std::size_t second = encoded.find('$', first + 1);
    if (second == std::string_view::npos) return false;

    int rounds = 0;
    const auto round_text = encoded.substr(0, first);
    const auto [ptr, ec] = std::from_chars(
        round_text.data(), round_text.data() + round_text.size(), rounds);
    if (ec != std::errc{} || ptr != round_text.data() + round_text.size()
        || rounds <= 0 || rounds > 1'000'000)
        return false;

    auto salt = from_hex(encoded.substr(first + 1, second - first - 1));
    auto expected = from_hex(encoded.substr(second + 1));
    if (!salt || !expected || salt->empty() || expected->size() != hash_bytes)
        return false;

    auto actual = derive(password, *salt, rounds);
    if (!actual) return false;

    return CRYPTO_memcmp(actual->data(), expected->data(), expected->size()) == 0;
}

} // namespace todo::security
