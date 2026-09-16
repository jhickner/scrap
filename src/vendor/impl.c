/* The single translation unit that instantiates the vendored single-header
 * libraries. Everything else includes their headers declaration-only. */

/* Pulls in the claude, codex, grok and pi drivers behind one vtable. */
#define BACKEND_IMPLEMENTATION
#include "agents/backend.h"

#define REPL_IMPLEMENTATION
#include "repl.h"

#include "screen_color.h"
#define COLORS_IMPLEMENTATION
#include "colors.h"

#define MACOS_VOICE_IMPLEMENTATION
#include "macos_voice.h"

#define GROKVNC_IMPLEMENTATION
#include "vnc/grokvnc.h"
