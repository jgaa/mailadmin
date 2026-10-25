#include "Api.h"
#include "Security.h"
#include "logging.h"
#include "mailadmin/version.h"
#include <boost/program_options.hpp>
#include <mysql.h>
#include <openssl/crypto.h>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
namespace po = boost::program_options;

enum class Command { help, version, serve, key_create, key_rotate, secret_create, schema_migrate, passwords_migrate };

struct Arguments {
    Command command = Command::help;
    std::string config;
    std::string key_file;
    std::string log_level;
};

Arguments parse_arguments(int argc, char** argv) {
    po::options_description general{"General options"};
    general.add_options()
        ("help,h", "Print help and exit")
        ("version,v", "Print version and exit");
    po::options_description server{"Server options"};
    server.add_options()
        ("config,c", po::value<std::string>(), "Configuration file (required)")
        ("log-level,l", po::value<std::string>(), "Override configured log level: info, debug or trace");
    po::options_description migration{"Migration options"};
    migration.add_options()
        ("confirm-hash-compatible", "Confirm hash-only SMTP/IMAP authentication was tested before removing plaintext");
    po::options_description key{"API key options"};
    key.add_options()
        ("key-file", po::value<std::string>(), "Absolute verifier file path (required)");
    po::options_description positional_options;
    positional_options.add_options()
        ("command", po::value<std::string>(), "Command")
        ("action", po::value<std::string>(), "Action");
    po::positional_options_description positional;
    positional.add("command", 1).add("action", 1);

    // Discover the command, then parse against only its supported options.
    // No arguments are logged: future commands may accept sensitive values.
    po::options_description all;
    all.add(general).add(server).add(key).add(migration).add(positional_options);
    po::variables_map values;
    po::store(po::command_line_parser(argc, argv).options(all).positional(positional)
                  .style(po::command_line_style::unix_style & ~po::command_line_style::allow_guessing)
                  .run(), values);
    const auto command = values.count("command") ? values["command"].as<std::string>() : std::string{};
    const auto action = values.count("action") ? values["action"].as<std::string>() : std::string{};
    if ((!command.empty() && command != "serve" && command != "key" && command != "secret" && command != "schema" && command != "passwords")
        || ((command.empty() || command == "serve") && !action.empty())
        || (command == "key" && !action.empty() && action != "create" && action != "rotate")
        || (command == "secret" && !action.empty() && action != "create")
        || ((command == "schema" || command == "passwords") && !action.empty() && action != "migrate")) {
        throw po::error{"Invalid command"};
    }

    po::options_description visible;
    visible.add(general);
    if (command == "serve" || command == "schema" || command == "passwords") visible.add(server);
    if (command == "passwords") visible.add(migration);
    if (command == "key") visible.add(key);
    po::options_description selected;
    selected.add(visible).add(positional_options);
    values.clear();
    po::store(po::command_line_parser(argc, argv).options(selected).positional(positional)
                  .style(po::command_line_style::unix_style & ~po::command_line_style::allow_guessing)
                  .run(), values);
    po::notify(values);

    Arguments result;
    if (values.count("help") || argc == 1) {
        std::cout << "Usage: mailadmin serve [options]\n"
                     "       mailadmin key create|rotate [options]\n"
                     "       mailadmin secret create [options]\n"
                     "       mailadmin schema migrate --config PATH\n"
                     "       mailadmin passwords migrate --config PATH --confirm-hash-compatible\n\n";
        if (command.empty()) visible.add(server).add(key).add(migration);
        std::cout << visible << '\n';
        return result;
    }
    if (values.count("version")) {
        result.command = Command::version;
        return result;
    }
    if ((command == "serve" || ((command == "schema" || command == "passwords") && action == "migrate")) && values.count("config")) {
        result.command = command == "serve" ? Command::serve : command == "schema" ? Command::schema_migrate : Command::passwords_migrate;
        if (command == "passwords" && !values.count("confirm-hash-compatible"))
            throw po::error{"Hash compatibility confirmation is required"};
        result.config = values["config"].as<std::string>();
        if (result.config.empty()) throw po::error{"Empty configuration path"};
        if (values.count("log-level")) {
            result.log_level = values["log-level"].as<std::string>();
            if (result.log_level != "info" && result.log_level != "debug" && result.log_level != "trace")
                throw po::error{"Invalid log level"};
        }
    } else if (command == "key" && !action.empty() && values.count("key-file")) {
        result.command = action == "rotate" ? Command::key_rotate : Command::key_create;
        result.key_file = values["key-file"].as<std::string>();
        if (result.key_file.empty()) throw po::error{"Empty verifier path"};
    } else if (command == "secret" && action == "create") {
        result.command = Command::secret_create;
    } else {
        throw po::error{"Missing command or required option"};
    }
    return result;
}

void initialize_logging(std::string_view level) {
    auto value = logfault::LogLevel::INFO;
    if (level == "debug") value = logfault::LogLevel::DEBUGGING;
    if (level == "trace") value = logfault::LogLevel::TRACE;
    logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, value));
}
}
int main(int argc, char** argv) {
    using namespace mailadmin;
    try {
        const auto args = parse_arguments(argc, argv);
        if (args.command == Command::help) return 0;
        if (args.command == Command::version) {
            std::cout << MAILADMIN_VERSION << '\n';
            return 0;
        }
        if (args.command == Command::secret_create) {
            auto secret = random_secret();
            std::cout << secret << '\n' << std::flush;
            OPENSSL_cleanse(secret.data(), secret.size());
            return std::cout ? 0 : 1;
        }
        if (args.command == Command::key_create || args.command == Command::key_rotate) {
            create_key(args.key_file, args.command == Command::key_rotate);
            return 0;
        }
        auto settings = read_config(args.config);
        if (!args.log_level.empty()) settings.log_level = args.log_level;
        auto config = std::make_shared<const Config>(std::move(settings));
        initialize_logging(config->log_level);
        if (mysql_library_init(0, nullptr, nullptr)) throw std::runtime_error{"Database library initialization failed"};
        struct Library { ~Library() { mysql_library_end(); } } library;
        if (args.command == Command::schema_migrate) { migrate_schema(*config); return 0; }
        if (args.command == Command::passwords_migrate) { migrate_passwords(*config); return 0; }
        if (geteuid() == 0) throw std::runtime_error{"Run the daemon as the Courier mailbox user, not root"};
        if (config->administration) {
            struct stat info{};
            if (lstat(config->key_file.c_str(), &info) || info.st_uid != 0 || !S_ISREG(info.st_mode))
                throw std::runtime_error{"Administration requires a root-owned API key verifier"};
        }
        const auto verifier = load_key(config->key_file);
        // Handle signals on the main thread. HttpServer::stop joins its HTTP
        // workers and must not be invoked from one of those workers.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGTERM);
        sigaddset(&signals, SIGINT);
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr)) throw std::runtime_error{"Cannot configure termination signals"};
        auto service = std::make_shared<Service>(config);
        yahat::HttpServer server{config->http, [verifier](const yahat::AuthReq& request) {
            yahat::Auth auth;
            if (request.req.headers.count(boost::beast::http::field::authorization) == 1)
                auth.access = verify_key(request.auth_header, verifier);
            if (auth.access) auth.account = "administrator";
            return auth;
        }, "mailadmin/" MAILADMIN_VERSION};
        struct ServerLifetime {
            yahat::HttpServer& server;
            std::shared_ptr<Service> service;
            bool stopped = false;
            void stop() {
                if (!stopped) { stopped = true; server.stop(); }
            }
            ~ServerLifetime() {
                try { stop(); } catch (...) { MA_ERROR << "HTTP shutdown failed"; }
                // DB completions can post back to the stopped HTTP context.
                // Finish them while that context and Connector/C still exist.
                service->finish_database_work();
            }
        } lifetime{server, service};
        mount_api(server, service);
        auto running = server.start();
        MA_INFO << "Mailadmin " MAILADMIN_VERSION " starting; administration=" << config->administration << " endpoint="
                << config->http.http_endpoint << ':' << config->http.http_port;
        MA_INFO << "DNS integration remains deferred";
        bool signalled = false;
        while (!server.getCtx().stopped()) {
            const timespec interval{1, 0};
            const auto result = sigtimedwait(&signals, nullptr, &interval);
            if (result == SIGTERM || result == SIGINT) { signalled = true; break; }
        }
        MA_INFO << "Stopping mailadmin";
        lifetime.stop();
        running.get();
        return signalled ? 0 : 1;
    } catch (const po::error&) {
        // Parser diagnostics can echo arbitrary argument values. Keep secrets
        // out of stderr as well as the application log.
        std::cerr << "mailadmin: invalid command-line arguments; use --help or COMMAND --help\n";
        return 2;
    } catch (const ApiError& error) {
        MA_ERROR << "Administration command failed; code=" << error.code;
        std::cerr << "mailadmin: " << error.code << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception&) {
        // A config/connector exception may contain a credential. Startup failure
        // text is deliberately generic; structured operational errors log codes.
        std::cerr << "mailadmin: command failed; check configuration, key permissions and runtime dependencies\n";
        return 1;
    }
}
