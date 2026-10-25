#include "Config.h"
#include <fstream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include "Dns.h"

namespace mailadmin {
Config read_config(const std::filesystem::path& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) || !S_ISREG(info.st_mode) || (info.st_mode & 0027)) {
        throw std::runtime_error{"Config must be a regular file with mode 0600 or 0640"};
    }
    if (info.st_size > 65536) throw std::runtime_error{"Config exceeds size limit"};
    std::ifstream file{path};
    if (!file) throw std::runtime_error{"Cannot read config"};
    std::string content{std::istreambuf_iterator<char>{file}, {}};
    boost::system::error_code error;
    auto value = boost::json::parse(content, error);
    if (error || !value.is_object()) throw std::runtime_error{"Invalid JSON configuration"};
    const auto& object = value.as_object();
    const std::set<std::string_view> names{
        "endpoint", "port", "tls_key", "tls_cert", "key_file", "database_socket",
        "database_name", "database_user", "database_password", "log_level",
        "database_timeout_seconds", "database_workers", "max_pending_requests", "administration",
        "spool_root", "generated_directory", "reconcile_seconds", "spam_cleanup_seconds",
        "nsblast_api_url", "nsblast_api_key_file", "dns_default_configuration", "nsblast_mta_sts_https_ready", "default_mail_hostname"};
    for (const auto& member : object) {
        if (!names.contains(member.key())) throw std::runtime_error{"Unknown config field"};
    }
    auto string_field = [&](std::string_view name, std::string fallback = {}) {
        if (const auto* field = object.if_contains(name)) {
            if (!field->is_string()) throw std::runtime_error{"Config string has wrong type"};
            std::string result{field->as_string()};
            if (result.find('\0') != std::string::npos) throw std::runtime_error{"NUL in config string"};
            return result;
        }
        return fallback;
    };
    auto number_field = [&](std::string_view name, unsigned fallback, unsigned maximum) {
        if (const auto* field = object.if_contains(name)) {
            if (!field->is_int64() || field->as_int64() < 1 || field->as_int64() > maximum)
                throw std::runtime_error{"Config number out of range"};
            return static_cast<unsigned>(field->as_int64());
        }
        return fallback;
    };
    Config config;
    config.http.http_endpoint = string_field("endpoint", "127.0.0.1");
    config.http.http_port = string_field("port", "8080");
    config.http.http_tls_key = string_field("tls_key");
    config.http.http_tls_cert = string_field("tls_cert");
    if (config.http.http_tls_key.empty() != config.http.http_tls_cert.empty())
        throw std::runtime_error{"Both TLS key and certificate are required"};
    if (config.http.http_endpoint != "127.0.0.1" && config.http.http_endpoint != "::1"
        && config.http.http_tls_cert.empty())
        throw std::runtime_error{"Non-loopback listeners require TLS"};
    config.http.enable_http_basic_auth = false;
    config.http.auto_handle_cors = false;
    config.http.num_http_threads = 2;
    config.http.max_request_body_size = 16384;
    config.http.max_request_header_size = 8192;
    config.http.max_decompressed_size = 16384;
    config.http.http_io_timeout = 15;
    // Our protected metrics route exports exactly the two domain gauges.
    config.http.enable_metrics = false;
    config.key_file = string_field("key_file");
    config.database_socket = string_field("database_socket", config.database_socket);
    config.database_name = string_field("database_name", config.database_name);
    config.database_user = string_field("database_user", config.database_user);
    config.database_password = string_field("database_password");
    config.log_level = string_field("log_level", config.log_level);
    config.database_timeout_seconds = number_field("database_timeout_seconds", 5, 30);
    config.database_workers = number_field("database_workers", 2, 8);
    config.max_pending_requests = number_field("max_pending_requests", 16, 128);
    if (const auto* field = object.if_contains("administration")) {
        if (!field->is_bool()) throw std::runtime_error{"Administration must be boolean"};
        config.administration = field->as_bool();
    }
    config.spool_root = string_field("spool_root", config.spool_root.string());
    config.generated_directory = string_field("generated_directory", config.generated_directory.string());
    config.reconcile_seconds = number_field("reconcile_seconds", 5, 300);
    config.spam_cleanup_seconds = number_field("spam_cleanup_seconds", 3600, 86400);
    config.nsblast_api_url = string_field("nsblast_api_url");
    config.default_mail_hostname = string_field("default_mail_hostname");
    config.nsblast_api_key_file = string_field("nsblast_api_key_file");
    if (const auto* ready = object.if_contains("nsblast_mta_sts_https_ready")) {
        if (!ready->is_bool()) throw std::runtime_error{"MTA-STS readiness must be boolean"};
        config.nsblast_mta_sts_https_ready = ready->as_bool();
    }
    if (const auto* value = object.if_contains("dns_default_configuration")) {
        if (!value->is_object()) throw std::runtime_error{"DNS defaults must be a JSON object"};
        config.dns_default_configuration = value->as_object();
    }
    if (!config.default_mail_hostname.empty()) {
        config.default_mail_hostname = domain_name(config.default_mail_hostname);
        if (!config.nsblast_api_url.empty() && !config.dns_default_configuration.contains("mail_servers")) {
            auto defaults = dns_defaults(config);
            for (auto& member : config.dns_default_configuration) defaults[member.key()] = member.value();
            config.dns_default_configuration = std::move(defaults);
        }
    }
    validate_dns_config(config);
    for (const auto& directory : {config.spool_root, config.generated_directory}) {
        if (!directory.is_absolute() || directory == "/" || directory.lexically_normal() != directory)
            throw std::runtime_error{"Administrative directories must be normalized absolute paths"};
        for (const char character : directory.string())
            if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '/' || character == '-' || character == '_'))
                throw std::runtime_error{"Unsupported administrative directory character"};
    }
    if (!config.key_file.is_absolute() || !std::filesystem::path{config.database_socket}.is_absolute()
        || config.database_name.empty() || config.database_user.empty())
        throw std::runtime_error{"Absolute key/socket paths and database identity are required"};
    if (config.log_level != "info" && config.log_level != "debug" && config.log_level != "trace")
        throw std::runtime_error{"Unknown log level"};
    return config;
}
}
