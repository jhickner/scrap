/* The vendored libraries without the voice helper client, for a harness that
   stands in for the helper itself. */

#define BACKEND_IMPLEMENTATION
#include "agents/backend.h"

#define REPL_IMPLEMENTATION
#include "repl.h"

#include "screen_color.h"
#define COLORS_IMPLEMENTATION
#include "colors.h"
