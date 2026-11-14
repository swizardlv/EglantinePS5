// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Tegra {

// Immutable after publication; the queued present retains ownership until its upload completes.
struct CpuFramebuffer {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t pitch{};
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool IsValid() const noexcept {
        const std::size_t row_pixels = width;
        if (width == 0 || height == 0 || row_pixels > std::numeric_limits<std::size_t>::max() / 4) {
            return false;
        }
        const std::size_t row = std::size_t{width} * 4;
        return pitch >= row && height <= std::numeric_limits<std::size_t>::max() / pitch &&
               pixels.size() >= pitch * height;
    }

    [[nodiscard]] bool CopyPacked(std::span<std::uint8_t> destination) const {
        if (!IsValid() || destination.size() < std::size_t{width} * 4 * height) {
            return false;
        }
        const std::size_t row = std::size_t{width} * 4;
        for (std::size_t y = 0; y < height; ++y) {
            std::copy_n(pixels.data() + y * pitch, row, destination.data() + y * row);
        }
        return true;
    }
};

// Stop invalidates an in-flight conversion as well as the last published frame.
class CpuVideoFrameStore {
public:
    using Token = std::uint64_t;

    [[nodiscard]] Token Start() {
        std::scoped_lock lock{mutex};
        frame.reset();
        active = ++generation;
        return active;
    }

    void Stop(Token token) {
        std::scoped_lock lock{mutex};
        if (token != 0 && token == active) {
            active = 0;
            frame.reset();
        }
    }

    [[nodiscard]] Token Restart(Token token) {
        std::scoped_lock lock{mutex};
        if (token == 0 || token != active) {
            return 0;
        }
        frame.reset();
        active = ++generation;
        return active;
    }

    [[nodiscard]] bool Publish(Token token, std::shared_ptr<const CpuFramebuffer> next) {
        if (!next || !next->IsValid()) {
            return false;
        }
        std::scoped_lock lock{mutex};
        if (token == 0 || token != active) {
            return false;
        }
        frame = std::move(next);
        return true;
    }

    [[nodiscard]] std::shared_ptr<const CpuFramebuffer> Snapshot() const {
        std::scoped_lock lock{mutex};
        return frame;
    }

private:
    mutable std::mutex mutex;
    Token generation{};
    Token active{};
    std::shared_ptr<const CpuFramebuffer> frame;
};

} // namespace Tegra
