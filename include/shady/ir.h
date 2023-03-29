#ifndef SHADY_IR_H
#define SHADY_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct IrArena_ IrArena;
typedef struct Node_ Node;
typedef struct Node_ Type;
typedef unsigned int VarId;
typedef const char* String;

//////////////////////////////// Lists & Strings ////////////////////////////////

typedef struct Nodes_ {
    size_t count;
    const Node** nodes;
} Nodes;

typedef struct Strings_ {
    size_t count;
    String* strings;
} Strings;

Nodes     nodes(IrArena*, size_t count, const Node*[]);
Strings strings(IrArena*, size_t count, const char*[]);

#define empty(arena) nodes(arena, 0, NULL)
Nodes singleton(const Node* type);
#define mk_nodes(arena, ...) nodes(arena, sizeof((const Node*[]) { __VA_ARGS__ }) / sizeof(const Node*), (const Node*[]) { __VA_ARGS__ })

const Node* first(Nodes nodes);

Nodes append_nodes(IrArena*, Nodes, const Node*);
Nodes concat_nodes(IrArena*, Nodes, Nodes);

String string_sized(IrArena* arena, size_t size, const char* start);
String string(IrArena* arena, const char* start);
String format_string(IrArena* arena, const char* str, ...);
String unique_name(IrArena* arena, const char* start);
String name_type_safe(IrArena*, const Type*);

//////////////////////////////// IR Arena ////////////////////////////////

typedef struct {
    bool name_bound;
    bool check_types;
    bool allow_fold;
    bool is_simt;
    /// Selects which type the subgroup intrinsic primops use to manipulate masks
    enum {
        /// Uses the MaskType
        SubgroupMaskAbstract,
        /// Uses a 64-bit integer
        SubgroupMaskInt64
    } subgroup_mask_representation;

    struct {
        IntSizes ptr_size;
        /// The base type for emulated memory
        IntSizes word_size;
    } memory;
} ArenaConfig;

ArenaConfig default_arena_config();

IrArena* new_ir_arena(ArenaConfig);
void destroy_ir_arena(IrArena*);

//////////////////////////////// Modules ////////////////////////////////

typedef struct Module_ Module;

Module* new_module(IrArena*, String name);

IrArena* get_module_arena(const Module*);
String get_module_name(const Module*);
Nodes get_module_declarations(const Module*);

//////////////////////////////// Grammar ////////////////////////////////

// The language grammar is big enough that it deserve its own files

#include "primops.h"
#include "grammar.h"

//////////////////////////////// Getters ////////////////////////////////

/// Get the name out of a global variable, function or constant
String get_decl_name(const Node*);

const IntLiteral* resolve_to_literal(const Node*);

int64_t get_int_literal_value(const Node*, bool sign_extend);
const char* get_string_literal(IrArena*, const Node*);

static inline bool is_physical_as(AddressSpace as) { return as <= AsGlobalPhysical; }

/// Returns true if variables in that address space can contain different data for threads in the same subgroup
bool is_addr_space_uniform(IrArena*, AddressSpace);

const Node* lookup_annotation(const Node* decl, const char* name);
const Node* lookup_annotation_list(Nodes, const char* name);
const Node* get_annotation_value(const Node* annotation);
Nodes get_annotation_values(const Node* annotation);
/// Gets the string literal attached to an annotation, if present.
const char* get_annotation_string_payload(const Node* annotation);
bool lookup_annotation_with_string_payload(const Node* decl, const char* annotation_name, const char* expected_payload);
bool is_annotation(const Node* node);
String get_annotation_name(const Node* node);
Nodes filter_out_annotation(IrArena*, Nodes, const char* name);

String get_abstraction_name(const Node* abs);
const Node* get_abstraction_body(const Node*);
Nodes get_abstraction_params(const Node*);
Module* get_abstraction_module(const Node*);

const Node* get_let_instruction(const Node* let);
const Node* get_let_tail(const Node* let);

//////////////////////////////// Constructors ////////////////////////////////

// autogenerated node ctors
#define NODE_CTOR_DECL_1(struct_name, short_name) const Node* short_name(IrArena*, struct_name);
#define NODE_CTOR_DECL_0(struct_name, short_name) const Node* short_name(IrArena*);
#define NODE_CTOR_1(has_payload, struct_name, short_name) NODE_CTOR_DECL_##has_payload(struct_name, short_name)
#define NODE_CTOR_0(has_payload, struct_name, short_name)
#define NODE_CTOR(autogen_ctor, _, has_payload, struct_name, short_name) NODE_CTOR_##autogen_ctor(has_payload, struct_name, short_name)
NODES(NODE_CTOR)
#undef NODE_CTOR
#undef NODE_CTOR_0
#undef NODE_CTOR_1
#undef NODE_CTOR_DECL_0
#undef NODE_CTOR_DECL_1

const Node* var(IrArena* arena, const Type* type, const char* name);

const Node* tuple(IrArena* arena, Nodes contents);
const Node* composite(IrArena* arena, const Type*, Nodes contents);

Node* function    (Module*, Nodes params, const char* name, Nodes annotations, Nodes return_types);
Node* constant    (Module*, Nodes annotations, const Type*, const char* name);
Node* global_var  (Module*, Nodes annotations, const Type*, String, AddressSpace);
Type* nominal_type(Module*, Nodes annotations, String name);

Node* basic_block (IrArena*, Node* function, Nodes params, const char* name);
const Node* lambda(Module*, Nodes params, const Node* body);

const Node* let(IrArena* arena, const Node* instruction, const Node* tail);
const Node* let_mut(IrArena* arena, const Node* instruction, const Node* tail);

typedef struct BodyBuilder_ BodyBuilder;
BodyBuilder* begin_body(Module*);

/// Appends an instruction to the builder, may apply optimisations.
/// If the arena is typed, returns a list of variables bound to the values yielded by that instruction
Nodes bind_instruction(BodyBuilder*, const Node* instruction);
Nodes bind_instruction_named(BodyBuilder*, const Node* instruction, String const output_names[]);

/// Like append instruction, but you explicitly give it information about any yielded values
/// ! In untyped arenas, you need to call this because we can't guess how many things are returned without typing info !
Nodes bind_instruction_extra_mutable(BodyBuilder*, const Node* initial_value, size_t outputs_count, Nodes* provided_types, String const output_names[]);
Nodes bind_instruction_extra(BodyBuilder*, const Node* initial_value, size_t outputs_count, Nodes* provided_types, String const output_names[]);

void bind_variables(BodyBuilder*, Nodes vars, Nodes values);

const Node* finish_body(BodyBuilder*, const Node* terminator);
void cancel_body(BodyBuilder*);
const Node* yield_values_and_wrap_in_control(BodyBuilder*, Nodes);

const Type* int8_type(IrArena* arena);
const Type* int16_type(IrArena* arena);
const Type* int32_type(IrArena* arena);
const Type* int64_type(IrArena* arena);

const Type* uint8_type(IrArena* arena);
const Type* uint16_type(IrArena* arena);
const Type* uint32_type(IrArena* arena);
const Type* uint64_type(IrArena* arena);

const Type* int8_literal(IrArena* arena,  int8_t i);
const Type* int16_literal(IrArena* arena, int16_t i);
const Type* int32_literal(IrArena* arena, int32_t i);
const Type* int64_literal(IrArena* arena, int64_t i);

const Type* uint8_literal(IrArena* arena,  uint8_t i);
const Type* uint16_literal(IrArena* arena, uint16_t i);
const Type* uint32_literal(IrArena* arena, uint32_t i);
const Type* uint64_literal(IrArena* arena, uint64_t i);

const Type* fp16_type(IrArena* arena);
const Type* fp32_type(IrArena* arena);
const Type* fp64_type(IrArena* arena);

/// Turns a value into an 'instruction' (the enclosing let will be folded away later)
/// Useful for local rewrites
const Node* quote(IrArena* arena, Nodes values);
/// Overload of quote for single values
const Node* quote_single(IrArena* arena, const Node* value);
/// Overload of quote for no values
const Node* unit(IrArena* arena);
const Node* unit_type(IrArena* arena);

//////////////////////////////// Compilation ////////////////////////////////

typedef struct CompilerConfig_ {
    bool allow_frontend_syntax;
    bool dynamic_scheduling;
    uint32_t per_thread_stack_size;
    uint32_t per_subgroup_stack_size;

    uint32_t subgroup_size;

    struct {
        uint8_t major;
        uint8_t minor;
    } target_spirv_version;

    struct {
        bool emulate_subgroup_ops;
        bool emulate_subgroup_ops_extended_types;
        bool simt_to_explicit_simd;
        bool int64;
    } lower;

    struct {
        bool spv_shuffle_instead_of_broadcast_first;
    } hacks;

    struct {
        bool memory_accesses;
        bool stack_accesses;
        bool god_function;
        bool stack_size;
    } printf_trace;

    struct {
        int max_top_iterations;
    } shader_diagnostics;

    struct {
        bool skip_generated, skip_builtin;
    } logging;
} CompilerConfig;

CompilerConfig default_compiler_config();

typedef enum CompilationResult_ {
    CompilationNoError
} CompilationResult;

CompilationResult parse_files(CompilerConfig*, size_t num_files, const char** file_names, const char** files_contents, Module* module);
CompilationResult run_compiler_passes(CompilerConfig* config, Module** mod);

//////////////////////////////// Emission ////////////////////////////////

void emit_spirv(CompilerConfig* config, Module*, size_t* output_size, char** output);

typedef enum {
    C,
    GLSL,
    ISPC
} CDialect;

typedef struct {
    CompilerConfig* config;
    CDialect dialect;
    bool explicitly_sized_types;
    bool allow_compound_literals;
} CEmitterConfig;

void emit_c(CEmitterConfig config, Module*, size_t* output_size, char** output);

void dump_cfg(FILE* file, Module*);
void dump_module(Module*);
void print_module_into_str(Module*, char** str_ptr, size_t*);
void dump_node(const Node* node);

#endif
