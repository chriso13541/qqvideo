/*
 * minijson.h / minijson.c
 *
 * Deliberately tiny JSON reader -- just enough to parse plugin manifest
 * files (objects, arrays of strings, strings, numbers, booleans). Not a
 * general-purpose JSON library. No external dependencies on purpose: a
 * plugin manifest parser is part of the trusted core, so it stays small
 * and auditable rather than pulling in a third-party parsing library.
 */
#ifndef LUMEN_MINIJSON_H
#define LUMEN_MINIJSON_H

typedef enum {
    MJ_NULL, MJ_BOOL, MJ_NUMBER, MJ_STRING, MJ_ARRAY, MJ_OBJECT
} mj_type_t;

typedef struct mj_value mj_value_t;

typedef struct mj_member {
    char        *key;
    mj_value_t  *value;
    struct mj_member *next;
} mj_member_t;

struct mj_value {
    mj_type_t type;
    union {
        int          boolean;
        double       number;
        char        *string;
        struct { mj_value_t **items; int count; } array;
        mj_member_t *object_members;
    } v;
};

/* Parse a NUL-terminated JSON document. Returns NULL on parse error. */
mj_value_t *mj_parse(const char *text);

/* Lookup helpers. Return NULL if missing or wrong type. */
mj_value_t *mj_get(const mj_value_t *obj, const char *key);
const char *mj_get_string(const mj_value_t *obj, const char *key, const char *fallback);
int         mj_get_int(const mj_value_t *obj, const char *key, int fallback);

/* Iterate a string array value (e.g. claims.fourccs). cb returns 0 to continue. */
void mj_foreach_string(const mj_value_t *array_val, void (*cb)(const char *s, void *ud), void *ud);

void mj_free(mj_value_t *v);

#endif
