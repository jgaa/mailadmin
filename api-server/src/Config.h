#pragma once
#include <filesystem>
#include <string>
#include <yahat/HttpServer.h>

namespace mailadmin {
struct Config {
    yahat::HttpConfig http;
    std::filesystem::path key_file;
    std::string database_socket = "/run/mysqld/mysqld.sock";
    std::string database_name = "mail";
    std::string database_user = "mailadmin";
    std::string database_password;
    std::string log_level = "info";
    unsigned database_timeout_seconds = 5;
    unsigned database_workers = 2;
    unsigned max_pending_requests = 16;
    bool administration = false;
    std::filesystem::path spool_root = "/var/lib/courier/spool-virtusers";
    std::filesystem::path generated_directory = "/etc/mailadmin/generated";
    unsigned reconcile_seconds = 5;
    unsigned spam_cleanup_seconds = 3600;
    std::string nsblast_api_url;
    std::string default_mail_hostname;
    std::filesystem::path nsblast_api_key_file;
    boost::json::object dns_default_configuration;
    bool nsblast_mta_sts_https_ready = false;
};
Config read_config(const std::filesystem::path& path);
}
