#include "replkeys.h"

int replkeys_map(const tty_event *ev, ReplEvent *out)
{
    static const struct {
        tty_key from;
        ReplKey to;
    } MAP[] = {
        {TK_NEWLINE, REPL_KEY_NEWLINE},       {TK_BACKSPACE, REPL_KEY_BACKSPACE},
        {TK_DELETE, REPL_KEY_DELETE},         {TK_LEFT, REPL_KEY_LEFT},
        {TK_RIGHT, REPL_KEY_RIGHT},           {TK_UP, REPL_KEY_UP},
        {TK_DOWN, REPL_KEY_DOWN},             {TK_WORD_LEFT, REPL_KEY_WORD_LEFT},
        {TK_WORD_RIGHT, REPL_KEY_WORD_RIGHT},
    };

    *out = (ReplEvent){0};

    switch (ev->key) {
    case TK_TEXT:
        if (!ev->text)
            return 0;
        out->key = REPL_KEY_TEXT;
        out->text = ev->text;
        return 1;

    case TK_CHAR:
        out->key = REPL_KEY_CHAR;
        out->codepoint = ev->cp;
        return 1;

    case TK_HOME:
    case TK_END:
        out->key = REPL_KEY_CHAR;
        out->codepoint = ev->key == TK_HOME ? 1 : 5;
        return 1;

    default:
        for (size_t i = 0; i < sizeof MAP / sizeof *MAP; i++)
            if (MAP[i].from == ev->key) {
                out->key = MAP[i].to;
                return 1;
            }
        return 0;
    }
}
