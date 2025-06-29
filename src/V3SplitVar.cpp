// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Break variables into separate words to avoid UNOPTFLAT
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// V3SplitVar divides a variable into multiple variables to avoid UNOPTFLAT warning
// and get better performance.
// Variables to be split must be marked by /*verilator split_var*/ metacomment.
// There are several kinds of data types that may cause the warning.
// 1) Unpacked arrays
// 2) Packed arrays
// 3) Unpacked structs
// 4) Packed structs
// 5) Bitfields within a signal. (Especially Verilog code predating structs/2D arrays.)
// 2-5 above are treated as bitfields in verilator.
//
// What this pass does looks as below.
//
//     // Original
//     logic [1:0] unpacked_array_var[0:1] /*verilator split_var*/;
//     always_comb begin
//        unpacked_array_var[1][0] =  unpacked_array_var[0][0]; // UNOPTFLAT warning
//        unpacked_array_var[1][1] = ~unpacked_array_var[0][1]; // UNOPTFLAT warning
//     end
//     logic [3:0] packed_var /*verilator split_var*/;
//     always_comb begin
//        if (some_cond) begin
//            packed_var = 4'b0;
//        end else begin
//            packed_var[3]   = some_input0;
//            packed_var[2:0] = some_input1;
//        end
//     end
//
// is initially converted to
//
//     // Intermediate
//     logic [1:0] unpacked_array_var0 /*verilator split_var*/;
//     logic [1:0] unpacked_array_var1 /*verilator split_var*/;
//     always_comb begin
//        unpacked_array_var1[0] =  unpacked_array_var0[0];
//        unpacked_array_var1[1] = ~unpacked_array_var0[1];
//     end
//     logic [3:0] packed_var /*verilator split_var*/;
//     always_comb begin
//        if (some_cond) begin
//            packed_var = 4'b0;
//        end else begin
//            packed_var[3]   = some_input0;
//            packed_var[2:0] = some_input1;
//        end
//     end
//
//  then converted to
//
//     // Final
//     logic unpacked_array_var0__BRA__0__KET__;
//     logic unpacked_array_var0__BRA__1__KET__;
//     logic unpacked_array_var1__BRA__0__KET__;
//     logic unpacked_array_var1__BRA__1__KET__;
//     always_comb begin
//        unpacked_array_var1__BRA__0__KET__ =  unpacked_array_var0__BRA__0__KET__;
//        unpacked_array_var1__BRA__1__KET__ = ~unpacked_array_var0__BRA__1__KET__;
//     end
//     logic       packed_var__BRA__3__KET__;
//     logic [2:0] packed_var__BRA__2_0__KET__;
//     always_comb begin
//        if (some_cond) begin
//            {packed_var__BRA__3__KET__, packed_var__BRA__2_0__KET__} = 4'b0;
//        end else begin
//            packed_var__BRA__3__KET__   = some_input0;
//            packed_var__BRA__2_0__KET__ = some_input1;
//        end
//     end
//
//
// Two visitor classes are defined here, SplitUnpackedVarVisitor and SplitPackedVarVisitor.
//
// - SplitUnpackedVarVisitor class splits unpacked arrays. ( 1) in the explanation above.)
//   "unpacked_array_var" in the example above is a target of the class.
//   The class changes AST from "Original" to "Intermediate".
//   The visitor does not change packed variables.
//   In addition to splitting unpacked arrays, the visitor collects the following information
//   for each module.
//     - AstVar
//     - AstVarRef
//     - AstSel
//   They are stored in a RefsInModule instance and will be used in SplitPackedVarVisitor.
//
// - SplitPackedVarVisitor class splits packed variables ( 2), 3), 4), and 5) in the explanation
//   above.)
//   "unpacked_array0", "unpacked_array_var1", and "packed_var" in "Intermediate" are split by the
//   class.
//   Packed variables here include the result of SplitUnpackedVarVisitor.
//   The result of this class looks like "Final" above.
//   The class visits just necessary AstNode based on RefsInModule collected in the preceding
//   SplitUnpackedVarVisitor.
//   The visitor does not have to visit the entire AST because the necessary information is
//   already in RefsInModule.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SplitVar.h"

#include "V3AstUserAllocator.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

struct SplitVarImpl VL_NOT_FINAL {
    // NODE STATE
    //  AstNodeModule::user1()  -> Block number counter for generating unique names
    const VNUser1InUse m_user1InUse;  // Only used in SplitUnpackedVarVisitor

    static AstNodeAssign* newAssign(FileLine* fileline, AstNodeExpr* lhsp, AstNodeExpr* rhsp,
                                    const AstVar* varp) {
        if (varp->isFuncLocal() || varp->isFuncReturn()) {
            return new AstAssign{fileline, lhsp, rhsp};
        } else {
            return new AstAssignW{fileline, lhsp, rhsp};
        }
    }

    // These check functions return valid pointer to the reason text if a variable cannot be split.

    // Check if a var type can be split
    static const char* cannotSplitVarTypeReason(VVarType type) {
        // Only SplitUnpackedVarVisitor can split WREAL. SplitPackedVarVisitor cannot.
        const bool ok
            = type == type.VAR || type == type.WIRE || type == type.PORT || type == type.WREAL;
        if (ok) return nullptr;
        return "it is not one of variable, net, port, nor wreal";
    }

    static const char* cannotSplitVarDirectionReason(VDirection dir) {
        if (dir == VDirection::REF) return "it is a ref argument";
        if (dir == VDirection::INOUT) return "it is an inout port";
        return nullptr;
    }

    static const char* cannotSplitConnectedPortReason(const AstPin* pinp) {
        const AstVar* const varp = pinp->modVarp();
        if (!varp) return "it is not connected";
        if (const char* const reason = cannotSplitVarDirectionReason(varp->direction())) {
            return reason;
        }
        return nullptr;
    }

    static const char* cannotSplitTaskReason(const AstNodeFTask* taskp) {
        if (taskp->prototype()) return "the task is prototype declaration";
        if (taskp->dpiImport()) return "the task is imported from DPI-C";
        if (taskp->dpiOpenChild()) return "the task takes DPI-C open array";
        return nullptr;
    }

    static const char* cannotSplitVarCommonReason(const AstVar* varp) {
        if (const AstNodeFTask* const taskp = VN_CAST(varp->backp(), NodeFTask)) {
            if (const char* const reason = cannotSplitTaskReason(taskp)) return reason;
        }
        if (const char* const reason = cannotSplitVarTypeReason(varp->varType())) {
            return reason;
        }
        if (const char* const reason = cannotSplitVarDirectionReason(varp->direction())) {
            return reason;
        }
        if (varp->isSigPublic()) return "it is public";
        if (varp->isUsedLoopIdx()) return "it is used as a loop variable";
        if (varp->isForceable()) return "it is forceable";
        return nullptr;
    }

    static const char* cannotSplitPackedVarReason(const AstVar* varp);

    template <typename T_ALWAYSLIKE>
    void insertBeginCore(T_ALWAYSLIKE* ap, AstNodeStmt* stmtp, AstNodeModule* modp) {
        if (ap->isJustOneBodyStmt() && ap->stmtsp() == stmtp) {
            stmtp->unlinkFrBack();
            // Insert begin-end because temp value may be inserted to this block later.
            const std::string name = "__VsplitVarBlk" + cvtToStr(modp->user1Inc(1));
            ap->addStmtsp(new AstBegin{ap->fileline(), name, stmtp});
        }
    }

    void insertBeginCore(AstInitial* initp, AstNodeStmt* stmtp, AstNodeModule* modp) {
        if (initp->isJustOneBodyStmt() && initp->stmtsp() == stmtp) {
            stmtp->unlinkFrBack();
            // Insert begin-end because temp value may be inserted to this block later.
            FileLine* const fl = initp->fileline();
            const std::string name = "__VsplitVarBlk" + cvtToStr(modp->user1Inc(1));
            initp->replaceWith(new AstInitial{fl, new AstBegin{fl, name, stmtp}});
            VL_DO_DANGLING(initp->deleteTree(), initp);
        }
    }

    void insertBeginIfNecessary(AstNodeStmt* stmtp, AstNodeModule* modp) {
        AstNode* const backp = stmtp->backp();
        if (AstAlways* const ap = VN_CAST(backp, Always)) {
            insertBeginCore(ap, stmtp, modp);
        } else if (AstAlwaysPublic* const ap = VN_CAST(backp, AlwaysPublic)) {
            insertBeginCore(ap, stmtp, modp);
        } else if (AstInitial* const ap = VN_CAST(backp, Initial)) {
            insertBeginCore(ap, stmtp, modp);
        } else if (auto* const ap = VN_CAST(backp, Initial)) {
            insertBeginCore(ap, stmtp, modp);
        }
    }

};  // SplitVarImpl

//######################################################################
// Utilities required in various placs

static void warnNoSplit(const AstVar* varp, const AstNode* wherep, const char* reasonp) {
    wherep->v3warn(SPLITVAR, varp->prettyNameQ()
                                 << " has split_var metacomment but will not be split because "
                                 << reasonp << ".\n");
}

//######################################################################
// Split Unpacked Variables
// Replacement policy:
// AstArraySel  -> Just replace with the AstVarRef for the split variable
// AstVarRef    -> Create a temporary variable and refer the variable
// AstSliceSel  -> Create a temporary variable and refer the variable

// Compare AstNode* to get deterministic ordering when showing messages.
struct AstNodeComparator final {
    bool operator()(const AstNode* ap, const AstNode* bp) const {
        const int lineComp = ap->fileline()->operatorCompare(*bp->fileline());
        if (lineComp != 0) return lineComp < 0;
        return ap < bp;
    }
};

class UnpackRef final {
    // m_nodep is called in this context (AstNodeStmt, AstCell, AstNodeFTask, or AstAlways)
    AstNode* const m_contextp;
    AstNode* const m_nodep;  // ArraySel, SliceSel, ArrayVarRef (entire value)
    const int m_index;  // for ArraySel
    const int m_msb;  // for SliceSel
    const int m_lsb;  // for SliceSel
    const VAccess m_access;
    const bool m_ftask;  // true if the reference is in function/task. false if in module.
public:
    UnpackRef(AstNode* stmtp, AstVarRef* nodep, bool ftask)
        : m_contextp{stmtp}
        , m_nodep{nodep}
        , m_index{-1}
        , m_msb{0}
        , m_lsb{1}
        , m_access{nodep->access()}
        , m_ftask{ftask} {}
    UnpackRef(AstNode* stmtp, AstArraySel* nodep, int idx, const VAccess& access, bool ftask)
        : m_contextp{stmtp}
        , m_nodep{nodep}
        , m_index{idx}
        , m_msb{0}
        , m_lsb{1}
        , m_access{access}
        , m_ftask{ftask} {}
    UnpackRef(AstNode* stmtp, AstSliceSel* nodep, int msb, int lsb, const VAccess& access,
              bool ftask)
        : m_contextp{stmtp}
        , m_nodep{nodep}
        , m_index{msb == lsb ? msb : -1}  // Equivalent to ArraySel
        , m_msb{msb}
        , m_lsb{lsb}
        , m_access{access}
        , m_ftask{ftask} {}
    AstNode* nodep() const { return m_nodep; }
    bool isSingleRef() const {
        return VN_IS(m_nodep, ArraySel) || (m_msb == m_lsb && m_lsb == m_index);
    }
    int index() const {
        UASSERT_OBJ(isSingleRef(), m_nodep, "not array sel");
        return m_index;
    }
    AstNode* context() const { return m_contextp; }
    VAccess access() const { return m_access; }
    bool ftask() const { return m_ftask; }
    bool operator<(const UnpackRef& other) const {
        return AstNodeComparator()(m_nodep, other.m_nodep);
    }
};

class UnpackRefMap final {
public:
    using MapType = std::map<AstVar*, std::set<UnpackRef>, AstNodeComparator>;
    using MapIt = MapType::iterator;

private:
    MapType m_map;
    bool addCore(AstVarRef* refp, const UnpackRef& ref) {
        AstVar* const varp = refp->varp();
        UASSERT_OBJ(varp->attrSplitVar(), varp, " no split_var metacomment");
        const MapIt it = m_map.find(varp);
        if (it == m_map.end()) return false;  // Not registered
        const bool ok = it->second.insert(ref).second;
        return ok;
    }

public:
    // Register a variable to split
    void registerVar(AstVar* varp) {
        const bool inserted = m_map.emplace(varp, MapType::value_type::second_type()).second;
        UASSERT_OBJ(inserted, varp, "already registered");
    }
    // Register the location where a variable is used.
    bool tryAdd(AstNode* context, AstVarRef* refp, AstArraySel* selp, int idx, bool ftask) {
        return addCore(refp, UnpackRef{context, selp, idx, refp->access(), ftask});
    }
    bool tryAdd(AstNode* context, AstVarRef* refp, AstSliceSel* selp, int msb, int lsb,
                bool ftask) {
        return addCore(refp, UnpackRef{context, selp, msb, lsb, refp->access(), ftask});
    }
    bool tryAdd(AstNode* context, AstVarRef* refp, bool ftask) {
        return addCore(refp, UnpackRef{context, refp, ftask});
    }

    // Remove a variable from the list to split
    void remove(AstVar* varp) {
        UASSERT_OBJ(varp->attrSplitVar(), varp, " no split_var metacomment");
        m_map.erase(varp);
        varp->attrSplitVar(!SplitVarImpl::cannotSplitPackedVarReason(varp));
    }
    bool empty() const { return m_map.empty(); }
    void swap(UnpackRefMap& other) { other.m_map.swap(m_map); }

    MapIt begin() { return m_map.begin(); }
    MapIt end() { return m_map.end(); }
};

// Found nodes for SplitPackedVarVisitor
struct RefsInModule final {
    std::set<AstVar*, AstNodeComparator> m_vars;
    std::set<AstVarRef*, AstNodeComparator> m_refs;
    std::set<AstSel*, AstNodeComparator> m_sels;

public:
    void add(AstVar* nodep) { m_vars.insert(nodep); }
    void add(AstVarRef* nodep) { m_refs.insert(nodep); }
    void add(AstSel* nodep) { m_sels.insert(nodep); }
    void remove(AstNode* nodep) {
        struct Visitor final : public VNVisitor {
            RefsInModule& m_parent;
            void visit(AstNode* nodep) override { iterateChildren(nodep); }
            void visit(AstVar* nodep) override { m_parent.m_vars.erase(nodep); }
            void visit(AstVarRef* nodep) override { m_parent.m_refs.erase(nodep); }
            void visit(AstSel* nodep) override {
                m_parent.m_sels.erase(nodep);
                iterateChildren(nodep);
            }
            explicit Visitor(RefsInModule& p)
                : m_parent(p) {}  // Need () or GCC 4.8 false warning
        } v(*this);
        v.iterate(nodep);
    }
    void visit(VNVisitor* visitor) {
        for (AstVar* const varp : m_vars) visitor->iterate(varp);
        for (AstSel* const selp : m_sels) {
            // If m_refs includes VarRef from ArraySel, remove it
            // because the VarRef would not be visited in SplitPackedVarVisitor::visit(AstSel*).
            if (AstVarRef* const refp = VN_CAST(selp->fromp(), VarRef)) {
                m_refs.erase(refp);
            } else if (AstVarRef* const refp = VN_CAST(selp->lsbp(), VarRef)) {
                m_refs.erase(refp);
            }
            UASSERT_OBJ(reinterpret_cast<uintptr_t>(selp->op1p()) != 1, selp, "stale");
            visitor->iterate(selp);
        }
        for (AstVarRef* const vrefp : m_refs) {
            UASSERT_OBJ(reinterpret_cast<uintptr_t>(vrefp->op1p()) != 1, vrefp, "stale");
            visitor->iterate(vrefp);
        }
    }
};

struct SplitVarRefs final {
    std::map<AstNodeModule*, RefsInModule, AstNodeComparator> m_refs;
    std::unordered_set<AstVar*> m_hasXref;
};

class SplitUnpackedVarVisitor final : public VNVisitor, public SplitVarImpl {
    using VarSet = std::set<AstVar*, AstNodeComparator>;
    VarSet m_foundTargetVar;
    UnpackRefMap m_refs;
    AstNodeModule* m_modp = nullptr;
    // AstNodeStmt, AstCell, or AstAlways(Public) for sensitivity
    AstNode* m_contextp = nullptr;
    const AstNodeFTask* m_inFTaskp = nullptr;
    size_t m_numSplit = 0;
    // List for SplitPackedVarVisitor
    SplitVarRefs m_forPackedSplit;
    V3UniqueNames m_tempNames;  // For generating unique temporary variable names

    static AstVarRef* isTargetVref(AstNode* nodep) {
        if (AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            if (refp->varp()->attrSplitVar()) return refp;
        }
        return nullptr;
    }
    static int outerMostSizeOfUnpackedArray(const AstVar* nodep) {
        const AstUnpackArrayDType* const dtypep
            = VN_AS(nodep->dtypep()->skipRefp(), UnpackArrayDType);
        UASSERT_OBJ(dtypep, nodep, "Must be unapcked array");
        return dtypep->elementsConst();
    }

    void setContextAndIterateChildren(AstNode* nodep) {
        VL_RESTORER(m_contextp);
        {
            m_contextp = nodep;
            iterateChildren(nodep);
        }
    }
    void setContextAndIterate(AstNode* contextp, AstNode* nodep) {
        VL_RESTORER(m_contextp);
        {
            m_contextp = contextp;
            iterate(nodep);
        }
    }
    void pushDeletep(AstNode* nodep) {  // overriding VNVisitor::pusDeletep()
        UASSERT_OBJ(m_modp, nodep, "Must not nullptr");
        m_forPackedSplit.m_refs[m_modp].remove(nodep);
        VNVisitor::pushDeletep(nodep);
    }
    AstVar* newVar(FileLine* fl, VVarType type, const std::string& name, AstNodeDType* dtp) {
        AstVar* const varp = new AstVar{fl, type, name, dtp};
        UASSERT_OBJ(m_modp, varp, "Must not nullptr");
        m_forPackedSplit.m_refs[m_modp].add(varp);
        return varp;
    }
    AstVarRef* newVarRef(FileLine* fl, AstVar* varp, const VAccess& access) {
        AstVarRef* const refp = new AstVarRef{fl, varp, access};
        UASSERT_OBJ(m_modp, refp, "Must not nullptr");
        m_forPackedSplit.m_refs[m_modp].add(refp);
        return refp;
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }
    void visit(AstNodeModule* nodep) override {
        UINFO(4, "Start checking " << nodep->prettyNameQ());
        if (!VN_IS(nodep, Module)) {
            UINFO(4, "Skip " << nodep->prettyNameQ());
            return;
        }
        UASSERT_OBJ(!m_modp, m_modp, "Nested module declaration");
        UASSERT_OBJ(m_refs.empty(), nodep, "The last module didn't finish split()");
        VL_RESTORER(m_modp);
        m_modp = nodep;
        m_tempNames.reset();
        iterateChildren(nodep);
        split();
    }
    void visit(AstNodeStmt* nodep) override { setContextAndIterateChildren(nodep); }
    void visit(AstCell* nodep) override { setContextAndIterateChildren(nodep); }
    void visit(AstAlways* nodep) override {
        if (nodep->sensesp()) {  // When visiting sensitivity list, always is the context
            setContextAndIterate(nodep, nodep->sensesp());
        }
        for (AstNode* bodysp = nodep->stmtsp(); bodysp; bodysp = bodysp->nextp()) {
            iterate(bodysp);
        }
    };
    void visit(AstAlwaysPublic* nodep) override {
        if (nodep->sensesp()) {  // When visiting sensitivity list, always is the context
            setContextAndIterate(nodep, nodep->sensesp());
        }
        for (AstNode* bodysp = nodep->stmtsp(); bodysp; bodysp = bodysp->nextp()) {
            iterate(bodysp);
        }
    }
    void visit(AstNodeFTaskRef* nodep) override {
        const AstNodeFTask* const ftaskp = nodep->taskp();
        UASSERT_OBJ(ftaskp, nodep, "Unlinked");
        // Iterate arguments of a function/task.
        for (AstNode *argp = nodep->pinsp(), *paramp = ftaskp->stmtsp(); argp;
             argp = argp->nextp(), paramp = paramp ? paramp->nextp() : nullptr) {
            const char* reason = nullptr;
            const AstVar* vparamp = nullptr;
            while (paramp) {
                vparamp = VN_CAST(paramp, Var);
                if (vparamp && vparamp->isIO()) {
                    reason = cannotSplitVarDirectionReason(vparamp->direction());
                    break;
                }
                paramp = paramp->nextp();
                vparamp = nullptr;
            }
            if (!reason && !vparamp) {
                reason = "the number of argument to the task/function mismatches";
            }
            m_foundTargetVar.clear();
            iterate(argp);
            if (reason) {
                for (AstVar* const varp : m_foundTargetVar) {
                    warnNoSplit(varp, argp, reason);
                    m_refs.remove(varp);
                }
            }
            m_foundTargetVar.clear();
        }
    }
    void visit(AstPin* nodep) override {
        UINFO(5, nodep->modVarp()->prettyNameQ() << " pin ");
        AstNode* const exprp = nodep->exprp();
        if (!exprp) return;  // Not connected pin
        m_foundTargetVar.clear();
        iterate(exprp);
        if (const char* const reason = cannotSplitConnectedPortReason(nodep)) {
            for (AstVar* const varp : m_foundTargetVar) {
                warnNoSplit(varp, nodep, reason);
                m_refs.remove(varp);
            }
            m_foundTargetVar.clear();
        }
    }
    void visit(AstNodeFTask* nodep) override {
        UASSERT_OBJ(!m_inFTaskp, nodep, "Nested func/task");
        if (!cannotSplitTaskReason(nodep)) {
            VL_RESTORER(m_inFTaskp);
            m_inFTaskp = nodep;
            iterateChildren(nodep);
        }
    }
    void visit(AstVar* nodep) override {
        m_forPackedSplit.m_refs[m_modp].add(nodep);
        if (!nodep->attrSplitVar()) return;  // Nothing to do
        if (!cannotSplitReason(nodep)) {
            m_refs.registerVar(nodep);
            UINFO(4, nodep->name() << " is added to candidate list.");
        }
    }
    void visit(AstVarRef* nodep) override {
        m_forPackedSplit.m_refs[m_modp].add(nodep);
        if (!nodep->varp()->attrSplitVar()) return;  // Nothing to do
        if (m_refs.tryAdd(m_contextp, nodep, m_inFTaskp)) {
            m_foundTargetVar.insert(nodep->varp());
        }
    }
    void visit(AstVarXRef* nodep) override {
        UINFO(4, nodep->varp() << " Has hierarchical reference");
        m_forPackedSplit.m_hasXref.emplace(nodep->varp());
    }
    void visit(AstSel* nodep) override {
        if (VN_IS(nodep->fromp(), VarRef)) m_forPackedSplit.m_refs[m_modp].add(nodep);
        iterateChildren(nodep);
    }
    void visit(AstArraySel* nodep) override {
        if (AstVarRef* const refp = isTargetVref(nodep->fromp())) {
            const AstConst* const indexp = VN_CAST(nodep->bitp(), Const);
            if (indexp) {  // OK
                UINFO(4, "add " << nodep << " for " << refp->varp()->prettyName());
                if (indexp->toSInt() < outerMostSizeOfUnpackedArray(refp->varp())) {
                    m_refs.tryAdd(m_contextp, refp, nodep, indexp->toSInt(), m_inFTaskp);
                } else {
                    warnNoSplit(refp->varp(), nodep->bitp(), "index is out of range");
                    m_refs.remove(refp->varp());
                }
            } else {
                warnNoSplit(refp->varp(), nodep->bitp(), "index cannot be determined statically");
                m_refs.remove(refp->varp());
                iterate(nodep->bitp());
            }
        } else {
            iterateChildren(nodep);
        }
    }
    void visit(AstSliceSel* nodep) override {
        if (AstVarRef* const refp = isTargetVref(nodep->fromp())) {
            const AstUnpackArrayDType* const dtypep
                = VN_AS(refp->varp()->dtypep()->skipRefp(), UnpackArrayDType);
            // declRange() of AstSliceSel is shifted by dtypep->declRange().lo() in V3WidthSel.cpp
            // restore the original decl range here.
            const VNumRange selRange{nodep->declRange().hi() + dtypep->declRange().lo(),
                                     nodep->declRange().lo() + dtypep->declRange().lo(),
                                     nodep->declRange().ascending()};
            UASSERT_OBJ(dtypep->lo() <= selRange.lo() && selRange.hi() <= dtypep->hi(), nodep,
                        "Range check for AstSliceSel must have been finished in V3Width.cpp");
            UINFO(4, "add " << nodep << " for " << refp->varp()->prettyName());
            m_refs.tryAdd(m_contextp, refp, nodep, nodep->declRange().hi(),
                          nodep->declRange().lo(), m_inFTaskp);
        } else {
            iterateChildren(nodep);
        }
    }
    AstVarRef* createTempVar(AstNode* context, AstNode* nodep, AstUnpackArrayDType* dtypep,
                             const std::string& name_prefix, std::vector<AstVar*>& vars,
                             int start_idx, bool lvalue, bool /*ftask*/) {
        FileLine* const fl = nodep->fileline();
        const std::string name = m_tempNames.get(nodep) + "__" + name_prefix;
        AstNodeAssign* const assignp = VN_CAST(context, NodeAssign);
        if (assignp) {
            // "always_comb a = b;" to "always_comb begin a = b; end" so that local
            // variable can be added.
            insertBeginIfNecessary(assignp, m_modp);
        }
        AstVar* const varp = newVar(fl, VVarType::VAR, name, dtypep);
        // Variable will be registered in the caller side.
        UINFO(4, varp->prettyNameQ()
                     << " is created lsb:" << dtypep->lo() << " msb:" << dtypep->hi());
        // Use AstAssign if true, otherwise AstAssignW
        const bool use_simple_assign
            = (context && VN_IS(context, NodeFTaskRef)) || (assignp && VN_IS(assignp, Assign));

        for (int i = 0; i < dtypep->elementsConst(); ++i) {
            AstNodeExpr* lhsp
                = newVarRef(fl, vars.at(start_idx + i), lvalue ? VAccess::WRITE : VAccess::READ);
            AstNodeExpr* rhsp = new AstArraySel{
                fl, newVarRef(fl, varp, !lvalue ? VAccess::WRITE : VAccess::READ), i};
            AstNode* const refp = lhsp;
            UINFO(9, "Creating assign idx:" << i << " + " << start_idx);
            if (!lvalue) std::swap(lhsp, rhsp);
            AstNode* newassignp;
            if (use_simple_assign) {
                AstNode* const insertp = context;
                newassignp = new AstAssign{fl, lhsp, rhsp};
                if (lvalue) {
                    // If varp is LHS, this assignment must appear after the original
                    // assignment(context).
                    insertp->addNextHere(newassignp);
                } else {
                    // If varp is RHS, this assignment comes just before the original assignment
                    insertp->addHereThisAsNext(newassignp);
                }
            } else {
                newassignp = new AstAssignW{fl, lhsp, rhsp};
                // Continuous assignment must be in module context.
                varp->addNextHere(newassignp);
            }
            UASSERT_OBJ(!m_contextp, m_contextp, "must be null");
            setContextAndIterate(newassignp, refp);
        }
        return newVarRef(fl, varp, lvalue ? VAccess::WRITE : VAccess::READ);
    }
    void connectPort(AstVar* varp, std::vector<AstVar*>& vars, AstNode* insertp) {
        UASSERT_OBJ(varp->isIO(), varp, "must be port");
        const bool lvalue = varp->direction().isWritable();
        FileLine* const fl = varp->fileline();
        for (size_t i = 0; i < vars.size(); ++i) {
            AstNodeExpr* const nodes[] = {
                new AstArraySel{fl, newVarRef(fl, varp, lvalue ? VAccess::WRITE : VAccess::READ),
                                static_cast<int>(i)},
                newVarRef(fl, vars.at(i), !lvalue ? VAccess::WRITE : VAccess::READ)};
            AstNodeExpr* const lhsp = nodes[lvalue ? 0 : 1];
            AstNodeExpr* const rhsp = nodes[lvalue ? 1 : 0];
            AstNodeAssign* const assignp = newAssign(fl, lhsp, rhsp, varp);
            if (insertp) {
                if (lvalue) {  // Just after writing to the temporary variable
                    insertp->addNextHere(assignp);
                } else {  // Just before reading the temporary variable
                    insertp->addHereThisAsNext(assignp);
                }
            } else {
                UASSERT_OBJ(VN_IS(assignp, AssignW), varp, "must be AssginW");
                vars.at(i)->addNextHere(assignp);
            }
            setContextAndIterate(assignp, nodes[1]);
        }
    }
    // cppcheck-has-bug-suppress constParameter
    size_t collapse(UnpackRefMap& refs) {
        size_t numSplit = 0;
        for (const auto& pair : refs) {
            UINFO(4, "In module " << m_modp->name() << " var " << pair.first->prettyNameQ()
                                  << " which has " << pair.second.size()
                                  << " refs will be split.");
            AstVar* const varp = pair.first;
            AstNode* insertp = varp;
            const AstUnpackArrayDType* const dtypep
                = VN_AS(varp->dtypep()->skipRefp(), UnpackArrayDType);
            AstNodeDType* const subTypep = dtypep->subDTypep();
            const bool needNext = VN_IS(subTypep, UnpackArrayDType);  // Still unpacked array.
            std::vector<AstVar*> vars;
            // Add the split variables
            for (int32_t i = 0; i < dtypep->elementsConst(); ++i) {
                // Unpacked array is traced as var(idx), not var[idx].
                const std::string name
                    = varp->name() + AstNode::encodeName('(' + cvtToStr(i + dtypep->lo()) + ')');
                AstVar* const newp = newVar(varp->fileline(), VVarType::VAR, name, subTypep);
                newp->propagateAttrFrom(varp);
                // If varp is an IO, varp will remain and will be traced.
                newp->trace(!varp->isIO() && varp->isTrace());
                newp->funcLocal(varp->isFuncLocal() || varp->isFuncReturn());
                insertp->addNextHere(newp);
                insertp = newp;
                newp->attrSplitVar(needNext || !cannotSplitPackedVarReason(newp));
                vars.push_back(newp);
                setContextAndIterate(nullptr, newp);
            }
            for (const UnpackRef& ref : pair.second) {
                AstNode* newp = nullptr;
                if (ref.isSingleRef()) {
                    newp = newVarRef(ref.nodep()->fileline(), vars.at(ref.index()), ref.access());
                } else {
                    AstVarRef* refp = VN_CAST(ref.nodep(), VarRef);
                    AstUnpackArrayDType* adtypep;
                    int lsb = 0;
                    if (refp) {
                        adtypep = VN_AS(refp->dtypep()->skipRefp(), UnpackArrayDType);
                    } else {
                        AstSliceSel* const selp = VN_AS(ref.nodep(), SliceSel);
                        refp = VN_AS(selp->fromp(), VarRef);
                        UASSERT_OBJ(refp, selp, "Unexpected op is registered");
                        adtypep = VN_AS(selp->dtypep()->skipRefp(), UnpackArrayDType);
                        lsb = adtypep->lo();
                    }
                    AstVarRef* const newrefp
                        = createTempVar(ref.context(), refp, adtypep, varp->name(), vars, lsb,
                                        refp->access(), ref.ftask());
                    newp = newrefp;
                    refp->varp()->addNextHere(newrefp->varp());
                    UINFO(4, "Create " << newrefp->varp()->prettyNameQ() << " for " << refp);
                }
                ref.nodep()->replaceWith(newp);
                pushDeletep(ref.nodep());
                setContextAndIterate(ref.context(), newp->backp());
                // AstAssign is used. So assignment is necessary for each reference.
                if (varp->isIO() && (varp->isFuncLocal() || varp->isFuncReturn()))
                    connectPort(varp, vars, ref.context());
            }
            if (varp->isIO()) {
                // AssignW will be created, so just once
                if (!varp->isFuncLocal() && !varp->isFuncReturn()) {
                    connectPort(varp, vars, nullptr);
                }
                varp->attrSplitVar(!cannotSplitPackedVarReason(varp));
                m_forPackedSplit.m_refs[m_modp].add(varp);
            } else {
                pushDeletep(varp->unlinkFrBack());
            }
            ++numSplit;
        }
        return numSplit;
    }
    void split() {
        for (int trial = 0; !m_refs.empty(); ++trial) {
            UnpackRefMap next;
            m_refs.swap(next);
            const size_t n = collapse(next);
            UINFO(4, n << " Variables are split " << trial << " th trial in "
                       << m_modp->prettyNameQ());
            if (trial == 0) m_numSplit += n;
        }
        doDeletes();
    }

public:
    explicit SplitUnpackedVarVisitor(AstNetlist* nodep)
        : m_tempNames{"__VsplitVar"} {
        iterate(nodep);
    }
    ~SplitUnpackedVarVisitor() override {
        UASSERT(m_refs.empty(), "Don't forget to call split()");
        V3Stats::addStat("SplitVar, unpacked arrays split due to attribute", m_numSplit);
    }
    const SplitVarRefs& getPackedVarRefs() const { return std::move(m_forPackedSplit); }

    // Check if the passed variable can be split.
    // Even if this function returns true, the variable may not be split
    // because the access to the variable cannot be determined statically.
    static const char* cannotSplitReason(const AstVar* nodep) {
        const std::pair<uint32_t, uint32_t> dim = nodep->dtypep()->dimensions(false);
        UINFO(7, nodep->prettyNameQ()
                     << " pub:" << nodep->isSigPublic() << " pri:" << nodep->isPrimaryIO()
                     << " io:" << nodep->isInout() << " typ:" << nodep->varType());
        const char* reason = nullptr;
        // Public variable cannot be split.
        // at least one unpacked dimension must exist
        if (dim.second < 1 || !VN_IS(nodep->dtypep()->skipRefp(), UnpackArrayDType))
            reason = "it is not an unpacked array";
        if (!reason) reason = cannotSplitVarCommonReason(nodep);
        if (reason) {
            UINFO(5, "Check " << nodep->prettyNameQ() << " cannot split because" << reason);
        }
        return reason;
    }
};

//######################################################################
//  Split packed variables

// Split variable
class SplitNewVar final {
    const int m_lsb;  // LSB in the original bitvector
    const int m_bitwidth;
    AstVar* m_varp;  // The LSB of this variable is always 0, not m_lsb
public:
    SplitNewVar(int lsb, int bitwidth, AstVar* varp = nullptr)
        : m_lsb{lsb}
        , m_bitwidth{bitwidth}
        , m_varp{varp} {}
    int lsb() const { return m_lsb; }
    int msb() const { return m_lsb + m_bitwidth - 1; }
    int bitwidth() const { return m_bitwidth; }
    void varp(AstVar* vp) {
        UASSERT_OBJ(!m_varp, m_varp, "must be nullptr");
        m_varp = vp;
    }
    AstVar* varp() const { return m_varp; }

    struct Match final {
        bool operator()(int bit, const SplitNewVar& a) const {
            return bit < a.m_lsb + a.m_bitwidth;
        }
    };
};

// One Entry instance for an AstVarRef instance
class PackedVarRefEntry final {
    AstNode* const m_nodep;  // Either AstSel or AstVarRef is expected.
    const int m_lsb;
    const int m_bitwidth;

public:
    PackedVarRefEntry(AstSel* selp, int lsb, int bitwidth)
        : m_nodep{selp}
        , m_lsb{lsb}
        , m_bitwidth{bitwidth} {}
    PackedVarRefEntry(AstVarRef* refp, int lsb, int bitwidth)
        : m_nodep{refp}
        , m_lsb{lsb}
        , m_bitwidth{bitwidth} {}
    AstNode* nodep() const { return m_nodep; }
    int lsb() const { return m_lsb; }
    int msb() const { return m_lsb + m_bitwidth - 1; }
    int bitwidth() const { return m_bitwidth; }
    void replaceNodeWith(AstNode* nodep) {
        m_nodep->replaceWith(nodep);
        VL_DO_DANGLING(m_nodep->deleteTree(), m_nodep);
    }
    // If this is AstVarRef and referred in the sensitivity list of always@,
    // return the sensitivity item
    AstSenItem* backSenItemp() const {
        if (const AstVarRef* const refp = VN_CAST(m_nodep, VarRef)) {
            return VN_CAST(refp->backp(), SenItem);
        }
        return nullptr;
    }
};

// How a variable is used
class PackedVarRef final {
    struct SortByFirst final {
        bool operator()(const std::pair<int, bool>& a, const std::pair<int, bool>& b) const {
            if (a.first == b.first) return a.second < b.second;
            return a.first < b.first;
        }
    };
    std::vector<PackedVarRefEntry> m_lhs, m_rhs;
    AstBasicDType* const m_basicp;  // Cache the ptr since varp->dtypep()->basicp() is expensive
    bool m_dedupDone = false;
    static void dedupRefs(std::vector<PackedVarRefEntry>& refs) {
        // Use raw pointer to dedup
        std::map<AstNode*, size_t, AstNodeComparator> nodes;
        for (size_t i = 0; i < refs.size(); ++i) nodes.emplace(refs[i].nodep(), i);
        std::vector<PackedVarRefEntry> vect;
        vect.reserve(nodes.size());
        for (const auto& pair : nodes) vect.push_back(refs[pair.second]);
        refs.swap(vect);
    }

public:
    using iterator = std::vector<PackedVarRefEntry>::iterator;
    using const_iterator = std::vector<PackedVarRefEntry>::const_iterator;
    std::vector<PackedVarRefEntry>& lhs() {
        UASSERT(m_dedupDone, "cannot read before dedup()");
        return m_lhs;
    }
    std::vector<PackedVarRefEntry>& rhs() {
        UASSERT(m_dedupDone, "cannot read before dedup()");
        return m_rhs;
    }
    explicit PackedVarRef(AstVar* varp)
        : m_basicp{varp->dtypep()->basicp()} {}
    void append(const PackedVarRefEntry& e, const VAccess& access) {
        UASSERT(!m_dedupDone, "cannot add after dedup()");
        if (access.isWriteOrRW()) m_lhs.push_back(e);
        if (access.isReadOrRW()) m_rhs.push_back(e);
    }
    void dedup() {
        UASSERT(!m_dedupDone, "dedup() called twice");
        dedupRefs(m_lhs);
        dedupRefs(m_rhs);
        m_dedupDone = true;
    }
    const AstBasicDType* basicp() const { return m_basicp; }
    // Make a plan for variables after split
    // when skipUnused==true, split variable for unread bits will not be created.
    std::vector<SplitNewVar> splitPlan(const AstVar* varp, bool skipUnused) const {
        UASSERT(m_dedupDone, "dedup() must be called before");
        AstNodeDType* const dtypep = varp->dtypeSkipRefp();
        std::vector<SplitNewVar> plan;
        std::vector<std::pair<int, bool>> points;  // <bit location, is end>
        points.reserve(m_lhs.size() * 2 + 2);  // 2 points will be added per one PackedVarRefEntry
        for (const PackedVarRefEntry& ref : m_lhs) {
            points.emplace_back(ref.lsb(), false);  // Start of a region
            points.emplace_back(ref.msb() + 1, true);  // End of a region
        }
        const int bit_lo = basicp()->lo();
        const int bit_hi = bit_lo + dtypep->width() - 1;
        if (skipUnused && !m_rhs.empty()) {  // Range to be read must be kept, so add points here
            int lsb = bit_hi + 1;
            int msb = bit_lo - 1;
            for (const PackedVarRefEntry& ref : m_rhs) {
                lsb = std::min(lsb, ref.lsb());
                msb = std::max(msb, ref.msb());
            }
            UASSERT_OBJ(lsb <= msb, m_basicp, "lsb:" << lsb << " msb:" << msb << " are wrong");
            points.emplace_back(lsb, false);
            points.emplace_back(msb + 1, true);
        }
        if (!skipUnused) {  // All bits are necessary
            points.emplace_back(bit_lo, false);
            points.emplace_back(bit_hi + 1, true);
        }
        std::sort(points.begin(), points.end(), SortByFirst());

        // Scan the sorted points and sub bitfields
        int refcount = 0;
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const int bitwidth = points[i + 1].first - points[i].first;
            if (points[i].second) {
                --refcount;  // End of a region
            } else {
                ++refcount;  // Start of a region
            }
            UASSERT(refcount >= 0, "refcounut must not be negative");
            if (bitwidth == 0 || refcount == 0) continue;  // Vacant region
            plan.emplace_back(SplitNewVar{points[i].first, bitwidth});
        }

        return plan;
    }
};

class SplitPackedVarVisitor final : public VNVisitor, public SplitVarImpl {
    // NODE STATE
    //  AstVar::user2()  -> bool. Automatically considered candidate
    //  AstVar::user3()  -> VarInfo. Used only in findCandidates
    const VNUser2InUse m_user2InUse;

    AstNetlist* const m_netp;
    const AstNodeModule* m_modp = nullptr;  // Current module (just for log)
    int m_numSplitAttr = 0;  // Number of variables split due to attribute
    int m_numSplitAuto = 0;  // Number of variables split automatically
    // key:variable to be split. value:location where the variable is referenced.
    std::map<AstVar*, PackedVarRef, AstNodeComparator> m_refs;
    void visit(AstNodeFTask* nodep) override {
        if (!cannotSplitTaskReason(nodep)) iterateChildren(nodep);
    }
    void visit(AstVar* nodep) override {
        if (!nodep->attrSplitVar() && !nodep->user2()) return;  // Nothing to do
        if (const char* const reason = cannotSplitReason(nodep, true)) {
            if (nodep->attrSplitVar()) {
                warnNoSplit(nodep, nodep, reason);
                nodep->attrSplitVar(false);
            }
        } else {  // Finally find a good candidate
            const bool inserted = m_refs.emplace(nodep, PackedVarRef{nodep}).second;
            if (inserted) UINFO(4, nodep->prettyNameQ() << " is added to candidate list.");
        }
    }
    void visit(AstVarRef* nodep) override {
        AstVar* const varp = nodep->varp();
        visit(varp);
        const auto refit = m_refs.find(varp);
        if (refit == m_refs.end()) return;  // variable without split_var metacomment
        UASSERT_OBJ(varp->attrSplitVar() || varp->user2(), varp, "must be a split candidate");
        UASSERT_OBJ(!nodep->classOrPackagep(), nodep,
                    "variable in package must have been dropped beforehand.");
        const AstBasicDType* const basicp = refit->second.basicp();
        refit->second.append(PackedVarRefEntry{nodep, basicp->lo(), varp->width()},
                             nodep->access());
        UINFO(5, varp->prettyName()
                     << " Entire bit of [" << basicp->lo() << "+:" << varp->width() << "]");
    }
    void visit(AstSel* nodep) override {
        const AstVarRef* const vrefp = VN_CAST(nodep->fromp(), VarRef);
        if (!vrefp) {
            iterateChildren(nodep);
            return;
        }

        AstVar* const varp = vrefp->varp();
        const auto refit = m_refs.find(varp);
        if (refit == m_refs.end()) {
            iterateChildren(nodep);
            return;  // Variable without split_var metacomment
        }
        UASSERT_OBJ(varp->attrSplitVar() || varp->user2(), varp, "must be a split candidate");

        if (AstConst* const lsbConstp = VN_CAST(nodep->lsbp(), Const)) {  // OK
            refit->second.append(
                PackedVarRefEntry(nodep, lsbConstp->toSInt() + refit->second.basicp()->lo(),
                                  nodep->widthConst()),
                vrefp->access());
            UINFO(5, varp->prettyName()
                         << " [" << lsbConstp->toSInt() << ":+" << nodep->widthConst()
                         << "] lsb:" << refit->second.basicp()->lo());
        } else {
            if (varp->attrSplitVar()) {
                warnNoSplit(vrefp->varp(), nodep, "its bit range cannot be determined statically");
                varp->attrSplitVar(false);
            }
            m_refs.erase(varp);
            iterateChildren(nodep);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

    // Extract necessary bit range from a newly created variable to meet ref
    static AstNodeExpr* extractBits(const PackedVarRefEntry& ref, const SplitNewVar& var,
                                    const VAccess access) {
        FileLine* const fl = ref.nodep()->fileline();
        AstVarRef* const refp = new AstVarRef{fl, var.varp(), access};
        if (ref.lsb() <= var.lsb() && var.msb() <= ref.msb()) {  // Use the entire bits
            return refp;
        } else {  // Use slice
            const int lsb = std::max(ref.lsb(), var.lsb());
            const int msb = std::min(ref.msb(), var.msb());
            const int bitwidth = msb + 1 - lsb;
            UINFO(4, var.varp()->prettyNameQ() << "[" << msb << ":" << lsb << "] used for "
                                               << ref.nodep()->prettyNameQ() << '\n');
            // LSB of varp is always 0. "lsb - var.lsb()" means this. see also SplitNewVar
            return new AstSel{fl, refp, lsb - var.lsb(), bitwidth};
        }
    }
    static void connectPortAndVar(const std::vector<SplitNewVar>& vars, AstVar* portp,
                                  AstNode* insertp) {
        for (; insertp; insertp = insertp->backp()) {
            if (VN_IS(insertp, NodeStmt)) break;
        }
        const bool in = portp->isReadOnly();
        FileLine* const fl = portp->fileline();
        const int portLsb = portp->basicp()->lo();
        for (const SplitNewVar& var : vars) {
            AstNodeExpr* rhsp
                = new AstSel{fl, new AstVarRef{fl, portp, !in ? VAccess::WRITE : VAccess::READ},
                             var.lsb() - portLsb, var.bitwidth()};
            AstNodeExpr* lhsp = new AstVarRef{fl, var.varp(), in ? VAccess::WRITE : VAccess::READ};
            if (!in) std::swap(lhsp, rhsp);
            AstNodeAssign* const assignp = newAssign(fl, lhsp, rhsp, portp);
            if (insertp) {
                if (in) {
                    insertp->addHereThisAsNext(assignp);
                } else {
                    insertp->addNextHere(assignp);
                }
            } else {
                var.varp()->addNextHere(assignp);
            }
        }
    }
    void createVars(AstVar* varp, const AstBasicDType* basicp, std::vector<SplitNewVar>& vars) {
        for (SplitNewVar& newvar : vars) {
            int left = newvar.msb();
            int right = newvar.lsb();
            if (basicp->ascending()) std::swap(left, right);
            const std::string name
                = (left == right)
                      ? varp->name() + "__BRA__" + AstNode::encodeNumber(left) + "__KET__"
                      : varp->name() + "__BRA__" + AstNode::encodeNumber(left)
                            + AstNode::encodeName(":") + AstNode::encodeNumber(right) + "__KET__";

            AstBasicDType* dtypep;
            switch (basicp->keyword()) {
            case VBasicDTypeKwd::BIT:
                dtypep = new AstBasicDType{varp->subDTypep()->fileline(), VFlagBitPacked{},
                                           newvar.bitwidth()};
                break;
            case VBasicDTypeKwd::LOGIC:
                dtypep = new AstBasicDType{varp->subDTypep()->fileline(), VFlagLogicPacked{},
                                           newvar.bitwidth()};
                break;
            default: UASSERT_OBJ(false, basicp, "Only bit and logic are allowed");
            }
            dtypep->rangep(new AstRange{
                varp->fileline(), VNumRange{newvar.msb(), newvar.lsb(), basicp->ascending()}});
            newvar.varp(new AstVar{varp->fileline(), VVarType::VAR, name, dtypep});
            newvar.varp()->propagateAttrFrom(varp);
            newvar.varp()->funcLocal(varp->isFuncLocal() || varp->isFuncReturn());
            // Enable this line to trace split variable directly:
            // newvar.varp()->trace(varp->isTrace());
            m_netp->typeTablep()->addTypesp(dtypep);
            varp->addNextHere(newvar.varp());
            UINFO(4, newvar.varp()->prettyNameQ() << " is added for " << varp->prettyNameQ());
        }
    }
    static void updateReferences(AstVar* varp, PackedVarRef& pref,
                                 const std::vector<SplitNewVar>& vars) {
        for (const bool lvalue : {false, true}) {  // Refer the new split variables
            std::vector<PackedVarRefEntry>& refs = lvalue ? pref.lhs() : pref.rhs();
            for (PackedVarRefEntry& ref : refs) {
                auto varit
                    = std::upper_bound(vars.begin(), vars.end(), ref.lsb(), SplitNewVar::Match());
                UASSERT_OBJ(varit != vars.end(), ref.nodep(), "Not found");
                UASSERT(!(varit->msb() < ref.lsb() || ref.msb() < varit->lsb()),
                        "wrong search result");
                AstNode* prevp;
                bool inSentitivityList = false;
                if (AstSenItem* const senitemp = ref.backSenItemp()) {
                    AstNode* const oldsenrefp = senitemp->sensp();
                    oldsenrefp->replaceWith(
                        new AstVarRef{senitemp->fileline(), varit->varp(), VAccess::READ});
                    VL_DO_DANGLING(oldsenrefp->deleteTree(), oldsenrefp);
                    prevp = senitemp;
                    inSentitivityList = true;
                } else {
                    prevp = extractBits(ref, *varit, lvalue ? VAccess::WRITE : VAccess::READ);
                }
                for (int residue = ref.msb() - varit->msb(); residue > 0;
                     residue -= varit->bitwidth()) {
                    ++varit;
                    UASSERT_OBJ(varit != vars.end(), ref.nodep(), "not enough split variables");
                    if (AstSenItem* const senitemp = VN_CAST(prevp, SenItem)) {
                        prevp = new AstSenItem{
                            senitemp->fileline(), senitemp->edgeType(),
                            new AstVarRef{senitemp->fileline(), varit->varp(), VAccess::READ}};
                        senitemp->addNextHere(prevp);
                    } else {
                        AstNodeExpr* const bitsp
                            = extractBits(ref, *varit, lvalue ? VAccess::WRITE : VAccess::READ);
                        prevp = new AstConcat{ref.nodep()->fileline(), bitsp,
                                              VN_AS(prevp, NodeExpr)};
                    }
                }
                // If varp is an argument of task/func, need to update temporary var
                // every time the var is updated. See also another call of connectPortAndVar() in
                // split()
                if (varp->isIO() && (varp->isFuncLocal() || varp->isFuncReturn()))
                    connectPortAndVar(vars, varp, ref.nodep());
                if (!inSentitivityList) ref.replaceNodeWith(prevp);
                UASSERT_OBJ(varit->msb() >= ref.msb(), varit->varp(), "Out of range");
            }
        }
    }
    // Do the actual splitting operation
    void split() {
        for (auto& pair : m_refs) {
            AstVar* const varp = pair.first;
            PackedVarRef& ref = pair.second;
            ref.dedup();
            UINFO(4, "In module " << m_modp->name() << " var " << varp->prettyNameQ()
                                  << " which has " << ref.lhs().size() << " lhs refs and "
                                  << ref.rhs().size() << " rhs refs will be split.");
            std::vector<SplitNewVar> vars
                = ref.splitPlan(varp, !varp->isTrace());  // If traced, all bit must be kept
            if (vars.empty()) continue;
            if (vars.size() == 1 && vars.front().bitwidth() == varp->width())
                continue;  // No split

            if (varp->attrSplitVar()) {
                ++m_numSplitAttr;
            } else {
                ++m_numSplitAuto;
            }

            createVars(varp, ref.basicp(), vars);  // Add the split variables

            updateReferences(varp, ref, vars);

            if (varp->isIO()) {  // port cannot be deleted
                // If varp is a port of a module, single AssignW is sufficient
                if (!(varp->isFuncLocal() || varp->isFuncReturn()))
                    connectPortAndVar(vars, varp, nullptr);
            } else if (varp->isTrace()) {
                // Let's reuse the original variable for tracing
                AstNodeExpr* rhsp = new AstVarRef{vars.front().varp()->fileline(),
                                                  vars.front().varp(), VAccess::READ};
                FileLine* const fl = varp->fileline();
                for (size_t i = 1; i < vars.size(); ++i) {
                    rhsp = new AstConcat{fl, new AstVarRef{fl, vars[i].varp(), VAccess::READ},
                                         rhsp};
                }
                varp->addNextHere(
                    newAssign(fl, new AstVarRef{fl, varp, VAccess::WRITE}, rhsp, varp));
            } else {  // the original variable is not used anymore.
                VL_DO_DANGLING(varp->unlinkFrBack()->deleteTree(), varp);
            }
        }
        m_refs.clear();  // Done
    }

    // Find Vars only referenced through non-overlapping constant selects,
    // and set their user2 to mark them as split candidates
    static void findCandidates(const RefsInModule& refSets,
                               const std::unordered_set<AstVar*>& hasXrefs) {
        // Inclusive index range
        using Range = std::pair<int32_t, int32_t>;

        // Store one VarInfo per AstVar via user3
        struct VarInfo final {
            bool ineligible = false;  // Ineligible for automatic consideration
            std::vector<Range> ranges;  // [lsb, msb] inclusive of Sels
        };
        const VNUser3InUse user3InUse;
        AstUser3Allocator<AstVar, VarInfo> varInfos;

        // Gather all Sels selecting from each variable, also mark if ineligible
        for (const AstVarRef* const vrefp : refSets.m_refs) {
            AstVar* const varp = vrefp->varp();
            VarInfo& info = varInfos(varp);
            if (info.ineligible) continue;

            // Function return values seem not safe for splitting, even though
            // the code above seems like it's tryinig to handle them.
            if (varp->isFuncReturn()) {
                info.ineligible = true;
                continue;
            }
            // Don't consider ports, we don't know what is connected to them at this point
            if (varp->isIO()) {
                info.ineligible = true;
                continue;
            }
            // Can't split variables referenced from outside the module
            if (hasXrefs.count(varp)) {
                info.ineligible = true;
                continue;
            }

            // Ineligible if it is not being Sel from
            AstSel* const selp = VN_CAST(vrefp->firstAbovep(), Sel);
            if (!selp || vrefp != selp->fromp()) {
                info.ineligible = true;
                continue;
            }
            // Ineligible if the selection range is not constant
            AstConst* const lsbConstp = VN_CAST(selp->lsbp(), Const);
            if (!lsbConstp) {
                info.ineligible = true;
                continue;
            }

            // All good, record the selection range
            const int32_t lsb = lsbConstp->toSInt();
            const int32_t msb = lsb + selp->widthConst() - 1;
            info.ranges.emplace_back(lsb, msb);
        }

        // Check the usage of each variable
        for (AstVar* const varp : refSets.m_vars) {
            VarInfo* const infop = varInfos.tryGet(varp);
            if (!infop) continue;
            // Don't consider if ineligible
            if (infop->ineligible) continue;
            // Sort ranges by LSB then MSB
            std::sort(infop->ranges.begin(), infop->ranges.end(),
                      [](const Range& a, const Range& b) {
                          if (a.first != b.first) return a.first < b.first;
                          return a.second < b.second;
                      });
            // Check for overlapping but non-identical ranges
            bool overlap = false;
            for (size_t i = 0; i + 1 < infop->ranges.size(); ++i) {
                const Range& a = infop->ranges[i];
                const Range& b = infop->ranges[i + 1];
                // OK if the two ranges are the same
                if (a == b) continue;
                // OK if they don't overlap
                if (a.second < b.first) continue;
                // Overlap found
                overlap = true;
                break;
            }
            // If no overlapping ranges, consider it for automatic splitting
            varp->user2(!overlap);
        }
    }

public:
    // When reusing the information from SplitUnpackedVarVisitor
    SplitPackedVarVisitor(AstNetlist* nodep, SplitVarRefs fromUnpackedSplit)
        : m_netp{nodep} {
        // If you want ignore refs and walk the tne entire AST,
        // just call iterateChildren(m_modp) and split() for each module
        if (v3Global.opt.fVarSplit()) {
            for (const auto& i : fromUnpackedSplit.m_refs) {
                findCandidates(i.second, fromUnpackedSplit.m_hasXref);
            }
        }
        for (auto& i : fromUnpackedSplit.m_refs) {
            m_modp = i.first;
            i.second.visit(this);
            split();
            m_modp = nullptr;
        }
    }
    ~SplitPackedVarVisitor() override {
        UASSERT(m_refs.empty(), "Forgot to call split()");
        V3Stats::addStat("SplitVar, packed variables split due to attribute", m_numSplitAttr);
        V3Stats::addStat("SplitVar, packed variables split automatically", m_numSplitAuto);
    }

    // Check if the passed variable can be split.
    // Even if this function returns true, the variable may not be split
    // when the access to the variable cannot be determined statically.
    static const char* cannotSplitReason(const AstVar* nodep, bool checkUnpacked) {
        const char* reason = nullptr;
        if (const AstBasicDType* const basicp = nodep->dtypep()->basicp()) {
            const std::pair<uint32_t, uint32_t> dim = nodep->dtypep()->dimensions(false);
            // Unpacked array will be split in SplitUnpackedVarVisitor() beforehand
            if (!((!checkUnpacked || dim.second == 0) && nodep->dtypep()->widthMin() > 1))
                reason = "its bitwidth is 1";
            if (!reason && !basicp->isBitLogic())  // Floating point and string are not supported
                reason = "it is not an aggregate type of bit nor logic";
            if (!reason) reason = cannotSplitVarCommonReason(nodep);
        } else {
            reason = "its type is unknown";  // LCOV_EXCL_LINE
        }
        if (reason) {
            UINFO(5, "Check " << nodep->prettyNameQ() << " cannot split because" << reason);
        }
        return reason;
    }
};

const char* SplitVarImpl::cannotSplitPackedVarReason(const AstVar* varp) {
    return SplitPackedVarVisitor::cannotSplitReason(varp, true);
}

//######################################################################
// Split class functions

void V3SplitVar::splitVariable(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    SplitVarRefs refs;
    {
        const SplitUnpackedVarVisitor visitor{nodep};
        refs = visitor.getPackedVarRefs();
    }
    V3Global::dumpCheckGlobalTree("split_var", 0, dumpTreeEitherLevel() >= 9);
    { SplitPackedVarVisitor{nodep, std::move(refs)}; }
    V3Global::dumpCheckGlobalTree("split_var", 0, dumpTreeEitherLevel() >= 9);
}

bool V3SplitVar::canSplitVar(const AstVar* varp) {
    // If either SplitUnpackedVarVisitor or SplitPackedVarVisitor can handle,
    // then accept varp.
    return !SplitUnpackedVarVisitor::cannotSplitReason(varp)
           || !SplitPackedVarVisitor::cannotSplitReason(varp, false);
}
