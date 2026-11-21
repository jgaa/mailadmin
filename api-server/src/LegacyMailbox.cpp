#include "LegacyMailbox.h"
#include "ReconciliationDiagnostics.h"
#include <tuple>
#include <unistd.h>

namespace mailadmin {
std::string_view legacy_reason(LegacyMailboxStatus status) {
    switch (status) {
    case LegacyMailboxStatus::intact: return "";
    case LegacyMailboxStatus::missing_home: return "missing_home";
    case LegacyMailboxStatus::missing_maildir: return "missing_maildir";
    case LegacyMailboxStatus::maildir_regular_file: return "maildir_regular_file";
    }
    throw std::logic_error{"Unknown legacy mailbox status"};
}
void migrate_legacy_mailboxes(Connection& db, bool existing, std::string_view managed_accounts) {
    for (const auto& [table,column,definition] : {
            std::tuple{"admin_account","mailbox_origin","ENUM('unclassified','legacy','managed') NOT NULL DEFAULT 'unclassified'"},
            std::tuple{"admin_account","legacy_assessment","ENUM('pending','intact','abandoned','not_required') NOT NULL DEFAULT 'pending'"},
            std::tuple{"admin_account","legacy_reason","VARCHAR(64) NOT NULL DEFAULT ''"},
            std::tuple{"admin_state","legacy_spool_device","VARCHAR(32) NOT NULL DEFAULT ''"},
            std::tuple{"admin_state","legacy_spool_inode","VARCHAR(32) NOT NULL DEFAULT ''"}}) {
        if (db.query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=? AND COLUMN_NAME=?",{table,column}).empty())
            db.query("ALTER TABLE "+std::string{table}+" ADD COLUMN "+column+" "+definition);
    }
    Transaction transaction{db};
    // The old schema had no provenance. The offline migration explicitly adopts
    // its pre-existing cohort as legacy (or managed for synthetic staging).
    while (!managed_accounts.empty()) {
        const auto end=managed_accounts.find(',');
        const auto id=std::to_string(identifier(managed_accounts.substr(0,end)));
        const auto account=db.query("SELECT mailbox_origin,legacy_assessment FROM admin_account WHERE passwd_id=?",{id});
        if (account.empty()) throw ApiError{409,"inventory_conflict","A preserved managed account does not exist"};
        if (account[0][0]=="legacy" && account[0][1]!="pending")
            throw ApiError{409,"legacy_assessment_conflict","A completed legacy assessment cannot be overridden by the managed-account migration option"};
        db.query("UPDATE admin_account SET mailbox_origin='managed',legacy_assessment='not_required',legacy_reason='' WHERE passwd_id=? AND mailbox_origin<>'managed'",{id});
        if (end==std::string_view::npos) break;
        managed_accounts.remove_prefix(end+1);
        if (managed_accounts.empty()) throw ApiError{400,"invalid_argument","Invalid managed account list"};
    }
    db.query(existing
        ? "UPDATE admin_account SET mailbox_origin='legacy',legacy_assessment='pending' WHERE mailbox_origin='unclassified'"
        : "UPDATE admin_account SET mailbox_origin='managed',legacy_assessment='not_required' WHERE mailbox_origin='unclassified'");
    transaction.commit();
}
void assess_legacy_accounts(Connection& db, const Config& config) {
    if (!config.legacy_mailbox_takeover) return;
    const auto rows=db.query("SELECT p.passwd_id,p.organization_id,p.home,p.maildir,p.uid,p.gid,a.legacy_assessment "
        "FROM passwd p JOIN admin_account a ON a.passwd_id=p.passwd_id JOIN organization o ON o.organization_id=p.organization_id "
        "WHERE a.mailbox_origin='legacy' AND a.legacy_assessment IN ('pending','intact') AND a.desired_active='yes' AND o.hosted_here='yes' ORDER BY p.passwd_id");
    // A durable abandoned classification is never undone by restart or an
    // accidental desired/effective-state mismatch. No filesystem cleanup is queued.
    const auto mismatches=db.query("SELECT a.passwd_id FROM admin_account a JOIN passwd p ON p.passwd_id=a.passwd_id "
        "WHERE a.legacy_assessment='abandoned' AND (a.desired_active<>'no' OR p.active<>'no')");
    if (!mismatches.empty()) {
        Transaction transaction{db};
        db.query("UPDATE passwd p JOIN admin_account a ON a.passwd_id=p.passwd_id SET p.active='no',a.desired_active='no' WHERE a.legacy_assessment='abandoned'");
        db.query("UPDATE admin_state SET generation=generation+1 WHERE singleton=1");
        transaction.commit();
    }
    if (rows.empty()) return;
    const auto current=legacy_spool_identity(config);
    auto root=db.query("SELECT legacy_spool_device,legacy_spool_inode FROM admin_state WHERE singleton=1").at(0);
    if (root[0].empty() && root[1].empty()) {
        db.query("UPDATE admin_state SET legacy_spool_device=?,legacy_spool_inode=? WHERE singleton=1",{current.device,current.inode});
        root={current.device,current.inode};
    }
    const SpoolIdentity expected{root[0],root[1]};
    if (current!=expected) throw ApiError{409,"legacy_spool_changed","Mailbox spool identity changed; verify its mount before assessment"};
    for (const auto& row : rows) {
        MailboxPlan mailbox; mailbox.id=row[0]; mailbox.organization_id=row[1]; mailbox.home=row[2]; mailbox.maildir=row[3];
        reconciliation_step({"legacy_assessment","passwd",row[0],"uid/gid",row[0],row[1]},[&] {
            if (row[4]!=std::to_string(geteuid()) || row[5]!=std::to_string(getegid()))
                throw ApiError{409,"mailbox_identity_conflict","Mailbox UID/GID differs from the daemon identity"};
        });
        const auto status=assess_legacy_mailbox(config,mailbox,expected);
        // Recheck the configured root before persisting any decision.
        if (legacy_spool_identity(config)!=expected)
            throw ApiError{409,"legacy_spool_changed","Mailbox spool identity changed; verify its mount before assessment"};
        if (status==LegacyMailboxStatus::intact && row[6]=="intact") continue;
        Transaction transaction{db};
        if (status==LegacyMailboxStatus::intact) {
            db.query("UPDATE admin_account SET legacy_assessment='intact',legacy_reason='' WHERE passwd_id=?",{mailbox.id});
        } else {
            db.query("UPDATE admin_account SET desired_active='no',legacy_assessment='abandoned',legacy_reason=? WHERE passwd_id=?",
                {std::string{legacy_reason(status)},mailbox.id});
            db.query("UPDATE passwd SET active='no' WHERE passwd_id=?",{mailbox.id});
            db.query("UPDATE admin_state SET generation=generation+1 WHERE singleton=1");
        }
        transaction.commit();
        MA_INFO << "Legacy mailbox assessed; passwd_id=" << diagnostic_id(mailbox.id)
            << " organization_id=" << diagnostic_id(mailbox.organization_id)
            << " assessment=" << (status==LegacyMailboxStatus::intact ? "intact" : "abandoned")
            << " reason=" << (status==LegacyMailboxStatus::intact ? "none" : legacy_reason(status));
    }
}
boost::json::array legacy_mailbox_report(Connection& db) {
    boost::json::array report;
    for (const auto& row : db.query("SELECT a.passwd_id,p.organization_id,a.legacy_assessment,a.legacy_reason,a.desired_active,p.active "
            "FROM admin_account a JOIN passwd p ON p.passwd_id=a.passwd_id WHERE a.mailbox_origin='legacy' ORDER BY a.passwd_id"))
        report.emplace_back(boost::json::object{{"passwd_id",identifier(row[0])},{"organization_id",identifier(row[1])},
            {"assessment",row[2]},{"reason",row[3]},{"desired_active",row[4]=="yes"},{"active",row[5]=="yes"}});
    return report;
}
}
