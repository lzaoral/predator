/*
 * Copyright (C) 2009-2011 Kamil Dudka <kdudka@redhat.com>
 * Copyright (C) 2010 Petr Peringer, FIT
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
#include "symheap.hh"

#include <cl/cl_msg.hh>
#include <cl/clutil.hh>
#include <cl/storage.hh>

#include "symabstract.hh"
#include "symdump.hh"
#include "symseg.hh"
#include "symutil.hh"
#include "util.hh"
#include "worklist.hh"

#include <algorithm>
#include <map>
#include <set>
#include <stack>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>

#ifdef NDEBUG
    // aggressive optimization
#   define DCAST static_cast
#else
#   define DCAST dynamic_cast
#endif

// /////////////////////////////////////////////////////////////////////////////
// Neq predicates store
class NeqDb {
    private:
        typedef std::pair<TValId /* valLt */, TValId /* valGt */> TItem;
        typedef std::set<TItem> TCont;
        TCont cont_;

    public:
        bool areNeq(TValId valLt, TValId valGt) const {
            sortValues(valLt, valGt);
            TItem item(valLt, valGt);
            return hasKey(cont_, item);
        }
        void add(TValId valLt, TValId valGt) {
            CL_BREAK_IF(valLt == valGt);

            sortValues(valLt, valGt);
            TItem item(valLt, valGt);
            cont_.insert(item);
        }
        void del(TValId valLt, TValId valGt) {
            CL_BREAK_IF(valLt == valGt);

            sortValues(valLt, valGt);
            TItem item(valLt, valGt);
            cont_.erase(item);
        }

        bool empty() const {
            return cont_.empty();
        }

        template <class TDst>
        void gatherRelatedValues(TDst &dst, TValId val) const {
            // FIXME: suboptimal due to performance
            BOOST_FOREACH(const TItem &item, cont_) {
                if (item.first == val)
                    dst.push_back(item.second);
                else if (item.second == val)
                    dst.push_back(item.first);
            }
        }

        friend void SymHeapCore::copyRelevantPreds(SymHeapCore &dst,
                                                   const SymHeapCore::TValMap &)
                                                   const;

        friend bool SymHeapCore::matchPreds(const SymHeapCore &,
                                            const SymHeapCore::TValMap &)
                                            const;
};

// /////////////////////////////////////////////////////////////////////////////
// CVar lookup container
class CVarMap {
    private:
        typedef std::map<CVar, TValId>              TCont;
        TCont                                       cont_;

    public:
        void insert(CVar cVar, TValId val) {
#ifndef NDEBUG
            const unsigned last = cont_.size();
#endif
            cont_[cVar] = val;

            // check for mapping redefinition
            CL_BREAK_IF(last == cont_.size());
        }

        void remove(CVar cVar) {
#ifndef NDEBUG
            if (1 != cont_.erase(cVar))
                // *** offset detected ***
                CL_TRAP;
#else
            cont_.erase(cVar);
#endif
        }

        TValId find(const CVar &cVar) {
            // regular lookup
            TCont::iterator iter = cont_.find(cVar);
            const bool found = (cont_.end() != iter);
            if (!cVar.inst) {
                // gl variable explicitly requested
                return (found)
                    ? iter->second
                    : VAL_INVALID;
            }

            // automatic fallback to gl variable
            CVar gl = cVar;
            gl.inst = /* global variable */ 0;
            TCont::iterator iterGl = cont_.find(gl);
            const bool foundGl = (cont_.end() != iterGl);

            if (!found && !foundGl)
                // not found anywhere
                return VAL_INVALID;

            // check for clash on uid among lc/gl variable
            CL_BREAK_IF(found && foundGl);

            if (found)
                return iter->second;
            else /* if (foundGl) */
                return iterGl->second;
        }

        template <class TDst>
        void getAll(TDst &dst) {
            BOOST_FOREACH(const TCont::value_type &item, cont_) {
                dst.push_back(item.first);
            }
        }
};

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeapCore
typedef std::set<TObjId>                                TUsedBy;
typedef std::map<TOffset, TValId>                       TOffMap;
typedef std::map<TObjType, TObjId>                      TObjByType;
typedef std::map<TOffset, TObjByType>                   TGrid;
typedef std::map<TObjId, bool /* isPtr */>              TLiveObjs;
typedef std::map<int, TValId>                           TCValueMap;

struct IHeapEntity {
    virtual ~IHeapEntity() { }
    virtual IHeapEntity* clone() const = 0;
};

struct HeapObject: public IHeapEntity {
    TValId                      value;
    TValId                      root;
    TOffset                     off;
    TObjType                    clt;

    HeapObject():
        value(VAL_INVALID),
        root(VAL_INVALID),
        off(0),
        clt(0)
    {
    }

    virtual IHeapEntity* clone() const {
        return new HeapObject(*this);
    }
};

// FIXME: really good idea to define this as a value type?
struct BaseValue: public IHeapEntity {
    EValueTarget                    code;
    EValueOrigin                    origin;
    TOffset                         offRoot;
    TUsedBy                         usedBy;

    BaseValue(EValueTarget code_, EValueOrigin origin_):
        code(code_),
        origin(origin_),
        offRoot(0)
    {
    }

    virtual BaseValue* clone() const {
        return new BaseValue(*this);
    }
};

struct CompValue: public BaseValue {
    TObjId                          compObj;

    CompValue(EValueTarget code_, EValueOrigin origin_):
        BaseValue(code_, origin_)
    {
    }

    virtual BaseValue* clone() const {
        return new CompValue(*this);
    }
};

struct CustomValue: public BaseValue {
    int                             customData;

    CustomValue(EValueTarget code_, EValueOrigin origin_):
        BaseValue(code_, origin_)
    {
    }

    virtual BaseValue* clone() const {
        return new CustomValue(*this);
    }
};

struct OffValue: public BaseValue {
    TValId                          root;

    OffValue(
            EValueTarget            code_,
            EValueOrigin            origin_,
            TValId                  root_,
            TOffset                 offRoot_):
        BaseValue(code_, origin_),
        root(root_)
    {
        offRoot = offRoot_;
    }

    virtual BaseValue* clone() const {
        return new OffValue(*this);
    }
};

struct RootValue: public BaseValue {
    TOffMap                         offMap;
    TValId                          addr;
    TObjType                        lastKnownClt;
    unsigned                        cbSize;
    CVar                            cVar;
    TObjList          /* XXX */     allObjs;
    TLiveObjs                       liveObjs;
    bool                            isProto;
    TUsedBy                         usedByGl;
    TGrid                           grid;

    RootValue(EValueTarget code_, EValueOrigin origin_):
        BaseValue(code_, origin_),
        addr(VAL_NULL),
        lastKnownClt(0),
        cbSize(0),
        isProto(false)
    {
    }

    virtual BaseValue* clone() const {
        return new RootValue(*this);
    }
};

struct SymHeapCore::Private {
    // allocate VAL_NULL (TODO: avoid allocation of VAL_NULL) and OBJ_RETURN
    Private();

    // clone heap entities, they are now allocated separately
    Private(const Private &);

    // delete heap entities, they are now allocated separately
    ~Private();

    CVarMap                         cVarMap;
    TCValueMap                      cValueMap;
    std::vector<IHeapEntity *>      ents;
    std::set<TValId>                liveRoots;
    NeqDb                           neqDb;

    template <typename T> T lastId() const;
    template <typename T> T nextId() const;

    inline bool valOutOfRange(TValId);
    inline bool objOutOfRange(TObjId);

    HeapObject* objData(const TObjId);
    BaseValue* valData(const TValId);
    RootValue* rootData(const TValId);

    inline TValId valRoot(const TValId, const IHeapEntity *);
    inline TValId valRoot(const TValId);

    TValId valCreate(EValueTarget code, EValueOrigin origin);

    TValId valDup(TValId);
    TObjId objCreate();
    TValId objDup(TValId root);
    void objDestroy(TValId obj);

    void releaseValueOf(TObjId obj, TValId val);
    void setValueOf(TObjId of, TValId val);

    void subsCreate(TValId root);

    bool gridLookup(TObjId *pFailCode, const TObjByType **pRow, const TValId);
    void neqOpWrap(SymHeap::ENeqOp, TValId, TValId);

    private:
        // intentionally not implemented
        Private& operator=(const Private &);
};

template <typename T> T SymHeapCore::Private::nextId() const {
    return static_cast<T>(this->ents.size());
}

template <typename T> T SymHeapCore::Private::lastId() const {
    return static_cast<T>(this->ents.size() - 1);
}

inline bool SymHeapCore::Private::valOutOfRange(TValId val) {
    return (val <= 0)
        || (this->lastId<TValId>() < val);
}

inline bool SymHeapCore::Private::objOutOfRange(TObjId obj) {
    return (obj < 0)
        || (this->lastId<TObjId>() < obj);
}

inline HeapObject* SymHeapCore::Private::objData(const TObjId obj) {
    CL_BREAK_IF(objOutOfRange(obj));
    IHeapEntity *ent = this->ents[obj];
    return DCAST<HeapObject *>(ent);
}

inline BaseValue* SymHeapCore::Private::valData(const TValId val) {
    CL_BREAK_IF(valOutOfRange(val));
    IHeapEntity *ent = this->ents[val];
    return DCAST<BaseValue *>(ent);
}

inline RootValue* SymHeapCore::Private::rootData(const TValId val) {
    CL_BREAK_IF(valOutOfRange(val));
    IHeapEntity *ent = this->ents[val];
    return DCAST<RootValue *>(ent);
}

inline TValId SymHeapCore::Private::valRoot(
        const TValId                val,
        const IHeapEntity          *ent)
{
    const BaseValue *valData = DCAST<const BaseValue *>(ent);
    if (!valData->offRoot)
        return val;

    const TValId valRoot = DCAST<const OffValue *>(valData)->root;
    CL_BREAK_IF(valOutOfRange(valRoot));
    return valRoot;
}

inline TValId SymHeapCore::Private::valRoot(const TValId val) {
    return this->valRoot(val, this->valData(val));
}

void SymHeapCore::Private::releaseValueOf(TObjId obj, TValId val) {
    if (val <= 0)
        // we do not track uses of special values
        return;

    BaseValue *valData = this->valData(val);
    if (1 != valData->usedBy.erase(obj))
        CL_BREAK_IF("SymHeapCore::Private::releaseValueOf(): offset detected");

    const TValId root = this->valRoot(val, valData);
    valData = this->valData(root);
    if (!isPossibleToDeref(valData->code))
        return;

    RootValue *rootData = DCAST<RootValue *>(valData);
    if (1 != rootData->usedByGl.erase(obj))
        CL_BREAK_IF("SymHeapCore::Private::releaseValueOf(): offset detected");
}

void SymHeapCore::Private::setValueOf(TObjId obj, TValId val) {
    // release old value
    HeapObject *objData = DCAST<HeapObject *>(this->ents[obj]);
    this->releaseValueOf(obj, objData->value);

    // store new value
    objData->value = val;
    if (val <= 0)
        return;

    // update usedBy
    BaseValue *valData = this->valData(val);
    valData->usedBy.insert(obj);
    if (!isPossibleToDeref(valData->code))
        return;

    // update usedByGl
    const TValId root = this->valRoot(val, valData);
    RootValue *rootData = this->rootData(root);
    rootData->usedByGl.insert(obj);
}

TObjId SymHeapCore::Private::objCreate() {
    // acquire object ID
    this->ents.push_back(new HeapObject);
    return this->lastId<TObjId>();
}

TValId SymHeapCore::Private::valCreate(
        EValueTarget                code,
        EValueOrigin                origin)
{
    switch (code) {
        case VT_INVALID:
        case VT_UNKNOWN:
        case VT_DELETED:
        case VT_LOST:
            this->ents.push_back(new BaseValue(code, origin));
            break;

        case VT_COMPOSITE:
            this->ents.push_back(new CompValue(code, origin));
            break;

        case VT_CUSTOM:
            this->ents.push_back(new CustomValue(code, origin));
            break;

        case VT_ABSTRACT:
            CL_BREAK_IF("invalid call of SymHeapCore::Private::valCreate()");
            // fall through!

        case VT_ON_HEAP:
        case VT_ON_STACK:
        case VT_STATIC:
            this->ents.push_back(new RootValue(code, origin));
            break;
    }

    return this->lastId<TValId>();
}

TValId SymHeapCore::Private::valDup(TValId val) {
    // deep copy the value
    const BaseValue *tpl = this->valData(val);
    this->ents.push_back(/* FIXME: subtle */ tpl->clone());

    // wipe BaseValue::usedBy
    const TValId dup = this->lastId<TValId>();
    BaseValue *dupData = this->valData(dup);
    dupData->usedBy.clear();

    return dup;
}

SymHeapCore::Private::Private() {
    this->ents.push_back(/* reserved for VAL_NULL */ 0);
}

SymHeapCore::Private::Private(const SymHeapCore::Private &ref):
    cVarMap     (ref.cVarMap),
    cValueMap   (ref.cValueMap),
    ents        (ref.ents),
    liveRoots   (ref.liveRoots),
    neqDb       (ref.neqDb)
{
    // deep copy of all heap entities
    BOOST_FOREACH(IHeapEntity *&ent, this->ents)
        if (ent)
            ent = ent->clone();
}

SymHeapCore::Private::~Private() {
    BOOST_FOREACH(const IHeapEntity *ent, this->ents)
        delete ent;
}

EValueOrigin originByCode(const EValueTarget code) {
    switch (code) {
        case VT_INVALID:
            return VO_INVALID;

        case VT_ON_HEAP:
            return VO_HEAP;

        case VT_ON_STACK:
            return VO_STACK;

        case VT_STATIC:
            return VO_STATIC;

        default:
            CL_BREAK_IF("invalid call of originByCode");
            return VO_INVALID;
    }
}

// FIXME: should this be declared non-const?
TValId SymHeapCore::valueOf(TObjId obj) const {
    // handle special cases first
    switch (obj) {
        case OBJ_UNKNOWN:
            // not implemented
        case OBJ_INVALID:
            return VAL_INVALID;

        case OBJ_DEREF_FAILED:
            return VAL_DEREF_FAILED;

        default:
            break;
    }

    HeapObject *objData = d->objData(obj);
    TValId &val = objData->value;
    if (VAL_INVALID != val)
        // the object has a value
        return val;

    if (isComposite(objData->clt)) {
        // deleayed creation of a composite value
        val = d->valCreate(VT_COMPOSITE, VO_INVALID);
        CompValue *valData = DCAST<CompValue *>(d->ents[val]);
        valData->compObj = obj;
    }
    else {
        // deleayed creation of an uninitialized value
        const BaseValue *rootData = d->rootData(objData->root);
        const EValueOrigin vo = originByCode(rootData->code);
        val = d->valCreate(VT_UNKNOWN, vo);
    }

    // store backward reference
    d->valData(val)->usedBy.insert(obj);
    return val;
}

void SymHeapCore::usedBy(TObjList &dst, TValId val) const {
    if (VAL_NULL == val)
        return;

    const BaseValue *valData = d->valData(val);
    const TUsedBy &usedBy = valData->usedBy;
    std::copy(usedBy.begin(), usedBy.end(), std::back_inserter(dst));
}

unsigned SymHeapCore::usedByCount(TValId val) const {
    if (VAL_NULL == val)
        return 0;

    const BaseValue *valData = d->valData(val);
    return valData->usedBy.size();
}

void SymHeapCore::pointedBy(TObjList &dst, TValId root) const {
    const RootValue *rootData = d->rootData(root);
    CL_BREAK_IF(rootData->offRoot);
    CL_BREAK_IF(!isPossibleToDeref(rootData->code));

    const TUsedBy &usedBy = rootData->usedByGl;
    std::copy(usedBy.begin(), usedBy.end(), std::back_inserter(dst));
}

unsigned SymHeapCore::lastId() const {
    return d->lastId<unsigned>();
}

TValId SymHeapCore::valClone(TValId val) {
    const BaseValue *valData = d->valData(val);
    const EValueTarget code = valData->code;
    if (!isPossibleToDeref(code))
        // duplicate an unknown value
        return d->valDup(val);

    // duplicate a root object
    const TValId root = d->valRoot(val, valData);
    const TValId dupAt = d->objDup(root);

    // take the offset into consideration
    return this->valByOffset(dupAt, valData->offRoot);
}

void SymHeapCore::valMerge(TValId val, TValId replaceBy) {
    CL_BREAK_IF(this->proveNeq(val, replaceBy));
    moveKnownValueToLeft(*this, replaceBy, val);
    this->valReplace(val, replaceBy);
}

template <class TValMap>
bool valMapLookup(const TValMap &valMap, TValId *pVal) {
    if (*pVal <= VAL_NULL)
        // special values always match, no need for mapping
        return true;

    typename TValMap::const_iterator iter = valMap.find(*pVal);
    if (valMap.end() == iter)
        // mapping not found
        return false;

    // match
    *pVal = iter->second;
    return true;
}

void SymHeapCore::Private::subsCreate(TValId rootAt) {
    this->liveRoots.insert(rootAt);

    // initialize root clt
    RootValue *rootData = this->rootData(rootAt);
    TObjType clt = rootData->lastKnownClt;
    rootData->cbSize = clt->size;

    typedef std::pair<TObjId, TObjType> TPair;
    typedef std::stack<TPair> TStack;
    TStack todo;

    CL_BREAK_IF(rootData->allObjs.size() != 1);
    TObjId obj = rootData->allObjs.front();
    TGrid &grid = rootData->grid;
    grid[/* off */ 0][clt] = obj;

    // we use explicit stack to avoid recursion
    push(todo, obj, clt);
    while (!todo.empty()) {
        boost::tie(obj, clt) = todo.top();
        todo.pop();
        CL_BREAK_IF(!clt);

        if (!isComposite(clt))
            continue;

        const TOffset offRoot = this->objData(obj)->off;

        const int cnt = clt->item_cnt;
        for (int i = 0; i < cnt; ++i) {
            const struct cl_type_item *item = clt->items + i;
            TObjType subClt = item->type;

            const TObjId subObj = this->objCreate();
            HeapObject *subData = this->objData(subObj);
            subData->clt  = subClt;
            subData->root = rootAt;

            const TOffset offTotal = offRoot + item->offset;
            subData->off = offTotal;
            grid[offTotal][subClt] = subObj;

            // XXX
            rootData->allObjs.push_back(subObj);

            push(todo, subObj, subClt);
        }
    }
}

TValId SymHeapCore::Private::objDup(TValId rootAt) {
    CL_DEBUG("SymHeapCore::Private::objDup() is taking place...");
    const RootValue *rootDataSrc = this->rootData(rootAt);

    // duplicate the root object
    const TObjId image = this->objCreate();

    // duplicate type-info of the root object
    const TObjType cltRoot = rootDataSrc->lastKnownClt;
    HeapObject *objDataDst = this->objData(image);
    objDataDst->clt = cltRoot;

    // assign an address to the clone
    const EValueTarget code = rootDataSrc->code;
    const TValId imageAt = this->valCreate(code, VO_ASSIGNED);
    RootValue *rootDataDst = this->rootData(imageAt);
    rootDataDst->addr = imageAt;
    objDataDst->root = imageAt;

    // duplicate root metadata
    rootDataDst->grid[/* off */0][cltRoot] = image;
    rootDataDst->cVar    = rootDataSrc->cVar;
    rootDataDst->isProto = rootDataSrc->isProto;
    rootDataDst->cbSize  = rootDataSrc->cbSize;
    rootDataDst->lastKnownClt = rootDataSrc->lastKnownClt;

    rootDataDst->allObjs.push_back(image);

    this->liveRoots.insert(imageAt);

    BOOST_FOREACH(const TObjId src, rootDataSrc->allObjs) {
        const HeapObject *objDataSrc = this->objData(src);

        // duplicate a single object
        const TObjId dst = this->objCreate();
        this->setValueOf(dst, objDataSrc->value);

        // copy the metadata
        objDataDst = this->objData(dst);
        objDataDst->off = objDataSrc->off;
        objDataDst->clt = objDataSrc->clt;

        // prevserve live ptr/data object
        const TLiveObjs &liveSrc = rootDataSrc->liveObjs;
        TLiveObjs::const_iterator it = liveSrc.find(src);
        if (liveSrc.end() != it)
            rootDataDst->liveObjs[dst] = /* isPtr */ it->second;

        // recover root
        objDataDst->root = imageAt;
        rootDataDst->allObjs.push_back(dst);

        // recover grid
        const TOffset off = objDataDst->off;
        const TObjType clt = objDataDst->clt;
        TGrid &grid = rootDataDst->grid;
        grid[off][clt] = dst;
    }

    return imageAt;
}

void SymHeapCore::gatherLivePointers(TObjList &dst, TValId root) const {
    const RootValue *rootData = d->rootData(root);
    BOOST_FOREACH(TLiveObjs::const_reference item, rootData->liveObjs) {
        if (/* isPtr */ item.second)
            dst.push_back(/* obj */ item.first);
    }
}

void SymHeapCore::gatherLiveObjects(TObjList &dst, TValId root) const {
    const RootValue *rootData = d->rootData(root);
    BOOST_FOREACH(TLiveObjs::const_reference item, rootData->liveObjs)
        dst.push_back(/* obj */ item.first);
}

SymHeapCore::SymHeapCore(TStorRef stor):
    stor_(stor),
    d(new Private)
{
    CL_BREAK_IF(!&stor_);

    // initialize VAL_ADDR_OF_RET
    const TValId addrRet = d->valCreate(VT_ON_STACK, VO_ASSIGNED);
    CL_BREAK_IF(VAL_ADDR_OF_RET != addrRet);
    d->rootData(addrRet)->addr = addrRet;
}

SymHeapCore::SymHeapCore(const SymHeapCore &ref):
    stor_(ref.stor_),
    d(new Private(*ref.d))
{
    CL_BREAK_IF(!&stor_);
}

SymHeapCore::~SymHeapCore() {
    delete d;
}

SymHeapCore& SymHeapCore::operator=(const SymHeapCore &ref) {
    CL_BREAK_IF(&stor_ != &ref.stor_);

    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeapCore::swap(SymHeapCore &ref) {
    CL_BREAK_IF(&stor_ != &ref.stor_);
    swapValues(this->d, ref.d);
}

void SymHeapCore::objSetValue(TObjId obj, TValId val) {
    // we allow to set values of atomic types only
    const HeapObject *objData = d->objData(obj);
    const TObjType clt = objData->clt;
    CL_BREAK_IF(isComposite(clt));

    // mark the destination object as live
    const TValId root = objData->root;
    RootValue *rootData = d->rootData(root);
    rootData->liveObjs[obj] = /* isPtr */ isDataPtr(clt);

    // now set the value
    d->setValueOf(obj, val);
}

TObjType SymHeapCore::objType(TObjId obj) const {
    if (obj < 0)
        return 0;

    const HeapObject *objData = d->objData(obj);
    return objData->clt;
}

TValId SymHeapCore::valByOffset(TValId at, TOffset off) {
    if (!off || at <= 0)
        return at;

    // subtract the root
    const BaseValue *valData = d->valData(at);
    const TValId valRoot = d->valRoot(at, valData);
    off += valData->offRoot;
    if (!off)
        return valRoot;

    const EValueTarget code = valData->code;
    if (VT_UNKNOWN == code || isGone(code))
        // do not track off-value for invalid targets
        return d->valDup(at);

    // off-value lookup
    RootValue *rootData = d->rootData(valRoot);
    TOffMap &offMap = rootData->offMap;
    TOffMap::const_iterator it = offMap.find(off);
    if (offMap.end() != it)
        return it->second;

    // create a new off-value
    d->ents.push_back(new OffValue(code, valData->origin, valRoot, off));
    const TValId val = d->lastId<TValId>();

    // store the mapping for next wheel
    offMap[off] = val;
    return val;
}

EValueOrigin SymHeapCore::valOrigin(TValId val) const {
    switch (val) {
        case VAL_INVALID:
            return VO_INVALID;

        case VAL_DEREF_FAILED:
            return VO_DEREF_FAILED;

        case VAL_NULL /* = VAL_FALSE */:
        case VAL_TRUE:
            return VO_ASSIGNED;

        default:
            break;
    }

    const BaseValue *valData = d->valData(val);
    return valData->origin;
}

EValueTarget SymHeapCore::valTarget(TValId val) const {
    if (val <= 0)
        return VT_INVALID;

    if (this->hasAbstractTarget(val))
        // the overridden implementation claims the target is abstract
        return VT_ABSTRACT;

    const BaseValue *valData = d->valData(val);
    const TOffset off = valData->offRoot;
    if (off < 0)
        // this value ended up above the root
        return VT_UNKNOWN;

    const EValueTarget code = valData->code;
    if (!isPossibleToDeref(code) || !off)
        // we are done for unknown values and root values
        return code;

    // off-value --> check the root, chances are it has already been deleted
    const TValId valRoot = d->valRoot(val, valData);
    return d->valData(valRoot)->code;
}

bool isUninitialized(EValueOrigin code) {
    switch (code) {
        case VO_STATIC:
            // this may be subject for discussion
        case VO_HEAP:
        case VO_STACK:
            return true;

        default:
            return false;
    }
}

bool isAbstract(EValueTarget code) {
    return (VT_ABSTRACT == code);
}

bool isKnownObject(EValueTarget code) {
    switch (code) {
        case VT_STATIC:
        case VT_ON_HEAP:
        case VT_ON_STACK:
            return true;

        default:
            return false;
    }
}

bool isGone(EValueTarget code) {
    switch (code) {
        case VT_DELETED:
        case VT_LOST:
            return true;

        default:
            return false;
    }
}

bool isOnHeap(EValueTarget code) {
    switch (code) {
        case VT_ON_HEAP:
        case VT_ABSTRACT:
            return true;

        default:
            return false;
    }
}

bool isProgramVar(EValueTarget code) {
    switch (code) {
        case VT_STATIC:
        case VT_ON_STACK:
            return true;

        default:
            return false;
    }
}

bool isPossibleToDeref(EValueTarget code) {
    return isOnHeap(code)
        || isProgramVar(code);
}

TValId SymHeapCore::valRoot(TValId val) const {
    if (val <= 0)
        return val;

    return d->valRoot(val);
}

TOffset SymHeapCore::valOffset(TValId val) const {
    if (val <= 0)
        return 0;

    const BaseValue *valData = d->valData(val);
    return valData->offRoot;
}

void SymHeapCore::valReplace(TValId val, TValId replaceBy) {
    const BaseValue *valData = d->valData(val);

    // we intentionally do not use a reference here (tight loop otherwise)
    TUsedBy usedBy = valData->usedBy;
    BOOST_FOREACH(const TObjId obj, usedBy)
        this->objSetValue(obj, replaceBy);

    // kill Neq predicate among the pair of values (if any)
    SymHeapCore::neqOp(NEQ_DEL, val, replaceBy);

    // reflect the change in NeqDb
    TValList neqs;
    d->neqDb.gatherRelatedValues(neqs, val);
    BOOST_FOREACH(const TValId valNeq, neqs) {
        SymHeapCore::neqOp(NEQ_DEL, valNeq, val);
        SymHeapCore::neqOp(NEQ_ADD, valNeq, replaceBy);
    }
}

void SymHeapCore::Private::neqOpWrap(ENeqOp op, TValId valA, TValId valB) {
    switch (op) {
        case NEQ_NOP:
            return;

        case NEQ_ADD:
            this->neqDb.add(valA, valB);
            return;

        case NEQ_DEL:
            this->neqDb.del(valA, valB);
            return;
    }
}

void SymHeapCore::neqOp(ENeqOp op, TValId valA, TValId valB) {
    d->neqOpWrap(op, valA, valB);

    const TOffset off = this->valOffset(valA);
    if (!off || off != this->valOffset(valB))
        return;

    // if both values have the same non-zero offset, connect also the roots
    valA = this->valRoot(valA);
    valB = this->valRoot(valB);
    d->neqOpWrap(op, valA, valB);
}

void SymHeapCore::gatherRelatedValues(TValList &dst, TValId val) const {
    d->neqDb.gatherRelatedValues(dst, val);
}

void SymHeapCore::copyRelevantPreds(SymHeapCore &dst, const TValMap &valMap)
    const
{
    // go through NeqDb
    BOOST_FOREACH(const NeqDb::TItem &item, d->neqDb.cont_) {
        TValId valLt = item.first;
        TValId valGt = item.second;

        if (!valMapLookup(valMap, &valLt) || !valMapLookup(valMap, &valGt))
            // not relevant
            continue;

        // create the image now!
        dst.neqOp(NEQ_ADD, valLt, valGt);
    }
}

bool SymHeapCore::matchPreds(const SymHeapCore &ref, const TValMap &valMap)
    const
{
    // go through NeqDb
    BOOST_FOREACH(const NeqDb::TItem &item, d->neqDb.cont_) {
        TValId valLt = item.first;
        TValId valGt = item.second;
        if (!valMapLookup(valMap, &valLt) || !valMapLookup(valMap, &valGt))
            // seems like a dangling predicate, which we are not interested in
            continue;

        if (!ref.d->neqDb.areNeq(valLt, valGt))
            // Neq predicate not matched
            return false;
    }

    return true;
}

// FIXME: should this be declared non-const?
TValId SymHeapCore::placedAt(TObjId obj) const {
    if (obj < 0)
        return VAL_INVALID;

    // jump to root
    const HeapObject *objData = d->objData(obj);
    const RootValue *rootData = d->rootData(objData->root);
    const TValId addr = rootData->addr;
    CL_BREAK_IF(addr <= 0);

    // then subtract the offset
    SymHeapCore &writable = /* FIXME */ const_cast<SymHeapCore &>(*this);
    return writable.valByOffset(addr, objData->off);
}

bool SymHeapCore::Private::gridLookup(
        TObjId                     *pFailCode,
        const TObjByType          **pRow,
        const TValId                val)
{
    if (val <= 0) {
        *pFailCode = OBJ_INVALID;
        return false;
    }

    const BaseValue *valData = this->valData(val);
    const EValueTarget code = valData->code;
    switch (code) {
        case VT_UNKNOWN:
            *pFailCode = /* XXX */ OBJ_UNKNOWN;
            return false;

        default:
            if (isPossibleToDeref(code))
                break;

            *pFailCode = OBJ_INVALID;
            return false;
    }

    // grid lookup
    const TValId valRoot = this->valRoot(val, valData);
    const RootValue *rootData = this->rootData(valRoot);
    const TGrid &grid = rootData->grid;

    // TODO: uncomment this once we are ready for lazy objects creation
    CL_BREAK_IF(grid.empty());

    const TOffset off = valData->offRoot;
    TGrid::const_iterator it = grid.find(off);
    if (grid.end() == it) {
        *pFailCode = OBJ_UNKNOWN;
        return false;
    }

    const TObjByType *row = &it->second;
    CL_BREAK_IF(row->empty());
    *pRow = row;
    return true;
}

TObjId SymHeapCore::ptrAt(TValId at) {
    TObjId failCode;
    const TObjByType *row;
    if (!d->gridLookup(&failCode, &row, at))
        return failCode;

    // seek a _data_ pointer at the given row
    BOOST_FOREACH(const TObjByType::const_reference item, *row) {
        const TObjType clt = item.first;
        if (!clt || clt->code != CL_TYPE_PTR)
            // not a pointer
            continue;

        const TObjType cltTarget = targetTypeOfPtr(clt);
        if (CL_TYPE_FNC != cltTarget->code)
            // matched
            return item.second;
    }

    // TODO: check that there is at most once pointer in the row in debug build

    return OBJ_UNKNOWN;
}

TObjId SymHeapCore::objAt(TValId at, TObjCode code) {
    if (at <= 0)
        return OBJ_INVALID;

    const BaseValue *valData = d->valData(at);
    const EValueTarget vt = valData->code;
    switch (vt) {
        case VT_COMPOSITE:
        case VT_CUSTOM:
            return OBJ_INVALID;

        default:
            break;
    }

    const TValId root = d->valRoot(at, valData);
    const RootValue *rootData = d->rootData(root);

    TObjId failCode;
    const TObjByType *row;
    if (!d->gridLookup(&failCode, &row, at))
        return failCode;

    // seek the bigest object at the given row
    int maxSize = 0;
    TObjId max = OBJ_UNKNOWN;
    BOOST_FOREACH(const TObjByType::const_reference item, *row) {
        const TObjType cltItem = item.first;
        const TObjId obj = item.second;
        CL_BREAK_IF(d->objOutOfRange(obj));

        const bool hasType = !!cltItem;
        if (CL_TYPE_VOID != code && (!hasType || cltItem->code != code))
            // not the type we are looking for
            continue;

        const int size = (hasType)
            ? cltItem->size
            : rootData->cbSize;

        if (size < maxSize)
            continue;

        if ((size == maxSize) && !isComposite(cltItem))
            // if two types have the same size, prefer the composite one
            continue;

        // update the maximum
        maxSize = size;
        max = obj;
    }

    return max;
}

TObjId SymHeapCore::objAt(TValId at, TObjType clt) {
    if (clt
            && CL_TYPE_PTR == clt->code
            && CL_TYPE_FNC != targetTypeOfPtr(clt)->code)
        // look for a pointer to data
        return this->ptrAt(at);

    TObjId failCode;
    const TObjByType *row;
    if (!d->gridLookup(&failCode, &row, at))
        return failCode;

    TObjByType::const_iterator it = row->find(clt);
    if (row->end() != it)
        // exact match
        return it->second;

    if (!clt)
        // no type-free object found
        return OBJ_UNKNOWN;

    // try semantic match
    BOOST_FOREACH(const TObjByType::const_reference item, *row) {
        const TObjType cltItem = item.first;
        if (cltItem && *cltItem == *clt)
            return item.second;
    }

    // not found
    return OBJ_UNKNOWN;
}

CVar SymHeapCore::cVarByRoot(TValId valRoot) const {
    const RootValue *rootData = d->rootData(valRoot);
    return rootData->cVar;
}

TValId SymHeapCore::addrOfVar(CVar cv) {
    TValId addr = d->cVarMap.find(cv);
    if (0 < addr)
        return addr;

    // lazy creation of a program variable
    const CodeStorage::Var &var = stor_.vars[cv.uid];
    TObjType clt = var.type;
    CL_BREAK_IF(!clt || clt->code == CL_TYPE_VOID);
#if DEBUG_SE_STACK_FRAME
    const struct cl_loc *loc = 0;
    std::string varString = varTostring(stor_, cv.uid, &loc);
    CL_DEBUG_MSG(loc, "FFF SymHeapCore::objByCVar() creates var " << varString);
#endif

    // create the corresponding heap object
    const TObjId root = d->objCreate();
    HeapObject *objData = d->objData(root);
    objData->clt = clt;

    // assign an address
    const EValueTarget code = isOnStack(var) ? VT_ON_STACK : VT_STATIC;
    addr = d->valCreate(code, VO_ASSIGNED);
    objData->root = addr;

    RootValue *rootData = d->rootData(addr);
    rootData->cVar = cv;
    rootData->addr = addr;
    rootData->lastKnownClt = clt;
    rootData->allObjs.push_back(root);

    // create the structure
    d->subsCreate(addr);

    // store the address for next wheel
    d->cVarMap.insert(cv, addr);
    return addr;
}

void SymHeapCore::gatherCVars(TCVarList &dst) const {
    d->cVarMap.getAll(dst);
}

static bool dummyFilter(EValueTarget) {
    return true;
}

void SymHeapCore::gatherRootObjects(TValList &dst, bool (*filter)(EValueTarget))
    const
{
    if (!filter)
        filter = dummyFilter;

    BOOST_FOREACH(const TValId at, d->liveRoots)
        if (filter(this->valTarget(at)))
            dst.push_back(at);
}

TObjId SymHeapCore::valGetComposite(TValId val) const {
    const BaseValue *valData = d->valData(val);
    CL_BREAK_IF(VT_COMPOSITE != valData->code);

    const CompValue *compData = DCAST<const CompValue *>(valData);
    return compData->compObj;
}

TValId SymHeapCore::heapAlloc(int cbSize) {
    // assign an address
    const TValId addr = d->valCreate(VT_ON_HEAP, VO_ASSIGNED);

    // initialize meta-data
    RootValue *rootData = d->rootData(addr);
    rootData->addr = addr;
    rootData->cbSize = cbSize;

    return addr;
}

bool SymHeapCore::valDestroyTarget(TValId val) {
    if (VAL_NULL == val)
        // VAL_NULL has no target
        return false;

    const BaseValue *valData = d->valData(val);
    if (valData->offRoot || !isPossibleToDeref(valData->code))
        // such a target is not supposed to be destroyed
        return false;

    d->objDestroy(val);
    return true;
}

int SymHeapCore::valSizeOfTarget(TValId val) const {
    const BaseValue *valData = d->valData(val);
    CL_BREAK_IF(!isPossibleToDeref(valData->code));

    const TValId root = d->valRoot(val, valData);
    const RootValue *rootData = d->rootData(root);

    // FIXME: this field is not initialized accurately in all cases
    const int rootSize = rootData->cbSize;
    const TOffset off = valData->offRoot;
    return rootSize - off;
}

void SymHeapCore::valSetLastKnownTypeOfTarget(TValId root, TObjType clt) {
    RootValue *rootData = d->rootData(root);

    // TODO: remove this
    TObjId obj; 

    if (VAL_ADDR_OF_RET == root) {
        // destroy any stale target of VAL_ADDR_OF_RET
        d->objDestroy(root);

        // allocate a new target of VAL_ADDR_OF_RET
        obj = d->objCreate();
        rootData->code = VT_ON_STACK;
    }
    else
        // TODO: remove this
        obj = d->objCreate();

    HeapObject *objData = d->objData(obj);
    objData->root = root;
    objData->clt = clt;

    // delayed object's type definition
    rootData->allObjs.push_back(obj);
    rootData->lastKnownClt = clt;
    d->subsCreate(root);
}

TObjType SymHeapCore::valLastKnownTypeOfTarget(TValId root) const {
    CL_BREAK_IF(d->valData(root)->offRoot);
    const RootValue *rootData = d->rootData(root);
    return rootData->lastKnownClt;
}

void SymHeapCore::Private::objDestroy(TValId root) {
    RootValue *rootData = this->rootData(root);

    EValueTarget code = VT_DELETED;
    const CVar cv = rootData->cVar;
    if (cv.uid != /* heap object */ -1) {
        // remove the corresponding program variable
        this->cVarMap.remove(cv);
        code = VT_LOST;
    }

    // release the root
    this->liveRoots.erase(root);

    // go through objects
    BOOST_FOREACH(const TObjId obj, rootData->allObjs) {
        HeapObject *objData = this->objData(obj);
        objData->clt = 0;

        // release a single object
        TValId &val = objData->value;
        this->releaseValueOf(obj, val);
        val = VAL_INVALID;
    }

    // wipe rootData
    rootData->lastKnownClt = 0;
    rootData->allObjs.clear();
    rootData->liveObjs.clear();
    rootData->grid.clear();
    rootData->code   = code;
}

TValId SymHeapCore::valCreate(EValueTarget code, EValueOrigin origin) {
    return d->valCreate(code, origin);
}

TValId SymHeapCore::valCreateCustom(int cVal) {
    TCValueMap::iterator iter = d->cValueMap.find(cVal);
    if (d->cValueMap.end() != iter)
        // custom value already wrapped, we have to reuse it
        return iter->second;

    // cVal not found, create a new wrapper for it
    const TValId val = d->valCreate(VT_CUSTOM, VO_ASSIGNED);

    // initialize the custom value
    CustomValue *valData = DCAST<CustomValue *>(d->ents[val]);
    valData->customData  = cVal;

    // store cVal --> val mapping
    d->cValueMap[cVal] = val;
    return val;
}

int SymHeapCore::valGetCustom(TValId val) const
{
    const BaseValue *valData = d->valData(val);
    CL_BREAK_IF(VT_CUSTOM != valData->code);

    const CustomValue *cstData = DCAST<const CustomValue *>(valData);
    return cstData->customData;
}

bool SymHeapCore::valTargetIsProto(TValId val) const {
    if (val <= 0)
        // not a prototype for sure
        return false;

    const BaseValue *valData = d->valData(val);
    if (!isPossibleToDeref(valData->code))
        // not a prototype for sure
        return false;

    // seek root
    const TValId root = d->valRoot(val, valData);
    const RootValue *rootData = d->rootData(root);
    return rootData->isProto;
}

void SymHeapCore::valTargetSetProto(TValId val, bool isProto) {
    const TValId root = d->valRoot(val);
    RootValue *rootData = d->rootData(root);
    rootData->isProto = isProto;
}

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeap
struct SymHeap::Private {
    struct AbstractObject {
        EObjKind            kind;
        BindingOff          off;

        AbstractObject():
            kind(OK_CONCRETE)
        {
        }
    };

    typedef std::map<TValId, AbstractObject> TData;
    TData data;
};

SymHeap::SymHeap(TStorRef stor):
    SymHeapCore(stor),
    d(new Private)
{
}

SymHeap::SymHeap(const SymHeap &ref):
    SymHeapCore(ref),
    d(new Private(*ref.d))
{
}

SymHeap::~SymHeap() {
    delete d;
}

SymHeap& SymHeap::operator=(const SymHeap &ref) {
    SymHeapCore::operator=(ref);
    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeap::swap(SymHeapCore &baseRef) {
    // swap base
    SymHeapCore::swap(baseRef);

    // swap self
    SymHeap &ref = DCAST<SymHeap &>(baseRef);
    swapValues(this->d, ref.d);
}

TValId SymHeap::valClone(TValId val) {
    const TValId dup = SymHeapCore::valClone(val);
    if (dup <= 0)
        return dup;

    const TValId valRoot = this->valRoot(val);
    const TValId dupRoot = this->valRoot(dup);
    CL_BREAK_IF(valRoot <= 0 || dupRoot <= 0);

    Private::TData::iterator iter = d->data.find(valRoot);
    if (d->data.end() != iter)
        // duplicate metadata of an abstract object
        d->data[dupRoot] = iter->second;

    return dup;
}

EObjKind SymHeap::valTargetKind(TValId val) const {
    if (val <= 0)
        // FIXME
        return OK_CONCRETE;

    const TValId valRoot = this->valRoot(val);
    Private::TData::iterator iter = d->data.find(valRoot);
    if (d->data.end() != iter)
        return iter->second.kind;

    return OK_CONCRETE;
}

bool SymHeap::hasAbstractTarget(TValId val) const {
    return (OK_CONCRETE != this->valTargetKind(val));
}

const BindingOff& SymHeap::segBinding(TValId at) const {
    const TValId valRoot = this->valRoot(at);

    Private::TData::iterator iter = d->data.find(valRoot);
    CL_BREAK_IF(d->data.end() == iter);

    return iter->second.off;
}

void SymHeap::valTargetSetAbstract(
        TValId                      at,
        EObjKind                    kind,
        const BindingOff            &off)
{
    const TValId valRoot = this->valRoot(at);

    Private::TData::iterator iter = d->data.find(valRoot);
    if (d->data.end() != iter && OK_SLS == kind) {
        Private::AbstractObject &aoData = iter->second;
        CL_BREAK_IF(OK_MAY_EXIST != aoData.kind || off != aoData.off);

        // OK_MAY_EXIST -> OK_SLS
        aoData.kind = kind;
        return;
    }

    CL_BREAK_IF(OK_CONCRETE == kind || hasKey(d->data, valRoot));

    // initialize abstract object
    Private::AbstractObject &aoData = d->data[valRoot];
    aoData.kind     = kind;
    aoData.off      = off;
}

void SymHeap::valTargetSetConcrete(TValId at) {
    const TValId valRoot = this->valRoot(at);

    CL_DEBUG("SymHeap::objSetConcrete() is taking place...");
    Private::TData::iterator iter = d->data.find(valRoot);
    CL_BREAK_IF(d->data.end() == iter);

    // just remove the object ID from the map
    d->data.erase(iter);
}

void SymHeap::valMerge(TValId v1, TValId v2) {
    // check that at least one value is unknown
    moveKnownValueToLeft(*this, v1, v2);
    const EValueTarget code1 = this->valTarget(v1);
    const EValueTarget code2 = this->valTarget(v2);
    CL_BREAK_IF(isKnownObject(code2));

    if (VT_ABSTRACT != code1 && VT_ABSTRACT != code2) {
        // no abstract objects involved
        SymHeapCore::valMerge(v1, v2);
        return;
    }

    if (VT_ABSTRACT == code1 && spliceOutListSegment(*this, v1, v2))
        // splice-out succeeded ... ls(v1, v2)
        return;

    if (VT_ABSTRACT == code2 && spliceOutListSegment(*this, v2, v1))
        // splice-out succeeded ... ls(v2, v1)
        return;

    CL_DEBUG("failed to splice-out list segment, has to over-approximate");
}

void SymHeap::dlSegCrossNeqOp(ENeqOp op, TValId seg1) {
    // seek roots
    seg1 = this->valRoot(seg1);
    TValId seg2 = dlSegPeer(*this, seg1);

    // read the values (addresses of the surround)
    const TValId val1 = this->valueOf(nextPtrFromSeg(*this, seg1));
    const TValId val2 = this->valueOf(nextPtrFromSeg(*this, seg2));

    // add/del Neq predicates
    SymHeapCore::neqOp(op, val1, segHeadAt(*this, seg2));
    SymHeapCore::neqOp(op, val2, segHeadAt(*this, seg1));

    if (NEQ_DEL == op)
        // removing the 1+ flag implies removal of the 2+ flag
        SymHeapCore::neqOp(NEQ_DEL, seg1, seg2);
}

void SymHeap::neqOp(ENeqOp op, TValId valA, TValId valB) {
    if (NEQ_ADD == op && haveDlSegAt(*this, valA, valB)) {
        // adding the 2+ flag implies adding of the 1+ flag
        this->dlSegCrossNeqOp(op, valA);
    }
    else {
        if (haveSeg(*this, valA, valB, OK_DLS)) {
            this->dlSegCrossNeqOp(op, valA);
            return;
        }

        if (haveSeg(*this, valB, valA, OK_DLS)) {
            this->dlSegCrossNeqOp(op, valB);
            return;
        }
    }

    SymHeapCore::neqOp(op, valA, valB);
}

bool SymHeapCore::proveNeq(TValId valA, TValId valB) const {
    // check for invalid values
    if (VAL_INVALID == valA || VAL_INVALID == valB)
        return false;

    // check for identical values
    if (valA == valB)
        return false;

    // having the values always in the same order leads to simpler code
    moveKnownValueToLeft(*this, valA, valB);

    // check for known bool values
    if (VAL_TRUE == valA)
        return (VAL_FALSE == valB);

    // we presume (0 <= valA) and (0 < valB) at this point
    CL_BREAK_IF(d->valOutOfRange(valB));
    const EValueTarget code = this->valTarget(valB);
    if (isKnownObject(code))
        // NOTE: we know (valA != valB) at this point, look above
        return true;

    // check for a Neq predicate
    if (d->neqDb.areNeq(valA, valB))
        return true;

    if (valA <= 0 || valB <= 0)
        // no handling of special values here
        return false;

    const TValId root1 = d->valRoot(valA);
    const TValId root2 = d->valRoot(valB);
    if (root1 == root2) {
        CL_BREAK_IF("not tested");
        return true;
    }

    const TOffset off = this->valOffset(valA);
    if (!off)
        // roots differ
        return false;

    if (off != this->valOffset(valB))
        // offsets differ
        return false;

    // check for Neq between the roots
    return d->neqDb.areNeq(root1, root2);
}

bool SymHeap::proveNeq(TValId ref, TValId val) const {
    if (SymHeapCore::proveNeq(ref, val))
        return true;

    // having the values always in the same order leads to simpler code
    moveKnownValueToLeft(*this, ref, val);
    const EValueTarget code = this->valTarget(ref);
    if (isAbstract(code))
        return false;

    std::set<TValId> haveSeen;

    while (0 < val && insertOnce(haveSeen, val)) {
        const EValueTarget code = this->valTarget(val);
        switch (code) {
            case VT_ON_STACK:
            case VT_ON_HEAP:
            case VT_STATIC:
            case VT_DELETED:
            case VT_LOST:
            case VT_CUSTOM:
                // concrete object reached --> prove done
                return (val != ref);

            case VT_ABSTRACT:
                break;

            default:
                // we can't prove much for unknown values
                return false;
        }

        if (SymHeapCore::proveNeq(ref, val))
            // prove done
            return true;

        SymHeap &writable = *const_cast<SymHeap *>(this);

        TValId seg = this->valRoot(val);
        if (OK_DLS == this->valTargetKind(val))
            seg = dlSegPeer(writable, seg);

        if (seg < 0)
            // no valid object here
            return false;

        const TValId valNext = this->valueOf(nextPtrFromSeg(writable, seg));
        if (SymHeapCore::proveNeq(val, valNext))
            // non-empty abstract object reached --> prove done
            return true;

        // jump to next value
        val = valNext;
    }

    return false;
}

bool SymHeap::valDestroyTarget(TValId val) {
    const TValId valRoot = this->valRoot(val);
    if (!SymHeapCore::valDestroyTarget(val))
        return false;

    CL_BREAK_IF(valRoot <= 0);
    if (d->data.erase(valRoot))
        CL_DEBUG("SymHeap::valDestroyTarget() destroys an abstract object");

    return true;
}
