/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <boost/intrusive/list.hpp>

#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "RegAlloc.h"

enum TryEntryType {
  TRY_START = 0,
  TRY_END = 1,
};

std::string show(TryEntryType t);

struct TryEntry {
  TryEntryType type;
  MethodItemEntry* catch_start;
  TryEntry(TryEntryType type, MethodItemEntry* catch_start):
      type(type), catch_start(catch_start) {
    always_assert(catch_start != nullptr);
  }
};

struct CatchEntry {
  DexType* catch_type;
  MethodItemEntry* next; // always null for catchall
  CatchEntry(DexType* catch_type): catch_type(catch_type), next(nullptr) {}
};

/*
 * Multi is where an opcode encodes more than
 * one branch end-point.  This is for packed
 * and sparse switch.  The index is only relevant
 * for multi-branch encodings.  The target is
 * implicit in the flow, where the target is from
 * i.e. what has to be re-written is what is recorded
 * in DexInstruction*.
 */
enum BranchTargetType {
  BRANCH_SIMPLE = 0,
  BRANCH_MULTI = 1,
};

struct MethodItemEntry;
struct BranchTarget {
  BranchTargetType type;
  MethodItemEntry* src;
  int32_t index;
};

enum MethodItemType {
  MFLOW_TRY,
  MFLOW_CATCH,
  MFLOW_OPCODE,
  MFLOW_TARGET,
  MFLOW_DEBUG,
  MFLOW_POSITION,
  MFLOW_FALLTHROUGH,
};

/*
 * MethodItemEntry (and the FatMethods that it gets linked into) is a data
 * structure of DEX methods that is easier to modify than DexMethod.
 *
 * For example, when inserting a new instruction into a DexMethod, one needs
 * to recalculate branch offsets, try-catch regions, and debug info. None of
 * that is necessary when inserting into a FatMethod; it gets done when the
 * FatMethod gets translated back into a DexMethod by MethodTransform::sync().
 */
struct MethodItemEntry {
  boost::intrusive::list_member_hook<> list_hook_;
  MethodItemType type;
  uint16_t addr;
  union {
    TryEntry* tentry;
    CatchEntry* centry;
    DexInstruction* insn;
    BranchTarget* target;
    std::unique_ptr<DexDebugInstruction> dbgop;
    std::unique_ptr<DexPosition> pos;
  };
  explicit MethodItemEntry(const MethodItemEntry&);
  MethodItemEntry(DexInstruction* insn) {
    this->type = MFLOW_OPCODE;
    this->insn = insn;
  }
  MethodItemEntry(TryEntryType try_type, MethodItemEntry* catch_start):
    type(MFLOW_TRY), tentry(new TryEntry(try_type, catch_start)) {}
  MethodItemEntry(DexType* catch_type):
    type(MFLOW_CATCH), centry(new CatchEntry(catch_type)) {}
  MethodItemEntry(BranchTarget* bt) {
    this->type = MFLOW_TARGET;
    this->target = bt;
  }
  MethodItemEntry(std::unique_ptr<DexDebugInstruction> dbgop)
      : type(MFLOW_DEBUG), dbgop(std::move(dbgop)) {}
  MethodItemEntry(std::unique_ptr<DexPosition> pos)
      : type(MFLOW_POSITION), pos(std::move(pos)) {}
  MethodItemEntry() { this->type = MFLOW_FALLTHROUGH; }
  ~MethodItemEntry();
};

using MethodItemMemberListOption =
    boost::intrusive::member_hook<MethodItemEntry,
                                  boost::intrusive::list_member_hook<>,
                                  &MethodItemEntry::list_hook_>;

using FatMethod =
    boost::intrusive::list<MethodItemEntry, MethodItemMemberListOption>;

struct FatMethodDisposer {
  void operator()(MethodItemEntry* mei) {
    delete mei;
  }
};

std::string show(const FatMethod*);

struct Block {
  Block(size_t id) : m_id(id) {}

  size_t id() const { return m_id; }
  std::vector<Block*>& preds() { return m_preds; }
  std::vector<Block*>& succs() { return m_succs; }
  FatMethod::iterator begin() { return m_begin; }
  FatMethod::iterator end() { return m_end; }
  FatMethod::reverse_iterator rbegin() {
    return FatMethod::reverse_iterator(m_end);
  }
  FatMethod::reverse_iterator rend() {
    return FatMethod::reverse_iterator(m_begin);
  }

 private:
  friend class MethodTransform;
  friend std::string show(const std::vector<Block*>& blocks);

  size_t m_id;
  FatMethod::iterator m_begin;
  FatMethod::iterator m_end;
  std::vector<Block*> m_preds;
  std::vector<Block*> m_succs;
};

inline bool is_catch(Block* b) {
  auto it = b->begin();
  return it->type == MFLOW_CATCH;
}

bool ends_with_may_throw(Block* b);

/*
 * Build a postorder sorted vector of blocks from the given CFG.  Uses a
 * standard depth-first search with a side table of already-visited nodes.
 */
class PostOrderSort {
 private:
  const std::vector<Block*>& m_cfg;
  std::unordered_set<Block*> m_visited;
  std::vector<Block*> m_postorder_list;

  void postorder(Block* b);

 public:
  PostOrderSort(const std::vector<Block*>& cfg) : m_cfg(cfg) {}

  std::vector<Block*>&& get();
};

class InlineContext;

class MethodTransform {
 private:
  using FatMethodCache = std::unordered_map<DexMethod*, MethodTransform*>;

  MethodTransform(DexMethod* method, FatMethod* fm)
    : m_method(method),
      m_fmethod(fm)
  {}

  ~MethodTransform();

  /* Create a FatMethod from a DexMethod. */
  static FatMethod* balloon(DexMethod* method);

  /* try_sync() is the work-horse of sync.  It's intended such that it can fail
   * in the event that an opcode needs to be resized.  In that instance, it
   * changes the opcode in question, and returns false.  It's intended to be
   * called multiple times until it returns true.
   */
  bool try_sync();

  void build_cfg();

  static FatMethodCache s_cache;
  static std::mutex s_lock;

  DexMethod* m_method;
  FatMethod* m_fmethod;
  std::vector<Block*> m_blocks;

 private:
  FatMethod::iterator main_block();
  FatMethod::iterator insert(FatMethod::iterator cur, DexInstruction* insn);
  FatMethod::iterator make_if_block(FatMethod::iterator cur,
                                    DexInstruction* insn,
                                    FatMethod::iterator* if_block);
  FatMethod::iterator make_if_else_block(FatMethod::iterator cur,
                                         DexInstruction* insn,
                                         FatMethod::iterator* if_block,
                                         FatMethod::iterator* else_block);
  FatMethod::iterator make_switch_block(
      FatMethod::iterator cur,
      DexInstruction* insn,
      FatMethod::iterator* default_block,
      std::map<int, FatMethod::iterator>& cases);

  friend struct MethodCreator;

 public:
  /*
   * Static factory method that checks the cache first.  Optionally builds a
   * control-flow graph, which makes the transform slightly more expensive.
   */
  static MethodTransform* get_method_transform(DexMethod* method,
                                               bool want_cfg = false);

  static MethodTransform* get_new_method(DexMethod* method);

  /*
   * Call before writing any dexes out, or doing analysis on DexMethod
   * structures.
   */
  static void sync_all();

  /*
   * Inline tail-called `callee` into `caller` at instruction `invoke`.
   *
   * NB: This is NOT a general-purpose inliner; it assumes that the caller does
   * not do any work after the call, so the only live registers are the
   * parameters to the callee.
   */
  static void inline_tail_call(DexMethod* caller,
                               DexMethod* callee,
                               DexInstruction* invoke);

  static bool inline_16regs(
      InlineContext& context,
      DexMethod *callee,
      DexOpcodeMethod *invoke);

  /* Return the control flow graph of this method as a vector of blocks. */
  std::vector<Block*>& cfg() { return m_blocks; }

  /* Write-back FatMethod to DexMethod */
  void sync();

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(DexInstruction* from, DexInstruction* to);

  /* push_back will take ownership of insn */
  void push_back(DexInstruction* insn);

  /* position = nullptr means at the head */
  void insert_after(DexInstruction* position, std::list<DexInstruction*>& opcodes);

  /* Memory ownership of "insn" passes to callee, it will delete it. */
  void remove_opcode(DexInstruction* insn);

  FatMethod::iterator begin() { return m_fmethod->begin(); }
  FatMethod::iterator end() { return m_fmethod->end(); }
  FatMethod::iterator erase(FatMethod::iterator it) {
    return m_fmethod->erase(it);
  }
  friend std::string show(const MethodTransform*);
};

/*
 * Scoped holder for MethodTransform to ensure sync_all is called.
 */
class MethodTransformer {
  MethodTransform* m_transform;

 public:
  MethodTransformer(DexMethod* m, bool want_cfg = false) {
    m_transform = MethodTransform::get_method_transform(m, want_cfg);
  }

  ~MethodTransformer() { m_transform->sync(); }

  MethodTransform* operator->() { return m_transform; }
};

/**
 * Carry context for multiple inline into a single caller.
 * In particular, it caches the liveness analysis so that we can reuse it when
 * multiple callees into the same caller.
 */
class InlineContext {
  std::unique_ptr<LivenessMap> m_liveness;
 public:
  MethodTransformer mtcaller;
  uint16_t original_regs;
  InlineContext(DexMethod* caller, bool use_liveness);
  Liveness live_out(DexInstruction*);
};
