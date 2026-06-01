#pragma once
#include <filesystem>

namespace fh6 {

// Install a CreateFileW IAT hook that redirects the game's standard
// Anthem.zip open to fh6-radio/custom/Anthem.zip when that file exists.
// Must be called early in run_bridge() before the game loads UI textures.
void install_logo_hook(const std::filesystem::path& game_dir) noexcept;

} // namespace fh6
