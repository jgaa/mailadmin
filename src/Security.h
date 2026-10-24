#pragma once
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace mailadmin {
using Digest = std::array<unsigned char, 32>;
Digest digest(std::string_view secret);
std::string random_secret();
std::string hex(std::span<const unsigned char> bytes);
void create_key(const std::filesystem::path& path, bool rotate);
Digest load_key(const std::filesystem::path& path);
bool verify_key(std::string_view authorization, const Digest& expected);
std::string password_hash(std::string_view password);
bool password_matches(std::string_view password, std::string_view hash);
}
