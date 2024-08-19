#include "free_frontier.h"

#include "visit.h"
#include "dict.h"

typedef struct {
    Visitor v;
    Scheduler* scheduler;
    CFG* cfg;
    CFNode* start;
    struct Dict* seen;
    struct Dict* frontier;
} FreeFrontierVisitor;

/// Whether 'a' is dominated by 'b'
static bool is_dominated(CFNode* a, CFNode* b) {
    while (a) {
        if (a == b)
            return true;
        a = a->idom;
    }
    return false;
}

static void visit_free_frontier(FreeFrontierVisitor* v, const Node* node) {
    if (find_key_dict(const Node*, v->seen, node))
        return;
    insert_set_get_result(const Node*, v->seen, node);
    CFNode* where = schedule_instruction(v->scheduler, node);
    if (where) {
        FreeFrontierVisitor vv = *v;
        if (is_dominated(where, v->start)) {
            visit_node_operands(&vv.v, NcAbstraction | NcDeclaration | NcType, node);
        } else {
            insert_set_get_result(const Node*, v->frontier, node);
        }
    }
}

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

struct Dict* free_frontier(Scheduler* scheduler, CFG* cfg, const Node* abs) {
    FreeFrontierVisitor ffv = {
        .v = {
            .visit_node_fn = (VisitNodeFn) visit_free_frontier,
        },
        .scheduler = scheduler,
        .cfg = cfg,
        .start = cfg_lookup(cfg, abs),
        .frontier = new_set(const Node*, (HashFn) hash_node, (CmpFn) compare_node),
        .seen = new_set(const Node*, (HashFn) hash_node, (CmpFn) compare_node),
    };
    if (get_abstraction_body(abs))
        visit_free_frontier(&ffv, get_abstraction_body(abs));
    destroy_dict(ffv.seen);
    return ffv.frontier;
}