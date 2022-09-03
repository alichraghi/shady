#include "shady/ir.h"

#include "log.h"
#include "portability.h"

#include "../type.h"
#include "../rewrite.h"

#include "../transform/ir_gen_helpers.h"
#include "../analysis/free_variables.h"

#include "list.h"
#include "dict.h"

#include <assert.h>

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

typedef struct Context_ {
    Rewriter rewriter;
    struct Dict* spilled;
    struct Dict* lifted;
    struct List* new_fns;
} Context;

typedef struct {
    const Node* old_cont;
    const Node* lifted_fn;
    struct List* save_values;
} LiftedCont;

#pragma GCC diagnostic error "-Wswitch"

static const Node* process_block(Context* ctx, BlockBuilder* builder, const Block* oblock);

static void add_spill_instrs(Context* ctx, BlockBuilder* builder, struct List* spilled_vars) {
    IrArena* arena = ctx->rewriter.dst_arena;

    size_t recover_context_size = entries_count_list(spilled_vars);
    for (size_t i = 0; i < recover_context_size; i++) {
        const Node* ovar = read_list(const Node*, spilled_vars)[i];
        const Node* var = rewrite_node(&ctx->rewriter, ovar);

        const Node* args[] = { extract_operand_type(var->payload.var.type), var };
        const Node* save_instruction = prim_op(arena, (PrimOp) {
            .op = push_stack_op,
            .operands = nodes(arena, 2, args)
        });
        append_block(builder, save_instruction);
    }
}

static const Node* lift_continuation_into_function(Context* ctx, const Node* cont) {
    assert(cont->tag == Function_TAG);
    IrArena* dst_arena = ctx->rewriter.dst_arena;

    // Compute the live stuff we'll need
    struct List* recover_context = compute_free_variables(cont);
    size_t recover_context_size = entries_count_list(recover_context);

    debug_print("free variables at '%s': ", cont->payload.fn.name);
    for (size_t i = 0; i < recover_context_size; i++) {
        debug_print("%s", read_list(const Node*, recover_context)[i]->payload.var.name);
        if (i + 1 < recover_context_size)
            debug_print(", ");
    }
    debug_print("\n");

    // Create and register new parameters for the lifted continuation
    Nodes new_params = recreate_variables(&ctx->rewriter, cont->payload.fn.params);
    for (size_t i = 0; i < new_params.count; i++)
        register_processed(&ctx->rewriter, cont->payload.fn.params.nodes[i], new_params.nodes[i]);
        //register_processed(&new_ctx.rewriter, cont->payload.fn.params.nodes[i], new_params.nodes[i]);

    // Keep annotations the same
    Nodes annotations = rewrite_nodes(&ctx->rewriter, cont->payload.fn.annotations);
    Node* new_fn = fn(dst_arena, annotations, cont->payload.fn.name, false, new_params, nodes(dst_arena, 0, NULL));

    LiftedCont* lifted_cont = calloc(sizeof(LiftedCont), 1);
    lifted_cont->old_cont = cont;
    lifted_cont->lifted_fn = new_fn;
    lifted_cont->save_values = recover_context;
    insert_dict(const Node*, LiftedCont*, ctx->lifted, cont, lifted_cont);

    Context spilled_ctx = *ctx;
    spilled_ctx.spilled = new_dict(const Node*, Node*, (HashFn) hash_node, (CmpFn) compare_node);

    // Recover that stuff inside the new block
    BlockBuilder* builder = begin_block(dst_arena);
    for (size_t i = recover_context_size - 1; i < recover_context_size; i--) {
        const Node* ovar = read_list(const Node*, recover_context)[i];
        assert(ovar->tag == Variable_TAG);
        const char* output_names[] = { ovar->payload.var.name };

        const Type* type = rewrite_node(&ctx->rewriter, extract_operand_type(ovar->payload.var.type));

        const Node* let_load = let(dst_arena, prim_op(dst_arena, (PrimOp) {
            .op = pop_stack_op,
            .operands = nodes(dst_arena, 1, (const Node* []) {type})
        }), 1, output_names);

        // this dict overrides the 'processed' region
        insert_dict(const Node*, const Node*, spilled_ctx.spilled, ovar, let_load->payload.let.variables.nodes[0]);
        append_block(builder, let_load);
    }

    // Write out the rest of the new block using this fresh context
    const Node* nbody = process_block(&spilled_ctx, builder, &cont->payload.fn.block->payload.block);
    destroy_dict(spilled_ctx.spilled);

    new_fn->payload.fn.block = nbody;
    append_list(const Node*, ctx->new_fns, new_fn);

    return new_fn;
}

static const Node* process_block(Context* ctx, BlockBuilder* builder, const Block* oblock) {
    IrArena* arena = ctx->rewriter.dst_arena;

    for (size_t i = 0; i < oblock->instructions.count; i++) {
        append_block(builder, rewrite_node(&ctx->rewriter, oblock->instructions.nodes[i]));
    }

    const Node* const oterminator = oblock->terminator;
    const Node* nterminator;
    switch (is_terminator(oblock->terminator)) {
        case NotATerminator: error("Not a terminator");
        case Branch_TAG: {
            const Node* ncallee;
            switch (oblock->terminator->payload.branch.branch_mode) {
                case BrTailcall: goto identity;
                case BrJump: {
                    // make sure the target is rewritten before we lookup the 'lifted' dict
                    const Node* otarget = oblock->terminator->payload.branch.target;
                    const Node* ntarget = rewrite_node(&ctx->rewriter, otarget);

                    LiftedCont* lifted = *find_value_dict(const Node*, LiftedCont*, ctx->lifted, otarget);
                    assert(lifted->lifted_fn == ntarget);
                    add_spill_instrs(ctx, builder, lifted->save_values);

                    ncallee = fn_addr(arena, (FnAddr) { .fn = lifted->lifted_fn });
                    break;
                }
                case BrIfElse: {
                    const Node* otargets[] = { oblock->terminator->payload.branch.true_target, oblock->terminator->payload.branch.false_target };
                    const Node* ntargets[2];
                    const Node* blocks[2];
                    for (size_t i = 0; i < 2; i++) {
                        const Node* otarget = otargets[i];
                        const Node* ntarget = rewrite_node(&ctx->rewriter, otarget);
                        ntargets[i] = ntarget;

                        LiftedCont* lifted = *find_value_dict(const Node*, LiftedCont*, ctx->lifted, otarget);
                        assert(lifted->lifted_fn == ntarget);

                        BlockBuilder* case_builder = begin_block(arena);
                        add_spill_instrs(ctx, case_builder, lifted->save_values);
                        blocks[i] = finish_block(case_builder, merge_construct(arena, (MergeConstruct) { .args = nodes(arena, 0, NULL), .construct = Selection }));
                    }

                    // Put the spilling code inside a selection construct
                    const Node* ncondition = rewrite_node(&ctx->rewriter, oblock->terminator->payload.branch.branch_condition);
                    append_block(builder, if_instr(arena, (If) { .condition = ncondition, .if_true = blocks[0], .if_false = blocks[1], .yield_types = nodes(arena, 0, NULL) }));

                    // Make the callee selection a select
                    ncallee = gen_primop_ce(builder, select_op, 3, (const Node* []) { ncondition, fn_addr(arena, (FnAddr) { .fn = ntargets[0] }), fn_addr(arena, (FnAddr) { .fn = ntargets[1] }) });
                    break;
                }
                case BrSwitch: error("TODO")
            }
            assert(ncallee && is_value(ncallee));
            Nodes nargs = rewrite_nodes(&ctx->rewriter, oblock->terminator->payload.branch.args);
            nterminator = branch(arena, (Branch) {
                .branch_mode = BrTailcall,
                .target = ncallee,
                .args = nargs,
            });
            break;
        }
        case Join_TAG: error("TODO")
        case Callc_TAG: {
            const Callc ocallc = oterminator->payload.callc;
            assert(!ocallc.is_return_indirect && is_value(ocallc.callee));
            const Node* ncallee = rewrite_node(&ctx->rewriter, ocallc.callee);
            Nodes nargs = rewrite_nodes(&ctx->rewriter, ocallc.args);

            const Node* nreturn_cont = rewrite_node(&ctx->rewriter, ocallc.join_at);
            LiftedCont* lifted = *find_value_dict(const Node*, LiftedCont*, ctx->lifted, ocallc.join_at);
            assert(lifted->lifted_fn == nreturn_cont);

            add_spill_instrs(ctx, builder, lifted->save_values);

            nterminator = callc(arena, (Callc) {
                .callee = ncallee,
                .args = nargs,
                .is_return_indirect = true,
                .join_at = fn_addr(arena, (FnAddr) { .fn = nreturn_cont })
            });
            break;
        }
        identity:
        case Return_TAG:
        case MergeConstruct_TAG:
        case Unreachable_TAG: {
            nterminator = recreate_node_identity(&ctx->rewriter, oblock->terminator);
            break;
        }
    }
    assert(nterminator);
    return finish_block(builder, nterminator);
}

static const Node* process_node(Context* ctx, const Node* node) {
    if (ctx->spilled) {
        const Node** spilled = find_value_dict(const Node*, const Node*, ctx->spilled, node);
        if (spilled) return *spilled;
    }

    const Node* found = search_processed(&ctx->rewriter, node);
    if (found) return found;

    switch (node->tag) {
        case Block_TAG: return process_block(ctx, begin_block(ctx->rewriter.dst_arena), &node->payload.block);
        case Function_TAG: {
            if (node->payload.fn.is_basic_block)
                return lift_continuation_into_function(ctx, node);
            // leave other declarations alone
            return recreate_node_identity(&ctx->rewriter, node);
        }
        default: return recreate_node_identity(&ctx->rewriter, node);
    }
}

const Node* lower_continuations(SHADY_UNUSED CompilerConfig* config, IrArena* src_arena, IrArena* dst_arena, const Node* src_program) {
    Context ctx = {
        .rewriter = {
            .dst_arena = dst_arena,
            .src_arena = src_arena,
            .rewrite_fn = (RewriteFn) process_node,
            .processed = new_dict(const Node*, Node*, (HashFn) hash_node, (CmpFn) compare_node),
        },
        .new_fns = new_list(const Node*),
        .lifted = new_dict(const Node*, LiftedCont*, (HashFn) hash_node, (CmpFn) compare_node),
        .spilled = NULL,
    };

    assert(src_program->tag == Root_TAG);

    const Node* rewritten = recreate_node_identity(&ctx.rewriter, src_program);
    Nodes new_decls = rewritten->payload.root.declarations;
    for (size_t i = 0; i < entries_count_list(ctx.new_fns); i++) {
        new_decls = append_nodes(dst_arena, new_decls, read_list(const Node*, ctx.new_fns)[i]);
    }
    rewritten = root(dst_arena, (Root) {
        .declarations = new_decls
    });

    destroy_list(ctx.new_fns);
    {
        size_t iter = 0;
        LiftedCont* lifted_cont;
        while (dict_iter(ctx.lifted, &iter, NULL, &lifted_cont)) {
            destroy_list(lifted_cont->save_values);
            free(lifted_cont);
        }
        destroy_dict(ctx.lifted);
    }
    destroy_dict(ctx.rewriter.processed);
    return rewritten;
}