#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>        // access(2), realpath()
#include <mach-o/dyld.h>   // _NSGetExecutablePath (macOS)
#include "lexer.h"
#include "parser.h"
#include "monomorph.h"
#include "checker.h"
#include "optimizer.h"
#include "irgen.h"
#include "iropt.h"
#include "regalloc.h"
#include "iremit.h"
#include "runtime_emit.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

// Locate the directory containing this compiler binary. On macOS,
// _NSGetExecutablePath returns the path; we strip the trailing
// filename to get the directory.
//
// This is the canonical "where is std/" anchor: the bundled stdlib
// lives in `<compiler-dir>/std/` (in a build tree, that's the project
// root; in an installed layout, it'd be wherever the install put the
// stdlib next to the binary). Resolving stdlib relative to the binary
// — rather than to cwd — lets users run `erbos run /any/path/file.ptt`
// from any directory and still find `std/string`, `std/list`, etc.
//
// Returns 1 on success and writes the directory path (with trailing
// `/`) into `out`. Returns 0 if the path can't be resolved (in which
// case callers should fall back to cwd-relative lookup so the
// existing dev workflow keeps working).
static int compiler_dir(char *out, size_t out_size) {
    char raw[1024];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return 0;
    // Resolve any symlinks/.. so the resulting path is canonical;
    // realpath() handles both. Falling back to the raw path when
    // realpath fails is fine — `_NSGetExecutablePath` typically
    // returns a usable absolute path even before resolution.
    char resolved[1024];
    const char *src = realpath(raw, resolved) ? resolved : raw;
    // Strip the binary filename to get the directory. Include the
    // trailing `/` so callers can `snprintf("%s%s", dir, suffix)`.
    const char *last_slash = strrchr(src, '/');
    if (!last_slash) return 0;
    size_t dir_len = (size_t)(last_slash - src) + 1; // include '/'
    if (dir_len + 1 > out_size) return 0;
    memcpy(out, src, dir_len);
    out[dir_len] = '\0';
    return 1;
}

// Walk up from the source file's directory looking for `potato.toml`.
// First ancestor that contains the marker is treated as the project
// root; the path is written into `out` (with trailing `/`) and 1 is
// returned. Returns 0 if no marker is found anywhere up to the
// filesystem root.
//
// `potato.toml` is currently consulted only as a marker — its
// content isn't read. The empty file is enough to signal "this
// directory is the root of a Potato project." The marker filename
// will eventually carry build/dependency metadata; doing the
// lookup now means programs in proper project layouts already get
// stable resolution without any further user action when content
// arrives.
static int find_project_root(const char *source_file, char *out, size_t out_size) {
    // Build absolute, normalised path to the source file's directory.
    char abs[1024];
    if (!realpath(source_file, abs)) {
        // realpath fails for non-existent paths; for a missing
        // source the rest of the pipeline will produce a clearer
        // error than we can here. Bail.
        return 0;
    }
    // Strip the filename component to get the directory.
    char *last_slash = strrchr(abs, '/');
    if (!last_slash) return 0;
    *last_slash = '\0'; // abs is now the directory
    // Walk up. Stop at filesystem root ("/" — i.e., abs == "" after
    // we strip the trailing slash one final time).
    while (abs[0] != '\0') {
        char marker[1024];
        snprintf(marker, sizeof(marker), "%s/potato.toml", abs);
        if (access(marker, F_OK) == 0) {
            // Found it. Return the directory with trailing slash so
            // callers can `snprintf("%s%s", root, suffix)`.
            size_t len = strlen(abs);
            if (len + 2 > out_size) return 0;
            memcpy(out, abs, len);
            out[len] = '/';
            out[len + 1] = '\0';
            return 1;
        }
        // Step up one directory.
        char *up = strrchr(abs, '/');
        if (!up) break;
        *up = '\0';
    }
    return 0;
}

// Rewrite NODE_CALL sites within an imported function's body so a
// bare `swap(...)` call against another free function in the same
// imported file resolves to the alias-prefixed `<alias>_swap` after
// import-time renaming.
//
// Walks the AST recursively. Match is by function name only; we
// don't change method-call dispatch (methods aren't prefixed).
static void rewrite_calls_with_prefix(Node *n,
                                      char **local_names, int local_count,
                                      const char *alias) {
    if (!n) return;
    switch (n->type) {
        case NODE_CALL: {
            for (int i = 0; i < local_count; i++) {
                if (n->call.name && !strcmp(n->call.name, local_names[i])) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "%s_%s", alias, local_names[i]);
                    n->call.name = strdup(buf);
                    break;
                }
            }
            for (int i = 0; i < n->call.arg_count; i++)
                rewrite_calls_with_prefix(n->call.args[i], local_names, local_count, alias);
            break;
        }
        case NODE_METHOD_CALL:
            rewrite_calls_with_prefix(n->method_call.object, local_names, local_count, alias);
            for (int i = 0; i < n->method_call.arg_count; i++)
                rewrite_calls_with_prefix(n->method_call.args[i], local_names, local_count, alias);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < n->block.stmts.count; i++)
                rewrite_calls_with_prefix(n->block.stmts.items[i], local_names, local_count, alias);
            break;
        case NODE_VAR_DECL:
            rewrite_calls_with_prefix(n->var_decl.value, local_names, local_count, alias);
            break;
        case NODE_ASSIGN:
            rewrite_calls_with_prefix(n->assign.value, local_names, local_count, alias);
            break;
        case NODE_FIELD_ASSIGN:
            rewrite_calls_with_prefix(n->field_assign.object, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->field_assign.value, local_names, local_count, alias);
            break;
        case NODE_FIELD_ACCESS:
            rewrite_calls_with_prefix(n->field_access.object, local_names, local_count, alias);
            break;
        case NODE_BINARY:
            rewrite_calls_with_prefix(n->binary.left, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->binary.right, local_names, local_count, alias);
            break;
        case NODE_UNARY:
            rewrite_calls_with_prefix(n->unary.operand, local_names, local_count, alias);
            break;
        case NODE_INDEX:
            rewrite_calls_with_prefix(n->index_access.object, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->index_access.index, local_names, local_count, alias);
            break;
        case NODE_INDEX_ASSIGN:
            rewrite_calls_with_prefix(n->index_assign.object, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->index_assign.index, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->index_assign.value, local_names, local_count, alias);
            break;
        case NODE_IF:
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                rewrite_calls_with_prefix(n->if_stmt.conds[i], local_names, local_count, alias);
                rewrite_calls_with_prefix(n->if_stmt.bodies[i], local_names, local_count, alias);
            }
            rewrite_calls_with_prefix(n->if_stmt.nah_body, local_names, local_count, alias);
            break;
        case NODE_THROUGH_RANGE:
            rewrite_calls_with_prefix(n->through_range.from, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->through_range.to, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->through_range.by, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->through_range.body, local_names, local_count, alias);
            break;
        case NODE_THROUGH_IN:
            rewrite_calls_with_prefix(n->through_in.collection, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->through_in.body, local_names, local_count, alias);
            break;
        case NODE_INFI:
            rewrite_calls_with_prefix(n->infi.cond, local_names, local_count, alias);
            rewrite_calls_with_prefix(n->infi.body, local_names, local_count, alias);
            break;
        case NODE_GIVE:
            rewrite_calls_with_prefix(n->give.value, local_names, local_count, alias);
            break;
        case NODE_MATCH:
            rewrite_calls_with_prefix(n->match_expr.expr, local_names, local_count, alias);
            for (int i = 0; i < n->match_expr.arm_count; i++)
                rewrite_calls_with_prefix(n->match_expr.arm_bodies[i], local_names, local_count, alias);
            break;
        case NODE_LIST_LIT:
            for (int i = 0; i < n->list_lit.count; i++)
                rewrite_calls_with_prefix(n->list_lit.items[i], local_names, local_count, alias);
            break;
        case NODE_MAP_LIT:
            for (int i = 0; i < n->map_lit.count; i++) {
                rewrite_calls_with_prefix(n->map_lit.keys[i], local_names, local_count, alias);
                rewrite_calls_with_prefix(n->map_lit.values[i], local_names, local_count, alias);
            }
            break;
        case NODE_ASSERT:
            rewrite_calls_with_prefix(n->assert_stmt.condition, local_names, local_count, alias);
            break;
        default:
            break;
    }
}

// Codex P1-11 round 3: assign every loaded module a unique
// canonical alias derived from its resolved file path. Two
// transitive `use helper as h` declarations from sibling dirs
// resolve to *different files*; both must emit their free
// functions under distinct symbol prefixes or the linker collides
// on `_h_<func>`. The user-written alias is purely lexical;
// canonical aliases are program-wide and stable.
//
// Strategy: a global path → canonical_alias map. When we encounter
// a `use X as A` (top-level or transitive), resolve X against the
// importer's dir, hand the resolved path to `assign_canonical_alias`,
// and rewrite `A.X()` call sites in the importer's body to use the
// canonical alias. The existing `<alias>_<func>` symbol-prefix
// scheme then naturally produces `<canonical>_<func>` symbols, one
// per resolved file.
typedef struct {
    char **paths;
    char **canonical;
    int count;
    int cap;
} CanonAliasMap;

static CanonAliasMap g_canon = {0};

// Look up or assign a canonical alias for a resolved file path.
// Returned string is owned by the map and lives until program exit.
static const char *canonical_alias_for(const char *resolved_path) {
    for (int i = 0; i < g_canon.count; i++) {
        if (!strcmp(g_canon.paths[i], resolved_path)) return g_canon.canonical[i];
    }
    if (g_canon.count >= g_canon.cap) {
        g_canon.cap = g_canon.cap ? g_canon.cap * 2 : 8;
        g_canon.paths = realloc(g_canon.paths, g_canon.cap * sizeof(char *));
        g_canon.canonical = realloc(g_canon.canonical, g_canon.cap * sizeof(char *));
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "m%d", g_canon.count);
    int idx = g_canon.count++;
    g_canon.paths[idx] = strdup(resolved_path);
    g_canon.canonical[idx] = strdup(buf);
    return g_canon.canonical[idx];
}

// Walk a function body and rewrite any NODE_METHOD_CALL whose
// receiver is a bare NODE_IDENT matching one of `aliases[i]`,
// replacing the IDENT name with `canonical_aliases[i]`. This is
// alias-rewriting for module-qualified calls (`h.foo()` →
// `<canon>.foo()`).
//
// Codex P1-11 round 4: scope-aware. The walker tracks names
// introduced by NODE_VAR_DECL, function parameters, range/iter
// loop vars, and match arm bindings. If a local with the same
// name is in scope at the call site, the IDENT receiver refers
// to that local (and the method call must dispatch on its
// runtime type), not to the import alias — so do not rewrite.
//
// The scope is a flat name list with push/pop. Block scopes save
// the count on entry and restore on exit, mirroring the checker's
// scope handling.
typedef struct {
    char **names;
    int count;
    int cap;
} ScopeStack;

static void scope_push(ScopeStack *s, const char *name) {
    if (!name) return;
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->names = realloc(s->names, s->cap * sizeof(char *));
    }
    s->names[s->count++] = strdup(name);
}

static void scope_truncate(ScopeStack *s, int new_count) {
    while (s->count > new_count) {
        s->count--;
        free(s->names[s->count]);
    }
}

static int scope_contains(const ScopeStack *s, const char *name) {
    if (!name) return 0;
    for (int i = s->count - 1; i >= 0; i--) {
        if (!strcmp(s->names[i], name)) return 1;
    }
    return 0;
}

static void rewrite_alias_idents_walk(Node *n,
                                      char **aliases,
                                      char **canonicals,
                                      int alias_count,
                                      ScopeStack *scope);

static void rewrite_alias_idents_walk(Node *n,
                                      char **aliases,
                                      char **canonicals,
                                      int alias_count,
                                      ScopeStack *scope) {
    if (!n) return;
    switch (n->type) {
        case NODE_METHOD_CALL: {
            // Rewrite the receiver if it's a bare alias IDENT and
            // no in-scope local shadows it. Stash the user-written
            // alias in `method_call.alias_display` so checker
            // diagnostics report the user's name rather than the
            // canonical `m<N>` synthetic.
            Node *obj = n->method_call.object;
            if (obj && obj->type == NODE_IDENT && obj->ident.name &&
                !scope_contains(scope, obj->ident.name)) {
                for (int i = 0; i < alias_count; i++) {
                    if (!strcmp(obj->ident.name, aliases[i])) {
                        if (!n->method_call.alias_display) {
                            n->method_call.alias_display = strdup(aliases[i]);
                        }
                        obj->ident.name = strdup(canonicals[i]);
                        break;
                    }
                }
            }
            rewrite_alias_idents_walk(obj, aliases, canonicals, alias_count, scope);
            for (int i = 0; i < n->method_call.arg_count; i++)
                rewrite_alias_idents_walk(n->method_call.args[i], aliases, canonicals, alias_count, scope);
            break;
        }
        case NODE_CALL:
            for (int i = 0; i < n->call.arg_count; i++)
                rewrite_alias_idents_walk(n->call.args[i], aliases, canonicals, alias_count, scope);
            break;
        case NODE_BLOCK: {
            int saved = scope->count;
            for (int i = 0; i < n->block.stmts.count; i++)
                rewrite_alias_idents_walk(n->block.stmts.items[i], aliases, canonicals, alias_count, scope);
            scope_truncate(scope, saved);
            break;
        }
        case NODE_VAR_DECL:
            // Walk the initializer in the *outer* scope (the new
            // name isn't in scope yet) — `x is some.expr()` reads
            // `some` before `x` exists. Then introduce the name
            // for any subsequent statements in the enclosing block.
            rewrite_alias_idents_walk(n->var_decl.value, aliases, canonicals, alias_count, scope);
            scope_push(scope, n->var_decl.name);
            break;
        case NODE_ASSIGN:
            rewrite_alias_idents_walk(n->assign.value, aliases, canonicals, alias_count, scope);
            break;
        case NODE_FIELD_ASSIGN:
            rewrite_alias_idents_walk(n->field_assign.object, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->field_assign.value, aliases, canonicals, alias_count, scope);
            break;
        case NODE_FIELD_ACCESS:
            rewrite_alias_idents_walk(n->field_access.object, aliases, canonicals, alias_count, scope);
            break;
        case NODE_BINARY:
            rewrite_alias_idents_walk(n->binary.left, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->binary.right, aliases, canonicals, alias_count, scope);
            break;
        case NODE_UNARY:
            rewrite_alias_idents_walk(n->unary.operand, aliases, canonicals, alias_count, scope);
            break;
        case NODE_INDEX:
            rewrite_alias_idents_walk(n->index_access.object, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->index_access.index, aliases, canonicals, alias_count, scope);
            break;
        case NODE_INDEX_ASSIGN:
            rewrite_alias_idents_walk(n->index_assign.object, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->index_assign.index, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->index_assign.value, aliases, canonicals, alias_count, scope);
            break;
        case NODE_IF:
            for (int i = 0; i < n->if_stmt.branch_count; i++) {
                rewrite_alias_idents_walk(n->if_stmt.conds[i], aliases, canonicals, alias_count, scope);
                rewrite_alias_idents_walk(n->if_stmt.bodies[i], aliases, canonicals, alias_count, scope);
            }
            rewrite_alias_idents_walk(n->if_stmt.nah_body, aliases, canonicals, alias_count, scope);
            break;
        case NODE_THROUGH_RANGE: {
            // Range expressions (from/to/by) are evaluated before
            // the loop var enters scope. Body sees the loop var.
            rewrite_alias_idents_walk(n->through_range.from, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->through_range.to,   aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->through_range.by,   aliases, canonicals, alias_count, scope);
            int saved = scope->count;
            scope_push(scope, n->through_range.var_name);
            rewrite_alias_idents_walk(n->through_range.body, aliases, canonicals, alias_count, scope);
            scope_truncate(scope, saved);
            break;
        }
        case NODE_THROUGH_IN: {
            rewrite_alias_idents_walk(n->through_in.collection, aliases, canonicals, alias_count, scope);
            int saved = scope->count;
            scope_push(scope, n->through_in.var_name);
            rewrite_alias_idents_walk(n->through_in.body, aliases, canonicals, alias_count, scope);
            scope_truncate(scope, saved);
            break;
        }
        case NODE_INFI:
            rewrite_alias_idents_walk(n->infi.cond, aliases, canonicals, alias_count, scope);
            rewrite_alias_idents_walk(n->infi.body, aliases, canonicals, alias_count, scope);
            break;
        case NODE_GIVE:
            rewrite_alias_idents_walk(n->give.value, aliases, canonicals, alias_count, scope);
            break;
        case NODE_MATCH: {
            rewrite_alias_idents_walk(n->match_expr.expr, aliases, canonicals, alias_count, scope);
            for (int i = 0; i < n->match_expr.arm_count; i++) {
                int saved = scope->count;
                int bcount = n->match_expr.arm_binding_counts ? n->match_expr.arm_binding_counts[i] : 0;
                if (n->match_expr.arm_bindings && n->match_expr.arm_bindings[i]) {
                    for (int b = 0; b < bcount; b++) {
                        scope_push(scope, n->match_expr.arm_bindings[i][b]);
                    }
                }
                rewrite_alias_idents_walk(n->match_expr.arm_bodies[i], aliases, canonicals, alias_count, scope);
                scope_truncate(scope, saved);
            }
            break;
        }
        case NODE_LIST_LIT:
            for (int i = 0; i < n->list_lit.count; i++)
                rewrite_alias_idents_walk(n->list_lit.items[i], aliases, canonicals, alias_count, scope);
            break;
        case NODE_MAP_LIT:
            for (int i = 0; i < n->map_lit.count; i++) {
                rewrite_alias_idents_walk(n->map_lit.keys[i], aliases, canonicals, alias_count, scope);
                rewrite_alias_idents_walk(n->map_lit.values[i], aliases, canonicals, alias_count, scope);
            }
            break;
        case NODE_ASSERT:
            rewrite_alias_idents_walk(n->assert_stmt.condition, aliases, canonicals, alias_count, scope);
            break;
        default:
            break;
    }
}

// Rewrite a function body, seeding the scope with parameter names
// so a function with `(math int)` doesn't have its `math.foo()`
// rewritten as a module call. Aliases that match nothing in scope
// or in the file's import list are left alone (the checker will
// surface "undefined variable" for genuinely unknown receivers).
static void rewrite_alias_idents_in_body(Node *body,
                                         char **param_names, int param_count,
                                         char **aliases,
                                         char **canonicals,
                                         int alias_count) {
    if (!body) return;
    ScopeStack scope = {0};
    for (int i = 0; i < param_count; i++) {
        scope_push(&scope, param_names[i]);
    }
    rewrite_alias_idents_walk(body, aliases, canonicals, alias_count, &scope);
    scope_truncate(&scope, 0);
    free(scope.names);
}

// Resolve a `use <use_path>` against the three-tier search order
// (importer-sibling, project-root, compiler-binary-dir). Writes the
// resolved absolute-or-relative file path into `out`. Returns 1 on
// success, 0 on failure.
//
// `importer_dir` is the directory of the source file that contains
// the `use` declaration — for top-level `use`s in the input this
// is the input file's dir; for transitive `use`s it's the dir of
// the imported file.
//
// `top_input` is the original input passed to `erbos`; only used
// to anchor `find_project_root` (the project root walk needs an
// absolute starting point that's stable across the import tree).
static int resolve_use_path(const char *use_path,
                            const char *importer_dir,
                            const char *top_input,
                            char *out, size_t out_size) {
    char candidate[512];
    // 1. importer-sibling
    snprintf(candidate, sizeof(candidate), "%s%s.ptt", importer_dir, use_path);
    FILE *f = fopen(candidate, "r");
    if (f) {
        fclose(f);
        if (strlen(candidate) + 1 > out_size) return 0;
        strcpy(out, candidate);
        return 1;
    }
    // 2. project-root
    char proj_root[1024];
    if (find_project_root(top_input, proj_root, sizeof(proj_root))) {
        snprintf(candidate, sizeof(candidate), "%s%s.ptt", proj_root, use_path);
        f = fopen(candidate, "r");
        if (f) {
            fclose(f);
            if (strlen(candidate) + 1 > out_size) return 0;
            strcpy(out, candidate);
            return 1;
        }
    }
    // 3. compiler-binary-dir (stdlib)
    char comp_dir[1024];
    if (compiler_dir(comp_dir, sizeof(comp_dir))) {
        snprintf(candidate, sizeof(candidate), "%s%s.ptt", comp_dir, use_path);
        f = fopen(candidate, "r");
        if (f) {
            fclose(f);
            if (strlen(candidate) + 1 > out_size) return 0;
            strcpy(out, candidate);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    // Recognize a --help-style ask before anything else. Without
    // this, `erbos --help` falls into the .ptt-input path and dies
    // with a confusing "expected .ptt file, got '--help'" error.
    int want_help = (argc < 2);
    for (int hi = 1; hi < argc; hi++) {
        if (!strcmp(argv[hi], "--help") ||
            !strcmp(argv[hi], "-h") ||
            !strcmp(argv[hi], "help")) {
            want_help = 1;
            break;
        }
    }
    if (want_help) {
        FILE *out = (argc < 2) ? stderr : stdout;
        fprintf(out,
            "usage: erbos [-O0|-O1|-O2] <file.ptt>      # build to binary\n"
            "       erbos [-O0|-O1|-O2] run <file.ptt>  # build and run, then clean up\n"
            "       erbos [-O0|-O1|-O2] test <file.ptt> # same as run; the test framework runs in the binary\n"
            "       erbos [-O0|-O1|-O2] ir <file.ptt>   # emit the .s only, don't assemble\n"
            "\n"
            "  -O0  skip iropt entirely (no IR-level transformations)\n"
            "  -O1  default — every iropt pass runs\n"
            "  -O2  reserved for tuning; identical to -O1 today\n");
        return (argc < 2) ? 1 : 0;
    }

    // First pass: extract any -O0/-O1/-O2 anywhere in argv. Default
    // is -O1 if no flag is given. The flag may appear before or
    // after the subcommand (`-O0 run file.ptt` and `run -O0 file.ptt`
    // are equivalent), since CLI ergonomics shouldn't depend on
    // memorising flag-vs-subcommand order. After this pass, argv is
    // compacted to remove the consumed flag(s).
    IROptLevel opt_level = IROPT_O1;
    int j = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O0"))      opt_level = IROPT_O0;
        else if (!strcmp(argv[i], "-O1")) opt_level = IROPT_O1;
        else if (!strcmp(argv[i], "-O2")) opt_level = IROPT_O2;
        else                              argv[j++] = argv[i];
    }
    argc = j;

    // Subcommand parsing. The IR backend is the only backend now;
    // `run` / `test` add execute-and-cleanup, `ir` stops after .s
    // emission. The earlier `legacy` opt-out (for the retired direct
    // codegen, src/codegen.c) was removed in #34 P4.3g.
    int run_mode = 0;     // run binary after linking, then delete it
    int ir_only = 0;      // stop after generating .s (no assemble/link/run)
    const char *input = NULL;

    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "test") == 0) {
        run_mode = 1;
        input = argv[2];
    } else if (argc >= 3 && strcmp(argv[1], "ir") == 0) {
        ir_only = 1;
        input = argv[2];
    } else if (argc >= 2) {
        input = argv[1];
    }
    if (!input) {
        fprintf(stderr, "error: no input file\n");
        return 1;
    }

    // Check file extension
    const char *ext = strrchr(input, '.');
    if (!ext || strcmp(ext, ".ptt") != 0) {
        fprintf(stderr, "error: expected .ptt file, got '%s'\n", input);
        return 1;
    }

    // Derive output name: replace .ptt with nothing
    char out_name[256];
    strncpy(out_name, input, sizeof(out_name) - 1);
    char *dot = strrchr(out_name, '.');
    if (dot) *dot = '\0';

    char asm_path[256];
    if (ir_only) {
        // `erbos ir <path>/foo.ptt` should write `foo.s` to cwd, not
        // dirty the source directory with build artifacts. Strip any
        // leading directory from the path. (Build mode keeps the
        // source-dir layout for `out_name` because the resulting
        // binary is meaningful at that location.)
        const char *base = strrchr(out_name, '/');
        base = base ? base + 1 : out_name;
        snprintf(asm_path, sizeof(asm_path), "%s.s", base);
    } else {
        snprintf(asm_path, sizeof(asm_path), "%s.s", out_name);
    }

    // Lex
    char *src = read_file(input);
    Lexer l;
    lexer_init(&l, src);
    lexer_tokenize(&l);

    // Parse
    Parser p;
    parser_init(&p, &l);
    p.filename = input;
    Node *program = parser_parse(&p);

    // Resolve imports. Track already-loaded paths so a file that's
    // imported transitively from multiple places doesn't get merged
    // multiple times (which would produce duplicate function/method
    // entries and a duplicate-symbol assembly error).
    //
    // Note: `std/string` used to be auto-loaded into every program
    // because String literals (`"..."`) and operator dispatch on
    // String (`+`, `eq`) lowered to `_String_*` symbols defined in
    // that file. The auto-load is gone — every file using String
    // must write `use std/string` explicitly. This makes the import
    // surface honest (no hidden module-loads) and matches every
    // other stdlib type. Programs that don't touch strings don't
    // pay for the load.
    int loaded_cap = 8;
    int loaded_count = 0;
    char **loaded_paths = malloc(loaded_cap * sizeof(char *));

    // Codex P1-11: each `use` resolves against the directory of the
    // file that *contains* it, not the top-level input file. Track
    // the originating dir parallel to use_paths so transitive
    // imports use their importer's dir as the sibling base. Without
    // this, `lib/a.ptt` doing `use helper` looks for
    // `<input-dir>/helper.ptt` instead of `<lib>/helper.ptt`.
    //
    // The Program AST today carries use_paths + use_aliases but no
    // origin info; we maintain origin_dirs only in main.c — it's a
    // codegen-time concern and adding a third Node field would
    // require sweeping the AST.
    char **use_origin_dirs = malloc(program->program.use_count * sizeof(char *));
    char top_dir[512] = {0};
    {
        strncpy(top_dir, input, sizeof(top_dir) - 1);
        char *ts = strrchr(top_dir, '/');
        if (ts) *(ts + 1) = '\0';
        else top_dir[0] = '\0';
    }
    for (int ui = 0; ui < program->program.use_count; ui++) {
        use_origin_dirs[ui] = strdup(top_dir);
    }

    // Codex P1-12: catch the same module imported under two
    // aliases up front. Pre-fix: dedupe was path-only, so
    //   use lib as a
    //   use lib as b
    // loaded the file once (under whichever alias's `use` ran
    // first), then `b.greet()` failed at the checker (or worse,
    // at the linker) because functions were emitted with `_a_`
    // prefixes only. Reject with a clear error pointing at the
    // duplicate.
    for (int i = 0; i < program->program.use_count; i++) {
        for (int j = i + 1; j < program->program.use_count; j++) {
            if (!strcmp(program->program.use_paths[i],
                        program->program.use_paths[j]) &&
                strcmp(program->program.use_aliases[i],
                       program->program.use_aliases[j]) != 0) {
                fprintf(stderr,
                    "error: module '%s' imported twice under different "
                    "aliases ('%s' and '%s')\n",
                    program->program.use_paths[i],
                    program->program.use_aliases[i],
                    program->program.use_aliases[j]);
                fprintf(stderr,
                    "  note: each module loads once; only the first alias "
                    "is bound to its free functions.\n");
                fprintf(stderr,
                    "  help: pick a single alias for `%s`\n",
                    program->program.use_paths[i]);
                return 1;
            }
        }
    }

    // Codex P1-11 round 3: rewrite top-level alias-qualified call
    // sites to use the canonical alias for each resolved module.
    // Without this, `lib1/a.ptt` and `lib2/b.ptt` both writing
    // `use helper as h` get their `h.foo()` calls emitted as
    // `_h_foo` — which collide at assembly when both lib1/helper
    // and lib2/helper define `foo`. The user-written `h` stays in
    // the source file; it's just translated to a unique canonical
    // string before the checker sees it.
    //
    // For the top-level input, the alias map is `<user_alias> →
    // canonical_alias_for(resolved_path)`. We apply it to every
    // function body and to each `use_aliases[ui]` slot so the
    // existing function-prefixing pass below picks up the
    // canonical name.
    {
        int n_uses = program->program.use_count;
        if (n_uses > 0) {
            char **user_aliases = malloc(n_uses * sizeof(char *));
            char **canon_aliases = malloc(n_uses * sizeof(char *));
            int valid = 0;
            for (int ui = 0; ui < n_uses; ui++) {
                char rp[512];
                if (!resolve_use_path(program->program.use_paths[ui],
                                      use_origin_dirs[ui], input,
                                      rp, sizeof(rp))) {
                    // Defer the resolution-failure diagnostic to the
                    // load loop below (it has the structured help).
                    // For now, skip this entry from the rewrite map.
                    continue;
                }
                user_aliases[valid] = strdup(program->program.use_aliases[ui]);
                canon_aliases[valid] = strdup(canonical_alias_for(rp));
                free(program->program.use_aliases[ui]);
                program->program.use_aliases[ui] = strdup(canon_aliases[valid]);
                valid++;
            }
            for (int fi = 0; fi < program->program.funcs.count; fi++) {
                Node *f = program->program.funcs.items[fi];
                if (f->func_def.body) {
                    rewrite_alias_idents_in_body(f->func_def.body,
                                                 f->func_def.param_names,
                                                 f->func_def.param_count,
                                                 user_aliases, canon_aliases, valid);
                }
            }
            // Also rewrite test bodies (test "name" { ... }). Tests
            // have no parameters, so the seed scope is empty.
            for (int ti = 0; ti < program->program.tests.count; ti++) {
                Node *t = program->program.tests.items[ti];
                if (t && t->test_def.body) {
                    rewrite_alias_idents_in_body(t->test_def.body,
                                                 NULL, 0,
                                                 user_aliases, canon_aliases, valid);
                }
            }
            for (int i = 0; i < valid; i++) {
                free(user_aliases[i]);
                free(canon_aliases[i]);
            }
            free(user_aliases);
            free(canon_aliases);
        }
    }

    for (int ui = 0; ui < program->program.use_count; ui++) {
        // Sibling base is the originating file's directory, recorded
        // when the `use` was queued. For top-level `use`s in the
        // input file this equals strrchr(input)/; for transitive
        // imports it's the directory of the file that contained the
        // nested `use`.
        const char *dir = use_origin_dirs[ui];
        const char *upath_resolved = program->program.use_paths[ui];

        // Codex P1-11 (round 2): dedupe is keyed on the *resolved*
        // file path, not the raw `use` text. Two transitive
        // `use helper` declarations from sibling dirs lib1/ and
        // lib2/ resolve to different files (lib1/helper.ptt and
        // lib2/helper.ptt) and must both load. The previous raw-
        // text dedupe collapsed them into a single load and
        // dropped the symbols from whichever helper.ptt was
        // visited second.
        char import_path[512];
        if (!resolve_use_path(upath_resolved, dir, input,
                              import_path, sizeof(import_path))) {
            // Surface a structured error that adapts to which
            // resolver tier failed, so the user knows the next
            // step instead of just "cannot find module."
            int starts_std = (strncmp(upath_resolved, "std/", 4) == 0);
            char proj_root_check[1024];
            int has_project_root = find_project_root(input, proj_root_check,
                                                     sizeof(proj_root_check));
            fprintf(stderr, "error: cannot resolve `use %s`\n",
                upath_resolved);
            if (starts_std) {
                // Stdlib lookup failed — almost always means the
                // compiler can't find `std/` next to its binary
                // (broken install) OR a typo in the module name.
                char comp_dir_show[1024];
                if (compiler_dir(comp_dir_show, sizeof(comp_dir_show))) {
                    fprintf(stderr, "  searched: %s%s.ptt\n",
                        comp_dir_show, upath_resolved);
                }
                fprintf(stderr, "  note: stdlib modules live next to the compiler binary\n");
                fprintf(stderr, "  help: check the spelling, or that std/ is intact next to the erbos binary\n");
            } else if (!has_project_root) {
                // Non-stdlib path AND no potato.toml — the user
                // is trying to import sibling/project code but
                // hasn't marked their project root. This is the
                // most common new-user failure.
                fprintf(stderr, "  note: no `potato.toml` found in any ancestor of %s\n",
                    input);
                fprintf(stderr, "  help: create an empty `potato.toml` at the root of your project\n");
                fprintf(stderr, "        so `use %s` resolves against `<project-root>/%s.ptt`\n",
                    upath_resolved, upath_resolved);
            } else {
                // potato.toml found but the file genuinely isn't
                // there. Show what was searched.
                fprintf(stderr, "  searched: %s%s.ptt (sibling)\n", dir, upath_resolved);
                fprintf(stderr, "            %s%s.ptt (project root)\n",
                    proj_root_check, upath_resolved);
                fprintf(stderr, "  help: check the path matches the file location relative to your project root\n");
            }
            return 1;
        }

        // Resolved-path dedupe: skip if this concrete file was
        // already loaded (whether through this `use` text or an
        // earlier transitive `use` that resolved to the same file).
        int already_loaded = 0;
        for (int li = 0; li < loaded_count; li++) {
            if (!strcmp(loaded_paths[li], import_path)) {
                already_loaded = 1;
                break;
            }
        }
        if (already_loaded) continue;
        if (loaded_count >= loaded_cap) {
            loaded_cap *= 2;
            loaded_paths = realloc(loaded_paths, loaded_cap * sizeof(char *));
        }
        loaded_paths[loaded_count++] = strdup(import_path);

        // Lex + parse the imported file
        char *imp_src = read_file(import_path);
        Lexer imp_l;
        lexer_init(&imp_l, imp_src);
        lexer_tokenize(&imp_l);
        Parser imp_p;
        parser_init(&imp_p, &imp_l);
        imp_p.filename = import_path;
        Node *imp_prog = parser_parse(&imp_p);

        // Codex P1-11 round 3: rewrite imp_prog's body's
        // alias-qualified calls to use canonical aliases derived
        // from each transitive `use`'s resolved file. Without this,
        // two distinct files lib1/a.ptt and lib2/b.ptt that both
        // wrote `use helper as h` would emit calls `h.foo()` that
        // collide on `_h_foo` after both are loaded. Each transitive
        // resolves its `use helper` against the *importer's* dir
        // (imp_dir below) so the canonical alias differs even when
        // the `use` text matches.
        char imp_dir[512] = {0};
        strncpy(imp_dir, import_path, sizeof(imp_dir) - 1);
        {
            char *ts = strrchr(imp_dir, '/');
            if (ts) *(ts + 1) = '\0';
            else imp_dir[0] = '\0';
        }
        if (imp_prog->program.use_count > 0) {
            int nu = imp_prog->program.use_count;
            char **user_aliases = malloc(nu * sizeof(char *));
            char **canon_aliases = malloc(nu * sizeof(char *));
            int valid = 0;
            for (int rui = 0; rui < nu; rui++) {
                char rp[512];
                if (!resolve_use_path(imp_prog->program.use_paths[rui],
                                      imp_dir, input,
                                      rp, sizeof(rp))) {
                    // Resolution failure surfaces in the load loop
                    // when the absorbed entry's iteration runs.
                    continue;
                }
                user_aliases[valid] = strdup(imp_prog->program.use_aliases[rui]);
                canon_aliases[valid] = strdup(canonical_alias_for(rp));
                free(imp_prog->program.use_aliases[rui]);
                imp_prog->program.use_aliases[rui] = strdup(canon_aliases[valid]);
                valid++;
            }
            for (int fi = 0; fi < imp_prog->program.funcs.count; fi++) {
                Node *f = imp_prog->program.funcs.items[fi];
                if (f->func_def.body) {
                    rewrite_alias_idents_in_body(f->func_def.body,
                                                 f->func_def.param_names,
                                                 f->func_def.param_count,
                                                 user_aliases, canon_aliases, valid);
                }
            }
            for (int i = 0; i < valid; i++) {
                free(user_aliases[i]);
                free(canon_aliases[i]);
            }
            free(user_aliases);
            free(canon_aliases);
        }

        // Merge funcs.
        //   - Free functions get prefixed with `<alias>_<name>` so the
        //     caller writes `math.max(...)` and the call-site dispatch
        //     resolves to `_math_max`.
        //   - Methods (func_def.receiver_type != NULL) keep their
        //     declared name; method dispatch is by receiver type, not
        //     by alias. `use std/string as foo` still gives `String.len`
        //     globally — `foo.len` would be nonsensical because there's
        //     no `foo` *value* to call a method on.
        //   - Within an imported file, one free function may call
        //     another free function defined in the same file by its
        //     bare name (e.g. `next_perm` calls `swap`). We collect
        //     every free-function name in the imported file first,
        //     then rewrite call sites in their bodies to point at the
        //     prefixed names. Without this, intra-file calls break
        //     after import-time renaming.
        const char *alias = program->program.use_aliases[ui];
        char prefixed[256];

        // Pass 1: collect every free-function name declared in the
        // imported file (skip methods — they keep their declared
        // name). This is the set we'll rewrite call sites to use.
        char **local_names = NULL;
        int local_count = 0;
        for (int fi = 0; fi < imp_prog->program.funcs.count; fi++) {
            Node *f = imp_prog->program.funcs.items[fi];
            if (f->func_def.receiver_type) continue;
            if (!f->func_def.name) continue;
            local_names = realloc(local_names, (local_count + 1) * sizeof(char *));
            local_names[local_count++] = strdup(f->func_def.name);
        }

        // Pass 2: rewrite intra-file call sites.
        for (int fi = 0; fi < imp_prog->program.funcs.count; fi++) {
            Node *f = imp_prog->program.funcs.items[fi];
            if (f->func_def.body) {
                rewrite_calls_with_prefix(f->func_def.body, local_names, local_count, alias);
            }
        }

        // Pass 3: rename the function decls themselves and merge.
        for (int fi = 0; fi < imp_prog->program.funcs.count; fi++) {
            Node *f = imp_prog->program.funcs.items[fi];
            if (!f->func_def.receiver_type) {
                snprintf(prefixed, sizeof(prefixed), "%s_%s", alias, f->func_def.name);
                f->func_def.name = strdup(prefixed);
            }
            if (program->program.funcs.count >= program->program.funcs.cap) {
                program->program.funcs.cap = program->program.funcs.cap ? program->program.funcs.cap * 2 : 4;
                program->program.funcs.items = realloc(program->program.funcs.items, program->program.funcs.cap * sizeof(Node *));
            }
            program->program.funcs.items[program->program.funcs.count++] = f;
        }
        for (int li = 0; li < local_count; li++) free(local_names[li]);
        free(local_names);
        // Merge structs
        for (int si = 0; si < imp_prog->program.structs.count; si++) {
            Node *s = imp_prog->program.structs.items[si];
            if (program->program.structs.count >= program->program.structs.cap) {
                program->program.structs.cap = program->program.structs.cap ? program->program.structs.cap * 2 : 4;
                program->program.structs.items = realloc(program->program.structs.items, program->program.structs.cap * sizeof(Node *));
            }
            program->program.structs.items[program->program.structs.count++] = s;
        }
        // Merge enums
        for (int ei = 0; ei < imp_prog->program.enums.count; ei++) {
            Node *e = imp_prog->program.enums.items[ei];
            if (program->program.enums.count >= program->program.enums.cap) {
                program->program.enums.cap = program->program.enums.cap ? program->program.enums.cap * 2 : 4;
                program->program.enums.items = realloc(program->program.enums.items, program->program.enums.cap * sizeof(Node *));
            }
            program->program.enums.items[program->program.enums.count++] = e;
        }
        // Recursively absorb the imported file's own `use` directives
        // into the parent program. This is what makes
        // `use std/string_map` (which itself does `use std/string`)
        // pull `String` into the parent program's struct table — no
        // explicit re-import needed at the top of the user's file.
        //
        // Codex P1-11 (round 2): dedupe at absorption time keys on
        // the full (use_path, origin_dir, alias) tuple. The raw
        // `use_path` alone is wrong — two transitive `use helper`
        // declarations from different importer dirs are distinct
        // entries (they resolve to different files) and both must
        // be queued. The load loop deduplicates by resolved file
        // path, so cycles and "same file via two `use` paths" are
        // still no-ops there. The tuple-dedupe here just prevents
        // the use_paths array from growing on identical re-imports.
        // (`imp_dir` already computed above for the alias-rewrite
        // pass; reused here.)
        for (int rui = 0; rui < imp_prog->program.use_count; rui++) {
            const char *rpath = imp_prog->program.use_paths[rui];
            const char *ralias = imp_prog->program.use_aliases[rui];
            int already = 0;
            for (int k = 0; k < program->program.use_count; k++) {
                if (!strcmp(program->program.use_paths[k], rpath) &&
                    !strcmp(program->program.use_aliases[k], ralias) &&
                    !strcmp(use_origin_dirs[k], imp_dir)) {
                    already = 1;
                    break;
                }
            }
            if (already) continue;
            int idx2 = program->program.use_count++;
            program->program.use_paths = realloc(program->program.use_paths,
                program->program.use_count * sizeof(char *));
            program->program.use_aliases = realloc(program->program.use_aliases,
                program->program.use_count * sizeof(char *));
            use_origin_dirs = realloc(use_origin_dirs,
                program->program.use_count * sizeof(char *));
            program->program.use_paths[idx2] = strdup(rpath);
            program->program.use_aliases[idx2] = strdup(ralias);
            use_origin_dirs[idx2] = strdup(imp_dir);
        }
        free(imp_src);
    }
    for (int ui = 0; ui < program->program.use_count; ui++) {
        free(use_origin_dirs[ui]);
    }
    free(use_origin_dirs);

    // Monomorphize generic structs and methods. After this pass the
    // AST is fully concrete; the checker, optimizer, and codegen need
    // no awareness of generics.
    monomorph_run(program);

    // Type check
    checker_run(program);

    // Optimize
    optimizer_run(program);

    // Helper: write the IR-pipeline assembly to `asm_path`. Splits its
    // work via the irgen + iremit + finalise sequence; runtime helpers
    // (yell, heap allocator, str/list/map/imap helpers, panic and
    // assert handlers, data section) come from src/runtime_emit.c.
    // Returns the number of IR functions emitted.
    #define EMIT_IR_TO_FILE(asm_path_arg) ({                                    \
        IRProgram *ir = irgen_generate(program);                                \
        iropt_run(ir, opt_level, program);                                      \
        FILE *ir_out = fopen((asm_path_arg), "w");                              \
        fprintf(ir_out, ".global _start\n.align 2\n");                          \
        fprintf(ir_out, ".section __TEXT,__text\n\n");                          \
        runtime_emit_builtins(ir_out);                                          \
        for (int si = 0; si < program->program.structs.count; si++) {           \
            Node *s = program->program.structs.items[si];                       \
            int size = s->struct_def.field_count * 8;                           \
            if (size == 0) size = 8;                                            \
            /* Decide whether this constructor needs to recursively              \
             * initialise struct-typed fields. A field type counts if            \
             * (after monomorphisation) its spelling matches another             \
             * struct's name in this program — including stdlib types            \
             * like `List__Item`, `Map__String__int`, `String`. If no            \
             * such field exists we keep the simple alloc + zero body            \
             * (no extra prologue, no callee-saves). */                         \
            int needs_field_init = 0;                                           \
            for (int fi = 0; fi < s->struct_def.field_count; fi++) {            \
                const char *ft = s->struct_def.field_types[fi];                 \
                if (!ft) continue;                                              \
                for (int sj = 0; sj < program->program.structs.count; sj++) {   \
                    if (sj == si) continue;                                     \
                    if (!strcmp(program->program.structs.items[sj]->struct_def.name, ft)) { \
                        needs_field_init = 1;                                   \
                        break;                                                  \
                    }                                                           \
                }                                                               \
                if (needs_field_init) break;                                    \
            }                                                                   \
            fprintf(ir_out, ".globl _alloc_%s\n.p2align 2\n_alloc_%s:\n",       \
                    s->struct_def.name, s->struct_def.name);                    \
            if (!needs_field_init) {                                            \
                fprintf(ir_out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n"); \
                fprintf(ir_out, "    mov x0, #%d\n    bl _heap_alloc\n", size); \
                /* ε5 prerequisite: zero the freshly-allocated bytes.           \
                 * `_heap_alloc` returns either a fresh mmap'd page (already    \
                 * zeroed) or a recycled free-list block whose first 16 bytes   \
                 * still hold [next, size] metadata. Stdlib types like          \
                 * `List of T` rely on `self.data eq 0` as a lazy-init signal;  \
                 * without zeroing, recycled blocks read garbage and the lazy   \
                 * path doesn't fire. Zero up to `size` bytes in 8-byte strides.\
                 * x0 holds the alloc pointer; preserve it across the loop. */ \
                fprintf(ir_out, "    mov x9, x0\n    mov x10, #0\n");           \
                for (int z = 0; z < size; z += 8) {                             \
                    fprintf(ir_out, "    str xzr, [x9, #%d]\n", z);             \
                }                                                               \
                fprintf(ir_out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n"); \
            } else {                                                            \
                /* Auto-init path: every field whose type is itself             \
                 * a struct gets a fresh `_alloc_<FieldType>` call and          \
                 * its pointer stored at the field's offset. Bug #114 —         \
                 * without this, `s is Outer(); s.field.method()` segfaults     \
                 * because `s.field` is a null pointer.                         \
                 *                                                              \
                 * Frame: 32 bytes (x29/x30 pair + x19/x20 pair). x19           \
                 * holds the parent pointer across recursive bl calls. */      \
                fprintf(ir_out, "    stp x29, x30, [sp, #-32]!\n");             \
                fprintf(ir_out, "    stp x19, x20, [sp, #16]\n");               \
                fprintf(ir_out, "    mov x29, sp\n");                           \
                fprintf(ir_out, "    mov x0, #%d\n    bl _heap_alloc\n", size); \
                /* Zero the block first so any non-struct field is 0. */        \
                fprintf(ir_out, "    mov x19, x0\n");                           \
                for (int z = 0; z < size; z += 8) {                             \
                    fprintf(ir_out, "    str xzr, [x19, #%d]\n", z);            \
                }                                                               \
                /* Per-field auto-init: for each field whose type is a          \
                 * struct in this program, call `_alloc_<FieldType>` and        \
                 * store the result at offset (fi*8) of the parent. */         \
                for (int fi = 0; fi < s->struct_def.field_count; fi++) {        \
                    const char *ft = s->struct_def.field_types[fi];             \
                    if (!ft) continue;                                          \
                    int is_struct_field = 0;                                    \
                    for (int sj = 0; sj < program->program.structs.count; sj++) { \
                        if (sj == si) continue;                                 \
                        if (!strcmp(program->program.structs.items[sj]->struct_def.name, ft)) { \
                            is_struct_field = 1;                                \
                            break;                                              \
                        }                                                       \
                    }                                                           \
                    if (!is_struct_field) continue;                             \
                    fprintf(ir_out, "    bl _alloc_%s\n", ft);                  \
                    fprintf(ir_out, "    str x0, [x19, #%d]\n", fi * 8);        \
                }                                                               \
                /* Return parent pointer in x0. */                              \
                fprintf(ir_out, "    mov x0, x19\n");                           \
                fprintf(ir_out, "    ldp x19, x20, [sp, #16]\n");               \
                fprintf(ir_out, "    ldp x29, x30, [sp], #32\n");               \
                fprintf(ir_out, "    ret\n\n");                                 \
            }                                                                   \
            /* `_clone_<X>(src) -> dst` — deep clone of one struct.             \
             * Used by `b is rep a`. Allocates a fresh block of the             \
             * same size and copies each field according to its type:           \
             *   - primitive (int/bool/byte/anything not a struct or            \
             *     `array__*`)            : 8-byte field copy                  \
             *   - struct in this program   : recurse via _clone_<FieldType>   \
             *     (skipped when src field is null — store null in dst)        \
             *   - `array__<elem>`          : alloc fresh 16-byte header +     \
             *     fresh data buffer, memcpy data bytes, store new header     \
             *     pointer in dst (skipped — store null — when src field is   \
             *     null)                                                       \
             *                                                                  \
             * x0 in: src pointer. x0 out: dst pointer (fresh block).           \
             * Frame: 48 bytes — x29/x30 + x19/x20 + x21/x22.                   \
             *   x19 = src, x20 = dst across the body.                          \
             *   x21/x22 are scratch saved across `bl` calls inside array       \
             *   field handling. */                                              \
            fprintf(ir_out, ".globl _clone_%s\n.p2align 2\n_clone_%s:\n",       \
                    s->struct_def.name, s->struct_def.name);                    \
            /* Borrow short-circuit: a borrowed value (e.g. a String              \
             * literal) is immutable rodata. Cloning it as a fresh                \
             * heap copy is wasteful and, more importantly, the                   \
             * inner `array of byte` header is also rodata — the                  \
             * generic clone path below would walk into it expecting              \
             * mutability. Just return the same pointer; the borrow               \
             * convention (`owned == 0` means "do not free, do not                \
             * mutate") guarantees this is sound. */                              \
            {                                                                   \
                int owned_off = -1;                                             \
                for (int ofi = 0; ofi < s->struct_def.field_count; ofi++) {     \
                    const char *fn = s->struct_def.field_names[ofi];            \
                    const char *ft = s->struct_def.field_types[ofi];            \
                    if (fn && ft && !strcmp(fn, "owned") && !strcmp(ft, "int")) { \
                        owned_off = ofi * 8;                                    \
                        break;                                                  \
                    }                                                           \
                }                                                               \
                if (owned_off >= 0) {                                            \
                    fprintf(ir_out, "    cbz x0, _clone_%s_borrowed\n",          \
                            s->struct_def.name);                                \
                    fprintf(ir_out, "    ldr x9, [x0, #%d]\n", owned_off);       \
                    fprintf(ir_out, "    cbnz x9, _clone_%s_owned\n",            \
                            s->struct_def.name);                                \
                    fprintf(ir_out, "_clone_%s_borrowed:\n",                     \
                            s->struct_def.name);                                \
                    fprintf(ir_out, "    ret\n");                                \
                    fprintf(ir_out, "_clone_%s_owned:\n",                        \
                            s->struct_def.name);                                \
                }                                                               \
            }                                                                   \
            fprintf(ir_out, "    stp x29, x30, [sp, #-48]!\n");                 \
            fprintf(ir_out, "    stp x19, x20, [sp, #16]\n");                   \
            fprintf(ir_out, "    stp x21, x22, [sp, #32]\n");                   \
            fprintf(ir_out, "    mov x29, sp\n");                               \
            /* x19 := src */                                                    \
            fprintf(ir_out, "    mov x19, x0\n");                               \
            /* x20 := alloc(size); zero-fill so any path that doesn't write    \
             * a field (e.g. recursion skipped on null) leaves a clean 0. */    \
            fprintf(ir_out, "    mov x0, #%d\n    bl _heap_alloc\n", size);     \
            fprintf(ir_out, "    mov x20, x0\n");                               \
            for (int z = 0; z < size; z += 8) {                                 \
                fprintf(ir_out, "    str xzr, [x20, #%d]\n", z);                \
            }                                                                   \
            for (int fi = 0; fi < s->struct_def.field_count; fi++) {            \
                const char *ft = s->struct_def.field_types[fi];                 \
                int off = fi * 8;                                               \
                /* Identify field kind. Same fix as _drop_<X>:                   \
                 * self-type fields (Node.next of type Node) recurse —           \
                 * the chain is finite via nil termination and the               \
                 * source's existing chain is what we need to mirror.            \
                 * Only _alloc_<X>'s init loop skips self-types (to              \
                 * avoid infinite recursion at construction time). */            \
                int is_struct_field = 0;                                        \
                if (ft) for (int sj = 0; sj < program->program.structs.count; sj++) { \
                    if (!strcmp(program->program.structs.items[sj]->struct_def.name, ft)) { \
                        is_struct_field = 1;                                    \
                        break;                                                  \
                    }                                                           \
                }                                                               \
                int is_array_field = ft && !strncmp(ft, "array__", 7);          \
                int is_byte_array  = ft && !strcmp(ft, "array__byte");          \
                if (is_struct_field) {                                          \
                    /* Recurse if non-null; else leave dst field as 0. */       \
                    fprintf(ir_out, "    ldr x9, [x19, #%d]\n", off);           \
                    fprintf(ir_out, "    cbz x9, _clone_%s_skip%d\n",           \
                        s->struct_def.name, fi);                                \
                    fprintf(ir_out, "    mov x0, x9\n");                        \
                    fprintf(ir_out, "    bl _clone_%s\n", ft);                  \
                    fprintf(ir_out, "    str x0, [x20, #%d]\n", off);           \
                    fprintf(ir_out, "_clone_%s_skip%d:\n",                      \
                        s->struct_def.name, fi);                                \
                } else if (is_array_field) {                                    \
                    /* Inline copy of the array: 16-byte header (cap@0,         \
                     * data@8) + cap*esz data bytes. Skip on null source.       \
                     * Layout assumptions match emit_scope_cleanup's array      \
                     * cleanup path in compiler/irgen.c. */                     \
                    int esz = is_byte_array ? 1 : 8;                            \
                    fprintf(ir_out, "    ldr x21, [x19, #%d]\n", off);          \
                    fprintf(ir_out, "    cbz x21, _clone_%s_skip%d\n",          \
                        s->struct_def.name, fi);                                \
                    /* Alloc new 16-byte header. x21 = src array ptr,           \
                     * stays alive across bl in callee-save x21. */             \
                    fprintf(ir_out, "    mov x0, #16\n    bl _heap_alloc\n");   \
                    fprintf(ir_out, "    mov x22, x0\n");                       \
                    /* cap := src_arr.cap; new_arr.cap := cap. */               \
                    fprintf(ir_out, "    ldr x9, [x21, #0]\n");                 \
                    fprintf(ir_out, "    str x9, [x22, #0]\n");                 \
                    /* Allocate cap*esz bytes for the new data buffer. */       \
                    if (esz == 1) {                                             \
                        fprintf(ir_out, "    mov x0, x9\n");                    \
                    } else {                                                    \
                        fprintf(ir_out, "    lsl x0, x9, #3\n");                \
                    }                                                           \
                    fprintf(ir_out, "    bl _heap_alloc\n");                    \
                    fprintf(ir_out, "    mov x10, x0\n");                       \
                    /* Store new data ptr in new header. */                     \
                    fprintf(ir_out, "    str x10, [x22, #8]\n");                \
                    /* Memcpy data bytes: src = src_arr.data, dst = new data,   \
                     * len = cap*esz. */                                         \
                    fprintf(ir_out, "    ldr x11, [x21, #8]\n");                \
                    fprintf(ir_out, "    ldr x12, [x21, #0]\n");                \
                    if (esz == 1) {                                             \
                        fprintf(ir_out, "    mov x13, x12\n");                  \
                    } else {                                                    \
                        fprintf(ir_out, "    lsl x13, x12, #3\n");              \
                    }                                                           \
                    fprintf(ir_out, "    cbz x13, _clone_%s_done%d\n",          \
                        s->struct_def.name, fi);                                \
                    fprintf(ir_out, "    mov x14, #0\n");                       \
                    fprintf(ir_out, "_clone_%s_loop%d:\n",                      \
                        s->struct_def.name, fi);                                \
                    fprintf(ir_out, "    ldrb w15, [x11, x14]\n");              \
                    fprintf(ir_out, "    strb w15, [x10, x14]\n");              \
                    fprintf(ir_out, "    add x14, x14, #1\n");                  \
                    fprintf(ir_out, "    cmp x14, x13\n");                      \
                    fprintf(ir_out, "    b.lt _clone_%s_loop%d\n",              \
                        s->struct_def.name, fi);                                \
                    fprintf(ir_out, "_clone_%s_done%d:\n",                      \
                        s->struct_def.name, fi);                                \
                    /* Store new header ptr in dst field. */                    \
                    fprintf(ir_out, "    str x22, [x20, #%d]\n", off);          \
                    fprintf(ir_out, "_clone_%s_skip%d:\n",                      \
                        s->struct_def.name, fi);                                \
                } else {                                                        \
                    /* Primitive (int/bool/byte/etc.). Plain 8-byte copy. */    \
                    fprintf(ir_out, "    ldr x9, [x19, #%d]\n", off);           \
                    fprintf(ir_out, "    str x9, [x20, #%d]\n", off);           \
                }                                                               \
            }                                                                   \
            fprintf(ir_out, "    mov x0, x20\n");                               \
            fprintf(ir_out, "    ldp x21, x22, [sp, #32]\n");                   \
            fprintf(ir_out, "    ldp x19, x20, [sp, #16]\n");                   \
            fprintf(ir_out, "    ldp x29, x30, [sp], #48\n");                   \
            fprintf(ir_out, "    ret\n\n");                                     \
            /* `_drop_<X>(ptr)` — recursively free a struct and its              \
             * owned heap fields. Codex P0-8.                                    \
             *                                                                   \
             * Per field, three cases (mirrors _clone_<X>):                      \
             *   - primitive (anything not in struct registry and not            \
             *     `array__*`): nothing to free.                                 \
             *   - struct in this program: bl _drop_<FieldType> if non-null.     \
             *   - `array__<elem>`: free data buffer (cap*esz bytes), free       \
             *     header (16 bytes). Both null-guarded.                         \
             * Then heap_free the struct itself (`size` bytes).                  \
             *                                                                   \
             * x0 in: ptr to struct. No return value.                            \
             * Frame: 32 bytes — x29/x30 + x19/x20 (x19=ptr saved across         \
             * recursive calls). x21/x22 not needed: array path uses             \
             * x9..x14 directly between heap_free calls without                  \
             * intermediate calls. */                                            \
            fprintf(ir_out, ".globl _drop_%s\n.p2align 2\n_drop_%s:\n",         \
                    s->struct_def.name, s->struct_def.name);                    \
            fprintf(ir_out, "    cbz x0, _drop_%s_done\n",                       \
                    s->struct_def.name);                                        \
            /* Borrow gate: if this struct has an `int owned` field (the         \
             * String/byte-array convention) and it's currently 0, this           \
             * value points at rodata (a literal, e.g. `"foo"`). Calling         \
             * `_heap_free` on rodata corrupts the heap free list and             \
             * later allocations crash. Skip the entire body — return            \
             * immediately. Detected by literal field name "owned" with           \
             * a primitive int type. Documented invariant in                     \
             * docs/string-rewrite.md. */                                        \
            {                                                                   \
                int owned_off = -1;                                             \
                for (int ofi = 0; ofi < s->struct_def.field_count; ofi++) {     \
                    const char *fn = s->struct_def.field_names[ofi];            \
                    const char *ft = s->struct_def.field_types[ofi];            \
                    if (fn && ft && !strcmp(fn, "owned") && !strcmp(ft, "int")) { \
                        owned_off = ofi * 8;                                    \
                        break;                                                  \
                    }                                                           \
                }                                                               \
                if (owned_off >= 0) {                                            \
                    fprintf(ir_out, "    ldr x9, [x0, #%d]\n", owned_off);       \
                    fprintf(ir_out, "    cbz x9, _drop_%s_done\n",               \
                            s->struct_def.name);                                \
                }                                                               \
            }                                                                   \
            fprintf(ir_out, "    stp x29, x30, [sp, #-32]!\n");                 \
            fprintf(ir_out, "    stp x19, x20, [sp, #16]\n");                   \
            fprintf(ir_out, "    mov x29, sp\n");                               \
            fprintf(ir_out, "    mov x19, x0\n");                               \
            for (int fi = 0; fi < s->struct_def.field_count; fi++) {            \
                const char *ft = s->struct_def.field_types[fi];                 \
                int off = fi * 8;                                               \
                int is_struct_field = 0;                                        \
                /* Codex review fix: self-type fields (e.g. Node.next            \
                 * of type Node) DO need to be dropped — the chain is             \
                 * finite via nil termination, and skipping the self-             \
                 * type would leak every link past the head. Distinct             \
                 * from _alloc_<X>'s init loop, which skips self-type             \
                 * fields specifically to avoid infinite recursion at             \
                 * construction. */                                              \
                if (ft) for (int sj = 0; sj < program->program.structs.count; sj++) { \
                    if (!strcmp(program->program.structs.items[sj]->struct_def.name, ft)) { \
                        is_struct_field = 1;                                    \
                        break;                                                  \
                    }                                                           \
                }                                                               \
                int is_array_field = ft && !strncmp(ft, "array__", 7);          \
                int is_byte_array  = ft && !strcmp(ft, "array__byte");          \
                if (is_struct_field) {                                          \
                    fprintf(ir_out, "    ldr x9, [x19, #%d]\n", off);           \
                    fprintf(ir_out, "    cbz x9, _drop_%s_skip%d\n",            \
                        s->struct_def.name, fi);                                \
                    fprintf(ir_out, "    mov x0, x9\n");                        \
                    fprintf(ir_out, "    bl _drop_%s\n", ft);                   \
                    fprintf(ir_out, "_drop_%s_skip%d:\n",                       \
                        s->struct_def.name, fi);                                \
                } else if (is_array_field) {                                    \
                    int esz = is_byte_array ? 1 : 8;                            \
                    fprintf(ir_out, "    ldr x9, [x19, #%d]\n", off);           \
                    fprintf(ir_out, "    cbz x9, _drop_%s_skip%d\n",            \
                        s->struct_def.name, fi);                                \
                    /* Array layout: [cap @ 0, data @ 8]. Free data first       \
                     * (size = cap*esz), then the 16-byte header. Mirrors       \
                     * the array RAII path in compiler/irgen.c                  \
                     * emit_scope_cleanup. */                                   \
                    fprintf(ir_out, "    ldr x10, [x9, #0]\n");                 \
                    fprintf(ir_out, "    ldr x11, [x9, #8]\n");                 \
                    /* x9=array_hdr, x10=cap, x11=data_ptr */                   \
                    fprintf(ir_out, "    cbz x11, _drop_%s_hdr%d\n",            \
                        s->struct_def.name, fi);                                \
                    if (esz == 1) {                                             \
                        fprintf(ir_out, "    mov x1, x10\n");                   \
                    } else {                                                    \
                        fprintf(ir_out, "    lsl x1, x10, #3\n");               \
                    }                                                           \
                    fprintf(ir_out, "    mov x0, x11\n");                       \
                    fprintf(ir_out, "    bl _heap_free\n");                     \
                    fprintf(ir_out, "_drop_%s_hdr%d:\n",                        \
                        s->struct_def.name, fi);                                \
                    fprintf(ir_out, "    ldr x0, [x19, #%d]\n", off);           \
                    fprintf(ir_out, "    mov x1, #16\n");                       \
                    fprintf(ir_out, "    bl _heap_free\n");                     \
                    fprintf(ir_out, "_drop_%s_skip%d:\n",                       \
                        s->struct_def.name, fi);                                \
                }                                                               \
                /* Primitive fields require no drop. */                         \
            }                                                                   \
            /* Free the struct itself. */                                       \
            fprintf(ir_out, "    mov x0, x19\n");                               \
            fprintf(ir_out, "    mov x1, #%d\n", size);                         \
            fprintf(ir_out, "    bl _heap_free\n");                             \
            fprintf(ir_out, "    ldp x19, x20, [sp, #16]\n");                   \
            fprintf(ir_out, "    ldp x29, x30, [sp], #32\n");                   \
            fprintf(ir_out, "_drop_%s_done:\n",                                 \
                    s->struct_def.name);                                        \
            fprintf(ir_out, "    ret\n\n");                                     \
        }                                                                       \
        for (int i = 0; i < ir->func_count; i++) {                              \
            RegAllocResult alloc = regalloc_run(&ir->funcs[i]);                 \
            iremit_func(ir_out, &ir->funcs[i], &alloc);                         \
            fprintf(ir_out, "\n");                                              \
            free(alloc.vreg_to_phys);                                           \
            free(alloc.vreg_to_spill);                                          \
        }                                                                       \
        int test_count = program->program.tests.count;                          \
        if (test_count > 0) {                                                   \
            fprintf(ir_out, ".globl _start\n.p2align 2\n_start:\n");            \
            for (int i = 0; i < test_count; i++) {                              \
                fprintf(ir_out, "    adrp x0, _pass_prefix@PAGE\n    add x0, x0, _pass_prefix@PAGEOFF\n"); \
                fprintf(ir_out, "    bl _yell_str\n");                          \
                fprintf(ir_out, "    adrp x0, _test_name_%d@PAGE\n    add x0, x0, _test_name_%d@PAGEOFF\n", i, i); \
                fprintf(ir_out, "    bl _yell_str\n");                          \
                fprintf(ir_out, "    bl _test_%d\n", i);                        \
            }                                                                   \
            fprintf(ir_out, "    mov x16, #1\n    mov x0, #0\n    svc #0x80\n\n"); \
        } else {                                                                \
            fprintf(ir_out, "_start:\n    bl _spark\n    mov x16, #1\n    mov x0, #0\n    svc #0x80\n\n"); \
        }                                                                       \
        iremit_finalize_data(ir_out);                                           \
        if (test_count > 0) {                                                   \
            /* P3.4: each test name is a `String` header (4 quads:              \
             * cap, count, data, owned=0) so the runtime test runner            \
             * can pass it to `_yell_str` (which now reads count from           \
             * the header instead of scanning bytes). The bytes live            \
             * in `_test_name_<i>_bytes`; the header is at                      \
             * `_test_name_<i>`. */                                              \
            fprintf(ir_out, ".section __DATA,__data\n");                        \
            for (int i = 0; i < test_count; i++) {                              \
                Node *t = program->program.tests.items[i];                      \
                int tn = (int)strlen(t->test_def.name);                         \
                fprintf(ir_out, "_test_name_%d_bytes: .asciz \"%s\"\n", i,      \
                    t->test_def.name);                                          \
                fprintf(ir_out, ".p2align 3\n_test_name_%d_arr:\n", i);         \
                fprintf(ir_out, "    .quad %d\n    .quad _test_name_%d_bytes\n", tn, i); \
                fprintf(ir_out, "_test_name_%d:\n", i);                         \
                fprintf(ir_out, "    .quad %d\n    .quad %d\n", tn, tn);        \
                fprintf(ir_out, "    .quad _test_name_%d_arr\n    .quad 0\n", i); \
            }                                                                   \
            fprintf(ir_out, ".section __TEXT,__text\n");                        \
        }                                                                       \
        fclose(ir_out);                                                         \
        ir->func_count;                                                         \
    })

    // `erbos ir <file.ptt>` — emit .s only, don't assemble or run.
    if (ir_only) {
        int func_count = EMIT_IR_TO_FILE(asm_path);
        printf("IR pipeline: generated %s (%d functions)\n", asm_path, func_count);
        free(src);
        return 0;
    }

    // Build path. The IR backend is now the only backend; the original
    // direct codegen (src/codegen.c) was retired in #34 P4.3g after a
    // release of opt-out testing through the `legacy` subcommand
    // confirmed the IR pipeline produced byte-identical output on
    // every program in the corpus.
    EMIT_IR_TO_FILE(asm_path);
    if (!run_mode) printf("generated %s\n", asm_path);

    // Assemble + link.
    // Codex P1-13 (partial): paths with spaces now build because
    // every path argument is wrapped in single quotes. Paths
    // containing a literal `'` still break — single quotes don't
    // escape themselves in shell. Full fix is posix_spawn with
    // argv (no shell at all), tracked as follow-up. The
    // common case (paths with spaces) works; pathological
    // characters in paths still don't.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "as -o '%s.o' '%s'", out_name, asm_path);
    if (system(cmd) != 0) { fprintf(stderr, "error: assembly failed\n"); return 1; }

    snprintf(cmd, sizeof(cmd), "ld -o '%s' '%s.o' -lSystem -syslibroot $(xcrun --show-sdk-path) -e _start", out_name, out_name);
    if (system(cmd) != 0) { fprintf(stderr, "error: linking failed\n"); return 1; }

    // Cleanup
    snprintf(cmd, sizeof(cmd), "rm -f '%s.o' '%s.s'", out_name, out_name);
    system(cmd);

    if (run_mode) {
        // Run the binary. If `out_name` is a bare basename (no `/`),
        // prepend `./` so the shell finds it via the cwd rather than
        // searching $PATH — otherwise an unrelated binary with the
        // same name on $PATH gets executed (or, more commonly, the
        // shell errors with "command not found" because the basename
        // isn't on $PATH at all).
        char run_cmd[1024];
        if (strchr(out_name, '/'))
            snprintf(run_cmd, sizeof(run_cmd), "'%s'", out_name);
        else
            snprintf(run_cmd, sizeof(run_cmd), "./'%s'", out_name);
        int ret = system(run_cmd);
        // Delete binary after running
        snprintf(cmd, sizeof(cmd), "rm -f '%s'", out_name);
        system(cmd);
        free(src);
        // Codex P1-10: propagate signals as 128+signum so the
        // shell convention surfaces them. WEXITSTATUS is undefined
        // for signal-terminated children — pre-fix, a SIGSEGV in
        // the user's program reported as exit 0 from `erbos run`,
        // hiding crashes from CI / shell-script callers.
        if (WIFEXITED(ret)) return WEXITSTATUS(ret);
        if (WIFSIGNALED(ret)) return 128 + WTERMSIG(ret);
        return 1;
    }

    printf("built %s\n", out_name);
    free(src);
    return 0;
}
