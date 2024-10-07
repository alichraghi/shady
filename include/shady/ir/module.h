#ifndef SHADY_IR_MODULE_H
#define SHADY_IR_MODULE_H

#include "shady/ir/base.h"

Module* shd_new_module(IrArena* arena, String name);

IrArena* shd_module_get_arena(const Module* m);
String shd_module_get_name(const Module* m);
Nodes shd_module_get_declarations(const Module* m);
Node* shd_module_get_declaration(const Module* m, String name);

void link_module(Module* dst, Module* src);

#endif
