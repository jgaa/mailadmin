#pragma once
#include "Mailstore.h"
#include "Sql.h"
#include <functional>

namespace mailadmin::detail {
// The production row decoder is separate from database I/O so invalid legacy
// rows can be exercised without a running server or any mailbox side effects.
std::vector<MailboxPlan> mailbox_plans(const Rows& rows,
    const std::function<Rows(const std::string&)>& forwards);
}
