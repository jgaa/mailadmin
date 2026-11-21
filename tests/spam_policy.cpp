#include "SpamPolicy.h"
#include "Mailstore.h"
#include "ReconciliationPlan.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace mailadmin;
void require(bool condition, const std::source_location location=std::source_location::current()) {
    if (!condition) { std::cerr << "Spam policy assertion failed at line " << location.line() << '\n'; throw std::runtime_error{"Spam policy regression failed"}; }
}
template<typename Function> void rejects(Function function) {
    bool failed=false;
    try { function(); } catch (const ApiError&) { failed=true; }
    require(failed);
}
std::string read(const std::filesystem::path& path) { std::ifstream input{path}; return {std::istreambuf_iterator<char>{input},{}}; }
struct stat metadata(const std::filesystem::path& path) { struct stat info{}; require(lstat(path.c_str(),&info)==0); return info; }
void unchanged(const std::filesystem::path& path, const struct stat& before) {
    const auto after=metadata(path);
    require(before.st_ino==after.st_ino && before.st_size==after.st_size && before.st_mode==after.st_mode
        && before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec);
}
MailboxPlan mailbox(const Config& config, std::string id, bool enabled=true) {
    MailboxPlan result; result.id=std::move(id); result.organization_id="93";
    result.home=(config.spool_root/"93"/result.id).string(); result.maildir=result.home+"/Maildir";
    result.address="owner@example.test"; result.spam=enabled; result.spam_trash="Trash";
    return result;
}
}
int main() {
    using namespace mailadmin;
    char temporary[]="/tmp/mailadmin-spam-policy-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    const std::filesystem::path root{temporary};
    try {
        const SpamSettings organization{true,"","30","Trash"};
        const auto inherited=resolve_spam_policy({},organization);
        require(inherited.enabled && inherited.folder=="Spam" && !inherited.cleanup_ready);
        const SpamSettings custom{true,"Custom_Junk","7","Archive"};
        const auto overridden=resolve_spam_policy({{},"",{},{}},custom);
        require(overridden.folder=="Spam" && overridden.retention_days==7 && overridden.trash=="Archive");
        const auto inherited_custom=resolve_spam_policy({},custom);
        require(inherited_custom.folder=="Custom_Junk" && inherited_custom.cleanup_ready);
        const auto own_custom=resolve_spam_policy({{},"Own_Junk","12",""},custom);
        require(own_custom.folder=="Own_Junk" && own_custom.retention_days==12 && own_custom.trash.empty() && own_custom.cleanup_ready);
        const auto disabled=resolve_spam_policy({false,{},{},{}},custom);
        require(!disabled.enabled && disabled.folder.empty());
        require(!resolve_spam_policy({false,"../unsafe","not a number","../unsafe"}).enabled);
        require(resolve_spam_policy({true,"","",""}).retention_days==30);
        const auto missing=resolve_spam_policy({true,{},{},{}});
        require(missing.folder=="Spam" && missing.trash=="Trash" && missing.retention_days==30 && !missing.cleanup_ready);
        for (const auto& folder : {"../unsafe",".Spam","folder with space","folder/name"})
            rejects([&] { resolve_spam_policy({true,folder,"30","Trash"}); });
        rejects([] { resolve_spam_policy({true,"Spam","30","../unsafe"}); });
        rejects([] { resolve_spam_policy({true,"","30","Spam"}); });
        for (const auto& days : {"0","-1","36501","30junk"," 30"})
            rejects([&] { resolve_spam_policy({true,"Spam",days,"Trash"}); });
        Config config; config.spool_root=root;
        const auto legacy=mailbox(config,"867");
        const Rows rows{{legacy.id,legacy.organization_id,legacy.address,legacy.home,legacy.maildir,"","yes","yes",
            "","Trash","","","no",std::to_string(geteuid()),std::to_string(getegid()),"0"}};
        auto fallback=detail::mailbox_plans(rows,[](const std::string&) { return Rows{}; }).front();
        auto missing_trash=rows; missing_trash[0][8]="Custom_Junk"; missing_trash[0][10]="30"; missing_trash[0][15]="1";
        const auto missing_trash_plan=detail::mailbox_plans(missing_trash,[](const std::string&) { return Rows{}; }).front();
        require(missing_trash_plan.spam_trash=="Trash" && !missing_trash_plan.spam_cleanup_ready);
        require(fallback.spam_folder=="Spam" && fallback.retention_days==30 && !fallback.spam_cleanup_ready);
        provision_mailbox(config,fallback);
        const auto folder=std::filesystem::path{fallback.maildir}/".Spam";
        for (const auto& child : {"tmp","new","cur"}) {
            require(std::filesystem::is_directory(folder/child));
            require((metadata(folder/child).st_mode&0777)==0700);
        }
        require((metadata(folder/"maildirfolder").st_mode&0777)==0600);
        require(read(fallback.home+"/.mailfilter").find("./Maildir/.Spam")!=std::string::npos);
        // Reuse a legacy marker and messages without replacement, renaming or cleanup.
        std::ofstream{folder/"maildirfolder"} << "legacy marker";
        std::ofstream{folder/"new"/"existing-message"} << "PRIVATE legacy mail content";
        const timespec old[2]{{1,0},{1,0}};
        require(utimensat(AT_FDCWD,(folder/"new"/"existing-message").c_str(),old,0)==0);
        const auto marker_before=metadata(folder/"maildirfolder");
        const auto message_before=metadata(folder/"new"/"existing-message");
        const auto folder_before=metadata(folder), new_before=metadata(folder/"new");
        provision_mailbox(config,fallback);
        provision_mailbox(config,fallback);
        clean_spam(config,fallback); // Implicit folder default does not authorize old-mail removal.
        unchanged(folder,folder_before); unchanged(folder/"new",new_before);
        unchanged(folder/"maildirfolder",marker_before); unchanged(folder/"new"/"existing-message",message_before);
        require(read(folder/"maildirfolder")=="legacy marker");
        require(read(folder/"new"/"existing-message")=="PRIVATE legacy mail content");
        auto customized=mailbox(config,"868"); customized.spam_folder="Custom_Junk";
        provision_mailbox(config,customized);
        require(std::filesystem::is_directory(customized.maildir+"/.Custom_Junk/new"));
        require(!std::filesystem::exists(customized.maildir+"/.Spam"));
        const auto custom_message=std::filesystem::path{customized.maildir}/".Custom_Junk/new/keep";
        std::ofstream{custom_message} << "PRIVATE custom-folder mail";
        const auto custom_before=metadata(custom_message);
        provision_mailbox(config,customized);
        unchanged(custom_message,custom_before);
        require(read(custom_message)=="PRIVATE custom-folder mail");
        auto direct=mailbox(config,"873");
        provision_mailbox(config,direct);
        require(std::filesystem::is_directory(direct.maildir+"/.Spam/new"));
        auto off=mailbox(config,"869",false); off.spam_trash="../unused"; off.retention_days=0;
        provision_mailbox(config,off); clean_spam(config,off);
        require(!std::filesystem::exists(off.maildir+"/.Spam") && !std::filesystem::exists(off.maildir+"/.Trash"));
        require(read(off.home+"/.mailfilter").find("X-Spam-Flag")==std::string::npos);
        auto unmanaged=mailbox(config,"870"); unmanaged.filters=false;
        provision_mailbox(config,unmanaged);
        require(!std::filesystem::exists(unmanaged.maildir+"/.Spam"));
        auto unsafe=mailbox(config,"871"); unsafe.spam_folder="../escape";
        rejects([&] { provision_mailbox(config,unsafe); });
        require(!std::filesystem::exists(unsafe.home));
        auto symlink=mailbox(config,"872",false); provision_mailbox(config,symlink);
        const auto outside=root/"outside"; std::filesystem::create_directory(outside);
        std::ofstream{outside/"keep"} << "PRIVATE untouched";
        std::filesystem::create_directory_symlink(outside,symlink.maildir+"/.Spam");
        symlink.spam=true;
        rejects([&] { provision_mailbox(config,symlink); });
        require(read(outside/"keep")=="PRIVATE untouched" && !std::filesystem::exists(outside/"new"));
        std::filesystem::remove(folder/"maildirfolder"); // Fixture: unsafe existing marker must not be replaced/followed.
        std::filesystem::create_symlink(outside/"keep",folder/"maildirfolder");
        rejects([&] { provision_mailbox(config,fallback); });
        require(read(outside/"keep")=="PRIVATE untouched");
        std::filesystem::remove_all(root);
        std::cout << "Legacy spam defaults, inheritance, conditional validation and filesystem preservation checks passed\n";
    } catch (...) {
        std::filesystem::remove_all(root);
        std::cerr << "Spam policy regression failed; no input or mailbox content printed\n";
        return 1;
    }
}
