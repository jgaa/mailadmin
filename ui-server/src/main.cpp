#include "Application.h"
#include "Authentication.h"
#include <openssl/crypto.h>
#include "BuildConfig.h"
#include "logging.h"

#include <Wt/WServer.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    std::string api_url, api_key_file, address, config, docroot, resources, log_level;
    unsigned port = 8081;
    std::string credentials_file, initial_password_file, username, password, password_file;
    std::vector<std::string> command;
    po::options_description general{"General"};
    general.add_options()
        ("help,h", "Show options")
        ("version,v", "Show version")
        ("log-level,l", po::value(&log_level)->default_value("info"), "info, debug or trace");
    po::options_description server_options{"UI server"};
    server_options.add_options()
        ("api-url", po::value(&api_url)->default_value("http://127.0.0.1:8080"), "API origin; HTTPS or numeric loopback HTTP")
        ("api-key-file", po::value(&api_key_file), "Protected API key file; alternatives: MAILADMIN_API_KEY or MAILADMIN_API_KEY_FILE")
        ("http-address", po::value(&address)->default_value("127.0.0.1"), "Numeric HTTP listener address")
        ("http-port", po::value(&port)->default_value(8081), "UI HTTP port")
        ("wt-config,c", po::value(&config)->default_value(MAILADMIN_UI_WT_CONFIG), "Wt configuration XML")
        ("docroot", po::value(&docroot)->default_value(MAILADMIN_UI_DOCROOT), "Public static document root; never use the source/config directory")
        ("resources-dir", po::value(&resources)->default_value(MAILADMIN_UI_RESOURCES), "Wt resources directory");
    po::options_description authentication_options{"UI authentication"};
    authentication_options.add_options()
        ("credentials-file", po::value(&credentials_file)->default_value("/var/lib/mailadmin-ui/admin.json"), "Persistent salted UI password hash")
        ("initial-password-file", po::value(&initial_password_file)->default_value("/var/lib/mailadmin-ui/initial-password"), "Generated initial password (0600)")
        ("username", po::value(&username)->default_value("admin"), "Username for bootstrap or password set")
        ("password", po::value(&password), "New UI password for password set (12-1024 bytes)")
        ("password-file", po::value(&password_file), "Read new UI password from a protected file");
    po::options_description hidden;
    hidden.add_options()("command", po::value(&command), "Command");
    po::positional_options_description positional;
    positional.add("command", 2);
    po::options_description options, parsing_options;
    options.add(general).add(server_options).add(authentication_options);
    parsing_options.add(options).add(hidden);
    po::variables_map values;
    try {
        po::store(po::command_line_parser(argc, argv).options(parsing_options).positional(positional)
            .style(po::command_line_style::unix_style & ~po::command_line_style::allow_guessing).run(), values);
        po::notify(values);
        if (values.count("help")) {
            std::cout << "mailadmin_ui [options]\nmailadmin_ui password set --password VALUE | --password-file PATH [options]\nWt mail administration UI. Configure its API credential using an environment variable or key file.\n" << options;
            return 0;
        }
        if (values.count("version")) { std::cout << MAILADMIN_UI_VERSION << '\n'; return 0; }
        if ((!command.empty() && command != std::vector<std::string>{"password", "set"})
            || (command.empty() && (values.count("password") || values.count("password-file")))
            || (!command.empty() && values.count("password") + values.count("password-file") != 1))
            throw std::runtime_error{"Invalid command"};
        boost::system::error_code address_error;
        (void)boost::asio::ip::make_address(address, address_error);
        if (address_error || port == 0 || port > 65535
            || (log_level != "info" && log_level != "debug" && log_level != "trace"))
            throw std::runtime_error{"Invalid option"};
    } catch (...) {
        std::cerr << "Invalid command line. Use --help.\n";
        return 2;
    }
    const auto level = log_level == "trace" ? logfault::LogLevel::TRACE
        : log_level == "debug" ? logfault::LogLevel::DEBUGGING : logfault::LogLevel::INFO;
    logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, level));
    auto authentication = std::shared_ptr<mailadmin::ui::Authentication>{};
    try {
        authentication = std::make_shared<mailadmin::ui::Authentication>(credentials_file, initial_password_file, username);
        if (!command.empty()) {
            if (values.count("password-file")) password = mailadmin::ui::Authentication::password_file(password_file);
            authentication->set_password(username, password);
            OPENSSL_cleanse(password.data(), password.size());
            std::cout << "UI password hash updated. Existing UI sessions will require sign-in again.\n";
            return 0;
        }
    } catch (const std::exception& error) {
        OPENSSL_cleanse(password.data(), password.size());
        UI_ERROR << "UI password command failed: " << error.what();
        return 1;
    }
    std::shared_ptr<const mailadmin::ui::Credential> credential;
    try {
        credential = mailadmin::ui::load_credential(api_key_file);
    } catch (const std::exception& error) {
        // Credential loading emits only fixed diagnostic messages, never values.
        UI_ERROR << error.what();
        return 1;
    }
    try {
        if (authentication->initialize())
            UI_INFO << "Initial UI administrator password written to " << authentication->initial_password_file().string();
    } catch (...) {
        UI_ERROR << "UI authentication initialization failed; check protected credential files and writable state directory";
        return 1;
    }
    try {
        auto client = std::make_shared<mailadmin::ui::ApiClient>(api_url);
        Wt::WServer server{argv[0], config};
        server.setServerConfiguration(argv[0], {"--http-address", address, "--http-port", std::to_string(port),
            "--docroot", docroot, "--resources-dir", resources, "--accesslog", "-", "--threads", "4"});
        server.addEntryPoint(Wt::EntryPointType::Application, [client, credential, authentication](const Wt::WEnvironment& environment) {
            return std::make_unique<mailadmin::ui::Application>(environment, client, credential, authentication);
        });
        if (!server.start()) {
            UI_ERROR << "UI listener could not be started";
            return 1;
        }
        UI_INFO << "Mailadmin UI " << MAILADMIN_UI_VERSION << " listening on " << address << ':' << port;
        Wt::WServer::waitForShutdown();
        server.stop();
        UI_INFO << "Mailadmin UI stopped";
    } catch (...) {
        UI_ERROR << "UI startup or runtime failure; check listener, API URL, Wt resources and configuration";
        return 1;
    }
}
