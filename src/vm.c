#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm.h"
#include "rt.h"

#define MAX_CALL_DEPTH 1000

typedef struct {
    const IrFunction *fn;
    int pc;
    Value *locals;      /* fn->local_count slots, VAL_UNSET until assigned */
    int stack_base;     /* where this frame's operand stack starts in the shared stack */
} Frame;

typedef struct {
    Value *stack;
    int sp;
    int stack_capacity;
    Frame frames[MAX_CALL_DEPTH + 1];
    int frame_count;
    Value *globals;
} Vm;

static void *checked_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    return p;
}

static void push(Vm *vm, Value v) {
    if (vm->sp == vm->stack_capacity) {
        vm->stack_capacity *= 2;
        vm->stack = realloc(vm->stack, sizeof(Value) * (size_t)vm->stack_capacity);
        if (!vm->stack) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
    }
    vm->stack[vm->sp++] = v;
}

static Value pop(Vm *vm) {
    return vm->stack[--vm->sp];
}

static void enter(Vm *vm, const IrFunction *fn, Token call_site) {
    if (vm->frame_count > MAX_CALL_DEPTH) {
        char message[256];
        snprintf(message, sizeof(message),
                 "Too many nested function calls (limit %d). Does '%.*s' call itself without ever stopping?",
                 MAX_CALL_DEPTH, fn->name.length, fn->name.start);
        rt_error(call_site, message);
    }
    Frame *frame = &vm->frames[vm->frame_count++];
    frame->fn = fn;
    frame->pc = 0;
    frame->locals = checked_alloc(sizeof(Value) * (size_t)fn->local_count);
    for (int i = 0; i < fn->local_count; i++) frame->locals[i] = rt_unset();
    /* Arguments are the top `param_count` stack values; move them into the first slots. */
    for (int i = fn->param_count - 1; i >= 0; i--) frame->locals[i] = pop(vm);
    frame->stack_base = vm->sp;
}

static void leave(Vm *vm) {
    Frame *frame = &vm->frames[--vm->frame_count];
    for (int i = 0; i < frame->fn->local_count; i++) value_release(frame->locals[i]);
    free(frame->locals);
}

static const AstNode *caller_class(const IrProgram *program, long index) {
    return index < 0 ? NULL : program->classes[index].decl;
}

void vm_run(const IrProgram *program) {
    Vm *vm = checked_alloc(sizeof(Vm));
    vm->stack_capacity = 256;
    vm->stack = checked_alloc(sizeof(Value) * (size_t)vm->stack_capacity);
    vm->globals = checked_alloc(sizeof(Value) * (size_t)(program->global_count + 1));
    for (int i = 0; i < program->global_count; i++) vm->globals[i] = rt_unset();

    enter(vm, &program->functions[0], (Token){ 0 });

    while (vm->frame_count > 0) {
        Frame *frame = &vm->frames[vm->frame_count - 1];
        const IrInstr *in = &frame->fn->code[frame->pc++];

        switch (in->op) {
            case IR_CONST_INT: push(vm, value_int(in->a)); break;
            case IR_CONST_STR:
                push(vm, value_new_string(program->strings[in->a], program->string_lengths[in->a]));
                break;
            case IR_POP: value_release(pop(vm)); break;
            case IR_LOAD_LOCAL: push(vm, value_retain(frame->locals[in->a])); break;
            case IR_STORE_LOCAL: {
                Value v = pop(vm);
                value_release(frame->locals[in->a]);
                frame->locals[in->a] = v;
                break;
            }
            case IR_LOAD_GLOBAL: push(vm, value_retain(vm->globals[in->a])); break;
            case IR_STORE_GLOBAL: {
                Value v = pop(vm);
                value_release(vm->globals[in->a]);
                vm->globals[in->a] = v;
                break;
            }
            case IR_LOAD_NAME: {
                if (in->a >= 0 && !rt_is_unset(frame->locals[in->a])) {
                    push(vm, value_retain(frame->locals[in->a]));
                } else if (!rt_is_unset(vm->globals[in->b])) {
                    push(vm, value_retain(vm->globals[in->b]));
                } else {
                    rt_undefined_variable(in->token.start, in->token.length, in->token);
                }
                break;
            }
            case IR_BINOP: {
                Value r = pop(vm);
                Value l = pop(vm);
                push(vm, rt_binop((TokenType)in->a, l, r, in->token));
                break;
            }
            case IR_NEG: push(vm, rt_neg(pop(vm), in->token)); break;
            case IR_POS: push(vm, rt_pos(pop(vm), in->token)); break;
            case IR_NOT: push(vm, rt_not(pop(vm), in->token)); break;
            case IR_TRUTH: push(vm, rt_truth(pop(vm), in->token)); break;
            case IR_JUMP: frame->pc = frame->fn->label_pos[in->a]; break;
            case IR_JUMP_IF_FALSE:
                if (!rt_condition(pop(vm), in->token)) frame->pc = frame->fn->label_pos[in->a];
                break;
            case IR_LABEL: break;
            case IR_NEW_ARRAY: {
                int n = (int)in->a;
                vm->sp -= n;
                push(vm, rt_new_array(&vm->stack[vm->sp], n));
                break;
            }
            case IR_INDEX: {
                Value index = pop(vm);
                Value target = pop(vm);
                push(vm, rt_index(target, index, in->token));
                break;
            }
            case IR_SET_INDEX: {
                Value value = pop(vm);
                Value index = pop(vm);
                Value target = pop(vm);
                rt_set_index(target, index, value, in->token);
                break;
            }
            case IR_LEN: {
                Value v = pop(vm);
                push(vm, rt_length_of(v));
                value_release(v);
                break;
            }
            case IR_CHECK_ITER: rt_check_iterable(vm->stack[vm->sp - 1], in->token); break;
            case IR_CHECK_NUM: rt_number(vm->stack[vm->sp - 1], IR_WHAT[in->a], in->token); break;
            case IR_CALL: enter(vm, &program->functions[in->a], in->token); break;
            case IR_BUILTIN: {
                int argc = (int)in->b;
                vm->sp -= argc;
                Value result = rt_builtin((int)in->a, &vm->stack[vm->sp], in->token);
                push(vm, result);
                break;
            }
            case IR_RETURN: {
                Value result = pop(vm);
                leave(vm);
                if (vm->frame_count > 0) push(vm, result);
                else value_release(result);
                break;
            }
            case IR_PRINT: {
                Value v = pop(vm);
                rt_print(v);
                value_release(v);
                break;
            }
            case IR_NEW_OBJECT: push(vm, value_new_object(program->classes[in->a].decl)); break;
            case IR_INIT_FIELD: {
                Value v = pop(vm);
                Object *object = vm->stack[vm->sp - 1].as.object;
                value_release(object->fields[in->a]);
                object->fields[in->a] = v;
                break;
            }
            case IR_DUP: push(vm, value_retain(vm->stack[vm->sp - 1])); break;
            case IR_GET_FIELD: {
                Value object = pop(vm);
                push(vm, rt_get_field(object, in->token.start, in->token.length, caller_class(program, in->a),
                                      in->token));
                break;
            }
            case IR_SET_FIELD: {
                Value value = pop(vm);
                Value object = pop(vm);
                rt_set_field(object, in->token.start, in->token.length, value, caller_class(program, in->a),
                             in->token);
                break;
            }
            case IR_CALL_METHOD: {
                int argc = (int)in->b;
                const AstNode *klass;
                int method = rt_find_method(vm->stack[vm->sp - argc - 1], in->token.start, in->token.length, argc,
                                            caller_class(program, in->a), in->token, &klass);
                int fn = -1;
                for (int i = 0; i < program->class_count; i++) {
                    if (program->classes[i].decl == klass) fn = program->classes[i].method_fn[method];
                }
                enter(vm, &program->functions[fn], in->token);
                break;
            }
        }
    }

    for (int i = 0; i < program->global_count; i++) value_release(vm->globals[i]);
    free(vm->globals);
    free(vm->stack);
    free(vm);
}
