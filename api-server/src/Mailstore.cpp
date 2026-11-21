#include "Mailstore.h"
#include "Administration.h"
#include "Security.h"
#include "SpamPolicy.h"
#include "LegacyMailbox.h"
#include <sys/statvfs.h>
#include "ReconciliationDiagnostics.h"
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <unistd.h>

namespace mailadmin {
namespace {
[[noreturn]] void filesystem_error() { throw ApiError{503,"filesystem_unavailable","Mailbox filesystem operation failed; check ownership, paths and disk space"}; }
struct DirectoryCloser { void operator()(DIR* value) const { closedir(value); } };
struct Fd {
    int value = -1;
    explicit Fd(int value) : value{value} { if (value<0) filesystem_error(); }
    Fd(Fd&& other) noexcept : value{other.value} { other.value=-1; }
    ~Fd() { if (value>=0) close(value); }
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
};
Fd directory(const std::filesystem::path& path) {
    Fd fd{open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC)};
    for (const auto& part : path.relative_path()) {
        if (part == "." || part == ".." || part.empty()) filesystem_error();
        Fd next{openat(fd.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
        std::swap(fd.value,next.value);
    }
    return fd;
}
void owned_directory(int fd) {
    struct stat info{};
    if (fstat(fd,&info) || info.st_uid!=geteuid() || (info.st_mode&0022)) filesystem_error();
}
Fd child(int parent, const std::string& name, bool create) {
    if (name.empty() || name=="." || name==".." || name.find('/')!=std::string::npos) filesystem_error();
    if (create && mkdirat(parent,name.c_str(),0700) && errno!=EEXIST) filesystem_error();
    Fd fd{openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    owned_directory(fd.value);
    return fd;
}
void sync_directory(int fd) { if (fsync(fd)) filesystem_error(); }
void atomic_write(int parent, const std::string& name, std::string_view data, mode_t mode=0600) {
    const auto temporary=".mailadmin-"+random_secret();
    Fd fd{openat(parent,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,mode)};
    try {
        while (!data.empty()) {
            const auto size=write(fd.value,data.data(),data.size());
            if (size<0 && errno==EINTR) continue;
            if (size<=0) filesystem_error();
            data.remove_prefix(size);
        }
        if (fsync(fd.value) || renameat(parent,temporary.c_str(),parent,name.c_str())) filesystem_error();
        sync_directory(parent);
    } catch (...) { unlinkat(parent,temporary.c_str(),0); throw; }
}
void quota(int messages, const std::string& definition) {
    quota_value(definition);
    const auto descriptor=openat(messages,"maildirsize",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (descriptor<0 && errno!=ENOENT) filesystem_error();
    if (descriptor<0 && definition.empty()) return;
    if (descriptor>=0) {
        Fd file{descriptor};
        struct stat info{};
        if (fstat(file.value,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid()) filesystem_error();
        char first_line[257];
        const auto count=read(file.value,first_line,sizeof(first_line));
        if (count<0) filesystem_error();
        const std::string_view content{first_line,static_cast<std::size_t>(count)};
        if (const auto newline=content.find('\n'); newline!=std::string_view::npos && content.substr(0,newline)==definition) return;
    }
    std::string invalidated=definition+"\n";
    while (invalidated.size()<5200) invalidated+="0 0\n";
    atomic_write(messages,"maildirsize",invalidated);
}
void maildir(int parent, const std::string& name, bool folder=false, bool create=true) {
    auto fd=child(parent,name,create);
    for (const auto& part : {"tmp","new","cur"}) { auto sub=child(fd.value,part,true); sync_directory(sub.value); }
    if (folder) {
        // Create missing metadata without replacing an existing legacy marker.
        const int marker=openat(fd.value,"maildirfolder",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (marker>=0) { Fd file{marker}; if (fsync(file.value)) filesystem_error(); }
        else {
            if (errno!=EEXIST) filesystem_error();
            struct stat info{};
            if (fstatat(fd.value,"maildirfolder",&info,AT_SYMLINK_NOFOLLOW) || !S_ISREG(info.st_mode)
                || info.st_uid!=geteuid() || (info.st_mode&0022)) filesystem_error();
        }
    }
    sync_directory(fd.value);
    sync_directory(parent);
}
void validate_path(const Config& config, const std::string& organization_id, const std::string& id, const std::string& home) {
    // IDs are validated independently even when the caller read them from DB.
    if (std::to_string(identifier(organization_id))!=organization_id || std::to_string(identifier(id))!=id
        || home!=(config.spool_root/organization_id/id).string())
        throw ApiError{409,"unsafe_mailbox_path","Mailbox path does not match its immutable organization/account identity"};
}
Fd mailbox_home(const Config& config, const MailboxPlan& mailbox, bool create, bool provisioning=false) {
    auto check = [&](std::string_view field, auto&& operation) -> decltype(auto) {
        return reconciliation_step({provisioning ? "provisioning" : "spam_cleanup","passwd",mailbox.id,
            field,mailbox.id,mailbox.organization_id,provisioning},std::forward<decltype(operation)>(operation));
    };
    check("home_identity",[&] { validate_path(config,mailbox.organization_id,mailbox.id,mailbox.home); });
    check("maildir",[&] {
        if (mailbox.maildir!=mailbox.home+"/Maildir") throw ApiError{409,"unsafe_mailbox_path","Unsupported legacy Maildir path"};
    });
    auto root=check("spool_root.open",[&] { return directory(config.spool_root); });
    check("spool_root.ownership",[&] { owned_directory(root.value); });
    auto organization=check("organization_directory",[&] { return child(root.value,mailbox.organization_id,create); });
    auto home=check("home_directory",[&] { return child(organization.value,mailbox.id,create); });
    check("home.sync",[&] { sync_directory(organization.value); sync_directory(root.value); });
    return home;
}
void erase_tree(int parent, const std::string& name, unsigned depth, std::size_t& budget) {
    if (!budget || depth>128) throw ApiError{503,"cleanup_pending","Cleanup batch limit reached; retrying remaining files"};
    --budget;
    struct stat info{};
    if (fstatat(parent,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)) { if (errno==ENOENT) return; filesystem_error(); }
    if (!S_ISDIR(info.st_mode)) {
        if (unlinkat(parent,name.c_str(),0) && errno!=ENOENT) filesystem_error();
        sync_directory(parent);
        return;
    }
    auto fd=child(parent,name,false);
    std::unique_ptr<DIR,DirectoryCloser> entries{fdopendir(dup(fd.value))};
    if (!entries) filesystem_error();
    while (auto* entry=readdir(entries.get())) {
        std::string item{entry->d_name};
        if (item=="." || item=="..") continue;
        erase_tree(fd.value,item,depth+1,budget);
    }
    sync_directory(fd.value);
    if (unlinkat(parent,name.c_str(),AT_REMOVEDIR) && errno!=ENOENT) filesystem_error();
    sync_directory(parent);
}
}
namespace {
void assess_access(int fd) {
    owned_directory(fd);
    struct statvfs filesystem{};
    if (faccessat(fd,".",R_OK|W_OK|X_OK,AT_EACCESS) || fstatvfs(fd,&filesystem)
        || (filesystem.f_flag&ST_RDONLY)) filesystem_error();
}
SpoolIdentity identity(int fd) {
    struct stat info{};
    if (fstat(fd,&info)) filesystem_error();
    return {std::to_string(info.st_dev),std::to_string(info.st_ino)};
}
LegacyMailboxStatus inspect(int parent, const std::string& name, bool home) {
    assess_access(parent);
    struct stat info{};
    const auto result=fstatat(parent,name.c_str(),&info,AT_SYMLINK_NOFOLLOW);
    const auto error=result ? errno : 0;
    return detail::legacy_entry_status(info,error,home);
}
}
namespace detail {
LegacyMailboxStatus legacy_entry_status(const struct stat& info, int error, bool home) {
    if (error==ENOENT) return home ? LegacyMailboxStatus::missing_home : LegacyMailboxStatus::missing_maildir;
    if (error) filesystem_error();
    if (S_ISDIR(info.st_mode)) return LegacyMailboxStatus::intact;
    if (!home && S_ISREG(info.st_mode)) return LegacyMailboxStatus::maildir_regular_file;
    throw ApiError{409,"unsafe_mailbox_path","Legacy mailbox contains an unexpected object or symlink"};
}
}
SpoolIdentity legacy_spool_identity(const Config& config) {
    auto root=directory(config.spool_root); assess_access(root.value); return identity(root.value);
}
LegacyMailboxStatus assess_legacy_mailbox(const Config& config, const MailboxPlan& mailbox,
        const SpoolIdentity& expected) {
    auto check=[&](std::string_view field, auto&& operation) -> decltype(auto) {
        return reconciliation_step({"legacy_assessment","passwd",mailbox.id,field,mailbox.id,mailbox.organization_id},
            std::forward<decltype(operation)>(operation));
    };
    check("home_identity",[&] { validate_path(config,mailbox.organization_id,mailbox.id,mailbox.home); });
    check("maildir",[&] {
        if (mailbox.maildir!=mailbox.home+"/Maildir")
            throw ApiError{409,"unsafe_mailbox_path","Unsupported legacy Maildir path"};
    });
    auto root=check("spool_root",[&] { auto value=directory(config.spool_root); assess_access(value.value); return value; });
    check("spool_identity",[&] {
        if (identity(root.value)!=expected)
            throw ApiError{409,"legacy_spool_changed","Mailbox spool identity changed; verify its mount before assessment"};
    });
    auto status=check("organization_directory",[&] { return inspect(root.value,mailbox.organization_id,true); });
    if (status!=LegacyMailboxStatus::intact) return status;
    auto organization=check("organization_directory",[&] { return child(root.value,mailbox.organization_id,false); });
    status=check("home",[&] { return inspect(organization.value,mailbox.id,true); });
    if (status!=LegacyMailboxStatus::intact) return status;
    auto home=check("home",[&] { return child(organization.value,mailbox.id,false); });
    status=check("maildir",[&] { return inspect(home.value,"Maildir",false); });
    if (status==LegacyMailboxStatus::intact) {
        auto messages=check("maildir_access",[&] { return child(home.value,"Maildir",false); });
        check("maildir_access",[&] { assess_access(messages.value); });
    }
    return status;
}
std::string render_filter(const MailboxPlan& mailbox) {
    const auto spam=resolve_spam_policy({mailbox.spam,mailbox.spam_folder,
        std::to_string(mailbox.retention_days),mailbox.spam_trash});
    const auto address=mailbox_address(mailbox.address);
    std::string result="# Managed by mailadmin; administrator-owned delivery policy\n";
    result+="SHELL='/bin/sh'\nSENDMAIL='/usr/sbin/sendmail'\n";
    if (spam.enabled) result+="if (/^X-Spam-Flag: *YES/)\n{\n  to './Maildir/."+spam.folder+"'\n}\n";
    // mailbot suppresses bulk/list messages, DSNs and repeated senders. It gets
    // the message through cc (not command substitution, which has no stdin).
    if (!mailbox.auto_reply.empty())
        result+="cc \"| /usr/bin/mailbot -t ./auto-reply.txt -c UTF-8 -d ./auto-reply.db -D 1 -A 'From: "
            +address+"' /usr/sbin/sendmail -t -f ''\"\n";
    if (!mailbox.forwards.empty()) {
        result += mailbox.keep_copy ? "cc '!" : "to '!";
        for (const auto& destination : mailbox.forwards) result+=mailbox_address(destination)+" ";
        result+="'\n";
    }
    result+="to './Maildir'\n";
    return result;
}
void provision_mailbox(const Config& config, const MailboxPlan& mailbox) {
    auto check = [&](std::string_view field, auto&& operation) -> decltype(auto) {
        return reconciliation_step({"provisioning","passwd",mailbox.id,field,mailbox.id,mailbox.organization_id,true},
            std::forward<decltype(operation)>(operation));
    };
    const auto spam=resolve_spam_policy({mailbox.spam,mailbox.spam_folder,
        std::to_string(mailbox.retention_days),mailbox.spam_trash}, {},
        {"provisioning","passwd",mailbox.id,{},mailbox.id,mailbox.organization_id,true});
    auto home=mailbox_home(config,mailbox,mailbox.create_mailbox,true);
    check("Maildir.create",[&] { maildir(home.value,"Maildir",false,mailbox.create_mailbox); });
    auto messages=check("Maildir.open",[&] { return child(home.value,"Maildir",false); });
    check("quota",[&] { quota(messages.value,mailbox.quota); });
    if (mailbox.filters && spam.enabled) {
        check("effective.spamfld",[&] { maildir(messages.value,"."+spam.folder,true); });
        if (!spam.trash.empty())
            check("effective.spamtrash",[&] { maildir(messages.value,"."+spam.trash,true); });
    }
    if (mailbox.filters) {
        if (!mailbox.auto_reply.empty()) check("auto_reply.write",[&] { atomic_write(home.value,"auto-reply.txt",mailbox.auto_reply); });
        const auto filter=check("delivery_filter.render",[&] { return render_filter(mailbox); });
        check("delivery_filter.write",[&] { atomic_write(home.value,".mailfilter",filter); });
        check("courier_delivery.write",[&] { atomic_write(home.value,".courier","| /usr/bin/spamc | /usr/bin/maildrop ./.mailfilter\n"); });
    } else {
        // An explicit opt-out preserves any manually maintained filter. Only
        // brand-new homes without .courier need a default delivery instruction.
        struct stat info{};
        if (fstatat(home.value,".courier",&info,AT_SYMLINK_NOFOLLOW) && errno==ENOENT)
            check("courier_delivery.write",[&] { atomic_write(home.value,".courier","./Maildir/\n"); });
    }
}
void remove_mailbox(const Config& config, const std::string& organization_id,
        const std::string& id, const std::string& home) {
    if (home.empty()) return; // Legacy disabled synthetic/unprovisioned account.
    validate_path(config,organization_id,id,home);
    auto root=directory(config.spool_root);
    owned_directory(root.value);
    struct stat info{};
    if (fstatat(root.value,organization_id.c_str(),&info,AT_SYMLINK_NOFOLLOW) && errno==ENOENT) return;
    auto organization=child(root.value,organization_id,false);
    std::size_t budget=10000;
    erase_tree(organization.value,id,0,budget);
}
void clean_spam(const Config& config, const MailboxPlan& mailbox) {
    if (!mailbox.filters || !mailbox.spam) return;
    const auto policy=resolve_spam_policy({mailbox.spam,mailbox.spam_folder,
        std::to_string(mailbox.retention_days),mailbox.spam_trash}, {},
        {"spam_cleanup","passwd",mailbox.id,{},mailbox.id,mailbox.organization_id});
    if (!mailbox.spam_cleanup_ready || !policy.cleanup_ready) return;
    auto home=mailbox_home(config,mailbox,false);
    auto messages=child(home.value,"Maildir",false);
    auto spam=child(messages.value,"."+policy.folder,false);
    const auto cutoff=std::time(nullptr)-static_cast<std::time_t>(policy.retention_days)*86400;
    std::size_t budget=10000;
    for (const auto& part : {"new","cur"}) {
        auto source=child(spam.value,part,false);
        std::unique_ptr<DIR,DirectoryCloser> entries{fdopendir(dup(source.value))};
        if (!entries) filesystem_error();
        while (auto* entry=readdir(entries.get())) {
            const std::string name{entry->d_name};
            if (name=="." || name=="..") continue;
            if (!budget--) return; // Continue in the next scheduled pass.
            struct stat info{};
            if (fstatat(source.value,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)) { if (errno==ENOENT) continue; filesystem_error(); }
            if (!S_ISREG(info.st_mode) || info.st_uid!=geteuid() || info.st_mtime>=cutoff) continue;
            if (policy.trash.empty()) {
                if (unlinkat(source.value,name.c_str(),0) && errno!=ENOENT) filesystem_error();
            } else {
                auto trash=child(messages.value,"."+policy.trash,false);
                auto destination=child(trash.value,"new",false);
                // Linux renameat2 moves atomically without replacing mail.
                // A crash/retry sees either the source or its destination.
                const auto destination_name="mailadmin-"+random_secret();
                if (syscall(SYS_renameat2,source.value,name.c_str(),destination.value,destination_name.c_str(),RENAME_NOREPLACE)) {
                    if (errno==ENOENT) continue; // An IMAP client moved it first.
                    filesystem_error();
                }
                sync_directory(destination.value);
            }
            sync_directory(source.value);
        }
    }
}
void publish_request(const Config& config, std::string_view content) {
    auto fd=directory(config.generated_directory);
    atomic_write(fd.value,"request.json",content,0640);
}
std::string publication_status(const Config& config) {
    auto directory_fd=directory(config.generated_directory);
    const auto descriptor=openat(directory_fd.value,"status.json",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (descriptor<0 && errno==ENOENT) return {};
    Fd fd{descriptor};
    struct stat info{};
    if (fstat(fd.value,&info) || !S_ISREG(info.st_mode) || info.st_uid!=0 || (info.st_mode&0022)
        || info.st_size>8192) filesystem_error();
    std::string result;
    char buffer[1024];
    for (;;) {
        const auto size=read(fd.value,buffer,sizeof(buffer));
        if (!size) break;
        if (size<0 && errno==EINTR) continue;
        if (size<0 || result.size()+size>8192) filesystem_error();
        result.append(buffer,size);
    }
    return result;
}
}
