/*
 * Copyright (C) 2010 Kamil Dudka <kdudka@redhat.com>
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

#include "config.h"
#include "symseg.hh"

#include <cl/cl_msg.hh>

#include "prototype.hh"
#include "symheap.hh"
#include "symutil.hh"

#include <boost/foreach.hpp>

bool segProveNeq(const SymHeap &sh, TValId ref, TValId val)
{
    if (proveNeq(sh, ref, val))
        // values are non-equal in non-abstract world
        return true;

    // collect the sets of values we get by jumping over 0+ abstract objects
    TValSet seen1, seen2;
    if (VAL_INVALID == lookThrough(sh, ref, &seen1))
        return false;
    if (VAL_INVALID == lookThrough(sh, val, &seen2))
        return false;

    // try to look through possibly empty abstract objects
    ref = lookThrough(sh, ref, &seen2);
    val = lookThrough(sh, val, &seen1);
    if (ref == val)
        return false;

    if (proveNeq(sh, ref, val))
        // values are non-equal in non-abstract world
        return true;

    // having the values always in the same order leads to simpler code
    moveKnownValueToLeft(sh, ref, val);

    const TSizeRange size2 = valSizeOfTarget(sh, val);
    if (size2.lo <= IR::Int0)
        // oops, we cannot prove the address is safely allocated, giving up
        return false;

    const TValId root2 = sh.valRoot(val);
    const TMinLen len2 = objMinLength(sh, root2);
    if (!len2)
        // one of the targets is possibly empty, giving up
        return false;

    if (VAL_NULL == ref)
        // one of them is VAL_NULL the other one is address of non-empty object
        return true;

    const TSizeRange size1 = valSizeOfTarget(sh, ref);
    if (size1.lo <= IR::Int0)
        // oops, we cannot prove the address is safely allocated, giving up
        return false;

    const TValId root1 = sh.valRoot(ref);
    const TMinLen len1 = objMinLength(sh, root1);
    if (!len1)
        // both targets are possibly empty, giving up
        return false;

    if (!isAbstractValue(sh, ref))
        // non-empty abstract object vs. concrete object
        return true;

    if (root2 != segPeer(sh, root1))
        // a pair of non-empty abstract objects
        return true;

    // one value points at segment and the other points at its peer
    CL_BREAK_IF(len1 != len2);
    return (1 < len1);
}

bool haveSeg(
        const SymHeap               &sh,
        const TValId                 atAddr,
        const TValId                 pointingTo,
        const EObjKind               kind)
{
    if (!isAbstractValue(sh, atAddr))
        // not an abstract object
        return false;

    TValId seg = sh.valRoot(atAddr);
    if (kind != sh.objKind(sh.objByAddr(seg)))
        // kind mismatch
        return false;

    if (OK_DLS == kind) {
        seg = dlSegPeer(sh, seg);
        if (OK_DLS != sh.objKind(sh.objByAddr(seg)))
            // invalid peer
            return false;
    }

    // compare the end-points
    SymHeap &writable = const_cast<SymHeap &>(sh);
    const TValId valNext = nextValFromSeg(writable, seg);
    return (valNext == pointingTo);
}

bool haveDlSegAt(const SymHeap &sh, TValId atAddr, TValId peerAddr)
{
    if (atAddr <= 0 || peerAddr <= 0)
        // no valid targets
        return false;

    if (!isAbstractValue(sh, atAddr) || !isAbstractValue(sh, peerAddr))
        // not abstract objects
        return false;

    const TValId seg = sh.valRoot(atAddr);
    if (OK_DLS != sh.objKind(sh.objByAddr(seg)))
        // not a DLS
        return false;

    const TValId peer = dlSegPeer(sh, seg);
    if (OK_DLS != sh.objKind(sh.objByAddr(peer)))
        // invalid peer
        return false;

    // compare the end-points
    return (segHeadAt(sh, peer) == peerAddr);
}

bool haveSegBidir(
        TValId                     *pDst,
        const SymHeap              &sh,
        const EObjKind              kind,
        const TValId                v1,
        const TValId                v2)
{
    if (haveSeg(sh, v1, v2, kind)) {
        *pDst = sh.valRoot(v1);
        return true;
    }

    if (haveSeg(sh, v2, v1, kind)) {
        *pDst = sh.valRoot(v2);
        return true;
    }

    // found nothing
    return false;
}

bool segApplyNeq(SymHeap &sh, TValId v1, TValId v2)
{
    if (!isAbstractValue(sh, v1) && !isAbstractValue(sh, v2))
        // no abstract objects involved
        return false;

    if (VAL_NULL == v1 && !sh.valOffset(v2))
        v1 = segNextRootObj(sh, v2);
    if (VAL_NULL == v2 && !sh.valOffset(v1))
        v2 = segNextRootObj(sh, v1);

    TValId seg;
    if (haveSegBidir(&seg, sh, OK_OBJ_OR_NULL, v1, v2)
            || haveSegBidir(&seg, sh, OK_SEE_THROUGH, v1, v2)
            || haveSegBidir(&seg, sh, OK_SEE_THROUGH_2N, v1, v2))
    {
        // replace OK_SEE_THROUGH/OK_OBJ_OR_NULL by OK_CONCRETE
        decrementProtoLevel(sh, seg);
        sh.objSetConcrete(sh.objByAddr(seg));
        return true;
    }

    if (haveSegBidir(&seg, sh, OK_SLS, v1, v2)) {
        segIncreaseMinLength(sh, seg, /* SLS 1+ */ 1);
        return true;
    }

    if (haveSegBidir(&seg, sh, OK_DLS, v1, v2)) {
        segIncreaseMinLength(sh, seg, /* DLS 1+ */ 1);
        return true;
    }

    if (haveDlSegAt(sh, v1, v2)) {
        segIncreaseMinLength(sh, v1, /* DLS 2+ */ 2);
        return true;
    }

    // fallback to explicit Neq predicate
    return false;
}

TValId segClone(SymHeap &sh, const TValId root)
{
    const TValId dup = objClone(sh, root);

    if (OK_DLS == sh.objKind(sh.objByAddr(root))) {
        // we need to clone the peer as well
        const TValId peer = dlSegPeer(sh, root);
        const TValId dupPeer = sh.valClone(peer);

        // dig the 'peer' selectors of the cloned objects
        const TOffset offpSeg  = sh.segBinding(sh.objByAddr(dup)).prev;
        const TOffset offpPeer = sh.segBinding(sh.objByAddr(dupPeer)).prev;

        // resolve selectors -> sub-objects
        const PtrHandle ppSeg (sh, sh.valByOffset(dup, offpSeg));
        const PtrHandle ppPeer(sh, sh.valByOffset(dupPeer, offpPeer));

        // now cross the 'peer' pointers
        ppSeg .setValue(segHeadAt(sh, dupPeer));
        ppPeer.setValue(segHeadAt(sh, dup));
    }

    return dup;
}

TValId lookThrough(const SymHeap &sh, TValId val, TValSet *pSeen)
{
    if (VT_RANGE == sh.valTarget(val))
        // not supported yet
        return VAL_INVALID;

    const TOffset off = sh.valOffset(val);

    while (0 < val) {
        if (pSeen && !insertOnce(*pSeen, val))
            // an already seen value
            break;

        if (!isAbstractValue(sh, val))
            // a non-abstract object reached
            break;

        const TValId root = sh.valRoot(val);
        const TValId segAt = segPeer(sh, root);
        const TObjId seg = sh.objByAddr(segAt);

        if (sh.segMinLength(seg))
            // non-empty abstract object reached
            break;

        const EObjKind kind = sh.objKind(seg);
        if (OK_OBJ_OR_NULL == kind) {
            // we always end up with VAL_NULL if OK_OBJ_OR_NULL is removed
            val = VAL_NULL;
            continue;
        }

        // when computing the actual shift, take the head offset into account
        const BindingOff &bOff = sh.segBinding(seg);
        const TOffset shiftBy = off - bOff.head;

        if (root != segAt) {
            // put the shifted address of DLS peer to the list of seen values
            const FldHandle ptrPrev = prevPtrFromSeg(sh, root);
            const TValId valPrev = ptrPrev.value();
            val = const_cast<SymHeap &>(sh).valByOffset(valPrev, shiftBy);
        }

        // jump to next value
        const TValId valNext = nextValFromSeg(sh, segAt);
        val = const_cast<SymHeap &>(sh).valByOffset(valNext, shiftBy);
    }

    return val;
}

bool dlSegCheckConsistency(const SymHeap &sh)
{
    TObjList objs;
    sh.gatherObjects(objs, isOnHeap);
    BOOST_FOREACH(const TObjId seg, objs) {
        if (OK_DLS != sh.objKind(seg))
            // we are interested in OK_DLS here
            continue;

        // TODO: drop this!
        const TValId at = sh.legacyAddrOfAny_XXX(seg);

        if (at <= VAL_NULL) {
            CL_ERROR("OK_DLS with invalid address detected");
            return false;
        }

        const TValId peerAt = dlSegPeer(sh, at);
        if (peerAt <= VAL_NULL) {
            CL_ERROR("OK_DLS with invalid peer detected");
            return false;
        }

        const TObjId peer = sh.objByAddr(peerAt);
        if (OK_DLS != sh.objKind(peer)) {
            CL_ERROR("DLS peer not a DLS");
            return false;
        }

        // check the consistency of Neq predicates
        const TMinLen len1 = sh.segMinLength(seg);
        const TMinLen len2 = sh.segMinLength(peer);
        if (len1 != len2) {
            CL_ERROR("peer of a DLS " << len1 << "+ is a DLS" << len2 << "+");
            return false;
        }
    }

    // all OK
    return true;
}
