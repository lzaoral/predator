/*
 * Copyright (C) 2012 Kamil Dudka <kdudka@redhat.com>
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../config_cl.h"
#include "../util.hh"

#include <cl/cl_msg.hh>
#include <cl/clutil.hh>
#include <cl/easy.hh>
#include <cl/storage.hh>

#include <map>
#include <set>

#include <boost/foreach.hpp>

// required by the gcc plug-in API
extern "C" { int plugin_is_GPL_compatible; }

typedef const CodeStorage::Fnc             *TFnc;
typedef const CodeStorage::Block           *TBlock;
typedef const CodeStorage::Insn            *TInsn;
typedef const struct cl_loc                *TLoc;
typedef const struct cl_operand            &TOp;
typedef CodeStorage::TKillVarList           TKillList;
typedef std::set<TBlock>                    TBlockSet;
typedef std::set<int /* uid */>             TState;
typedef std::map<TBlock, TState>            TStateMap;

struct PerFncData {
    const TFnc                      fnc;
    TStateMap                       stateMap;
    TBlockSet                       todo;

    PerFncData(const TFnc fnc_):
        fnc(fnc_)
    {
    }
};

bool chkAssert(
        const TInsn                 insn,
        const TState               &state,
        const char                 *name)
{
    const TLoc loc = &insn->loc;

    const CodeStorage::TOperandList &opList = insn->operands;
    if (opList.size() < /* ret + fnc + state + op0 */ 4) {
        CL_ERROR_MSG(loc, name << ": missing operand");
        return false;
    }

    const bool live = intCstFromOperand(&opList[/* state */ 2]);

    for (unsigned i = /* op0 */ 3; i < opList.size(); ++i) {
        const unsigned cnt = i - 2;

        TOp op = opList[i];
        if (!isLcVar(op)) {
            CL_ERROR_MSG(loc, name << ": invalid operand #" << cnt);
            continue;
        }

        const char *varName = NULL;
        const int uid = varIdFromOperand(&op, &varName);
        if (hasKey(state, uid) == live)
            // matched
            continue;

        const char *status = (live)
            ? "VK_LIVE"
            : "VK_DEAD";

        CL_ERROR_MSG(loc, name << ": property violated: "
                << status << ": " << varName);
    }

    // built-in handled
    return true;
}

bool handleBuiltIn(
        const TInsn                 insn,
        const TState               &state)
{
    if (CL_INSN_CALL != insn->code)
        // not a function call
        return false;

    const char *name;
    if (!fncNameFromCst(&name, &insn->operands[/* fnc */ 1]))
        // indirect function call?
        return false;

    if (STREQ("VK_ASSERT", name))
        return chkAssert(insn, state, name);

    // no built-in matched
    return false;
}

void killVars(TState &state, const TInsn insn, const TKillList &kList) {
    BOOST_FOREACH(const CodeStorage::KillVar &kv, kList) {
        if (kv.onlyIfNotPointed)
            // TODO: try all possibilities?
            continue;

        const int uid = kv.uid;
        if (1 == state.erase(uid))
            // successfully killed a variable
            continue;

        CL_DEBUG_MSG(&insn->loc, "attempt to kill a dead variable: "
                << varToString(*insn->stor, uid));
    }
}

void handleNontermInsn(
        PerFncData                 &data,
        const TInsn                 insn,
        const TState               &origin)
{
    const CodeStorage::TTargetList &tList = insn->targets;
    for (unsigned target = 0; target < tList.size(); ++target) {
        // kill variables per-target
        TState state(origin);
        killVars(state, insn, insn->killPerTarget[target]);

        // resolve the target block
        const TBlock bb = tList[target];
        TState &dst = data.stateMap[bb];

        // update the state in the target block
        const unsigned lastSize = dst.size();
        dst.insert(state.begin(), state.end());

        if (lastSize != dst.size())
            // schedule the _target_ block for processing
            data.todo.insert(bb);
    }
}

void updateBlock(PerFncData &data, const TBlock bb) {
    TState state(data.stateMap[bb]);

    BOOST_FOREACH(const TInsn insn, *bb) {
        if (cl_is_term_insn(insn->code)) {
            handleNontermInsn(data, insn, state);
            return;
        }

        if (handleBuiltIn(insn, state))
            // handled as a built-in function
            continue;

        // first mark all local variables used by this insn as live
        BOOST_FOREACH(TOp op, insn->operands) {
            if (!isLcVar(op))
                // not a local variable
                continue;

            const int uid = varIdFromOperand(&op);
            state.insert(uid);
        }

        // then kill all variables suggested by varKiller
        killVars(state, insn, insn->varsToKill);
    }
}

void chkFunction(const TFnc fnc) {
    PerFncData data(fnc);

    // start with the entrance basic block
    const TBlock entry = fnc->cfg.entry();
    TState &state = data.stateMap[entry];
    BOOST_FOREACH(const int uid, data.fnc->args)
        state.insert(uid);

    // schedule the entry block for processing
    TBlockSet &todo = data.todo;
    todo.insert(entry);

    // fixed-point computation
    CL_DEBUG("computing a fixed-point for " << nameOf(*fnc) << "()");

    unsigned cntSteps = 1;
    while (!todo.empty()) {
        TBlockSet::iterator i = todo.begin();
        TBlock bb = *i;
        todo.erase(i);

        // (re)compute a single basic block
        updateBlock(data, bb);
        ++cntSteps;
    }

    CL_DEBUG("fixed-point for " << nameOf(*fnc) << "() reached in "
            << cntSteps << " steps");
}

void clEasyRun(const CodeStorage::Storage &stor, const char *) {
    // go through all _defined_ functions
    BOOST_FOREACH(const TFnc fnc, stor.fncs)
        if (isDefined(*fnc))
            chkFunction(fnc);
}
