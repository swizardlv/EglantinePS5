// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone parser/loader checks for the synthetic SPEC-numbered NUP fixture.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
#include "core/loader/nup.h"

namespace {
constexpr u64 TitleId = 0x0400ABCD12340000ULL;
constexpr u64 AddonId = 0x0400ABCD12341001ULL;
int failures{};

void Expect(bool success, const char* message) {
    std::printf("%s %s\n", success ? "ok  " : "FAIL", message);
    failures += !success;
}

std::string Text(const FileSys::VirtualFile& file) {
    if (!file) {
        return "<missing>";
    }
    const auto bytes = file->ReadAllBytes();
    return {bytes.begin(), bytes.end()};
}

struct SpillDirectory {
    std::filesystem::path path;
    SpillDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "linux-nup-check-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("cannot create NUP check temporary directory");
        }
        path = pattern;
    }
    ~SpillDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void Check(const std::filesystem::path& fixture) {
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(fixture.string(), FileSys::OpenMode::Read);
    Expect(file != nullptr, "synthetic SPEC metadata fixture opens");
    if (!file) {
        return;
    }
    SpillDirectory spill;
    // The 9 MiB member exceeds half this budget and exercises the temporary-file path.
    FileSys::NupContainer container(file, 64 * 1024, spill.path);
    Expect(container.GetStatus() == Loader::ResultStatus::Success, "container parses");
    if (container.GetStatus() != Loader::ResultStatus::Success) {
        return;
    }
    Expect(container.GetPackages().size() == 3, "base, update and add-on packages recognized");
    const auto base = container.FindBasePackage();
    const auto update = container.FindBestUpdatePackage();
    const auto addon = container.FindPackage(FileSys::NupContainer::PackageKind::Addon, AddonId);
    Expect(base.has_value() && update.has_value() && addon.has_value(), "package lookup succeeds");
    if (!base || !update || !addon) {
        return;
    }
    const auto& base_info = container.GetPackages()[*base];
    Expect(base_info.title_id == TitleId && base_info.title_name == "NUP Sample" &&
               base_info.publisher == "EglantinePS5 checks" && base_info.display_version == "1.0.1",
           "title ID and SPEC-numbered metadata read correctly");
    Expect(container.GetPackages()[*update].version == 65536, "newest update version selected");
    Expect(FileSys::NupContainer::IsNupFile(file) && Loader::IdentifyFile(file) == Loader::FileType::NUP &&
               Loader::GuessFromFilename("sample.nup") == Loader::FileType::NUP,
           "magic, loader identification and filename agree");

    const auto exefs = container.GetExeFSDir(*base);
    const auto update_exefs = container.GetExeFSDir(*update);
    const auto main = exefs ? exefs->GetFile("main") : nullptr;
    const auto updated_main = update_exefs ? update_exefs->GetFile("main") : nullptr;
    const auto main_bytes = main ? main->ReadAllBytes() : std::vector<u8>{};
    const auto update_bytes = updated_main ? updated_main->ReadAllBytes() : std::vector<u8>{};
    Expect(main_bytes.size() == 0x100 && main_bytes[0x40] == 'B' &&
               update_bytes.size() == 0x100 && update_bytes[0x40] == 'U',
           "base and update ExeFS members stay distinct");
    FileSys::ProgramMetadata metadata;
    Expect(exefs && metadata.Load(exefs->GetFile("main.npdm")) == Loader::ResultStatus::Success &&
               metadata.IsSwitch2() && metadata.GetTitleID() == TitleId,
           "synthetic Switch 2 NPDM parses");
    const auto addon_dir = container.GetRomFSDir(*addon);
    Expect(addon_dir && Text(addon_dir->GetFile("dlc.txt")) == "dlc\n", "add-on member reads back");

    const auto base_romfs = container.GetRomFSDir(*base);
    const auto large = base_romfs ? base_romfs->GetFileRelative("data/big.bin") : nullptr;
    bool large_ok = large && large->GetSize() == (9u << 20);
    if (large_ok) {
        std::array<u8, 4096> tail{}, head{};
        large_ok = large->Read(tail.data(), tail.size(), large->GetSize() - tail.size()) == tail.size() &&
                   large->Read(head.data(), head.size(), 0) == head.size();
        for (std::size_t i = 0; large_ok && i < tail.size(); ++i) {
            large_ok = tail[i] == (large->GetSize() - tail.size() + i) % 251 && head[i] == i % 251;
        }
    }
    Expect(large_ok, "9 MiB compressed member reads correctly out of order with a small cache budget");
    Expect(container.VerifyIntegrity({}) == Loader::ResultStatus::Success,
           "all member data hashes verify");

    Loader::AppLoader_NUP loader(file);
    u64 program_id{};
    std::string title;
    Expect(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success && program_id == TitleId,
           "loader returns the base program ID");
    Expect(loader.ReadTitle(title) == Loader::ResultStatus::Success && title == "NUP Sample",
           "loader returns the title");
    FileSys::NACP control;
    Expect(loader.ReadControlData(control) == Loader::ResultStatus::Success &&
               control.GetApplicationName() == "NUP Sample" &&
               control.GetDeveloperName() == "EglantinePS5 checks" && control.raw.save_data_owner_id == TitleId,
           "loader synthesizes control metadata");
    std::vector<u8> icon;
    Expect(loader.ReadIcon(icon) == Loader::ResultStatus::Success && icon.size() >= 2 &&
               icon[0] == 0xFF && icon[1] == 0xD8,
           "loader reads the fixture JPEG icon");
    FileSys::VirtualFile romfs_image;
    const auto romfs_result = loader.ReadRomFS(romfs_image);
    const auto merged = romfs_image ? FileSys::ExtractRomFS(romfs_image) : nullptr;
    Expect(romfs_result == Loader::ResultStatus::Success && merged &&
               Text(merged->GetFile("base_only.txt")) == "from the base\n" &&
               Text(merged->GetFileRelative("data/shared.txt")) == "update version\n",
           "loader RomFS keeps base-only data and overlays the update");

    auto damaged = file->ReadAllBytes();
    damaged[0x80 + 0x40] ^= 1;
    FileSys::NupContainer bad_toc(std::make_shared<FileSys::VectorVfsFile>(damaged, "bad.nup"));
    Expect(bad_toc.GetStatus() == Loader::ResultStatus::ErrorNUPBadFormat, "TOC corruption rejected");
    auto truncated = file->ReadAllBytes();
    truncated.resize(truncated.size() - 16);
    FileSys::NupContainer short_file(std::make_shared<FileSys::VectorVfsFile>(truncated, "short.nup"));
    Expect(short_file.GetStatus() == Loader::ResultStatus::ErrorNUPBadFormat, "truncated container rejected");
    Loader::AppLoader_NUP broken(std::make_shared<FileSys::VectorVfsFile>(damaged, "bad.nup"));
    Expect(broken.VerifyIntegrity({}) == Loader::ResultStatus::ErrorNUPBadFormat,
           "loader exposes malformed-container status");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/nup directory>\n", argv[0]);
        return 2;
    }
    try {
        // The current core API follows SPEC's one-based metadata tags.
        Check(std::filesystem::path(argv[1]) / "sample-spec-meta.nup");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NUP check exception: %s\n", error.what());
        return 1;
    }
    std::puts(failures ? "linux-nup-check: FAIL" : "linux-nup-check: PASS");
    return failures ? 1 : 0;
}
