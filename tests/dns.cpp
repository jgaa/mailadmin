#include "Dns.h"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <functional>
#include <iostream>

namespace {
void require(bool condition) { if (!condition) throw std::runtime_error{"DNS configuration check failed"}; }
void rejects(const std::function<void()>& action) {
    bool rejected = false;
    try { action(); } catch (const mailadmin::ApiError&) { rejected = true; }
    require(rejected);
}
std::string encode(EVP_PKEY* key, int (*writer)(const EVP_PKEY*,unsigned char**)) {
    const auto size = writer(key,nullptr);
    require(size > 0);
    std::vector<unsigned char> bytes(size);
    auto* cursor = bytes.data();
    require(writer(key,&cursor) == size);
    std::string result(4 * ((size + 2) / 3) + 1,'\0');
    result.resize(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),bytes.data(),size));
    OPENSSL_cleanse(bytes.data(),bytes.size());
    return result;
}
}
int main() {
    try {
        mailadmin::Config config;
        config.default_mail_hostname = "mx.example.test";
        auto desired = mailadmin::dns_defaults(config);
        mailadmin::validate_dns_desired(config,desired);
        require(desired.at("mail_servers").as_array()[0].at("hostname") == "mx.example.test");
        require(!desired.at("mta_sts").at("enabled").as_bool());
        auto unsafe = desired;
        unsafe["mta_sts"] = boost::json::object{{"enabled",true}};
        rejects([&] { mailadmin::validate_dns_desired(config,unsafe); });
        unsafe = desired; unsafe["private_key"] = "never accept a private-key field";
        rejects([&] { mailadmin::validate_dns_desired(config,unsafe); });
        std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> context{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free};
        require(context && EVP_PKEY_keygen_init(context.get()) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(),2048) == 1);
        EVP_PKEY* raw = nullptr;
        require(EVP_PKEY_keygen(context.get(),&raw) == 1);
        std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key{raw,EVP_PKEY_free};
        auto public_key = encode(key.get(),i2d_PublicKey);
        auto private_key = encode(key.get(),i2d_PrivateKey);
        const auto spki = encode(key.get(),i2d_PUBKEY);
        desired["dkim"] = boost::json::array{boost::json::object{{"selector","rotation.old"},{"public_key",public_key}}};
        mailadmin::validate_dns_desired(config,desired);
        desired.at("dkim").as_array()[0].at("public_key") = private_key;
        rejects([&] { mailadmin::validate_dns_desired(config,desired); });
        OPENSSL_cleanse(private_key.data(),private_key.size());
        desired.at("dkim").as_array()[0].at("public_key") = spki;
        rejects([&] { mailadmin::validate_dns_desired(config,desired); });
        desired.at("dkim").as_array()[0].at("public_key") = "-----BEGIN RSA PUBLIC KEY-----";
        rejects([&] { mailadmin::validate_dns_desired(config,desired); });
        desired.at("dkim").as_array()[0].at("public_key") = public_key + "AAAA";
        rejects([&] { mailadmin::validate_dns_desired(config,desired); });
        std::cout << "DNS defaults and public-key boundary checks passed\n";
    } catch (...) {
        std::cerr << "DNS configuration check failed\n";
        return 1;
    }
}
