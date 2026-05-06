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

typedef struct Type {
    TypeKind kind;
    char *struct_name;       // for TYPE_STRUCT
    struct Type *elem_type;  // for TYPE_LIST: element type
    struct Type *key_type;   // for TYPE_MAP: key type
    struct Type *val_type;   // for TYPE_MAP: value type
} Type;

#endif
