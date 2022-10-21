#include "ir_private.h"
#include "rewrite.h"
#include "fold.h"
#include "log.h"
#include "portability.h"

#include "../type.h"

#include "list.h"
#include "dict.h"

#include <stdlib.h>
#include <assert.h>

typedef struct {
    const Node* instr;
    Node* tail;
    bool mut;
} StackEntry;

BodyBuilder* begin_body(IrArena* arena) {
    BodyBuilder* builder = malloc(sizeof(BodyBuilder));
    *builder = (BodyBuilder) {
        .arena = arena,
        .stack = new_list(StackEntry),
    };
    return builder;
}

static Nodes create_output_variables(IrArena* arena, const Node* value, size_t outputs_count, Nodes* provided_types, const char* output_names[]) {
    LARRAY(Node*, vars, outputs_count);

    if (arena->config.check_types) {
        Nodes types = unwrap_multiple_yield_types(arena, value->type);
        if (outputs_count == 0 && types.count > 0)
            outputs_count = types.count;
        assert(types.count == outputs_count);
        if (provided_types) {
            assert(provided_types->count == outputs_count);
            // Check that the types we got are subtypes of what we care about
            for (size_t i = 0; i < outputs_count; i++)
                assert(is_subtype(provided_types->nodes[i], types.nodes[i]));
            types = *provided_types;
        }

        for (size_t i = 0; i < outputs_count; i++)
            vars[i] = (Node*) var(arena, types.nodes[i], output_names ? output_names[i] : node_tags[value->tag]);
    } else {
        for (size_t i = 0; i < outputs_count; i++)
            vars[i] = (Node*) var(arena, provided_types ? provided_types->nodes[i] : NULL, output_names ? output_names[i] : node_tags[value->tag]);
    }

    for (size_t i = 0; i < outputs_count; i++) {
        vars[i]->payload.var.instruction = value;
        vars[i]->payload.var.output = i;
    }
    return nodes(arena, outputs_count, vars);
}

Nodes append_instruction(BodyBuilder* builder, const Node* instruction) {
    Nodes params = create_output_variables(builder->arena, instruction, 0, NULL, NULL);
    StackEntry entry = {
        .instr = instruction,
        .tail = lambda(builder->arena, params),
        .mut = false,
    };
    append_list(StackEntry, builder->stack, entry);
    return params;
}

Nodes declare_local_variable(BodyBuilder* builder, const Node* initial_value, bool mut, Nodes* provided_types, size_t outputs_count, const char* output_names[]) {
    Nodes params = create_output_variables(builder->arena, initial_value, outputs_count, provided_types, output_names);
    StackEntry entry = {
        .instr = initial_value,
        .tail = lambda(builder->arena, params),
        .mut = mut,
    };
    append_list(StackEntry, builder->stack, entry);
    return params;
}

#undef arena

const Node* finish_body(BodyBuilder* builder, const Node* terminator) {
    size_t stack_size = entries_count_list(builder->stack);
    for (size_t i = stack_size - 1; i < stack_size; i--) {
        StackEntry entry = read_list(StackEntry, builder->stack)[i];
        entry.tail->payload.lam.body = terminator;
        terminator = let(builder->arena, entry.mut, entry.instr, entry.tail);
    }

    destroy_list(builder->stack);
    free(builder);
    return terminator;
}