#include "f2/native_save.h"

#include <cstring>

namespace f2 {

void WorldArchive::raw(void* p, std::size_t n) {
    if (mode_ == ArchiveMode::Write) {
        const auto* src = static_cast<const std::uint8_t*>(p);
        buffer_.insert(buffer_.end(), src, src + n);
    } else {
        auto* dst = static_cast<std::uint8_t*>(p);
        if (pos_ + n > buffer_.size()) {
            ok_ = false;
            std::memset(dst, 0, n);  // graceful: zero-fill past the end
            pos_ = buffer_.size();
            return;
        }
        std::memcpy(dst, buffer_.data() + pos_, n);
        pos_ += n;
    }
}

void WorldArchive::visit(std::string& v) {
    std::uint32_t len = static_cast<std::uint32_t>(v.size());
    visit(len);
    if (reading()) {
        if (pos_ + len > buffer_.size()) {
            ok_ = false;
            v.clear();
            pos_ = buffer_.size();
            return;
        }
        v.assign(reinterpret_cast<const char*>(buffer_.data() + pos_), len);
        pos_ += len;
    } else {
        buffer_.insert(buffer_.end(), v.begin(), v.end());
    }
}

void WorldArchive::write_bytes(const std::vector<std::uint8_t>& b) {
    buffer_.insert(buffer_.end(), b.begin(), b.end());
}

std::vector<std::uint8_t> WorldArchive::read_bytes(std::size_t n) {
    if (pos_ + n > buffer_.size()) {
        ok_ = false;
        std::vector<std::uint8_t> rest(buffer_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                       buffer_.end());
        pos_ = buffer_.size();
        return rest;
    }
    std::vector<std::uint8_t> out(buffer_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return out;
}

void NativeGameState::serialize(WorldArchive& ar) {
    ar.visit(header.version);
    ar.visit(header.episode);
    ar.visit(header.chapter);
    ar.visit(quest_completion);
    ar.visit(hero_position);
}

}  // namespace f2
