#include "Dns.h"
#include "Security.h"
#define RESTINCURL_LOG(message) do {} while (false)
#define RESTINCURL_LOG_TRACE(message) do {} while (false)
#include <restincurl/restincurl.h>
#include "logging.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace mailadmin {
namespace {
using Object = boost::json::object;
using Value = boost::json::value;
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
[[noreturn]] void invalid(std::string message) { throw ApiError{400,"invalid_dns_configuration",std::move(message)}; }
void fields(const Object& value, std::initializer_list<std::string_view> allowed) {
    for (const auto& item : value)
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) invalid("Unknown DNS configuration field");
}
std::string string(const Value& value) {
    if (!value.is_string() || value.as_string().find('\0') != boost::json::string::npos) invalid("DNS text field has an invalid type");
    return std::string{value.as_string()};
}
void public_key(const Value& value) {
    const auto encoded = string(value);
    if (encoded.empty() || encoded.size() > 2048 || encoded.size() % 4) invalid("DKIM needs canonical Base64 RSAPublicKey DER");
    std::vector<unsigned char> bytes(encoded.size());
    int length = EVP_DecodeBlock(bytes.data(), reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    if (length < 0) invalid("Invalid Base64 DKIM public key");
    if (encoded.ends_with("==")) length -= 2;
    else if (encoded.ends_with('=')) --length;
    if (length <= 0) invalid("Empty DKIM public key");
    std::string canonical(4 * ((length + 2) / 3) + 1, '\0');
    canonical.resize(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(canonical.data()), bytes.data(), length));
    if (canonical != encoded) invalid("DKIM public key must use canonical Base64");
    const unsigned char* cursor = bytes.data();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{d2i_PublicKey(EVP_PKEY_RSA,nullptr,&cursor,length),EVP_PKEY_free};
    if (!key || cursor != bytes.data() + length || EVP_PKEY_get_bits(key.get()) < 2048 || EVP_PKEY_get_bits(key.get()) > 8192)
        invalid("DKIM requires a 2048–8192-bit RSA public key, not a private key, PEM or SubjectPublicKeyInfo");
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context{EVP_PKEY_CTX_new(key.get(),nullptr),EVP_PKEY_CTX_free};
    if (!context || EVP_PKEY_public_check(context.get()) != 1) invalid("Invalid RSA public key");
}
std::string read_key(const std::filesystem::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error{"Cannot open DNS credential"};
    struct Close { int fd; ~Close() { close(fd); } } close_fd{fd};
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || (info.st_mode & 0037) || info.st_size < 1 || info.st_size > 4096
        || (info.st_uid != 0 && info.st_uid != geteuid())) throw std::runtime_error{"Unsafe DNS credential file"};
    std::string key(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < key.size()) {
        const auto count = read(fd, key.data() + offset, key.size() - offset);
        if (count <= 0) throw std::runtime_error{"Cannot read DNS credential"};
        offset += static_cast<std::size_t>(count);
    }
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
    if (key.empty() || !std::ranges::all_of(key, [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw std::runtime_error{"Invalid DNS credential"};
    return key;
}
std::size_t receive(char* data, std::size_t size, std::size_t count, void* pointer) noexcept {
    auto& body = *static_cast<std::string*>(pointer);
    if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto bytes = size * count;
    if (bytes > 65536 - body.size()) return 0;
    try { body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
}
Object report(const Object& value, const std::string& key) {
    Object result;
    for (const auto* name : {"domain","status","transition","earliest_completion","applied_policy","created","updated","deleted","warnings","attempts","max_retries","next","failure"}) {
        if (const auto* field = value.if_contains(name)) result[name] = *field;
    }
    const auto serialized = boost::json::serialize(result);
    if (serialized.find(key) != std::string::npos) return {{"status","failed"},{"failure","DNS provider response contained credential material"}};
    if (serialized.size() > 60000) {
        result.erase("created"); result.erase("updated"); result.erase("deleted");
        result["report_truncated"] = true;
        if (boost::json::serialize(result).size() > 60000) return {{"status","failed"},{"failure","DNS provider report exceeded the storage limit"}};
    }
    return result;
}
}

void validate_dns_config(const Config& config) {
    if (config.nsblast_api_url.empty()) {
        if (!config.nsblast_api_key_file.empty() || !config.dns_default_configuration.empty())
            throw std::runtime_error{"DNS URL is required when DNS configuration is supplied"};
        return;
    }
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url{curl_url(), curl_url_cleanup};
    if (!url || curl_url_set(url.get(), CURLUPART_URL, config.nsblast_api_url.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error{"Invalid nsblast API URL"};
    auto part = [&](CURLUPart field) {
        char* raw = nullptr;
        (void)curl_url_get(url.get(), field, &raw, 0);
        std::unique_ptr<char, decltype(&curl_free)> memory{raw, curl_free};
        return raw ? std::string{raw} : std::string{};
    };
    const auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST), path = part(CURLUPART_PATH);
    if ((scheme != "https" && !(scheme == "http" && (host == "127.0.0.1" || host == "[::1]")))
        || (path != "/api/v1" && path != "/api/v1/") || !part(CURLUPART_USER).empty()
        || !part(CURLUPART_PASSWORD).empty() || !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty())
        throw std::runtime_error{"Use an HTTPS nsblast /api/v1 root or numeric loopback HTTP"};
    if (!config.nsblast_api_key_file.is_absolute()) throw std::runtime_error{"Absolute DNS credential path is required"};
    if (!config.dns_default_configuration.empty()) validate_dns_desired(config, config.dns_default_configuration);
}
void validate_dns_desired(const Config& config, const Object& desired) {
    fields(desired, {"mail_servers","ttl","spf","dkim","dmarc","mta_sts","tls_reporting"});
    if (const auto* mta = desired.if_contains("mta_sts"); mta && mta->is_object()) {
        const auto* enabled = mta->as_object().if_contains("enabled");
        if (enabled && enabled->is_bool() && enabled->as_bool() && !config.nsblast_mta_sts_https_ready)
            invalid("Configure trusted HTTPS policy delivery and confirm MTA-STS readiness in the service configuration first");
    }
    const auto* servers = desired.if_contains("mail_servers");
    if (!servers || !servers->is_array() || servers->as_array().empty() || servers->as_array().size() > 16)
        invalid("Provide between one and sixteen MX hosts");
    for (const auto& server : servers->as_array()) {
        if (!server.is_object()) invalid("Each MX host must be an object");
        fields(server.as_object(), {"hostname","priority","ipv4","ipv6"});
        const auto* hostname = server.as_object().if_contains("hostname");
        if (!hostname) invalid("MX hostname is required");
        (void)domain_name(string(*hostname));
    }
    if (const auto* dkim = desired.if_contains("dkim")) {
        if (!dkim->is_array() || dkim->as_array().size() > 32) invalid("DKIM must be an array of public selectors");
        for (const auto& selector : dkim->as_array()) {
            if (!selector.is_object()) invalid("DKIM selector must be an object");
            fields(selector.as_object(), {"selector","algorithm","public_key"});
            const auto* key = selector.as_object().if_contains("public_key");
            if (!key || !key->is_string() || key->as_string().find("PRIVATE") != boost::json::string::npos
                || key->as_string().find("-----") != boost::json::string::npos) invalid("Supply only DNS DKIM public-key data");
            public_key(*key);
        }
    }
    if (boost::json::serialize(desired).size() > 15000) invalid("DNS configuration exceeds Mailadmin's request limit");
}
Object dns_defaults(const Config& config) {
    Object result{{"ttl",3600}, {"spf",Object{{"enabled",true},{"policy","v=spf1 mx -all"}}},
                  {"dmarc",Object{{"enabled",true},{"policy","none"}}}, {"dkim",boost::json::array{}},
                  {"mta_sts",Object{{"enabled",false}}}, {"tls_reporting",Object{{"enabled",false}}}};
    if (!config.default_mail_hostname.empty())
        result["mail_servers"] = boost::json::array{Object{{"hostname",config.default_mail_hostname},{"priority",10}}};
    return result;
}
DnsLocks::~DnsLocks() {
    for (const auto& name : locks_) { try { db_.query("SELECT RELEASE_LOCK(?)", {name}); } catch (...) {} }
}
void DnsLocks::lock(const std::string& domain) {
    const auto name = "mailadmin.dns." + hex(digest(domain)).substr(0, 40);
    if (std::find(locks_.begin(), locks_.end(), name) != locks_.end()) return;
    if (db_.query("SELECT GET_LOCK(?,0)", {name}).at(0).at(0) != "1")
        throw ApiError{409,"dns_busy","A DNS exchange is running for this domain; retry after refreshing its status"};
    locks_.push_back(name);
}
void migrate_dns(Connection& db) {
    db.query("CREATE TABLE IF NOT EXISTS admin_dns (domain VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY, method VARCHAR(8) NOT NULL, desired MEDIUMTEXT NOT NULL, revision BIGINT NOT NULL DEFAULT 1, status VARCHAR(32) NOT NULL DEFAULT 'queued', transition_id VARCHAR(256) NOT NULL DEFAULT '', report MEDIUMTEXT NOT NULL, last_error VARCHAR(128) NOT NULL DEFAULT '', attempts INT NOT NULL DEFAULT 0, next_attempt BIGINT NOT NULL DEFAULT 0, earliest_completion BIGINT NOT NULL DEFAULT 0, poll_deadline BIGINT NOT NULL DEFAULT 0, managed TINYINT NOT NULL DEFAULT 0) ENGINE=InnoDB");
    if (db.query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='admin_dns' AND COLUMN_NAME='managed'").empty()) {
        db.query("ALTER TABLE admin_dns ADD managed TINYINT NOT NULL DEFAULT 0");
    }
    // Backfill is safe to repeat if a crash followed the implicitly committed
    // ALTER but preceded this update.
    db.query("UPDATE admin_dns SET managed=1 WHERE status IN ('pending','running','polling') OR (method<>'DELETE' AND status='complete')");
}
void dns_available_name(Connection& db, const std::string& domain) {
    if (!db.query("SELECT domain FROM admin_dns WHERE domain=? AND method='DELETE' AND status<>'complete'", {domain}).empty())
        throw ApiError{409,"dns_retiring","Wait for pending DNS removal before reusing this domain"};
}
void queue_dns(Connection& db, DnsLocks& locks, const Config& config, const std::string& domain,
               const std::string& method, const Object& desired) {
    if (config.nsblast_api_url.empty()) throw ApiError{409,"dns_disabled","Configure the nsblast URL and credential before provisioning DNS"};
    locks.lock(domain);
    if (method != "DELETE") validate_dns_desired(config, desired);
    const auto payload = method == "DELETE" ? std::string{} : boost::json::serialize(desired);
    const auto previous = db.query("SELECT method,desired,status,managed FROM admin_dns WHERE domain=? FOR UPDATE", {domain});
    if (!previous.empty() && previous[0][0] == method && previous[0][1] == payload && previous[0][2] != "cancelled") {
        // An identical failed request must not reset nsblast's retry budget.
        return;
    }
    if (method == "POST" && !previous.empty()
        && (previous[0][3] == "1" || previous[0][2] == "unknown" || previous[0][2] == "pending"
            || previous[0][2] == "running" || previous[0][2] == "polling" || previous[0][2] == "sending" || previous[0][2] == "retry"))
        throw ApiError{409,"dns_replacement_required","Inspect the existing DNS operation and use an explicit complete replacement instead of a new create-only request"};
    db.query("INSERT INTO admin_dns(domain,method,desired,report) VALUES(?,?,?,'{}') ON DUPLICATE KEY UPDATE method=VALUES(method),desired=VALUES(desired),revision=revision+1,status='queued',transition_id='',report='{}',last_error='',attempts=0,next_attempt=0,earliest_completion=0,poll_deadline=0",
             {domain,method,payload});
    MA_INFO << "Mail DNS intent queued; domain=" << domain << " method=" << method;
}
void dns_created(Connection& db, DnsLocks& locks, const Config& config, const std::string& domain, const Object& input) {
    bool provision = !config.dns_default_configuration.empty();
    if (const auto* flag = input.if_contains("provision_dns")) {
        if (!flag->is_bool()) invalid("provision_dns must be a boolean");
        provision = flag->as_bool();
    }
    const auto* desired = input.if_contains("dns");
    const auto* dedicated = input.if_contains("mail_server");
    if (dedicated && (desired || (input.if_contains("provision_dns") && !provision))) invalid("Choose dedicated mail provisioning or a complete DNS configuration");
    if (dedicated) {
        if (!dedicated->is_object()) invalid("mail_server must be an MX host object");
        if (!dedicated->as_object().contains("ipv4") && !dedicated->as_object().contains("ipv6")) invalid("Dedicated mail provisioning requires its host addresses");
        auto configuration = config.dns_default_configuration.empty() ? dns_defaults(config) : config.dns_default_configuration;
        configuration["mail_servers"] = boost::json::array{*dedicated};
        queue_dns(db, locks, config, domain, "POST", configuration);
        return;
    }
    if (desired && !desired->is_object()) invalid("dns must be a complete configuration object");
    if (desired && input.if_contains("provision_dns") && !provision) invalid("Cannot supply DNS configuration while disabling provisioning");
    if (desired || provision) queue_dns(db, locks, config, domain, "POST", desired ? desired->as_object() : config.dns_default_configuration);
}
void dns_removed(Connection& db, DnsLocks& locks, const Config& config, const std::string& domain) {
    locks.lock(domain);
    const auto rows = db.query("SELECT method,status,managed,last_error FROM admin_dns WHERE domain=? FOR UPDATE", {domain});
    if (rows.empty() || rows[0][0] == "DELETE") return;
    const auto& row = rows[0];
    if (row[2] == "1") { queue_dns(db, locks, config, domain, "DELETE"); return; }
    // A definitive create conflict/authorization/validation failure never
    // established ownership. Removing the local record must preserve those
    // pre-existing remote records. A lost acknowledgement needs an explicit
    // operator decision rather than an automatic destructive follow-up.
    if (row[1] != "queued" && row[1] != "cancelled"
        && !(row[1] == "failed" && row[3].starts_with("provider_http_4")))
        throw ApiError{409,"dns_ownership_unknown","Inspect the unresolved DNS outcome and explicitly reconcile or remove its mail DNS before deleting this domain"};
    db.query("UPDATE admin_dns SET status='cancelled',last_error='',next_attempt=0 WHERE domain=?", {domain});
}
Object dns_state(Connection& db, const Config& config, const std::string& domain) {
    Object result{{"domain",domain},{"enabled",!config.nsblast_api_url.empty()},{"status","unmanaged"},
                  {"default_configuration",config.dns_default_configuration}};
    result["managed"] = false;
    const auto rows = db.query("SELECT method,desired,revision,status,transition_id,report,last_error,attempts,next_attempt,earliest_completion,managed FROM admin_dns WHERE domain=?", {domain});
    if (rows.empty()) return result;
    const auto& row = rows[0];
    result["method"] = row[0]; result["revision"] = std::stoll(row[2]); result["status"] = row[3];
    result["transition"] = row[4]; result["error"] = row[6]; result["attempts"] = std::stoi(row[7]);
    result["next_attempt"] = std::stoll(row[8]);
    result["managed"] = row[10] == "1";
    result["earliest_completion"] = row[9] == "0" ? Value{} : Value(std::stoll(row[9]));
    if (!row[1].empty()) result["desired"] = boost::json::parse(row[1]);
    if (!row[5].empty()) result["report"] = boost::json::parse(row[5]);
    return result;
}
boost::json::array dns_jobs(Connection& db) {
    boost::json::array result;
    for (const auto& row : db.query("SELECT domain,revision,status,last_error,earliest_completion FROM admin_dns ORDER BY domain LIMIT 1000"))
        result.emplace_back(Object{{"domain",row[0]},{"revision",std::stoll(row[1])},{"status",row[2]},{"error",row[3]},{"earliest_completion",std::stoll(row[4])}});
    return result;
}

DnsWorker::DnsWorker(std::shared_ptr<const Config> config)
    : config_{std::move(config)}, key_{read_key(config_->nsblast_api_key_file)}, client_{std::make_unique<restincurl::Client>()}, timer_{io_} {
    boost::asio::co_spawn(io_, run(), [this](std::exception_ptr error) {
        if (error && !stopping_) MA_ERROR << "DNS worker stopped unexpectedly";
    });
    thread_ = std::thread{[this] { io_.run(); }};
}
DnsWorker::~DnsWorker() { stop(); OPENSSL_cleanse(key_.data(), key_.size()); }
void DnsWorker::stop() {
    if (!stopping_.exchange(true)) boost::asio::post(io_, [this] { timer_.cancel(); });
    if (thread_.joinable()) thread_.join();
}
boost::asio::awaitable<void> DnsWorker::run() {
    while (!stopping_) {
        try { co_await step(); }
        catch (...) { MA_ERROR << "DNS reconciliation deferred; no credentials or provider response logged"; }
        if (stopping_) break;
        timer_.expires_after(std::chrono::seconds{5});
        boost::system::error_code error;
        co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    }
}
boost::asio::awaitable<void> DnsWorker::step() {
    Connection db{*config_};
    const auto jobs = db.query("SELECT domain FROM admin_dns WHERE status IN ('queued','retry','sending','pending','running','polling') AND next_attempt<=? ORDER BY next_attempt,domain LIMIT 32", {std::to_string(now())});
    for (const auto& job : jobs) {
        if (stopping_) break;
        DnsLocks locks{db};
        try { locks.lock(job[0]); } catch (const ApiError&) { continue; }
        const auto rows = db.query("SELECT domain,method,desired,revision,status,transition_id,attempts,earliest_completion,poll_deadline FROM admin_dns WHERE domain=?", {job[0]});
        const auto row = rows.at(0);
        if ((row[4] != "queued" && row[4] != "retry" && row[4] != "sending" && row[4] != "pending" && row[4] != "running" && row[4] != "polling")) continue;
        const auto domain = domain_name(row[0]);
        const auto method = row[1];
        const bool polling = row[4] == "pending" || row[4] == "running" || row[4] == "polling";
        const auto attempts = std::stoll(row[6]);
        auto update = [&](const std::string& status, const std::string& error, std::int64_t next = 0) {
            db.query("UPDATE admin_dns SET status=?,last_error=?,next_attempt=? WHERE domain=? AND revision=?", {status,error,std::to_string(next),domain,row[3]});
            if (!error.empty()) MA_WARN << "Mail DNS state changed; domain=" << domain << " status=" << status << " code=" << error;
        };
        if (row[4] == "sending" && method == "POST") { update("unknown","ambiguous_create_outcome"); continue; }
        if (polling && std::stoll(row[8]) != 0 && now() > std::stoll(row[8])) { update("poll_timeout","provider_operation_may_still_be_pending"); continue; }
        auto root = config_->nsblast_api_url;
        while (root.ends_with('/')) root.pop_back();
        // domain_name accepts ASCII DNS labels only, so the name is already a
        // single URL-safe segment and can never supply a path or query.
        auto url = root + "/provision/mail/" + domain;
        if (polling) url += "/status";
        else if (method == "DELETE") url += "?preserve_ip=true";
        auto request = client_->Build();
        if (polling) request->Get(url);
        else if (method == "POST") request->Post(url);
        else if (method == "PUT") request->Put(url);
        else request->Delete(url);
        if (!polling && method != "DELETE") request->Header("Content-Type","application/json").SendData(row[2]);
        std::string body;
        request->Header("Authorization","Bearer " + key_).AcceptJson().Header("Expect", "")
            .ConnectTimeout(3000).RequestTimeout(10000)
            .Option(CURLOPT_FOLLOWLOCATION,0L).Option(CURLOPT_PROTOCOLS_STR,"https,http").Option(CURLOPT_PROXY,"")
            .Option(CURLOPT_SSL_VERIFYPEER,1L).Option(CURLOPT_SSL_VERIFYHOST,2L).Option(CURLOPT_NOSIGNAL,1L)
            .SetWriteHandler(receive,&body);
        // This durable intent distinguishes a crashed POST from one that was
        // never dispatched. PUT/DELETE can safely repeat identical intent.
        db.query("UPDATE admin_dns SET status=?,attempts=attempts+1 WHERE domain=? AND revision=?", {polling ? "polling" : "sending",domain,row[3]});
        const auto reply = co_await request->AsioAsyncExecute(boost::asio::use_awaitable);
        const auto http = reply.http_response_code;
        if (reply.curl_code != CURLE_OK || http >= 500) {
            if (!polling && method == "POST") update("unknown","ambiguous_create_outcome");
            else if (attempts >= 11 || (!polling && attempts >= 5)) update(polling ? "poll_timeout" : "failed","retry_limit_reached");
            else update(polling ? "pending" : "retry","provider_temporarily_unavailable",now() + std::min<std::int64_t>(3600,30LL << std::min<std::int64_t>(attempts,6)));
            continue;
        }
        if (http != 200 && http != 201) {
            boost::system::error_code error;
            const auto detail = boost::json::parse(body,error);
            if (!error && detail.is_object() && body.find(key_) == std::string::npos) {
                const auto* message = detail.as_object().if_contains("message");
                if (message && message->is_string() && message->as_string().size() <= 1024)
                    db.query("UPDATE admin_dns SET report=? WHERE domain=? AND revision=?",
                        {boost::json::serialize(Object{{"status","failed"},{"failure",*message}}),domain,row[3]});
            }
            update("failed","provider_http_" + std::to_string(http)); continue;
        }
        boost::system::error_code error;
        const auto envelope = boost::json::parse(body,error);
        if (error || !envelope.is_object()) { update(method == "POST" && !polling ? "unknown" : "failed","invalid_provider_response"); continue; }
        const auto& object = envelope.as_object();
        const auto* failed = object.if_contains("error");
        const auto* value = object.if_contains("value");
        if (!failed || !failed->is_bool() || failed->as_bool() || !value || !value->is_object()) { update("failed","provider_rejected_operation"); continue; }
        const auto& result = value->as_object();
        const auto* response_domain = result.if_contains("domain");
        if (!response_domain || !response_domain->is_string() || response_domain->as_string() != domain) {
            update(method == "POST" && !polling ? "unknown" : "failed","invalid_provider_domain"); continue;
        }
        const auto* status_value = result.if_contains("status");
        if (!status_value || !status_value->is_string()) { update("failed","missing_provider_status"); continue; }
        const auto status = std::string{status_value->as_string()};
        const auto* transition_value = result.if_contains("transition");
        const auto transition = transition_value && transition_value->is_string() ? std::string{transition_value->as_string()} : std::string{};
        if (transition.size() > 256 || (polling && !transition.empty() && transition != row[5])) { update("superseded","provider_transition_changed"); continue; }
        auto safe_report = report(result,key_);
        db.query("UPDATE admin_dns SET report=? WHERE domain=? AND revision=?", {boost::json::serialize(safe_report),domain,row[3]});
        if (safe_report.if_contains("failure") && safe_report.at("status") == "failed") { update("failed","provider_transition_failed"); continue; }
        if (status == "failed") { update("failed","provider_transition_failed"); continue; }
        if ((!polling && status == (method == "DELETE" ? "deleted" : "provisioned")) || (polling && status == "complete")) {
            db.query("UPDATE admin_dns SET status='complete',last_error='',next_attempt=0,managed=? WHERE domain=? AND revision=?", {method == "DELETE" ? "0" : "1",domain,row[3]});
            MA_INFO << "Mail DNS operation completed; domain=" << domain << " revision=" << row[3]; continue;
        }
        if ((status != "pending" && status != "running") || transition.empty()) { update("failed","unexpected_provider_status"); continue; }
        std::int64_t earliest = std::stoll(row[7]);
        if (const auto* deadline = result.if_contains("earliest_completion"); deadline && deadline->is_int64()) earliest = deadline->as_int64();
        if (earliest < 0 || earliest > now() + 2LL * 366 * 86400) { update("failed","invalid_provider_deadline"); continue; }
        const auto next = earliest > now() ? std::min(earliest,now() + 3600) : now() + 60;
        const auto poll_deadline = row[8] == "0" ? std::max(now(),earliest) + 86400 : std::stoll(row[8]);
        db.query("UPDATE admin_dns SET status=?,transition_id=?,last_error='',attempts=0,next_attempt=?,earliest_completion=?,poll_deadline=?,managed=1 WHERE domain=? AND revision=?",
            {status,transition,std::to_string(next),std::to_string(earliest),std::to_string(poll_deadline),domain,row[3]});
    }
}
}
