#include "LegacyMailbox.h"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace {
using namespace mailadmin;
void require(bool value, const std::source_location location=std::source_location::current()) {
    if (!value) { std::cerr << "Legacy mailbox assertion failed at line " << location.line() << '\n'; throw std::runtime_error{"Legacy mailbox test failed"}; }
}
template<typename Function> void rejects(Function function) { bool failed=false; try { function(); } catch (const ApiError&) { failed=true; } require(failed); }
void secure_directory(const std::filesystem::path& path) {
    std::filesystem::create_directory(path); std::filesystem::permissions(path,std::filesystem::perms::owner_all);
}
}
int main() {
    char temporary[]="/tmp/mailadmin-legacy-mailbox-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    const std::filesystem::path root{temporary};
    try {
        Config config; config.spool_root=root;
        const auto expected=legacy_spool_identity(config);
        MailboxPlan mailbox; mailbox.id="31"; mailbox.organization_id="6"; mailbox.address="abandoned@example.test";
        mailbox.home=(root/"6"/"31").string(); mailbox.maildir=mailbox.home+"/Maildir";
        require(assess_legacy_mailbox(config,mailbox,expected)==LegacyMailboxStatus::missing_home);
        require(!std::filesystem::exists(root/"6"));
        secure_directory(root/"6"); secure_directory(mailbox.home);
        require(assess_legacy_mailbox(config,mailbox,expected)==LegacyMailboxStatus::missing_maildir);
        require(!std::filesystem::exists(mailbox.maildir));
        std::ofstream{mailbox.maildir} << "PRIVATE remaining abandoned file";
        struct stat before{},after{}; require(lstat(mailbox.maildir.c_str(),&before)==0);
        require(assess_legacy_mailbox(config,mailbox,expected)==LegacyMailboxStatus::maildir_regular_file);
        require(assess_legacy_mailbox(config,mailbox,expected)==LegacyMailboxStatus::maildir_regular_file);
        require(lstat(mailbox.maildir.c_str(),&after)==0 && before.st_ino==after.st_ino && before.st_size==after.st_size);
        std::filesystem::remove(mailbox.maildir); // Test setup only.
        std::filesystem::create_directory_symlink(root/"missing-target",mailbox.maildir);
        rejects([&] { assess_legacy_mailbox(config,mailbox,expected); });
        require(std::filesystem::is_symlink(mailbox.maildir));
        std::filesystem::remove(mailbox.maildir);
        require(mkfifo(mailbox.maildir.c_str(),0600)==0);
        rejects([&] { assess_legacy_mailbox(config,mailbox,expected); });
        std::filesystem::remove(mailbox.maildir);
        secure_directory(mailbox.maildir);
        require(assess_legacy_mailbox(config,mailbox,expected)==LegacyMailboxStatus::intact);
        mailbox.create_mailbox=false; provision_mailbox(config,mailbox);
        require(std::filesystem::is_directory(mailbox.maildir+"/new"));
        std::filesystem::rename(mailbox.maildir,mailbox.home+"/saved-Maildir");
        rejects([&] { provision_mailbox(config,mailbox); });
        require(!std::filesystem::exists(mailbox.maildir));
        std::filesystem::rename(mailbox.home,root/"6"/"saved-home");
        rejects([&] { provision_mailbox(config,mailbox); });
        require(!std::filesystem::exists(mailbox.home));
        auto changed=expected; changed.inode="0";
        rejects([&] { assess_legacy_mailbox(config,mailbox,changed); });
        auto unavailable=config; unavailable.spool_root=root/"unavailable-mount";
        rejects([&] { legacy_spool_identity(unavailable); });
        require(!std::filesystem::exists(unavailable.spool_root));
        struct stat info{};
        for (const auto error : {EACCES,EPERM,EIO,ENODEV,ESTALE,ENOTDIR,ELOOP,ETIMEDOUT})
            rejects([&] { detail::legacy_entry_status(info,error,false); });
        // Insecure/inaccessible parents must not turn absence into abandonment.
        std::filesystem::permissions(root/"6",std::filesystem::perms::all);
        rejects([&] { assess_legacy_mailbox(config,mailbox,expected); });
        std::filesystem::permissions(root/"6",std::filesystem::perms::owner_all);
        if (geteuid()!=0) {
            std::filesystem::permissions(root/"6",std::filesystem::perms::none);
            rejects([&] { assess_legacy_mailbox(config,mailbox,expected); });
            std::filesystem::permissions(root/"6",std::filesystem::perms::owner_all);
        }
        std::filesystem::remove_all(root);
        std::cout << "Legacy filesystem assessment checks passed\n";
    } catch (...) {
        std::filesystem::permissions(root,std::filesystem::perms::owner_all);
        if (std::filesystem::exists(root/"6")) std::filesystem::permissions(root/"6",std::filesystem::perms::owner_all);
        std::filesystem::remove_all(root);
        std::cerr << "Legacy filesystem assessment failed; no input or mailbox contents printed\n";
        return 1;
    }
}
