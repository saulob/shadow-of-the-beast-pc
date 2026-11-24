#include "sotb_exit_game_l10n.h"

#include <string.h>

/* Canonical Menu & Subtitles cultures (launcher/text_languages.cpp), using official Game.locres terms. */
static const SotbExitGameStrings kTable[] = {
    {"en_US", "EXIT GAME", "Exit Game", "Are you sure you want to exit the game?", "Confirm", "Cancel", "Saving..."},
    {"en_GB", "EXIT GAME", "Exit Game", "Are you sure you want to exit the game?", "Confirm", "Cancel", "Saving..."},
    {"ja", "ゲーム終了", "ゲームを終了", "本当にゲームを終了しますか？", "確認", "キャンセル", "保存中…"},
    {"ko", "게임 종료", "게임 종료", "정말로 게임을 종료하시겠습니까?", "확인", "취소", "저장 중…"},
    {"zh_Hant", "結束遊戲", "結束遊戲", "是否確定要結束遊戲？", "確認", "取消", "存檔中..."},
    {"fr", "QUITTER LE JEU", "Quitter le jeu", "Êtes-vous certain de vouloir quitter le jeu ?", "Confirmer", "Annuler",
     "Sauvegarde..."},
    {"de", "SPIEL BEENDEN", "Spiel beenden", "Möchtest du das Spiel wirklich beenden?", "Bestätigen", "Abbrechen",
     "Speichert ..."},
    {"it", "ESCI DAL GIOCO", "Esci dal gioco", "Vuoi davvero uscire dal gioco?", "Conferma", "Annulla",
     "Salvataggio..."},
    {"es_ES", "SALIR DEL JUEGO", "Salir del juego", "¿Seguro que quieres salir del juego?", "Confirmar", "Cancelar",
     "Guardando..."},
    {"es_MX", "SALIR DEL JUEGO", "Salir del juego", "¿Confirmas que deseas salir del juego?", "Confirmar", "Cancelar",
     "Guardando..."},
    {"pt_BR", "SAIR DO JOGO", "Sair do jogo", "Você quer mesmo sair do jogo?", "Confirmar", "Cancelar", "Salvando..."},
    {"pt_PT", "SAIR DO JOGO", "Sair do jogo", "Tens a certeza de que queres sair do jogo?", "Confirmar", "Cancelar",
     "A guardar..."},
    {"nl", "SPEL STOPPEN", "Spel stoppen", "Weet je zeker dat je het spel wilt stoppen?", "Bevestigen", "Annuleren",
     "Opslaan..."},
    {"ru", "ВЫЙТИ ИЗ ИГРЫ", "Выйти из игры", "Вы хотите выйти из игры?", "Подтвердить", "Отмена", "Сохранение..."},
    {"pl", "WYJŚCIE Z GRY", "Wyjście z gry", "Czy na pewno chcesz wyjść z gry?", "Potwierdź", "Anuluj",
     "Zapisywanie..."},
    {"fi", "LOPETA PELI", "Lopeta peli", "Haluatko varmasti lopettaa pelin?", "Vahvista", "Peru", "Tallennetaan..."},
    {"sv", "AVSLUTA SPEL", "Avsluta spel", "Vill du verkligen avsluta spelet?", "Bekräfta", "Avbryt", "Sparar ..."},
    {"da", "AFSLUT SPIL", "Afslut spil", "Er du sikker på, at du vil afslutte spillet?", "Bekræft", "Afbryd",
     "Gemmer ..."},
    {"nb", "AVSLUTT SPILL", "Avslutt spill", "Er du sikker på at du ønsker å avslutte spillet?", "Bekreft", "Avbryt",
     "Lagrer ..."},
};

static int ascii_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca == '-') ca = '_';
        if (cb == '-') cb = '_';
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static size_t base_len(const char *locale) {
    size_t n = 0;
    while (locale[n] && locale[n] != '_' && locale[n] != '-') ++n;
    return n;
}

static int base_ci_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return 1;
}

const SotbExitGameStrings *sotb_exit_game_strings(const char *locale) {
    const size_t count = sizeof(kTable) / sizeof(kTable[0]);
    if (!locale || !*locale) return &kTable[0];
    for (size_t i = 0; i < count; ++i) {
        if (ascii_ci_eq(kTable[i].locale, locale)) return &kTable[i];
    }
    const size_t bl = base_len(locale);
    if (bl > 0) {
        for (size_t i = 0; i < count; ++i) {
            if (base_len(kTable[i].locale) == bl && base_ci_eq(kTable[i].locale, locale, bl)) return &kTable[i];
        }
    }
    return &kTable[0];
}

const SotbExitGameStrings *sotb_exit_game_strings_table(size_t *count) {
    if (count) *count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}
