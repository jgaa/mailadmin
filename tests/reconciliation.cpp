#include "Administration.h"
#include "ReconciliationDiagnostics.h"
#include "ReconciliationPlan.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <source_location>
#include <unistd.h>

namespace {
using namespace mailadmin;
void require(bool condition, const std::source_location location=std::source_location::current()) {
    if (!condition) {
        std::cerr << "Reconciliation assertion failed at line " << location.line() << '\n';
        throw std::runtime_error{"Reconciliation regression failed"};
    }
}
std::ostringstream diagnostics;
void reset_log() { diagnostics.str(""); diagnostics.clear(); }
void logged(std::string_view expected, const std::source_location location=std::source_location::current()) {
    require(diagnostics.str().find(expected)!=std::string::npos,location);
}
void private_values_absent() {
    for (const auto value : {"PRIVATE", "Legacy.Spam", "Legacy.Trash", "invalid-quota-value", "private[address]@example.test"})
        require(diagnostics.str().find(value)==std::string::npos);
}
template<typename Function>
void api_failure(Function&& function, int status, std::string_view code, std::string_view message = {}) {
    bool failed=false;
    try { function(); }
    catch (const ApiError& error) {
        failed=true; require(error.status==status && error.code==code);
        if (!message.empty()) require(error.what()==message);
        require(std::string_view{error.what()}.find("passwd_id=")==std::string_view::npos);
    }
    require(failed); private_values_absent();
}
Rows no_forwards(const std::string&) { return {}; }
}
int main() {
    using namespace mailadmin;
    char name[]="/tmp/mailadmin-reconciliation-XXXXXX";
    if (!mkdtemp(name)) return 1;
    const std::filesystem::path root{name};
    logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(diagnostics,logfault::LogLevel::DEBUGGING));
    try {
        Config config; config.spool_root=root;
        auto row=std::vector<std::string>{"11","6","owner@example.test",(root/"6"/"11").string(),
            (root/"6"/"11"/"Maildir").string(),"","yes","yes","spam","Trash","30",
            "PRIVATE password, ma_PRIVATE_API_TOKEN and mailbox contents","no",std::to_string(geteuid()),std::to_string(getegid()),"0","managed","not_required"};
        require(detail::mailbox_plans({row},no_forwards).size()==1);
        auto invalid_field = [&](std::size_t column, std::string value, std::string_view field, int status, std::string_view code) {
            reset_log(); auto bad=row; bad[column]=std::move(value);
            // A valid earlier row must not be provisioned when a later legacy
            // row fails planning. This is the production all-plans-first boundary.
            bad[0]="12";
            api_failure([&] {
                const auto mailboxes=detail::mailbox_plans({row,bad},no_forwards);
                for (const auto& mailbox : mailboxes) provision_mailbox(config,mailbox);
            },status,code);
            logged("stage=planning"); logged("passwd_id=12"); logged("organization_id=6");
            logged(std::string{"field="}+std::string{field}); logged("provisioning_started=no");
            require(!std::filesystem::exists(root/"6"));
        };
        invalid_field(2,"private[address]@example.test","id",400,"invalid_argument");
        invalid_field(8,"Legacy.Spam","effective.spamfld",400,"invalid_argument");
        invalid_field(9,"Legacy.Trash","effective.spamtrash",400,"invalid_argument");
        invalid_field(10,"0","effective.spamdays",409,"spam_policy_conflict");
        invalid_field(9,"spam","effective.spamfld/spamtrash",409,"spam_policy_conflict");
        invalid_field(13,std::to_string(geteuid()+1),"uid/gid",409,"mailbox_identity_conflict");
        // Dormant legacy policies must not block planning, even if incomplete.
        auto disabled=row; disabled[7]="no"; disabled[8]="Legacy.Spam";
        disabled[9]="../PRIVATE"; disabled[10]="PRIVATE-not-a-number";
        reset_log(); require(detail::mailbox_plans({disabled},no_forwards).size()==1);
        require(diagnostics.str().empty());
        reset_log();
        api_failure([&] { detail::mailbox_plans({row},[](const std::string&) { return Rows{{"73","private[address]@example.test"}}; }); },400,"invalid_argument","Unsupported local part in mail address");
        logged("record=forward record_id=73"); logged("passwd_id=11"); logged("field=destination");
        for (const auto& value : {"PRIVATE-not-a-number","999999999999999999999999999999"}) {
            auto bad=row; bad[10]=value; reset_log();
            api_failure([&] { detail::mailbox_plans({bad},no_forwards); },400,"invalid_argument");
            logged("field=effective.spamdays"); logged("code=invalid_argument");
        }
        reset_log();
        api_failure([&] {
            reconciliation_step({"routing","alias","91","mailaddr","11","6",true},[] {
                throw ApiError{409,"fixture_conflict","PRIVATE exception text must not be logged"};
            });
        },409,"fixture_conflict","PRIVATE exception text must not be logged");
        logged("record=alias record_id=91"); logged("code=fixture_conflict");
        auto mailbox=detail::mailbox_plans({row},no_forwards).front();
        auto bad=mailbox; bad.home="PRIVATE-unsafe-home"; reset_log();
        api_failure([&] { provision_mailbox(config,bad); },409,"unsafe_mailbox_path");
        logged("stage=provisioning"); logged("field=home_identity"); require(!std::filesystem::exists(root/"6"));
        bad=mailbox; bad.maildir="PRIVATE-unsafe-maildir"; reset_log();
        api_failure([&] { provision_mailbox(config,bad); },409,"unsafe_mailbox_path","Unsupported legacy Maildir path");
        logged("field=maildir"); require(!std::filesystem::exists(root/"6"));
        bad=mailbox; bad.id="PRIVATE\nforged-log"; reset_log();
        api_failure([&] { provision_mailbox(config,bad); },409,"inventory_conflict");
        logged("record_id=invalid"); logged("passwd_id=invalid");
        // Quota validation originally occurs after creation of the Maildir.
        // Keep that ordering and show why the old code-only log was ambiguous.
        auto quota_row=row; quota_row[5]="invalid-quota-value";
        bad=detail::mailbox_plans({quota_row},no_forwards).front(); reset_log();
        api_failure([&] { provision_mailbox(config,bad); },400,"invalid_argument","Invalid Courier quota");
        logged("stage=provisioning"); logged("field=quota"); logged("provisioning_started=yes");
        require(std::filesystem::is_directory(bad.maildir+"/new"));
        require(!std::filesystem::exists(bad.home+"/.mailfilter"));
        require(!std::filesystem::exists(root/"request.json"));
        std::filesystem::remove_all(root/"6");
        const auto outside=root/"outside"; std::filesystem::create_directory(outside);
        std::ofstream{outside/"keep"} << "PRIVATE mailbox contents";
        std::filesystem::create_directory(root/"6");
        std::filesystem::permissions(root/"6",std::filesystem::perms::owner_all);
        std::filesystem::create_directory_symlink(outside,root/"6"/"11");
        reset_log(); api_failure([&] { provision_mailbox(config,mailbox); },503,"filesystem_unavailable");
        logged("field=home_directory"); logged("passwd_id=11");
        require(std::filesystem::exists(outside/"keep") && !std::filesystem::exists(outside/"Maildir"));
        logfault::LogManager::Instance().ClearHandlers();
        std::filesystem::remove_all(root);
        std::cout << "Legacy reconciliation diagnostics and provisioning boundary checks passed\n";
    } catch (...) {
        logfault::LogManager::Instance().ClearHandlers();
        std::filesystem::remove_all(root);
        std::cerr << "Reconciliation regression failed; no captured logs or input values printed\n";
        return 1;
    }
}
