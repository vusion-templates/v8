// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_OBJECTS_JS_FUNCTION_INL_H_
#define V8_OBJECTS_JS_FUNCTION_INL_H_

#include "src/objects/js-function.h"

// Include other inline headers *after* including js-function.h, such that e.g.
// the definition of JSFunction is available (and this comment prevents
// clang-format from merging that include into the following ones).
#include "src/diagnostics/code-tracer.h"
#include "src/ic/ic.h"
#include "src/init/bootstrapper.h"
#include "src/objects/abstract-code-inl.h"
#include "src/objects/feedback-cell-inl.h"
#include "src/objects/map-updater.h"
#include "src/objects/shared-function-info-inl.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

#include "torque-generated/src/objects/js-function-tq-inl.inc"

TQ_OBJECT_CONSTRUCTORS_IMPL(JSFunctionOrBoundFunctionOrWrappedFunction)
TQ_OBJECT_CONSTRUCTORS_IMPL(JSBoundFunction)
TQ_OBJECT_CONSTRUCTORS_IMPL(JSWrappedFunction)
TQ_OBJECT_CONSTRUCTORS_IMPL(JSFunction)

ACCESSORS(JSFunction, raw_feedback_cell, FeedbackCell, kFeedbackCellOffset)
RELEASE_ACQUIRE_ACCESSORS(JSFunction, raw_feedback_cell, FeedbackCell,
                          kFeedbackCellOffset)

DEF_GETTER(JSFunction, feedback_vector, FeedbackVector) {
  DCHECK(has_feedback_vector(cage_base));
  return FeedbackVector::cast(raw_feedback_cell(cage_base).value(cage_base));
}

ClosureFeedbackCellArray JSFunction::closure_feedback_cell_array() const {
  DCHECK(has_closure_feedback_cell_array());
  return ClosureFeedbackCellArray::cast(raw_feedback_cell().value());
}

void JSFunction::reset_tiering_state() {
  DCHECK(has_feedback_vector());
  feedback_vector().reset_tiering_state();
}

bool JSFunction::ChecksTieringState() { return code().checks_tiering_state(); }

void JSFunction::CompleteInobjectSlackTrackingIfActive() {
  if (!has_prototype_slot()) return;
  if (has_initial_map() && initial_map().IsInobjectSlackTrackingInProgress()) {
    MapUpdater::CompleteInobjectSlackTracking(GetIsolate(), initial_map());
  }
}

template <typename IsolateT>
AbstractCode JSFunction::abstract_code(IsolateT* isolate) {
  if (ActiveTierIsIgnition()) {
    return AbstractCode::cast(shared().GetBytecodeArray(isolate));
  } else {
    return AbstractCode::cast(code(kAcquireLoad));
  }
}

int JSFunction::length() { return shared().length(); }

ACCESSORS_RELAXED(JSFunction, code, Code, kCodeOffset)
RELEASE_ACQUIRE_GETTER_CHECKED(JSFunction, code, Code, kCodeOffset, true)
void JSFunction::set_code(Code value, ReleaseStoreTag, WriteBarrierMode mode) {
  TaggedField<Code, kCodeOffset>::Release_Store(*this, value);
  CONDITIONAL_WRITE_BARRIER(*this, kCodeOffset, value, mode);
  if (V8_UNLIKELY(v8_flags.log_function_events && has_feedback_vector())) {
    feedback_vector().set_log_next_execution(true);
  }
}
RELEASE_ACQUIRE_ACCESSORS(JSFunction, context, Context, kContextOffset)

Address JSFunction::instruction_start() const {
  return Code::cast(code()).instruction_start();
}

// TODO(ishell): Why relaxed read but release store?
DEF_GETTER(JSFunction, shared, SharedFunctionInfo) {
  return shared(cage_base, kRelaxedLoad);
}

DEF_RELAXED_GETTER(JSFunction, shared, SharedFunctionInfo) {
  return TaggedField<SharedFunctionInfo,
                     kSharedFunctionInfoOffset>::Relaxed_Load(cage_base, *this);
}

void JSFunction::set_shared(SharedFunctionInfo value, WriteBarrierMode mode) {
  // Release semantics to support acquire read in NeedsResetDueToFlushedBytecode
  RELEASE_WRITE_FIELD(*this, kSharedFunctionInfoOffset, value);
  CONDITIONAL_WRITE_BARRIER(*this, kSharedFunctionInfoOffset, value, mode);
}

TieringState JSFunction::tiering_state() const {
  if (!has_feedback_vector()) return TieringState::kNone;
  return feedback_vector().tiering_state();
}

void JSFunction::set_tiering_state(TieringState state) {
  DCHECK(has_feedback_vector());
  DCHECK(IsNone(state) || ChecksTieringState());
  feedback_vector().set_tiering_state(state);
}

TieringState JSFunction::osr_tiering_state() {
  DCHECK(has_feedback_vector());
  return feedback_vector().osr_tiering_state();
}

void JSFunction::set_osr_tiering_state(TieringState marker) {
  DCHECK(has_feedback_vector());
  feedback_vector().set_osr_tiering_state(marker);
}

DEF_GETTER(JSFunction, has_feedback_vector, bool) {
  return shared(cage_base).is_compiled() &&
         raw_feedback_cell(cage_base).value(cage_base).IsFeedbackVector(
             cage_base);
}

bool JSFunction::has_closure_feedback_cell_array() const {
  return shared().is_compiled() &&
         raw_feedback_cell().value().IsClosureFeedbackCellArray();
}

Context JSFunction::context() {
  return TaggedField<Context, kContextOffset>::load(*this);
}

DEF_RELAXED_GETTER(JSFunction, context, Context) {
  return TaggedField<Context, kContextOffset>::Relaxed_Load(cage_base, *this);
}

bool JSFunction::has_context() const {
  return TaggedField<HeapObject, kContextOffset>::load(*this).IsContext();
}

JSGlobalProxy JSFunction::global_proxy() { return context().global_proxy(); }

NativeContext JSFunction::native_context() {
  return context().native_context();
}

RELEASE_ACQUIRE_ACCESSORS_CHECKED(JSFunction, prototype_or_initial_map,
                                  HeapObject, kPrototypeOrInitialMapOffset,
                                  map().has_prototype_slot())

DEF_GETTER(JSFunction, has_prototype_slot, bool) {
  return map(cage_base).has_prototype_slot();
}

DEF_GETTER(JSFunction, initial_map, Map) {
  return Map::cast(prototype_or_initial_map(cage_base, kAcquireLoad));
}

DEF_GETTER(JSFunction, has_initial_map, bool) {
  DCHECK(has_prototype_slot(cage_base));
  return prototype_or_initial_map(cage_base, kAcquireLoad).IsMap(cage_base);
}

DEF_GETTER(JSFunction, has_instance_prototype, bool) {
  DCHECK(has_prototype_slot(cage_base));
  return has_initial_map(cage_base) ||
         !prototype_or_initial_map(cage_base, kAcquireLoad)
              .IsTheHole(GetReadOnlyRoots(cage_base));
}

DEF_GETTER(JSFunction, has_prototype, bool) {
  DCHECK(has_prototype_slot(cage_base));
  return map(cage_base).has_non_instance_prototype() ||
         has_instance_prototype(cage_base);
}

DEF_GETTER(JSFunction, has_prototype_property, bool) {
  return (has_prototype_slot(cage_base) && IsConstructor(cage_base)) ||
         IsGeneratorFunction(shared(cage_base).kind());
}

DEF_GETTER(JSFunction, PrototypeRequiresRuntimeLookup, bool) {
  return !has_prototype_property(cage_base) ||
         map(cage_base).has_non_instance_prototype();
}

DEF_GETTER(JSFunction, instance_prototype, HeapObject) {
  DCHECK(has_instance_prototype(cage_base));
  if (has_initial_map(cage_base)) {
    return initial_map(cage_base).prototype(cage_base);
  }
  // When there is no initial map and the prototype is a JSReceiver, the
  // initial map field is used for the prototype field.
  return HeapObject::cast(prototype_or_initial_map(cage_base, kAcquireLoad));
}

DEF_GETTER(JSFunction, prototype, Object) {
  DCHECK(has_prototype(cage_base));
  // If the function's prototype property has been set to a non-JSReceiver
  // value, that value is stored in the constructor field of the map.
  Map map = this->map(cage_base);
  if (map.has_non_instance_prototype()) {
    return map.GetNonInstancePrototype(cage_base);
  }
  return instance_prototype(cage_base);
}

bool JSFunction::is_compiled() const {
  return code(kAcquireLoad).builtin_id() != Builtin::kCompileLazy &&
         shared().is_compiled();
}

bool JSFunction::NeedsResetDueToFlushedBytecode() {
  // Do a raw read for shared and code fields here since this function may be
  // called on a concurrent thread. JSFunction itself should be fully
  // initialized here but the SharedFunctionInfo, InstructionStream objects may
  // not be initialized. We read using acquire loads to defend against that.
  Object maybe_shared = ACQUIRE_READ_FIELD(*this, kSharedFunctionInfoOffset);
  if (!maybe_shared.IsSharedFunctionInfo()) return false;

  Object maybe_code = ACQUIRE_READ_FIELD(*this, kCodeOffset);
  if (!maybe_code.IsCode()) return false;
  Code code = Code::cast(maybe_code);

  SharedFunctionInfo shared = SharedFunctionInfo::cast(maybe_shared);
  return !shared.is_compiled() && code.builtin_id() != Builtin::kCompileLazy;
}

bool JSFunction::NeedsResetDueToFlushedBaselineCode() {
  return code().kind() == CodeKind::BASELINE && !shared().HasBaselineCode();
}

void JSFunction::ResetIfCodeFlushed(
    base::Optional<std::function<void(HeapObject object, ObjectSlot slot,
                                      HeapObject target)>>
        gc_notify_updated_slot) {
  const bool kBytecodeCanFlush =
      v8_flags.flush_bytecode || v8_flags.stress_snapshot;
  const bool kBaselineCodeCanFlush =
      v8_flags.flush_baseline_code || v8_flags.stress_snapshot;
  if (!kBytecodeCanFlush && !kBaselineCodeCanFlush) return;

  DCHECK_IMPLIES(NeedsResetDueToFlushedBytecode(), kBytecodeCanFlush);
  if (kBytecodeCanFlush && NeedsResetDueToFlushedBytecode()) {
    // Bytecode was flushed and function is now uncompiled, reset JSFunction
    // by setting code to CompileLazy and clearing the feedback vector.
    set_code(*BUILTIN_CODE(GetIsolate(), CompileLazy));
    raw_feedback_cell().reset_feedback_vector(gc_notify_updated_slot);
    return;
  }

  DCHECK_IMPLIES(NeedsResetDueToFlushedBaselineCode(), kBaselineCodeCanFlush);
  if (kBaselineCodeCanFlush && NeedsResetDueToFlushedBaselineCode()) {
    // Flush baseline code from the closure if required
    set_code(*BUILTIN_CODE(GetIsolate(), InterpreterEntryTrampoline));
  }
}

}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_JS_FUNCTION_INL_H_
