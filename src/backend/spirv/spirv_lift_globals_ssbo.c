#include "shady/pass.h"
#include "shady/ir/memory_layout.h"

#include "../shady/type.h"
#include "../shady/transform/ir_gen_helpers.h"

#include "dict.h"
#include "portability.h"
#include "log.h"

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
    BodyBuilder* bb;
    Node* lifted_globals_decl;
} Context;

static const Node* process(Context* ctx, const Node* node) {
    Rewriter* r = &ctx->rewriter;
    IrArena* a = r->dst_arena;

    switch (node->tag) {
        case Function_TAG: {
            Node* newfun = shd_recreate_node_head(r, node);
            if (get_abstraction_body(node)) {
                Context functx = *ctx;
                functx.rewriter.map = shd_clone_dict(functx.rewriter.map);
                shd_dict_clear(functx.rewriter.map);
                shd_register_processed_list(&functx.rewriter, get_abstraction_params(node), get_abstraction_params(newfun));
                functx.bb = begin_body_with_mem(a, shd_get_abstraction_mem(newfun));
                Node* post_prelude = basic_block(a, shd_empty(a), "post-prelude");
                shd_register_processed(&functx.rewriter, shd_get_abstraction_mem(node), shd_get_abstraction_mem(post_prelude));
                shd_set_abstraction_body(post_prelude, shd_rewrite_node(&functx.rewriter, get_abstraction_body(node)));
                shd_set_abstraction_body(newfun, finish_body(functx.bb, jump_helper(a, bb_mem(functx.bb), post_prelude,
                                                                                    shd_empty(a))));
                shd_destroy_dict(functx.rewriter.map);
            }
            return newfun;
        }
        case RefDecl_TAG: {
            const Node* odecl = node->payload.ref_decl.decl;
            if (odecl->tag != GlobalVariable_TAG || odecl->payload.global_variable.address_space != AsGlobal)
                break;
            assert(ctx->bb && "this RefDecl node isn't appearing in an abstraction - we cannot replace it with a load!");
            const Node* ptr_addr = gen_lea(ctx->bb, ref_decl_helper(a, ctx->lifted_globals_decl), shd_int32_literal(a, 0), shd_singleton(shd_rewrite_node(&ctx->rewriter, odecl)));
            const Node* ptr = gen_load(ctx->bb, ptr_addr);
            return ptr;
        }
        case GlobalVariable_TAG:
            if (node->payload.global_variable.address_space != AsGlobal)
                break;
            assert(false);
        default: break;
    }

    if (is_declaration(node)) {
        Context declctx = *ctx;
        declctx.bb = NULL;
        return shd_recreate_node(&declctx.rewriter, node);
    }

    return shd_recreate_node(&ctx->rewriter, node);
}

Module* shd_spvbe_pass_lift_globals_ssbo(SHADY_UNUSED const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *shd_get_arena_config(shd_module_get_arena(src));
    IrArena* a = shd_new_ir_arena(&aconfig);
    Module* dst = shd_new_module(a, shd_module_get_name(src));

    Context ctx = {
        .rewriter = shd_create_node_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config
    };

    Nodes old_decls = shd_module_get_declarations(src);
    LARRAY(const Type*, member_tys, old_decls.count);
    LARRAY(String, member_names, old_decls.count);

    Nodes annotations = mk_nodes(a, annotation(a, (Annotation) { .name = "Generated" }));
    annotations = shd_empty(a);

    annotations = shd_nodes_append(a, annotations, annotation_value(a, (AnnotationValue) { .name = "DescriptorSet", .value = shd_int32_literal(a, 0) }));
    annotations = shd_nodes_append(a, annotations, annotation_value(a, (AnnotationValue) { .name = "DescriptorBinding", .value = shd_int32_literal(a, 0) }));
    annotations = shd_nodes_append(a, annotations, annotation(a, (Annotation) { .name = "Constants" }));

    size_t lifted_globals_count = 0;
    for (size_t i = 0; i < old_decls.count; i++) {
        const Node* odecl = old_decls.nodes[i];
        if (odecl->tag != GlobalVariable_TAG || odecl->payload.global_variable.address_space != AsGlobal)
            continue;

        member_tys[lifted_globals_count] = shd_rewrite_node(&ctx.rewriter, odecl->type);
        member_names[lifted_globals_count] = get_declaration_name(odecl);

        shd_register_processed(&ctx.rewriter, odecl, shd_int32_literal(a, lifted_globals_count));
        lifted_globals_count++;
    }

    if (lifted_globals_count > 0) {
        const Type* lifted_globals_struct_t = record_type(a, (RecordType) {
            .members = shd_nodes(a, lifted_globals_count, member_tys),
            .names = shd_strings(a, lifted_globals_count, member_names),
            .special = DecorateBlock
        });
        ctx.lifted_globals_decl = global_var(dst, annotations, lifted_globals_struct_t, "lifted_globals", AsShaderStorageBufferObject);
    }

    shd_rewrite_module(&ctx.rewriter);

    lifted_globals_count = 0;
    for (size_t i = 0; i < old_decls.count; i++) {
        const Node* odecl = old_decls.nodes[i];
        if (odecl->tag != GlobalVariable_TAG || odecl->payload.global_variable.address_space != AsGlobal)
            continue;
        if (odecl->payload.global_variable.init)
            ctx.lifted_globals_decl->payload.global_variable.annotations = shd_nodes_append(a, ctx.lifted_globals_decl->payload.global_variable.annotations, annotation_values(a, (AnnotationValues) {
                .name = "InitialValue",
                .values = mk_nodes(a, shd_int32_literal(a, lifted_globals_count), shd_rewrite_node(&ctx.rewriter, odecl->payload.global_variable.init))
            }));

        lifted_globals_count++;
    }

    shd_destroy_rewriter(&ctx.rewriter);
    return dst;
}
