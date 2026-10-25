#pragma once
#include "Config.h"
#include <string>
#include <vector>

namespace mailadmin {
struct MailboxPlan {
    std::string id, organization_id, address, home, maildir, quota;
    bool filters = true, spam = false, keep_copy = false;
    std::string spam_folder, spam_trash, auto_reply;
    unsigned retention_days = 30;
    std::vector<std::string> forwards;
};
void provision_mailbox(const Config& config, const MailboxPlan& mailbox);
void remove_mailbox(const Config& config, const std::string& organization_id,
    const std::string& id, const std::string& home);
void clean_spam(const Config& config, const MailboxPlan& mailbox);
void publish_request(const Config& config, std::string_view content);
std::string publication_status(const Config& config);
std::string render_filter(const MailboxPlan& mailbox);
}
