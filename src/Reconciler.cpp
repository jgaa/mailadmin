#include "Administration.h"
#include "Mailstore.h"
#include "Security.h"
#include "logging.h"
#include <chrono>
#include <unistd.h>

namespace mailadmin {
namespace {
std::vector<MailboxPlan> plans(Connection& db) {
    std::vector<MailboxPlan> result;
    for (const auto& row : db.query("SELECT p.passwd_id,p.organization_id,p.id,p.home,p.maildir,p.quota,"
            "COALESCE(p.filterctl,o.filterctl),COALESCE(p.spammgt,o.spammgt),COALESCE(p.spamfld,o.spamfld),"
            "COALESCE(p.spamtrash,o.spamtrash),COALESCE(p.spamdays,o.spamdays),COALESCE(p.oomsg,''),p.keep_copy,p.uid,p.gid "
            "FROM passwd p JOIN organization o ON o.organization_id=p.organization_id "
            "JOIN admin_account a ON a.passwd_id=p.passwd_id WHERE a.desired_active='yes' AND o.hosted_here='yes' ORDER BY p.passwd_id")) {
        if (row[13]!=std::to_string(geteuid()) || row[14]!=std::to_string(getegid()))
            throw ApiError{409,"mailbox_identity_conflict","Mailbox UID/GID differs from the daemon identity"};
        MailboxPlan plan;
        plan.id=row[0]; plan.organization_id=row[1]; plan.address=mailbox_address(row[2]);
        plan.home=row[3]; plan.maildir=row[4]; plan.quota=row[5]; plan.filters=row[6]=="yes";
        plan.spam=row[7]=="yes"; plan.spam_folder=folder_name(row[8]); plan.spam_trash=folder_name(row[9],true);
        const auto days=std::stoll(row[10]);
        if (days<1 || days>36500 || plan.spam_folder==plan.spam_trash)
            throw ApiError{409,"spam_policy_conflict","Invalid effective spam retention or folder policy"};
        plan.retention_days=days; plan.auto_reply=row[11]; plan.keep_copy=row[12]=="yes";
        for (const auto& forward : db.query("SELECT destination FROM forward WHERE passwd_id=? ORDER BY forward_id",{plan.id}))
            plan.forwards.push_back(mailbox_address(forward[0]));
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
    if (state[0]!=state[1]) {
        try {
            for (const auto& mailbox : mailboxes) provision_mailbox(config,mailbox);
            boost::json::array domains, aliases;
            for (const auto& row : db.query("SELECT d.name FROM domain d JOIN organization o ON o.organization_id=d.organization_id WHERE o.hosted_here='yes' ORDER BY d.name"))
                domains.emplace_back(domain_name(row[0]));
            for (const auto& row : db.query("SELECT a.mailaddr,p.id FROM alias a JOIN passwd p ON p.passwd_id=a.passwd_id JOIN admin_account m ON m.passwd_id=p.passwd_id JOIN organization o ON o.organization_id=p.organization_id WHERE m.desired_active='yes' AND o.hosted_here='yes' ORDER BY a.mailaddr"))
                aliases.emplace_back(boost::json::object{{"address",mailbox_address(row[0])},{"destination",mailbox_address(row[1])}});
            const auto request=boost::json::serialize(boost::json::object{
                {"generation",std::stoll(state[0])},{"domains",std::move(domains)},{"aliases",std::move(aliases)}});
            const auto hash=hex(digest(request));
            auto status_text=publication_status(config);
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
                publish_request(config,request);
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
    { std::lock_guard guard{mutex_}; stopping_=true; }
    condition_.notify_all();
    if (thread_.joinable()) thread_.join();
}
}
