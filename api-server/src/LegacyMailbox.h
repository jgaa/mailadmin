#pragma once
#include "Mailstore.h"
#include "Sql.h"
#include <sys/stat.h>
#include <string_view>

namespace mailadmin {
struct SpoolIdentity {
    std::string device, inode;
    bool operator==(const SpoolIdentity&) const = default;
};
enum class LegacyMailboxStatus { intact, missing_home, missing_maildir, maildir_regular_file };
std::string_view legacy_reason(LegacyMailboxStatus status);
SpoolIdentity legacy_spool_identity(const Config& config);
LegacyMailboxStatus assess_legacy_mailbox(const Config& config, const MailboxPlan& mailbox,
    const SpoolIdentity& expected);
void migrate_legacy_mailboxes(Connection& db, bool existing, std::string_view managed_accounts);
void assess_legacy_accounts(Connection& db, const Config& config);
boost::json::array legacy_mailbox_report(Connection& db);
namespace detail {
// Shared classification rule, separately testable for filesystem errno failures.
LegacyMailboxStatus legacy_entry_status(const struct stat& info, int error, bool home);
}
}
