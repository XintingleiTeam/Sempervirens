#include "nbt.hpp"

#include "atomic_file.hpp"
#include "text.hpp"
#include "miniz/miniz.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace sempervirens {

namespace {
constexpr std::size_t max_nbt_bytes = 128U * 1024U * 1024U;
constexpr std::size_t max_entries = 1'000'000;
constexpr int max_depth = 64;

std::uint32_t little32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8 |
           std::uint32_t(data[2]) << 16 | std::uint32_t(data[3]) << 24;
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read NBT file");
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > max_nbt_bytes)
        throw std::runtime_error("NBT file exceeds safety limit");
    input.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty() && !input.read(reinterpret_cast<char*>(data.data()), data.size()))
        throw std::runtime_error("Cannot read complete NBT file");
    return data;
}

std::vector<std::uint8_t> gunzip(const std::vector<std::uint8_t>& compressed) {
    if (compressed.size() < 18 || compressed[2] != 8 || compressed[3] & 0xE0)
        throw std::runtime_error("Invalid gzip header");
    std::size_t offset = 10;
    const auto flags = compressed[3];
    const auto payload_end = compressed.size() - 8;
    if (flags & 4) {
        if (offset + 2 > payload_end) throw std::runtime_error("Truncated gzip extra length");
        const auto length = std::size_t(compressed[offset]) | std::size_t(compressed[offset + 1]) << 8;
        offset += 2 + length;
        if (offset > payload_end) throw std::runtime_error("Truncated gzip extra field");
    }
    for (int bit : {8, 16}) if (flags & bit) {
        while (offset < payload_end && compressed[offset] != 0) ++offset;
        if (offset >= payload_end) throw std::runtime_error("Truncated gzip string field");
        ++offset;
    }
    if (flags & 2) {
        if (offset + 2 > payload_end) throw std::runtime_error("Truncated gzip header CRC");
        offset += 2;
    }
    mz_stream stream{};
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        throw std::runtime_error("Could not initialize gzip decompressor");
    stream.next_in = compressed.data() + offset;
    stream.avail_in = static_cast<mz_uint>(payload_end - offset);
    std::vector<std::uint8_t> output;
    std::array<std::uint8_t, 64 * 1024> block{};
    int status = MZ_OK;
    do {
        stream.next_out = block.data();
        stream.avail_out = static_cast<mz_uint>(block.size());
        status = mz_inflate(&stream, MZ_NO_FLUSH);
        const auto count = block.size() - stream.avail_out;
        if (output.size() + count > max_nbt_bytes) {
            mz_inflateEnd(&stream);
            throw std::runtime_error("Decompressed NBT exceeds safety limit");
        }
        output.insert(output.end(), block.begin(), block.begin() + count);
    } while (status == MZ_OK && (stream.avail_in > 0 || stream.avail_out == 0));
    mz_inflateEnd(&stream);
    if (status != MZ_STREAM_END || stream.total_in != payload_end - offset)
        throw std::runtime_error("Invalid or truncated gzip deflate data");
    const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, output.data(), output.size()));
    if (crc != little32(compressed.data() + payload_end) ||
        static_cast<std::uint32_t>(output.size()) != little32(compressed.data() + payload_end + 4))
        throw std::runtime_error("Gzip checksum mismatch");
    return output;
}

void append_little32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> gzip(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> result{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    mz_stream stream{};
    if (mz_deflateInit2(&stream, MZ_BEST_COMPRESSION, MZ_DEFLATED,
                        -MZ_DEFAULT_WINDOW_BITS, 8, MZ_DEFAULT_STRATEGY) != MZ_OK)
        throw std::runtime_error("Could not initialize gzip compressor");
    stream.next_in = raw.data();
    stream.avail_in = static_cast<mz_uint>(raw.size());
    std::array<std::uint8_t, 64 * 1024> block{};
    int status;
    do {
        stream.next_out = block.data();
        stream.avail_out = static_cast<mz_uint>(block.size());
        status = mz_deflate(&stream, MZ_FINISH);
        result.insert(result.end(), block.begin(), block.begin() + (block.size() - stream.avail_out));
    } while (status == MZ_OK);
    mz_deflateEnd(&stream);
    if (status != MZ_STREAM_END) throw std::runtime_error("Could not compress NBT file");
    append_little32(result, static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, raw.data(), raw.size())));
    append_little32(result, static_cast<std::uint32_t>(raw.size()));
    return result;
}

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

    std::uint8_t byte() {
        if (position_ >= data_.size()) throw std::runtime_error("Truncated NBT file");
        return data_[position_++];
    }
    std::uint16_t u16() { return std::uint16_t(byte()) << 8 | byte(); }
    std::uint32_t u32() {
        return std::uint32_t(byte()) << 24 | std::uint32_t(byte()) << 16 |
               std::uint32_t(byte()) << 8 | byte();
    }
    std::uint64_t u64() { return std::uint64_t(u32()) << 32 | u32(); }
    std::string string() {
        const auto length = u16();
        if (length > data_.size() - position_) throw std::runtime_error("Truncated NBT string");
        std::string result(reinterpret_cast<const char*>(data_.data() + position_), length);
        position_ += length;
        return utf8_with_replacement(result);
    }
    std::size_t count(std::size_t element_size) {
        const auto value = static_cast<std::int32_t>(u32());
        if (value < 0 || static_cast<std::size_t>(value) > max_entries ||
            static_cast<std::size_t>(value) > (data_.size() - position_) / element_size)
            throw std::runtime_error("Invalid NBT array or list length");
        return static_cast<std::size_t>(value);
    }
    NbtTag payload(NbtType type, int depth) {
        if (depth > max_depth) throw std::runtime_error("NBT nesting limit exceeded");
        NbtTag tag;
        tag.type = type;
        switch (type) {
        case NbtType::byte: tag.number = byte(); break;
        case NbtType::short_integer: tag.number = static_cast<std::int16_t>(u16()); break;
        case NbtType::integer: tag.number = static_cast<std::int32_t>(u32()); break;
        case NbtType::long_integer: tag.number = static_cast<std::int64_t>(u64()); break;
        case NbtType::floating: {
            const auto bits = u32();
            tag.decimal = std::bit_cast<float>(bits);
            break;
        }
        case NbtType::double_precision: {
            const auto bits = u64();
            tag.decimal = std::bit_cast<double>(bits);
            break;
        }
        case NbtType::byte_array: {
            const auto length = count(1);
            tag.bytes.insert(tag.bytes.end(), data_.begin() + position_, data_.begin() + position_ + length);
            position_ += length;
            break;
        }
        case NbtType::string: tag.text = string(); break;
        case NbtType::list: {
            tag.item_type = static_cast<NbtType>(byte());
            const auto length = count(1);
            if (tag.item_type == NbtType::end && length != 0)
                throw std::runtime_error("Nonempty NBT list has End element type");
            for (std::size_t i = 0; i < length; ++i) tag.items.push_back(payload(tag.item_type, depth + 1));
            break;
        }
        case NbtType::compound:
            for (std::size_t i = 0; i < max_entries; ++i) {
                const auto child_type = static_cast<NbtType>(byte());
                if (child_type == NbtType::end) return tag;
                auto name = string();
                tag.set(std::move(name), payload(child_type, depth + 1));
            }
            throw std::runtime_error("NBT compound has too many entries");
        case NbtType::int_array: {
            const auto length = count(4);
            for (std::size_t i = 0; i < length; ++i) tag.integers.push_back(static_cast<std::int32_t>(u32()));
            break;
        }
        case NbtType::long_array: {
            const auto length = count(8);
            for (std::size_t i = 0; i < length; ++i) tag.long_integers.push_back(static_cast<std::int64_t>(u64()));
            break;
        }
        default: throw std::runtime_error("Unsupported NBT tag type");
        }
        return tag;
    }
private:
    const std::vector<std::uint8_t>& data_;
    std::size_t position_ = 0;
};

class Writer {
public:
    void byte(std::uint8_t value) { data_.push_back(value); }
    void u16(std::uint16_t value) { byte(value >> 8); byte(value); }
    void u32(std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) byte(value >> shift); }
    void u64(std::uint64_t value) { u32(value >> 32); u32(value); }
    void string(const std::string& value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("NBT string is too long");
        u16(static_cast<std::uint16_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }
    void count(std::size_t value) {
        if (value > max_entries || value > std::numeric_limits<std::int32_t>::max())
            throw std::runtime_error("NBT array is too large");
        u32(static_cast<std::uint32_t>(value));
    }
    void payload(const NbtTag& tag, int depth) {
        if (depth > max_depth || data_.size() > max_nbt_bytes)
            throw std::runtime_error("NBT output exceeds safety limit");
        switch (tag.type) {
        case NbtType::byte: byte(static_cast<std::uint8_t>(tag.number)); break;
        case NbtType::short_integer: u16(static_cast<std::uint16_t>(tag.number)); break;
        case NbtType::integer: u32(static_cast<std::uint32_t>(tag.number)); break;
        case NbtType::long_integer: u64(static_cast<std::uint64_t>(tag.number)); break;
        case NbtType::floating: u32(std::bit_cast<std::uint32_t>(static_cast<float>(tag.decimal))); break;
        case NbtType::double_precision: u64(std::bit_cast<std::uint64_t>(tag.decimal)); break;
        case NbtType::byte_array: count(tag.bytes.size()); data_.insert(data_.end(), tag.bytes.begin(), tag.bytes.end()); break;
        case NbtType::string: string(tag.text); break;
        case NbtType::list:
            byte(static_cast<std::uint8_t>(tag.item_type)); count(tag.items.size());
            for (const auto& item : tag.items) {
                if (item.type != tag.item_type) throw std::runtime_error("NBT list item type mismatch");
                payload(item, depth + 1);
            }
            break;
        case NbtType::compound:
            for (const auto& [name, child] : tag.fields) {
                if (child.type == NbtType::end) throw std::runtime_error("NBT compound contains End tag");
                byte(static_cast<std::uint8_t>(child.type)); string(name); payload(child, depth + 1);
            }
            byte(0); break;
        case NbtType::int_array:
            count(tag.integers.size()); for (const auto value : tag.integers) u32(static_cast<std::uint32_t>(value)); break;
        case NbtType::long_array:
            count(tag.long_integers.size()); for (const auto value : tag.long_integers) u64(static_cast<std::uint64_t>(value)); break;
        default: throw std::runtime_error("Unsupported NBT tag type");
        }
    }
    const std::vector<std::uint8_t>& data() const { return data_; }
private:
    std::vector<std::uint8_t> data_;
};
} // namespace

const NbtTag* NbtTag::get(const std::string& name) const {
    for (const auto& [key, tag] : fields) if (key == name) return &tag;
    return nullptr;
}
NbtTag* NbtTag::get(const std::string& name) {
    for (auto& [key, tag] : fields) if (key == name) return &tag;
    return nullptr;
}
void NbtTag::set(std::string name, NbtTag value) {
    for (auto& [key, tag] : fields) if (key == name) { tag = std::move(value); return; }
    fields.emplace_back(std::move(name), std::move(value));
}
std::string NbtTag::get_string(const std::string& name) const {
    const auto* tag = get(name);
    return tag && tag->type == NbtType::string ? tag->text : std::string{};
}

bool nbt_equal(const NbtTag& left, const NbtTag& right) {
    if (left.type != right.type) return false;
    switch (left.type) {
    case NbtType::byte:
    case NbtType::short_integer:
    case NbtType::integer:
    case NbtType::long_integer: return left.number == right.number;
    case NbtType::floating:
    case NbtType::double_precision:
        // System.Single/Double.Equals, used by the original implementation,
        // considers two NaN values equal even though IEEE operator== does not.
        return left.decimal == right.decimal ||
               (std::isnan(left.decimal) && std::isnan(right.decimal));
    case NbtType::string: return left.text == right.text;
    case NbtType::byte_array: return left.bytes == right.bytes;
    case NbtType::int_array: return left.integers == right.integers;
    case NbtType::long_array: return left.long_integers == right.long_integers;
    case NbtType::list:
        if (left.item_type != right.item_type || left.items.size() != right.items.size()) return false;
        for (std::size_t i = 0; i < left.items.size(); ++i)
            if (!nbt_equal(left.items[i], right.items[i])) return false;
        return true;
    case NbtType::compound:
        if (left.fields.size() != right.fields.size()) return false;
        for (const auto& [name, tag] : left.fields) {
            const auto* other = right.get(name);
            if (!other || !nbt_equal(tag, *other)) return false;
        }
        return true;
    default: return false;
    }
}

bool nbt_server_other_fields_equal(const NbtTag& left, const NbtTag& right) {
    int left_count = 0, right_count = 0;
    for (const auto& [name, tag] : left.fields) {
        if (ascii_lower(name) == "name") continue;
        ++left_count;
        const auto* other = right.get(name);
        if (!other || !nbt_equal(tag, *other)) return false;
    }
    for (const auto& [name, _] : right.fields) if (ascii_lower(name) != "name") ++right_count;
    return left_count == right_count;
}

NbtTag read_nbt_compound(const fs::path& path) {
    auto data = read_bytes(path);
    if (data.size() >= 2 && data[0] == 0x1f && data[1] == 0x8b) data = gunzip(data);
    Reader reader(data);
    if (reader.byte() != static_cast<std::uint8_t>(NbtType::compound))
        throw std::runtime_error("NBT root is not a Compound");
    (void)reader.string();
    return reader.payload(NbtType::compound, 0);
}

void write_nbt_compound(const fs::path& path, const NbtTag& root,
                        std::stop_token cancellation) {
    if (root.type != NbtType::compound) throw std::runtime_error("NBT root is not a Compound");
    Writer writer;
    writer.byte(static_cast<std::uint8_t>(NbtType::compound));
    writer.string("");
    writer.payload(root, 0);
    const auto data = gzip(writer.data());
    write_file_atomically(path,
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), cancellation);
}

} // namespace sempervirens
