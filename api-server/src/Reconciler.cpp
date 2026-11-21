#include "Administration.h"
#include "Mailstore.h"
#include "Security.h"
#include "Dns.h"
#include "logging.h"
#include "ReconciliationDiagnostics.h"
#include "ReconciliationPlan.h"
#include "SpamPolicy.h"
#include <chrono>
#include <unistd.h>

namespace mailadmin {
namespace {
std::vector<MailboxPlan> plans(Connection& db) {
    return detail::mailbox_plans(db.query("SELECT p.passwd_id,p.organization_id,p.id,p.home,p.maildir,p.quota,"
            "COALESCE(p.filterctl,o.filterctl),COALESCE(p.spammgt,o.spammgt),COALESCE(p.spamfld,o.spamfld),"
            "COALESCE(p.spamtrash,o.spamtrash,'Trash'),COALESCE(p.spamdays,o.spamdays),COALESCE(p.oomsg,''),p.keep_copy,p.uid,p.gid,(p.spamtrash IS NULL AND o.spamtrash IS NULL) "
            "FROM passwd p JOIN organization o ON o.organization_id=p.organization_id "
            "JOIN admin_account a ON a.passwd_id=p.passwd_id WHERE a.desired_active='yes' AND o.hosted_here='yes' ORDER BY p.passwd_id"), [&db](const std::string& id) {
        return db.query("SELECT forward_id,destination FROM forward WHERE passwd_id=? ORDER BY forward_id",{id});
    });
}
}
namespace detail {
std::vector<MailboxPlan> mailbox_plans(const Rows& rows,
        const std::function<Rows(const std::string&)>& forwards) {
    std::vector<MailboxPlan> result;
    for (const auto& row : rows) {
        auto check = [&](std::string_view field, auto&& operation) -> decltype(auto) {
            return reconciliation_step({"planning","passwd",row[0],field,row[0],row[1]},
                std::forward<decltype(operation)>(operation));
        };
        check("uid/gid",[&] {
            if (row[13]!=std::to_string(geteuid()) || row[14]!=std::to_string(getegid()))
                throw ApiError{409,"mailbox_identity_conflict","Mailbox UID/GID differs from the daemon identity"};
        });
        MailboxPlan plan;
        plan.id=row[0]; plan.organization_id=row[1];
        plan.address=check("id",[&] { return mailbox_address(row[2]); });
        plan.home=row[3]; plan.maildir=row[4]; plan.quota=row[5]; plan.filters=row[6]=="yes";
        const auto trash=row[15]=="1" ? std::optional<std::string>{} : std::optional<std::string>{row[9]};
        const auto spam=resolve_spam_policy({row[7]=="yes",row[8],row[10],trash}, {},
            {"planning","passwd",row[0],{},row[0],row[1]});
        plan.spam=spam.enabled; plan.spam_folder=spam.folder; plan.spam_trash=spam.trash;
        plan.retention_days=spam.retention_days; plan.spam_cleanup_ready=spam.cleanup_ready;
        plan.auto_reply=row[11]; plan.keep_copy=row[12]=="yes";
        for (const auto& forward : forwards(plan.id)) {
            plan.forwards.push_back(reconciliation_step(
                {"planning","forward",forward[0],"destination",plan.id,plan.organization_id},
                [&] { return mailbox_address(forward[1]); }));
        }
        result.emplace_back(std::move(plan));
    }
    return result;
}
}

void reconcile(const Config& config, bool cleanup_spam) {
    Connection db{config};
    auto lock=std::make_unique<AdministrationLock>(db);
    const auto state=db.query("SELECT generation,applied_generation FROM admin_state WHERE singleton=1").at(0);
    std::vector<MailboxPlan> mailboxes;
    try { mailboxes=plans(db); }
    catch (const ApiError& error) {
        db.query("UPDATE admin_state SET last_error=? WHERE singleton=1",{error.code});
        throw;
    }
    bool provisioning_started=false;
    if (state[0]!=state[1]) {
        try {
            MA_DEBUG << "Mailbox planning complete; accounts=" << mailboxes.size() << " provisioning_started=no";
            for (const auto& mailbox : mailboxes) {
                provisioning_started=true;
                provision_mailbox(config,mailbox);
            }
            boost::json::array domains, aliases;
            std::size_t domain_row=0;
            for (const auto& row : db.query("SELECT d.name,d.organization_id FROM domain d JOIN organization o ON o.organization_id=d.organization_id WHERE o.hosted_here='yes' ORDER BY d.name")) {
                // Domain names are configuration values. Identify their ordered
                // row and owner rather than exposing the name in diagnostics.
                const auto index=std::to_string(++domain_row);
                domains.emplace_back(reconciliation_step(
                    {"routing","domain_row",index,"name",{},row[1],provisioning_started},
                    [&] { return domain_name(row[0]); }));
            }
            for (const auto& row : db.query("SELECT a.mailaddr,p.id,a.alias_id,p.passwd_id,p.organization_id FROM alias a JOIN passwd p ON p.passwd_id=a.passwd_id JOIN admin_account m ON m.passwd_id=p.passwd_id JOIN organization o ON o.organization_id=p.organization_id WHERE m.desired_active='yes' AND o.hosted_here='yes' ORDER BY a.mailaddr")) {
                const auto address=reconciliation_step(
                    {"routing","alias",row[2],"mailaddr",row[3],row[4],provisioning_started},
                    [&] { return mailbox_address(row[0]); });
                const auto destination=reconciliation_step(
                    {"routing","alias",row[2],"passwd.id",row[3],row[4],provisioning_started},
                    [&] { return mailbox_address(row[1]); });
                aliases.emplace_back(boost::json::object{{"address",address},{"destination",destination}});
            }
            const auto request=boost::json::serialize(boost::json::object{
                {"generation",std::stoll(state[0])},{"domains",std::move(domains)},{"aliases",std::move(aliases)}});
            const auto hash=hex(digest(request));
            auto status_text=reconciliation_step(
                {"publication","admin_state","1","read_status",{},{},provisioning_started},
                [&] { return publication_status(config); });
            bool published=false;
            if (!status_text.empty()) {
                boost::system::error_code error;
                const auto value=boost::json::parse(status_text,error);
                if (!error && value.is_object()) {
                    const auto& status=value.as_object();
                    const auto* status_hash=status.if_contains("digest");
                    const auto* success=status.if_contains("success");
                    if (status_hash && status_hash->is_string() && status_hash->as_string()==hash && success && success->is_bool()) {
                        published=success->as_bool();
                        if (!published) db.query("UPDATE admin_state SET last_error='courier_activation_failed' WHERE singleton=1");
                    }
                }
            }
            if (!published) {
                reconciliation_step({"publication","admin_state","1","write_request",{},{},provisioning_started},
                    [&] { publish_request(config,request); });
                // Publication/rebuild is performed by the restricted root host
                // helper. Wait for its durable, digest-matched acknowledgement.
                MA_DEBUG << "Courier activation pending; generation=" << state[0];
                return;
            }
            Transaction transaction{db};
            db.query("UPDATE passwd p JOIN admin_account a ON a.passwd_id=p.passwd_id JOIN organization o ON o.organization_id=p.organization_id SET p.active=IF(a.desired_active='yes' AND o.hosted_here='yes','yes','no')");
            db.query("UPDATE admin_state SET applied_generation=generation,last_error='' WHERE singleton=1");
            db.query("DELETE FROM admin_retired_address WHERE generation<=(SELECT applied_generation FROM admin_state WHERE singleton=1)");
            transaction.commit();
            MA_INFO << "Courier configuration activated; generation=" << state[0];
        } catch (const ApiError& error) {
            db.query("UPDATE admin_state SET last_error=? WHERE singleton=1",{error.code});
            throw;
        }
    }
    // Cleanup identities are immutable and never reused. Do not hold the
    // writer lock while removing large mailboxes or scanning spam folders.
    lock.reset();
    for (const auto& row : db.query("SELECT passwd_id,organization_id,home FROM admin_cleanup WHERE generation<=(SELECT applied_generation FROM admin_state WHERE singleton=1) ORDER BY passwd_id LIMIT 100")) {
        try {
            remove_mailbox(config,row[1],row[0],row[2]);
            db.query("DELETE FROM admin_cleanup WHERE passwd_id=?",{row[0]});
            MA_INFO << "Mailbox files removed; account_id=" << row[0];
        } catch (const ApiError& error) {
            db.query("UPDATE admin_cleanup SET attempts=attempts+1,last_error=? WHERE passwd_id=?",{error.code,row[0]});
            MA_WARN << "Mailbox cleanup deferred; account_id=" << row[0] << " code=" << error.code;
        }
    }
    if (cleanup_spam) {
        for (const auto& mailbox : mailboxes) {
            try { clean_spam(config,mailbox); }
            catch (const ApiError& error) {
                MA_WARN << "Spam cleanup failed; account_id=" << mailbox.id << " code=" << error.code;
            }
        }
        MA_DEBUG << "Scheduled spam cleanup pass completed";
    }
}
Reconciler::Reconciler(std::shared_ptr<const Config> config) : config_{std::move(config)} {
    if (!config_->administration) return;
    if (!config_->nsblast_api_url.empty()) dns_ = std::make_unique<DnsWorker>(config_);
    thread_=std::thread{[this] {
        auto next_spam=std::chrono::steady_clock::now();
        bool first=true;
        for (;;) {
            try {
                if (first) {
                    Connection db{*config_};
                    AdministrationLock lock{db};
                    db.query("UPDATE admin_state SET generation=generation+1 WHERE singleton=1");
                    first=false;
                }
                const auto now=std::chrono::steady_clock::now();
                const bool run_spam=now>=next_spam;
                reconcile(*config_,run_spam);
                if (run_spam) next_spam=now+std::chrono::seconds{config_->spam_cleanup_seconds};
            } catch (const ApiError& error) {
                MA_ERROR << "Reconciliation deferred; code=" << error.code;
            } catch (const std::exception&) {
                MA_ERROR << "Reconciliation failed; no input or exception data logged";
            }
            std::unique_lock guard{mutex_};
            if (condition_.wait_for(guard,std::chrono::seconds{config_->reconcile_seconds},[this] { return stopping_; })) break;
        }
    }};
}
Reconciler::~Reconciler() { stop(); }
void Reconciler::stop() {
    if (dns_) dns_->stop();
    { std::lock_guard guard{mutex_}; stopping_=true; }
    condition_.notify_all();
    if (thread_.joinable()) thread_.join();
}
}
