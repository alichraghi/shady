#include "pass.h"

#include "../analysis/uses.h"
#include "../ir_private.h"
#include "../type.h"

#include "portability.h"
#include "log.h"

#pragma GCC diagnostic error "-Wswitch"

typedef struct {
    Rewriter rewriter;
    const UsesMap* map;
    bool* todo;
} Context;

static size_t count_calls(const UsesMap* map, const Node* bb) {
    size_t count = 0;
    const Use* use = get_first_use(map, bb);
    for (;use; use = use->next_use) {
        if (use->user->tag == Jump_TAG) {
            const Use* jump_use = get_first_use(map, use->user);
            for (; jump_use; jump_use = jump_use->next_use) {
                if (jump_use->operand_class == NcJump)
                    return SIZE_MAX; // you can never inline conditional jumps
                count++;
            }
        } else if (use->operand_class == NcBasic_block)
            return SIZE_MAX; // you can never inline basic blocks used for other purposes
    }
    return count;
}

static bool has_side_effects(const Node* instr) {
    bool side_effects = true;
    if (instr->tag == PrimOp_TAG)
        side_effects = has_primop_got_side_effects(instr->payload.prim_op.op);
    switch (instr->tag) {
        case Load_TAG: return false;
        default: break;
    }
    return side_effects;
}

const Node* process(Context* ctx, const Node* old) {
    Rewriter* r = &ctx->rewriter;
    IrArena* a = r->dst_arena;
    if (old->tag == Function_TAG || old->tag == Constant_TAG) {
        Context c = *ctx;
        c.map = create_uses_map(old, NcType | NcDeclaration);
        const Node* new = recreate_node_identity(&c.rewriter, old);
        destroy_uses_map(c.map);
        return new;
    }

    switch (old->tag) {
        case BasicBlock_TAG: {
            size_t uses = count_calls(ctx->map, old);
            if (uses <= 1) {
                log_string(DEBUGVV, "Eliminating basic block '%s' since it's used only %d times.\n", get_abstraction_name_safe(old), uses);
                return NULL;
            }
            break;
        }
        case Jump_TAG: {
            const Node* otarget = old->payload.jump.target;
            const Node* ntarget = rewrite_node(r, otarget);
            if (!ntarget) {
                // it's been inlined away! just steal the body
                Nodes nargs = rewrite_nodes(r, old->payload.jump.args);
                register_processed_list(r, get_abstraction_params(otarget), nargs);
                register_processed(r, get_abstraction_mem(otarget), rewrite_node(r, old->payload.jump.mem));
                return rewrite_node(r, get_abstraction_body(otarget));
            }
            break;
        }
        default: break;
    }

    return recreate_node_identity(&ctx->rewriter, old);
}

OptPass simplify;

bool simplify(SHADY_UNUSED const CompilerConfig* config, Module** m) {
    Module* src = *m;

    IrArena* a = get_module_arena(src);
    *m = new_module(a, get_module_name(*m));
    bool todo = false;
    Context ctx = { .todo = &todo };
    ctx.rewriter = create_node_rewriter(src, *m, (RewriteNodeFn) process),
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    return todo;
}

OptPass opt_demote_alloca;
RewritePass import;

Module* cleanup(SHADY_UNUSED const CompilerConfig* config, Module* const src) {
    ArenaConfig aconfig = *get_arena_config(get_module_arena(src));
    if (!aconfig.check_types)
        return src;
    bool todo;
    size_t r = 0;
    Module* m = src;
    do {
        debugv_print("Cleanup round %d\n", r);
        if (getenv("SHADY_DUMP_CLEAN_ROUNDS"))
            log_module(DEBUGVV, config, m);
        todo = false;
        todo |= opt_demote_alloca(config, &m);
        if (getenv("SHADY_DUMP_CLEAN_ROUNDS"))
            log_module(DEBUGVV, config, m);
        todo |= simplify(config, &m);
        r++;
    } while (todo);
    return import(config, m);
}
