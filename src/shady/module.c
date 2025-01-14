#include "ir_private.h"

#include "list.h"
#include "portability.h"

#include <string.h>

Module* shd_new_module(IrArena* arena, String name) {
    Module* m = shd_arena_alloc(arena->arena, sizeof(Module));
    *m = (Module) {
        .arena = arena,
        .name = string(arena, name),
        .decls = shd_new_list(Node*),
    };
    shd_list_append(Module*, arena->modules, m);
    return m;
}

IrArena* shd_module_get_arena(const Module* m) {
    return m->arena;
}

String shd_module_get_name(const Module* m) {
    return m->name;
}

Nodes shd_module_get_declarations(const Module* m) {
    size_t count = shd_list_count(m->decls);
    const Node** start = shd_read_list(const Node*, m->decls);
    return shd_nodes(shd_module_get_arena(m), count, start);
}

void _shd_module_add_decl(Module* m, Node* node) {
    assert(is_declaration(node));
    assert(!shd_module_get_declaration(m, get_declaration_name(node)) && "duplicate declaration");
    shd_list_append(Node*, m->decls, node);
}

Node* shd_module_get_declaration(const Module* m, String name) {
    Nodes existing_decls = shd_module_get_declarations(m);
    for (size_t i = 0; i < existing_decls.count; i++) {
        if (strcmp(get_declaration_name(existing_decls.nodes[i]), name) == 0)
            return (Node*) existing_decls.nodes[i];
    }
    return NULL;
}

void shd_destroy_module(Module* m) {
    shd_destroy_list(m->decls);
}
