// The Menu & Subtitles languages the installed game carries: one Game.locres per culture in pakchunk0-ps4.pak
// (TextLanguage::archive is that entry; ps4_id is the system language the guest reads).
#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "game_profile.h"

namespace Sotb {


inline constexpr char kTextLanguageKey[] = "text_language";
inline constexpr char kDefaultTextLanguage[] = "en_US";

// English (US) and English (UK) first, then the rest alphabetically by English name.
void SortTextLanguages(std::vector<TextLanguage>& languages);

std::span<const TextLanguage> TextLanguages();

// The language a saved key names; English (US), the game's default, when it is empty or not one of them.
const TextLanguage& ResolveTextLanguage(std::string_view key);

} // namespace Sotb
