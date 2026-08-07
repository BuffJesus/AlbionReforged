#pragma once

#include "f2/render/render_backend.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace f2::render {

// Maps a stable integer key (e.g. a NativeUiAsset value, or a video-slot constant) to a stable
// TextureId, and each TextureId to a backend-opaque handle value. This is the indirection that lets
// the shared scene reference textures by TextureId while each backend stores its own resource:
// the D3D12 backend stores D3D12_GPU_DESCRIPTOR_HANDLE::ptr, Vulkan stores its descriptor, etc.
// Backend-agnostic and headless-testable (handles are just uint64 to this layer).
class TextureRegistry {
public:
    // Get-or-create a stable TextureId for `key`. Repeated calls with the same key return the same id.
    TextureId id_for_key(std::uint32_t key) {
        auto it = key_to_id_.find(key);
        if (it != key_to_id_.end()) return it->second;
        const TextureId id = static_cast<TextureId>(handles_.size()) + 1;  // ids start at 1
        key_to_id_.emplace(key, id);
        handles_.push_back(0);
        return id;
    }

    // Set/replace the backend handle for a TextureId (opaque; e.g. a D3D12 descriptor ptr).
    void set_handle(TextureId id, std::uint64_t handle) {
        if (id == kInvalidTexture || id > handles_.size()) return;
        handles_[id - 1] = handle;
    }

    // Resolve a TextureId to its backend handle (0 if unset / invalid).
    [[nodiscard]] std::uint64_t handle(TextureId id) const {
        if (id == kInvalidTexture || id > handles_.size()) return 0;
        return handles_[id - 1];
    }

    [[nodiscard]] std::size_t size() const noexcept { return handles_.size(); }
    void clear() noexcept {
        key_to_id_.clear();
        handles_.clear();
    }

private:
    std::unordered_map<std::uint32_t, TextureId> key_to_id_;
    std::vector<std::uint64_t> handles_;  // indexed by (id - 1)
};

}  // namespace f2::render
