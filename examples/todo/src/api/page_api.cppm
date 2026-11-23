/**
 * @file page_api.cppm
 * @brief 前端页面路由 — 返回示例目录下的 web/index.html
 */
export module todo.api.page;

import std;
import cboot.core.annotations;  // Bean
import cboot.web.http;          // Get / Response

/**
 * @brief 返回单文件 HTML
 *
 * 框架不提供静态文件服务；示例在启动时读入 HTML，通过普通路由返回，
 * 保证本地运行不依赖 nginx。启动目录应为 examples/todo。
 */
export class [[=Bean{}]] PageApi {
public:
    PageApi() {
        for (const auto* path : {"web/index.html", "examples/todo/web/index.html"}) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            html_ = std::string(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
            break;
        }
        if (html_.empty())
            html_ = "<!doctype html><h1>web/index.html not found</h1>";
    }

    [[=Get{"/"}]]
    Response index() const { return page(); }

    [[=Get{"/index.html"}]]
    Response index_html() const { return page(); }

private:
    Response page() const {
        Response r;
        r.content_type = "text/html; charset=utf-8";
        r.body = html_;
        return r;
    }

    std::string html_;
};
