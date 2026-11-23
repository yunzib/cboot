/**
 * @file postgres.cpp
 * @brief PostgreSQL TokenStore 实现
 */
module cboot.auth.postgres;

import std;
import cboot.auth;
import cboot.json;
import cboot.orm;

namespace auth {

namespace {

/** @brief auth_sessions 表实体（内部） */
struct [[=orm::Table{.name = "cboot_auth_sessions"}]] token_row {
    [[=orm::Id{}]] std::string token;
    std::string subject;
    std::string roles_json;
    std::string scopes_json;
    std::int64_t issued_at{};
    std::int64_t expires_at{};
    std::string device;
};

static_assert(!orm::entity_meta_v<token_row>.id_generated,
              "token_row must not use a generated id");

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string dump_strings(const std::vector<std::string>& values) {
    return json::dump(json::from(values));
}

std::vector<std::string> load_strings(std::string_view text) {
    return json::get_as<std::vector<std::string>>(json::parse(text));
}

} // namespace

struct PgTokenStore::impl {
    orm::EntityManager* db = nullptr;
};

PgTokenStore::PgTokenStore(orm::EntityManager& db)
    : impl_(std::make_unique<impl>()) {
    impl_->db = &db;
}

PgTokenStore::~PgTokenStore() = default;

void PgTokenStore::ensure_schema() {
    impl_->db->create_schema<token_row>();

    const auto& d = impl_->db->dialect();
    const std::string table = d.quote_identifier("cboot_auth_sessions");
    const std::string index = d.quote_identifier("cboot_auth_sessions_subject_idx");
    const std::string subject = d.quote_identifier("subject");
    // 旧表升级：create_schema 不会 ALTER，补 sliding 续期所需列。
    const std::string issued = d.quote_identifier("issued_at");
    impl_->db->execute(
        "ALTER TABLE " + table + " ADD COLUMN IF NOT EXISTS " + issued
            + " BIGINT NOT NULL DEFAULT 0",
        {});
    impl_->db->execute(
        "CREATE INDEX IF NOT EXISTS " + index + " ON " + table
            + " (" + subject + ")",
        {});
}

std::string PgTokenStore::create(session s) {
    if (s.issued_at <= 0) s.issued_at = now_seconds();
    if (s.expires_at <= 0)
        s.expires_at = s.issued_at + 7200;

    token_row row{
        .token = new_token(),
        .subject = std::move(s.subject),
        .roles_json = dump_strings(s.roles),
        .scopes_json = dump_strings(s.scopes),
        .issued_at = s.issued_at,
        .expires_at = s.expires_at,
        .device = std::move(s.device),
    };
    impl_->db->insert(row);
    return row.token;
}

std::optional<session> PgTokenStore::get(std::string_view token) const {
    auto row = impl_->db->find<token_row>(std::string(token));
    if (!row) return std::nullopt;

    if (row->expires_at > 0 && now_seconds() > row->expires_at) {
        impl_->db->remove<token_row>(std::string(token));
        return std::nullopt;
    }

    try {
        session s;
        s.subject = std::move(row->subject);
        s.roles = load_strings(row->roles_json);
        s.scopes = load_strings(row->scopes_json);
        s.issued_at = row->issued_at;
        s.expires_at = row->expires_at;
        s.device = std::move(row->device);
        return s;
    } catch (const std::exception&) {
        // 持久化行损坏时视为无效 token，不泄露内部细节
        return std::nullopt;
    }
}

void PgTokenStore::revoke(std::string_view token) {
    impl_->db->remove<token_row>(std::string(token));
}

void PgTokenStore::revoke_subject(std::string_view subject) {
    const auto& d = impl_->db->dialect();
    const std::string table = d.quote_identifier("cboot_auth_sessions");
    const std::string column = d.quote_identifier("subject");
    impl_->db->execute(
        "DELETE FROM " + table + " WHERE " + column + " = " + d.placeholder(1),
        {std::string(subject)});
}

bool PgTokenStore::touch(std::string_view token, std::int64_t expires_at) {
    const auto& d = impl_->db->dialect();
    const std::string table = d.quote_identifier("cboot_auth_sessions");
    const std::string expires = d.quote_identifier("expires_at");
    const std::string column = d.quote_identifier("token");
    return impl_->db->execute(
               "UPDATE " + table + " SET " + expires + " = " + d.placeholder(1)
                   + " WHERE " + column + " = " + d.placeholder(2)
                   + " AND " + expires + " > " + std::to_string(now_seconds()),
               {expires_at, std::string(token)}) > 0;
}

} // namespace auth
