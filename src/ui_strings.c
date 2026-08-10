#include "ui_strings.h"

// The two language tables, generated from UI_STRING_LIST so they cannot drift
// out of sync with the str_id_t enum. Kept here (not in the header) so each
// translation unit does not get its own private copy of both arrays.

static const char *const STRS_EN[STR_ID__COUNT] = {
#define X(name, en, ru) en,
    UI_STRING_LIST(X)
#undef X
};

static const char *const STRS_RU[STR_ID__COUNT] = {
#define X(name, en, ru) ru,
    UI_STRING_LIST(X)
#undef X
};

static const char *const *s_table = STRS_EN;

void ui_set_language(int lang_id)
{
    s_table = (lang_id == APP_LANG_RU) ? STRS_RU : STRS_EN;
}

const char *ui_str(str_id_t id)
{
    if ((unsigned)id >= (unsigned)STR_ID__COUNT) {
        return "";
    }
    return s_table[id];
}
