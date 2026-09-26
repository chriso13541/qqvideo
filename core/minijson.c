#include "minijson.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *p;
} mj_parser_t;

static void skip_ws(mj_parser_t *P) {
    while (*P->p && isspace((unsigned char)*P->p)) P->p++;
}

static mj_value_t *mj_alloc(mj_type_t t) {
    mj_value_t *v = (mj_value_t *)calloc(1, sizeof(mj_value_t));
    v->type = t;
    return v;
}

static mj_value_t *parse_value(mj_parser_t *P);

static char *parse_raw_string(mj_parser_t *P) {
    if (*P->p != '"') return NULL;
    P->p++;
    const char *start = P->p;
    size_t len = 0;
    while (P->p[len] && P->p[len] != '"') {
        if (P->p[len] == '\\' && P->p[len+1]) len++;
        len++;
    }
    char *out = (char *)malloc(len + 1);
    size_t oi = 0;
    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        if (c == '\\' && i + 1 < len) {
            i++;
            char esc = start[i];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                default: c = esc; break;
            }
        }
        out[oi++] = c;
    }
    out[oi] = '\0';
    P->p = start + len;
    if (*P->p == '"') P->p++;
    return out;
}

static mj_value_t *parse_string(mj_parser_t *P) {
    mj_value_t *v = mj_alloc(MJ_STRING);
    v->v.string = parse_raw_string(P);
    return v;
}

static mj_value_t *parse_number(mj_parser_t *P) {
    const char *start = P->p;
    if (*P->p == '-') P->p++;
    while (isdigit((unsigned char)*P->p)) P->p++;
    if (*P->p == '.') { P->p++; while (isdigit((unsigned char)*P->p)) P->p++; }
    mj_value_t *v = mj_alloc(MJ_NUMBER);
    v->v.number = strtod(start, NULL);
    return v;
}

static mj_value_t *parse_array(mj_parser_t *P) {
    mj_value_t *v = mj_alloc(MJ_ARRAY);
    P->p++; /* [ */
    skip_ws(P);
    int cap = 8, n = 0;
    mj_value_t **items = (mj_value_t **)malloc(sizeof(mj_value_t *) * cap);
    if (*P->p == ']') { P->p++; v->v.array.items = items; v->v.array.count = 0; return v; }
    while (1) {
        skip_ws(P);
        mj_value_t *item = parse_value(P);
        if (!item) break;
        if (n == cap) { cap *= 2; items = (mj_value_t **)realloc(items, sizeof(mj_value_t *) * cap); }
        items[n++] = item;
        skip_ws(P);
        if (*P->p == ',') { P->p++; continue; }
        break;
    }
    skip_ws(P);
    if (*P->p == ']') P->p++;
    v->v.array.items = items;
    v->v.array.count = n;
    return v;
}

static mj_value_t *parse_object(mj_parser_t *P) {
    mj_value_t *v = mj_alloc(MJ_OBJECT);
    P->p++; /* { */
    skip_ws(P);
    mj_member_t *head = NULL, *tail = NULL;
    if (*P->p == '}') { P->p++; v->v.object_members = NULL; return v; }
    while (1) {
        skip_ws(P);
        if (*P->p != '"') break;
        char *key = parse_raw_string(P);
        skip_ws(P);
        if (*P->p == ':') P->p++;
        skip_ws(P);
        mj_value_t *val = parse_value(P);
        mj_member_t *m = (mj_member_t *)malloc(sizeof(mj_member_t));
        m->key = key;
        m->value = val;
        m->next = NULL;
        if (!head) head = m; else tail->next = m;
        tail = m;
        skip_ws(P);
        if (*P->p == ',') { P->p++; continue; }
        break;
    }
    skip_ws(P);
    if (*P->p == '}') P->p++;
    v->v.object_members = head;
    return v;
}

static mj_value_t *parse_value(mj_parser_t *P) {
    skip_ws(P);
    if (*P->p == '"') return parse_string(P);
    if (*P->p == '{') return parse_object(P);
    if (*P->p == '[') return parse_array(P);
    if (*P->p == '-' || isdigit((unsigned char)*P->p)) return parse_number(P);
    if (strncmp(P->p, "true", 4) == 0) { P->p += 4; mj_value_t *v = mj_alloc(MJ_BOOL); v->v.boolean = 1; return v; }
    if (strncmp(P->p, "false", 5) == 0) { P->p += 5; mj_value_t *v = mj_alloc(MJ_BOOL); v->v.boolean = 0; return v; }
    if (strncmp(P->p, "null", 4) == 0) { P->p += 4; return mj_alloc(MJ_NULL); }
    return NULL;
}

mj_value_t *mj_parse(const char *text) {
    mj_parser_t P;
    P.p = text;
    skip_ws(&P);
    return parse_value(&P);
}

mj_value_t *mj_get(const mj_value_t *obj, const char *key) {
    if (!obj || obj->type != MJ_OBJECT) return NULL;
    for (mj_member_t *m = obj->v.object_members; m; m = m->next)
        if (strcmp(m->key, key) == 0) return m->value;
    return NULL;
}

const char *mj_get_string(const mj_value_t *obj, const char *key, const char *fallback) {
    mj_value_t *v = mj_get(obj, key);
    if (v && v->type == MJ_STRING) return v->v.string;
    return fallback;
}

int mj_get_int(const mj_value_t *obj, const char *key, int fallback) {
    mj_value_t *v = mj_get(obj, key);
    if (v && v->type == MJ_NUMBER) return (int)v->v.number;
    return fallback;
}

void mj_foreach_string(const mj_value_t *array_val, void (*cb)(const char *s, void *ud), void *ud) {
    if (!array_val || array_val->type != MJ_ARRAY) return;
    for (int i = 0; i < array_val->v.array.count; i++) {
        mj_value_t *item = array_val->v.array.items[i];
        if (item->type == MJ_STRING) cb(item->v.string, ud);
    }
}

void mj_free(mj_value_t *v) {
    if (!v) return;
    switch (v->type) {
        case MJ_STRING: free(v->v.string); break;
        case MJ_ARRAY:
            for (int i = 0; i < v->v.array.count; i++) mj_free(v->v.array.items[i]);
            free(v->v.array.items);
            break;
        case MJ_OBJECT: {
            mj_member_t *m = v->v.object_members;
            while (m) {
                mj_member_t *next = m->next;
                free(m->key);
                mj_free(m->value);
                free(m);
                m = next;
            }
            break;
        }
        default: break;
    }
    free(v);
}
