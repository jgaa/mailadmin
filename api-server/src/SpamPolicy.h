#pragma once
#include "ReconciliationDiagnostics.h"
#include <optional>
#include <string>

namespace mailadmin {
// Missing account values inherit independently. An explicit empty folder
// overrides inheritance and requests the effective default when enabled.
struct SpamSettings {
    std::optional<bool> enabled;
    std::optional<std::string> folder, retention_days, trash;
};
struct SpamPolicy {
    bool enabled = false;
    std::string folder, trash;
    unsigned retention_days = 30;
    // Defaults authorize safe delivery/provisioning, not destructive cleanup
    // of an implicitly selected existing legacy folder.
    bool cleanup_ready = false;
};
SpamPolicy resolve_spam_policy(const SpamSettings& account, const SpamSettings& organization = {},
    const ReconciliationLocation& location = {});
}
