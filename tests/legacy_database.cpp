#include "Administration.h"
#include "LegacyMailbox.h"
#include "logging.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace {
using namespace mailadmin;
void require(bool condition, const std::source_location location=std::source_location::current()) {
    if (!condition) { std::cerr << "Legacy DB assertion failed at line " << location.line() << '\n'; throw std::runtime_error{"Legacy DB regression failed"}; }
}
std::string read(const std::filesystem::path& path) { std::ifstream f{path}; return {std::istreambuf_iterator<char>{f},{}}; }
void directory(const std::filesystem::path& path) { std::filesystem::create_directory(path); std::filesystem::permissions(path,std::filesystem::perms::owner_all); }
Operation request(const Config& config, yahat::Request::Type method, std::string path, std::string body={}) {
    boost::asio::io_context io;
    auto future=boost::asio::co_spawn(io,administer(std::make_shared<Config>(config),method,std::move(path),std::move(body)),boost::asio::use_future);
    io.run(); return future.get();
}
template<typename Function> void rejects(Function function, std::string_view code) {
    bool failed=false; try { function(); } catch (const ApiError& error) { require(error.code==code); failed=true; } require(failed);
}
}
int main(int argc, char** argv) {
    if (argc!=2) return 2;
    char temporary[]="/tmp/mailadmin-legacy-integration-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    const std::filesystem::path root{temporary};
    if (mysql_library_init(0,nullptr,nullptr)) return 1;
    std::string_view phase="setup";
    logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog,logfault::LogLevel::INFO));
    try {
        Config config; config.database_socket=argv[1]; config.database_name="mailadmin_legacy_test";
        config.database_user="root"; config.administration=true; config.spool_root=root/"spool"; config.generated_directory=root/"generated";
        directory(config.spool_root); directory(config.generated_directory); directory(config.spool_root/"6");
        Connection db{config};
        db.query("INSERT INTO organization(organization_id,name,default_domain,inbound_mailserver,outbound_mailserver) VALUES(6,'Legacy','example.test','','')");
        for (const auto id : {"31","32","33","34","35"}) {
            const auto home=(config.spool_root/"6"/id).string();
            db.query("INSERT INTO passwd(passwd_id,id,crypt,clear,uid,gid,organization_id,home,maildir) VALUES(?,?,?,?,?,?,?,?,?)",
                {id,std::string{"owner"}+id+"@example.test","PRIVATE-HASH","PRIVATE-CLEAR",std::to_string(geteuid()),std::to_string(getegid()),"6",home,home+"/Maildir"});
        }
        const auto home31=config.spool_root/"6"/"31"; directory(home31);
        std::ofstream{home31/"Maildir"} << "PRIVATE abandoned Maildir file";
        struct stat before{}; require(lstat((home31/"Maildir").c_str(),&before)==0);
        directory(config.spool_root/"6"/"32"); directory(config.spool_root/"6"/"32"/"Maildir");
        directory(config.spool_root/"6"/"34");
        db.query("INSERT INTO alias(mailaddr,passwd_id,organization_id) VALUES('abandoned@example.test',31,6),('valid@example.test',32,6)");
        db.query("INSERT INTO forward(passwd_id,destination) VALUES(31,'retained@example.test')");
        // Exact pre-upgrade admin_account schema: imported rows have no origin metadata.
        db.query("CREATE TABLE admin_account(passwd_id INT PRIMARY KEY,desired_active ENUM('yes','no') NOT NULL,FOREIGN KEY(passwd_id) REFERENCES passwd(passwd_id) ON DELETE CASCADE) ENGINE=InnoDB");
        db.query("INSERT INTO admin_account SELECT passwd_id,'yes' FROM passwd");
        const auto private_fields=db.query("SELECT crypt,clear,home,maildir,quota FROM passwd WHERE passwd_id=31");
        phase="migration";
        migrate_schema(config,"35"); // Operator identifies a pre-upgrade Mailadmin-created pending account.
        migrate_schema(config,"35");
        require(db.query("SELECT mailbox_origin,legacy_assessment FROM admin_account WHERE passwd_id=31")==Rows{{"legacy","pending"}});
        require(db.query("SELECT mailbox_origin,legacy_assessment FROM admin_account WHERE passwd_id=35")==Rows{{"managed","not_required"}});
        phase="first_reconciliation";
        reconcile(config,false);
        require(db.query("SELECT desired_active,legacy_assessment,legacy_reason FROM admin_account WHERE passwd_id=31")==Rows{{"no","abandoned","maildir_regular_file"}});
        require(db.query("SELECT active FROM passwd WHERE passwd_id=31")==Rows{{"no"}});
        require(db.query("SELECT crypt,clear,home,maildir,quota FROM passwd WHERE passwd_id=31")==private_fields);
        require(db.query("SELECT destination FROM forward WHERE passwd_id=31")==Rows{{"retained@example.test"}});
        require(db.query("SELECT COUNT(*) FROM alias WHERE passwd_id=31")==Rows{{"1"}});
        require(db.query("SELECT COUNT(*) FROM admin_cleanup")==Rows{{"0"}});
        require(db.query("SELECT legacy_reason FROM admin_account WHERE passwd_id=33")==Rows{{"missing_home"}});
        require(db.query("SELECT legacy_reason FROM admin_account WHERE passwd_id=34")==Rows{{"missing_maildir"}});
        require(!std::filesystem::exists(config.spool_root/"6"/"33") && !std::filesystem::exists(config.spool_root/"6"/"34"/"Maildir"));
        require(std::filesystem::is_directory(config.spool_root/"6"/"32"/"Maildir"/"new"));
        require(std::filesystem::is_directory(config.spool_root/"6"/"35"/"Maildir"/"new"));
        const auto published=boost::json::parse(read(config.generated_directory/"request.json")).as_object();
        require(published.at("aliases").as_array().size()==1);
        require(published.at("aliases").as_array()[0].at("address")=="valid@example.test");
        rejects([&] { migrate_schema(config,"31"); },"legacy_assessment_conflict");
        directory(config.spool_root/"6"/"33"); directory(config.spool_root/"6"/"33"/"Maildir");
        const auto generation=db.query("SELECT generation FROM admin_state WHERE singleton=1");
        reconcile(config,false); migrate_schema(config); reconcile(config,false);
        require(db.query("SELECT generation FROM admin_state WHERE singleton=1")==generation);
        require(db.query("SELECT desired_active,legacy_assessment FROM admin_account WHERE passwd_id=33")==Rows{{"no","abandoned"}});
        require(!std::filesystem::exists(config.spool_root/"6"/"33"/"Maildir"/"new"));
        require(db.query("SELECT legacy_assessment,legacy_reason FROM admin_account WHERE passwd_id=31")==Rows{{"abandoned","maildir_regular_file"}});
        struct stat after{}; require(lstat((home31/"Maildir").c_str(),&after)==0 && before.st_ino==after.st_ino
            && before.st_size==after.st_size && before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec);
        require(read(home31/"Maildir")=="PRIVATE abandoned Maildir file");
        phase="api_report";
        const auto report=request(config,yahat::Request::Type::GET,"/api/v1/operations");
        const auto serialized=boost::json::serialize(report.body);
        require(serialized.find("PRIVATE")==std::string::npos && serialized.find(root.string())==std::string::npos);
        require(report.body.at("legacy_mailboxes").as_array().size()==4);
        require(report.body.at("legacy_mailboxes").as_array()[0].at("reason")=="maildir_regular_file");
        rejects([&] { request(config,yahat::Request::Type::PATCH,"/api/v1/accounts/31",R"({"active":true})"); },"legacy_mailbox_abandoned");
        phase="api_create";
        // Even an accidental old-writer mismatch cannot undo the durable disable.
        db.query("UPDATE admin_account a JOIN passwd p ON p.passwd_id=a.passwd_id SET a.desired_active='yes',p.active='yes' WHERE p.passwd_id=31");
        reconcile(config,false);
        require(db.query("SELECT a.desired_active,p.active FROM admin_account a JOIN passwd p USING(passwd_id) WHERE passwd_id=31")==Rows{{"no","no"}});
        const auto created=request(config,yahat::Request::Type::POST,"/api/v1/accounts",R"({"address":"new@example.test","password":"PRIVATE test account password"})");
        const auto created_id=std::to_string(created.body.at("account").at("id").as_int64());
        require(db.query("SELECT mailbox_origin FROM admin_account WHERE passwd_id=?",{created_id})==Rows{{"managed"}});
        reconcile(config,false);
        require(std::filesystem::is_directory(config.spool_root/"6"/created_id/"Maildir"/"new"));
        std::filesystem::remove_all(config.spool_root/"6"/created_id); // Fixture: managed creation must remain resumable.
        reconcile(config,false);
        require(std::filesystem::is_directory(config.spool_root/"6"/created_id/"Maildir"/"new"));
        // A previously intact legacy account is disabled, never recreated, after confirmed disappearance.
        std::filesystem::rename(config.spool_root/"6"/"32"/"Maildir",config.spool_root/"6"/"32"/"saved-Maildir");
        reconcile(config,false);
        require(db.query("SELECT legacy_assessment,legacy_reason,desired_active FROM admin_account WHERE passwd_id=32")==Rows{{"abandoned","missing_maildir","no"}});
        require(!std::filesystem::exists(config.spool_root/"6"/"32"/"Maildir"));
        std::filesystem::rename(config.spool_root/"6"/"32"/"saved-Maildir",config.spool_root/"6"/"32"/"Maildir");
        // Suspicious symlinks leave authentication/desired state unchanged, not abandoned.
        const auto home99=(config.spool_root/"6"/"99").string(); directory(home99);
        std::filesystem::create_directory_symlink(root/"unavailable",home99+"/Maildir");
        db.query("INSERT INTO passwd(passwd_id,id,uid,gid,organization_id,home,maildir) VALUES(99,'uncertain@example.test',?,?,6,?,?)",{std::to_string(geteuid()),std::to_string(getegid()),home99,home99+"/Maildir"});
        db.query("INSERT INTO admin_account(passwd_id,desired_active,mailbox_origin) VALUES(99,'yes','legacy')");
        rejects([&] { reconcile(config,false); },"unsafe_mailbox_path");
        require(db.query("SELECT a.desired_active,p.active,a.legacy_assessment FROM passwd p JOIN admin_account a USING(passwd_id) WHERE passwd_id=99")==Rows{{"yes","yes","pending"}});
        const auto identity=db.query("SELECT legacy_spool_device,legacy_spool_inode FROM admin_state WHERE singleton=1")[0];
        db.query("UPDATE admin_state SET legacy_spool_inode='0' WHERE singleton=1");
        rejects([&] { reconcile(config,false); },"legacy_spool_changed");
        require(db.query("SELECT legacy_assessment FROM admin_account WHERE passwd_id=99")==Rows{{"pending"}});
        db.query("UPDATE admin_state SET legacy_spool_inode=? WHERE singleton=1",{identity[1]});
        std::filesystem::remove_all(root);
        std::cout << "Legacy takeover migration, atomic disable, routing continuation, API report and recovery checks passed\n";
    } catch (const ApiError& error) {
        std::filesystem::remove_all(root);
        std::cerr << "Legacy database failure; stage=" << phase << " code=" << error.code << '\n';
        return 1;
    } catch (...) {
        std::filesystem::remove_all(root);
        std::cerr << "Legacy database unexpected failure; stage=" << phase << '\n';
        std::cerr << "Legacy database regression failed; no SQL, credentials or mailbox content printed\n";
        return 1;
    }
}
