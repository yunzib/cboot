/**
 * @file log_smoke.cpp
 * @brief 日志门面/配置冒烟测试
 *
 * 覆盖：级别过滤、命名 logger 覆盖、text/JSON、MDC、file/rolling sink、
 * LogProps/LogSetup 装配。
 */
import cboot.log;
import cboot.log.component;
import std;

namespace {

class memory_sink final : public logging::sink {
public:
    void write(logging::level lv, std::string_view line) override {
        lines.emplace_back(lv, std::string(line));
    }

    std::vector<std::pair<logging::level, std::string>> lines;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    // ── 级别过滤 ───────────────────────────────────────────────────────
    auto memory = std::make_shared<memory_sink>();
    logging::clear_sinks();
    logging::set_level(logging::level::info);
    logging::set_layout(std::make_shared<logging::text_layout>());
    logging::add_sink(memory);

    auto app = logging::get("app");
    app.debug("debug hidden");
    app.info("hello {}", 42);
    app.warn("warn visible");

    if (memory->lines.size() != 2) return 1;
    if (memory->lines[0].first != logging::level::info) return 2;
    if (!contains(memory->lines[0].second, "hello 42")) return 3;
    if (!contains(memory->lines[0].second, "INFO")) return 4;
    if (memory->lines[1].first != logging::level::warn) return 5;

    // ── 命名 logger 前缀覆盖 ───────────────────────────────────────────
    memory->lines.clear();
    logging::set_logger_level("app.db", logging::level::debug);
    logging::get("app.db").debug("db debug");
    logging::get("app.web").debug("web hidden");
    if (memory->lines.size() != 1) return 6;
    if (!contains(memory->lines[0].second, "db debug")) return 7;

    // ── JSON + MDC ─────────────────────────────────────────────────────
    memory->lines.clear();
    logging::set_layout(std::make_shared<logging::json_layout>());
    {
        logging::context::scope ctx("request_id", "req-123");
        logging::get("app").info("json {}", "line");
    }
    if (memory->lines.size() != 1) return 8;
    if (!contains(memory->lines[0].second, "\"level\":\"info\"")) return 9;
    if (!contains(memory->lines[0].second, "\"request_id\":\"req-123\""))
        return 10;

    // ── file sink ──────────────────────────────────────────────────────
    const auto temp = std::filesystem::temp_directory_path()
        / "cboot_log_smoke";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);

    {
        logging::clear_sinks();
        logging::set_level(logging::level::debug);
        logging::set_layout(std::make_shared<logging::text_layout>());
        logging::add_sink(std::make_shared<logging::file_sink>(
            (temp / "app.log").string()));
        logging::get("file").debug("file line");
        logging::flush();

        const auto content = read_file(temp / "app.log");
        if (!contains(content, "file line")) return 11;
    }

    // ── rolling file sink ──────────────────────────────────────────────
    {
        logging::clear_sinks();
        logging::set_level(logging::level::debug);
        logging::set_layout(std::make_shared<logging::text_layout>("%msg%n"));
        logging::add_sink(std::make_shared<logging::rolling_file_sink>(
            (temp / "roll.log").string(), 128, 3));

        for (int i = 0; i < 30; ++i)
            logging::get("roll").info("rolling line number {}",
                                  std::to_string(i));
        logging::flush();

        if (!std::filesystem::exists(temp / "roll.log")) return 12;
        if (!std::filesystem::exists(temp / "roll.log.1")) return 13;
    }

    // ── LogProps / LogSetup ────────────────────────────────────────────
    {
        logging::clear_sinks();
        logging::LogProps props;
        props.level = "debug";
        props.format = "json";
        props.output = "file";
        props.file = (temp / "configured.log").string();
        props.max_size_mb = 1;
        props.max_files = 2;
        props.file_level = "debug";
        props.logger_levels = {"cfg=debug"};

        logging::LogSetup setup(props);
        setup.configure();

        logging::get("cfg").debug("configured {}", "ok");
        setup.shutdown();

        const auto content = read_file(temp / "configured.log");
        if (!contains(content, "\"msg\":\"configured ok\"")) return 14;
    }

    logging::clear_sinks();
    std::filesystem::remove_all(temp, ec);

    std::println("LOG SMOKE: PASS");
    return 0;
}
