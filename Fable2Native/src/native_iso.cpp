#include "f2/native_iso.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <ranges>
#include <string_view>
#include <vector>

namespace f2 {

namespace {

constexpr std::uint64_t kSectorSize = 2048;
constexpr std::uint64_t kDescriptorOffset = 0x10000;
constexpr std::string_view kMediaMagic = "MICROSOFT*XBOX*MEDIA";
constexpr std::array<std::uint64_t, 4> kPartitionOffsets{
    0xFD90000, 0x02080000, 0x18300000, 0};

struct Entry {
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    bool directory = false;
};

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool unsafe_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\') return true;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == std::string_view::npos
                                                     ? path.size() - start
                                                     : end - start);
        if (component.empty() || component == "." || component == "..") return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

class XdvdfsReader {
public:
    bool open(const std::filesystem::path& path, std::string& error) {
        input_.open(path, std::ios::binary);
        if (!input_) {
            error = "Unable to open the selected ISO.";
            return false;
        }
        input_.seekg(0, std::ios::end);
        file_size_ = static_cast<std::uint64_t>(input_.tellg());
        input_.seekg(0, std::ios::beg);

        std::array<char, kMediaMagic.size()> magic{};
        for (const auto candidate : kPartitionOffsets) {
            if (!read_at(candidate + kDescriptorOffset, magic.data(), magic.size()) ||
                std::string_view(magic.data(), magic.size()) != kMediaMagic) {
                continue;
            }
            std::array<std::uint8_t, 8> root{};
            if (!read_at(candidate + kDescriptorOffset + 20, root.data(), root.size())) {
                error = "The ISO volume descriptor is truncated.";
                return false;
            }
            base_ = candidate;
            const auto root_sector = read_u32(root.data());
            const auto root_size = read_u32(root.data() + 4);
            if (root_size == 0 || root_size > 32 * 1024 * 1024) {
                error = "The ISO root directory is invalid.";
                return false;
            }
            return read_directory(root_sector, root_size, {}, error);
        }
        error = "The selected file is not a recognized Xbox 360 XDVDFS ISO.";
        return false;
    }

    [[nodiscard]] std::uint64_t total_bytes() const {
        std::uint64_t total = 0;
        for (const auto& entry : entries_) {
            if (!entry.directory && !should_skip(entry.path)) total += entry.size;
        }
        return total;
    }

    bool extract(const std::filesystem::path& output_root,
                 const InstallProgress& progress, std::string& error) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(output_root, filesystem_error);
        if (filesystem_error) {
            error = "Unable to create the native game install directory.";
            return false;
        }

        const auto total = total_bytes();
        std::uint64_t copied = 0;
        std::vector<std::uint8_t> buffer(4 * 1024 * 1024);
        for (const auto& entry : entries_) {
            if (entry.directory || should_skip(entry.path)) continue;
            const auto target = output_root / std::filesystem::path(entry.path);
            std::filesystem::create_directories(target.parent_path(), filesystem_error);
            if (filesystem_error) {
                error = "Unable to create an install subdirectory.";
                return false;
            }
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "Unable to create " + entry.path + ".";
                return false;
            }

            std::uint64_t remaining = entry.size;
            std::uint64_t offset = entry.offset;
            while (remaining > 0) {
                const auto chunk = static_cast<std::streamsize>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                if (!read_at(offset, buffer.data(), static_cast<std::size_t>(chunk))) {
                    error = "Unable to read " + entry.path + " from the ISO.";
                    return false;
                }
                output.write(reinterpret_cast<const char*>(buffer.data()), chunk);
                if (!output) {
                    error = "Unable to write " + entry.path + ".";
                    return false;
                }
                offset += static_cast<std::uint64_t>(chunk);
                remaining -= static_cast<std::uint64_t>(chunk);
                copied += static_cast<std::uint64_t>(chunk);
                if (progress && !progress(copied, total)) {
                    error = "Installation cancelled.";
                    return false;
                }
            }
        }
        return true;
    }

private:
    bool read_at(std::uint64_t offset, void* destination, std::size_t size) {
        if (offset > file_size_ || size > file_size_ - offset) return false;
        input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
        return input_.good() || input_.gcount() == static_cast<std::streamsize>(size);
    }

    bool read_directory(std::uint32_t sector, std::uint32_t size,
                        const std::string& prefix, std::string& error) {
        std::vector<std::uint8_t> directory(size);
        if (!read_at(base_ + static_cast<std::uint64_t>(sector) * kSectorSize,
                     directory.data(), directory.size())) {
            error = "Unable to read an ISO directory.";
            return false;
        }
        std::function<bool(std::uint32_t)> walk = [&](std::uint32_t word_offset) {
            const auto byte_offset = static_cast<std::size_t>(word_offset) * 4;
            if (byte_offset + 14 > directory.size()) return true;
            const auto left = read_u16(directory.data() + byte_offset);
            const auto right = read_u16(directory.data() + byte_offset + 2);
            const auto child_sector = read_u32(directory.data() + byte_offset + 4);
            const auto child_size = read_u32(directory.data() + byte_offset + 8);
            const auto attributes = directory[byte_offset + 12];
            const auto name_length = directory[byte_offset + 13];
            if (byte_offset + 14 + name_length > directory.size()) return false;
            if (left != 0 && left != 0xFFFF && !walk(left)) return false;

            const std::string name(reinterpret_cast<const char*>(directory.data() + byte_offset + 14),
                                   name_length);
            const auto path = prefix.empty() ? name : prefix + "/" + name;
            if (unsafe_path(path)) return false;
            const bool is_directory = (attributes & 0x10) != 0;
            entries_.push_back({path, base_ + static_cast<std::uint64_t>(child_sector) * kSectorSize,
                                child_size, is_directory});
            if (is_directory && child_size > 0 && !read_directory(child_sector, child_size, path, error)) {
                return false;
            }
            if (right != 0 && right != 0xFFFF && !walk(right)) return false;
            return true;
        };
        if (!walk(0)) {
            if (error.empty()) error = "The ISO contains an invalid directory entry.";
            return false;
        }
        return true;
    }

    static bool should_skip(std::string_view path) {
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto end = path.find('/', start);
            auto component = path.substr(start, end == std::string_view::npos
                                                    ? path.size() - start
                                                    : end - start);
            std::string lower(component);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (lower == "$systemupdate") return true;
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return false;
    }

    std::ifstream input_;
    std::uint64_t file_size_ = 0;
    std::uint64_t base_ = 0;
    std::vector<Entry> entries_;
};

}  // namespace

bool extract_xbox360_iso(const std::filesystem::path& iso_path,
                         const std::filesystem::path& output_root,
                         const InstallProgress& progress,
                         std::string& error) {
    XdvdfsReader reader;
    if (!reader.open(iso_path, error)) return false;
    return reader.extract(output_root, progress, error);
}

}  // namespace f2
