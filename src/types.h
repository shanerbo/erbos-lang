#ifndef TYPES_H
#define TYPES_H

typedef enum {
    TYPE_INT,
    TYPE_STR,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_LIST,
    TYPE_MAP,
    TYPE_STRUCT,
    TYPE_TASK,
    TYPE_UNKNOWN,
} TypeKind;

typedef struct {
    TypeKind kind;
    char *struct_name;  // for TYPE_STRUCT
} Type;

#endif
