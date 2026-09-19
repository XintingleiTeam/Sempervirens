#pragma once

#include "model.hpp"

#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace sempervirens {

enum class NbtType : std::uint8_t {
    end = 0, byte = 1, short_integer = 2, integer = 3, long_integer = 4,
    floating = 5, double_precision = 6, byte_array = 7, string = 8,
    list = 9, compound = 10, int_array = 11, long_array = 12
};

struct NbtTag {
    NbtType type = NbtType::end;
    std::int64_t number = 0;
    double decimal = 0;
    std::string text;
    std::vector<std::uint8_t> bytes;
    std::vector<std::int32_t> integers;
    std::vector<std::int64_t> long_integers;
    NbtType item_type = NbtType::end;
    std::vector<NbtTag> items;
    std::vector<std::pair<std::string, NbtTag>> fields;

    const NbtTag* get(const std::string& name) const;
    NbtTag* get(const std::string& name);
    void set(std::string name, NbtTag value);
    std::string get_string(const std::string& name) const;
};

NbtTag read_nbt_compound(const fs::path& path);
void write_nbt_compound(const fs::path& path, const NbtTag& root,
                        std::stop_token cancellation = {});
bool nbt_equal(const NbtTag& left, const NbtTag& right);
bool nbt_server_other_fields_equal(const NbtTag& left, const NbtTag& right);

} // namespace sempervirens
