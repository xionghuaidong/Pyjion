/*
* The MIT License (MIT)
*
* Copyright (c) Microsoft Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
*/

#include "absint.h"
#include "taggedptr.h"
#include <opcode.h>
#include <deque>
#include <unordered_map>

#define NUM_ARGS(n) ((n)&0xFF)
#define NUM_KW_ARGS(n) (((n)>>8) & 0xff)

AbstractInterpreter::AbstractInterpreter(PyCodeObject *code, IPythonCompiler* comp) : m_code(code), m_comp(comp) {
    m_byteCode = (unsigned char *)((PyBytesObject*)code->co_code)->ob_sval;
    m_size = PyBytes_Size(code->co_code);
    m_returnValue = &Undefined;
	if (comp != nullptr) {
		m_retLabel = comp->emit_define_label();
		m_retValue = comp->emit_define_local();
	}
}

AbstractInterpreter::~AbstractInterpreter() {
    // clean up any dynamically allocated objects...
    for (auto source : m_sources) {
        delete source;
    }
    for (auto absValue : m_values) {
        delete absValue;
    }
}

#define NEXTARG() *(unsigned short*)&m_byteCode[curByte + 1]; curByte+= 2

void AbstractInterpreter::preprocess() {
	for (int i = 0; i < m_code->co_argcount; i++) {
		// all parameters are initially definitely assigned
		m_assignmentState[i] = true;
	}

	int oparg;
    vector<bool> ehKind;
    vector<AbsIntBlockInfo> blockStarts;
    for (size_t curByte = 0; curByte < m_size; curByte++) {
        auto opcodeIndex = curByte;
        auto byte = m_byteCode[curByte];
        if (HAS_ARG(byte)) {
            oparg = NEXTARG();
        }

        switch (byte) {
			case UNPACK_EX:
				if (m_comp != nullptr) {
					m_sequenceLocals[curByte] = m_comp->emit_allocate_stack_array(((oparg & 0xFF) + (oparg >> 8)) * sizeof(void*));
				}
				break;
			case UNPACK_SEQUENCE:
				// we need a buffer for the slow case, but we need 
				// to avoid allocating it in loops.
				if (m_comp != nullptr) {
					m_sequenceLocals[curByte] = m_comp->emit_allocate_stack_array(oparg * sizeof(void*));
				}
				break;
			case DELETE_FAST:
				if (oparg < m_code->co_argcount) {
					// this local is deleted, so we need to check for assignment
					m_assignmentState[oparg] = false;
				}
				break;
			case SETUP_WITH:
				// not supported...
				return;
            case SETUP_LOOP:
                blockStarts.push_back(AbsIntBlockInfo(opcodeIndex, oparg + curByte + 1, true));
                break;
            case POP_BLOCK:
                {
                    auto blockStart = blockStarts.back();
                    blockStarts.pop_back();
                    m_blockStarts[opcodeIndex] = blockStart.BlockStart;
                    break;
                }
            case SETUP_EXCEPT:
                blockStarts.push_back(AbsIntBlockInfo(opcodeIndex, oparg + curByte + 1, false));
                ehKind.push_back(false);
                break;
            case SETUP_FINALLY:
                blockStarts.push_back(AbsIntBlockInfo(opcodeIndex, oparg + curByte + 1, false));
                ehKind.push_back(true);
                break;
            case END_FINALLY:
                m_endFinallyIsFinally[opcodeIndex] = ehKind.back();
                ehKind.pop_back();
                break;
            case BREAK_LOOP:
                for (auto iter = blockStarts.rbegin(); iter != blockStarts.rend(); ++iter) {
                    if (iter->IsLoop) {
                        m_breakTo[opcodeIndex] = *iter;
                        break;
                    }
                }
                break;
        }
    }

}

void AbstractInterpreter::init_starting_state() {
    InterpreterState lastState = InterpreterState(m_code->co_nlocals);

    int localIndex = 0;
    for (int i = 0; i < m_code->co_argcount + m_code->co_kwonlyargcount; i++) {
        // all parameters are initially definitely assigned
        // TODO: Populate this with type information from profiling...
        lastState.replace_local(localIndex++, AbstractLocalInfo(&Any));
    }

    if (m_code->co_flags & CO_VARARGS) {
        lastState.replace_local(localIndex++, AbstractLocalInfo(&Tuple));
    }
    if (m_code->co_flags & CO_VARKEYWORDS) {
        lastState.replace_local(localIndex++, AbstractLocalInfo(&Dict));
    }

    for (; localIndex < m_code->co_nlocals; localIndex++) {
        // All locals are initially undefined
        lastState.replace_local(localIndex, AbstractLocalInfo(&Undefined, true));
    }

    update_start_state(lastState, 0);
}

bool AbstractInterpreter::interpret() {
    preprocess();

    init_starting_state();
    //dump();


    // walk all the blocks in the code one by one, analyzing them, and enqueing any
    // new blocks that we encounter from branches.
    deque<size_t> queue;
    queue.push_back(0);
    do {
        int oparg;
        auto cur = queue.front();
        queue.pop_front();
        for (size_t curByte = cur; curByte < m_size; curByte++) {
            // get our starting state when we entered this opcode
            InterpreterState lastState = m_startStates.find(curByte)->second;

            auto opcodeIndex = curByte;

            auto opcode = m_byteCode[curByte];
            if (HAS_ARG(opcode)) {
                oparg = NEXTARG();
            }

        processOpCode:
            switch (opcode) {
                case EXTENDED_ARG:
                    {
                        opcode = m_byteCode[++curByte];
                        int bottomArg = NEXTARG();
                        oparg = (oparg << 16) | bottomArg;
                        goto processOpCode;
                    }
                case NOP: break;
                case ROT_TWO:
                    {
                        auto tmp = lastState[lastState.stack_size() - 1];
                        lastState[lastState.stack_size() - 1] = lastState[lastState.stack_size() - 2];
                        lastState[lastState.stack_size() - 2] = tmp;
                        break;
                    }
                case ROT_THREE:
                    {
                        auto top = lastState.pop_no_escape();
                        auto second = lastState.pop_no_escape();
                        auto third = lastState.pop_no_escape();

                        lastState.push(top);
                        lastState.push(third);
                        lastState.push(second);
                        break;
                    }
                case POP_TOP:
                    lastState.pop_no_escape();
                    break;
                case DUP_TOP:
                    lastState.push(lastState[lastState.stack_size() - 1]);
                    break;
                case DUP_TOP_TWO:
                    {
                        auto top = lastState[lastState.stack_size() - 1];
                        auto second = lastState[lastState.stack_size() - 2];
                        lastState.push(second);
                        lastState.push(top);
                        break;
                    }
                case LOAD_CONST:
                    {
                        auto constSource = add_const_source(opcodeIndex, oparg);

                        lastState.push(
                            AbstractValueWithSources(
                                to_abstract(PyTuple_GetItem(m_code->co_consts, oparg)),
                                constSource
                                )
                            );
                        break;
                    }
                case LOAD_FAST:
                    {
                        auto localSource = add_local_source(opcodeIndex, oparg);
                        auto local = lastState.get_local(oparg);

                        local.ValueInfo.Sources = AbstractSource::combine(localSource, local.ValueInfo.Sources);

                        lastState.push(local.ValueInfo);
                        break;
                    }
                case STORE_FAST:
                    {
                        auto valueInfo = lastState.pop_no_escape();
                        m_opcodeSources[opcodeIndex] = valueInfo.Sources;
                        lastState.replace_local(oparg, AbstractLocalInfo(valueInfo, false));
                    }
                    break;
                case DELETE_FAST:
                    // We need to box any previous stores so we can delete them...  Otherwise
                    // we won't know if we should raise an unbound local error
                    lastState.get_local(oparg).ValueInfo.escapes();
                    lastState.replace_local(oparg, AbstractLocalInfo(&Undefined, true));
                    break;
                case BINARY_SUBSCR:
                case BINARY_TRUE_DIVIDE:
                case BINARY_FLOOR_DIVIDE:
                case BINARY_POWER:
                case BINARY_MODULO:
                case BINARY_MATRIX_MULTIPLY:
                case BINARY_LSHIFT:
                case BINARY_RSHIFT:
                case BINARY_AND:
                case BINARY_XOR:
                case BINARY_OR:
                case BINARY_MULTIPLY:
                case BINARY_SUBTRACT:
                case BINARY_ADD:
                case INPLACE_POWER:
                case INPLACE_MULTIPLY:
                case INPLACE_MATRIX_MULTIPLY:
                case INPLACE_TRUE_DIVIDE:
                case INPLACE_FLOOR_DIVIDE:
                case INPLACE_MODULO:
                case INPLACE_ADD:
                case INPLACE_SUBTRACT:
                case INPLACE_LSHIFT:
                case INPLACE_RSHIFT:
                case INPLACE_AND:
                case INPLACE_XOR:
                case INPLACE_OR:
                    {
                        auto two = lastState.pop_no_escape();
                        auto one = lastState.pop_no_escape();
                        auto binaryRes = one.Value->binary(one.Sources, opcode, two);

                        // create an intermediate source which will propagate changes up...
                        auto sources = add_intermediate_source(opcodeIndex);
                        AbstractSource::combine(
                            AbstractSource::combine(one.Sources, two.Sources),
                            sources
                            );

                        lastState.push(AbstractValueWithSources(binaryRes, sources));
                    }
                    break;
                case POP_JUMP_IF_FALSE:
                    {
                        auto value = lastState.pop_no_escape();

                        // merge our current state into the branched to location...
                        if (update_start_state(lastState, oparg)) {
                            queue.push_back(oparg);
                        }

						value.Value->truth(value.Sources);
                        if (value.Value->is_always_false()) {
                            // We're always jumping, we don't need to process the following opcodes...
                            goto next;
                        }

                        // we'll continue processing after the jump with our new state...
                        break;
                    }
                case POP_JUMP_IF_TRUE:
                    {
                        auto value = lastState.pop_no_escape();

                        // merge our current state into the branched to location...
                        if (update_start_state(lastState, oparg)) {
                            queue.push_back(oparg);
                        }

						value.Value->truth(value.Sources);
                        if (value.Value->is_always_true()) {
                            // We're always jumping, we don't need to process the following opcodes...
                            goto next;
                        }

                        // we'll continue processing after the jump with our new state...
                        break;
                    }
                case JUMP_IF_TRUE_OR_POP:
                    {
                        auto curState = lastState;
                        if (update_start_state(lastState, oparg)) {
                            queue.push_back(oparg);
                        }
                        auto value = lastState.pop_no_escape();
						value.Value->truth(value.Sources);
                        if (value.Value->is_always_true()) {
                            // we always jump, no need to analyze the following instructions...
                            goto next;
                        }
                    }
                    break;
                case JUMP_IF_FALSE_OR_POP:
                    {
                        auto curState = lastState;
                        if (update_start_state(lastState, oparg)) {
                            queue.push_back(oparg);
                        }
                        auto value = lastState.pop_no_escape();
						value.Value->truth(value.Sources);
                        if (value.Value->is_always_false()) {
                            // we always jump, no need to analyze the following instructions...
                            goto next;
                        }
                    }
                    break;
                case JUMP_ABSOLUTE:
                    if (update_start_state(lastState, oparg)) {
                        queue.push_back(oparg);
                    }
                    // Done processing this basic block, we'll need to see a branch
                    // to the following opcodes before we'll process them.
                    goto next;
                case JUMP_FORWARD:
                    if (update_start_state(lastState, (size_t)oparg + curByte + 1)) {
                        queue.push_back((size_t)oparg + curByte + 1);
                    }
                    // Done processing this basic block, we'll need to see a branch
                    // to the following opcodes before we'll process them.
                    goto next;
                case RETURN_VALUE:
                    {
                        // We don't treat returning as escaping as it would just result in a single
                        // boxing over the lifetime of the function.
                        auto retValue = lastState.pop_no_escape();

                        // We add a a source here just so we can know if the return value
                        // was unboxed or not, this is a little ugly...
                        auto retSource = add_const_source(opcodeIndex, 0);

                        if (retValue.needs_boxing()) {
                            retSource->escapes();
                        }

                        m_returnValue = m_returnValue->merge_with(retValue.Value);
                    }
                    goto next;
                case LOAD_NAME:
                    // Used to load __name__ for a class def
                    lastState.push(&Any);
                    break;
                case STORE_NAME:
                    // Stores __module__, __doc__, __qualname__, as well as class/function defs sometimes
                    lastState.pop();
                    break;
                case DELETE_NAME:
                    break;
                case LOAD_CLASSDEREF:
                    lastState.push(&Any);
                    break;
                case LOAD_GLOBAL:
                    // TODO: Look in globals, then builtins, and see if we can resolve
                    // this to anything concrete.
                    lastState.push(&Any);
                    break;
                case STORE_GLOBAL:
                    lastState.pop();
                    break;
                case LOAD_ATTR:
                    // TODO: Add support for resolving known members of known types
                    lastState.pop();
                    lastState.push(&Any);
                    break;
                case STORE_ATTR:
                    lastState.pop();
                    lastState.pop();
                    break;
                case DELETE_ATTR:
                    lastState.pop();
                    break;
                case BUILD_LIST:
                    for (int i = 0; i < oparg; i++) {
                        lastState.pop();
                    }
                    lastState.push(&List);
                    break;
                case BUILD_TUPLE:
                    {
                        vector<AbstractValueWithSources> sources;
                        for (int i = 0; i < oparg; i++) {
                            lastState.pop();
                            //sources.push_back(lastState.pop_no_escape());
                        }
                        // TODO: Currently we don't mark the consuming portions of the tuple
                        // as escaping until the tuple escapes.  Is that good enough?  That
                        // means that if we have a non-escaping tuple we need to optimize
                        // it away too, otherwise our assumptions about what's in the tuple are
                        // broken.
                        // This optimization is disabled until the above comment sorted out.
                        //auto tuple = new TupleSource(sources);
                        //m_sources.push_back(tuple);
                        //lastState.push(AbstractValueWithSources(&Tuple, tuple));
                        lastState.push(&Tuple);
                        break;
                    }
                case BUILD_MAP:
                    for (int i = 0; i < oparg; i++) {
                        lastState.pop();
                        lastState.pop();
                    }
                    lastState.push(&Dict);
                    break;
                case COMPARE_OP:
                    switch (oparg) {
                        case PyCmp_IS:
                        case PyCmp_IS_NOT:
                        case PyCmp_IN:
                        case PyCmp_NOT_IN:
                            lastState.pop();
                            lastState.pop();
                            lastState.push(&Bool);
                            break;
                        case PyCmp_EXC_MATCH:
                            // TODO: Produces an error or a bool, but no way to represent that so we're conservative
                            lastState.pop();
                            lastState.pop();
                            lastState.push(&Any);
                            break;
                        default:
                            {
                                auto two = lastState.pop_no_escape();
                                auto one = lastState.pop_no_escape();
                                auto binaryRes = one.Value->compare(one.Sources, oparg, two);

                                auto sources = add_intermediate_source(opcodeIndex);
                                AbstractSource::combine(
                                    AbstractSource::combine(one.Sources, two.Sources),
                                    sources
                                    );
                                lastState.push(AbstractValueWithSources(binaryRes, sources));
                                break;
                            }
                    }
                    break;
                case IMPORT_NAME:
                    lastState.pop();
                    lastState.pop();
                    lastState.push(&Any);
                    break;
                case IMPORT_FROM:
                    // leave the module on the stack, and push the new value.
                    lastState.push(&Any);
                    break;
                case LOAD_CLOSURE:
                    lastState.push(&Any);
                    break;
                case CALL_FUNCTION:
                    {
                        // TODO: Known functions could return known return types.
                        int argCnt = oparg & 0xff;
                        int kwArgCnt = (oparg >> 8) & 0xff;

                        for (int i = 0; i < argCnt; i++) {
                            lastState.pop();
                        }
                        for (int i = 0; i < kwArgCnt; i++) {
                            lastState.pop();
                            lastState.pop();
                        }

                        // TODO: If the function was a known quantity we could
                        // abstract interpret it here with the known args, and
                        // potentially consider inlining it.

                        // pop the function...
                        lastState.pop();

                        lastState.push(&Any);
                        break;
                    }

                case CALL_FUNCTION_VAR:
                case CALL_FUNCTION_KW:
                case CALL_FUNCTION_VAR_KW:
                    {
                        int na = NUM_ARGS(oparg);
                        int nk = NUM_KW_ARGS(oparg >> 8);
                        int flags = (opcode - CALL_FUNCTION) & 3;
                        int n = na + 2 * nk;

#define CALL_FLAG_VAR 1
#define CALL_FLAG_KW 2
                        if (flags & CALL_FLAG_KW) {
                            lastState.pop();
                        }
                        for (int i = 0; i < nk; i++) {
                            lastState.pop();
                            lastState.pop();
                        }
                        if (flags & CALL_FLAG_VAR) {
                            lastState.pop();
                        }
                        for (int i = 0; i < na; i++) {
                            lastState.pop();
                        }

                        // pop the function
                        lastState.pop();

                        lastState.push(&Any);
                        break;
                    }
                case MAKE_CLOSURE:
                case MAKE_FUNCTION:
                    {
                        int posdefaults = oparg & 0xff;
                        int kwdefaults = (oparg >> 8) & 0xff;
                        int num_annotations = (oparg >> 16) & 0x7fff;

                        lastState.pop(); // name
                        lastState.pop(); // code

                        if (opcode == MAKE_CLOSURE) {
                            lastState.pop(); // closure object
                        }

                        if (num_annotations > 0) {
                            lastState.pop();
                            for (int i = 0; i < num_annotations - 1; i++) {
                                lastState.pop();
                            }
                        }

                        for (int i = 0; i < kwdefaults; i++) {
                            lastState.pop();
                            lastState.pop();
                        }
                        for (int i = 0; i < posdefaults; i++) {
                            lastState.pop();

                        }

                        if (num_annotations == 0) {
                            lastState.push(&Function);
                        }
                        else {
                            // we're not sure of the type with an annotation present
                            lastState.push(&Any);
                        }
                        break;
                    }
                case BUILD_SLICE:
                    for (int i = 0; i < oparg; i++) {
                        lastState.pop();
                    }
                    lastState.push(&Slice);
                    break;
                case UNARY_POSITIVE:
                case UNARY_NEGATIVE:
                case UNARY_INVERT:
                case UNARY_NOT:
                    {
                        auto one = lastState.pop_no_escape();

                        auto unaryRes = one.Value->unary(one.Sources, opcode);

                        auto sources = add_intermediate_source(opcodeIndex);
                        AbstractSource::combine(
                            one.Sources,
                            sources
                            );

                        lastState.push(AbstractValueWithSources(unaryRes, sources));
                        break;
                    }
                case UNPACK_EX:
                    lastState.pop();
                    for (int i = 0; i < oparg >> 8; i++) {
                        lastState.push(&Any);
                    }
                    lastState.push(&List);
                    for (int i = 0; i < (oparg & 0xff); i++) {
                        lastState.push(&Any);
                    }
                    break;
                case UNPACK_SEQUENCE:
                    // TODO: If the sequence is a known type we could know what types we're pushing here.
                    lastState.pop();
                    for (int i = 0; i < oparg; i++) {
                        lastState.push(&Any);
                    }
                    break;
                case RAISE_VARARGS:
                    for (int i = 0; i < oparg; i++) {
                        lastState.pop();
                    }
                    goto next;
                case STORE_SUBSCR:
                    // TODO: Do we want to track types on store for lists?
                    lastState.pop();
                    lastState.pop();
                    lastState.pop();
                    break;
                case DELETE_SUBSCR:
                    lastState.pop();
                    lastState.pop();
                    break;
                case BUILD_SET:
                    for (int i = 0; i < oparg; i++) {
                        lastState.pop();
                    }
                    lastState.push(&Set);
                    break;
                case STORE_DEREF:
                    lastState.pop();
                    break;
                case LOAD_DEREF:
                    lastState.push(&Any);
                    break;
                case GET_ITER:
                    // TODO: Known iterable types
                    lastState.pop();
                    lastState.push(&Any);
                    break;
                case FOR_ITER:
                    {
                        // For branches out with the value consumed
                        auto leaveState = lastState;
                        leaveState.pop();
                        if (update_start_state(leaveState, (size_t)oparg + curByte + 1)) {
                            queue.push_back((size_t)oparg + curByte + 1);
                        }

                        // When we compile this we don't actually leave the value on the stack,
                        // but the sequence of opcodes assumes that happens.  to keep our stack
                        // properly balanced we match what's really going on.
                        lastState.push(&Any);

                        break;
                    }
                case SETUP_LOOP:
                    break;
                case POP_BLOCK:
                    // Restore the stack state to what we had on entry
                    lastState.m_stack = m_startStates[m_blockStarts[opcodeIndex]].m_stack;
                    //merge_states(m_startStates[m_blockStarts[opcodeIndex]], lastState);
                    break;
                case POP_EXCEPT:
                    break;
                case LOAD_BUILD_CLASS:
                    // TODO: if we know this is __builtins__.__build_class__ we can push a special value
                    // to optimize the call.f
                    lastState.push(&Any);
                    break;
                case SET_ADD:
                    // pop the value being stored off, leave set on stack
                    lastState.pop();
                    break;
                case LIST_APPEND:
                    // pop the value being stored off, leave list on stack
                    lastState.pop();
                    break;
                case MAP_ADD:
                    // pop the value and key being stored off, leave list on stack
                    lastState.pop();
                    lastState.pop();
                    break;
                case CONTINUE_LOOP:
                    if (update_start_state(lastState, oparg)) {
                        queue.push_back(oparg);
                    }
                    // Done processing this basic block, we'll need to see a branch
                    // to the following opcodes before we'll process them.
                    goto next;
                case BREAK_LOOP:
                    {
                        auto breakTo = m_breakTo[opcodeIndex];

                        // BREAK_LOOP does an unwind block, which restores the
                        // stack state to what it was when we entered the loop.  So
                        // we get the start state for where the SETUP_LOOP happened
                        // here and propagate it to where we're breaking to.
                        if (update_start_state(m_startStates[breakTo.BlockStart], breakTo.BlockEnd)) {
                            queue.push_back(breakTo.BlockEnd);
                        }

                        goto next;
                    }
                case SETUP_FINALLY:
                    {
                        auto finallyState = lastState;
                        // Finally is entered with value pushed onto stack indicating reason for 
                        // the finally running...
                        finallyState.push(&Any);
                        if (update_start_state(finallyState, (size_t)oparg + curByte + 1)) {
                            queue.push_back((size_t)oparg + curByte + 1);
                        }
                    }
                    break;
                case SETUP_EXCEPT:
                    {
                        auto ehState = lastState;
                        // Except is entered with the exception object, traceback, and exception
                        // type.  TODO: We could type these stronger then they currently are typed
                        ehState.push(&Any);
                        ehState.push(&Any);
                        ehState.push(&Any);
                        if (update_start_state(ehState, (size_t)oparg + curByte + 1)) {
                            queue.push_back((size_t)oparg + curByte + 1);
                        }
                    }
                    break;
                case END_FINALLY:
                    {
                        bool isFinally = m_endFinallyIsFinally[opcodeIndex];
                        if (isFinally) {
                            // single value indicating whether we're unwinding or
                            // completed normally.
                            lastState.pop();
                        }
                        else {
                            lastState.pop();
                            lastState.pop();
                            lastState.pop();
                        }
                        break;
                    }
                case SETUP_WITH:
                case YIELD_VALUE:
                    return false;
                default:
#ifdef _DEBUG
                    printf("Unknown unsupported opcode: %s", opcode_name(opcode));
#endif
                    return false;
            }
            update_start_state(lastState, curByte + 1);

        }
    next:;
    } while (queue.size() != 0);
    return true;
}

bool AbstractInterpreter::update_start_state(InterpreterState& newState, size_t index) {
    auto initialState = m_startStates.find(index);
    if (initialState != m_startStates.end()) {
        return merge_states(newState, initialState->second);
    }
    else {
        m_startStates[index] = newState;
        return true;
    }
}

bool AbstractInterpreter::merge_states(InterpreterState& newState, InterpreterState& mergeTo) {
    bool changed = false;
    if (mergeTo.m_locals != newState.m_locals) {
        //    if (mergeTo.m_locals.get() != newState.m_locals.get()) {
            // need to merge locals...
        _ASSERT(mergeTo.local_count() == newState.local_count());
        for (size_t i = 0; i < newState.local_count(); i++) {
            auto oldType = mergeTo.get_local(i);
            auto newType = oldType.merge_with(newState.get_local(i));
            if (newType != oldType) {
                if (oldType.ValueInfo.needs_boxing()) {
                    newType.ValueInfo.escapes();
                }
                mergeTo.replace_local(i, newType);
                changed = true;
            }

        }
    }

    if (mergeTo.stack_size() == 0) {
        // first time we assigned, or empty stack...
        mergeTo.m_stack = newState.m_stack;
        changed |= newState.stack_size() != 0;
    }
    else {
        // need to merge the stacks...
        _ASSERT(mergeTo.stack_size() == newState.stack_size());
        for (size_t i = 0; i < newState.stack_size(); i++) {
            auto newType = mergeTo[i].merge_with(newState[i]);
            if (mergeTo[i] != newType) {
                mergeTo[i] = newType;
                changed = true;
            }
        }
    }
    return changed;
}

AbstractValue* AbstractInterpreter::to_abstract(PyObject*value) {
    if (value == Py_None) {
        return &None;
    }
    else if (PyLong_CheckExact(value)) {
        return &Integer;
    }
    else if (PyUnicode_Check(value)) {
        return &String;
    }
    else if (PyList_CheckExact(value)) {
        return &List;
    }
    else if (PyDict_CheckExact(value)) {
        return &Dict;
    }
    else if (PyTuple_CheckExact(value)) {
        return &Tuple;
    }
    else if (PyBool_Check(value)) {
        return &Bool;
    }
    else if (PyFloat_CheckExact(value)) {
        return &Float;
    }
    else if (PyBytes_CheckExact(value)) {
        return &Bytes;
    }
    else if (PySet_Check(value)) {
        return &Set;
    }
    else if (PyComplex_CheckExact(value)) {
        return &Complex;
    }


    return &Any;
}

void AbstractInterpreter::dump() {
    printf("Dumping %s from %s line %d\r\n",
        PyUnicode_AsUTF8(m_code->co_name),
        PyUnicode_AsUTF8(m_code->co_filename),
        m_code->co_firstlineno
        );
    for (size_t curByte = 0; curByte < m_size; curByte++) {
        auto opcode = m_byteCode[curByte];
        auto byteIndex = curByte;

        auto find = m_startStates.find(byteIndex);
        if (find != m_startStates.end()) {
            auto state = find->second;
            for (size_t i = 0; i < state.local_count(); i++) {
                auto local = state.get_local(i);
                if (local.IsMaybeUndefined) {
                    if (local.ValueInfo.Value == &Undefined) {
                        printf(
                            "          %-20s UNDEFINED\r\n",
                            PyUnicode_AsUTF8(PyTuple_GetItem(m_code->co_varnames, i))
                            );
                    }
                    else {
                        printf(
                            "          %-20s %s (maybe UNDEFINED)\r\n",
                            PyUnicode_AsUTF8(PyTuple_GetItem(m_code->co_varnames, i)),
                            local.ValueInfo.Value->describe()
                            );
                    }
                    dump_sources(local.ValueInfo.Sources);
                }
                else {
                    printf(
                        "          %-20s %s\r\n",
                        PyUnicode_AsUTF8(PyTuple_GetItem(m_code->co_varnames, i)),
                        local.ValueInfo.Value->describe()
                        );
                    dump_sources(local.ValueInfo.Sources);

                }
            }
            for (size_t i = 0; i < state.stack_size(); i++) {
                printf("          %-20Id %s\r\n", i, state[i].Value->describe());
                dump_sources(state[i].Sources);
            }
        }

        if (HAS_ARG(opcode)) {
            int oparg = NEXTARG();
            switch (opcode) {
                case SETUP_LOOP:
                case SETUP_EXCEPT:
                case SETUP_FINALLY:
                case JUMP_FORWARD:
                case FOR_ITER:
                    printf("    %-3Id %-22s %d (to %Id)\r\n",
                        byteIndex,
                        opcode_name(opcode),
                        oparg,
                        oparg + curByte + 1
                        );
                    break;
                case LOAD_FAST:
                    printf("    %-3Id %-22s %d (%s) [%s]\r\n",
                        byteIndex,
                        opcode_name(opcode),
                        oparg,
                        PyUnicode_AsUTF8(PyTuple_GetItem(m_code->co_varnames, oparg)),
                        should_box(byteIndex) ? "BOXED" : "NON-BOXED"
                        );
                    break;
                case STORE_FAST:
                case DELETE_FAST:
                    printf("    %-3Id %-22s %d (%s)\r\n",
                        byteIndex,
                        opcode_name(opcode),
                        oparg,
                        PyUnicode_AsUTF8(PyTuple_GetItem(m_code->co_varnames, oparg))
                        );
                    break;
                case LOAD_ATTR:
                case STORE_ATTR:
                case DELETE_ATTR:
                case STORE_NAME:
                case LOAD_NAME:
                case DELETE_NAME:
                case LOAD_GLOBAL:
                case STORE_GLOBAL:
                case DELETE_GLOBAL:
                    printf("    %-3Id %-22s %d (%s)\r\n",
                        byteIndex,
                        opcode_name(opcode),
                        oparg,
                        PyUnicode_AsUTF8(PyTuple_GetItem(m_code->co_names, oparg))
                        );
                    break;
                case LOAD_CONST:
                    {
                        auto repr = PyObject_Repr(PyTuple_GetItem(m_code->co_consts, oparg));
                        auto reprStr = PyUnicode_AsUTF8(repr);
                        printf(
                            "    %-3Id %-22s %d (%s) [%s]\r\n",
                            byteIndex,
                            opcode_name(opcode),
                            oparg,
                            reprStr,
                            should_box(byteIndex) ? "BOXED" : "NON-BOXED"
                            );
                        Py_DECREF(repr);
                        break;
                    }
                default:
                    printf("    %-3Id %-22s %d\r\n", byteIndex, opcode_name(opcode), oparg);
                    break;
            }
        }
        else {
            printf("    %-3Id %-22s\r\n", byteIndex, opcode_name(opcode));
        }
    }
    printf("Returns %s\r\n", m_returnValue->describe());
}

bool AbstractInterpreter::should_box(size_t opcodeIndex) {
    auto boxInfo = m_opcodeSources.find(opcodeIndex);
    if (boxInfo != m_opcodeSources.end()) {
        if (boxInfo->second == nullptr) {
            return true;
        }
        return boxInfo->second->needs_boxing();
    }
    return true;
}

bool AbstractInterpreter::can_skip_lasti_update(size_t opcodeIndex) {
    switch (m_byteCode[opcodeIndex]) {
        case BINARY_TRUE_DIVIDE:
        case BINARY_FLOOR_DIVIDE:
        case BINARY_POWER:
        case BINARY_MODULO:
        case BINARY_MATRIX_MULTIPLY:
        case BINARY_LSHIFT:
        case BINARY_RSHIFT:
        case BINARY_AND:
        case BINARY_XOR:
        case BINARY_OR:
        case BINARY_MULTIPLY:
        case BINARY_SUBTRACT:
        case BINARY_ADD:
        case INPLACE_POWER:
        case INPLACE_MULTIPLY:
        case INPLACE_MATRIX_MULTIPLY:
        case INPLACE_TRUE_DIVIDE:
        case INPLACE_FLOOR_DIVIDE:
        case INPLACE_MODULO:
        case INPLACE_ADD:
        case INPLACE_SUBTRACT:
        case INPLACE_LSHIFT:
        case INPLACE_RSHIFT:
        case INPLACE_AND:
        case INPLACE_XOR:
        case INPLACE_OR:
        case COMPARE_OP:
        case LOAD_FAST:
            return !should_box(opcodeIndex);
    }

    return false;

}

void AbstractInterpreter::dump_sources(AbstractSource* sources) {
    if (sources != nullptr) {
        for (auto value : sources->Sources->Sources) {
            printf("              %s (%p)\r\n", value->describe(), value);
        }
    }
}

char* AbstractInterpreter::opcode_name(int opcode) {
#define OP_TO_STR(x)   case x: return #x;
    switch (opcode) {
        OP_TO_STR(POP_TOP)
            OP_TO_STR(ROT_TWO)
            OP_TO_STR(ROT_THREE)
            OP_TO_STR(DUP_TOP)
            OP_TO_STR(DUP_TOP_TWO)
            OP_TO_STR(NOP)
            OP_TO_STR(UNARY_POSITIVE)
            OP_TO_STR(UNARY_NEGATIVE)
            OP_TO_STR(UNARY_NOT)
            OP_TO_STR(UNARY_INVERT)
            OP_TO_STR(BINARY_MATRIX_MULTIPLY)
            OP_TO_STR(INPLACE_MATRIX_MULTIPLY)
            OP_TO_STR(BINARY_POWER)
            OP_TO_STR(BINARY_MULTIPLY)
            OP_TO_STR(BINARY_MODULO)
            OP_TO_STR(BINARY_ADD)
            OP_TO_STR(BINARY_SUBTRACT)
            OP_TO_STR(BINARY_SUBSCR)
            OP_TO_STR(BINARY_FLOOR_DIVIDE)
            OP_TO_STR(BINARY_TRUE_DIVIDE)
            OP_TO_STR(INPLACE_FLOOR_DIVIDE)
            OP_TO_STR(INPLACE_TRUE_DIVIDE)
            OP_TO_STR(GET_AITER)
            OP_TO_STR(GET_ANEXT)
            OP_TO_STR(BEFORE_ASYNC_WITH)
            OP_TO_STR(INPLACE_ADD)
            OP_TO_STR(INPLACE_SUBTRACT)
            OP_TO_STR(INPLACE_MULTIPLY)
            OP_TO_STR(INPLACE_MODULO)
            OP_TO_STR(STORE_SUBSCR)
            OP_TO_STR(DELETE_SUBSCR)
            OP_TO_STR(BINARY_LSHIFT)
            OP_TO_STR(BINARY_RSHIFT)
            OP_TO_STR(BINARY_AND)
            OP_TO_STR(BINARY_XOR)
            OP_TO_STR(BINARY_OR)
            OP_TO_STR(INPLACE_POWER)
            OP_TO_STR(GET_ITER)
            OP_TO_STR(PRINT_EXPR)
            OP_TO_STR(LOAD_BUILD_CLASS)
            OP_TO_STR(YIELD_FROM)
            OP_TO_STR(GET_AWAITABLE)
            OP_TO_STR(INPLACE_LSHIFT)
            OP_TO_STR(INPLACE_RSHIFT)
            OP_TO_STR(INPLACE_AND)
            OP_TO_STR(INPLACE_XOR)
            OP_TO_STR(INPLACE_OR)
            OP_TO_STR(BREAK_LOOP)
            OP_TO_STR(WITH_CLEANUP_START)
            OP_TO_STR(WITH_CLEANUP_FINISH)
            OP_TO_STR(RETURN_VALUE)
            OP_TO_STR(IMPORT_STAR)
            OP_TO_STR(YIELD_VALUE)
            OP_TO_STR(POP_BLOCK)
            OP_TO_STR(END_FINALLY)
            OP_TO_STR(POP_EXCEPT)
            OP_TO_STR(STORE_NAME)
            OP_TO_STR(DELETE_NAME)
            OP_TO_STR(UNPACK_SEQUENCE)
            OP_TO_STR(FOR_ITER)
            OP_TO_STR(UNPACK_EX)
            OP_TO_STR(STORE_ATTR)
            OP_TO_STR(DELETE_ATTR)
            OP_TO_STR(STORE_GLOBAL)
            OP_TO_STR(DELETE_GLOBAL)
            OP_TO_STR(LOAD_CONST)
            OP_TO_STR(LOAD_NAME)
            OP_TO_STR(BUILD_TUPLE)
            OP_TO_STR(BUILD_LIST)
            OP_TO_STR(BUILD_SET)
            OP_TO_STR(BUILD_MAP)
            OP_TO_STR(LOAD_ATTR)
            OP_TO_STR(COMPARE_OP)
            OP_TO_STR(IMPORT_NAME)
            OP_TO_STR(IMPORT_FROM)
            OP_TO_STR(JUMP_FORWARD)
            OP_TO_STR(JUMP_IF_FALSE_OR_POP)
            OP_TO_STR(JUMP_IF_TRUE_OR_POP)
            OP_TO_STR(JUMP_ABSOLUTE)
            OP_TO_STR(POP_JUMP_IF_FALSE)
            OP_TO_STR(POP_JUMP_IF_TRUE)
            OP_TO_STR(LOAD_GLOBAL)
            OP_TO_STR(CONTINUE_LOOP)
            OP_TO_STR(SETUP_LOOP)
            OP_TO_STR(SETUP_EXCEPT)
            OP_TO_STR(SETUP_FINALLY)
            OP_TO_STR(LOAD_FAST)
            OP_TO_STR(STORE_FAST)
            OP_TO_STR(DELETE_FAST)
            OP_TO_STR(RAISE_VARARGS)
            OP_TO_STR(CALL_FUNCTION)
            OP_TO_STR(MAKE_FUNCTION)
            OP_TO_STR(BUILD_SLICE)
            OP_TO_STR(MAKE_CLOSURE)
            OP_TO_STR(LOAD_CLOSURE)
            OP_TO_STR(LOAD_DEREF)
            OP_TO_STR(STORE_DEREF)
            OP_TO_STR(DELETE_DEREF)
            OP_TO_STR(CALL_FUNCTION_VAR)
            OP_TO_STR(CALL_FUNCTION_KW)
            OP_TO_STR(CALL_FUNCTION_VAR_KW)
            OP_TO_STR(SETUP_WITH)
            OP_TO_STR(EXTENDED_ARG)
            OP_TO_STR(LIST_APPEND)
            OP_TO_STR(SET_ADD)
            OP_TO_STR(MAP_ADD)
            OP_TO_STR(LOAD_CLASSDEREF)
            OP_TO_STR(BUILD_LIST_UNPACK)
            OP_TO_STR(BUILD_MAP_UNPACK)
            OP_TO_STR(BUILD_MAP_UNPACK_WITH_CALL)
            OP_TO_STR(BUILD_TUPLE_UNPACK)
            OP_TO_STR(BUILD_SET_UNPACK)
            OP_TO_STR(SETUP_ASYNC_WITH)
    }
    return "unknown";
}


// Returns information about the specified local variable at a specific 
// byte code index.
AbstractLocalInfo AbstractInterpreter::get_local_info(size_t byteCodeIndex, size_t localIndex) {
    return m_startStates[byteCodeIndex].get_local(localIndex);
}

// Returns information about the stack at the specific byte code index.
vector<AbstractValueWithSources>& AbstractInterpreter::get_stack_info(size_t byteCodeIndex) {
    return m_startStates[byteCodeIndex].m_stack;
}

AbstractValue* AbstractInterpreter::get_return_info() {
    return m_returnValue;
}

bool AbstractInterpreter::has_info(size_t byteCodeIndex) {
    return m_startStates.find(byteCodeIndex) != m_startStates.end();
}


AbstractSource* AbstractInterpreter::add_local_source(size_t opcodeIndex, size_t localIndex) {
    auto store = m_opcodeSources.find(opcodeIndex);
    if (store == m_opcodeSources.end()) {
        return m_opcodeSources[opcodeIndex] = new_source(new LocalSource());
    }

    return store->second;
}

AbstractSource* AbstractInterpreter::add_const_source(size_t opcodeIndex, size_t constIndex) {
    auto store = m_opcodeSources.find(opcodeIndex);
    if (store == m_opcodeSources.end()) {
        return m_opcodeSources[opcodeIndex] = new_source(new ConstSource());
    }

    return store->second;
}

AbstractSource* AbstractInterpreter::add_intermediate_source(size_t opcodeIndex) {
    auto store = m_opcodeSources.find(opcodeIndex);
    if (store == m_opcodeSources.end()) {
        return m_opcodeSources[opcodeIndex] = new_source(new IntermediateSource());
    }

    return store->second;
}

/*****************
 * Code generation
 
 */


 /************************************************************************
 * Compiler driver code...
 */



 // Checks to see if we have a non-zero error code on the stack, and if so,
 // branches to the current error handler.  Consumes the error code in the process
void AbstractInterpreter::int_error_check() {
	auto noErr = m_comp->emit_define_label();
	m_comp->emit_int(0);
	m_comp->emit_branch(BranchEqual, noErr);

	branch_raise();
	m_comp->emit_mark_label(noErr);
}

// Checks to see if we have a null value as the last value on our stack
// indicating an error, and if so, branches to our current error handler.
void AbstractInterpreter::error_check() {
	auto noErr = m_comp->emit_define_label();
	m_comp->emit_dup();
	m_comp->emit_null();
	m_comp->emit_branch(BranchNotEqual, noErr);

	m_comp->emit_pop();
	branch_raise();
	m_comp->emit_mark_label(noErr);
}

Label AbstractInterpreter::getOffsetLabel(int jumpTo) {
	auto jumpToLabelIter = m_offsetLabels.find(jumpTo);
	Label jumpToLabel;
	if (jumpToLabelIter == m_offsetLabels.end()) {
		m_offsetLabels[jumpTo] = jumpToLabel = m_comp->emit_define_label();
	}
	else {
		jumpToLabel = jumpToLabelIter->second;
	}
	return jumpToLabel;
}

vector<Label>& AbstractInterpreter::getRaiseAndFreeLabels(size_t blockId) {
	while (m_raiseAndFree.size() <= blockId) {
		m_raiseAndFree.emplace_back();
	}

	return m_raiseAndFree.data()[blockId];
}

void AbstractInterpreter::branch_raise() {
	auto ehBlock = get_ehblock();

	auto& entry_stack = ehBlock.Stack;

	// clear any non-object values from the stack up
	// to the stack that owned the block when we entered.
	size_t count = m_stack.size() - entry_stack.size();

	for (auto cur = m_stack.rbegin(); cur != m_stack.rend(); cur++) {
		if (*cur == STACK_KIND_VALUE) {
			count--;
			m_comp->emit_pop();
		}
		else {
			break;
		}
	}

	if (count == 0) {
		m_comp->emit_branch(BranchAlways, ehBlock.Raise);
	}
	else {
		// We don't currently expect to have stacks w/ mixed value and object types...
		// If we hit this then we need to support cleaning those up too.
		for (auto value : m_stack) {
			_ASSERTE(value != STACK_KIND_VALUE);
		}

		auto& labels = getRaiseAndFreeLabels(ehBlock.BlockId);

		for (auto i = labels.size(); i < count; i++) {
			labels.push_back(m_comp->emit_define_label());
		}

		m_comp->emit_branch(BranchAlways, labels[count - 1]);
	}
}

void AbstractInterpreter::clean_stack_for_reraise() {
	auto ehBlock = get_ehblock();

	auto& entry_stack = ehBlock.Stack;
	size_t count = m_stack.size() - entry_stack.size();

	for (size_t i = m_stack.size(); i-- > entry_stack.size();) {
		m_comp->emit_pop_top();
	}
}

#define CALL_FLAG_VAR 1
#define CALL_FLAG_KW 2

void AbstractInterpreter::fancy_call(int na, int nk, int flags) {
	int n = na + 2 * nk;
	Local varArgs, varKwArgs, map;
	if (flags & CALL_FLAG_KW) {
		// kw args dict is last on the stack, save it....
		varKwArgs = m_comp->emit_spill();
		dec_stack();
	}

	if (nk != 0) {
		// if we have keywords build the map, and then save them...
		build_map(nk);
		map = m_comp->emit_spill();
	}

	if (flags & CALL_FLAG_VAR) {
		// then save var args...
		varArgs = m_comp->emit_spill();
		dec_stack();
	}

	// now we have the normal args (if any), and then the function
	// Build a tuple of the normal args...
	if (na != 0) {
		build_tuple(na);
	}
	else {
		m_comp->emit_null();
	}

	// If we have keywords load them or null
	if (nk != 0) {
		m_comp->emit_load_and_free_local(map);
	}
	else {
		m_comp->emit_null();
	}

	// If we have var args load them or null
	if (flags & CALL_FLAG_VAR) {
		m_comp->emit_load_and_free_local(varArgs);
	}
	else {
		m_comp->emit_null();
	}

	// If we have a kw dict, load it...
	if (flags & CALL_FLAG_KW) {
		m_comp->emit_load_and_free_local(varKwArgs);
	}
	else {
		m_comp->emit_null();
	}
	dec_stack(); // for the callable
				 // finally emit the call to our helper...

	m_comp->emit_fancy_call();
	error_check();

	// the result is back...
	inc_stack();
}

void AbstractInterpreter::build_tuple(size_t argCnt) {
	m_comp->emit_new_tuple(argCnt);
	if (argCnt != 0) {
		error_check();
		m_comp->emit_tuple_store(argCnt);
		dec_stack(argCnt);
	}
}

void AbstractInterpreter::build_list(size_t argCnt) {
	m_comp->emit_new_list(argCnt);
	error_check();
	if (argCnt != 0) {
		m_comp->emit_list_store(argCnt);
	}
	dec_stack(argCnt);
}


void AbstractInterpreter::build_set(size_t argCnt) {
	m_comp->emit_new_set();
	error_check();
	m_comp->emit_set_store(argCnt);
	dec_stack(argCnt);
}


void AbstractInterpreter::build_map(size_t  argCnt) {
	m_comp->emit_new_dict(argCnt);
	error_check();

	if (argCnt > 0) {
		auto map = m_comp->emit_spill();
		for (size_t curArg = 0; curArg < argCnt; curArg++) {
			m_comp->emit_load_local(map);

			m_comp->emit_dict_store();

			dec_stack(2);
			int_error_check();
		}
		m_comp->emit_load_and_free_local(map);
	}
}

void AbstractInterpreter::make_function(int posdefaults, int kwdefaults, int num_annotations, bool isClosure) {
	m_comp->emit_new_function();
	dec_stack(2);

	if (isClosure) {
		m_comp->emit_set_closure();
		dec_stack();
	}
	if (num_annotations > 0 || kwdefaults > 0 || posdefaults > 0) {
		auto func = m_comp->emit_spill();
		//dec_stack();
		if (num_annotations > 0) {
			// names is on stack, followed by values.
			//PyObject* values, PyObject* names, PyObject* func
			auto names = m_comp->emit_spill();
			dec_stack();

			// for whatever reason ceval.c has "assert(num_annotations == name_ix+1);", where
			// name_ix is the numbe of elements in the names tuple.  Otherwise num_annotations
			// goes unused!
			// TODO: Stack count isn't quite right here...
			build_tuple(num_annotations - 1);

			m_comp->emit_load_and_free_local(names);
			m_comp->emit_load_local(func);
			m_comp->emit_set_annotations();

			int_error_check();
		}
		if (kwdefaults > 0) {
			// TODO: If we hit an OOM here then build_map doesn't release the function
			build_map(kwdefaults);
			m_comp->emit_load_local(func);
			m_comp->emit_set_kw_defaults();
			int_error_check();
		}
		if (posdefaults > 0) {
			build_tuple(posdefaults);
			m_comp->emit_load_local(func);

			m_comp->emit_set_defaults();
			int_error_check();
		}
		m_comp->emit_load_and_free_local(func);
	}
	inc_stack();
}

void AbstractInterpreter::dec_stack(size_t size) {
	_ASSERTE(m_stack.size() >= size);
	for (size_t i = 0; i < size; i++) {
		m_stack.pop_back();
	}
}

void AbstractInterpreter::inc_stack(size_t size, bool kind) {
	for (size_t i = 0; i < size; i++) {
		m_stack.push_back(kind);
	}
}

// Frees our iteration temporary variable which gets allocated when we hit
// a FOR_ITER.  Used when we're breaking from the current loop.
void AbstractInterpreter::free_iter_local() {
	for (auto cur = m_blockStack.rbegin(); cur != m_blockStack.rend(); cur++) {
		if ((*cur).Kind == SETUP_LOOP) {
			if ((*cur).LoopVar.is_valid()) {
				m_comp->emit_load_local((*cur).LoopVar);
				m_comp->emit_pop_top();
				break;
			}
		}
	}
}

// Frees all of the iteration variables in a range. Used when we're
// going to branch to a finally through multiple loops.
void AbstractInterpreter::free_all_iter_locals(size_t to) {
	for (size_t i = m_blockStack.size() - 1; i != to - 1; i--) {
		if (m_blockStack[i].Kind == SETUP_LOOP) {
			if (m_blockStack[i].LoopVar.is_valid()) {
				m_comp->emit_load_local(m_blockStack[i].LoopVar);
				m_comp->emit_pop_top();
			}
		}
	}
}

// Frees all of our iteration variables.  Used when we're unwinding the function
// on an exception.
void AbstractInterpreter::free_iter_locals_on_exception() {
	int loopCount = 0;
	for (auto cur = m_blockStack.rbegin(); cur != m_blockStack.rend(); cur++) {
		if ((*cur).Kind == SETUP_LOOP) {
			if ((*cur).LoopVar.is_valid()) {
				m_comp->emit_load_local((*cur).LoopVar);
				m_comp->emit_pop_top();
			}
		}
		else {
			break;
		}
	}
}

// Handles POP_JUMP_IF_FALSE/POP_JUMP_IF_TRUE with a possible error value on the stack.
// If the value on the stack is -1, we branch to the current error handler.
// Otherwise branches based if the current value is true/false based upon the current opcode 
void AbstractInterpreter::branch_or_error(int& curByte) {
	auto jmpType = m_byteCode[curByte + 1];
	curByte++;
	mark_offset_label(curByte);
	auto oparg = NEXTARG();

	m_comp->emit_dup();
	m_comp->emit_int(-1);

	auto noErr = m_comp->emit_define_label();
	m_comp->emit_branch(BranchNotEqual, noErr);
	// we need to issue a leave to clear the stack as we may have
	// values on the stack...

	m_comp->emit_pop();
	branch_raise();
	m_comp->emit_mark_label(noErr);

	m_comp->emit_branch(jmpType == POP_JUMP_IF_FALSE ? BranchFalse : BranchTrue, getOffsetLabel(oparg));
	m_offsetStack[oparg] = m_stack;
}

// Handles POP_JUMP_IF_FALSE/POP_JUMP_IF_TRUE with a bool value known to be on the stack.
// Branches based if the current value is true/false based upon the current opcode 
void AbstractInterpreter::branch(int& curByte) {
	auto jmpType = m_byteCode[curByte + 1];
	curByte++;
	mark_offset_label(curByte);
	auto oparg = NEXTARG();

	m_comp->emit_branch(jmpType == POP_JUMP_IF_FALSE ? BranchFalse : BranchTrue, getOffsetLabel(oparg));
	dec_stack();
	m_offsetStack[oparg] = m_stack;
}

JittedCode* AbstractInterpreter::compile_worker() {
	int oparg;
	Label ok;

	auto raiseLabel = m_comp->emit_define_label();
	auto reraiseLabel = m_comp->emit_define_label();

	m_comp->emit_lasti_init();
	m_comp->emit_push_frame();

	m_blockStack.push_back(BlockInfo(vector<bool>(), m_blockIds++, raiseLabel, reraiseLabel, Label(), -1, NOP));

	for (int curByte = 0; curByte < m_size; curByte++) {
		auto opcodeIndex = curByte;
		auto byte = m_byteCode[curByte];

		// See FOR_ITER for special handling of the offset label
		if (byte != FOR_ITER) {
			mark_offset_label(curByte);
		}

		auto curStackDepth = m_offsetStack.find(curByte);
		if (curStackDepth != m_offsetStack.end()) {
			m_stack = curStackDepth->second;
		}

		// update f_lasti
		if (!can_skip_lasti_update(curByte)) {
			m_comp->emit_lasti_update(curByte);
		}

		if (HAS_ARG(byte)) {
			oparg = NEXTARG();
		}
	processOpCode:
		switch (byte) {
		case NOP: break;
		case ROT_TWO: m_comp->emit_rot_two(); break;
		case ROT_THREE: m_comp->emit_rot_three(); break;
		case POP_TOP:
			m_comp->emit_pop_top();
			dec_stack();
			break;
		case DUP_TOP:
			m_comp->emit_dup_top();
			inc_stack();
			break;
		case DUP_TOP_TWO:
			inc_stack(2);
			m_comp->emit_dup_top_two();
			break;
		case COMPARE_OP: compare_op(oparg, curByte, opcodeIndex); break;
		case SETUP_LOOP:
			// offset is relative to end of current instruction
			m_blockStack.push_back(
				BlockInfo(
					m_stack,
					m_blockStack.back().BlockId,
					m_blockStack.back().Raise,
					m_blockStack.back().ReRaise,
					m_blockStack.back().ErrorTarget,
					oparg + curByte + 1,
					SETUP_LOOP
					)
				);
			break;
		case BREAK_LOOP:
		case CONTINUE_LOOP:
			// if we have finally blocks we need to unwind through them...
			// used in exceptional case...
		{
			bool inFinally = false;
			size_t loopIndex = -1, clearEh = -1;
			for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
				if (m_blockStack[i].Kind == SETUP_LOOP) {
					// we found our loop, we don't need additional processing...
					loopIndex = i;
					break;
				}
				else if (m_blockStack[i].Kind == END_FINALLY || m_blockStack[i].Kind == POP_EXCEPT) {
					if (clearEh == -1) {
						clearEh = i;
					}
				}
				else if (m_blockStack[i].Kind == SETUP_FINALLY) {
					// we need to run the finally before continuing to the loop...
					// That means we need to spill the stack, branch to the finally,
					// run it, and have the finally branch back to our oparg.
					// CPython handles this by pushing the opcode to continue at onto
					// the stack, and then pushing an integer value which indicates END_FINALLY
					// should continue execution.  Our END_FINALLY expects only a single value
					// on the stack, and we also need to preserve any loop variables.
					m_blockStack.data()[i].Flags |= byte == BREAK_LOOP ? EHF_BlockBreaks : EHF_BlockContinues;

					if (!inFinally) {
						// only emit the branch to the first finally, subsequent branches
						// to other finallys will be handled by the END_FINALLY code.  But we
						// need to mark those finallys as needing special handling.
						inFinally = true;
						if (clearEh != -1) {
							unwind_eh(m_blockStack[clearEh].ExVars);
						}
						m_comp->emit_int(byte == BREAK_LOOP ? EHF_BlockBreaks : EHF_BlockContinues);
						m_comp->emit_branch(BranchAlways, m_blockStack[i].ErrorTarget);
						if (byte == CONTINUE_LOOP) {
							m_blockStack.data()[i].ContinueOffset = oparg;
						}
					}
				}
			}

			if (!inFinally) {
				if (clearEh != -1) {
					unwind_eh(m_blockStack[clearEh].ExVars);
				}
				if (byte != CONTINUE_LOOP) {
					free_iter_local();
				}

				if (byte == BREAK_LOOP) {
					assert(loopIndex != -1);
					m_comp->emit_branch(BranchAlways, getOffsetLabel(m_blockStack[loopIndex].EndOffset));
				}
				else {
					m_comp->emit_branch(BranchAlways, getOffsetLabel(oparg));
				}
			}

		}
		break;
		case LOAD_BUILD_CLASS:
			m_comp->emit_load_build_class();
			error_check();
			inc_stack();
			break;
		case JUMP_ABSOLUTE: jump_absolute(oparg); break;
		case JUMP_FORWARD:  jump_absolute(oparg + curByte + 1); break;
		case JUMP_IF_FALSE_OR_POP:
		case JUMP_IF_TRUE_OR_POP: jump_if_or_pop(byte != JUMP_IF_FALSE_OR_POP, opcodeIndex, oparg); break;
		case POP_JUMP_IF_TRUE:
		case POP_JUMP_IF_FALSE: pop_jump_if(byte != POP_JUMP_IF_FALSE, opcodeIndex, oparg); break;
		case LOAD_NAME:
			m_comp->emit_load_name(PyTuple_GetItem(m_code->co_names, oparg));
			error_check();
			inc_stack();
			break;
		case STORE_ATTR:
			m_comp->emit_store_attr(PyTuple_GetItem(m_code->co_names, oparg));
			dec_stack(2);
			int_error_check();
			break;
		case DELETE_ATTR:
			m_comp->emit_delete_attr(PyTuple_GetItem(m_code->co_names, oparg));
			int_error_check();
			dec_stack();
			break;
		case LOAD_ATTR:
			m_comp->emit_load_attr(PyTuple_GetItem(m_code->co_names, oparg));
			dec_stack();
			error_check();
			inc_stack();
			break;
		case STORE_GLOBAL:
			m_comp->emit_store_global(PyTuple_GetItem(m_code->co_names, oparg));
			dec_stack();
			int_error_check();
			break;
		case DELETE_GLOBAL:
			m_comp->emit_delete_global(PyTuple_GetItem(m_code->co_names, oparg));
			int_error_check();
			break;
		case LOAD_GLOBAL:
			m_comp->emit_load_global(PyTuple_GetItem(m_code->co_names, oparg));
			error_check();
			inc_stack();
			break;
		case LOAD_CONST: load_const(oparg, opcodeIndex); break;
		case STORE_NAME:
			m_comp->emit_store_name(PyTuple_GetItem(m_code->co_names, oparg));
			dec_stack();
			int_error_check();
			break;
		case DELETE_NAME:
			m_comp->emit_delete_name(PyTuple_GetItem(m_code->co_names, oparg));
			int_error_check();
			break;
		case DELETE_FAST:
			load_fast_worker(oparg, true);
			m_comp->emit_pop_top();
			m_comp->emit_delete_fast(oparg, PyTuple_GetItem(m_code->co_varnames, oparg));
			break;
		case STORE_FAST: store_fast(oparg, opcodeIndex); break;
		case LOAD_FAST: load_fast(oparg, opcodeIndex); break;
		case UNPACK_SEQUENCE:
			unpack_sequence(oparg, curByte);
			break;
		case UNPACK_EX: unpack_ex(oparg, curByte); break;
		case CALL_FUNCTION_VAR:
		case CALL_FUNCTION_KW:
		case CALL_FUNCTION_VAR_KW:
			fancy_call(NUM_ARGS(oparg), NUM_KW_ARGS(oparg), (byte - CALL_FUNCTION) & 3);
			break;
		case CALL_FUNCTION:
		{
			size_t argCnt = oparg & 0xff, kwArgCnt = (oparg >> 8) & 0xff;
			if (kwArgCnt == 0) {
				if (!m_comp->emit_call(argCnt)) {
					build_tuple(argCnt);
					m_comp->emit_call_with_tuple();
					dec_stack();// function
				}
				else {
					dec_stack(argCnt + 1); // + function
				}
			}
			else {
				build_map(kwArgCnt);
				auto map = m_comp->emit_spill();

				build_tuple(argCnt);
				m_comp->emit_load_and_free_local(map);

				m_comp->emit_call_with_kws();
				dec_stack();
			}
			error_check();
			inc_stack();
			break;
		}
		case BUILD_TUPLE:
			build_tuple(oparg);
			inc_stack();
			break;
		case BUILD_LIST:
			build_list(oparg);
			inc_stack();
			break;
		case BUILD_MAP:
			build_map(oparg);
			inc_stack();
			break;
		case STORE_SUBSCR:
			dec_stack(3);
			m_comp->emit_store_subscr();
			int_error_check();
			break;
		case DELETE_SUBSCR:
			dec_stack(2);
			m_comp->emit_delete_subscr();
			int_error_check();
			break;
		case BUILD_SLICE:
			dec_stack(oparg);
			if (oparg != 3) {
				m_comp->emit_null();
			}
			m_comp->emit_build_slice();
			inc_stack();
			break;
		case BUILD_SET: build_set(oparg); inc_stack(); break;
		case UNARY_POSITIVE: unary_positive(opcodeIndex); break;
		case UNARY_NEGATIVE: unary_negative(opcodeIndex); break;
		case UNARY_NOT: unary_not(curByte); break;
		case UNARY_INVERT:
			dec_stack(1);
			m_comp->emit_unary_invert();
			error_check();
			inc_stack();
			break;
		case BINARY_SUBSCR:
		case BINARY_ADD:
		case BINARY_TRUE_DIVIDE:
		case BINARY_FLOOR_DIVIDE:
		case BINARY_POWER:
		case BINARY_MODULO:
		case BINARY_MATRIX_MULTIPLY:
		case BINARY_LSHIFT:
		case BINARY_RSHIFT:
		case BINARY_AND:
		case BINARY_XOR:
		case BINARY_OR:
		case BINARY_MULTIPLY:
		case BINARY_SUBTRACT:
		case INPLACE_POWER:
		case INPLACE_MULTIPLY:
		case INPLACE_MATRIX_MULTIPLY:
		case INPLACE_TRUE_DIVIDE:
		case INPLACE_FLOOR_DIVIDE:
		case INPLACE_MODULO:
		case INPLACE_ADD:
		case INPLACE_SUBTRACT:
		case INPLACE_LSHIFT:
		case INPLACE_RSHIFT:
		case INPLACE_AND:
		case INPLACE_XOR:
		case INPLACE_OR:
			if (!should_box(opcodeIndex)) {
				auto stackInfo = get_stack_info(opcodeIndex);
				auto one = stackInfo[stackInfo.size() - 1];
				auto two = stackInfo[stackInfo.size() - 2];

				// Currently we only optimize floating point numbers..
				if (one.Value->kind() == AVK_Float && two.Value->kind() == AVK_Float) {
					_ASSERTE(m_stack[m_stack.size() - 1] == STACK_KIND_VALUE);
					_ASSERTE(m_stack[m_stack.size() - 2] == STACK_KIND_VALUE);

					dec_stack(2);

					if (byte == INPLACE_TRUE_DIVIDE || byte == BINARY_TRUE_DIVIDE) {
						m_comp->emit_dup();
						m_comp->emit_float(0);
						auto noErr = m_comp->emit_define_label();
						m_comp->emit_compare_equal();
						m_comp->emit_branch(BranchFalse, noErr);
						// Move the stack depth down to zero (we already did the decstack above)
						m_comp->emit_pop();
						m_comp->emit_pop();
						m_comp->emit_pyerr_setstring(PyExc_ZeroDivisionError, "float division by zero");
						branch_raise();

						m_comp->emit_mark_label(noErr);
					}

					m_comp->emit_binary_float(byte);

					inc_stack(1, STACK_KIND_VALUE);
					break;
				}
				else if (one.Value->kind() == AVK_Integer && two.Value->kind() == AVK_Integer) {
					dec_stack(2);

					m_comp->emit_binary_tagged_int(byte);

					error_check();

					inc_stack();
					break;
				}
			}
			dec_stack(2);

			m_comp->emit_binary_object(byte);

			error_check();
			inc_stack();

			break;
		case RETURN_VALUE: return_value(opcodeIndex); break;
		case EXTENDED_ARG:
		{
			byte = m_byteCode[++curByte];
			int bottomArg = NEXTARG();
			oparg = (oparg << 16) | bottomArg;
			goto processOpCode;
		}
		case MAKE_CLOSURE:
		case MAKE_FUNCTION:
			make_function(oparg & 0xff, (oparg >> 8) & 0xff, (oparg >> 16) & 0x7fff, byte == MAKE_CLOSURE);
			break;
		case LOAD_DEREF:
			m_comp->emit_load_deref(oparg);
			error_check();
			inc_stack();
			break;
		case STORE_DEREF:
			dec_stack();
			m_comp->emit_store_deref(oparg);
			break;
		case DELETE_DEREF: m_comp->emit_delete_deref(oparg); break;
		case LOAD_CLOSURE:
			m_comp->emit_load_closure(oparg);
			inc_stack();
			break;
		case GET_ITER:
			// GET_ITER can be followed by FOR_ITER, or a CALL_FUNCTION.                
		{
			/*
			bool optFor = false;
			Local loopOpt1, loopOpt2;
			if (m_byteCode[curByte + 1] == FOR_ITER) {
			for (size_t blockIndex = m_blockStack.size() - 1; blockIndex != (-1); blockIndex--) {
			if (m_blockStack[blockIndex].Kind == SETUP_LOOP) {
			// save our iter variable so we can free it on break, continue, return, and
			// when encountering an exception.
			m_blockStack.data()[blockIndex].LoopOpt1 = loopOpt1 = m_comp->emit_define_local(Parameter(CORINFO_TYPE_NATIVEINT));
			m_blockStack.data()[blockIndex].LoopOpt2 = loopOpt2 = m_comp->emit_define_local(Parameter(CORINFO_TYPE_NATIVEINT));
			optFor = true;
			break;
			}
			}
			}
			*/
			/*if (optFor) {
			m_comp->emit_getiter_opt();
			}
			else*/ {
				m_comp->emit_getiter();
				dec_stack();
				error_check();
				inc_stack();
			}
		}
		break;
		case FOR_ITER:
		{
			BlockInfo *loopBlock = nullptr;
			for (size_t blockIndex = m_blockStack.size() - 1; blockIndex != -1; blockIndex--) {
				if (m_blockStack[blockIndex].Kind == SETUP_LOOP) {
					// save our iter variable so we can free it on break, continue, return, and
					// when encountering an exception.
					loopBlock = &m_blockStack.data()[blockIndex];
					//loopOpt1 = m_blockStack.data()[blockIndex].LoopOpt1;
					//loopOpt2 = m_blockStack.data()[blockIndex].LoopOpt2;
					//inLoop = true;
					break;
				}
			}
			for_iter(curByte + oparg + 1, curByte - 2, loopBlock);
			break;
		}
		case SET_ADD:
			// TODO: Are these stack counts right?
			// We error_check with the set/map/list on the
			// stack, but it's not in the count
			m_comp->emit_set_add();
			dec_stack(2);
			error_check();
			inc_stack();
			break;
		case MAP_ADD:
			m_comp->emit_map_add();
			dec_stack(3);
			error_check();
			inc_stack();
			break;
		case LIST_APPEND:
			m_comp->emit_list_append();
			dec_stack(2);
			error_check();
			inc_stack();
			break;
		case PRINT_EXPR:
			m_comp->emit_print_expr();
			dec_stack();
			int_error_check();
			break;
		case LOAD_CLASSDEREF:
			m_comp->emit_load_classderef(oparg);
			error_check();
			inc_stack();
			break;
		case RAISE_VARARGS:
			// do raise (exception, cause)
			// We can be invoked with no args (bare raise), raise exception, or raise w/ exceptoin and cause
			switch (oparg) {
			case 0: m_comp->emit_null();
			case 1: m_comp->emit_null();
			case 2:
				dec_stack(oparg);
				// raise exc
				m_comp->emit_raise_varargs();
				// returns 1 if we're doing a re-raise in which case we don't need
				// to update the traceback.  Otherwise returns 0.
				auto curHandler = get_ehblock();
				if (oparg == 0) {
					m_comp->emit_branch(BranchFalse, curHandler.Raise);
					m_comp->emit_branch(BranchAlways, curHandler.ReRaise);
				}
				else {
					// if we have args we'll always return 0...
					m_comp->emit_pop();
					m_comp->emit_branch(BranchAlways, curHandler.Raise);
				}
				break;
			}
			break;
		case SETUP_EXCEPT:
		{
			auto handlerLabel = getOffsetLabel(oparg + curByte + 1);
			auto blockInfo = BlockInfo(m_stack, m_blockIds++, m_comp->emit_define_label(), m_comp->emit_define_label(), handlerLabel, oparg + curByte + 1, SETUP_EXCEPT);
			blockInfo.ExVars = ExceptionVars(
				m_comp->emit_define_local(true),
				m_comp->emit_define_local(true),
				m_comp->emit_define_local(true)
				);
			m_blockStack.push_back(blockInfo);
			m_allHandlers.push_back(blockInfo);
			m_ehInfo.push_back(EhInfo(false));
			vector<bool> newStack = m_stack;
			for (int j = 0; j < 3; j++) {
				newStack.push_back(STACK_KIND_OBJECT);
			}
			m_offsetStack[oparg + curByte + 1] = newStack;
		}
		break;
		case SETUP_FINALLY:
		{
			auto handlerLabel = getOffsetLabel(oparg + curByte + 1);
			auto blockInfo = BlockInfo(m_stack, m_blockIds++, m_comp->emit_define_label(), m_comp->emit_define_label(), handlerLabel, oparg + curByte + 1, SETUP_FINALLY);
			blockInfo.ExVars = ExceptionVars(
				m_comp->emit_define_local(true),
				m_comp->emit_define_local(true),
				m_comp->emit_define_local(true)
				);
			m_blockStack.push_back(blockInfo);
			m_allHandlers.push_back(blockInfo);
			m_ehInfo.push_back(EhInfo(true));
		}
		break;
		case POP_EXCEPT: pop_except(); break;
		case POP_BLOCK:
		{
			auto curHandler = m_blockStack.back();
			m_blockStack.pop_back();
			if (curHandler.Kind == SETUP_FINALLY || curHandler.Kind == SETUP_EXCEPT) {
				m_ehInfo.data()[m_ehInfo.size() - 1].Flags = curHandler.Flags;

				// convert block into an END_FINALLY BlockInfo which will
				// dispatch to all of the previous locations...
				auto back = m_blockStack.back();
				auto newBlock = BlockInfo(
					back.Stack,
					back.BlockId,
					back.Raise,		// if we take a nested exception this is where we go to...
					back.ReRaise,
					back.ErrorTarget,
					back.EndOffset,
					curHandler.Kind == SETUP_FINALLY ? END_FINALLY : POP_EXCEPT,
					curHandler.Flags,
					curHandler.ContinueOffset
					);
				newBlock.ExVars = curHandler.ExVars;
				m_blockStack.push_back(newBlock);
			}
		}
		break;
		case END_FINALLY:
		{
			// CPython excepts END_FINALLY can be entered in 1 of 3 ways:
			//	1) With a status code for why the finally is unwinding, indicating a RETURN
			//			or a continue.  In this case there is an extra retval on the stack
			//	2) With an excpetion class which is being raised.  In this case there are 2 extra
			//			values on the stack, the exception value, and the traceback.
			//	3) After the try block has completed normally.  In this case None is on the stack.
			//
			//	That means in CPython this opcode can be branched to with 1 of 3 different stack
			//		depths, and the CLR doesn't like that.  Worse still the rest of the generated
			//		byte code assumes this is true.  For case 2 an except handler includes tests
			//		and pops which remove the 3 values from the class.  For case 3 the byte code
			//		at the end of the finally range includes the opcode to load None.
			//
			//  END_FINALLY can also be encountered w/o a SETUP_FINALLY, as happens when it's used
			//	solely for re-throwing exceptions.

			auto ehInfo = m_ehInfo.back();
			m_ehInfo.pop_back();
			auto exVars = m_blockStack.back().ExVars;
			m_blockStack.pop_back();

			if (ehInfo.IsFinally) {
				int flags = ehInfo.Flags;

				// restore the previous exception...
				unwind_eh(exVars);

				// We're actually ending a finally.  If we're in an exceptional case we
				// need to re-throw, otherwise we need to just continue execution.  Our
				// exception handling code will only push the exception type on in this case.
				auto finallyReason = m_comp->emit_define_local();
				auto noException = m_comp->emit_define_label();
				dec_stack();
				m_comp->emit_store_local(finallyReason);
				m_comp->emit_load_local(finallyReason);
				m_comp->emit_py_object(Py_None);
				m_comp->emit_branch(BranchEqual, noException);

				if (flags & EHF_BlockBreaks) {
					for (auto cur = m_blockStack.rbegin(); cur != m_blockStack.rend(); cur++) {
						if ((*cur).Kind == SETUP_LOOP) {
							m_comp->emit_load_local(finallyReason);
							m_comp->emit_int(EHF_BlockBreaks);
							m_comp->emit_branch(BranchEqual, getOffsetLabel((*cur).EndOffset));
							break;
						}
						else if ((*cur).Kind == SETUP_FINALLY) {
							// need to dispatch to outer finally...
							m_comp->emit_load_local(finallyReason);
							m_comp->emit_branch(BranchAlways, (*cur).ErrorTarget);
							break;
						}
					}
				}

				if (flags & EHF_BlockContinues) {
					for (auto cur = m_blockStack.rbegin(); cur != m_blockStack.rend(); cur++) {
						if (cur->Kind == SETUP_LOOP) {
							m_comp->emit_load_local(finallyReason);
							m_comp->emit_int(EHF_BlockContinues);
							m_comp->emit_branch(BranchEqual, getOffsetLabel(cur->ContinueOffset));
							break;
						}
						else if ((*cur).Kind == SETUP_FINALLY) {
							// need to dispatch to outer finally...
							m_comp->emit_load_local(finallyReason);
							m_comp->emit_branch(BranchAlways, cur->ErrorTarget);
							break;
						}
					}
				}

				if (flags & EHF_BlockReturns) {
					auto exceptional = m_comp->emit_define_label();
					m_comp->emit_load_local(finallyReason);
					m_comp->emit_int(EHF_BlockReturns);
					m_comp->emit_compare_equal();
					m_comp->emit_branch(BranchFalse, exceptional);

					bool hasOuterFinally = false;
					for (auto cur = m_blockStack.rbegin(); cur != m_blockStack.rend(); cur++) {
						if (cur->Kind == SETUP_FINALLY) {
							// need to dispatch to outer finally...
							m_comp->emit_load_local(finallyReason);
							m_comp->emit_branch(BranchAlways, cur->ErrorTarget);
							hasOuterFinally = true;
							break;
						}
					}
					if (!hasOuterFinally) {
						m_comp->emit_branch(BranchAlways, m_retLabel);
					}

					m_comp->emit_mark_label(exceptional);
				}

				// re-raise the exception...
				free_iter_locals_on_exception();

				m_comp->emit_restore_err(finallyReason);

				auto ehBlock = get_ehblock();

				clean_stack_for_reraise();

				m_comp->emit_branch(BranchAlways, get_ehblock().ReRaise);

				m_comp->emit_mark_label(noException);
				m_comp->emit_free_local(finallyReason);
			}
			else {
				// END_FINALLY is marking the EH rethrow.  The byte code branches
				// around this in the non-exceptional case.

				// If we haven't sent a branch to this END_FINALLY then we have
				// a bare try/except: which handles all exceptions.  In that case
				// we have no values to pop off, and this code will never be invoked
				// anyway.
				if (m_offsetStack.find(curByte) != m_offsetStack.end()) {
					dec_stack(3);
					free_iter_locals_on_exception();
					m_comp->emit_restore_err();

					clean_stack_for_reraise();

					m_comp->emit_branch(BranchAlways, get_ehblock().ReRaise);
				}
			}
		}
		break;

		case YIELD_FROM:
		case YIELD_VALUE:
			return nullptr;

		case IMPORT_NAME:
			m_comp->emit_import_name(PyTuple_GetItem(m_code->co_names, oparg));
			dec_stack(2);
			error_check();
			inc_stack();
			break;
		case IMPORT_FROM:
			m_comp->emit_import_from(PyTuple_GetItem(m_code->co_names, oparg));
			error_check();
			inc_stack();
			break;
		case IMPORT_STAR:
			m_comp->emit_import_star();
			dec_stack(1);
			int_error_check();
			break;
		case SETUP_WITH:
		case WITH_CLEANUP_START:
		case WITH_CLEANUP_FINISH:
			return nullptr;
		default:
#if _DEBUG
			printf("Unsupported opcode: %d (with related)\r\n", byte);
#endif
			return nullptr;
		}
	}


	// for each exception handler we need to load the exception
	// information onto the stack, and then branch to the correct
	// handler.  When we take an error we'll branch down to this
	// little stub and then back up to the correct handler.
	if (m_allHandlers.size() != 0) {
		for(auto& handler : m_allHandlers) {
			auto raiseAndFreeLabels = getRaiseAndFreeLabels(handler.BlockId);
			for (auto& curFree = raiseAndFreeLabels.rbegin(); curFree != raiseAndFreeLabels.rend(); curFree++) {
				m_comp->emit_mark_label(*curFree);
				m_comp->emit_pop_top();
			}

			m_comp->emit_mark_label(handler.Raise);

			m_comp->emit_eh_trace();

			m_comp->emit_mark_label(handler.ReRaise);

			m_comp->emit_prepare_exception(handler.ExVars.PrevExc, handler.ExVars.PrevExcVal, handler.ExVars.PrevTraceback, handler.Kind != SETUP_FINALLY);
			m_comp->emit_branch(BranchAlways, handler.ErrorTarget);
		}
	}

	// label we branch to for error handling when we have no EH handlers, return NULL.
	auto raiseAndFreeLabels = getRaiseAndFreeLabels(m_blockStack[0].BlockId);
	for (auto curLabel = raiseAndFreeLabels.rbegin(); curLabel != raiseAndFreeLabels.rend(); curLabel++) {
		m_comp->emit_mark_label(*curLabel);
		m_comp->emit_pop_top();
	}
	m_comp->emit_mark_label(raiseLabel);
	m_comp->emit_mark_label(reraiseLabel);

	m_comp->emit_null();
	auto finalRet = m_comp->emit_define_label();
	m_comp->emit_branch(BranchAlways, finalRet);

	m_comp->emit_mark_label(m_retLabel);
	m_comp->emit_load_local(m_retValue);

	m_comp->emit_mark_label(finalRet);
	m_comp->emit_pop_frame();

	m_comp->emit_check_function_result();
	m_comp->emit_ret();

	return m_comp->emit_compile();
}

void AbstractInterpreter::test_bool_and_branch(Local value, bool isTrue, Label target) {
	m_comp->emit_load_local(value);
	m_comp->emit_ptr(isTrue ? Py_False : Py_True);
	m_comp->emit_branch(BranchEqual, target);
}

void AbstractInterpreter::jump_if_or_pop(bool isTrue, int opcodeIndex, int jumpTo) {
	auto stackInfo = get_stack_info(opcodeIndex);
	auto one = stackInfo[stackInfo.size() - 1];

	auto target = getOffsetLabel(jumpTo);
	switch (one.Value->kind()) {
		case AVK_Float:
			m_comp->emit_dup();
			m_comp->emit_float(0);
			m_comp->emit_branch(isTrue ? BranchNotEqual : BranchEqual, target);
			m_comp->emit_pop_top();
			break;
		case AVK_Integer:
			m_comp->emit_dup();
			m_comp->emit_unary_not_tagged_int_push_bool();
			m_comp->emit_branch(isTrue ? BranchFalse : BranchTrue, target);
			m_comp->emit_pop_top();
			break;
		default:
			auto tmp = m_comp->emit_spill();
			auto noJump = m_comp->emit_define_label();
			auto willJump = m_comp->emit_define_label();
			
			// fast checks for true/false.
			test_bool_and_branch(tmp, isTrue, noJump);
			test_bool_and_branch(tmp, !isTrue, willJump);

			// Use PyObject_IsTrue
			m_comp->emit_load_local(tmp);
			m_comp->emit_is_true();
			m_comp->emit_branch(isTrue ? BranchFalse : BranchTrue, noJump);

			// Jumping, load the value back and jump
			m_comp->emit_mark_label(willJump);
			m_comp->emit_load_local(tmp);	// load the value back onto the stack
			m_comp->emit_branch(BranchAlways, target);

			// not jumping, load the value and dec ref it
			m_comp->emit_mark_label(noJump);
			m_comp->emit_load_local(tmp);
			m_comp->emit_pop_top();

			m_comp->emit_free_local(tmp);
			break;
	}

	m_offsetStack[jumpTo] = m_stack;
	dec_stack();
}

void AbstractInterpreter::pop_jump_if(bool isTrue, int opcodeIndex, int jumpTo) {
	auto stackInfo = get_stack_info(opcodeIndex);
	auto one = stackInfo[stackInfo.size() - 1];

	auto target = getOffsetLabel(jumpTo);
	switch (one.Value->kind()) {
		case AVK_Float:
			m_comp->emit_float(0);
			m_comp->emit_branch(isTrue ? BranchNotEqual : BranchEqual, target);
			break;
		case AVK_Integer:
			m_comp->emit_unary_not_tagged_int_push_bool();
			m_comp->emit_branch(isTrue ? BranchFalse : BranchTrue, target);
			break;
		default:
			auto noJump = m_comp->emit_define_label();
			auto willJump = m_comp->emit_define_label();
			
			// fast checks for true/false...
			m_comp->emit_dup();
			m_comp->emit_ptr(isTrue ? Py_False : Py_True);
			m_comp->emit_branch(BranchEqual, noJump);

			m_comp->emit_dup();
			m_comp->emit_ptr(isTrue ? Py_True : Py_False);
			m_comp->emit_branch(BranchEqual, willJump);

			// Use PyObject_IsTrue
			m_comp->emit_dup();
			m_comp->emit_is_true();
			m_comp->emit_branch(isTrue ? BranchFalse : BranchTrue, noJump);

			// Branching, pop the value and branch
			m_comp->emit_mark_label(willJump);
			m_comp->emit_pop_top();
			m_comp->emit_branch(BranchAlways, target);

			// Not branching, just pop the value and fall through
			m_comp->emit_mark_label(noJump);
			m_comp->emit_pop_top();
			break;
	}
	dec_stack();
	m_offsetStack[jumpTo] = m_stack;
}

void AbstractInterpreter::unary_positive(int opcodeIndex) {
	auto stackInfo = get_stack_info(opcodeIndex);
	auto one = stackInfo[stackInfo.size() - 1];

	switch (one.Value->kind()) {
		case AVK_Float:
		case AVK_Integer:
			// nop
			break;
		default:
			dec_stack();
			m_comp->emit_unary_positive();
			error_check();
			inc_stack();
			break;
	}
}

void AbstractInterpreter::unary_negative(int opcodeIndex) {
	auto stackInfo = get_stack_info(opcodeIndex);
	auto one = stackInfo[stackInfo.size() - 1];

	switch (one.Value->kind()) {
		case AVK_Float:
			dec_stack();
			m_comp->emit_unary_negative_float();
			inc_stack(1, STACK_KIND_VALUE);
			break;
		case AVK_Integer:
			dec_stack();
			m_comp->emit_unary_negative_tagged_int();
			inc_stack();
			break;
		default:
			dec_stack();
			m_comp->emit_unary_negative();
			error_check();
			inc_stack();
			break;
	}
}

void AbstractInterpreter::unary_not(int& opcodeIndex) {
	auto stackInfo = get_stack_info(opcodeIndex);
	auto one = stackInfo[stackInfo.size() - 1];

	if (m_byteCode[opcodeIndex + 1] == POP_JUMP_IF_TRUE || m_byteCode[opcodeIndex + 1] == POP_JUMP_IF_FALSE) {
		// optimizing away the unnecessary boxing and True/False comparisons
		dec_stack();
		switch (one.Value->kind()) {
			case AVK_Float:
				m_comp->emit_unary_not_float_push_bool();
				branch(opcodeIndex);
				break;
			case AVK_Integer:
				m_comp->emit_unary_not_tagged_int_push_bool();
				branch(opcodeIndex);
				break;
			default:
				m_comp->emit_unary_not_push_int();
				branch_or_error(opcodeIndex);
				break;
		}
	}
	else {
		// We don't know the consumer of this, produce an object
		dec_stack();
		switch (one.Value->kind()) {
			case AVK_Float:
				m_comp->emit_unary_not_float_push_bool();
				m_comp->emit_box_bool();
				break;
			case AVK_Integer:
				m_comp->emit_unary_not_tagged_int_push_bool();
				m_comp->emit_box_bool();
				break;
			default:
				m_comp->emit_unary_not();
				error_check();
				break;
		}
		inc_stack();
	}
}

JittedCode* AbstractInterpreter::compile() {
	bool interpreted = interpret();	
	preprocess();

	return compile_worker();
}

bool AbstractInterpreter::can_skip_lasti_update(int opcodeIndex) {
	switch (m_byteCode[opcodeIndex]) {
	case DUP_TOP:
	case SETUP_EXCEPT:
	case NOP:
	case ROT_TWO:
	case ROT_THREE:
	case POP_BLOCK:
	case POP_JUMP_IF_FALSE:
	case POP_JUMP_IF_TRUE:
	case POP_TOP:
	case DUP_TOP_TWO:
	case BREAK_LOOP:
	case CONTINUE_LOOP:
	case END_FINALLY:
	case LOAD_CONST:
	case JUMP_FORWARD:
	case STORE_FAST:
		return true;
	}

	return false;
}

void AbstractInterpreter::store_fast(int local, int opcodeIndex) {
	if (!should_box(opcodeIndex)) {
		auto stackInfo = get_stack_info(opcodeIndex);
		auto stackValue = stackInfo[stackInfo.size() - 1];
		// We only optimize floats so far...

		if (stackValue.Value->kind() == AVK_Float) {
			_ASSERTE(m_stack[m_stack.size() - 1] == STACK_KIND_VALUE);
			m_comp->emit_store_local(get_optimized_local(local, AVK_Float));
			dec_stack();
			return;
		}
		else if (stackValue.Value->kind() == AVK_Integer) {
			m_comp->emit_store_local(get_optimized_local(local, AVK_Any));
			dec_stack();
			return;
		}
	}

	_ASSERTE(m_stack[m_stack.size() - 1] == STACK_KIND_OBJECT);
	m_comp->emit_store_fast(local);
	dec_stack();
}

void AbstractInterpreter::load_const(int constIndex, int opcodeIndex) {
	auto constValue = PyTuple_GetItem(m_code->co_consts, constIndex);
	if (!should_box(opcodeIndex)) {
		if (PyFloat_CheckExact(constValue)) {
			m_comp->emit_float(PyFloat_AsDouble(constValue));
			inc_stack(1, STACK_KIND_VALUE);
			return;
		}
		else if (PyLong_CheckExact(constValue)) {
			int overflow;
			auto value = PyLong_AsLongLongAndOverflow(constValue, &overflow);
			if (!overflow && can_tag(value)) {
				m_comp->emit_tagged_int(value);
				inc_stack();
				return;
			}
		}
	}
	m_comp->emit_py_object(constValue);
	inc_stack();
}

void AbstractInterpreter::return_value(int opcodeIndex) {
	if (!should_box(opcodeIndex)) {
		// We need to box the value now...
		auto stackInfo = get_stack_info(opcodeIndex);
		// We only optimize floats so far...
		if (stackInfo[stackInfo.size() - 1].Value->kind() == AVK_Float) {
			_ASSERTE(m_stack[m_stack.size() - 1] == STACK_KIND_VALUE);

			// we need to convert the returned floating point value back into a boxed float.
			m_comp->emit_box_float();
		}
		else if (stackInfo[stackInfo.size() - 1].Value->kind() == AVK_Integer) {
			m_comp->emit_box_tagged_ptr();
		}
	}

	dec_stack();

	m_comp->emit_store_local(m_retValue);

	size_t clearEh = -1;
	bool inFinally = false;
	for (size_t blockIndex = m_blockStack.size() - 1; blockIndex != (-1); blockIndex--) {
		if (m_blockStack[blockIndex].Kind == SETUP_FINALLY) {
			// we need to run the finally before returning...
			m_blockStack.data()[blockIndex].Flags |= EHF_BlockReturns;

			if (!inFinally) {
				// Only emit the store and branch to the inner most finally, but
				// we need to mark all finallys as being capable of being returned
				// through.
				inFinally = true;
				if (clearEh != -1) {
					unwind_eh(m_blockStack[clearEh].ExVars);
				}
				free_all_iter_locals(blockIndex);
				m_comp->emit_int(EHF_BlockReturns);
				m_comp->emit_branch(BranchAlways, m_blockStack[blockIndex].ErrorTarget);
			}
		}
		else if (m_blockStack[blockIndex].Kind == POP_EXCEPT || m_blockStack[blockIndex].Kind == END_FINALLY) {
			// we need to restore the previous exception before we return
			if (clearEh == -1) {
				clearEh = blockIndex;
			}
		}
	}

	if (!inFinally) {
		if (clearEh != -1) {
			unwind_eh(m_blockStack[clearEh].ExVars);
		}
		free_all_iter_locals();
		m_comp->emit_branch(BranchLeave, m_retLabel);
	}
}


void AbstractInterpreter::unpack_sequence(size_t size, int opcode) {
	auto valueTmp = m_comp->emit_spill();
	dec_stack();

	auto success = m_comp->emit_define_label();
	m_comp->emit_unpack_sequence(valueTmp, m_sequenceLocals[opcode], success, size);

	branch_raise();

	m_comp->emit_mark_label(success);
	auto fastTmp = m_comp->emit_spill();

	// Equivalent to CPython's:
	//while (oparg--) {
	//	item = items[oparg];
	//	Py_INCREF(item);
	//	PUSH(item);
	//}

	auto tmpOpArg = size;
	while (tmpOpArg--) {
		m_comp->emit_load_local(fastTmp);
		m_comp->emit_load_array(tmpOpArg);
		inc_stack();
	}

	m_comp->emit_load_and_free_local(valueTmp);
	m_comp->emit_pop_top();

	m_comp->emit_free_local(fastTmp);
}

void AbstractInterpreter::for_iter(int loopIndex, int opcodeIndex, BlockInfo *loopInfo) {
	// CPython always generates LOAD_FAST or a GET_ITER before a FOR_ITER.
	// Therefore we know that we always fall into a FOR_ITER when it is
	// initialized, and we branch back to it for the loop condition.  We
	// do this becaues keeping the value on the stack becomes problematic.
	// At the very least it requires that we spill the value out when we
	// are doing a "continue" in a for loop.

	// oparg is where to jump on break
	auto iterValue = m_comp->emit_spill();
	dec_stack();
	//bool inLoop = false;
	//Local loopOpt1, loopOpt2;
	if (loopInfo != nullptr) {
		loopInfo->LoopVar = iterValue;
	}

	// now that we've saved the value into a temp we can mark the offset
	// label.
	mark_offset_label(opcodeIndex);	// minus 2 removes our oparg

	m_comp->emit_load_local(iterValue);

	// TODO: It'd be nice to inline this...
	auto processValue = m_comp->emit_define_label();

	m_comp->emit_for_next(processValue, iterValue);

	int_error_check();

	jump_absolute(loopIndex);

	m_comp->emit_mark_label(processValue);
	inc_stack();
}

void AbstractInterpreter::compare_op(int compareType, int& i, int opcodeIndex) {
	switch (compareType) {
	case PyCmp_IS:
	case PyCmp_IS_NOT:
		//	TODO: Inlining this would be nice, but then we need the dec refs, e.g.:
		if (m_byteCode[i + 1] == POP_JUMP_IF_TRUE || m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
			m_comp->emit_is_push_int(compareType != PyCmp_IS);
			dec_stack(); // popped 2, pushed 1
			branch(i);
		}
		else {
			m_comp->emit_is(compareType != PyCmp_IS);
			dec_stack();
		}
		break;
	case PyCmp_IN:
		if (m_byteCode[i + 1] == POP_JUMP_IF_TRUE || m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
			m_comp->emit_in_push_int();
			dec_stack(2);
			branch_or_error(i);
		}
		else {
			m_comp->emit_in();
			dec_stack(2);
			error_check();
			inc_stack();
		}
		break;
	case PyCmp_NOT_IN:
		if (m_byteCode[i + 1] == POP_JUMP_IF_TRUE || m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
			m_comp->emit_not_in_push_int();
			dec_stack(2);
			branch_or_error(i);
		}
		else {
			m_comp->emit_not_in();
			dec_stack(2);
			error_check();
			inc_stack();
		}
		break;
	case PyCmp_EXC_MATCH:
		if (m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
			m_comp->emit_compare_exceptions_int();
			dec_stack(2);
			branch_or_error(i);
		}
		else {
			// this will actually not currently be reached due to the way
			// CPython generates code, but is left for completeness.
			m_comp->emit_compare_exceptions();
			dec_stack(2);
			error_check();
			inc_stack();
		}
		break;
	default:
		if (!should_box(opcodeIndex)) {
			auto stackInfo = get_stack_info(opcodeIndex);
			// We only optimize floats so far...
			if (stackInfo[stackInfo.size() - 1].Value->kind() == AVK_Float &&
				stackInfo[stackInfo.size() - 2].Value->kind() == AVK_Float) {

				_ASSERTE(m_stack[m_stack.size() - 1] == STACK_KIND_VALUE);
				_ASSERTE(m_stack[m_stack.size() - 2] == STACK_KIND_VALUE);

				m_comp->emit_compare_float(compareType);
				dec_stack();

				if (m_byteCode[i + 1] == POP_JUMP_IF_TRUE || m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
					branch(i);
				}
				else {
					// push Python bool onto the stack
					m_comp->emit_box_bool();
				}
				return;
			}
			else if (stackInfo[stackInfo.size() - 1].Value->kind() == AVK_Integer &&
				stackInfo[stackInfo.size() - 2].Value->kind() == AVK_Integer) {

				m_comp->emit_compare_tagged_int(compareType);
				dec_stack();

				if (m_byteCode[i + 1] == POP_JUMP_IF_TRUE || m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
					branch(i);
				}
				else {
					// push Python bool onto the stack
					m_comp->emit_box_bool();
				}
				return;
			}
		}

		bool generated = false;
		if (m_byteCode[i + 1] == POP_JUMP_IF_TRUE || m_byteCode[i + 1] == POP_JUMP_IF_FALSE) {
			generated = m_comp->emit_compare_object_push_int(compareType);
			if (generated) {
				dec_stack(2);
				branch_or_error(i);
			}
		}

		if (!generated) {
			m_comp->emit_compare_object(compareType);
			dec_stack(2);
			error_check();
			inc_stack();
		}
		break;
	}
}

void AbstractInterpreter::load_fast(int local, int opcodeIndex) {
	if (!should_box(opcodeIndex)) {
		// We have an optimized local...
		auto localInfo = get_local_info(opcodeIndex, local);
		auto kind = localInfo.ValueInfo.Value->kind();
		// We only optimize floats so far...
		if (kind == AVK_Float) {
			m_comp->emit_load_local(get_optimized_local(local, AVK_Float));
			inc_stack(1, STACK_KIND_VALUE);
			return;
		}
		else if (kind == AVK_Integer) {
			m_comp->emit_load_local(get_optimized_local(local, AVK_Any));
			inc_stack();
			return;
		}
	}

	bool checkUnbound = m_assignmentState.find(local) == m_assignmentState.end() || !m_assignmentState.find(local)->second;
	load_fast_worker(local, checkUnbound);
	inc_stack();
}

void AbstractInterpreter::load_fast_worker(int local, bool checkUnbound) {
	m_comp->emit_load_fast(local);

	if (checkUnbound) {
		Label success = m_comp->emit_define_label();

		m_comp->emit_unbound_local_check(local, success);
		branch_raise();

		m_comp->emit_mark_label(success);
	}
}

void AbstractInterpreter::unpack_ex(size_t size, int opcode) {
	auto valueTmp = m_comp->emit_spill();
	auto listTmp = m_comp->emit_define_local();
	auto remainderTmp = m_comp->emit_define_local();

	dec_stack();

	m_comp->emit_unpack_ex(valueTmp, size & 0xff, size >> 8, m_sequenceLocals[opcode], listTmp, remainderTmp);
	// load the iterable, the sizes, and our temporary 
	// storage if we need to iterate over the object, 
	// the list local address, and the remainder address
	// PyObject* seq, size_t leftSize, size_t rightSize, PyObject** tempStorage, PyObject** list, PyObject*** remainder

	error_check(); // TODO: We leak the sequence on failure

	auto fastTmp = m_comp->emit_spill();

	// load the right hand side...
	auto tmpOpArg = size >> 8;
	while (tmpOpArg--) {
		m_comp->emit_load_local(remainderTmp);
		m_comp->emit_load_array(tmpOpArg);
		inc_stack();
	}

	// load the list
	m_comp->emit_load_and_free_local(listTmp);
	inc_stack();
	// load the left hand side, Equivalent to CPython's:
	//while (oparg--) {
	//	item = items[oparg];
	//	Py_INCREF(item);
	//	PUSH(item);
	//}

	tmpOpArg = size & 0xff;
	while (tmpOpArg--) {
		m_comp->emit_load_local(fastTmp);
		m_comp->emit_load_array(tmpOpArg);
		inc_stack();
	}

	m_comp->emit_load_and_free_local(valueTmp);
	m_comp->emit_pop_top();

	m_comp->emit_free_local(fastTmp);
	m_comp->emit_free_local(remainderTmp);
}

void AbstractInterpreter::jump_absolute(int index) {
	m_offsetStack[index] = m_stack;
	m_comp->emit_branch(BranchAlways, getOffsetLabel(index));
}

void AbstractInterpreter::unwind_eh(ExceptionVars& exVars) {
	m_comp->emit_unwind_eh(exVars.PrevExc, exVars.PrevExcVal, exVars.PrevTraceback);
}

BlockInfo AbstractInterpreter::get_ehblock() {
	for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
		if (m_blockStack[i].Kind != SETUP_LOOP) {
			return m_blockStack.data()[i];
		}
	}
	assert(0);
	return BlockInfo();
}

void AbstractInterpreter::mark_offset_label(int index) {
	auto existingLabel = m_offsetLabels.find(index);
	if (existingLabel != m_offsetLabels.end()) {
		m_comp->emit_mark_label(existingLabel->second);
	}
	else {
		auto label = m_comp->emit_define_label();
		m_offsetLabels[index] = label;
		m_comp->emit_mark_label(label);
	}
}

LocalKind get_optimized_local_kind(AbstractValueKind kind) {
	switch (kind) {
	case AVK_Float:
		return LK_Float;
	}
	return LK_Pointer;
}

Local AbstractInterpreter::get_optimized_local(int index, AbstractValueKind kind) {

	if (m_optLocals.find(index) == m_optLocals.end()) {
		m_optLocals[index] = unordered_map<AbstractValueKind, Local>();
	}
	auto& map = m_optLocals.find(index)->second;
	if (map.find(kind) == map.end()) {
		return map[kind] = m_comp->emit_define_local(get_optimized_local_kind(kind));
	}
	return map.find(kind)->second;
}


void AbstractInterpreter::pop_except() {
	// we made it to the end of an EH block w/o throwing,
	// clear the exception.
	m_comp->emit_clear_eh();
	auto block = m_blockStack.back();
	//m_blockStack.pop_back();
	unwind_eh(block.ExVars);
#ifdef DEBUG_TRACE
	m_il.ld_i("Exception cleared");
	m_il.emit_call(METHOD_DEBUG_TRACE);
#endif
}

EhFlags operator | (EhFlags lhs, EhFlags rhs) {
	return (EhFlags)(static_cast<int>(lhs) | static_cast<int>(rhs));
}

EhFlags operator |= (EhFlags& lhs, EhFlags rhs) {
	lhs = (EhFlags)(static_cast<int>(lhs) | static_cast<int>(rhs));
	return lhs;
}
