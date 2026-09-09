#ifndef CJSON_H
#define CJSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length);
void cJSON_Delete(cJSON *item);
cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string);
int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);

#define cJSON_IsArray(item) ((item) != NULL && (((item)->type & 0xFF) == cJSON_Array))
#define cJSON_IsString(item) ((item) != NULL && (((item)->type & 0xFF) == cJSON_String) && ((item)->valuestring != NULL))
#define cJSON_IsNumber(item) ((item) != NULL && (((item)->type & 0xFF) == cJSON_Number))

#define cJSON_ArrayForEach(element, array) \
    for ((element) = ((array) != NULL) ? (array)->child : NULL; (element) != NULL; (element) = (element)->next)

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H */
