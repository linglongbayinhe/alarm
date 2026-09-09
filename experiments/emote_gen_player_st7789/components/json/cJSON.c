#include "cJSON.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cursor;
    const char *end;
} json_parser_t;

static void skip_ws(json_parser_t *parser)
{
    while ((parser->cursor < parser->end) && isspace((unsigned char)*parser->cursor)) {
        parser->cursor++;
    }
}

static cJSON *new_item(int type)
{
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item != NULL) {
        item->type = type;
    }
    return item;
}

static void append_child(cJSON *parent, cJSON *child)
{
    cJSON *tail = NULL;

    if ((parent == NULL) || (child == NULL)) {
        return;
    }

    if (parent->child == NULL) {
        parent->child = child;
        return;
    }

    tail = parent->child;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = child;
    child->prev = tail;
}

static char *parse_string_value(json_parser_t *parser)
{
    char *out = NULL;
    size_t capacity = 0;
    size_t used = 0;

    if ((parser->cursor >= parser->end) || (*parser->cursor != '"')) {
        return NULL;
    }
    parser->cursor++;

    capacity = (size_t)(parser->end - parser->cursor) + 1;
    out = (char *)malloc(capacity);
    if (out == NULL) {
        return NULL;
    }

    while (parser->cursor < parser->end) {
        char ch = *parser->cursor++;
        if (ch == '"') {
            out[used] = '\0';
            return out;
        }
        if (ch == '\\') {
            if (parser->cursor >= parser->end) {
                break;
            }
            ch = *parser->cursor++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                out[used++] = ch;
                break;
            case 'b':
                out[used++] = '\b';
                break;
            case 'f':
                out[used++] = '\f';
                break;
            case 'n':
                out[used++] = '\n';
                break;
            case 'r':
                out[used++] = '\r';
                break;
            case 't':
                out[used++] = '\t';
                break;
            case 'u':
                if ((parser->end - parser->cursor) >= 4) {
                    parser->cursor += 4;
                    out[used++] = '?';
                } else {
                    free(out);
                    return NULL;
                }
                break;
            default:
                out[used++] = ch;
                break;
            }
            continue;
        }
        out[used++] = ch;
    }

    free(out);
    return NULL;
}

static cJSON *parse_value(json_parser_t *parser);

static cJSON *parse_array(json_parser_t *parser)
{
    cJSON *array = NULL;

    if ((parser->cursor >= parser->end) || (*parser->cursor != '[')) {
        return NULL;
    }
    parser->cursor++;

    array = new_item(cJSON_Array);
    if (array == NULL) {
        return NULL;
    }

    skip_ws(parser);
    if ((parser->cursor < parser->end) && (*parser->cursor == ']')) {
        parser->cursor++;
        return array;
    }

    while (parser->cursor < parser->end) {
        cJSON *child = parse_value(parser);
        if (child == NULL) {
            cJSON_Delete(array);
            return NULL;
        }
        append_child(array, child);

        skip_ws(parser);
        if ((parser->cursor < parser->end) && (*parser->cursor == ',')) {
            parser->cursor++;
            skip_ws(parser);
            continue;
        }
        if ((parser->cursor < parser->end) && (*parser->cursor == ']')) {
            parser->cursor++;
            return array;
        }
        break;
    }

    cJSON_Delete(array);
    return NULL;
}

static cJSON *parse_object(json_parser_t *parser)
{
    cJSON *object = NULL;

    if ((parser->cursor >= parser->end) || (*parser->cursor != '{')) {
        return NULL;
    }
    parser->cursor++;

    object = new_item(cJSON_Object);
    if (object == NULL) {
        return NULL;
    }

    skip_ws(parser);
    if ((parser->cursor < parser->end) && (*parser->cursor == '}')) {
        parser->cursor++;
        return object;
    }

    while (parser->cursor < parser->end) {
        char *key = NULL;
        cJSON *value = NULL;

        skip_ws(parser);
        key = parse_string_value(parser);
        if (key == NULL) {
            cJSON_Delete(object);
            return NULL;
        }

        skip_ws(parser);
        if ((parser->cursor >= parser->end) || (*parser->cursor != ':')) {
            free(key);
            cJSON_Delete(object);
            return NULL;
        }
        parser->cursor++;

        value = parse_value(parser);
        if (value == NULL) {
            free(key);
            cJSON_Delete(object);
            return NULL;
        }
        value->string = key;
        append_child(object, value);

        skip_ws(parser);
        if ((parser->cursor < parser->end) && (*parser->cursor == ',')) {
            parser->cursor++;
            continue;
        }
        if ((parser->cursor < parser->end) && (*parser->cursor == '}')) {
            parser->cursor++;
            return object;
        }
        break;
    }

    cJSON_Delete(object);
    return NULL;
}

static cJSON *parse_number(json_parser_t *parser)
{
    char number_text[48];
    const char *start = parser->cursor;
    char *endptr = NULL;
    double value = 0.0;
    cJSON *number = NULL;
    size_t len = 0;

    while ((parser->cursor < parser->end) &&
           (isdigit((unsigned char)*parser->cursor) ||
            (*parser->cursor == '-') ||
            (*parser->cursor == '+') ||
            (*parser->cursor == '.') ||
            (*parser->cursor == 'e') ||
            (*parser->cursor == 'E'))) {
        parser->cursor++;
    }

    len = (size_t)(parser->cursor - start);
    if ((len == 0) || (len >= sizeof(number_text))) {
        return NULL;
    }
    memcpy(number_text, start, len);
    number_text[len] = '\0';

    value = strtod(number_text, &endptr);
    if ((endptr == number_text) || (*endptr != '\0')) {
        return NULL;
    }

    number = new_item(cJSON_Number);
    if (number == NULL) {
        return NULL;
    }
    number->valuedouble = value;
    number->valueint = (int)value;
    return number;
}

static int consume_literal(json_parser_t *parser, const char *literal)
{
    size_t len = strlen(literal);
    if ((size_t)(parser->end - parser->cursor) < len) {
        return 0;
    }
    if (strncmp(parser->cursor, literal, len) != 0) {
        return 0;
    }
    parser->cursor += len;
    return 1;
}

static cJSON *parse_value(json_parser_t *parser)
{
    cJSON *item = NULL;

    skip_ws(parser);
    if (parser->cursor >= parser->end) {
        return NULL;
    }

    switch (*parser->cursor) {
    case '[':
        return parse_array(parser);
    case '{':
        return parse_object(parser);
    case '"':
        item = new_item(cJSON_String);
        if (item == NULL) {
            return NULL;
        }
        item->valuestring = parse_string_value(parser);
        if (item->valuestring == NULL) {
            cJSON_Delete(item);
            return NULL;
        }
        return item;
    case 't':
        return consume_literal(parser, "true") ? new_item(cJSON_True) : NULL;
    case 'f':
        return consume_literal(parser, "false") ? new_item(cJSON_False) : NULL;
    case 'n':
        return consume_literal(parser, "null") ? new_item(cJSON_NULL) : NULL;
    default:
        if ((*parser->cursor == '-') || isdigit((unsigned char)*parser->cursor)) {
            return parse_number(parser);
        }
        break;
    }

    return NULL;
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    cJSON *root = NULL;
    json_parser_t parser = {
        .cursor = value,
        .end = value + buffer_length,
    };

    if ((value == NULL) || (buffer_length == 0)) {
        return NULL;
    }

    root = parse_value(&parser);
    if (root == NULL) {
        return NULL;
    }

    skip_ws(&parser);
    if (parser.cursor != parser.end) {
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

void cJSON_Delete(cJSON *item)
{
    while (item != NULL) {
        cJSON *next = item->next;
        if (item->child != NULL) {
            cJSON_Delete(item->child);
        }
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}

cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    cJSON *child = NULL;

    if ((object == NULL) || (string == NULL) || ((object->type & 0xFF) != cJSON_Object)) {
        return NULL;
    }

    child = object->child;
    while (child != NULL) {
        if ((child->string != NULL) && (strcmp(child->string, string) == 0)) {
            return child;
        }
        child = child->next;
    }

    return NULL;
}

int cJSON_GetArraySize(const cJSON *array)
{
    int count = 0;
    cJSON *child = NULL;

    if ((array == NULL) || ((array->type & 0xFF) != cJSON_Array)) {
        return 0;
    }

    child = array->child;
    while (child != NULL) {
        count++;
        child = child->next;
    }
    return count;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    int current = 0;
    cJSON *child = NULL;

    if ((array == NULL) || (index < 0) || ((array->type & 0xFF) != cJSON_Array)) {
        return NULL;
    }

    child = array->child;
    while (child != NULL) {
        if (current == index) {
            return child;
        }
        current++;
        child = child->next;
    }

    return NULL;
}
