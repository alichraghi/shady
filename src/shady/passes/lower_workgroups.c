#include "pass.h"

#include "../ir_private.h"
#include "../type.h"
#include "../transform/ir_gen_helpers.h"

#include "util.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
    Node** globals;
    bool is_entry_point;
} Context;

static const Node* process(Context* ctx, const Node* node) {
    Rewriter* r = &ctx->rewriter;
    IrArena* a = r->dst_arena;
    Module* m = r->dst_module;

    switch (node->tag) {
        case GlobalVariable_TAG: {
            const Node* ba = lookup_annotation(node, "Builtin");
            if (ba) {
                Nodes filtered_as = rewrite_nodes(&ctx->rewriter, filter_out_annotation(a, node->payload.global_variable.annotations, "Builtin"));
                Builtin b = get_builtin_by_name(get_annotation_string_payload(ba));
                switch (b) {
                    case BuiltinSubgroupId:
                    case BuiltinWorkgroupId:
                    case BuiltinGlobalInvocationId:
                    case BuiltinLocalInvocationId:
                        return global_var(m, filtered_as, rewrite_node(&ctx->rewriter, node->payload.global_variable.type), node->payload.global_variable.name, AsPrivate);
                    case BuiltinNumWorkgroups:
                        return global_var(m, filtered_as, rewrite_node(&ctx->rewriter, node->payload.global_variable.type), node->payload.global_variable.name, AsExternal);
                    default:
                        break;
                }
                return get_or_create_builtin(ctx->rewriter.dst_module, b, get_declaration_name(node));
            }
            break;
        }
        case Function_TAG: {
            Context ctx2 = *ctx;
            ctx2.is_entry_point = false;
            const Node* epa = lookup_annotation(node, "EntryPoint");
            if (epa && strcmp(get_annotation_string_payload(epa), "Compute") == 0) {
                ctx2.is_entry_point = true;
                assert(node->payload.fun.return_types.count == 0 && "entry points do not return at this stage");

                Nodes wannotations = rewrite_nodes(&ctx->rewriter, node->payload.fun.annotations);
                Nodes wparams = recreate_params(&ctx->rewriter, node->payload.fun.params);
                Node* wrapper = function(m, wparams, get_abstraction_name(node), wannotations, empty(a));
                register_processed(&ctx->rewriter, node, wrapper);

                // recreate the old entry point, but this time it's not the entry point anymore
                Nodes nannotations = filter_out_annotation(a, wannotations, "EntryPoint");
                Nodes nparams = recreate_params(&ctx->rewriter, node->payload.fun.params);
                Node* inner = function(m, nparams, format_string_arena(a->arena, "%s_wrapped", get_abstraction_name(node)), nannotations, empty(a));
                register_processed_list(&ctx->rewriter, node->payload.fun.params, nparams);
                set_abstraction_body(inner, recreate_node_identity(&ctx->rewriter, node->payload.fun.body));

                BodyBuilder* bb = begin_body_with_mem(a, get_abstraction_mem(wrapper));
                const Node* num_workgroups_var = rewrite_node(&ctx->rewriter, get_or_create_builtin(ctx->rewriter.src_module, BuiltinNumWorkgroups, NULL));
                const Node* workgroup_num_vec3 = gen_load(bb, ref_decl_helper(a, num_workgroups_var));

                // prepare variables for iterating over workgroups
                String names[] = { "gx", "gy", "gz" };
                const Node* workgroup_id[3];
                const Node* num_workgroups[3];
                for (int dim = 0; dim < 3; dim++) {
                    workgroup_id[dim] = param(a, qualified_type_helper(uint32_type(a), false), names[dim]);
                    num_workgroups[dim] = gen_extract(bb, workgroup_num_vec3, singleton(uint32_literal(a, dim)));
                }

                // Prepare variables for iterating inside workgroups
                const Node* subgroup_id[3];
                uint32_t num_subgroups[3];
                const Node* num_subgroups_literals[3];
                assert(ctx->config->specialization.subgroup_size);
                assert(a->config.specializations.workgroup_size[0] && a->config.specializations.workgroup_size[1] && a->config.specializations.workgroup_size[2]);
                num_subgroups[0] = a->config.specializations.workgroup_size[0] / ctx->config->specialization.subgroup_size;
                num_subgroups[1] = a->config.specializations.workgroup_size[1];
                num_subgroups[2] = a->config.specializations.workgroup_size[2];
                String names2[] = { "sgx", "sgy", "sgz" };
                for (int dim = 0; dim < 3; dim++) {
                    subgroup_id[dim] = param(a, qualified_type_helper(uint32_type(a), false), names2[dim]);
                    num_subgroups_literals[dim] = uint32_literal(a, num_subgroups[dim]);
                }

                BodyBuilder* bb2 = begin_block_with_side_effects(a);
                // write the workgroup ID
                gen_store(bb2, ref_decl_helper(a, rewrite_node(&ctx->rewriter, get_or_create_builtin(ctx->rewriter.src_module, BuiltinWorkgroupId, NULL))), composite_helper(a, pack_type(a, (PackType) { .element_type = uint32_type(a), .width = 3 }), mk_nodes(a, workgroup_id[0], workgroup_id[1], workgroup_id[2])));
                // write the local ID
                const Node* local_id[3];
                // local_id[0] = SUBGROUP_SIZE * subgroup_id[0] + subgroup_local_id
                local_id[0] = gen_primop_e(bb2, add_op, empty(a), mk_nodes(a, gen_primop_e(bb2, mul_op, empty(a), mk_nodes(a, uint32_literal(a, ctx->config->specialization.subgroup_size), subgroup_id[0])), gen_builtin_load(m, bb, BuiltinSubgroupLocalInvocationId)));
                local_id[1] = subgroup_id[1];
                local_id[2] = subgroup_id[2];
                gen_store(bb2, ref_decl_helper(a, rewrite_node(&ctx->rewriter, get_or_create_builtin(ctx->rewriter.src_module, BuiltinLocalInvocationId, NULL))), composite_helper(a, pack_type(a, (PackType) { .element_type = uint32_type(a), .width = 3 }), mk_nodes(a, local_id[0], local_id[1], local_id[2])));
                // write the global ID
                const Node* global_id[3];
                for (int dim = 0; dim < 3; dim++)
                    global_id[dim] = gen_primop_e(bb2, add_op, empty(a), mk_nodes(a, gen_primop_e(bb2, mul_op, empty(a), mk_nodes(a, uint32_literal(a, a->config.specializations.workgroup_size[dim]), workgroup_id[dim])), local_id[dim]));
                gen_store(bb2, ref_decl_helper(a, rewrite_node(&ctx->rewriter, get_or_create_builtin(ctx->rewriter.src_module, BuiltinGlobalInvocationId, NULL))), composite_helper(a, pack_type(a, (PackType) { .element_type = uint32_type(a), .width = 3 }), mk_nodes(a, global_id[0], global_id[1], global_id[2])));
                // TODO: write the subgroup ID

                bind_instruction(bb2, call(a, (Call) { .callee = fn_addr_helper(a, inner), .args = wparams }));
                const Node* instr = yield_values_and_wrap_in_block(bb2, empty(a));

                // Wrap in 3 loops for iterating over subgroups, then again for workgroups
                for (int scope = 0; scope < 2; scope++) {
                    const Node** params;
                    const Node** maxes;
                    if (scope == 0) {
                        params = subgroup_id;
                        maxes = num_subgroups_literals;
                    } else if (scope == 1) {
                        params = workgroup_id;
                        maxes = num_workgroups;
                    } else
                        assert(false);
                    for (int dim = 0; dim < 3; dim++) {
                        Node* loop_body = case_(a, singleton(params[dim]));
                        BodyBuilder* body_bb = begin_body_with_mem(a, get_abstraction_mem(loop_body));
                        Node* out_of_bounds_case = case_(a, empty(a));
                        set_abstraction_body(out_of_bounds_case, merge_break(a, (MergeBreak) {.args = empty(a), .mem = get_abstraction_mem(out_of_bounds_case)}));
                        gen_if(body_bb, empty(a), gen_primop_e(body_bb, gte_op, empty(a), mk_nodes(a, params[dim], maxes[dim])), out_of_bounds_case, NULL);
                        bind_instruction(body_bb, instr);

                        BodyBuilder* bb3 = begin_block_with_side_effects(a);
                        set_abstraction_body(loop_body, finish_body(body_bb, merge_continue(a, (MergeContinue) {.args = singleton(gen_primop_e(body_bb, add_op, empty(a), mk_nodes(a, params[dim], uint32_literal(a, 1))))})));
                        gen_loop(bb3, empty(a), singleton(uint32_literal(a, 0)), loop_body);
                        instr = yield_values_and_wrap_in_block(bb3, empty(a));
                    }
                }

                bind_instruction(bb, instr);
                set_abstraction_body(wrapper, finish_body(bb, fn_ret(a, (Return) { .args = empty(a) })));
                return wrapper;
            }
            return recreate_node_identity(&ctx2.rewriter, node);
        }
        case Load_TAG: {
            Load payload = node->payload.load;
            const Node* ptr = payload.ptr;
            if (ptr->tag == RefDecl_TAG)
                ptr = ptr->payload.ref_decl.decl;
            if (ptr == get_or_create_builtin(ctx->rewriter.src_module, BuiltinSubgroupId, NULL)) {
                BodyBuilder* bb = begin_body_with_mem(a, rewrite_node(r, payload.mem));
                const Node* loaded = first(bind_instruction(bb, recreate_node_identity(&ctx->rewriter, node)));
                const Node* uniformized = first(gen_primop(bb, subgroup_broadcast_first_op, empty(a), singleton(loaded)));
                return yield_values_and_wrap_in_block(bb, singleton(uniformized));
            }
        }
        default: break;
    }

    return recreate_node_identity(&ctx->rewriter, node);
}

Module* lower_workgroups(const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(&aconfig);
    Module* dst = new_module(a, get_module_name(src));
    Context ctx = {
        .rewriter = create_node_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
        .globals = calloc(sizeof(Node*), PRIMOPS_COUNT),
    };
    rewrite_module(&ctx.rewriter);
    free(ctx.globals);
    destroy_rewriter(&ctx.rewriter);
    return dst;
}
