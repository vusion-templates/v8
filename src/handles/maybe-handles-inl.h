// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HANDLES_MAYBE_HANDLES_INL_H_
#define V8_HANDLES_MAYBE_HANDLES_INL_H_

#include "src/handles/maybe-handles.h"

#include "src/handles/handles-inl.h"
#include "src/objects/maybe-object-inl.h"

namespace v8 {
namespace internal {

template <typename T>
MaybeHandle<T>::MaybeHandle(T object, Isolate* isolate)
    : MaybeHandle(handle(object, isolate)) {}

template <typename T>
MaybeHandle<T>::MaybeHandle(T object, LocalHeap* local_heap)
    : MaybeHandle(handle(object, local_heap)) {}

MaybeObjectHandle::MaybeObjectHandle(MaybeObject object, Isolate* isolate) {
  HeapObject heap_object;
  DCHECK(!object->IsCleared());
  if (object->GetHeapObjectIfWeak(&heap_object)) {
    handle_ = handle(heap_object, isolate);
    reference_type_ = HeapObjectReferenceType::WEAK;
  } else {
    handle_ = handle(object->cast<Object>(), isolate);
    reference_type_ = HeapObjectReferenceType::STRONG;
  }
}

MaybeObjectHandle::MaybeObjectHandle(MaybeObject object,
                                     LocalHeap* local_heap) {
  HeapObject heap_object;
  DCHECK(!object->IsCleared());
  if (object->GetHeapObjectIfWeak(&heap_object)) {
    handle_ = handle(heap_object, local_heap);
    reference_type_ = HeapObjectReferenceType::WEAK;
  } else {
    handle_ = handle(object->cast<Object>(), local_heap);
    reference_type_ = HeapObjectReferenceType::STRONG;
  }
}

MaybeObjectHandle::MaybeObjectHandle(Handle<Object> object)
    : reference_type_(HeapObjectReferenceType::STRONG), handle_(object) {}

MaybeObjectHandle::MaybeObjectHandle(Object object, Isolate* isolate)
    : reference_type_(HeapObjectReferenceType::STRONG),
      handle_(object, isolate) {}

MaybeObjectHandle::MaybeObjectHandle(Object object, LocalHeap* local_heap)
    : reference_type_(HeapObjectReferenceType::STRONG),
      handle_(object, local_heap) {}

MaybeObjectHandle::MaybeObjectHandle(Object object,
                                     HeapObjectReferenceType reference_type,
                                     Isolate* isolate)
    : reference_type_(reference_type), handle_(handle(object, isolate)) {}

MaybeObjectHandle::MaybeObjectHandle(Handle<Object> object,
                                     HeapObjectReferenceType reference_type)
    : reference_type_(reference_type), handle_(object) {}

MaybeObjectHandle MaybeObjectHandle::Weak(Handle<Object> object) {
  return MaybeObjectHandle(object, HeapObjectReferenceType::WEAK);
}

MaybeObjectHandle MaybeObjectHandle::Weak(Object object, Isolate* isolate) {
  return MaybeObjectHandle(object, HeapObjectReferenceType::WEAK, isolate);
}

bool MaybeObjectHandle::is_identical_to(const MaybeObjectHandle& other) const {
  Handle<Object> this_handle;
  Handle<Object> other_handle;
  return reference_type_ == other.reference_type_ &&
         handle_.ToHandle(&this_handle) ==
             other.handle_.ToHandle(&other_handle) &&
         this_handle.is_identical_to(other_handle);
}

MaybeObject MaybeObjectHandle::operator*() const {
  if (reference_type_ == HeapObjectReferenceType::WEAK) {
    return HeapObjectReference::Weak(*handle_.ToHandleChecked());
  } else {
    return MaybeObject::FromObject(*handle_.ToHandleChecked());
  }
}

MaybeObject MaybeObjectHandle::operator->() const {
  if (reference_type_ == HeapObjectReferenceType::WEAK) {
    return HeapObjectReference::Weak(*handle_.ToHandleChecked());
  } else {
    return MaybeObject::FromObject(*handle_.ToHandleChecked());
  }
}

Handle<Object> MaybeObjectHandle::object() const {
  return handle_.ToHandleChecked();
}

inline MaybeObjectHandle handle(MaybeObject object, Isolate* isolate) {
  return MaybeObjectHandle(object, isolate);
}

inline MaybeObjectHandle handle(MaybeObject object, LocalHeap* local_heap) {
  return MaybeObjectHandle(object, local_heap);
}

template <typename T>
inline std::ostream& operator<<(std::ostream& os, MaybeHandle<T> handle) {
  if (handle.is_null()) return os << "null";
  return os << handle.ToHandleChecked();
}

#ifdef V8_ENABLE_CONSERVATIVE_STACK_SCANNING

template <typename T>
MaybeDirectHandle<T>::MaybeDirectHandle(T object, Isolate* isolate)
    : MaybeDirectHandle(DirectHandle<T>(object.ptr())) {}

template <typename T>
inline std::ostream& operator<<(std::ostream& os, MaybeDirectHandle<T> handle) {
  if (handle.is_null()) return os << "null";
  return os << handle.ToHandleChecked();
}

MaybeObjectDirectHandle::MaybeObjectDirectHandle(MaybeObject object,
                                                 Isolate* isolate) {
  HeapObject heap_object;
  DCHECK(!object->IsCleared());
  if (object->GetHeapObjectIfWeak(&heap_object)) {
    handle_ = DirectHandle<HeapObject>(heap_object.ptr());
    reference_type_ = HeapObjectReferenceType::WEAK;
  } else {
    handle_ = DirectHandle<Object>(object->cast<Object>().ptr());
    reference_type_ = HeapObjectReferenceType::STRONG;
  }
}

MaybeObjectDirectHandle::MaybeObjectDirectHandle(DirectHandle<Object> object)
    : reference_type_(HeapObjectReferenceType::STRONG), handle_(object) {}

MaybeObjectDirectHandle::MaybeObjectDirectHandle(Object object,
                                                 Isolate* isolate)
    : reference_type_(HeapObjectReferenceType::STRONG),
      handle_(object, isolate) {}

MaybeObjectDirectHandle::MaybeObjectDirectHandle(
    Object object, HeapObjectReferenceType reference_type, Isolate* isolate)
    : reference_type_(reference_type), handle_(object, isolate) {}

MaybeObjectDirectHandle::MaybeObjectDirectHandle(
    DirectHandle<Object> object, HeapObjectReferenceType reference_type)
    : reference_type_(reference_type), handle_(object) {}

MaybeObjectDirectHandle MaybeObjectDirectHandle::Weak(
    DirectHandle<Object> object) {
  return MaybeObjectDirectHandle(object, HeapObjectReferenceType::WEAK);
}

MaybeObjectDirectHandle MaybeObjectDirectHandle::Weak(Object object,
                                                      Isolate* isolate) {
  return MaybeObjectDirectHandle(object, HeapObjectReferenceType::WEAK,
                                 isolate);
}

MaybeObject MaybeObjectDirectHandle::operator*() const {
  if (reference_type_ == HeapObjectReferenceType::WEAK) {
    return HeapObjectReference::Weak(*handle_.ToHandleChecked());
  } else {
    return MaybeObject::FromObject(*handle_.ToHandleChecked());
  }
}

MaybeObject MaybeObjectDirectHandle::operator->() const {
  if (reference_type_ == HeapObjectReferenceType::WEAK) {
    return HeapObjectReference::Weak(*handle_.ToHandleChecked());
  } else {
    return MaybeObject::FromObject(*handle_.ToHandleChecked());
  }
}

DirectHandle<Object> MaybeObjectDirectHandle::object() const {
  return handle_.ToHandleChecked();
}

#endif  // V8_ENABLE_CONSERVATIVE_STACK_SCANNING

}  // namespace internal
}  // namespace v8

#endif  // V8_HANDLES_MAYBE_HANDLES_INL_H_
