#include "Administration.h"
#include "Mailstore.h"
#include "Security.h"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool value) { if (!value) throw std::runtime_error{"Mailstore regression failed"}; }
template<typename Function> void rejects(Function function) {
    bool failed=false;
    try { function(); } catch (const std::exception&) { failed=true; }
    require(failed);
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input},{}};
}
}
int main() {
    char temporary[]="/tmp/mailadmin-mailstore-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{temporary};
    try {
        using namespace mailadmin;
        Config config;
        config.spool_root=cleanup.path;
        MailboxPlan mailbox;
        mailbox.id="2"; mailbox.organization_id="1"; mailbox.address="owner@example.test";
        mailbox.home=(cleanup.path/"1"/"2").string(); mailbox.maildir=mailbox.home+"/Maildir";
        mailbox.quota="10000000S,1000C";
        mailbox.spam=true; mailbox.spam_folder="spam"; mailbox.spam_trash="Trash";
        mailbox.auto_reply="I am away.\n"; mailbox.forwards={"another@example.test"}; mailbox.keep_copy=true;
        provision_mailbox(config,mailbox);
        provision_mailbox(config,mailbox); // Crash/retry provisioning does not lose messages.
        require(std::filesystem::is_directory(mailbox.maildir+"/new"));
        require(read(mailbox.home+"/.courier").find("maildrop ./.mailfilter")!=std::string::npos);
        require(read(mailbox.maildir+"/maildirsize").starts_with(mailbox.quota+"\n"));
        require(std::filesystem::file_size(mailbox.maildir+"/maildirsize")>5120);
        const auto original=mailbox.maildir+"/.spam/new/message";
        std::ofstream{original} << "Message-ID: test\n\nspam\n";
        const timespec old[2]{{1,0},{1,0}};
        require(utimensat(AT_FDCWD,original.c_str(),old,0)==0);
        clean_spam(config,mailbox);
        clean_spam(config,mailbox);
        require(!std::filesystem::exists(original));
        unsigned messages=0;
        for (const auto& entry : std::filesystem::directory_iterator(mailbox.maildir+"/.Trash/new")) {
            ++messages;
            require(read(entry.path()).find("spam")!=std::string::npos);
        }
        require(messages==1);
        mailbox.spam_trash="";
        std::ofstream{original} << "expired spam\n";
        require(utimensat(AT_FDCWD,original.c_str(),old,0)==0);
        clean_spam(config,mailbox);
        require(!std::filesystem::exists(original));
        mailbox.spam_trash="Trash";
        // Cleanup does not follow a symlink into another mailbox/directory.
        const auto outside=cleanup.path/"outside";
        std::filesystem::create_directory(outside);
        std::ofstream{outside/"keep"} << "keep";
        std::filesystem::create_directory_symlink(outside,mailbox.home+"/escape");
        remove_mailbox(config,"1","2",mailbox.home);
        remove_mailbox(config,"1","2",mailbox.home);
        require(read(outside/"keep")=="keep");
        rejects([&] { remove_mailbox(config,"1","2",outside.string()); });
        std::filesystem::create_directory_symlink(outside,cleanup.path/"1"/"2");
        rejects([&] { provision_mailbox(config,mailbox); });
        require(read(outside/"keep")=="keep");
        rejects([&] { folder_name("../outside"); });
        rejects([&] { mailbox_address("-option@example.test"); });
        rejects([&] { mailbox_address("x';touch x@example.test"); });
        require(mailbox_address("Owner@EXAMPLE.TEST")=="owner@example.test");
        const auto hash=password_hash("a secure test password");
        require(hash.starts_with("$6$rounds=100000$"));
        require(password_matches("a secure test password",hash));
        require(!password_matches("wrong password",hash));
        require(hash!=password_hash("a secure test password"));
        const auto filter=render_filter(mailbox);
        require(filter.find("SHELL='/bin/sh'")!=std::string::npos);
        require(filter.find("cc '!another@example.test '")!=std::string::npos);
        require(filter.find("cc \"| /usr/bin/mailbot")!=std::string::npos);
        std::cout << "Mailstore/password regression checks passed\n";
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Mailstore/password regression checks failed\n";
        return 1;
    }
}
