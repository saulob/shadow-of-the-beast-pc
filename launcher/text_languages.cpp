#include "text_languages.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <vector>
#include <system_error>

#include "settings.h"
#include "sotb_launcher.h"
#include "utf8.h"

namespace Sotb {
namespace {

#define L(culture) "Beast/Content/Localization/Game/" culture "/Game.locres"
constexpr TextLanguage kLanguages[] = {
    {"en_US", "English (US)", 1, L("en_US")},        {"en_GB", "English (UK)", 18, L("en_GB")},
    {"ja", "Japanese", 0, L("ja")},               {"ko", "Korean", 9, L("ko")},
    {"zh_Hant", "Chinese (Traditional)", 10, L("zh_Hant")}, {"fr", "French", 2, L("fr")},
    {"de", "German", 4, L("de")},                 {"it", "Italian", 5, L("it")},
    {"es_ES", "Spanish (Spain)", 3, L("es_ES")},     {"es_MX", "Spanish (Latin America)", 20, L("es_MX")},
    {"pt_BR", "Portuguese (Brazil)", 17, L("pt_BR")}, {"pt_PT", "Portuguese (Portugal)", 7, L("pt_PT")},
    {"nl", "Dutch", 6, L("nl")},                  {"ru", "Russian", 8, L("ru")},
    {"pl", "Polish", 16, L("pl")},                {"fi", "Finnish", 12, L("fi")},
    {"sv", "Swedish", 13, L("sv")},               {"da", "Danish", 14, L("da")},
    {"nb", "Norwegian", 15, L("nb")},
};
#undef L

int PresentationRank(const TextLanguage& language) {
    return language.key == "en_US" ? 0 : language.key == "en_GB" ? 1 : 2;
}

std::string Lowered(std::string_view text) {
    std::string lowered(text);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered;
}

} // namespace

void SortTextLanguages(std::vector<TextLanguage>& languages) {
    std::stable_sort(languages.begin(), languages.end(), [](const TextLanguage& a, const TextLanguage& b) {
        const int rank_a = PresentationRank(a), rank_b = PresentationRank(b);
        return rank_a != rank_b ? rank_a < rank_b : Lowered(a.name) < Lowered(b.name);
    });
}

std::span<const TextLanguage> TextLanguages() {
    static const std::vector<TextLanguage> sorted = [] {
        std::vector<TextLanguage> languages(std::begin(kLanguages), std::end(kLanguages));
        SortTextLanguages(languages);
        return languages;
    }();
    return sorted;
}

const TextLanguage& ResolveTextLanguage(std::string_view key) {
    const auto found = std::find_if(std::begin(kLanguages), std::end(kLanguages),
                                    [key](const TextLanguage& language) { return language.key == key; });
    return found == std::end(kLanguages) ? kLanguages[0] : *found;
}

} // namespace Sotb

extern "C" int sotb_text_language(const char* settings_file) {
    if (!settings_file || !*settings_file) {
        return Sotb::ResolveTextLanguage({}).ps4_id;
    }
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file));
    const Sotb::TextLanguage& language = Sotb::ResolveTextLanguage(settings.Get(Sotb::kTextLanguageKey));
    return language.ps4_id;
}

extern "C" const char* sotb_text_language_key(const char* settings_file) {
    if (!settings_file || !*settings_file) {
        return Sotb::ResolveTextLanguage({}).key.data();
    }
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file));
    const Sotb::TextLanguage& language = Sotb::ResolveTextLanguage(settings.Get(Sotb::kTextLanguageKey));
    return language.key.data();
}
