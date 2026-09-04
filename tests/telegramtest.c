
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TELEGRAM_IMPLEMENTATION
#include "telegram.h"

static void case_rows(void)
{
    const tg_button b[] = {
        {"one", "1:0"}, {"two", "1:1"}, {"three", "1:2"},
    };

    cJSON *markup = tg_keyboard_json(b, 3, 2);
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(markup, "inline_keyboard");
    assert(cJSON_GetArraySize(rows) == 2);
    assert(cJSON_GetArraySize(cJSON_GetArrayItem(rows, 0)) == 2);
    assert(cJSON_GetArraySize(cJSON_GetArrayItem(rows, 1)) == 1);

    cJSON *first = cJSON_GetArrayItem(cJSON_GetArrayItem(rows, 0), 0);
    assert(!strcmp(cJSON_GetObjectItem(first, "text")->valuestring, "one"));
    assert(!strcmp(cJSON_GetObjectItem(first, "callback_data")->valuestring, "1:0"));
    cJSON_Delete(markup);

    markup = tg_keyboard_json(b, 3, 1);
    rows = cJSON_GetObjectItemCaseSensitive(markup, "inline_keyboard");
    assert(cJSON_GetArraySize(rows) == 3);
    cJSON_Delete(markup);
    assert(tg_keyboard_json(b, 0, 1) == NULL);
    assert(tg_keyboard_json(NULL, 3, 1) == NULL);
}

static void case_tap(void)
{
    const char *json =
        "{\"id\":\"4382\",\"data\":\"7:2\","
        " \"from\":{\"id\":99},"
        " \"message\":{\"message_id\":1234,\"chat\":{\"id\":-100}}}";
    cJSON *cb = cJSON_Parse(json);
    assert(cb);

    tg_update u = {0};
    tg_parse_callback(cb, &u);
    assert(!strcmp(u.callback_id, "4382"));
    assert(!strcmp(u.callback_data, "7:2"));
    assert(u.chat_id == -100);
    assert(u.from_id == 99);
    assert(u.message_id == 1234);

    cJSON_Delete(cb);
    free(u.callback_id);
    free(u.callback_data);
}

static void case_tap_no_message(void)
{
    cJSON *cb = cJSON_Parse("{\"id\":\"9\",\"data\":\"1:0\"}");
    tg_update u = {0};
    tg_parse_callback(cb, &u);
    assert(!strcmp(u.callback_data, "1:0"));
    assert(u.chat_id == 0 && u.message_id == 0);
    cJSON_Delete(cb);
    free(u.callback_id);
    free(u.callback_data);
}

int main(void)
{
    case_rows();
    case_tap();
    case_tap_no_message();
    printf("ok\n");
    return 0;
}
