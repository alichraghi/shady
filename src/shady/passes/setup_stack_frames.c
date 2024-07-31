#include "pass.h"

#include "../visit.h"
#include "../type.h"
#include "../ir_private.h"
#include "../transform/ir_gen_helpers.h"

#include "log.h"
#include "portability.h"
#include "list.h"
#include "util.h"

#include <assert.h>

typedef struct Context_ {
    Rewriter rewriter;
    bool disable_lowering;

    const CompilerConfig* config;
    const Node* stack_size_on_entry;
} Context;

static const Node* process(Context* ctx, const Node* node) {
    const Node* found = search_processed(&ctx->rewriter, node);
    if (found) return found;

    IrArena* a = ctx->rewriter.dst_arena;
    Rewriter* r = &ctx->rewriter;
    switch (node->tag) {
        case Function_TAG: {
            Node* fun = recreate_decl_header_identity(r, node);
            Context ctx2 = *ctx;
            ctx2.disable_lowering = lookup_annotation_with_string_payload(node, "DisablePass", "setup_stack_frames") || ctx->config->per_thread_stack_size == 0;

            BodyBuilder* bb = begin_body(a);
            if (!ctx2.disable_lowering) {
                ctx2.stack_size_on_entry = gen_get_stack_size(bb);
                set_value_name((Node*) ctx2.stack_size_on_entry, format_string_arena(a->arena, "saved_stack_ptr_entering_%s", get_abstraction_name(fun)));
            }
            if (node->payload.fun.body)
                fun->payload.fun.body = finish_body(bb, rewrite_node(&ctx2.rewriter, node->payload.fun.body));
            else
                cancel_body(bb);
            return fun;
        }
        case Return_TAG: {
            BodyBuilder* bb = begin_body(a);
            if (!ctx->disable_lowering) {
                assert(ctx->stack_size_on_entry);
                // Restore SP before calling exit
                gen_set_stack_size(bb, ctx->stack_size_on_entry);
            }
            return finish_body(bb, recreate_node_identity(r, node));
        }
        default: break;
    }
    return recreate_node_identity(r, node);
}

Module* setup_stack_frames(SHADY_UNUSED const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(&aconfig);
    Module* dst = new_module(a, get_module_name(src));
    Context ctx = {
        .rewriter = create_node_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
    };
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    return dst;
}
