#include "type.h"

#include "log.h"
#include "ir_private.h"
#include "portability.h"

#include "dict.h"

#include <string.h>
#include <assert.h>

#pragma GCC diagnostic error "-Wswitch"

static bool are_types_identical(size_t num_types, const Type* types[]) {
    for (size_t i = 0; i < num_types; i++) {
        assert(types[i]);
        if (types[0] != types[i])
            return false;
    }
    return true;
}

bool is_subtype(const Type* supertype, const Type* type) {
    assert(supertype && type);
    if (supertype->tag != type->tag)
        return false;
    switch (is_type(supertype)) {
        case NotAType: error("supplied not a type to is_subtype");
        case QualifiedType_TAG: {
            // uniform T <: varying T
            if (supertype->payload.qualified_type.is_uniform && !type->payload.qualified_type.is_uniform)
                return false;
            return is_subtype(supertype->payload.qualified_type.type, type->payload.qualified_type.type);
        }
        case RecordType_TAG: {
            const Nodes* supermembers = &supertype->payload.record_type.members;
            const Nodes* members = &type->payload.record_type.members;
            for (size_t i = 0; i < members->count; i++) {
                if (!is_subtype(supermembers->nodes[i], members->nodes[i]))
                    return false;
            }
            return true;
        }
        case JoinPointType_TAG: {
            const Nodes* superparams = &supertype->payload.join_point_type.yield_types;
            const Nodes* params = &type->payload.join_point_type.yield_types;
            if (params->count != superparams->count) return false;
            for (size_t i = 0; i < params->count; i++) {
                if (!is_subtype(params->nodes[i], superparams->nodes[i]))
                    return false;
            }
            return true;
        }
        case FnType_TAG: {
            // check returns
            if (supertype->payload.fn_type.return_types.count != type->payload.fn_type.return_types.count)
                return false;
            for (size_t i = 0; i < type->payload.fn_type.return_types.count; i++)
                if (!is_subtype(supertype->payload.fn_type.return_types.nodes[i], type->payload.fn_type.return_types.nodes[i]))
                    return false;
            // check params
            const Nodes* superparams = &supertype->payload.fn_type.param_types;
            const Nodes* params = &type->payload.fn_type.param_types;
            if (params->count != superparams->count) return false;
            for (size_t i = 0; i < params->count; i++) {
                if (!is_subtype(params->nodes[i], superparams->nodes[i]))
                    return false;
            }
            return true;
        } case BBType_TAG: {
            // check params
            const Nodes* superparams = &supertype->payload.bb_type.param_types;
            const Nodes* params = &type->payload.bb_type.param_types;
            if (params->count != superparams->count) return false;
            for (size_t i = 0; i < params->count; i++) {
                if (!is_subtype(params->nodes[i], superparams->nodes[i]))
                    return false;
            }
            return true;
        } case LamType_TAG: {
            // check params
            const Nodes* superparams = &supertype->payload.lam_type.param_types;
            const Nodes* params = &type->payload.lam_type.param_types;
            if (params->count != superparams->count) return false;
            for (size_t i = 0; i < params->count; i++) {
                if (!is_subtype(params->nodes[i], superparams->nodes[i]))
                    return false;
            }
            return true;
        } case PtrType_TAG: {
            if (supertype->payload.ptr_type.address_space != type->payload.ptr_type.address_space)
                return false;
            return is_subtype(supertype->payload.ptr_type.pointed_type, type->payload.ptr_type.pointed_type);
        }
        case Int_TAG: return supertype->payload.int_type.width == type->payload.int_type.width;
        case ArrType_TAG: {
            return supertype->payload.arr_type.size == type->payload.arr_type.size
            && is_subtype(supertype->payload.arr_type.element_type, type->payload.arr_type.element_type);
        }
        case PackType_TAG: {
            return supertype->payload.pack_type.width == type->payload.pack_type.width
            && is_subtype(supertype->payload.pack_type.element_type, type->payload.pack_type.element_type);
        }
        case Type_TypeDeclRef_TAG: {
            return supertype->payload.type_decl_ref.decl == type->payload.type_decl_ref.decl;
        }
        case NoRet_TAG:
        case Bool_TAG:
        case MaskType_TAG:
        case Float_TAG:
            return true;
    }
    SHADY_UNREACHABLE;
}

void check_subtype(const Type* supertype, const Type* type) {
    if (!is_subtype(supertype, type)) {
        log_node(ERROR, type);
        error_print(" isn't a subtype of ");
        log_node(ERROR, supertype);
        error_print("\n");
        error("failed check_subtype")
    }
}

/// Oracle of what casts are legal
static bool is_reinterpret_cast_legal(const Type* src_type, const Type* dst_type) {
    // TODO implement rules
    assert(is_type(src_type) && is_type(dst_type));
    return true;
}

/// Does the same point value refer to the same memory, across the invocations in a subgroup ?
bool is_addr_space_uniform(IrArena* arena, AddressSpace as) {
    switch (as) {
        case AsFunctionLogical:
        case AsPrivateLogical:
        case AsPrivatePhysical:
        case AsInput:
            return !arena->config.is_simt;
        default:
            return true;
    }
}

static const Type* get_actual_mask_type(IrArena* arena) {
    switch (arena->config.subgroup_mask_representation) {
        case SubgroupMaskAbstract: return mask_type(arena);
        case SubgroupMaskSpvKHRBallot: return pack_type(arena, (PackType) { .element_type = int32_type(arena), .width = 4 });
        default: assert(false);
    }
}

String name_type_safe(IrArena* arena, const Type* t) {
    switch (is_type(t)) {
        case NotAType: assert(false);
        case Type_MaskType_TAG: return "mask_t";
        case Type_JoinPointType_TAG: return "join_type_t";
        case Type_NoRet_TAG: return "no_ret";
        case Type_Int_TAG: return format_string(arena, "int_%s", ((String[]) { "8", "16", "32", "64" })[t->payload.int_type.width]);
        case Type_Float_TAG: return "float";
        case Type_Bool_TAG: return "bool";
        case Type_RecordType_TAG: break;
        case Type_FnType_TAG: break;
        case Type_BBType_TAG: break;
        case Type_LamType_TAG: break;
        case Type_PtrType_TAG: break;
        case Type_QualifiedType_TAG: break;
        case Type_ArrType_TAG: break;
        case Type_PackType_TAG: break;
        case Type_TypeDeclRef_TAG: return t->payload.type_decl_ref.decl->payload.nom_type.name;
    }
    return unique_name(arena, node_tags[t->tag]);
}

/// Is this a type that a value in the language can have ?
bool is_value_type(const Type* type) {
    if (type->tag != QualifiedType_TAG)
        return false;
    return is_data_type(get_unqualified_type(type));
}

/// Is this a valid data type (for usage in other types and as type arguments) ?
bool is_data_type(const Type* type) {
    switch (is_type(type)) {
        case Type_MaskType_TAG:
        case Type_JoinPointType_TAG:
        case Type_Int_TAG:
        case Type_Float_TAG:
        case Type_Bool_TAG:
        case Type_PtrType_TAG:
        case Type_ArrType_TAG:
        case Type_PackType_TAG:
            return true;
        // multi-return record types are the results of instructions, but are not values themselves
        case Type_RecordType_TAG:
            return type->payload.record_type.special == NotSpecial;
        case Type_TypeDeclRef_TAG:
            return !get_nominal_type_body(type) || is_data_type(get_nominal_type_body(type));
        // qualified types are not data types because that information is only meant for values
        case Type_QualifiedType_TAG: return false;
        // values cannot contain abstractions
        case Type_FnType_TAG:
        case Type_BBType_TAG:
        case Type_LamType_TAG:
            return false;
        // this type has no values to begin with
        case Type_NoRet_TAG:
            return false;
        case NotAType:
            return false;
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

const Type* check_type_join_point_type(IrArena* arena, JoinPointType type) {
    for (size_t i = 0; i < type.yield_types.count; i++) {
        assert(!contains_qualified_type(type.yield_types.nodes[i]));
    }
    return NULL;
}

const Type* check_type_record_type(IrArena* arena, RecordType type) {
    assert(type.names.count == 0 || type.names.count == type.members.count);
    for (size_t i = 0; i < type.members.count; i++) {
        assert((type.special == MultipleReturn) == contains_qualified_type(type.members.nodes[i]));
    }
    return NULL;
}

const Type* check_type_qualified_type(IrArena* arena, QualifiedType qualified_type) {
    assert(is_data_type(qualified_type.type));
    assert(arena->config.is_simt || qualified_type.is_uniform);
    return NULL;
}

const Type* check_type_arr_type(IrArena* arena, ArrType type) {
    assert(is_data_type(type.element_type));
    return NULL;
}

const Type* check_type_pack_type(IrArena* arena, PackType pack_type) {
    assert(is_data_type(pack_type.element_type));
    return NULL;
}

const Type* check_type_var(IrArena* arena, Variable variable) {
    assert(variable.type);
    return variable.type;
}

const Type* check_type_untyped_number(IrArena* arena, UntypedNumber untyped) {
    error("should never happen");
}

const Type* check_type_int_literal(IrArena* arena, IntLiteral lit) {
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = true,
        .type = int_type(arena, (Int) { .width = lit.width })
    });
}

const Type* check_type_true_lit(IrArena* arena) { return qualified_type(arena, (QualifiedType) { .type = bool_type(arena), .is_uniform = true }); }
const Type* check_type_false_lit(IrArena* arena) { return qualified_type(arena, (QualifiedType) { .type = bool_type(arena), .is_uniform = true }); }

const Type* check_type_string_lit(IrArena* arena, StringLiteral str_lit) {
    const Type* t = arr_type(arena, (ArrType) {
        .element_type = int8_type(arena),
        .size = int32_literal(arena, strlen(str_lit.string))
    });
    return qualified_type(arena, (QualifiedType) {
        .type = t,
        .is_uniform = true,
    });
}

const Type* check_type_composite(IrArena* arena, Composite composite) {
    Nodes expected_member_types = get_composite_type_element_types(composite.type);
    bool is_uniform = true;
    assert(composite.contents.count == expected_member_types.count);
    for (size_t i = 0; i < composite.contents.count; i++) {
        const Type* element_type = composite.contents.nodes[i]->type;
        is_uniform &= deconstruct_qualified_type(&element_type);
        assert(is_subtype(expected_member_types.nodes[i], element_type));
    }
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = is_uniform,
        .type = composite.type
    });
}

const Type* check_type_fn_addr(IrArena* arena, FnAddr fn_addr) {
    assert(fn_addr.fn->type->tag == FnType_TAG);
    assert(fn_addr.fn->tag == Function_TAG);
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = true,
        .type = ptr_type(arena, (PtrType) {
            .pointed_type = fn_addr.fn->type,
            .address_space = AsProgramCode,
        })
    });
}

const Type* check_type_ref_decl(IrArena* arena, RefDecl ref_decl) {
    const Type* t = ref_decl.decl->type;
    assert(t && "RefDecl needs to be applied on a decl with a non-null type. Did you forget to set 'type' on a constant ?");
    switch (ref_decl.decl->tag) {
        case GlobalVariable_TAG:
        case Constant_TAG: break;
        default: error("You can only use RefDecl on a global or a constant. See FnAddr for taking addresses of functions.")
    }
    assert(!contains_qualified_type(t));
    return qualified_type(arena, (QualifiedType) {
        .type = t,
        .is_uniform = true,
    });
}

static bool can_do_arithm(const Type* t) {
    return t->tag == Int_TAG || t->tag == Float_TAG;
}

static bool can_do_bitstuff(const Type* t) {
    return t->tag == Int_TAG || t->tag == Bool_TAG || t->tag == MaskType_TAG;
}

static bool can_be_compared(bool ordered, const Type* t) {
    if (ordered)
        return can_do_arithm(t);
    return true; // TODO this is fine to allow, but we'll need to lower it for composite and native ptr types !
}

const Type* check_type_prim_op(IrArena* arena, PrimOp prim_op) {
    for (size_t i = 0; i < prim_op.type_arguments.count; i++) {
        const Node* ta = prim_op.type_arguments.nodes[i];
        assert(ta && is_type(ta));
    }
    for (size_t i = 0; i < prim_op.operands.count; i++) {
        const Node* operand = prim_op.operands.nodes[i];
        assert(operand && is_value(operand));
    }

    bool ordered = false;
    AddressSpace as;
    switch (prim_op.op) {
        case assign_op:
        case subscript_op: error("These ops are only allowed in untyped IR before desugaring. They don't type to anything.");
        case quote_op: {
            assert(prim_op.type_arguments.count == 0);
            return wrap_multiple_yield_types(arena, get_values_types(arena, prim_op.operands));
        }
        case neg_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);

            const Type* type = first(prim_op.operands)->type;
            assert(can_do_arithm(get_maybe_packed_type_element(get_unqualified_type(type))));
            return type;
        }
        case rshift_arithm_op:
        case rshift_logical_op:
        case lshift_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = first(prim_op.operands)->type;
            const Type* second_operand_type = prim_op.operands.nodes[1]->type;

            bool uniform_result = deconstruct_qualified_type(&first_operand_type);
            uniform_result &= deconstruct_qualified_type(&second_operand_type);

            size_t value_simd_width = deconstruct_maybe_packed_type(&first_operand_type);
            size_t shift_simd_width = deconstruct_maybe_packed_type(&second_operand_type);
            assert(value_simd_width == shift_simd_width);

            assert(first_operand_type->tag == Int_TAG);
            assert(second_operand_type->tag == Int_TAG);

            return qualified_type_helper(maybe_packed_type_helper(first_operand_type, value_simd_width), uniform_result);
        }
        case add_op:
        case sub_op:
        case mul_op:
        case div_op:
        case mod_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = get_unqualified_type(first(prim_op.operands)->type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = deconstruct_qualified_type(&operand_type);

                assert(can_do_arithm(get_maybe_packed_type_element(operand_type)));
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return qualified_type_helper(first_operand_type, result_uniform);
        }

        case not_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);

            const Type* type = first(prim_op.operands)->type;
            assert(can_do_bitstuff(get_maybe_packed_type_element(get_unqualified_type(type))));
            return type;
        }
        case or_op:
        case xor_op:
        case and_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = get_unqualified_type(first(prim_op.operands)->type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = deconstruct_qualified_type(&operand_type);

                assert(can_do_bitstuff(get_maybe_packed_type_element(operand_type)));
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return qualified_type_helper(first_operand_type, result_uniform);
        }
        case lt_op:
        case lte_op:
        case gt_op:
        case gte_op: ordered = true; SHADY_FALLTHROUGH
        case eq_op:
        case neq_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = get_unqualified_type(first(prim_op.operands)->type);
            size_t first_operand_width = get_maybe_packed_type_width(first_operand_type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = deconstruct_qualified_type(&operand_type);

                assert(can_be_compared(ordered, get_maybe_packed_type_element(operand_type)));
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return qualified_type_helper(maybe_packed_type_helper(bool_type(arena), first_operand_width), result_uniform);
        }
        case load_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);

            const Node* ptr = first(prim_op.operands);
            const Node* ptr_type = ptr->type;
            bool ptr_uniform = deconstruct_qualified_type(&ptr_type);
            size_t width = deconstruct_maybe_packed_type(&ptr_type);

            assert(ptr_type->tag == PtrType_TAG);
            const PtrType* node_ptr_type_ = &ptr_type->payload.ptr_type;
            const Type* elem_type = node_ptr_type_->pointed_type;
            elem_type = maybe_packed_type_helper(elem_type, width);
            return qualified_type_helper(elem_type, ptr_uniform && is_addr_space_uniform(arena, ptr_type->payload.ptr_type.address_space));
        }
        case store_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);

            const Node* ptr = first(prim_op.operands);
            const Node* ptr_type = ptr->type;
            bool ptr_uniform = deconstruct_qualified_type(&ptr_type);
            size_t width = deconstruct_maybe_packed_type(&ptr_type);
            assert(ptr_type->tag == PtrType_TAG);
            const PtrType* ptr_type_payload = &ptr_type->payload.ptr_type;
            const Type* elem_type = ptr_type_payload->pointed_type;
            elem_type = maybe_packed_type_helper(elem_type, width);
            // we don't enforce uniform stores - but we care about storing the right thing :)
            const Type* val_expected_type = qualified_type(arena, (QualifiedType) {
                .is_uniform = !arena->config.is_simt,
                .type = elem_type
            });

            const Node* val = prim_op.operands.nodes[1];
            assert(is_subtype(val_expected_type, val->type));
            return unit_type(arena);
        }
        case alloca_logical_op:  as = AsFunctionLogical;  goto alloca_case;
        case alloca_subgroup_op: as = AsSubgroupPhysical; goto alloca_case;
        case alloca_op:          as = AsPrivatePhysical;  goto alloca_case;
        alloca_case: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 0);
            const Type* elem_type = prim_op.type_arguments.nodes[0];
            assert(is_type(elem_type));
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = is_addr_space_uniform(arena, as),
                .type = ptr_type(arena, (PtrType) {
                    .pointed_type = elem_type,
                    .address_space = as,
                })
            });
        }
        case lea_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count >= 2);

            const Node* base = prim_op.operands.nodes[0];
            bool uniform = is_qualified_type_uniform(base->type);

            const Type* curr_ptr_type = get_unqualified_type(base->type);
            assert(curr_ptr_type->tag == PtrType_TAG && "lea expects a pointer as a base");

            const Node* offset = prim_op.operands.nodes[1];
            assert(offset);
            const Type* offset_type = offset->type;
            bool offset_uniform = deconstruct_qualified_type(&offset_type);
            assert(offset_type->tag == Int_TAG && "lea expects an integer offset");
            const Type* pointee_type = curr_ptr_type->payload.ptr_type.pointed_type;

            const IntLiteral* lit = resolve_to_literal(offset);
            bool offset_is_zero = lit && lit->value.i64 == 0;
            assert(offset_is_zero || pointee_type->tag == ArrType_TAG && "if an offset is used, the base pointer must point to an array");
            uniform &= offset_uniform;

            // enter N levels of pointers
            size_t i = 2;
            while (true) {
                assert(curr_ptr_type->tag == PtrType_TAG && "lea is supposed to work on, and yield pointers");
                if (i >= prim_op.operands.count) break;

                const Node* selector = prim_op.operands.nodes[i];
                const Type* selector_type = selector->type;
                bool selector_uniform = deconstruct_qualified_type(&selector_type);

                assert(selector_type->tag == Int_TAG && "selectors must be integers");
                uniform &= selector_uniform;
                const Type* pointee_type = curr_ptr_type->payload.ptr_type.pointed_type;
                switch (pointee_type->tag) {
                    case ArrType_TAG: {
                        curr_ptr_type = ptr_type(arena, (PtrType) {
                            .pointed_type = pointee_type->payload.arr_type.element_type,
                            .address_space = curr_ptr_type->payload.ptr_type.address_space
                        });
                        break;
                    }
                    case TypeDeclRef_TAG: {
                        const Node* decl = pointee_type->payload.type_decl_ref.decl;
                        assert(decl && decl->tag == NominalType_TAG);
                        pointee_type = decl->payload.nom_type.body;
                        assert(pointee_type);
                        SHADY_FALLTHROUGH
                    }
                    case RecordType_TAG: {
                        assert(selector->tag == IntLiteral_TAG && "selectors when indexing into a record need to be constant");
                        size_t index = get_int_literal_value(selector, false);
                        assert(index < pointee_type->payload.record_type.members.count);
                        curr_ptr_type = ptr_type(arena, (PtrType) {
                            .pointed_type = pointee_type->payload.record_type.members.nodes[index],
                            .address_space = curr_ptr_type->payload.ptr_type.address_space
                        });
                        break;
                    }
                    // also remember to assert literals for the selectors !
                    default: error("lea selectors can only work on pointers to arrays or records")
                }
                i++;
            }

            return qualified_type(arena, (QualifiedType) {
                .is_uniform = uniform,
                .type = curr_ptr_type
            });
        }
        case reinterpret_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 1);
            const Node* source = prim_op.operands.nodes[0];
            const Type* src_type = source->type;
            bool src_uniform = deconstruct_qualified_type(&src_type);

            const Type* target_type = prim_op.type_arguments.nodes[0];
            assert(is_data_type(target_type));
            assert(is_reinterpret_cast_legal(src_type, target_type));

            return qualified_type(arena, (QualifiedType) {
                .is_uniform = src_uniform,
                .type = target_type
            });
        }
        case select_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 3);
            const Type* condition_type = prim_op.operands.nodes[0]->type;
            bool condition_uniform = deconstruct_qualified_type(&condition_type);
            size_t width = deconstruct_maybe_packed_type(&condition_type);

            const Type* alternatives_types[2];
            bool alternatives_all_uniform = true;
            for (size_t i = 0; i < 2; i++) {
                alternatives_types[i] = prim_op.operands.nodes[1 + i]->type;
                alternatives_all_uniform &= deconstruct_qualified_type(&alternatives_types[i]);
                size_t alternative_width = deconstruct_maybe_packed_type(&alternatives_types[i]);
                assert(alternative_width == width);
            }

            assert(is_subtype(bool_type(arena), condition_type));
            // todo find true supertype
            assert(are_types_identical(2, alternatives_types));

            return qualified_type_helper(maybe_packed_type_helper(alternatives_types[0], width), alternatives_all_uniform && condition_uniform);
        }
        case extract_dynamic_op:
        case extract_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count >= 2);
            const Type* source = prim_op.operands.nodes[0];

            const Type* current_type = source->type;
            bool is_uniform = deconstruct_qualified_type(&current_type);

            for (size_t i = 1; i < prim_op.operands.count; i++) {
                assert(is_data_type(current_type));

                // Check index is valid !
                const Node* ith_index = prim_op.operands.nodes[i];
                bool dynamic_index = prim_op.op == extract_dynamic_op;
                if (dynamic_index) {
                    const Type* index_type = ith_index->type;
                    bool index_uniform = deconstruct_qualified_type(&index_type);
                    is_uniform &= index_uniform;
                    assert(index_type->tag == Int_TAG && "extract_dynamic requires integers for the indices");
                } else {
                    assert(ith_index->tag == IntLiteral_TAG && "extract takes integer literals");
                }

                // Go down one level...
                try_again:
                switch(current_type->tag) {
                    case RecordType_TAG: {
                        assert(!dynamic_index);
                        size_t index_value = ith_index->payload.int_literal.value.i32;
                        assert(index_value < current_type->payload.record_type.members.count);
                        current_type = current_type->payload.record_type.members.nodes[index_value];
                        continue;
                    }
                    case ArrType_TAG: {
                        assert(!dynamic_index);
                        current_type = current_type->payload.arr_type.element_type;
                        continue;
                    }
                    case TypeDeclRef_TAG: {
                        assert(!dynamic_index);
                        const Node* nom_decl = current_type->payload.type_decl_ref.decl;
                        assert(nom_decl->tag == NominalType_TAG);
                        current_type = nom_decl->payload.nom_type.body;
                        goto try_again;
                    }
                    case PackType_TAG: {
                        current_type = current_type->payload.pack_type.element_type;
                        continue;
                    }
                    default: error("Not a valid type to extract from")
                }
            }
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = is_uniform,
                .type = current_type
            });
        }
        case convert_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 1);
            const Type* dst_type = prim_op.type_arguments.nodes[0];
            assert(is_data_type(dst_type));

            const Type* src_type = prim_op.operands.nodes[0];
            bool is_uniform = deconstruct_qualified_type(&src_type);
            // TODO check the conversion is legal
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = is_uniform,
                .type = dst_type
            });
        }
        // Mask management
        case empty_mask_op: {
            assert(prim_op.type_arguments.count == 0 && prim_op.operands.count == 0);
            return qualified_type_helper(get_actual_mask_type(arena), true);
        }
        case mask_is_thread_active_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = is_qualified_type_uniform(prim_op.operands.nodes[0]->type) && is_qualified_type_uniform(prim_op.operands.nodes[1]->type),
                .type = bool_type(arena)
            });
        }
        // Subgroup ops
        case subgroup_active_mask_op: {
            assert(prim_op.type_arguments.count == 0 && prim_op.operands.count == 0);
            return qualified_type_helper(get_actual_mask_type(arena), true);
        }
        case subgroup_ballot_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = get_actual_mask_type(arena)
            });
        }
        case subgroup_elect_first_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = false,
                .type = bool_type(arena)
            });
        }
        case subgroup_broadcast_first_op:
        case subgroup_reduce_sum_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);
            const Type* operand_type = get_unqualified_type(prim_op.operands.nodes[0]->type);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = operand_type
            });
        }
        // Intermediary ops
        case create_joint_point_op: {
            assert(prim_op.operands.count == 1);
            const Node* join_point = first(prim_op.operands);
            assert(is_qualified_type_uniform(join_point->type));
            return qualified_type(arena, (QualifiedType) { .type = join_point_type(arena, (JoinPointType) { .yield_types = prim_op.type_arguments }), .is_uniform = true });
        }
        // Invocation ID and compute kernel stuff
        case subgroup_local_id_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = false,
                .type = int32_type(arena)
            });
        }
        case subgroup_id_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = int32_type(arena)
            });
        }
        case workgroup_id_op:
        case workgroup_num_op:
        case workgroup_size_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = pack_type(arena, (PackType) { .element_type = int32_type(arena), .width = 3 })
            });
        }
        case workgroup_local_id_op:
        case global_id_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = false,
                .type = pack_type(arena, (PackType) { .element_type = int32_type(arena), .width = 3 })
            });
        }
        // Stack stuff
        case get_stack_pointer_op:
        case get_stack_pointer_uniform_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) { .is_uniform = prim_op.op == get_stack_pointer_uniform_op, .type = int32_type(arena) });
        }
        case get_stack_base_op:
        case get_stack_base_uniform_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 0);
            const Node* ptr = ptr_type(arena, (PtrType) { .pointed_type = arr_type(arena, (ArrType) { .element_type = int32_type(arena), .size = NULL }), .address_space = prim_op.op == get_stack_base_op ? AsPrivatePhysical : AsSubgroupPhysical});
            return qualified_type(arena, (QualifiedType) { .is_uniform = prim_op.op == get_stack_base_uniform_op, .type = ptr });
        }
        case set_stack_pointer_op:
        case set_stack_pointer_uniform_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);
            bool is_uniform = prim_op.op == set_stack_pointer_uniform_op;
            if (is_uniform)
                assert(is_qualified_type_uniform(prim_op.operands.nodes[0]->type));
            assert(get_unqualified_type(prim_op.operands.nodes[0]->type) == int32_type(arena));
            return unit_type(arena);
        }
        case push_stack_uniform_op:
        case push_stack_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 1);
            const Type* element_type = first(prim_op.type_arguments);
            assert(is_data_type(element_type));
            const Type* qual_element_type = qualified_type(arena, (QualifiedType) {
                .is_uniform = prim_op.op == push_stack_uniform_op,
                .type = element_type
            });
            // the operand has to be a subtype of the annotated type
            assert(is_subtype(qual_element_type, first(prim_op.operands)->type));
            return unit_type(arena);
        }
        case pop_stack_op:
        case pop_stack_uniform_op: {
            assert(prim_op.operands.count == 0);
            assert(prim_op.type_arguments.count == 1);
            const Type* element_type = prim_op.type_arguments.nodes[0];
            assert(is_data_type(element_type));
            return qualified_type(arena, (QualifiedType) { .is_uniform = prim_op.op == pop_stack_uniform_op, .type = element_type});
        }
        // Debugging ops
        case debug_printf_op: {
            assert(prim_op.type_arguments.count == 0);
            // TODO ?
            return unit_type(arena);
        }
        case PRIMOPS_COUNT: assert(false);
    }
}

static void check_arguments_types_against_parameters_helper(Nodes param_types, Nodes arg_types) {
    if (param_types.count != arg_types.count)
        error("Mismatched number of arguments/parameters");
    for (size_t i = 0; i < param_types.count; i++)
        check_subtype(param_types.nodes[i], arg_types.nodes[i]);
}

/// Shared logic between indirect calls and tailcalls
static Nodes check_value_call(const Node* callee, Nodes argument_types) {
    assert(is_value(callee));

    const Type* callee_type = callee->type;
    SHADY_UNUSED bool callee_uniform = deconstruct_qualified_type(&callee_type);
    AddressSpace as = deconstruct_pointer_type(&callee_type);
    assert(as == AsProgramCode);

    assert(callee_type->tag == FnType_TAG);

    const FnType* fn_type = &callee_type->payload.fn_type;
    check_arguments_types_against_parameters_helper(fn_type->param_types, argument_types);
    // TODO force the return types to be varying if the callee is not uniform
    return fn_type->return_types;
}

const Type* check_type_leaf_call(IrArena* arena, LeafCall call) {
    Nodes args = call.args;
    for (size_t i = 0; i < args.count; i++) {
        const Node* argument = args.nodes[i];
        assert(is_value(argument));
    }
    Nodes argument_types = get_values_types(arena, args);
    assert(is_function(call.callee));
    assert(call.callee->type->tag == FnType_TAG);
    check_arguments_types_against_parameters_helper(call.callee->type->payload.fn_type.param_types, argument_types);
    return wrap_multiple_yield_types(arena, call.callee->payload.fun.return_types);
}

const Type* check_type_indirect_call(IrArena* arena, IndirectCall call) {
    Nodes args = call.args;
    for (size_t i = 0; i < args.count; i++) {
        const Node* argument = args.nodes[i];
        assert(is_value(argument));
    }
    Nodes argument_types = get_values_types(arena, args);
    return wrap_multiple_yield_types(arena, check_value_call(call.callee, argument_types));
}

const Type* check_type_if_instr(IrArena* arena, If if_instr) {
    if (get_unqualified_type(if_instr.condition->type) != bool_type(arena))
        error("condition of an if should be bool");
    // TODO check the contained Merge instrs
    if (if_instr.yield_types.count > 0)
        assert(if_instr.if_false);

    return wrap_multiple_yield_types(arena, if_instr.yield_types);
}

const Type* check_type_loop_instr(IrArena* arena, Loop loop_instr) {
    // TODO check param against initial_args
    // TODO check the contained Merge instrs
    return wrap_multiple_yield_types(arena, loop_instr.yield_types);
}

const Type* check_type_match_instr(IrArena* arena, Match match_instr) {
    // TODO check param against initial_args
    // TODO check the contained Merge instrs
    return wrap_multiple_yield_types(arena, match_instr.yield_types);
}

const Type* check_type_control(IrArena* arena, Control control) {
    // TODO check it then !
    assert(is_anonymous_lambda(control.inside));
    const Node* join_point = first(control.inside->payload.anon_lam.params);

    const Type* join_point_type = join_point->type;
    bool join_point_uniform = deconstruct_qualified_type(&join_point_type);
    assert(join_point_uniform && join_point_type->tag == JoinPointType_TAG);

    Nodes join_point_yield_types = join_point_type->payload.join_point_type.yield_types;
    assert(join_point_yield_types.count == control.yield_types.count);
    for (size_t i = 0; i < control.yield_types.count; i++) {
        assert(is_data_type(control.yield_types.nodes[i]));
        assert(is_subtype(control.yield_types.nodes[i], join_point_yield_types.nodes[i]));
    }

    return wrap_multiple_yield_types(arena, add_qualifiers(arena, join_point_yield_types, !arena->config.is_simt /* non-simt worlds might have spurious control statements, but they ban varying types */));
}

const Type* check_type_let(IrArena* arena, Let let) {
    assert(is_anonymous_lambda(let.tail));
    Nodes produced_types = unwrap_multiple_yield_types(arena, let.instruction->type);
    Nodes param_types = get_variables_types(arena, let.tail->payload.anon_lam.params);

    check_arguments_types_against_parameters_helper(param_types, produced_types);
    return noret_type(arena);
}

const Type* check_type_tail_call(IrArena* arena, TailCall tail_call) {
    Nodes args = tail_call.args;
    for (size_t i = 0; i < args.count; i++) {
        const Node* argument = args.nodes[i];
        assert(is_value(argument));
    }
    assert(check_value_call(tail_call.target, get_values_types(arena, tail_call.args)).count == 0);
    return noret_type(arena);
}

static void check_basic_block_call(const Node* block, Nodes argument_types) {
    assert(is_basic_block(block));
    assert(block->type->tag == BBType_TAG);
    BBType bb_type = block->type->payload.bb_type;
    check_arguments_types_against_parameters_helper(bb_type.param_types, argument_types);
}

const Type* check_type_jump(IrArena* arena, Jump jump) {
    for (size_t i = 0; i < jump.args.count; i++) {
        const Node* argument = jump.args.nodes[i];
        assert(is_value(argument));
    }

    check_basic_block_call(jump.target, get_values_types(arena, jump.args));
    return noret_type(arena);
}

const Type* check_type_branch(IrArena* arena, Branch branch) {
    for (size_t i = 0; i < branch.args.count; i++) {
        const Node* argument = branch.args.nodes[i];
        assert(is_value(argument));
    }

    const Type* condition_type = branch.branch_condition->type;
    bool uniform = deconstruct_qualified_type(&condition_type);
    assert(bool_type(arena) == condition_type);

    const Node* branches[2] = { branch.true_target, branch.false_target };
    for (size_t i = 0; i < 2; i++)
        check_basic_block_call(branches[i], get_values_types(arena, branch.args));

    return noret_type(arena);
}

const Type* check_type_br_switch(IrArena* arena, Switch br_switch) {
    for (size_t i = 0; i < br_switch.args.count; i++) {
        const Node* argument = br_switch.args.nodes[i];
        assert(is_value(argument));
    }

    error("TODO")

    return noret_type(arena);
}

const Type* check_type_join(IrArena* arena, Join join) {
    for (size_t i = 0; i < join.args.count; i++) {
        const Node* argument = join.args.nodes[i];
        assert(is_value(argument));
    }

    const Type* join_target_type = join.join_point->type;

    bool join_target_uniform = deconstruct_qualified_type(&join_target_type);
    assert(join_target_uniform);
    assert(join_target_type->tag == JoinPointType_TAG);

    Nodes join_point_param_types = join_target_type->payload.join_point_type.yield_types;
    join_point_param_types = add_qualifiers(arena, join_point_param_types, !arena->config.is_simt);

    check_arguments_types_against_parameters_helper(join_point_param_types, get_values_types(arena, join.args));

    return noret_type(arena);
}

const Type* check_type_unreachable(IrArena* arena) {
    return noret_type(arena);
}

const Type* check_type_merge_selection(IrArena* arena, MergeSelection mc) {
    // TODO check it
    return noret_type(arena);
}

const Type* check_type_merge_continue(IrArena* arena, MergeContinue mc) {
    // TODO check it
    return noret_type(arena);
}

const Type* check_type_merge_break(IrArena* arena, MergeBreak mc) {
    // TODO check it
    return noret_type(arena);
}

const Type* check_type_fn_ret(IrArena* arena, Return ret) {
    // TODO check it then !
    return noret_type(arena);
}

const Type* check_type_fun(IrArena* arena, Function fn) {
    for (size_t i = 0; i < fn.return_types.count; i++) {
        assert(is_value_type(fn.return_types.nodes[i]));
    }
    return fn_type(arena, (FnType) { .param_types = get_variables_types(arena, (&fn)->params), .return_types = (&fn)->return_types });
}

const Type* check_type_basic_block(IrArena* arena, BasicBlock bb) {
    return bb_type(arena, (BBType) { .param_types = get_variables_types(arena, (&bb)->params) });
}

const Type* check_type_anon_lam(IrArena* arena, AnonLambda lam) {
    return lam_type(arena, (LamType) { .param_types = get_variables_types(arena, (&lam)->params) });
}

const Type* check_type_global_variable(IrArena* arena, GlobalVariable global_variable) {
    assert(is_type(global_variable.type));
    return ptr_type(arena, (PtrType) {
        .pointed_type = global_variable.type,
        .address_space = global_variable.address_space
    });
}

const Type* check_type_constant(IrArena* arena, Constant cnst) {
    assert(is_data_type(cnst.type_hint));
    return cnst.type_hint;
}

#pragma GCC diagnostic pop
