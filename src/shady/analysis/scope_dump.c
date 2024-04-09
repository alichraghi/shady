#include "scope.h"

#include "shady/ir_private.h"
#include "shady/print.h"

#include "list.h"
#include "dict.h"
#include "util.h"
#include "printer.h"

#include <string.h>
#include <assert.h>

static int extra_uniqueness = 0;

static CFNode* get_let_pred(const CFNode* n) {
    if (entries_count_list(n->pred_edges) == 1) {
        CFEdge pred = read_list(CFEdge, n->pred_edges)[0];
        assert(pred.dst == n);
        if (pred.type == LetTailEdge && entries_count_list(pred.src->succ_edges) == 1) {
            assert(is_case(n->node));
            return pred.src;
        }
    }
    return NULL;
}

static void print_node_helper(Printer* p, const Node* n) {
    Growy* tmp_g = new_growy();
    Printer* tmp_p = open_growy_as_printer(tmp_g);

    PrintConfig config = {
            .color = false,
            .in_cfg = true,
    };

    print_node(tmp_p, n, config);

    String label = printer_growy_unwrap(tmp_p);
    char* escaped_label = calloc(strlen(label) * 2, 1);
    unapply_escape_codes(label, strlen(label), escaped_label);

    print(p, "%s", escaped_label);
    free(escaped_label);
    free((void*)label);
}

static void dump_cf_node(FILE* output, const CFNode* n) {
    const Node* bb = n->node;
    const Node* body = get_abstraction_body(bb);
    if (!body)
        return;
    if (get_let_pred(n))
        return;

    String color = "black";
    if (is_case(bb))
        color = "green";
    else if (is_basic_block(bb))
        color = "blue";

    Growy* g = new_growy();
    Printer* p = open_growy_as_printer(g);

    String abs_name = get_abstraction_name_unsafe(bb);
    if (!abs_name)
        abs_name = format_string_interned(bb->arena, "%%%d", bb->id);

    print(p, "%s: \n%s: ", abs_name, abs_name);

    const CFNode* let_chain_end = n;
    while (body->tag == Let_TAG) {
        if (entries_count_list(let_chain_end->succ_edges) != 1 || read_list(CFEdge, let_chain_end->succ_edges)[0].type != LetTailEdge)
            break;

        print_node_helper(p, body);
        print(p, "\\l");

        const Node* abs = body->payload.let.tail;
        print(p, "%%%d: ", abs->id);

        let_chain_end = read_list(CFEdge, let_chain_end->succ_edges)[0].dst;
        assert(let_chain_end->node == abs);
        assert(is_case(abs));
        body = get_abstraction_body(abs);
    }

    print_node_helper(p, body);
    print(p, "\\l");

    String label = printer_growy_unwrap(p);
    fprintf(output, "bb_%zu [nojustify=true, label=\"%s\", color=\"%s\", shape=box];\n", (size_t) n, label, color);
    free((void*) label);

    for (size_t i = 0; i < entries_count_list(n->dominates); i++) {
        CFNode* d = read_list(CFNode*, n->dominates)[i];
        if (!find_key_dict(const Node*, n->structurally_dominates, d->node))
        dump_cf_node(output, d);
    }
}

static void dump_cfg_scope(FILE* output, Scope* scope) {
    extra_uniqueness++;

    const Node* entry = scope->entry->node;
    fprintf(output, "subgraph cluster_%s {\n", get_abstraction_name(entry));
    fprintf(output, "label = \"%s\";\n", get_abstraction_name(entry));
    for (size_t i = 0; i < entries_count_list(scope->contents); i++) {
        const CFNode* n = read_list(const CFNode*, scope->contents)[i];
        dump_cf_node(output, n);
    }
    for (size_t i = 0; i < entries_count_list(scope->contents); i++) {
        const CFNode* bb_node = read_list(const CFNode*, scope->contents)[i];
        const CFNode* src_node = bb_node;
        while (true) {
            const CFNode* let_parent = get_let_pred(src_node);
            if (let_parent)
                src_node = let_parent;
            else
                break;
        }

        for (size_t j = 0; j < entries_count_list(bb_node->succ_edges); j++) {
            CFEdge edge = read_list(CFEdge, bb_node->succ_edges)[j];
            const CFNode* target_node = edge.dst;

            if (edge.type == LetTailEdge && get_let_pred(target_node) == bb_node)
                continue;

            String edge_color = "black";
            switch (edge.type) {
                case LetTailEdge:             edge_color = "green"; break;
                case StructuredEnterBodyEdge: edge_color = "blue"; break;
                case StructuredLeaveBodyEdge: edge_color = "red"; break;
                case StructuredPseudoExitEdge: edge_color = "darkred"; break;
                default: break;
            }

            fprintf(output, "bb_%zu -> bb_%zu [color=\"%s\"];\n", (size_t) (src_node), (size_t) (target_node), edge_color);
        }
    }
    fprintf(output, "}\n");
}

void dump_cfg(FILE* output, Module* mod) {
    if (output == NULL)
        output = stderr;

    fprintf(output, "digraph G {\n");
    struct List* scopes = build_scopes(mod);
    for (size_t i = 0; i < entries_count_list(scopes); i++) {
        Scope* scope = read_list(Scope*, scopes)[i];
        dump_cfg_scope(output, scope);
        destroy_scope(scope);
    }
    destroy_list(scopes);
    fprintf(output, "}\n");
}

void dump_cfg_auto(Module* mod) {
    FILE* f = fopen("cfg.dot", "wb");
    dump_cfg(f, mod);
    fclose(f);
}
