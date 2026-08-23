
#ifndef REPLYJSON_H
#define REPLYJSON_H

#include "vendor/cJSON.h"

// The JSON object in a model's reply. Asked for JSON and nothing else, a model
// will still sometimes wrap it in a sentence, or answer twice and mean the
// second one -- so it is the last complete object that counts, not the first.
// NULL when there is none. The caller frees it.
cJSON *replyjson_parse(const char *text);

#endif
