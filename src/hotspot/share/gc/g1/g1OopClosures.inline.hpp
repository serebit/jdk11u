/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_G1_G1OOPCLOSURES_INLINE_HPP
#define SHARE_VM_GC_G1_G1OOPCLOSURES_INLINE_HPP

#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1OopClosures.hpp"
#include "gc/g1/g1ParScanThreadState.inline.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"

template <class T>
inline void G1ScanClosureBase::prefetch_and_push(T* p, const oop obj) {
  // We're not going to even bother checking whether the object is
  // already forwarded or not, as this usually causes an immediate
  // stall. We'll try to prefetch the object (for write, given that
  // we might need to install the forwarding reference) and we'll
  // get back to it when pop it from the queue
  Prefetch::write(obj->mark_addr_raw(), 0);
  Prefetch::read(obj->mark_addr_raw(), (HeapWordSize*2));

  // slightly paranoid test; I'm trying to catch potential
  // problems before we go into push_on_queue to know where the
  // problem is coming from
  assert((obj == RawAccess<>::oop_load(p)) ||
         (obj->is_forwarded() &&
         obj->forwardee() == RawAccess<>::oop_load(p)),
         "p should still be pointing to obj or to its forwardee");

  _par_scan_state->push_on_queue(p);
}

template <class T>
inline void G1ScanClosureBase::handle_non_cset_obj_common(InCSetState const state, T* p, oop const obj) {
  if (state.is_humongous()) {
    _g1h->set_humongous_is_live(obj);
  }
}

inline void G1ScanClosureBase::trim_queue_partially() {
  _par_scan_state->trim_queue_partially();
}

template <class T>
inline void G1ScanEvacuatedObjClosure::do_oop_work(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);

  if (CompressedOops::is_null(heap_oop)) {
    return;
  }
  oop obj = CompressedOops::decode_not_null(heap_oop);
  const InCSetState state = _g1h->in_cset_state(obj);
  if (state.is_in_cset()) {
    prefetch_and_push(p, obj);
  } else {
    if (HeapRegion::is_in_same_region(p, obj)) {
      return;
    }
    handle_non_cset_obj_common(state, p, obj);
    _par_scan_state->update_rs(_from, p, obj);
  }
}

template <class T>
inline void G1CMOopClosure::do_oop_work(T* p) {
  _task->deal_with_reference(p);
}

template <class T>
inline void G1RootRegionScanClosure::do_oop_work(T* p) {
  T heap_oop = RawAccess<MO_VOLATILE>::oop_load(p);
  if (CompressedOops::is_null(heap_oop)) {
    return;
  }
  oop obj = CompressedOops::decode_not_null(heap_oop);
  _cm->mark_in_next_bitmap(_worker_id, obj);
}

template <class T>
inline static void check_obj_during_refinement(T* p, oop const obj) {
#ifdef ASSERT
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  // can't do because of races
  // assert(oopDesc::is_oop_or_null(obj), "expected an oop");
  assert(check_obj_alignment(obj), "not oop aligned");
  assert(g1h->is_in_reserved(obj), "must be in heap");

  HeapRegion* from = g1h->heap_region_containing(p);

  assert(from != NULL, "from region must be non-NULL");
  assert(from->is_in_reserved(p) ||
         (from->is_humongous() &&
          g1h->heap_region_containing(p)->is_humongous() &&
          from->humongous_start_region() == g1h->heap_region_containing(p)->humongous_start_region()),
         "p " PTR_FORMAT " is not in the same region %u or part of the correct humongous object starting at region %u.",
         p2i(p), from->hrm_index(), from->humongous_start_region()->hrm_index());
#endif // ASSERT
}

template <class T>
inline void G1ConcurrentRefineOopClosure::do_oop_work(T* p) {
  T o = RawAccess<MO_VOLATILE>::oop_load(p);
  if (CompressedOops::is_null(o)) {
    return;
  }
  oop obj = CompressedOops::decode_not_null(o);

  check_obj_during_refinement(p, obj);

  if (HeapRegion::is_in_same_region(p, obj)) {
    // Normally this closure should only be called with cross-region references.
    // But since Java threads are manipulating the references concurrently and we
    // reload the values things may have changed.
    // Also this check lets slip through references from a humongous continues region
    // to its humongous start region, as they are in different regions, and adds a
    // remembered set entry. This is benign (apart from memory usage), as we never
    // try to either evacuate or eager reclaim humonguous arrays of j.l.O.
    return;
  }

  HeapRegionRemSet* to_rem_set = _g1h->heap_region_containing(obj)->rem_set();

  assert(to_rem_set != NULL, "Need per-region 'into' remsets.");
  if (to_rem_set->is_tracked()) {
    to_rem_set->add_reference(p, _worker_i);
  }
}

template <class T>
inline void G1ScanObjsDuringUpdateRSClosure::do_oop_work(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (CompressedOops::is_null(o)) {
    return;
  }
  oop obj = CompressedOops::decode_not_null(o);

  check_obj_during_refinement(p, obj);

  assert(!_g1h->is_in_cset((HeapWord*)p), "Oop originates from " PTR_FORMAT " (region: %u) which is in the collection set.", p2i(p), _g1h->addr_to_region((HeapWord*)p));
  const InCSetState state = _g1h->in_cset_state(obj);
  if (state.is_in_cset()) {
    // Since the source is always from outside the collection set, here we implicitly know
    // that this is a cross-region reference too.
    prefetch_and_push(p, obj);
  } else {
    HeapRegion* to = _g1h->heap_region_containing(obj);
    if (_from == to) {
      return;
    }
    handle_non_cset_obj_common(state, p, obj);
    to->rem_set()->add_reference(p, _worker_i);
  }
}

template <class T>
inline void G1ScanObjsDuringScanRSClosure::do_oop_work(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);
  if (CompressedOops::is_null(heap_oop)) {
    return;
  }
  oop obj = CompressedOops::decode_not_null(heap_oop);

  const InCSetState state = _g1h->in_cset_state(obj);
  if (state.is_in_cset()) {
    prefetch_and_push(p, obj);
  } else {
    if (HeapRegion::is_in_same_region(p, obj)) {
      return;
    }
    handle_non_cset_obj_common(state, p, obj);
  }
}

void G1ParCopyHelper::do_cld_barrier(oop new_obj) {
  if (_g1h->heap_region_containing(new_obj)->is_young()) {
    _scanned_cld->record_modified_oops();
  }
}

void G1ParCopyHelper::mark_object(oop obj) {
  assert(!_g1h->heap_region_containing(obj)->in_collection_set(), "should not mark objects in the CSet");

  // We know that the object is not moving so it's safe to read its size.
  _cm->mark_in_next_bitmap(_worker_id, obj);
}

void G1ParCopyHelper::mark_forwarded_object(oop from_obj, oop to_obj) {
  assert(from_obj->is_forwarded(), "from obj should be forwarded");
  assert(from_obj->forwardee() == to_obj, "to obj should be the forwardee");
  assert(from_obj != to_obj, "should not be self-forwarded");

  assert(_g1h->heap_region_containing(from_obj)->in_collection_set(), "from obj should be in the CSet");
  assert(!_g1h->heap_region_containing(to_obj)->in_collection_set(), "should not mark objects in the CSet");

  // The object might be in the process of being copied by another
  // worker so we cannot trust that its to-space image is
  // well-formed. So we have to read its size from its from-space
  // image which we know should not be changing.
  _cm->mark_in_next_bitmap(_worker_id, to_obj, from_obj->size());
}

void G1ParCopyHelper::trim_queue_partially() {
  _par_scan_state->trim_queue_partially();
}

template <G1Barrier barrier, G1Mark do_mark_object>
template <class T>
void G1ParCopyClosure<barrier, do_mark_object>::do_oop_work(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);

  if (CompressedOops::is_null(heap_oop)) {
    return;
  }

  oop obj = CompressedOops::decode_not_null(heap_oop);

  assert(_worker_id == _par_scan_state->worker_id(), "sanity");

  const InCSetState state = _g1h->in_cset_state(obj);
  if (state.is_in_cset()) {
    oop forwardee;
    markOop m = obj->mark_raw();
    if (m.is_marked()) {
      forwardee = (oop) m.decode_pointer();
    } else {
      forwardee = _par_scan_state->copy_to_survivor_space(state, obj, m);
    }
    assert(forwardee != NULL, "forwardee should not be NULL");
    RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);
    if (do_mark_object != G1MarkNone && forwardee != obj) {
      // If the object is self-forwarded we don't need to explicitly
      // mark it, the evacuation failure protocol will do so.
      mark_forwarded_object(obj, forwardee);
    }

    if (barrier == G1BarrierCLD) {
      do_cld_barrier(forwardee);
    }
  } else {
    if (state.is_humongous()) {
      _g1h->set_humongous_is_live(obj);
    }

    // The object is not in collection set. If we're a root scanning
    // closure during an initial mark pause then attempt to mark the object.
    if (do_mark_object == G1MarkFromRoot) {
      mark_object(obj);
    }
  }
  trim_queue_partially();
}

template <class T> void G1RebuildRemSetClosure::do_oop_work(T* p) {
  oop const obj = RawAccess<MO_VOLATILE>::oop_load(p);
  if (obj == NULL) {
    return;
  }

  if (HeapRegion::is_in_same_region(p, obj)) {
    return;
  }

  HeapRegion* to = _g1h->heap_region_containing(obj);
  HeapRegionRemSet* rem_set = to->rem_set();
  rem_set->add_reference(p, _worker_id);
}

#endif // SHARE_VM_GC_G1_G1OOPCLOSURES_INLINE_HPP
