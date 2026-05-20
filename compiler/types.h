#ifndef TYPES_H
#define TYPES_H

typedef enum {
    TYPE_INT,
    TYPE_STRING,        // Result of operator + on String operands. The
                        // concrete carrier is `String` from std/string;
                        // this kind exists because the binary `+`
                        // typing path predates the struct-based
                        // representation (see audit-plan-2026-05.md
                        // T06; was previously named TYPE_STR with a
                        // stale "no longer produced" comment).
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_LIST,
    TYPE_MAP,
    TYPE_STRUCT,
    TYPE_TASK,
    TYPE_ARRAY,        // α3: typed-storage primitive `array of T`.
                       // Distinct from TYPE_LIST (which is the
                       // legacy keyword-form list runtime).
    TYPE_BYTE,         // α8: a byte (8-bit unsigned int). Used only
                       // as the element type of `array of byte`.
                       // At the value level, byte values flow as int.
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
