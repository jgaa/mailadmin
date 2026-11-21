#include "SpamPolicy.h"
#include "Administration.h"
#include <charconv>

namespace mailadmin {
SpamPolicy resolve_spam_policy(const SpamSettings& account, const SpamSettings& organization,
        const ReconciliationLocation& location) {
    SpamPolicy policy;
    policy.enabled=account.enabled.value_or(organization.enabled.value_or(false));
    if (!policy.enabled) return policy; // Unused legacy values are not interpreted.
    const auto inherit=[](const auto& value, const auto& fallback) { return value ? value : fallback; };
    const auto folder=inherit(account.folder,organization.folder);
    const auto days=inherit(account.retention_days,organization.retention_days);
    const auto trash=inherit(account.trash,organization.trash);
    auto check=[&](std::string_view field, auto&& operation) -> decltype(auto) {
        if (location.stage.empty()) return operation();
        auto context=location; context.field=field;
        return reconciliation_step(context,std::forward<decltype(operation)>(operation));
    };
    policy.folder=check("effective.spamfld",[&] {
        return folder_name(!folder || folder->empty() ? "Spam" : *folder);
    });
    policy.trash=check("effective.spamtrash",[&] { return folder_name(trash.value_or("Trash"),true); });
    check("effective.spamdays",[&] {
        if (!days || days->empty()) return; // Missing legacy retention: safe delivery default.
        unsigned count=0;
        const auto [end,error]=std::from_chars(days->data(),days->data()+days->size(),count);
        if (error!=std::errc{} || end!=days->data()+days->size())
            throw ApiError{400,"invalid_argument","Invalid spam retention"};
        if (count<1 || count>36500)
            throw ApiError{409,"spam_policy_conflict","Invalid effective spam retention or folder policy"};
        policy.retention_days=count;
    });
    check("effective.spamfld/spamtrash",[&] {
        if (policy.folder==policy.trash)
            throw ApiError{409,"spam_policy_conflict","Spam and trash folders must differ"};
    });
    policy.cleanup_ready=folder && !folder->empty() && days && !days->empty() && trash.has_value();
    return policy;
}
}
