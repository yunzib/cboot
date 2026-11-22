// cboot.web.routing 实现单元 — 路由匹配与调度（含 URL 解码 / 405）
module cboot.web.routing;

import std;
import cboot.web.http;

namespace {

// 路径切段（'/' 分隔，忽略空段）+ 逐段百分号解码
// 零分配快路径：路径无 '%' 时全部为指向原串的视图；含 '%' 时解码进 scratch
// （先 reserve，视图指向 scratch 不会因扩容失效）
struct segments {
    std::array<std::string_view, 16> items{};
    std::size_t count = 0;
};

segments split_view(std::string_view path, std::string& scratch) {
    segments out;
    const bool need_decode = path.find('%') != std::string_view::npos;
    if (need_decode) scratch.reserve(scratch.size() + path.size() + 1);
    std::size_t i = 0;
    while (i < path.size() && out.count < out.items.size()) {
        std::size_t slash = path.find('/', i);
        if (slash == std::string_view::npos) slash = path.size();
        std::string_view seg = path.substr(i, slash - i);
        if (!seg.empty()) {
            if (!need_decode) {
                out.items[out.count++] = seg;
            } else {
                const std::size_t before = scratch.size();
                url_decode_append(scratch, seg);
                out.items[out.count++] =
                    std::string_view(scratch.data() + before, scratch.size() - before);
            }
        }
        i = slash + 1;
    }
    return out;
}

std::string join_segments(const segments& segs) {
    std::string out;
    for (std::size_t i = 0; i < segs.count; ++i) {
        out.push_back('/');
        out.append(segs.items[i]);
    }
    return out;
}

} // namespace

void router::add_route(http_method method, std::string_view pattern,
                       detail::handler_fn h) {
    route_item item{method, {}, std::move(h)};
    {
        std::string scratch;
        const segments segs = split_view(pattern, scratch);
        for (std::size_t i = 0; i < segs.count; ++i)
            item.segs.emplace_back(segs.items[i]);
    }
    routes_.push_back(std::move(item));
}

/** @brief 路径匹配（忽略方法）；命中时填充参数段视图 */
static bool path_matches(const std::vector<std::string>& pattern,
                         const segments& target,
                         std::vector<std::string_view>& pp) {
    if (pattern.size() != target.count) return false;
    std::vector<std::string_view> tmp;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (!pattern[i].empty() && pattern[i][0] == '{') {
            tmp.push_back(target.items[i]);        // 参数段：按位收集
        } else if (pattern[i] != target.items[i]) {
            return false;
        }
    }
    pp = std::move(tmp);
    return true;
}

Response router::dispatch(std::string_view method, std::string_view target,
                          std::string_view body) const {
    // 方法解析（未知方法 → 405 并给 Allow）
    const std::optional<http_method> m = method_from_name(method);
    std::string_view path = target;
    std::string_view query;
    if (std::size_t q = target.find('?'); q != std::string_view::npos) {
        path = target.substr(0, q);
        query = target.substr(q + 1);
    }
    std::string scratch;
    const segments segs = split_view(path, scratch);
    // %-编码时路径解码后的存储（视图指向它；仅本次 dispatch 有效）
    std::string decoded_path;
    const std::string_view req_path = scratch.empty() ? path : std::string_view{};
    if (!scratch.empty()) decoded_path = join_segments(segs);
    std::vector<std::string_view> pp;

    if (m) {
        for (const auto& r : routes_) {
            if (r.method != *m) continue;
            if (path_matches(r.segs, segs, pp)) {
                Request req{.method = method,
                            .path = scratch.empty() ? req_path
                                                    : std::string_view(decoded_path),
                            .query = query,
                            .body = body};
                // handler 异常统一收敛为 500（dispatch 自身不抛）
                try {
                    return r.h(req, pp);
                } catch (const std::exception& e) {
                    Response err = Response::text(std::string("500 ") + e.what());
                    err.status = 500;
                    return err;
                } catch (...) {
                    Response err = Response::text("500 internal error");
                    err.status = 500;
                    return err;
                }
            }
        }
    }

    // 路径命中但方法不匹配 → 405 + Allow（未知方法同样走此路径）
    std::string allow;
    for (const auto& r : routes_) {
        std::vector<std::string_view> ignored;
        if (!path_matches(r.segs, segs, ignored)) continue;
        std::string_view name = method_name(r.method);
        if (allow.find(name) != std::string::npos) continue;
        if (!allow.empty()) allow.append(", ");
        allow.append(name);
    }
    if (!allow.empty()) {
        Response r = Response::text("405 method not allowed");
        r.status = 405;
        r.header("Allow", std::move(allow));
        return r;
    }

    Response not_found = Response::text("404 not found");
    not_found.status = 404;
    return not_found;
}
