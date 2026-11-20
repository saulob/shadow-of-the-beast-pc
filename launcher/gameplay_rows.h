#pragma once

#include <string>
#include <vector>

#include "settings.h"
#include "text_languages.h"
#include "trg/launcher.h"

namespace Sotb {

inline void DrawGameplayStartRows(trg::Ui& ui) {
    std::vector<trg::Option> languages;
    for (const TextLanguage& language : TextLanguages()) {
        languages.push_back({std::string(language.key), std::string(language.name)});
    }
    ui.Combo("Menu & Subtitles", kTextLanguageHelp, kTextLanguageKey, kDefaultTextLanguage, languages);
    ui.Toggle("Skip Intro", kSkipIntroHelp, kSkipIntroKey, false, "Off", "On");
}

} // namespace Sotb
