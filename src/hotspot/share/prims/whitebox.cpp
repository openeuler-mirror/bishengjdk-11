/*
 * Copyright (c) 2012, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"

#include <new>

#include "classfile/classLoaderData.hpp"
#include "classfile/modules.hpp"
#include "classfile/stringTable.hpp"
#include "code/codeCache.hpp"
#include "compiler/methodMatcher.hpp"
#include "compiler/directivesParser.hpp"
#include "gc/shared/gcConfig.hpp"
#include "gc/shared/genCollectedHeap.hpp"
#include "jvmtifiles/jvmtiEnv.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "memory/oopFactory.hpp"
#include "oops/array.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "prims/wbtestmethods/parserTests.hpp"
#include "prims/whitebox.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/compilationPolicy.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handshake.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/sweeper.hpp"
#include "runtime/thread.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vm_version.hpp"
#include "services/memoryService.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/elfFile.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_CDS
#include "prims/cdsoffsets.hpp"
#endif // INCLUDE_CDS
#if INCLUDE_G1GC
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMark.hpp"
#include "gc/g1/g1ConcurrentMarkThread.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#endif // INCLUDE_G1GC
#if INCLUDE_PARALLELGC
#include "gc/parallel/parallelScavengeHeap.inline.hpp"
#include "gc/parallel/adjoiningGenerations.hpp"
#endif // INCLUDE_PARALLELGC
#if INCLUDE_NMT
#include "services/mallocSiteTable.hpp"
#include "services/memTracker.hpp"
#include "utilities/nativeCallStack.hpp"
#endif // INCLUDE_NMT
#if INCLUDE_AOT
#include "aot/aotLoader.hpp"
#endif // INCLUDE_AOT

#ifdef LINUX
#include "os_linux.hpp"
#include "osContainer_linux.hpp"
#include "cgroupSubsystem_linux.hpp"
#endif

#define SIZE_T_MAX_VALUE ((size_t) -1)

#define CHECK_JNI_EXCEPTION_(env, value)                               \
  do {                                                                 \
    JavaThread* THREAD = JavaThread::thread_from_jni_environment(env); \
    THREAD->clear_pending_jni_exception_check();                       \
    if (HAS_PENDING_EXCEPTION) {                                       \
      return(value);                                                   \
    }                                                                  \
  } while (0)

#define CHECK_JNI_EXCEPTION(env)                                       \
  do {                                                                 \
    JavaThread* THREAD = JavaThread::thread_from_jni_environment(env); \
    THREAD->clear_pending_jni_exception_check();                       \
    if (HAS_PENDING_EXCEPTION) {                                       \
      return;                                                          \
    }                                                                  \
  } while (0)

bool WhiteBox::_used = false;
volatile bool WhiteBox::compilation_locked = false;

class VM_WhiteBoxOperation : public VM_Operation {
 public:
  VM_WhiteBoxOperation()                         { }
  VMOp_Type type()                  const        { return VMOp_WhiteBoxOperation; }
  bool allow_nested_vm_operations() const        { return true; }
};


WB_ENTRY(jlong, WB_GetObjectAddress(JNIEnv* env, jobject o, jobject obj))
  return (jlong)(void*)JNIHandles::resolve(obj);
WB_END

WB_ENTRY(jint, WB_GetHeapOopSize(JNIEnv* env, jobject o))
  return heapOopSize;
WB_END

WB_ENTRY(jint, WB_GetVMPageSize(JNIEnv* env, jobject o))
  return os::vm_page_size();
WB_END

WB_ENTRY(jlong, WB_GetVMAllocationGranularity(JNIEnv* env, jobject o))
  return os::vm_allocation_granularity();
WB_END

WB_ENTRY(jlong, WB_GetVMLargePageSize(JNIEnv* env, jobject o))
  return os::large_page_size();
WB_END

class WBIsKlassAliveClosure : public KlassClosure {
    Symbol* _name;
    bool _found;
public:
    WBIsKlassAliveClosure(Symbol* name) : _name(name), _found(false) {}

    void do_klass(Klass* k) {
      if (_found) return;
      Symbol* ksym = k->name();
      if (ksym->fast_compare(_name) == 0) {
        _found = true;
      }
    }

    bool found() const {
        return _found;
    }
};

WB_ENTRY(jboolean, WB_IsClassAlive(JNIEnv* env, jobject target, jstring name))
  oop h_name = JNIHandles::resolve(name);
  if (h_name == NULL) return false;
  Symbol* sym = java_lang_String::as_symbol(h_name, CHECK_false);
  TempNewSymbol tsym(sym); // Make sure to decrement reference count on sym on return

  WBIsKlassAliveClosure closure(sym);
  ClassLoaderDataGraph::classes_do(&closure);

  return closure.found();
WB_END

WB_ENTRY(jint, WB_GetSymbolRefcount(JNIEnv* env, jobject unused, jstring name))
  oop h_name = JNIHandles::resolve(name);
  if (h_name == NULL) return false;
  Symbol* sym = java_lang_String::as_symbol(h_name, CHECK_0);
  TempNewSymbol tsym(sym); // Make sure to decrement reference count on sym on return
  return (jint)sym->refcount();
WB_END


WB_ENTRY(void, WB_AddToBootstrapClassLoaderSearch(JNIEnv* env, jobject o, jstring segment)) {
#if INCLUDE_JVMTI
  ResourceMark rm;
  const char* seg = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(segment));
  JvmtiEnv* jvmti_env = JvmtiEnv::create_a_jvmti(JVMTI_VERSION);
  jvmtiError err = jvmti_env->AddToBootstrapClassLoaderSearch(seg);
  assert(err == JVMTI_ERROR_NONE, "must not fail");
#endif
}
WB_END

WB_ENTRY(void, WB_AddToSystemClassLoaderSearch(JNIEnv* env, jobject o, jstring segment)) {
#if INCLUDE_JVMTI
  ResourceMark rm;
  const char* seg = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(segment));
  JvmtiEnv* jvmti_env = JvmtiEnv::create_a_jvmti(JVMTI_VERSION);
  jvmtiError err = jvmti_env->AddToSystemClassLoaderSearch(seg);
  assert(err == JVMTI_ERROR_NONE, "must not fail");
#endif
}
WB_END


WB_ENTRY(jlong, WB_GetCompressedOopsMaxHeapSize(JNIEnv* env, jobject o)) {
  return (jlong)Arguments::max_heap_for_compressed_oops();
}
WB_END

WB_ENTRY(void, WB_PrintHeapSizes(JNIEnv* env, jobject o)) {
  CollectorPolicy * p = Universe::heap()->collector_policy();
  tty->print_cr("Minimum heap " SIZE_FORMAT " Initial heap "
    SIZE_FORMAT " Maximum heap " SIZE_FORMAT " Space alignment " SIZE_FORMAT " Heap alignment " SIZE_FORMAT,
    p->min_heap_byte_size(), p->initial_heap_byte_size(), p->max_heap_byte_size(),
    p->space_alignment(), p->heap_alignment());
}
WB_END

#ifndef PRODUCT
// Forward declaration
void TestReservedSpace_test();
void TestReserveMemorySpecial_test();
void TestVirtualSpace_test();
#endif

WB_ENTRY(void, WB_RunMemoryUnitTests(JNIEnv* env, jobject o))
#ifndef PRODUCT
  TestReservedSpace_test();
  TestReserveMemorySpecial_test();
  TestVirtualSpace_test();
#endif
WB_END

WB_ENTRY(void, WB_ReadFromNoaccessArea(JNIEnv* env, jobject o))
  size_t granularity = os::vm_allocation_granularity();
  ReservedHeapSpace rhs(100 * granularity, granularity, false);
  VirtualSpace vs;
  vs.initialize(rhs, 50 * granularity);

  // Check if constraints are complied
  if (!( UseCompressedOops && rhs.base() != NULL &&
         Universe::narrow_oop_base() != NULL &&
         Universe::narrow_oop_use_implicit_null_checks() )) {
    tty->print_cr("WB_ReadFromNoaccessArea method is useless:\n "
                  "\tUseCompressedOops is %d\n"
                  "\trhs.base() is " PTR_FORMAT "\n"
                  "\tUniverse::narrow_oop_base() is " PTR_FORMAT "\n"
                  "\tUniverse::narrow_oop_use_implicit_null_checks() is %d",
                  UseCompressedOops,
                  p2i(rhs.base()),
                  p2i(Universe::narrow_oop_base()),
                  Universe::narrow_oop_use_implicit_null_checks());
    return;
  }
  tty->print_cr("Reading from no access area... ");
  tty->print_cr("*(vs.low_boundary() - rhs.noaccess_prefix() / 2 ) = %c",
                *(vs.low_boundary() - rhs.noaccess_prefix() / 2 ));
WB_END

static jint wb_stress_virtual_space_resize(size_t reserved_space_size,
                                           size_t magnitude, size_t iterations) {
  size_t granularity = os::vm_allocation_granularity();
  ReservedHeapSpace rhs(reserved_space_size * granularity, granularity, false);
  VirtualSpace vs;
  if (!vs.initialize(rhs, 0)) {
    tty->print_cr("Failed to initialize VirtualSpace. Can't proceed.");
    return 3;
  }

  int seed = os::random();
  tty->print_cr("Random seed is %d", seed);
  os::init_random(seed);

  for (size_t i = 0; i < iterations; i++) {

    // Whether we will shrink or grow
    bool shrink = os::random() % 2L == 0;

    // Get random delta to resize virtual space
    size_t delta = (size_t)os::random() % magnitude;

    // If we are about to shrink virtual space below zero, then expand instead
    if (shrink && vs.committed_size() < delta) {
      shrink = false;
    }

    // Resizing by delta
    if (shrink) {
      vs.shrink_by(delta);
    } else {
      // If expanding fails expand_by will silently return false
      vs.expand_by(delta, true);
    }
  }
  return 0;
}

WB_ENTRY(jint, WB_StressVirtualSpaceResize(JNIEnv* env, jobject o,
        jlong reserved_space_size, jlong magnitude, jlong iterations))
  tty->print_cr("reservedSpaceSize=" JLONG_FORMAT ", magnitude=" JLONG_FORMAT ", "
                "iterations=" JLONG_FORMAT "\n", reserved_space_size, magnitude,
                iterations);
  if (reserved_space_size < 0 || magnitude < 0 || iterations < 0) {
    tty->print_cr("One of variables printed above is negative. Can't proceed.\n");
    return 1;
  }

  // sizeof(size_t) depends on whether OS is 32bit or 64bit. sizeof(jlong) is
  // always 8 byte. That's why we should avoid overflow in case of 32bit platform.
  if (sizeof(size_t) < sizeof(jlong)) {
    jlong size_t_max_value = (jlong) SIZE_T_MAX_VALUE;
    if (reserved_space_size > size_t_max_value || magnitude > size_t_max_value
        || iterations > size_t_max_value) {
      tty->print_cr("One of variables printed above overflows size_t. Can't proceed.\n");
      return 2;
    }
  }

  return wb_stress_virtual_space_resize((size_t) reserved_space_size,
                                        (size_t) magnitude, (size_t) iterations);
WB_END

WB_ENTRY(jboolean, WB_IsGCSupported(JNIEnv* env, jobject o, jint name))
  return GCConfig::is_gc_supported((CollectedHeap::Name)name);
WB_END

WB_ENTRY(jboolean, WB_IsGCSelected(JNIEnv* env, jobject o, jint name))
  return GCConfig::is_gc_selected((CollectedHeap::Name)name);
WB_END

WB_ENTRY(jboolean, WB_IsGCSelectedErgonomically(JNIEnv* env, jobject o))
  return GCConfig::is_gc_selected_ergonomically();
WB_END

WB_ENTRY(jboolean, WB_isObjectInOldGen(JNIEnv* env, jobject o, jobject obj))
  oop p = JNIHandles::resolve(obj);
#if INCLUDE_G1GC
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    const HeapRegion* hr = g1h->heap_region_containing(p);
    if (hr == NULL) {
      return false;
    }
    return !(hr->is_young());
  }
#endif
#if INCLUDE_PARALLELGC
  if (UseParallelGC) {
    ParallelScavengeHeap* psh = ParallelScavengeHeap::heap();
    return !psh->is_in_young(p);
  }
#endif
#if INCLUDE_ZGC
  if (UseZGC) {
    return Universe::heap()->is_in(p);
  }
#endif
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  return !gch->is_in_young(p);
WB_END

WB_ENTRY(jlong, WB_GetObjectSize(JNIEnv* env, jobject o, jobject obj))
  oop p = JNIHandles::resolve(obj);
  return p->size() * HeapWordSize;
WB_END

WB_ENTRY(jlong, WB_GetHeapSpaceAlignment(JNIEnv* env, jobject o))
  size_t alignment = Universe::heap()->collector_policy()->space_alignment();
  return (jlong)alignment;
WB_END

WB_ENTRY(jlong, WB_GetHeapAlignment(JNIEnv* env, jobject o))
  size_t alignment = Universe::heap()->collector_policy()->heap_alignment();
  return (jlong)alignment;
WB_END

WB_ENTRY(jboolean, WB_SupportsConcurrentGCPhaseControl(JNIEnv* env, jobject o))
  return Universe::heap()->supports_concurrent_phase_control();
WB_END

WB_ENTRY(jobjectArray, WB_GetConcurrentGCPhases(JNIEnv* env, jobject o))
  const char* const* phases = Universe::heap()->concurrent_phases();
  jint nphases = 0;
  for ( ; phases[nphases] != NULL; ++nphases) ;

  ResourceMark rm(thread);
  ThreadToNativeFromVM ttn(thread);
  jclass clazz = env->FindClass(vmSymbols::java_lang_Object()->as_C_string());
  CHECK_JNI_EXCEPTION_(env, NULL);

  jobjectArray result = env->NewObjectArray(nphases, clazz, NULL);
  CHECK_JNI_EXCEPTION_(env, NULL);

  // If push fails, return with pending exception.
  if (env->PushLocalFrame(nphases) < 0) return NULL;
  for (jint i = 0; i < nphases; ++i) {
    jstring phase = env->NewStringUTF(phases[i]);
    CHECK_JNI_EXCEPTION_(env, NULL);
    env->SetObjectArrayElement(result, i, phase);
    CHECK_JNI_EXCEPTION_(env, NULL);
  }
  env->PopLocalFrame(NULL);

  return result;
WB_END

WB_ENTRY(jboolean, WB_RequestConcurrentGCPhase(JNIEnv* env, jobject o, jstring name))
  Handle h_name(THREAD, JNIHandles::resolve(name));
  ResourceMark rm;
  const char* c_name = java_lang_String::as_utf8_string(h_name());
  return Universe::heap()->request_concurrent_phase(c_name);
WB_END

#if INCLUDE_G1GC

WB_ENTRY(jboolean, WB_G1IsHumongous(JNIEnv* env, jobject o, jobject obj))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    oop result = JNIHandles::resolve(obj);
    const HeapRegion* hr = g1h->heap_region_containing(result);
    return hr->is_humongous();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1IsHumongous: G1 GC is not enabled");
WB_END

WB_ENTRY(jboolean, WB_G1BelongsToHumongousRegion(JNIEnv* env, jobject o, jlong addr))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    const HeapRegion* hr = g1h->heap_region_containing((void*) addr);
    return hr->is_humongous();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1BelongsToHumongousRegion: G1 GC is not enabled");
WB_END

WB_ENTRY(jboolean, WB_G1BelongsToFreeRegion(JNIEnv* env, jobject o, jlong addr))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    const HeapRegion* hr = g1h->heap_region_containing((void*) addr);
    return hr->is_free();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1BelongsToFreeRegion: G1 GC is not enabled");
WB_END

WB_ENTRY(jlong, WB_G1NumMaxRegions(JNIEnv* env, jobject o))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    size_t nr = g1h->max_regions();
    return (jlong)nr;
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1NumMaxRegions: G1 GC is not enabled");
WB_END

WB_ENTRY(jlong, WB_G1NumFreeRegions(JNIEnv* env, jobject o))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    size_t nr = g1h->num_free_regions();
    return (jlong)nr;
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1NumFreeRegions: G1 GC is not enabled");
WB_END

WB_ENTRY(jboolean, WB_G1InConcurrentMark(JNIEnv* env, jobject o))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    return g1h->concurrent_mark()->cm_thread()->during_cycle();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1InConcurrentMark: G1 GC is not enabled");
WB_END

WB_ENTRY(jboolean, WB_G1StartMarkCycle(JNIEnv* env, jobject o))
  if (UseG1GC) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    if (!g1h->concurrent_mark()->cm_thread()->during_cycle()) {
      g1h->collect(GCCause::_wb_conc_mark);
      return true;
    }
    return false;
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1StartMarkCycle: G1 GC is not enabled");
WB_END

WB_ENTRY(jint, WB_G1RegionSize(JNIEnv* env, jobject o))
  if (UseG1GC) {
    return (jint)HeapRegion::GrainBytes;
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1RegionSize: G1 GC is not enabled");
WB_END

#endif // INCLUDE_G1GC

#if INCLUDE_PARALLELGC

WB_ENTRY(jlong, WB_PSVirtualSpaceAlignment(JNIEnv* env, jobject o))
  if (UseParallelGC) {
    return ParallelScavengeHeap::heap()->gens()->virtual_spaces()->alignment();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_PSVirtualSpaceAlignment: Parallel GC is not enabled");
WB_END

WB_ENTRY(jlong, WB_PSHeapGenerationAlignment(JNIEnv* env, jobject o))
  if (UseParallelGC) {
    return ParallelScavengeHeap::heap()->generation_alignment();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_PSHeapGenerationAlignment: Parallel GC is not enabled");
WB_END

#endif // INCLUDE_PARALLELGC

#if INCLUDE_G1GC

WB_ENTRY(jobject, WB_G1AuxiliaryMemoryUsage(JNIEnv* env))
  if (UseG1GC) {
    ResourceMark rm(THREAD);
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    MemoryUsage usage = g1h->get_auxiliary_data_memory_usage();
    Handle h = MemoryService::create_MemoryUsage_obj(usage, CHECK_NULL);
    return JNIHandles::make_local(env, h());
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1AuxiliaryMemoryUsage: G1 GC is not enabled");
WB_END

WB_ENTRY(jint, WB_G1ActiveMemoryNodeCount(JNIEnv* env, jobject o))
  if (UseG1GC) {
    G1NUMA* numa = G1NUMA::numa();
    return (jint)numa->num_active_nodes();
  }
  THROW_MSG_0(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1ActiveMemoryNodeCount: G1 GC is not enabled");
WB_END

WB_ENTRY(jintArray, WB_G1MemoryNodeIds(JNIEnv* env, jobject o))
  if (UseG1GC) {
    G1NUMA* numa = G1NUMA::numa();
    int num_node_ids = (int)numa->num_active_nodes();
    const int* node_ids = numa->node_ids();

    typeArrayOop result = oopFactory::new_intArray(num_node_ids, CHECK_NULL);
    for (int i = 0; i < num_node_ids; i++) {
      result->int_at_put(i, (jint)node_ids[i]);
    }
    return (jintArray) JNIHandles::make_local(env, result);
  }
  THROW_MSG_NULL(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1MemoryNodeIds: G1 GC is not enabled");
WB_END

class OldRegionsLivenessClosure: public HeapRegionClosure {

 private:
  const int _liveness;
  size_t _total_count;
  size_t _total_memory;
  size_t _total_memory_to_free;

 public:
  OldRegionsLivenessClosure(int liveness) :
    _liveness(liveness),
    _total_count(0),
    _total_memory(0),
    _total_memory_to_free(0) { }

    size_t total_count() { return _total_count; }
    size_t total_memory() { return _total_memory; }
    size_t total_memory_to_free() { return _total_memory_to_free; }

  bool do_heap_region(HeapRegion* r) {
    if (r->is_old()) {
      size_t prev_live = r->marked_bytes();
      size_t live = r->live_bytes();
      size_t size = r->used();
      size_t reg_size = HeapRegion::GrainBytes;
      if (size > 0 && ((int)(live * 100 / size) < _liveness)) {
        _total_memory += size;
        ++_total_count;
        if (size == reg_size) {
        // we don't include non-full regions since they are unlikely included in mixed gc
        // for testing purposes it's enough to have lowest estimation of total memory that is expected to be freed
          _total_memory_to_free += size - prev_live;
        }
      }
    }
    return false;
  }
};


WB_ENTRY(jlongArray, WB_G1GetMixedGCInfo(JNIEnv* env, jobject o, jint liveness))
  if (!UseG1GC) {
    THROW_MSG_NULL(vmSymbols::java_lang_UnsupportedOperationException(), "WB_G1GetMixedGCInfo: G1 GC is not enabled");
  }
  if (liveness < 0) {
    THROW_MSG_NULL(vmSymbols::java_lang_IllegalArgumentException(), "liveness value should be non-negative");
  }

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  OldRegionsLivenessClosure rli(liveness);
  g1h->heap_region_iterate(&rli);

  typeArrayOop result = oopFactory::new_longArray(3, CHECK_NULL);
  result->long_at_put(0, rli.total_count());
  result->long_at_put(1, rli.total_memory());
  result->long_at_put(2, rli.total_memory_to_free());
  return (jlongArray) JNIHandles::make_local(env, result);
WB_END

#endif // INCLUDE_G1GC

#if INCLUDE_NMT
// Alloc memory using the test memory type so that we can use that to see if
// NMT picks it up correctly
WB_ENTRY(jlong, WB_NMTMalloc(JNIEnv* env, jobject o, jlong size))
  jlong addr = 0;
  addr = (jlong)(uintptr_t)os::malloc(size, mtTest);
  return addr;
WB_END

// Alloc memory with pseudo call stack. The test can create psudo malloc
// allocation site to stress the malloc tracking.
WB_ENTRY(jlong, WB_NMTMallocWithPseudoStack(JNIEnv* env, jobject o, jlong size, jint pseudo_stack))
  address pc = (address)(size_t)pseudo_stack;
  NativeCallStack stack(&pc, 1);
  return (jlong)(uintptr_t)os::malloc(size, mtTest, stack);
WB_END

// Alloc memory with pseudo call stack and specific memory type.
WB_ENTRY(jlong, WB_NMTMallocWithPseudoStackAndType(JNIEnv* env, jobject o, jlong size, jint pseudo_stack, jint type))
  address pc = (address)(size_t)pseudo_stack;
  NativeCallStack stack(&pc, 1);
  return (jlong)(uintptr_t)os::malloc(size, (MEMFLAGS)type, stack);
WB_END

// Free the memory allocated by NMTAllocTest
WB_ENTRY(void, WB_NMTFree(JNIEnv* env, jobject o, jlong mem))
  os::free((void*)(uintptr_t)mem);
WB_END

WB_ENTRY(jlong, WB_NMTReserveMemory(JNIEnv* env, jobject o, jlong size))
  jlong addr = 0;

  addr = (jlong)(uintptr_t)os::reserve_memory(size);
  MemTracker::record_virtual_memory_type((address)addr, mtTest);

  return addr;
WB_END

WB_ENTRY(jlong, WB_NMTAttemptReserveMemoryAt(JNIEnv* env, jobject o, jlong addr, jlong size))
  addr = (jlong)(uintptr_t)os::attempt_reserve_memory_at((size_t)size, (char*)(uintptr_t)addr);
  MemTracker::record_virtual_memory_type((address)addr, mtTest);

  return addr;
WB_END

WB_ENTRY(void, WB_NMTCommitMemory(JNIEnv* env, jobject o, jlong addr, jlong size))
  os::commit_memory((char *)(uintptr_t)addr, size, !ExecMem);
  MemTracker::record_virtual_memory_type((address)(uintptr_t)addr, mtTest);
WB_END

WB_ENTRY(void, WB_NMTUncommitMemory(JNIEnv* env, jobject o, jlong addr, jlong size))
  os::uncommit_memory((char *)(uintptr_t)addr, size);
WB_END

WB_ENTRY(void, WB_NMTReleaseMemory(JNIEnv* env, jobject o, jlong addr, jlong size))
  os::release_memory((char *)(uintptr_t)addr, size);
WB_END

WB_ENTRY(jboolean, WB_NMTChangeTrackingLevel(JNIEnv* env))
  // Test that we can downgrade NMT levels but not upgrade them.
  if (MemTracker::tracking_level() == NMT_off) {
    MemTracker::transition_to(NMT_off);
    return MemTracker::tracking_level() == NMT_off;
  } else {
    assert(MemTracker::tracking_level() == NMT_detail, "Should start out as detail tracking");
    MemTracker::transition_to(NMT_summary);
    assert(MemTracker::tracking_level() == NMT_summary, "Should be summary now");

    // Can't go to detail once NMT is set to summary.
    MemTracker::transition_to(NMT_detail);
    assert(MemTracker::tracking_level() == NMT_summary, "Should still be summary now");

    // Shutdown sets tracking level to minimal.
    MemTracker::shutdown();
    assert(MemTracker::tracking_level() == NMT_minimal, "Should be minimal now");

    // Once the tracking level is minimal, we cannot increase to summary.
    // The code ignores this request instead of asserting because if the malloc site
    // table overflows in another thread, it tries to change the code to summary.
    MemTracker::transition_to(NMT_summary);
    assert(MemTracker::tracking_level() == NMT_minimal, "Should still be minimal now");

    // Really can never go up to detail, verify that the code would never do this.
    MemTracker::transition_to(NMT_detail);
    assert(MemTracker::tracking_level() == NMT_minimal, "Should still be minimal now");
    return MemTracker::tracking_level() == NMT_minimal;
  }
WB_END

WB_ENTRY(jint, WB_NMTGetHashSize(JNIEnv* env, jobject o))
  int hash_size = MallocSiteTable::hash_buckets();
  assert(hash_size > 0, "NMT hash_size should be > 0");
  return (jint)hash_size;
WB_END

WB_ENTRY(jlong, WB_NMTNewArena(JNIEnv* env, jobject o, jlong init_size))
  Arena* arena =  new (mtTest) Arena(mtTest, size_t(init_size));
  return (jlong)arena;
WB_END

WB_ENTRY(void, WB_NMTFreeArena(JNIEnv* env, jobject o, jlong arena))
  Arena* a = (Arena*)arena;
  delete a;
WB_END

WB_ENTRY(void, WB_NMTArenaMalloc(JNIEnv* env, jobject o, jlong arena, jlong size))
  Arena* a = (Arena*)arena;
  a->Amalloc(size_t(size));
WB_END
#endif // INCLUDE_NMT

static jmethodID reflected_method_to_jmid(JavaThread* thread, JNIEnv* env, jobject method) {
  assert(method != NULL, "method should not be null");
  ThreadToNativeFromVM ttn(thread);
  return env->FromReflectedMethod(method);
}

static CompLevel highestCompLevel() {
  return TieredCompilation ? MIN2((CompLevel) TieredStopAtLevel, CompLevel_highest_tier) : CompLevel_highest_tier;
}

// Deoptimizes all compiled frames and makes nmethods not entrant if it's requested
class VM_WhiteBoxDeoptimizeFrames : public VM_WhiteBoxOperation {
 private:
  int _result;
  const bool _make_not_entrant;
 public:
  VM_WhiteBoxDeoptimizeFrames(bool make_not_entrant) :
        _result(0), _make_not_entrant(make_not_entrant) { }
  int  result() const { return _result; }

  void doit() {
    for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
      if (t->has_last_Java_frame()) {
        for (StackFrameStream fst(t, UseBiasedLocking); !fst.is_done(); fst.next()) {
          frame* f = fst.current();
          if (f->can_be_deoptimized() && !f->is_deoptimized_frame()) {
            RegisterMap* reg_map = fst.register_map();
            Deoptimization::deoptimize(t, *f, reg_map);
            if (_make_not_entrant) {
                CompiledMethod* cm = CodeCache::find_compiled(f->pc());
                assert(cm != NULL, "sanity check");
                cm->make_not_entrant();
            }
            ++_result;
          }
        }
      }
    }
  }
};

WB_ENTRY(jint, WB_DeoptimizeFrames(JNIEnv* env, jobject o, jboolean make_not_entrant))
  VM_WhiteBoxDeoptimizeFrames op(make_not_entrant == JNI_TRUE);
  VMThread::execute(&op);
  return op.result();
WB_END

WB_ENTRY(void, WB_DeoptimizeAll(JNIEnv* env, jobject o))
  MutexLockerEx mu(Compile_lock);
  CodeCache::mark_all_nmethods_for_deoptimization();
  VM_Deoptimize op;
  VMThread::execute(&op);
WB_END

WB_ENTRY(jint, WB_DeoptimizeMethod(JNIEnv* env, jobject o, jobject method, jboolean is_osr))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  int result = 0;
  CHECK_JNI_EXCEPTION_(env, result);
  MutexLockerEx mu(Compile_lock);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  if (is_osr) {
    result += mh->mark_osr_nmethods();
  } else if (mh->code() != NULL) {
    mh->code()->mark_for_deoptimization();
    ++result;
  }
  result += CodeCache::mark_for_deoptimization(mh());
  if (result > 0) {
    VM_Deoptimize op;
    VMThread::execute(&op);
  }
  return result;
WB_END

WB_ENTRY(jboolean, WB_IsMethodCompiled(JNIEnv* env, jobject o, jobject method, jboolean is_osr))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  MutexLockerEx mu(Compile_lock);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  CompiledMethod* code = is_osr ? mh->lookup_osr_nmethod_for(InvocationEntryBci, CompLevel_none, false) : mh->code();
  if (code == NULL) {
    return JNI_FALSE;
  }
  return (code->is_alive() && !code->is_marked_for_deoptimization());
WB_END

WB_ENTRY(jboolean, WB_IsMethodCompilable(JNIEnv* env, jobject o, jobject method, jint comp_level, jboolean is_osr))
  if (method == NULL || comp_level > highestCompLevel()) {
    return false;
  }
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  MutexLockerEx mu(Compile_lock);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  if (is_osr) {
    return CompilationPolicy::can_be_osr_compiled(mh, comp_level);
  } else {
    return CompilationPolicy::can_be_compiled(mh, comp_level);
  }
WB_END

WB_ENTRY(jboolean, WB_IsMethodQueuedForCompilation(JNIEnv* env, jobject o, jobject method))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  MutexLockerEx mu(Compile_lock);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  return mh->queued_for_compilation();
WB_END

WB_ENTRY(jboolean, WB_IsIntrinsicAvailable(JNIEnv* env, jobject o, jobject method, jobject compilation_context, jint compLevel))
  if (compLevel < CompLevel_none || compLevel > highestCompLevel()) {
    return false; // Intrinsic is not available on a non-existent compilation level.
  }
  jmethodID method_id, compilation_context_id;
  method_id = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(method_id));

  DirectiveSet* directive;
  AbstractCompiler* comp = CompileBroker::compiler((int)compLevel);
  assert(comp != NULL, "compiler not available");
  if (compilation_context != NULL) {
    compilation_context_id = reflected_method_to_jmid(thread, env, compilation_context);
    CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
    methodHandle cch(THREAD, Method::checked_resolve_jmethod_id(compilation_context_id));
    directive = DirectivesStack::getMatchingDirective(cch, comp);
  } else {
    // Calling with NULL matches default directive
    directive = DirectivesStack::getDefaultDirective(comp);
  }
  bool result = comp->is_intrinsic_available(mh, directive);
  DirectivesStack::release(directive);
  return result;
WB_END

WB_ENTRY(jint, WB_GetMethodCompilationLevel(JNIEnv* env, jobject o, jobject method, jboolean is_osr))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, CompLevel_none);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  CompiledMethod* code = is_osr ? mh->lookup_osr_nmethod_for(InvocationEntryBci, CompLevel_none, false) : mh->code();
  return (code != NULL ? code->comp_level() : CompLevel_none);
WB_END

WB_ENTRY(void, WB_MakeMethodNotCompilable(JNIEnv* env, jobject o, jobject method, jint comp_level, jboolean is_osr))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION(env);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  if (is_osr) {
    mh->set_not_osr_compilable(comp_level, true /* report */, "WhiteBox");
  } else {
    mh->set_not_compilable(comp_level, true /* report */, "WhiteBox");
  }
WB_END

WB_ENTRY(jint, WB_GetMethodEntryBci(JNIEnv* env, jobject o, jobject method))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, InvocationEntryBci);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  CompiledMethod* code = mh->lookup_osr_nmethod_for(InvocationEntryBci, CompLevel_none, false);
  return (code != NULL && code->is_osr_method() ? code->osr_entry_bci() : InvocationEntryBci);
WB_END

WB_ENTRY(jboolean, WB_TestSetDontInlineMethod(JNIEnv* env, jobject o, jobject method, jboolean value))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  bool result = mh->dont_inline();
  mh->set_dont_inline(value == JNI_TRUE);
  return result;
WB_END

WB_ENTRY(jint, WB_GetCompileQueueSize(JNIEnv* env, jobject o, jint comp_level))
  if (comp_level == CompLevel_any) {
    return CompileBroker::queue_size(CompLevel_full_optimization) /* C2 */ +
        CompileBroker::queue_size(CompLevel_full_profile) /* C1 */;
  } else {
    return CompileBroker::queue_size(comp_level);
  }
WB_END

WB_ENTRY(jboolean, WB_TestSetForceInlineMethod(JNIEnv* env, jobject o, jobject method, jboolean value))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  bool result = mh->force_inline();
  mh->set_force_inline(value == JNI_TRUE);
  return result;
WB_END

#ifdef LINUX
bool WhiteBox::validate_cgroup(const char* proc_cgroups,
                               const char* proc_self_cgroup,
                               const char* proc_self_mountinfo,
                               u1* cg_flags) {
  CgroupInfo cg_infos[CG_INFO_LENGTH];
  return CgroupSubsystemFactory::determine_type(cg_infos, proc_cgroups,
                                                    proc_self_cgroup,
                                                    proc_self_mountinfo, cg_flags);
}
#endif

bool WhiteBox::compile_method(Method* method, int comp_level, int bci, Thread* THREAD) {
  // Screen for unavailable/bad comp level or null method
  AbstractCompiler* comp = CompileBroker::compiler(comp_level);
  if (method == NULL) {
    tty->print_cr("WB error: request to compile NULL method");
    return false;
  }
  if (comp_level > highestCompLevel()) {
    tty->print_cr("WB error: invalid compilation level %d", comp_level);
    return false;
  }
  if (comp == NULL) {
    tty->print_cr("WB error: no compiler for requested compilation level %d", comp_level);
    return false;
  }

  // Check if compilation is blocking
  methodHandle mh(THREAD, method);
  DirectiveSet* directive = DirectivesStack::getMatchingDirective(mh, comp);
  bool is_blocking = !directive->BackgroundCompilationOption;
  DirectivesStack::release(directive);

  // Compile method and check result
  nmethod* nm = CompileBroker::compile_method(mh, bci, comp_level, mh, mh->invocation_count(), CompileTask::Reason_Whitebox, THREAD);
  MutexLockerEx mu(Compile_lock);
  bool is_queued = mh->queued_for_compilation();
  if ((!is_blocking && is_queued) || nm != NULL) {
    return true;
  }
  tty->print("WB error: failed to %s compile at level %d method ", is_blocking ? "blocking" : "", comp_level);
  mh->print_short_name(tty);
  tty->cr();
  if (is_blocking && is_queued) {
    tty->print_cr("WB error: blocking compilation is still in queue!");
  }
  return false;
}

WB_ENTRY(jboolean, WB_EnqueueMethodForCompilation(JNIEnv* env, jobject o, jobject method, jint comp_level, jint bci))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);
  return WhiteBox::compile_method(Method::checked_resolve_jmethod_id(jmid), comp_level, bci, THREAD);
WB_END

WB_ENTRY(jboolean, WB_EnqueueInitializerForCompilation(JNIEnv* env, jobject o, jclass klass, jint comp_level))
  InstanceKlass* ik = InstanceKlass::cast(java_lang_Class::as_Klass(JNIHandles::resolve(klass)));
  Method* clinit = ik->class_initializer();
  if (clinit == NULL) {
    return false;
  }
  return WhiteBox::compile_method(clinit, comp_level, InvocationEntryBci, THREAD);
WB_END

WB_ENTRY(jboolean, WB_ShouldPrintAssembly(JNIEnv* env, jobject o, jobject method, jint comp_level))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);

  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  DirectiveSet* directive = DirectivesStack::getMatchingDirective(mh, CompileBroker::compiler(comp_level));
  bool result = directive->PrintAssemblyOption;
  DirectivesStack::release(directive);

  return result;
WB_END

WB_ENTRY(jint, WB_MatchesInline(JNIEnv* env, jobject o, jobject method, jstring pattern))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);

  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));

  ResourceMark rm;
  const char* error_msg = NULL;
  char* method_str = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(pattern));
  InlineMatcher* m = InlineMatcher::parse_inline_pattern(method_str, error_msg);

  if (m == NULL) {
    assert(error_msg != NULL, "Always have an error message");
    tty->print_cr("Got error: %s", error_msg);
    return -1; // Pattern failed
  }

  // Pattern works - now check if it matches
  int result;
  if (m->match(mh, InlineMatcher::force_inline)) {
    result = 2; // Force inline match
  } else if (m->match(mh, InlineMatcher::dont_inline)) {
    result = 1; // Dont inline match
  } else {
    result = 0; // No match
  }
  delete m;
  return result;
WB_END

WB_ENTRY(jint, WB_MatchesMethod(JNIEnv* env, jobject o, jobject method, jstring pattern))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, JNI_FALSE);

  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));

  ResourceMark rm;
  char* method_str = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(pattern));

  const char* error_msg = NULL;

  BasicMatcher* m = BasicMatcher::parse_method_pattern(method_str, error_msg);
  if (m == NULL) {
    assert(error_msg != NULL, "Must have error_msg");
    tty->print_cr("Got error: %s", error_msg);
    return -1;
  }

  // Pattern works - now check if it matches
  int result = m->matches(mh);
  delete m;
  assert(result == 0 || result == 1, "Result out of range");
  return result;
WB_END

WB_ENTRY(void, WB_MarkMethodProfiled(JNIEnv* env, jobject o, jobject method))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION(env);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));

  MethodData* mdo = mh->method_data();
  if (mdo == NULL) {
    Method::build_interpreter_method_data(mh, CHECK_AND_CLEAR);
    mdo = mh->method_data();
  }
  mdo->init();
  InvocationCounter* icnt = mdo->invocation_counter();
  InvocationCounter* bcnt = mdo->backedge_counter();
  // set i-counter according to TieredThresholdPolicy::is_method_profiled
  icnt->set(InvocationCounter::wait_for_compile, Tier4MinInvocationThreshold);
  bcnt->set(InvocationCounter::wait_for_compile, Tier4CompileThreshold);
WB_END

WB_ENTRY(void, WB_ClearMethodState(JNIEnv* env, jobject o, jobject method))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION(env);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  MutexLockerEx mu(Compile_lock);
  MethodData* mdo = mh->method_data();
  MethodCounters* mcs = mh->method_counters();

  if (mdo != NULL) {
    mdo->init();
    ResourceMark rm;
    int arg_count = mdo->method()->size_of_parameters();
    for (int i = 0; i < arg_count; i++) {
      mdo->set_arg_modified(i, 0);
    }
    MutexLockerEx mu(mdo->extra_data_lock());
    mdo->clean_method_data(/*always_clean*/true);
  }

  mh->clear_not_c1_compilable();
  mh->clear_not_c2_compilable();
  mh->clear_not_c2_osr_compilable();
  NOT_PRODUCT(mh->set_compiled_invocation_count(0));
  if (mcs != NULL) {
    mcs->backedge_counter()->init();
    mcs->invocation_counter()->init();
    mcs->set_interpreter_invocation_count(0);
    mcs->set_interpreter_throwout_count(0);

#ifdef TIERED
    mcs->set_rate(0.0F);
    mh->set_prev_event_count(0);
    mh->set_prev_time(0);
#endif
  }
WB_END

template <typename T>
static bool GetVMFlag(JavaThread* thread, JNIEnv* env, jstring name, T* value, JVMFlag::Error (*TAt)(const char*, T*, bool, bool)) {
  if (name == NULL) {
    return false;
  }
  ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
  const char* flag_name = env->GetStringUTFChars(name, NULL);
  CHECK_JNI_EXCEPTION_(env, false);
  JVMFlag::Error result = (*TAt)(flag_name, value, true, true);
  env->ReleaseStringUTFChars(name, flag_name);
  return (result == JVMFlag::SUCCESS);
}

template <typename T>
static bool SetVMFlag(JavaThread* thread, JNIEnv* env, jstring name, T* value, JVMFlag::Error (*TAtPut)(const char*, T*, JVMFlag::Flags)) {
  if (name == NULL) {
    return false;
  }
  ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
  const char* flag_name = env->GetStringUTFChars(name, NULL);
  CHECK_JNI_EXCEPTION_(env, false);
  JVMFlag::Error result = (*TAtPut)(flag_name, value, JVMFlag::INTERNAL);
  env->ReleaseStringUTFChars(name, flag_name);
  return (result == JVMFlag::SUCCESS);
}

template <typename T>
static jobject box(JavaThread* thread, JNIEnv* env, Symbol* name, Symbol* sig, T value) {
  ResourceMark rm(thread);
  jclass clazz = env->FindClass(name->as_C_string());
  CHECK_JNI_EXCEPTION_(env, NULL);
  jmethodID methodID = env->GetStaticMethodID(clazz,
        vmSymbols::valueOf_name()->as_C_string(),
        sig->as_C_string());
  CHECK_JNI_EXCEPTION_(env, NULL);
  jobject result = env->CallStaticObjectMethod(clazz, methodID, value);
  CHECK_JNI_EXCEPTION_(env, NULL);
  return result;
}

static jobject booleanBox(JavaThread* thread, JNIEnv* env, jboolean value) {
  return box(thread, env, vmSymbols::java_lang_Boolean(), vmSymbols::Boolean_valueOf_signature(), value);
}
static jobject integerBox(JavaThread* thread, JNIEnv* env, jint value) {
  return box(thread, env, vmSymbols::java_lang_Integer(), vmSymbols::Integer_valueOf_signature(), value);
}
static jobject longBox(JavaThread* thread, JNIEnv* env, jlong value) {
  return box(thread, env, vmSymbols::java_lang_Long(), vmSymbols::Long_valueOf_signature(), value);
}
/* static jobject floatBox(JavaThread* thread, JNIEnv* env, jfloat value) {
  return box(thread, env, vmSymbols::java_lang_Float(), vmSymbols::Float_valueOf_signature(), value);
}*/
static jobject doubleBox(JavaThread* thread, JNIEnv* env, jdouble value) {
  return box(thread, env, vmSymbols::java_lang_Double(), vmSymbols::Double_valueOf_signature(), value);
}

static JVMFlag* getVMFlag(JavaThread* thread, JNIEnv* env, jstring name) {
  ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
  const char* flag_name = env->GetStringUTFChars(name, NULL);
  CHECK_JNI_EXCEPTION_(env, NULL);
  JVMFlag* result = JVMFlag::find_flag(flag_name, strlen(flag_name), true, true);
  env->ReleaseStringUTFChars(name, flag_name);
  return result;
}

WB_ENTRY(jboolean, WB_IsConstantVMFlag(JNIEnv* env, jobject o, jstring name))
  JVMFlag* flag = getVMFlag(thread, env, name);
  return (flag != NULL) && flag->is_constant_in_binary();
WB_END

WB_ENTRY(jboolean, WB_IsLockedVMFlag(JNIEnv* env, jobject o, jstring name))
  JVMFlag* flag = getVMFlag(thread, env, name);
  return (flag != NULL) && !(flag->is_unlocked() || flag->is_unlocker());
WB_END

WB_ENTRY(jobject, WB_GetBooleanVMFlag(JNIEnv* env, jobject o, jstring name))
  bool result;
  if (GetVMFlag <bool> (thread, env, name, &result, &JVMFlag::boolAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return booleanBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetIntVMFlag(JNIEnv* env, jobject o, jstring name))
  int result;
  if (GetVMFlag <int> (thread, env, name, &result, &JVMFlag::intAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetUintVMFlag(JNIEnv* env, jobject o, jstring name))
  uint result;
  if (GetVMFlag <uint> (thread, env, name, &result, &JVMFlag::uintAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetIntxVMFlag(JNIEnv* env, jobject o, jstring name))
  intx result;
  if (GetVMFlag <intx> (thread, env, name, &result, &JVMFlag::intxAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetUintxVMFlag(JNIEnv* env, jobject o, jstring name))
  uintx result;
  if (GetVMFlag <uintx> (thread, env, name, &result, &JVMFlag::uintxAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetUint64VMFlag(JNIEnv* env, jobject o, jstring name))
  uint64_t result;
  if (GetVMFlag <uint64_t> (thread, env, name, &result, &JVMFlag::uint64_tAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetSizeTVMFlag(JNIEnv* env, jobject o, jstring name))
  size_t result;
  if (GetVMFlag <size_t> (thread, env, name, &result, &JVMFlag::size_tAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetDoubleVMFlag(JNIEnv* env, jobject o, jstring name))
  double result;
  if (GetVMFlag <double> (thread, env, name, &result, &JVMFlag::doubleAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    return doubleBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jstring, WB_GetStringVMFlag(JNIEnv* env, jobject o, jstring name))
  ccstr ccstrResult;
  if (GetVMFlag <ccstr> (thread, env, name, &ccstrResult, &JVMFlag::ccstrAt)) {
    ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
    jstring result = env->NewStringUTF(ccstrResult);
    CHECK_JNI_EXCEPTION_(env, NULL);
    return result;
  }
  return NULL;
WB_END

WB_ENTRY(void, WB_SetBooleanVMFlag(JNIEnv* env, jobject o, jstring name, jboolean value))
  bool result = value == JNI_TRUE ? true : false;
  SetVMFlag <bool> (thread, env, name, &result, &JVMFlag::boolAtPut);
WB_END

WB_ENTRY(void, WB_SetIntVMFlag(JNIEnv* env, jobject o, jstring name, jlong value))
  int result = value;
  SetVMFlag <int> (thread, env, name, &result, &JVMFlag::intAtPut);
WB_END

WB_ENTRY(void, WB_SetUintVMFlag(JNIEnv* env, jobject o, jstring name, jlong value))
  uint result = value;
  SetVMFlag <uint> (thread, env, name, &result, &JVMFlag::uintAtPut);
WB_END

WB_ENTRY(void, WB_SetIntxVMFlag(JNIEnv* env, jobject o, jstring name, jlong value))
  intx result = value;
  SetVMFlag <intx> (thread, env, name, &result, &JVMFlag::intxAtPut);
WB_END

WB_ENTRY(void, WB_SetUintxVMFlag(JNIEnv* env, jobject o, jstring name, jlong value))
  uintx result = value;
  SetVMFlag <uintx> (thread, env, name, &result, &JVMFlag::uintxAtPut);
WB_END

WB_ENTRY(void, WB_SetUint64VMFlag(JNIEnv* env, jobject o, jstring name, jlong value))
  uint64_t result = value;
  SetVMFlag <uint64_t> (thread, env, name, &result, &JVMFlag::uint64_tAtPut);
WB_END

WB_ENTRY(void, WB_SetSizeTVMFlag(JNIEnv* env, jobject o, jstring name, jlong value))
  size_t result = value;
  SetVMFlag <size_t> (thread, env, name, &result, &JVMFlag::size_tAtPut);
WB_END

WB_ENTRY(void, WB_SetDoubleVMFlag(JNIEnv* env, jobject o, jstring name, jdouble value))
  double result = value;
  SetVMFlag <double> (thread, env, name, &result, &JVMFlag::doubleAtPut);
WB_END

WB_ENTRY(void, WB_SetStringVMFlag(JNIEnv* env, jobject o, jstring name, jstring value))
  ThreadToNativeFromVM ttnfv(thread);   // can't be in VM when we call JNI
  const char* ccstrValue;
  if (value == NULL) {
    ccstrValue = NULL;
  }
  else {
    ccstrValue = env->GetStringUTFChars(value, NULL);
    CHECK_JNI_EXCEPTION(env);
  }
  ccstr ccstrResult = ccstrValue;
  bool needFree;
  {
    ThreadInVMfromNative ttvfn(thread); // back to VM
    needFree = SetVMFlag <ccstr> (thread, env, name, &ccstrResult, &JVMFlag::ccstrAtPut);
  }
  if (value != NULL) {
    env->ReleaseStringUTFChars(value, ccstrValue);
  }
  if (needFree) {
    FREE_C_HEAP_ARRAY(char, ccstrResult);
  }
WB_END

WB_ENTRY(void, WB_LockCompilation(JNIEnv* env, jobject o, jlong timeout))
  WhiteBox::compilation_locked = true;
WB_END

WB_ENTRY(void, WB_UnlockCompilation(JNIEnv* env, jobject o))
  MonitorLockerEx mo(Compilation_lock, Mutex::_no_safepoint_check_flag);
  WhiteBox::compilation_locked = false;
  mo.notify_all();
WB_END

WB_ENTRY(void, WB_ForceNMethodSweep(JNIEnv* env, jobject o))
  // Force a code cache sweep and block until it finished
  NMethodSweeper::force_sweep();
WB_END

WB_ENTRY(jboolean, WB_IsInStringTable(JNIEnv* env, jobject o, jstring javaString))
  ResourceMark rm(THREAD);
  int len;
  jchar* name = java_lang_String::as_unicode_string(JNIHandles::resolve(javaString), len, CHECK_false);
  return (StringTable::lookup(name, len) != NULL);
WB_END

WB_ENTRY(void, WB_FullGC(JNIEnv* env, jobject o))
  Universe::heap()->soft_ref_policy()->set_should_clear_all_soft_refs(true);
  Universe::heap()->collect(GCCause::_wb_full_gc);
#if INCLUDE_G1GC
  if (UseG1GC) {
    // Needs to be cleared explicitly for G1
    Universe::heap()->soft_ref_policy()->set_should_clear_all_soft_refs(false);
  }
#endif // INCLUDE_G1GC
WB_END

WB_ENTRY(void, WB_YoungGC(JNIEnv* env, jobject o))
  Universe::heap()->collect(GCCause::_wb_young_gc);
WB_END

WB_ENTRY(void, WB_ReadReservedMemory(JNIEnv* env, jobject o))
  // static+volatile in order to force the read to happen
  // (not be eliminated by the compiler)
  static char c;
  static volatile char* p;

  p = os::reserve_memory(os::vm_allocation_granularity(), NULL, 0);
  if (p == NULL) {
    THROW_MSG(vmSymbols::java_lang_OutOfMemoryError(), "Failed to reserve memory");
  }

  c = *p;
WB_END

WB_ENTRY(jstring, WB_GetCPUFeatures(JNIEnv* env, jobject o))
  const char* features = VM_Version::features_string();
  ThreadToNativeFromVM ttn(thread);
  jstring features_string = env->NewStringUTF(features);

  CHECK_JNI_EXCEPTION_(env, NULL);

  return features_string;
WB_END

int WhiteBox::get_blob_type(const CodeBlob* code) {
  guarantee(WhiteBoxAPI, "internal testing API :: WhiteBox has to be enabled");
  if (code->is_aot()) {
    return -1;
  }
  return CodeCache::get_code_heap(code)->code_blob_type();
}

CodeHeap* WhiteBox::get_code_heap(int blob_type) {
  guarantee(WhiteBoxAPI, "internal testing API :: WhiteBox has to be enabled");
  return CodeCache::get_code_heap(blob_type);
}

struct CodeBlobStub {
  CodeBlobStub(const CodeBlob* blob) :
      name(os::strdup(blob->name())),
      size(blob->size()),
      blob_type(WhiteBox::get_blob_type(blob)),
      address((jlong) blob) { }
  ~CodeBlobStub() { os::free((void*) name); }
  const char* const name;
  const jint        size;
  const jint        blob_type;
  const jlong       address;
};

static jobjectArray codeBlob2objectArray(JavaThread* thread, JNIEnv* env, CodeBlobStub* cb) {
  ResourceMark rm;
  jclass clazz = env->FindClass(vmSymbols::java_lang_Object()->as_C_string());
  CHECK_JNI_EXCEPTION_(env, NULL);
  jobjectArray result = env->NewObjectArray(4, clazz, NULL);

  jstring name = env->NewStringUTF(cb->name);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 0, name);

  jobject obj = integerBox(thread, env, cb->size);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 1, obj);

  obj = integerBox(thread, env, cb->blob_type);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 2, obj);

  obj = longBox(thread, env, cb->address);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 3, obj);

  return result;
}

WB_ENTRY(jobjectArray, WB_GetNMethod(JNIEnv* env, jobject o, jobject method, jboolean is_osr))
  ResourceMark rm(THREAD);
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, NULL);
  methodHandle mh(THREAD, Method::checked_resolve_jmethod_id(jmid));
  CompiledMethod* code = is_osr ? mh->lookup_osr_nmethod_for(InvocationEntryBci, CompLevel_none, false) : mh->code();
  jobjectArray result = NULL;
  if (code == NULL) {
    return result;
  }
  int comp_level = code->comp_level();
  int insts_size = comp_level == CompLevel_aot ? code->code_end() - code->code_begin() : code->insts_size();

  ThreadToNativeFromVM ttn(thread);
  jclass clazz = env->FindClass(vmSymbols::java_lang_Object()->as_C_string());
  CHECK_JNI_EXCEPTION_(env, NULL);
  result = env->NewObjectArray(5, clazz, NULL);
  if (result == NULL) {
    return result;
  }

  CodeBlobStub stub(code);
  jobjectArray codeBlob = codeBlob2objectArray(thread, env, &stub);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 0, codeBlob);

  jobject level = integerBox(thread, env, comp_level);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 1, level);

  jbyteArray insts = env->NewByteArray(insts_size);
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetByteArrayRegion(insts, 0, insts_size, (jbyte*) code->insts_begin());
  env->SetObjectArrayElement(result, 2, insts);

  jobject id = integerBox(thread, env, code->compile_id());
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 3, id);

  jobject entry_point = longBox(thread, env, (jlong) code->entry_point());
  CHECK_JNI_EXCEPTION_(env, NULL);
  env->SetObjectArrayElement(result, 4, entry_point);

  return result;
WB_END

CodeBlob* WhiteBox::allocate_code_blob(int size, int blob_type) {
  guarantee(WhiteBoxAPI, "internal testing API :: WhiteBox has to be enabled");
  BufferBlob* blob;
  int full_size = CodeBlob::align_code_offset(sizeof(BufferBlob));
  if (full_size < size) {
    full_size += align_up(size - full_size, oopSize);
  }
  {
    MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    blob = (BufferBlob*) CodeCache::allocate(full_size, blob_type);
    if (blob != NULL) {
      ::new (blob) BufferBlob("WB::DummyBlob", full_size);
    }
  }
  // Track memory usage statistic after releasing CodeCache_lock
  MemoryService::track_code_cache_memory_usage();
  return blob;
}

WB_ENTRY(jlong, WB_AllocateCodeBlob(JNIEnv* env, jobject o, jint size, jint blob_type))
  if (size < 0) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
      err_msg("WB_AllocateCodeBlob: size is negative: " INT32_FORMAT, size));
  }
  return (jlong) WhiteBox::allocate_code_blob(size, blob_type);
WB_END

WB_ENTRY(void, WB_FreeCodeBlob(JNIEnv* env, jobject o, jlong addr))
  if (addr == 0) {
    return;
  }
  BufferBlob::free((BufferBlob*) addr);
WB_END

WB_ENTRY(jobjectArray, WB_GetCodeHeapEntries(JNIEnv* env, jobject o, jint blob_type))
  ResourceMark rm;
  GrowableArray<CodeBlobStub*> blobs;
  {
    MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    CodeHeap* heap = WhiteBox::get_code_heap(blob_type);
    if (heap == NULL) {
      return NULL;
    }
    for (CodeBlob* cb = (CodeBlob*) heap->first();
         cb != NULL; cb = (CodeBlob*) heap->next(cb)) {
      CodeBlobStub* stub = NEW_RESOURCE_OBJ(CodeBlobStub);
      new (stub) CodeBlobStub(cb);
      blobs.append(stub);
    }
  }
  ThreadToNativeFromVM ttn(thread);
  jobjectArray result = NULL;
  jclass clazz = env->FindClass(vmSymbols::java_lang_Object()->as_C_string());
  CHECK_JNI_EXCEPTION_(env, NULL);
  result = env->NewObjectArray(blobs.length(), clazz, NULL);
  CHECK_JNI_EXCEPTION_(env, NULL);
  if (result == NULL) {
    return result;
  }
  int i = 0;
  for (GrowableArrayIterator<CodeBlobStub*> it = blobs.begin();
       it != blobs.end(); ++it) {
    jobjectArray obj = codeBlob2objectArray(thread, env, *it);
    CHECK_JNI_EXCEPTION_(env, NULL);
    env->SetObjectArrayElement(result, i, obj);
    CHECK_JNI_EXCEPTION_(env, NULL);
    ++i;
  }
  return result;
WB_END

WB_ENTRY(jint, WB_GetCompilationActivityMode(JNIEnv* env, jobject o))
  return CompileBroker::get_compilation_activity_mode();
WB_END

WB_ENTRY(jobjectArray, WB_GetCodeBlob(JNIEnv* env, jobject o, jlong addr))
  if (addr == 0) {
    THROW_MSG_NULL(vmSymbols::java_lang_NullPointerException(),
      "WB_GetCodeBlob: addr is null");
  }
  ThreadToNativeFromVM ttn(thread);
  CodeBlobStub stub((CodeBlob*) addr);
  return codeBlob2objectArray(thread, env, &stub);
WB_END

WB_ENTRY(jlong, WB_GetMethodData(JNIEnv* env, jobject wv, jobject method))
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, 0);
  methodHandle mh(thread, Method::checked_resolve_jmethod_id(jmid));
  return (jlong) mh->method_data();
WB_END

WB_ENTRY(jlong, WB_GetThreadStackSize(JNIEnv* env, jobject o))
  return (jlong) Thread::current()->stack_size();
WB_END

WB_ENTRY(jlong, WB_GetThreadRemainingStackSize(JNIEnv* env, jobject o))
  JavaThread* t = JavaThread::current();
  return (jlong) t->stack_available(os::current_stack_pointer()) - (jlong)JavaThread::stack_shadow_zone_size();
WB_END


int WhiteBox::array_bytes_to_length(size_t bytes) {
  return Array<u1>::bytes_to_length(bytes);
}

WB_ENTRY(jlong, WB_AllocateMetaspace(JNIEnv* env, jobject wb, jobject class_loader, jlong size))
  if (size < 0) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
        err_msg("WB_AllocateMetaspace: size is negative: " JLONG_FORMAT, size));
  }

  oop class_loader_oop = JNIHandles::resolve(class_loader);
  ClassLoaderData* cld = class_loader_oop != NULL
      ? java_lang_ClassLoader::loader_data_acquire(class_loader_oop)
      : ClassLoaderData::the_null_class_loader_data();

  void* metadata = MetadataFactory::new_array<u1>(cld, WhiteBox::array_bytes_to_length((size_t)size), thread);

  return (jlong)(uintptr_t)metadata;
WB_END

WB_ENTRY(void, WB_FreeMetaspace(JNIEnv* env, jobject wb, jobject class_loader, jlong addr, jlong size))
  oop class_loader_oop = JNIHandles::resolve(class_loader);
  ClassLoaderData* cld = class_loader_oop != NULL
      ? java_lang_ClassLoader::loader_data_acquire(class_loader_oop)
      : ClassLoaderData::the_null_class_loader_data();

  MetadataFactory::free_array(cld, (Array<u1>*)(uintptr_t)addr);
WB_END

WB_ENTRY(void, WB_DefineModule(JNIEnv* env, jobject o, jobject module, jboolean is_open,
                                jstring version, jstring location, jobjectArray packages))
  ResourceMark rm(THREAD);

  objArrayOop packages_oop = objArrayOop(JNIHandles::resolve(packages));
  objArrayHandle packages_h(THREAD, packages_oop);
  int num_packages = (packages_h == NULL ? 0 : packages_h->length());

  char** pkgs = NULL;
  if (num_packages > 0) {
    pkgs = NEW_RESOURCE_ARRAY_IN_THREAD(THREAD, char*, num_packages);
    for (int x = 0; x < num_packages; x++) {
      oop pkg_str = packages_h->obj_at(x);
      if (pkg_str == NULL || !pkg_str->is_a(SystemDictionary::String_klass())) {
        THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
                  err_msg("Bad package name"));
      }
      pkgs[x] = java_lang_String::as_utf8_string(pkg_str);
    }
  }
  Modules::define_module(module, is_open, version, location, (const char* const*)pkgs, num_packages, CHECK);
WB_END

WB_ENTRY(void, WB_AddModuleExports(JNIEnv* env, jobject o, jobject from_module, jstring package, jobject to_module))
  ResourceMark rm(THREAD);
  char* package_name = NULL;
  if (package != NULL) {
      package_name = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(package));
  }
  Modules::add_module_exports_qualified(from_module, package_name, to_module, CHECK);
WB_END

WB_ENTRY(void, WB_AddModuleExportsToAllUnnamed(JNIEnv* env, jobject o, jclass module, jstring package))
  ResourceMark rm(THREAD);
  char* package_name = NULL;
  if (package != NULL) {
      package_name = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(package));
  }
  Modules::add_module_exports_to_all_unnamed(module, package_name, CHECK);
WB_END

WB_ENTRY(void, WB_AddModuleExportsToAll(JNIEnv* env, jobject o, jclass module, jstring package))
  ResourceMark rm(THREAD);
  char* package_name = NULL;
  if (package != NULL) {
      package_name = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(package));
  }
  Modules::add_module_exports(module, package_name, NULL, CHECK);
WB_END

WB_ENTRY(void, WB_AddReadsModule(JNIEnv* env, jobject o, jobject from_module, jobject source_module))
  Modules::add_reads_module(from_module, source_module, CHECK);
WB_END

WB_ENTRY(jlong, WB_IncMetaspaceCapacityUntilGC(JNIEnv* env, jobject wb, jlong inc))
  if (inc < 0) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
        err_msg("WB_IncMetaspaceCapacityUntilGC: inc is negative: " JLONG_FORMAT, inc));
  }

  jlong max_size_t = (jlong) ((size_t) -1);
  if (inc > max_size_t) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
        err_msg("WB_IncMetaspaceCapacityUntilGC: inc does not fit in size_t: " JLONG_FORMAT, inc));
  }

  size_t new_cap_until_GC = 0;
  size_t aligned_inc = align_down((size_t) inc, Metaspace::commit_alignment());
  bool success = MetaspaceGC::inc_capacity_until_GC(aligned_inc, &new_cap_until_GC);
  if (!success) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalStateException(),
                "WB_IncMetaspaceCapacityUntilGC: could not increase capacity until GC "
                "due to contention with another thread");
  }
  return (jlong) new_cap_until_GC;
WB_END

WB_ENTRY(jlong, WB_MetaspaceCapacityUntilGC(JNIEnv* env, jobject wb))
  return (jlong) MetaspaceGC::capacity_until_GC();
WB_END

WB_ENTRY(jboolean, WB_MetaspaceShouldConcurrentCollect(JNIEnv* env, jobject wb))
  return MetaspaceGC::should_concurrent_collect();
WB_END

WB_ENTRY(jlong, WB_MetaspaceReserveAlignment(JNIEnv* env, jobject wb))
  return (jlong)Metaspace::reserve_alignment();
WB_END

WB_ENTRY(void, WB_AssertMatchingSafepointCalls(JNIEnv* env, jobject o, jboolean mutexSafepointValue, jboolean attemptedNoSafepointValue))
  Monitor::SafepointCheckRequired sfpt_check_required = mutexSafepointValue ?
                                           Monitor::_safepoint_check_always :
                                           Monitor::_safepoint_check_never;
  MutexLockerEx ml(new Mutex(Mutex::leaf, "SFPT_Test_lock", true, sfpt_check_required),
                   attemptedNoSafepointValue == JNI_TRUE);
WB_END

WB_ENTRY(jboolean, WB_IsMonitorInflated(JNIEnv* env, jobject wb, jobject obj))
  oop obj_oop = JNIHandles::resolve(obj);
  return (jboolean) obj_oop->mark()->has_monitor();
WB_END

WB_ENTRY(void, WB_ForceSafepoint(JNIEnv* env, jobject wb))
  VM_ForceSafepoint force_safepoint_op;
  VMThread::execute(&force_safepoint_op);
WB_END

WB_ENTRY(jlong, WB_GetConstantPool(JNIEnv* env, jobject wb, jclass klass))
  InstanceKlass* ik = InstanceKlass::cast(java_lang_Class::as_Klass(JNIHandles::resolve(klass)));
  return (jlong) ik->constants();
WB_END

WB_ENTRY(jint, WB_GetConstantPoolCacheIndexTag(JNIEnv* env, jobject wb))
  return ConstantPool::CPCACHE_INDEX_TAG;
WB_END

WB_ENTRY(jint, WB_GetConstantPoolCacheLength(JNIEnv* env, jobject wb, jclass klass))
  InstanceKlass* ik = InstanceKlass::cast(java_lang_Class::as_Klass(JNIHandles::resolve(klass)));
  ConstantPool* cp = ik->constants();
  if (cp->cache() == NULL) {
      return -1;
  }
  return cp->cache()->length();
WB_END

WB_ENTRY(jint, WB_ConstantPoolRemapInstructionOperandFromCache(JNIEnv* env, jobject wb, jclass klass, jint index))
  InstanceKlass* ik = InstanceKlass::cast(java_lang_Class::as_Klass(JNIHandles::resolve(klass)));
  ConstantPool* cp = ik->constants();
  if (cp->cache() == NULL) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalStateException(), "Constant pool does not have a cache");
  }
  jint cpci = index;
  jint cpciTag = ConstantPool::CPCACHE_INDEX_TAG;
  if (cpciTag > cpci || cpci >= cp->cache()->length() + cpciTag) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(), "Constant pool cache index is out of range");
  }
  jint cpi = cp->remap_instruction_operand_from_cache(cpci);
  return cpi;
WB_END

WB_ENTRY(jint, WB_ConstantPoolEncodeIndyIndex(JNIEnv* env, jobject wb, jint index))
  return ConstantPool::encode_invokedynamic_index(index);
WB_END

WB_ENTRY(void, WB_ClearInlineCaches(JNIEnv* env, jobject wb, jboolean preserve_static_stubs))
  VM_ClearICs clear_ics(preserve_static_stubs == JNI_TRUE);
  VMThread::execute(&clear_ics);
WB_END

template <typename T>
static bool GetMethodOption(JavaThread* thread, JNIEnv* env, jobject method, jstring name, T* value) {
  assert(value != NULL, "sanity");
  if (method == NULL || name == NULL) {
    return false;
  }
  jmethodID jmid = reflected_method_to_jmid(thread, env, method);
  CHECK_JNI_EXCEPTION_(env, false);
  methodHandle mh(thread, Method::checked_resolve_jmethod_id(jmid));
  // can't be in VM when we call JNI
  ThreadToNativeFromVM ttnfv(thread);
  const char* flag_name = env->GetStringUTFChars(name, NULL);
  CHECK_JNI_EXCEPTION_(env, false);
  bool result =  CompilerOracle::has_option_value(mh, flag_name, *value);
  env->ReleaseStringUTFChars(name, flag_name);
  return result;
}

WB_ENTRY(jobject, WB_GetMethodBooleaneOption(JNIEnv* env, jobject wb, jobject method, jstring name))
  bool result;
  if (GetMethodOption<bool> (thread, env, method, name, &result)) {
    // can't be in VM when we call JNI
    ThreadToNativeFromVM ttnfv(thread);
    return booleanBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetMethodIntxOption(JNIEnv* env, jobject wb, jobject method, jstring name))
  intx result;
  if (GetMethodOption <intx> (thread, env, method, name, &result)) {
    // can't be in VM when we call JNI
    ThreadToNativeFromVM ttnfv(thread);
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetMethodUintxOption(JNIEnv* env, jobject wb, jobject method, jstring name))
  uintx result;
  if (GetMethodOption <uintx> (thread, env, method, name, &result)) {
    // can't be in VM when we call JNI
    ThreadToNativeFromVM ttnfv(thread);
    return longBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetMethodDoubleOption(JNIEnv* env, jobject wb, jobject method, jstring name))
  double result;
  if (GetMethodOption <double> (thread, env, method, name, &result)) {
    // can't be in VM when we call JNI
    ThreadToNativeFromVM ttnfv(thread);
    return doubleBox(thread, env, result);
  }
  return NULL;
WB_END

WB_ENTRY(jobject, WB_GetMethodStringOption(JNIEnv* env, jobject wb, jobject method, jstring name))
  ccstr ccstrResult;
  if (GetMethodOption <ccstr> (thread, env, method, name, &ccstrResult)) {
    // can't be in VM when we call JNI
    ThreadToNativeFromVM ttnfv(thread);
    jstring result = env->NewStringUTF(ccstrResult);
    CHECK_JNI_EXCEPTION_(env, NULL);
    return result;
  }
  return NULL;
WB_END

WB_ENTRY(jboolean, WB_IsShared(JNIEnv* env, jobject wb, jobject obj))
  oop obj_oop = JNIHandles::resolve(obj);
  return MetaspaceShared::is_archive_object(obj_oop);
WB_END

WB_ENTRY(jboolean, WB_IsSharedClass(JNIEnv* env, jobject wb, jclass clazz))
  return (jboolean)MetaspaceShared::is_in_shared_metaspace(java_lang_Class::as_Klass(JNIHandles::resolve_non_null(clazz)));
WB_END

WB_ENTRY(jboolean, WB_AreSharedStringsIgnored(JNIEnv* env))
  return !StringTable::shared_string_mapped();
WB_END

WB_ENTRY(jobject, WB_GetResolvedReferences(JNIEnv* env, jobject wb, jclass clazz))
  Klass *k = java_lang_Class::as_Klass(JNIHandles::resolve_non_null(clazz));
  if (k->is_instance_klass()) {
    InstanceKlass *ik = InstanceKlass::cast(k);
    ConstantPool *cp = ik->constants();
    objArrayOop refs =  cp->resolved_references();
    return (jobject)JNIHandles::make_local(env, refs);
  } else {
    return NULL;
  }
WB_END

WB_ENTRY(jboolean, WB_AreOpenArchiveHeapObjectsMapped(JNIEnv* env))
  return MetaspaceShared::open_archive_heap_region_mapped();
WB_END

WB_ENTRY(jboolean, WB_IsCDSIncluded(JNIEnv* env))
#if INCLUDE_CDS
# ifdef _LP64
    if (!UseCompressedOops || !UseCompressedClassPointers) {
      // On 64-bit VMs, CDS is supported only with compressed oops/pointers
      return false;
    }
# endif // _LP64
  return true;
#else
  return false;
#endif // INCLUDE_CDS
WB_END

WB_ENTRY(jboolean, WB_IsJavaHeapArchiveSupported(JNIEnv* env))
  return MetaspaceShared::is_heap_object_archiving_allowed();
WB_END


WB_ENTRY(jboolean, WB_IsJFRIncluded(JNIEnv* env))
#if INCLUDE_JFR
  return true;
#else
  return false;
#endif // INCLUDE_JFR
WB_END

#if INCLUDE_CDS

WB_ENTRY(jint, WB_GetOffsetForName(JNIEnv* env, jobject o, jstring name))
  ResourceMark rm;
  char* c_name = java_lang_String::as_utf8_string(JNIHandles::resolve_non_null(name));
  int result = CDSOffsets::find_offset(c_name);
  return (jint)result;
WB_END

#endif // INCLUDE_CDS

WB_ENTRY(jint, WB_HandshakeWalkStack(JNIEnv* env, jobject wb, jobject thread_handle, jboolean all_threads))
  class TraceSelfClosure : public HandshakeClosure {
    jint _num_threads_completed;

    void do_thread(Thread* th) {
      assert(th->is_Java_thread(), "sanity");
      JavaThread* jt = (JavaThread*)th;
      ResourceMark rm;

      jt->print_on(tty);
      jt->print_stack_on(tty);
      tty->cr();
      Atomic::inc(&_num_threads_completed);
    }

  public:
    TraceSelfClosure() : HandshakeClosure("WB_TraceSelf"), _num_threads_completed(0) {}

    jint num_threads_completed() const { return _num_threads_completed; }
  };
  TraceSelfClosure tsc;

  if (all_threads) {
    Handshake::execute(&tsc);
  } else {
    oop thread_oop = JNIHandles::resolve(thread_handle);
    if (thread_oop != NULL) {
      JavaThread* target = java_lang_Thread::thread(thread_oop);
      Handshake::execute(&tsc, target);
    }
  }
  return tsc.num_threads_completed();
WB_END

//Some convenience methods to deal with objects from java
int WhiteBox::offset_for_field(const char* field_name, oop object,
    Symbol* signature_symbol) {
  assert(field_name != NULL && strlen(field_name) > 0, "Field name not valid");
  Thread* THREAD = Thread::current();

  //Get the class of our object
  Klass* arg_klass = object->klass();
  //Turn it into an instance-klass
  InstanceKlass* ik = InstanceKlass::cast(arg_klass);

  //Create symbols to look for in the class
  TempNewSymbol name_symbol = SymbolTable::lookup(field_name, (int) strlen(field_name),
      THREAD);

  //To be filled in with an offset of the field we're looking for
  fieldDescriptor fd;

  Klass* res = ik->find_field(name_symbol, signature_symbol, &fd);
  if (res == NULL) {
    tty->print_cr("Invalid layout of %s at %s", ik->external_name(),
        name_symbol->as_C_string());
    vm_exit_during_initialization("Invalid layout of preloaded class: use -Xlog:class+load=info to see the origin of the problem class");
  }

  //fetch the field at the offset we've found
  int dest_offset = fd.offset();

  return dest_offset;
}


const char* WhiteBox::lookup_jstring(const char* field_name, oop object) {
  int offset = offset_for_field(field_name, object,
      vmSymbols::string_signature());
  oop string = object->obj_field(offset);
  if (string == NULL) {
    return NULL;
  }
  const char* ret = java_lang_String::as_utf8_string(string);
  return ret;
}

bool WhiteBox::lookup_bool(const char* field_name, oop object) {
  int offset =
      offset_for_field(field_name, object, vmSymbols::bool_signature());
  bool ret = (object->bool_field(offset) == JNI_TRUE);
  return ret;
}

void WhiteBox::register_methods(JNIEnv* env, jclass wbclass, JavaThread* thread, JNINativeMethod* method_array, int method_count) {
  ResourceMark rm;
  ThreadToNativeFromVM ttnfv(thread); // can't be in VM when we call JNI

  //  one by one registration natives for exception catching
  jclass no_such_method_error_klass = env->FindClass(vmSymbols::java_lang_NoSuchMethodError()->as_C_string());
  CHECK_JNI_EXCEPTION(env);
  for (int i = 0, n = method_count; i < n; ++i) {
    // Skip dummy entries
    if (method_array[i].fnPtr == NULL) continue;
    if (env->RegisterNatives(wbclass, &method_array[i], 1) != 0) {
      jthrowable throwable_obj = env->ExceptionOccurred();
      if (throwable_obj != NULL) {
        env->ExceptionClear();
        if (env->IsInstanceOf(throwable_obj, no_such_method_error_klass)) {
          // NoSuchMethodError is thrown when a method can't be found or a method is not native.
          // Ignoring the exception since it is not preventing use of other WhiteBox methods.
          tty->print_cr("Warning: 'NoSuchMethodError' on register of sun.hotspot.WhiteBox::%s%s",
              method_array[i].name, method_array[i].signature);
        }
      } else {
        // Registration failed unexpectedly.
        tty->print_cr("Warning: unexpected error on register of sun.hotspot.WhiteBox::%s%s. All methods will be unregistered",
            method_array[i].name, method_array[i].signature);
        env->UnregisterNatives(wbclass);
        break;
      }
    }
  }
}

WB_ENTRY(jint, WB_AddCompilerDirective(JNIEnv* env, jobject o, jstring compDirect))
  // can't be in VM when we call JNI
  ThreadToNativeFromVM ttnfv(thread);
  const char* dir = env->GetStringUTFChars(compDirect, NULL);
  CHECK_JNI_EXCEPTION_(env, 0);
  int ret;
  {
    ThreadInVMfromNative ttvfn(thread); // back to VM
    ret = DirectivesParser::parse_string(dir, tty);
  }
  env->ReleaseStringUTFChars(compDirect, dir);
  // -1 for error parsing directive. Return 0 as number of directives added.
  if (ret == -1) {
    ret = 0;
  }
  return (jint) ret;
WB_END

WB_ENTRY(void, WB_RemoveCompilerDirective(JNIEnv* env, jobject o, jint count))
  DirectivesStack::pop(count);
WB_END

// Checks that the library libfile has the noexecstack bit set.
WB_ENTRY(jboolean, WB_CheckLibSpecifiesNoexecstack(JNIEnv* env, jobject o, jstring libfile))
  jboolean ret = false;
#ifdef LINUX
  // Can't be in VM when we call JNI.
  ThreadToNativeFromVM ttnfv(thread);
  const char* lf = env->GetStringUTFChars(libfile, NULL);
  CHECK_JNI_EXCEPTION_(env, 0);
  ret = (jboolean) ElfFile::specifies_noexecstack(lf);
  env->ReleaseStringUTFChars(libfile, lf);
#endif
  return ret;
WB_END

WB_ENTRY(jboolean, WB_IsContainerized(JNIEnv* env, jobject o))
  LINUX_ONLY(return OSContainer::is_containerized();)
  return false;
WB_END

// Physical memory of the host machine (including containers)
WB_ENTRY(jlong, WB_HostPhysicalMemory(JNIEnv* env, jobject o))
  LINUX_ONLY(return os::Linux::physical_memory();)
  return os::physical_memory();
WB_END

// Physical swap of the host machine (including containers), Linux only.
WB_ENTRY(jlong, WB_HostPhysicalSwap(JNIEnv* env, jobject o))
  LINUX_ONLY(return (jlong)os::Linux::host_swap();)
  return -1; // Not used/implemented on other platforms
WB_END

WB_ENTRY(jint, WB_ValidateCgroup(JNIEnv* env,
                                    jobject o,
                                    jstring proc_cgroups,
                                    jstring proc_self_cgroup,
                                    jstring proc_self_mountinfo))
  jint ret = 0;
#ifdef LINUX
  ThreadToNativeFromVM ttnfv(thread);
  const char* p_cgroups = env->GetStringUTFChars(proc_cgroups, NULL);
  CHECK_JNI_EXCEPTION_(env, 0);
  const char* p_s_cgroup = env->GetStringUTFChars(proc_self_cgroup, NULL);
  CHECK_JNI_EXCEPTION_(env, 0);
  const char* p_s_mountinfo = env->GetStringUTFChars(proc_self_mountinfo, NULL);
  CHECK_JNI_EXCEPTION_(env, 0);
  u1 cg_type_flags = 0;
  // This sets cg_type_flags
  WhiteBox::validate_cgroup(p_cgroups, p_s_cgroup, p_s_mountinfo, &cg_type_flags);
  ret = (jint)cg_type_flags;
  env->ReleaseStringUTFChars(proc_cgroups, p_cgroups);
  env->ReleaseStringUTFChars(proc_self_cgroup, p_s_cgroup);
  env->ReleaseStringUTFChars(proc_self_mountinfo, p_s_mountinfo);
#endif
  return ret;
WB_END

WB_ENTRY(void, WB_PrintOsInfo(JNIEnv* env, jobject o))
  os::print_os_info(tty);
WB_END

// Elf decoder
WB_ENTRY(void, WB_DisableElfSectionCache(JNIEnv* env))
#if !defined(_WINDOWS) && !defined(__APPLE__) && !defined(_AIX)
  ElfFile::_do_not_cache_elf_section = true;
#endif
WB_END

WB_ENTRY(jint, WB_AotLibrariesCount(JNIEnv* env, jobject o))
  jint result = 0;
#if INCLUDE_AOT
  result = (jint) AOTLoader::heaps_count();
#endif
  return result;
WB_END

WB_ENTRY(jstring, WB_GetLibcName(JNIEnv* env, jobject o))
  ThreadToNativeFromVM ttn(thread);
  jstring info_string = env->NewStringUTF(XSTR(LIBC));
  CHECK_JNI_EXCEPTION_(env, NULL);
  return info_string;
WB_END

class VM_WhiteBoxCleanMetaspaces : public VM_WhiteBoxOperation {
 public:
  void doit() {
    ClassLoaderDataGraph::do_unloading(true);
  }
};

WB_ENTRY(void, WB_CleanMetaspaces(JNIEnv* env, jobject target))
  VM_WhiteBoxCleanMetaspaces op;
  VMThread::execute(&op);
WB_END

#define CC (char*)

static JNINativeMethod methods[] = {
  {CC"getObjectAddress0",                CC"(Ljava/lang/Object;)J", (void*)&WB_GetObjectAddress  },
  {CC"getObjectSize0",                   CC"(Ljava/lang/Object;)J", (void*)&WB_GetObjectSize     },
  {CC"isObjectInOldGen0",                CC"(Ljava/lang/Object;)Z", (void*)&WB_isObjectInOldGen  },
  {CC"getHeapOopSize",                   CC"()I",                   (void*)&WB_GetHeapOopSize    },
  {CC"getVMPageSize",                    CC"()I",                   (void*)&WB_GetVMPageSize     },
  {CC"getVMAllocationGranularity",       CC"()J",                   (void*)&WB_GetVMAllocationGranularity },
  {CC"getVMLargePageSize",               CC"()J",                   (void*)&WB_GetVMLargePageSize},
  {CC"getHeapSpaceAlignment",            CC"()J",                   (void*)&WB_GetHeapSpaceAlignment},
  {CC"getHeapAlignment",                 CC"()J",                   (void*)&WB_GetHeapAlignment},
  {CC"isClassAlive0",                    CC"(Ljava/lang/String;)Z", (void*)&WB_IsClassAlive      },
  {CC"getSymbolRefcount",                CC"(Ljava/lang/String;)I", (void*)&WB_GetSymbolRefcount },
  {CC"parseCommandLine0",
      CC"(Ljava/lang/String;C[Lsun/hotspot/parser/DiagnosticCommand;)[Ljava/lang/Object;",
      (void*) &WB_ParseCommandLine
  },
  {CC"addToBootstrapClassLoaderSearch0", CC"(Ljava/lang/String;)V",
                                                      (void*)&WB_AddToBootstrapClassLoaderSearch},
  {CC"addToSystemClassLoaderSearch0",    CC"(Ljava/lang/String;)V",
                                                      (void*)&WB_AddToSystemClassLoaderSearch},
  {CC"getCompressedOopsMaxHeapSize", CC"()J",
      (void*)&WB_GetCompressedOopsMaxHeapSize},
  {CC"printHeapSizes",     CC"()V",                   (void*)&WB_PrintHeapSizes    },
  {CC"runMemoryUnitTests", CC"()V",                   (void*)&WB_RunMemoryUnitTests},
  {CC"readFromNoaccessArea",CC"()V",                  (void*)&WB_ReadFromNoaccessArea},
  {CC"stressVirtualSpaceResize",CC"(JJJ)I",           (void*)&WB_StressVirtualSpaceResize},
#if INCLUDE_CDS
  {CC"getOffsetForName0", CC"(Ljava/lang/String;)I",  (void*)&WB_GetOffsetForName},
#endif
#if INCLUDE_G1GC
  {CC"g1InConcurrentMark", CC"()Z",                   (void*)&WB_G1InConcurrentMark},
  {CC"g1IsHumongous0",      CC"(Ljava/lang/Object;)Z", (void*)&WB_G1IsHumongous     },
  {CC"g1BelongsToHumongousRegion0", CC"(J)Z",         (void*)&WB_G1BelongsToHumongousRegion},
  {CC"g1BelongsToFreeRegion0", CC"(J)Z",              (void*)&WB_G1BelongsToFreeRegion},
  {CC"g1NumMaxRegions",    CC"()J",                   (void*)&WB_G1NumMaxRegions  },
  {CC"g1NumFreeRegions",   CC"()J",                   (void*)&WB_G1NumFreeRegions  },
  {CC"g1RegionSize",       CC"()I",                   (void*)&WB_G1RegionSize      },
  {CC"g1StartConcMarkCycle",       CC"()Z",           (void*)&WB_G1StartMarkCycle  },
  {CC"g1AuxiliaryMemoryUsage", CC"()Ljava/lang/management/MemoryUsage;",
                                                      (void*)&WB_G1AuxiliaryMemoryUsage  },
  {CC"g1ActiveMemoryNodeCount", CC"()I",              (void*)&WB_G1ActiveMemoryNodeCount },
  {CC"g1MemoryNodeIds",    CC"()[I",                  (void*)&WB_G1MemoryNodeIds },
  {CC"g1GetMixedGCInfo",   CC"(I)[J",                 (void*)&WB_G1GetMixedGCInfo },
#endif // INCLUDE_G1GC
#if INCLUDE_PARALLELGC
  {CC"psVirtualSpaceAlignment",CC"()J",               (void*)&WB_PSVirtualSpaceAlignment},
  {CC"psHeapGenerationAlignment",CC"()J",             (void*)&WB_PSHeapGenerationAlignment},
#endif
#if INCLUDE_NMT
  {CC"NMTMalloc",           CC"(J)J",                 (void*)&WB_NMTMalloc          },
  {CC"NMTMallocWithPseudoStack", CC"(JI)J",           (void*)&WB_NMTMallocWithPseudoStack},
  {CC"NMTMallocWithPseudoStackAndType", CC"(JII)J",   (void*)&WB_NMTMallocWithPseudoStackAndType},
  {CC"NMTFree",             CC"(J)V",                 (void*)&WB_NMTFree            },
  {CC"NMTReserveMemory",    CC"(J)J",                 (void*)&WB_NMTReserveMemory   },
  {CC"NMTAttemptReserveMemoryAt",    CC"(JJ)J",       (void*)&WB_NMTAttemptReserveMemoryAt },
  {CC"NMTCommitMemory",     CC"(JJ)V",                (void*)&WB_NMTCommitMemory    },
  {CC"NMTUncommitMemory",   CC"(JJ)V",                (void*)&WB_NMTUncommitMemory  },
  {CC"NMTReleaseMemory",    CC"(JJ)V",                (void*)&WB_NMTReleaseMemory   },
  {CC"NMTChangeTrackingLevel", CC"()Z",               (void*)&WB_NMTChangeTrackingLevel},
  {CC"NMTGetHashSize",      CC"()I",                  (void*)&WB_NMTGetHashSize     },
  {CC"NMTNewArena",         CC"(J)J",                 (void*)&WB_NMTNewArena        },
  {CC"NMTFreeArena",        CC"(J)V",                 (void*)&WB_NMTFreeArena       },
  {CC"NMTArenaMalloc",      CC"(JJ)V",                (void*)&WB_NMTArenaMalloc     },
#endif // INCLUDE_NMT
  {CC"deoptimizeFrames",   CC"(Z)I",                  (void*)&WB_DeoptimizeFrames  },
  {CC"deoptimizeAll",      CC"()V",                   (void*)&WB_DeoptimizeAll     },
  {CC"deoptimizeMethod0",   CC"(Ljava/lang/reflect/Executable;Z)I",
                                                      (void*)&WB_DeoptimizeMethod  },
  {CC"isMethodCompiled0",   CC"(Ljava/lang/reflect/Executable;Z)Z",
                                                      (void*)&WB_IsMethodCompiled  },
  {CC"isMethodCompilable0", CC"(Ljava/lang/reflect/Executable;IZ)Z",
                                                      (void*)&WB_IsMethodCompilable},
  {CC"isMethodQueuedForCompilation0",
      CC"(Ljava/lang/reflect/Executable;)Z",          (void*)&WB_IsMethodQueuedForCompilation},
  {CC"isIntrinsicAvailable0",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/reflect/Executable;I)Z",
                                                      (void*)&WB_IsIntrinsicAvailable},
  {CC"makeMethodNotCompilable0",
      CC"(Ljava/lang/reflect/Executable;IZ)V",        (void*)&WB_MakeMethodNotCompilable},
  {CC"testSetDontInlineMethod0",
      CC"(Ljava/lang/reflect/Executable;Z)Z",         (void*)&WB_TestSetDontInlineMethod},
  {CC"getMethodCompilationLevel0",
      CC"(Ljava/lang/reflect/Executable;Z)I",         (void*)&WB_GetMethodCompilationLevel},
  {CC"getMethodEntryBci0",
      CC"(Ljava/lang/reflect/Executable;)I",          (void*)&WB_GetMethodEntryBci},
  {CC"getCompileQueueSize",
      CC"(I)I",                                       (void*)&WB_GetCompileQueueSize},
  {CC"testSetForceInlineMethod0",
      CC"(Ljava/lang/reflect/Executable;Z)Z",         (void*)&WB_TestSetForceInlineMethod},
  {CC"enqueueMethodForCompilation0",
      CC"(Ljava/lang/reflect/Executable;II)Z",        (void*)&WB_EnqueueMethodForCompilation},
  {CC"enqueueInitializerForCompilation0",
      CC"(Ljava/lang/Class;I)Z",                      (void*)&WB_EnqueueInitializerForCompilation},
  {CC"markMethodProfiled",
      CC"(Ljava/lang/reflect/Executable;)V",          (void*)&WB_MarkMethodProfiled},
  {CC"clearMethodState0",
      CC"(Ljava/lang/reflect/Executable;)V",          (void*)&WB_ClearMethodState},
  {CC"lockCompilation",    CC"()V",                   (void*)&WB_LockCompilation},
  {CC"unlockCompilation",  CC"()V",                   (void*)&WB_UnlockCompilation},
  {CC"matchesMethod",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)I",
                                                      (void*)&WB_MatchesMethod},
  {CC"matchesInline",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)I",
                                                      (void*)&WB_MatchesInline},
  {CC"shouldPrintAssembly",
        CC"(Ljava/lang/reflect/Executable;I)Z",
                                                        (void*)&WB_ShouldPrintAssembly},

  {CC"isConstantVMFlag",   CC"(Ljava/lang/String;)Z", (void*)&WB_IsConstantVMFlag},
  {CC"isLockedVMFlag",     CC"(Ljava/lang/String;)Z", (void*)&WB_IsLockedVMFlag},
  {CC"setBooleanVMFlag",   CC"(Ljava/lang/String;Z)V",(void*)&WB_SetBooleanVMFlag},
  {CC"setIntVMFlag",       CC"(Ljava/lang/String;J)V",(void*)&WB_SetIntVMFlag},
  {CC"setUintVMFlag",      CC"(Ljava/lang/String;J)V",(void*)&WB_SetUintVMFlag},
  {CC"setIntxVMFlag",      CC"(Ljava/lang/String;J)V",(void*)&WB_SetIntxVMFlag},
  {CC"setUintxVMFlag",     CC"(Ljava/lang/String;J)V",(void*)&WB_SetUintxVMFlag},
  {CC"setUint64VMFlag",    CC"(Ljava/lang/String;J)V",(void*)&WB_SetUint64VMFlag},
  {CC"setSizeTVMFlag",     CC"(Ljava/lang/String;J)V",(void*)&WB_SetSizeTVMFlag},
  {CC"setDoubleVMFlag",    CC"(Ljava/lang/String;D)V",(void*)&WB_SetDoubleVMFlag},
  {CC"setStringVMFlag",    CC"(Ljava/lang/String;Ljava/lang/String;)V",
                                                      (void*)&WB_SetStringVMFlag},
  {CC"getBooleanVMFlag",   CC"(Ljava/lang/String;)Ljava/lang/Boolean;",
                                                      (void*)&WB_GetBooleanVMFlag},
  {CC"getIntVMFlag",       CC"(Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetIntVMFlag},
  {CC"getUintVMFlag",      CC"(Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetUintVMFlag},
  {CC"getIntxVMFlag",      CC"(Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetIntxVMFlag},
  {CC"getUintxVMFlag",     CC"(Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetUintxVMFlag},
  {CC"getUint64VMFlag",    CC"(Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetUint64VMFlag},
  {CC"getSizeTVMFlag",     CC"(Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetSizeTVMFlag},
  {CC"getDoubleVMFlag",    CC"(Ljava/lang/String;)Ljava/lang/Double;",
                                                      (void*)&WB_GetDoubleVMFlag},
  {CC"getStringVMFlag",    CC"(Ljava/lang/String;)Ljava/lang/String;",
                                                      (void*)&WB_GetStringVMFlag},
  {CC"isInStringTable",    CC"(Ljava/lang/String;)Z", (void*)&WB_IsInStringTable  },
  {CC"fullGC",   CC"()V",                             (void*)&WB_FullGC },
  {CC"youngGC",  CC"()V",                             (void*)&WB_YoungGC },
  {CC"readReservedMemory", CC"()V",                   (void*)&WB_ReadReservedMemory },
  {CC"allocateMetaspace",
     CC"(Ljava/lang/ClassLoader;J)J",                 (void*)&WB_AllocateMetaspace },
  {CC"freeMetaspace",
     CC"(Ljava/lang/ClassLoader;JJ)V",                (void*)&WB_FreeMetaspace },
  {CC"incMetaspaceCapacityUntilGC", CC"(J)J",         (void*)&WB_IncMetaspaceCapacityUntilGC },
  {CC"metaspaceCapacityUntilGC", CC"()J",             (void*)&WB_MetaspaceCapacityUntilGC },
  {CC"metaspaceShouldConcurrentCollect", CC"()Z",     (void*)&WB_MetaspaceShouldConcurrentCollect },
  {CC"metaspaceReserveAlignment", CC"()J",            (void*)&WB_MetaspaceReserveAlignment },
  {CC"getCPUFeatures",     CC"()Ljava/lang/String;",  (void*)&WB_GetCPUFeatures     },
  {CC"getNMethod0",         CC"(Ljava/lang/reflect/Executable;Z)[Ljava/lang/Object;",
                                                      (void*)&WB_GetNMethod         },
  {CC"forceNMethodSweep",  CC"()V",                   (void*)&WB_ForceNMethodSweep  },
  {CC"allocateCodeBlob",   CC"(II)J",                 (void*)&WB_AllocateCodeBlob   },
  {CC"freeCodeBlob",       CC"(J)V",                  (void*)&WB_FreeCodeBlob       },
  {CC"getCodeHeapEntries", CC"(I)[Ljava/lang/Object;",(void*)&WB_GetCodeHeapEntries },
  {CC"getCompilationActivityMode",
                           CC"()I",                   (void*)&WB_GetCompilationActivityMode},
  {CC"getMethodData0",     CC"(Ljava/lang/reflect/Executable;)J",
                                                      (void*)&WB_GetMethodData      },
  {CC"getCodeBlob",        CC"(J)[Ljava/lang/Object;",(void*)&WB_GetCodeBlob        },
  {CC"getThreadStackSize", CC"()J",                   (void*)&WB_GetThreadStackSize },
  {CC"getThreadRemainingStackSize", CC"()J",          (void*)&WB_GetThreadRemainingStackSize },
  {CC"DefineModule",       CC"(Ljava/lang/Object;ZLjava/lang/String;Ljava/lang/String;[Ljava/lang/Object;)V",
                                                      (void*)&WB_DefineModule },
  {CC"AddModuleExports",   CC"(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)V",
                                                      (void*)&WB_AddModuleExports },
  {CC"AddReadsModule",     CC"(Ljava/lang/Object;Ljava/lang/Object;)V",
                                                      (void*)&WB_AddReadsModule },
  {CC"AddModuleExportsToAllUnnamed", CC"(Ljava/lang/Object;Ljava/lang/String;)V",
                                                      (void*)&WB_AddModuleExportsToAllUnnamed },
  {CC"AddModuleExportsToAll", CC"(Ljava/lang/Object;Ljava/lang/String;)V",
                                                      (void*)&WB_AddModuleExportsToAll },
  {CC"assertMatchingSafepointCalls", CC"(ZZ)V",       (void*)&WB_AssertMatchingSafepointCalls },
  {CC"isMonitorInflated0", CC"(Ljava/lang/Object;)Z", (void*)&WB_IsMonitorInflated  },
  {CC"forceSafepoint",     CC"()V",                   (void*)&WB_ForceSafepoint     },
  {CC"getConstantPool0",   CC"(Ljava/lang/Class;)J",  (void*)&WB_GetConstantPool    },
  {CC"getConstantPoolCacheIndexTag0", CC"()I",  (void*)&WB_GetConstantPoolCacheIndexTag},
  {CC"getConstantPoolCacheLength0", CC"(Ljava/lang/Class;)I",  (void*)&WB_GetConstantPoolCacheLength},
  {CC"remapInstructionOperandFromCPCache0",
      CC"(Ljava/lang/Class;I)I",                      (void*)&WB_ConstantPoolRemapInstructionOperandFromCache},
  {CC"encodeConstantPoolIndyIndex0",
      CC"(I)I",                      (void*)&WB_ConstantPoolEncodeIndyIndex},
  {CC"getMethodBooleanOption",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)Ljava/lang/Boolean;",
                                                      (void*)&WB_GetMethodBooleaneOption},
  {CC"getMethodIntxOption",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetMethodIntxOption},
  {CC"getMethodUintxOption",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)Ljava/lang/Long;",
                                                      (void*)&WB_GetMethodUintxOption},
  {CC"getMethodDoubleOption",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)Ljava/lang/Double;",
                                                      (void*)&WB_GetMethodDoubleOption},
  {CC"getMethodStringOption",
      CC"(Ljava/lang/reflect/Executable;Ljava/lang/String;)Ljava/lang/String;",
                                                      (void*)&WB_GetMethodStringOption},
  {CC"isShared",           CC"(Ljava/lang/Object;)Z", (void*)&WB_IsShared },
  {CC"isSharedClass",      CC"(Ljava/lang/Class;)Z",  (void*)&WB_IsSharedClass },
  {CC"areSharedStringsIgnored",           CC"()Z",    (void*)&WB_AreSharedStringsIgnored },
  {CC"getResolvedReferences", CC"(Ljava/lang/Class;)Ljava/lang/Object;", (void*)&WB_GetResolvedReferences},
  {CC"areOpenArchiveHeapObjectsMapped",   CC"()Z",    (void*)&WB_AreOpenArchiveHeapObjectsMapped},
  {CC"isCDSIncluded",                     CC"()Z",    (void*)&WB_IsCDSIncluded },
  {CC"isJFRIncluded",                     CC"()Z",    (void*)&WB_IsJFRIncluded },
  {CC"isJavaHeapArchiveSupported",      CC"()Z",      (void*)&WB_IsJavaHeapArchiveSupported },

  {CC"clearInlineCaches0",  CC"(Z)V",                 (void*)&WB_ClearInlineCaches },
  {CC"handshakeWalkStack", CC"(Ljava/lang/Thread;Z)I", (void*)&WB_HandshakeWalkStack },
  {CC"addCompilerDirective",    CC"(Ljava/lang/String;)I",
                                                      (void*)&WB_AddCompilerDirective },
  {CC"removeCompilerDirective",   CC"(I)V",           (void*)&WB_RemoveCompilerDirective },
  {CC"isGCSupported",             CC"(I)Z",           (void*)&WB_IsGCSupported},
  {CC"isGCSelected",              CC"(I)Z",           (void*)&WB_IsGCSelected},
  {CC"isGCSelectedErgonomically", CC"()Z",            (void*)&WB_IsGCSelectedErgonomically},
  {CC"supportsConcurrentGCPhaseControl", CC"()Z",     (void*)&WB_SupportsConcurrentGCPhaseControl},
  {CC"getConcurrentGCPhases",     CC"()[Ljava/lang/String;",
                                                      (void*)&WB_GetConcurrentGCPhases},
  {CC"requestConcurrentGCPhase0", CC"(Ljava/lang/String;)Z",
                                                      (void*)&WB_RequestConcurrentGCPhase},
  {CC"checkLibSpecifiesNoexecstack", CC"(Ljava/lang/String;)Z",
                                                      (void*)&WB_CheckLibSpecifiesNoexecstack},
  {CC"isContainerized",           CC"()Z",            (void*)&WB_IsContainerized },
  {CC"validateCgroup",
      CC"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
                                                      (void*)&WB_ValidateCgroup },
  {CC"hostPhysicalMemory",        CC"()J",            (void*)&WB_HostPhysicalMemory },
  {CC"hostPhysicalSwap",          CC"()J",            (void*)&WB_HostPhysicalSwap },
  {CC"printOsInfo",               CC"()V",            (void*)&WB_PrintOsInfo },
  {CC"disableElfSectionCache",    CC"()V",            (void*)&WB_DisableElfSectionCache },
  {CC"aotLibrariesCount", CC"()I",                    (void*)&WB_AotLibrariesCount },
  {CC"getLibcName",     CC"()Ljava/lang/String;",     (void*)&WB_GetLibcName},
  {CC"cleanMetaspaces", CC"()V",                      (void*)&WB_CleanMetaspaces},
};


#undef CC

JVM_ENTRY(void, JVM_RegisterWhiteBoxMethods(JNIEnv* env, jclass wbclass))
  {
    if (WhiteBoxAPI) {
      // Make sure that wbclass is loaded by the null classloader
      InstanceKlass* ik = InstanceKlass::cast(JNIHandles::resolve(wbclass)->klass());
      Handle loader(THREAD, ik->class_loader());
      if (loader.is_null()) {
        WhiteBox::register_methods(env, wbclass, thread, methods, sizeof(methods) / sizeof(methods[0]));
        WhiteBox::set_used();
      }
    }
  }
JVM_END
