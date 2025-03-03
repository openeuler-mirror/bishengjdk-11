/*
 * Copyright (c) 2020, 2024, Huawei Technologies Co., Ltd. All rights reserved.
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "classfile/javaClasses.inline.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "compiler/compileBroker.hpp"
#include "jbolt/jBoltCallGraph.hpp"
#include "jbolt/jBoltControlThread.hpp"
#include "jbolt/jBoltManager.hpp"
#include "jbolt/jBoltUtils.inline.hpp"
#include "jfr/jfr.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/os.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/sweeper.hpp"
#include "utilities/formatBuffer.hpp"

#define LINE_BUF_SIZE       8192    // used to parse JBolt order file
#define MIN_FRAMESCOUNT     2       // used as default stacktrace depth
#define ILL_NM_STATE        -2      // used to present nmethod illegal state
#define PATH_LENGTH         256     // used to store path

#define B_TF(b) (b ? "V" : "X")

GrowableArray<JBoltMethodKey>* JBoltManager::_hot_methods_sorted = NULL;
JBoltManager::MethodKeyMap* JBoltManager::_hot_methods_vis = NULL;
int JBoltManager::_reorder_method_threshold_cnt = 0;

volatile int JBoltManager::_reorder_phase = JBoltReorderPhase::Available;
volatile int JBoltManager::_reorderable_method_cnt = 0;
Method* volatile JBoltManager::_cur_reordering_method = NULL;

Thread* JBoltManager::_start_reordering_thread = NULL;

JBoltManager::StackFrameKeyMap* JBoltManager::_sampled_methods_refs = NULL;
JBoltManager::MethodHotCountMap* JBoltManager::_sampled_methods_hotcount_stored = NULL;

bool JBoltManager::_auto_mode = false;
 
// swap between MethodJBoltHot and MethodJBoltTmp
volatile int JBoltManager::_primary_hot_seg = CodeBlobType::MethodJBoltHot;
volatile int JBoltManager::_secondary_hot_seg = CodeBlobType::MethodJBoltTmp;

// used in Reordering phase, reset to ##false after swapping the hot codecache
volatile bool JBoltManager::_hot_codecache_full = false;
volatile bool JBoltManager::_force_sweep = false;

GrowableArray<char*>* JBoltManager::_rescheduling_time = NULL;
GrowableArray<JBoltFunc>* _order_stored = NULL;

// This is a tmp obj used only in initialization phases.
// We cannot alloc Symbol in phase 1 so we have to parses the order file again
// in phase 2.
// This obj will be freed after initialization.
static FILE* _order_fp = NULL;

static bool read_line(FILE* fp, char* buf, int buf_len, int* res_len) {
  if (fgets(buf, buf_len, fp) == NULL) {
    return false;
  }
  int len = (int) strcspn(buf, "\r\n");
  buf[len] = '\0';
  *res_len = len;
  return true;
}

static bool read_a_size(char* buf, size_t* res) {
  char* t = strchr(buf, ' ');
  if (t == NULL) return false;
  *t = '\0';
  julong v;
  if (!Arguments::atojulong(buf, &v)) {
    *t = ' ';
    return false;
  }
  *t = ' ';
  *res = (size_t) v;
  return true;
}

static void replace_all(char* s, char from, char to) {
  char* begin = s;
  while (true) {
    char* t = strchr(begin, from);
    if (t == NULL) {
      break;
    }
    *t = to;
    begin = t + 1;
  }
}

JBoltMethodValue::~JBoltMethodValue() {
  if (_comp_info != NULL) delete get_comp_info();
}

CompileTaskInfo* JBoltMethodValue::get_comp_info() {
  return OrderAccess::load_acquire(&_comp_info);
}

bool JBoltMethodValue::set_comp_info(CompileTaskInfo* info) {
  return Atomic::cmpxchg(info, &_comp_info, (CompileTaskInfo*) NULL) == NULL;
}

void JBoltMethodValue::clear_comp_info_but_not_release() {
  OrderAccess::release_store(&_comp_info, (CompileTaskInfo*) NULL);
}

JBoltStackFrameValue::~JBoltStackFrameValue() {
  if (_method_holder != NULL) {
    if (JNIHandles::is_weak_global_handle(_method_holder)) {
      JNIHandles::destroy_weak_global(_method_holder);
    } else {
      JNIHandles::destroy_global(_method_holder);
    }
  }
}

jobject JBoltStackFrameValue::get_method_holder() { return _method_holder; }

void JBoltStackFrameValue::clear_method_holder_but_not_release() { _method_holder = NULL; }

CompileTaskInfo::CompileTaskInfo(Method* method, int osr_bci, int comp_level, int comp_reason, Method* hot_method, int hot_cnt):
        _method(method), _osr_bci(osr_bci), _comp_level(comp_level), _comp_reason(comp_reason), _hot_method(hot_method), _hot_count(hot_cnt) {
  Thread* thread = Thread::current();

  assert(_method != NULL, "sanity");
  // _method_holder can be null for boot loader (the null loader)
  _method_holder = JNIHandles::make_weak_global(Handle(thread, _method->method_holder()->klass_holder()));

  if (_hot_method != NULL && _hot_method != _method) {
    _hot_method_holder = JNIHandles::make_weak_global(Handle(thread, _hot_method->method_holder()->klass_holder()));
  } else {
    _hot_method_holder = NULL;
  }
}

CompileTaskInfo::~CompileTaskInfo() {
  if (_method_holder != NULL) {
    if (JNIHandles::is_weak_global_handle(_method_holder)) {
      JNIHandles::destroy_weak_global(_method_holder);
    } else {
      JNIHandles::destroy_global(_method_holder);
    }
  }
  if (_hot_method_holder != NULL) {
    if (JNIHandles::is_weak_global_handle(_hot_method_holder)) {
      JNIHandles::destroy_weak_global(_hot_method_holder);
    } else {
      JNIHandles::destroy_global(_hot_method_holder);
    }
  }
}

/**
 * Set the weak reference to strong reference if the method is not unloaded.
 * It seems that the life cycle of Method is consistent with that of the Klass and CLD.
 * @see CompileTask::select_for_compilation()
 */
bool CompileTaskInfo::try_select() {
  NoSafepointVerifier nsv;
  Thread* thread = Thread::current();
  // is unloaded
  if (_method_holder != NULL && JNIHandles::is_weak_global_handle(_method_holder) && JNIHandles::is_global_weak_cleared(_method_holder)) {
    if (log_is_enabled(Debug, jbolt)) {
      log_debug(jbolt)("Some method has been unloaded so skip reordering for it: p=%p.", _method);
    }
    return false;
  }

  assert(_method->method_holder()->is_loader_alive(), "should be alive");
  Handle method_holder(thread, _method->method_holder()->klass_holder());
  JNIHandles::destroy_weak_global(_method_holder);
  _method_holder = JNIHandles::make_global(method_holder);

  if (_hot_method_holder != NULL) {
    Handle hot_method_holder(thread, _hot_method->method_holder()->klass_holder());
    JNIHandles::destroy_weak_global(_hot_method_holder);
    _hot_method_holder = JNIHandles::make_global(Handle(thread, _hot_method->method_holder()->klass_holder()));
  }
  return true;
}

static const char *method_type_to_string(u1 type) {
  switch (type) {
    case JfrStackFrame::FRAME_INTERPRETER:
      return "Interpreted";
    case JfrStackFrame::FRAME_JIT:
      return "JIT compiled";
    case JfrStackFrame::FRAME_INLINE:
      return "Inlined";
    case JfrStackFrame::FRAME_NATIVE:
      return "Native";
    default:
      ShouldNotReachHere();
      return "Unknown";
  }
}

uintptr_t related_data_jbolt_log_do[] = {
  (uintptr_t)in_bytes(JfrStackTrace::hash_offset()),
  (uintptr_t)in_bytes(JfrStackTrace::id_offset()),
  (uintptr_t)in_bytes(JfrStackTrace::hotcount_offset()),
  (uintptr_t)in_bytes(JfrStackTrace::frames_offset()),
  (uintptr_t)in_bytes(JfrStackTrace::frames_count_offset()),
 
  (uintptr_t)in_bytes(JfrStackFrame::method_offset()),
  (uintptr_t)in_bytes(JfrStackFrame::methodid_offset()),
  (uintptr_t)in_bytes(JfrStackFrame::bci_offset()),
  (uintptr_t)in_bytes(JfrStackFrame::type_offset()),
 
  (uintptr_t)JBoltFunc::constructor,
  (uintptr_t)JBoltFunc::copy_constructor,
  (uintptr_t)JBoltCall::constructor,
  (uintptr_t)JBoltCall::copy_constructor,
  (uintptr_t)JBoltCallGraph::static_add_func,
  (uintptr_t)JBoltCallGraph::static_add_call
};

/**
 * Invoked in JfrStackTraceRepository::add_jbolt().
 * Each time JFR record a valid stacktrace, 
 * we log a weak ptr of each unique method in _sampled_methods_refs. 
 */
void JBoltManager::log_stacktrace(const JfrStackTrace& stacktrace) {
  Thread* thread = Thread::current();
  HandleMark hm(thread);

  const JfrStackFrame* frames = stacktrace.get_frames();
  unsigned int framesCount = stacktrace.get_framesCount();

  for (u4 i = 0; i < framesCount; ++i) {
    const JfrStackFrame& frame = frames[i];

    JBoltStackFrameKey stackframe_key(const_cast<Method*>(frame.get_method()), frame.get_methodId());

    if (!_sampled_methods_refs->contains(stackframe_key)) {
      jobject method_holder = JNIHandles::make_weak_global(Handle(thread, frame.get_method()->method_holder()->klass_holder()));
      JBoltStackFrameValue stackframe_value(method_holder);
      _sampled_methods_refs->put(stackframe_key, stackframe_value);
      // put() transmits method_holder ownership to element in map
      // set the method_holder to NULL in temp variable stackframe_value, to avoid double free
      stackframe_value.clear_method_holder_but_not_release();
    }
  }
}

methodHandle JBoltManager::lookup_method(Method* method, traceid method_id) {
  Thread* thread = Thread::current();
  JBoltStackFrameKey stackframe_key(method, method_id);
  JBoltStackFrameValue* stackframe_value = _sampled_methods_refs->get(stackframe_key);
  if (stackframe_value == NULL) {
    return methodHandle();
  }

  jobject method_holder = stackframe_value->get_method_holder();
  if (method_holder != NULL && JNIHandles::is_weak_global_handle(method_holder) && JNIHandles::is_global_weak_cleared(method_holder)) {
    log_debug(jbolt)("method at %p is unloaded", (void*)method);
    return methodHandle();
  }

  const Method* const lookup_method = method;
  if (lookup_method == NULL) {
    // stacktrace obsolete
    return methodHandle();
  }
  assert(lookup_method != NULL, "invariant");
  methodHandle method_handle(thread, const_cast<Method*>(lookup_method));

  return method_handle;
}

void JBoltManager::construct_stacktrace(const JfrStackTrace& stacktrace) {
  NoSafepointVerifier nsv;
  if (stacktrace.get_framesCount() < MIN_FRAMESCOUNT)
    return;

  u4 topFrameIndex = 0;
  u4 max_frames = 0;

  const JfrStackFrame* frames = stacktrace.get_frames();
  unsigned int framesCount = stacktrace.get_framesCount();

  // Native method subsidence
  while (topFrameIndex < framesCount) { 
    const JfrStackFrame& frame = frames[topFrameIndex];

    if (strcmp(method_type_to_string(frame.get_type()), "Native") != 0) {
      break;
    }

    topFrameIndex++;
  }

  if (framesCount - topFrameIndex < MIN_FRAMESCOUNT) {
    return;
  }

  os::Linux::jboltLog_precalc(topFrameIndex, max_frames, framesCount);

  JBoltFunc **tempfunc = NULL;

  for (u4 i = 0; i < max_frames; ++i) {
    const JfrStackFrame& frame = frames[topFrameIndex + i];

    methodHandle method = lookup_method(const_cast<Method*>(frame.get_method()), frame.get_methodId());
    if (method.is_null()) {
      break;
    }

    if (i == 0) {
        int hotcount = stacktrace.hotcount();
        int* exist_hotcount = _sampled_methods_hotcount_stored->get(method());
        if (exist_hotcount != NULL) {
            hotcount += *exist_hotcount;
        }
        _sampled_methods_hotcount_stored->put(method(), hotcount);
    }

    const CompiledMethod* const compiled = method->code();

    log_trace(jbolt)(
      "Method id - %lu\n\tBytecode index - %d\n\tSignature - %s\n\tType - %s\n\tCompiler - %s\n\tCompile Level - %d\n\tSize - %dB\n",
      frame.get_methodId(),
      frame.get_byteCodeIndex(),
      method->external_name(),
      method_type_to_string(frame.get_type()),
      compiled != NULL ? compiled->compiler_name() : "None",
      compiled != NULL ? compiled->comp_level() : -1,
      compiled != NULL ? compiled->size() : 0);

    if (compiled == NULL) continue;

    JBoltMethodKey method_key(method->constants()->pool_holder()->name(), method->name(), method->signature());
    JBoltFunc* func = JBoltFunc::constructor(frame.get_method(), frame.get_methodId(), compiled->size(), method_key);
    
    if (!os::Linux::jboltLog_do(related_data_jbolt_log_do, (address)(const_cast<JfrStackTrace*>(&stacktrace)), i, compiled->comp_level(), (address)func, (address*)&tempfunc)) {
      delete func;
      func = NULL;
      continue;
    }
  }

  log_trace(jbolt)(
    "StackTrace hash - %u hotcount - %u\n==============================\n", stacktrace.hash(), stacktrace.hotcount());
}

/**
 * Invoked in JfrStackTraceRepository::write().
 * Each time JfrChunkWrite do write and clear stacktrace table, 
 * we update the CG by invoke construct_stacktrace().
 */
void JBoltManager::construct_cg_once() {
  guarantee((UseJBolt && JBoltManager::reorder_phase_is_profiling_or_waiting()), "sanity");
 
  GrowableArray<JfrStackTrace*>* traces = create_growable_array<JfrStackTrace*>();
 
  {
    MutexLockerEx lock(JfrStacktrace_lock, Mutex::_no_safepoint_check_flag);
    const JfrStackTraceRepository& repository = JfrStackTraceRepository::instance();
 
    if (repository.get_entries_count_jbolt() == 0) {
      return;
    }
 
    const JfrStackTrace* const * table = repository.get_stacktrace_table_jbolt();
    for (uint i = 0; i < repository.TABLE_SIZE; ++i) {
      for (const JfrStackTrace* trace = table[i]; trace != NULL; trace = trace->next()) {
        traces->append(const_cast<JfrStackTrace*>(trace));
      }
    }
  }
  
  for (int i = 0; i < traces->length(); ++i) {
    construct_stacktrace(*(traces->at(i)));
  }
 
  log_trace(jbolt)(
    "+++++++ one time log over ++++++\n\n");
  delete traces;
}

static void write_order(const GrowableArray<JBoltFunc>* order, fileStream& fs) {
  assert(order != NULL, "sanity");
  const char* methodFlag = "M";
  const char* segmentor = "C\n";
 
  log_debug(jbolt)("+============================+\n\t\t\tORDER\n");
 
  for (int i = 0; i < order->length(); ++i) {
    const JBoltFunc& func = order->at(i);
    if (func.method() == NULL) {
      fs.write(segmentor, strlen(segmentor));
      continue;
    }
 
    char* holder_name = func.method_key().klass()->as_C_string();
    char* name = func.method_key().name()->as_C_string();
    char* signature = func.method_key().sig()->as_C_string();
    char size[LINE_BUF_SIZE] = {0};
    snprintf(size, sizeof(size), "%d", func.size());
 
    log_debug(jbolt)("order %d --- Method - %s %s %s\n", i, holder_name, name, signature);
 
    fs.write(methodFlag, strlen(methodFlag));
    fs.write(" ", 1);
    fs.write(size, strlen(size));
    fs.write(" ", 1);
    fs.write(holder_name, strlen(holder_name));
    fs.write(" ", 1);
    fs.write(name, strlen(name));
    fs.write(" ", 1);
    fs.write(signature, strlen(signature));
    fs.write("\n", 1);
  }
}

/**
 * Invoked in before_exit().
 * 
 * Dump the order to JBoltOrderFile before vm exit.
 */
void JBoltManager::dump_order_in_manual() {
  guarantee((UseJBolt && JBoltDumpMode), "sanity");
  guarantee(reorder_phase_profiling_to_waiting(), "sanity");
  NoSafepointVerifier nsv;
  ResourceMark rm;
  GrowableArray<JBoltFunc>* order = JBoltCallGraph::callgraph_instance().hfsort();

  fileStream order_file(JBoltOrderFile, "w+");

  if (JBoltOrderFile == NULL || !order_file.is_open()) {
    log_error(jbolt)("JBoltOrderFile open error");
    vm_exit_during_initialization("JBoltOrderFile open error");
  }

  write_order(order, order_file);

  log_info(jbolt)("order generate successful !!");
  log_debug(jbolt)("+============================+\n");  
  delete order;
  delete _sampled_methods_refs;
  _sampled_methods_refs = NULL;
  JBoltCallGraph::deinitialize();
}

JBoltErrorCode JBoltManager::dump_order_in_jcmd(const char* filename) {
  guarantee(UseJBolt, "sanity");
  NoSafepointVerifier nsv;
  ResourceMark rm;
 
  if (_order_stored == NULL) return JBoltOrderNULL;
 
  fileStream order_file(filename, "w+");
 
  if (filename == NULL || !order_file.is_open()) return JBoltOpenFileError;
 
  write_order(_order_stored, order_file);
 
  return JBoltOK;
}

#define check_arg_not_set(flag)                                                                           \
do {                                                                                                      \
    if (FLAG_IS_CMDLINE(flag)) {                                                                          \
        vm_exit_during_initialization(err_msg("Do not set VM option " #flag " without UseJBolt enabled.")); \
    }                                                                                                     \
} while(0)

/**
 * Do not set the JBolt-related flags manually if UseJBolt is not enabled.
 */
void JBoltManager::check_arguments_not_set() {
  if (UseJBolt) return;

  check_arg_not_set(JBoltDumpMode);
  check_arg_not_set(JBoltLoadMode);
  check_arg_not_set(JBoltOrderFile);
  check_arg_not_set(JBoltSampleInterval);
  check_arg_not_set(JBoltCodeHeapSize);
  check_arg_not_set(JBoltRescheduling);
  check_arg_not_set(JBoltReorderThreshold);
  check_arg_not_set(EnableDumpGraph);
}

/**
 * Check which mode is JBolt in.
 * If JBoltDumpMode or JBoltLoadMode is set manually then do nothing, else it will be fully auto sched by JBolt itself.
 */
void JBoltManager::check_mode() {
  if (!(JBoltDumpMode || JBoltLoadMode)) {
    _auto_mode = true;
    return;
  }
 
  if (!FLAG_IS_DEFAULT(JBoltSampleInterval)) {
    log_warning(jbolt)("JBoltSampleInterval is ignored because it is not in auto mode.");
  }

  if (JBoltDumpMode && JBoltLoadMode) {
    vm_exit_during_initialization("Do not set both JBoltDumpMode and JBoltLoadMode!");
  }
 
  guarantee((JBoltDumpMode ^ JBoltLoadMode), "Must set either JBoltDumpMode or JBoltLoadMode!");
}
 
/**
 * If in auto mode, JBoltOrderFile will be ignored
 * If in any manual mode, then JBoltOrderFile will be necessary.
 * Check whether the order file exists or is accessable.
 */
void JBoltManager::check_order_file() {
  if (auto_mode()) {
    if (JBoltOrderFile != NULL) log_warning(jbolt)("JBoltOrderFile is ignored because it is in auto mode.");
    return;
  }

  if (JBoltOrderFile == NULL) {
    vm_exit_during_initialization("JBoltOrderFile is not set!");
  }

  bool file_exist = (::access(JBoltOrderFile, F_OK) == 0);
  if (file_exist) {
    if (JBoltDumpMode) {
      log_warning(jbolt)("JBoltOrderFile to dump already exists and will be overwritten: file=%s.", JBoltOrderFile);
      ::remove(JBoltOrderFile);
    }
  } else {
    if (JBoltLoadMode) {
      vm_exit_during_initialization(err_msg("JBoltOrderFile does not exist or cannot be accessed! file=\"%s\".", JBoltOrderFile));
    }
  }
}

void JBoltManager::check_dependency() {
  if (FLAG_IS_CMDLINE(FlightRecorder) ? !FlightRecorder : false) {
    vm_exit_during_initialization("JBolt depends on JFR!");
  }

  if (!CompilerConfig::is_c2_enabled()) {
    vm_exit_during_initialization("JBolt depends on C2!");
  }

  if (!SegmentedCodeCache) {
    vm_exit_during_initialization("JBolt depends on SegmentedCodeCache!");
  }
}

size_t JBoltManager::calc_nmethod_size_with_padding(size_t nmethod_size) {
  return align_up(nmethod_size, (size_t) CodeCacheSegmentSize);
}

size_t JBoltManager::calc_segment_size_with_padding(size_t segment_size) {
  size_t page_size = CodeCache::page_size();
  if (segment_size < page_size) return page_size;
  return align_down(segment_size, page_size);
}

/**
 * We have to parse the file twice because SymbolTable is not inited in phase 1...
 */
void JBoltManager::load_order_file_phase1(int* method_cnt, size_t* segment_size) {
  assert(JBoltOrderFile != NULL, "sanity");

  _order_fp = os::fopen(JBoltOrderFile, "r");
  if (_order_fp == NULL) {
    vm_exit_during_initialization(err_msg("Cannot open file JBoltOrderFile! file=\"%s\".", JBoltOrderFile));
  }

  int mth_cnt = 0;
  size_t seg_size = 0;

  char line[LINE_BUF_SIZE];
  int len = -1;
  while (read_line(_order_fp, line, sizeof(line), &len)) {
    if (len <= 2) continue;
    if (line[0] != 'M' || line[1] != ' ') continue;
    char* left_start = line + 2;

    // parse nmethod size
    size_t nmethod_size;
    if (!read_a_size(left_start, &nmethod_size)) {
      vm_exit_during_initialization(err_msg("Wrong format of JBolt order line! line=\"%s\".", line));
    }
    ++mth_cnt;
    seg_size += calc_nmethod_size_with_padding(nmethod_size);
  }

  *method_cnt = mth_cnt;
  *segment_size = seg_size;
  log_trace(jbolt)("Read order file method_cnt=%d, estimated_segment_size=" SIZE_FORMAT ".", mth_cnt, seg_size);
}

bool JBoltManager::parse_method_line_phase2(char* const line, const int len, TRAPS) {
  // Skip "M ".
  char* left_start = line + 2;

  // Skip nmethod size (has parsed in phase1).
  {
    char* t = strchr(left_start, ' ');
    if (t == NULL) return false;
    left_start = t + 1;
  }

  // Modify "java.lang.Obj" to "java/lang/Obj".
  replace_all(left_start, '.', '/');

  // Parse the three symbols: class name, method name, signature.
  Symbol* three_symbols[3];
  for (int i = 0; i < 2; ++i) {
    char* t = strchr(left_start, ' ');
    if (t == NULL) return false;
    Symbol* sym = SymbolTable::new_symbol(left_start, t - left_start, THREAD);
    three_symbols[i] = sym;
    left_start = t + 1;
  }
  Symbol* sym = SymbolTable::new_symbol(left_start, line + len - left_start, THREAD);
  three_symbols[2] = sym;
  if (log_is_enabled(Trace, jbolt)) {
    log_trace(jbolt)("HotMethod init: key={%s %s %s}",
                      three_symbols[0]->as_C_string(),
                      three_symbols[1]->as_C_string(),
                      three_symbols[2]->as_C_string());
  }

  // Add to data structure.
  JBoltMethodKey method_key(three_symbols[0], three_symbols[1], three_symbols[2]);
  _hot_methods_sorted->append(method_key);
  JBoltMethodValue method_value;
  bool put = _hot_methods_vis->put(method_key, method_value);
  if (!put) {
    vm_exit_during_initialization(err_msg("Duplicated method: {%s %s %s}!",
            three_symbols[0]->as_C_string(),
            three_symbols[1]->as_C_string(),
            three_symbols[2]->as_C_string()));
  }

  return true;
}

bool JBoltManager::parse_connected_component_line_phase2(char* const line, const int len) { return true; }

void JBoltManager::load_order_file_phase2(TRAPS) {
  guarantee(_order_fp != NULL, "sanity");

  // re-scan
  fseek(_order_fp, 0, SEEK_SET);

  char line[LINE_BUF_SIZE];
  int len = -1;
  while (read_line(_order_fp, line, sizeof(line), &len)) {
    if (len <= 0) continue;
    bool success = false;
    switch (line[0]) {
      case '#': success = true; break;  // ignore comments
      case 'M': success = parse_method_line_phase2(line, len, THREAD); break;
      case 'C': success = parse_connected_component_line_phase2(line, len); break;
      default: break;
    }
    if (!success) {
      vm_exit_during_initialization(err_msg("Wrong format of JBolt order line! line=\"%s\".", line));
    }
  }
  fclose(_order_fp);
  _order_fp = NULL;
}

void JBoltManager::init_load_mode_phase1() {
  if (!(auto_mode() || JBoltLoadMode)) return;

  if (auto_mode()) {
    // auto mode has no order now, initialize as default.
    _hot_methods_sorted = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<JBoltMethodKey>(1, mtCompiler);
    _hot_methods_vis = new (ResourceObj::C_HEAP, mtCompiler) MethodKeyMap();
    log_info(jbolt)("Default set JBoltCodeHeapSize=" UINTX_FORMAT " B (" UINTX_FORMAT " MB).", JBoltCodeHeapSize, JBoltCodeHeapSize / 1024 / 1024);
    return;
  }
  guarantee(reorder_phase_available_to_collecting(), "sanity");
  size_t total_nmethod_size = 0;
  int method_cnt = 0;
  load_order_file_phase1(&method_cnt, &total_nmethod_size);
 
  _hot_methods_sorted = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<JBoltMethodKey>(method_cnt, mtCompiler);
  _hot_methods_vis = new (ResourceObj::C_HEAP, mtCompiler) MethodKeyMap();

  if (FLAG_IS_DEFAULT(JBoltCodeHeapSize)) {
    FLAG_SET_ERGO(uintx, JBoltCodeHeapSize, calc_segment_size_with_padding(total_nmethod_size));
    log_info(jbolt)("Auto set JBoltCodeHeapSize=" UINTX_FORMAT " B (" UINTX_FORMAT " MB).", JBoltCodeHeapSize, JBoltCodeHeapSize / 1024 / 1024);
  }
}

void JBoltManager::init_load_mode_phase2(TRAPS) {
  // Only manual load mode need load phase2
  if (!JBoltLoadMode) return;

  load_order_file_phase2(CHECK);
  _reorderable_method_cnt = 0;
  _reorder_method_threshold_cnt = _hot_methods_sorted->length() * JBoltReorderThreshold;
}

void JBoltManager::init_dump_mode_phase2(TRAPS) {
  if (!(auto_mode() || JBoltDumpMode)) return;
 
  JBoltCallGraph::initialize();
  _sampled_methods_refs = new (ResourceObj::C_HEAP, mtTracing) StackFrameKeyMap();
  _sampled_methods_hotcount_stored = new (ResourceObj::C_HEAP, mtTracing) MethodHotCountMap();
 
  // JBolt will create a JFR by itself
  // In auto mode, will stop in JBoltControlThread::start_thread() after JBoltSampleInterval.
  // In manual dump mode, won't stop until program exit.
  log_info(jbolt)("JBolt in dump mode now, start a JFR recording named \"jbolt-jfr\".");
  bufferedStream output;
  DCmd::parse_and_execute(DCmd_Source_Internal, &output, "JFR.start name=jbolt-jfr", ' ', THREAD);
  if (HAS_PENDING_EXCEPTION) {
    ResourceMark rm;
    log_warning(jbolt)("unable to start jfr jbolt-jfr");
    log_warning(jbolt)("exception type: %s", PENDING_EXCEPTION->klass()->external_name());
    // don't unwind this exception
    CLEAR_PENDING_EXCEPTION;
  }
}
 
static void update_stored_order(const GrowableArray<JBoltFunc>* order) {
  if (_order_stored != NULL) {
    // use a tmp for releasing space to provent _order_stored from being a wild pointer
    GrowableArray<JBoltFunc>* tmp = _order_stored;
    _order_stored = NULL;
    delete tmp;
  }
  _order_stored = new (ResourceObj::C_HEAP, mtTracing) GrowableArray<JBoltFunc>(order->length(), mtTracing);
  _order_stored->appendAll(order);
}
 
static CompileTaskInfo* create_compile_task_info(const methodHandle& method) {
    CompiledMethod* compiled = method->code();
    if (compiled == NULL) {
      log_trace(jbolt)("Recompilation Task init failed because of null nmethod. func: %s.", method->external_name());
      return NULL;
    }
    int osr_bci = compiled->is_osr_method() ? compiled->osr_entry_bci() : InvocationEntryBci;
    int comp_level = compiled->comp_level();
    // comp_level adaptation for deoptmization
    if (comp_level > CompLevel_simple && comp_level <= CompLevel_full_optimization) comp_level = CompLevel_full_optimization;
    CompileTask::CompileReason comp_reason = CompileTask::Reason_Reorder;
    CompileTaskInfo* ret = new CompileTaskInfo(method(), osr_bci, comp_level, (int)comp_reason,
                                                       NULL, 0);
    return ret;
}
 
/**
 * This function is invoked by JBoltControlThread.
 * Do initialization for converting dump mode to load mode.
 */
void JBoltManager::init_auto_transition(size_t* segment_size, TRAPS) {
  guarantee(UseJBolt && auto_mode(), "sanity");
  NoSafepointVerifier nsv;
  ResourceMark rm;
 
  GrowableArray<JBoltFunc>* order = JBoltCallGraph::callgraph_instance().hfsort();
  update_stored_order(order);
 
  size_t seg_size = 0;
  for (int i = 0; i < order->length(); ++i) {
    const JBoltFunc& func = order->at(i);
    if (func.method() == NULL) {
      continue;
    }
 
    methodHandle method = lookup_method(const_cast<Method*>(func.method()), func.method_id());
    if (method.is_null()) {
      continue;
    }
 
    CompileTaskInfo* cti = create_compile_task_info(method);
    if (cti == NULL) {
      continue;
    }
 
    JBoltMethodKey method_key = func.method_key();
    JBoltMethodValue method_value;
    if (!method_value.set_comp_info(cti)) {
      delete cti;
      continue;
    }
 
    seg_size += calc_nmethod_size_with_padding(func.size());
    _hot_methods_sorted->append(method_key);
    bool put = _hot_methods_vis->put(method_key, method_value);
    if (!put) {
      vm_exit_during_initialization(err_msg("Duplicated method: {%s %s %s}!",
              method_key.klass()->as_C_string(),
              method_key.name()->as_C_string(),
              method_key.sig()->as_C_string()));
    }
    method_value.clear_comp_info_but_not_release();
  }
  log_info(jbolt)("order generate successful !!");
  *segment_size = calc_segment_size_with_padding(seg_size);
  delete order;
}

/**
 * This function must be invoked after CompilerConfig::ergo_initialize() in Arguments::apply_ergo().
 * This function must be invoked before CodeCache::initialize_heaps() in codeCache_init() in init_globals().
 * Thread and SymbolTable is not inited now!
 */
void JBoltManager::init_phase1() {
  if (!UseJBolt) return;
  check_mode();
  check_dependency();
  check_order_file();
  parse_rescheduling();

  /* dump mode has nothing to do in phase1 */
  init_load_mode_phase1();
}

void JBoltManager::init_phase2(TRAPS) {
  if (!UseJBolt) return;

  ResourceMark rm(THREAD);
  init_dump_mode_phase2(CHECK);
  init_load_mode_phase2(CHECK);
 
  // Manual dump mode doesn't need JBoltControlThread, directly go to profiling phase
  if (JBoltDumpMode) {
    guarantee(JBoltManager::reorder_phase_available_to_profiling(), "sanity");
    return;
  }
 
  JBoltControlThread::init(CHECK);
  // Auto mode will start control thread earlier.
  // Manual load mode start later in check_start_reordering()
  if (auto_mode()) {
    JBoltControlThread::start_thread(CHECK_AND_CLEAR);
  }
}

/**
 * Code heaps are initialized between init phase 1 and init phase 2.
 */
void JBoltManager::init_code_heaps(size_t non_nmethod_size, size_t profiled_size, size_t non_profiled_size, size_t cache_size, size_t alignment) {
  assert(UseJBolt && !JBoltDumpMode, "sanity");
  if(!is_aligned(JBoltCodeHeapSize, alignment)) {
    vm_exit_during_initialization(err_msg("JBoltCodeHeapSize should be %ld aligned, please adjust", alignment));
  }

  size_t jbolt_hot_size     = JBoltCodeHeapSize;
  size_t jbolt_tmp_size     = JBoltCodeHeapSize;
  size_t jbolt_total_size   = jbolt_hot_size + jbolt_tmp_size;
  if (non_profiled_size <= jbolt_total_size) {
    vm_exit_during_initialization(err_msg(
        "Not enough space in non-profiled code heap to split out JBolt heap(s): " SIZE_FORMAT "K <= " SIZE_FORMAT "K",
        non_profiled_size/K, jbolt_total_size/K));
  }
  non_profiled_size -= jbolt_total_size;
  non_profiled_size = align_down(non_profiled_size, alignment);
  FLAG_SET_ERGO(uintx, NonProfiledCodeHeapSize, non_profiled_size);

  ReservedCodeSpace rs = CodeCache::reserve_heap_memory(cache_size);
  ReservedSpace non_nmethod_space, profiled_space, non_profiled_space, jbolt_hot_space, jbolt_tmp_space;

  uintptr_t related_data_jbolt_heap_init[] = {
    (uintptr_t)non_nmethod_size,
    (uintptr_t)profiled_size,
    (uintptr_t)non_profiled_size,
    (uintptr_t)jbolt_hot_size,
    (uintptr_t)jbolt_tmp_size,

    (uintptr_t)ReservedSpace::static_first_part,
    (uintptr_t)ReservedSpace::static_last_part
  };

  if (!os::Linux::jboltHeap_init(related_data_jbolt_heap_init, (address)&rs, (address)&non_nmethod_space, (address)&profiled_space, (address)&non_profiled_space, (address)&jbolt_hot_space, (address)&jbolt_tmp_space)) {
    jbolt_hot_size = CodeCache::page_size();
    jbolt_tmp_size = CodeCache::page_size();
    non_profiled_size += (jbolt_total_size - 2 * CodeCache::page_size());
    // Reserve one continuous chunk of memory for CodeHeaps and split it into
    // parts for the individual heaps. The memory layout looks like this:
    // ---------- high -----------
    //    Non-profiled nmethods
    //      JBolt tmp nmethods
    //      JBolt hot nmethods
    //      Profiled nmethods
    //         Non-nmethods
    // ---------- low ------------
    non_nmethod_space   = rs.first_part(non_nmethod_size);
    ReservedSpace r1    = rs.last_part(non_nmethod_size);
    profiled_space      = r1.first_part(profiled_size);
    ReservedSpace r2    = r1.last_part(profiled_size);
    jbolt_hot_space     = r2.first_part(jbolt_hot_size);
    ReservedSpace r3    = r2.last_part(jbolt_hot_size);
    jbolt_tmp_space     = r3.first_part(jbolt_tmp_size);
    non_profiled_space  = r3.last_part(jbolt_tmp_size);
  }

  CodeCache::add_heap(non_nmethod_space, "CodeHeap 'non-nmethods'", CodeBlobType::NonNMethod);
  CodeCache::add_heap(profiled_space, "CodeHeap 'profiled nmethods'", CodeBlobType::MethodProfiled);
  CodeCache::add_heap(non_profiled_space, "CodeHeap 'non-profiled nmethods'", CodeBlobType::MethodNonProfiled);
  const char* no_space = NULL;
  CodeCache::add_heap(jbolt_hot_space, "CodeHeap 'jbolt hot nmethods'", CodeBlobType::MethodJBoltHot);
  if (jbolt_hot_size != jbolt_hot_space.size()) {
    no_space = "hot";
  }
  CodeCache::add_heap(jbolt_tmp_space, "CodeHeap 'jbolt tmp nmethods'", CodeBlobType::MethodJBoltTmp);
  if (jbolt_tmp_size != jbolt_tmp_space.size()) {
    no_space = "tmp";
  }
  if (no_space != NULL) {
    vm_exit_during_initialization(FormatBuffer<1024>(
        "No enough space for JBolt %s heap: \n"
        "Expect: cache_size=" SIZE_FORMAT "K, profiled_size=" SIZE_FORMAT "K, non_nmethod_size=" SIZE_FORMAT "K, jbolt_hot_size=" SIZE_FORMAT "K, non_profiled_size=" SIZE_FORMAT "K, jbolt_tmp_size=" SIZE_FORMAT "K\n"
        "Actual: cache_size=" SIZE_FORMAT "K, profiled_size=" SIZE_FORMAT "K, non_nmethod_size=" SIZE_FORMAT "K, jbolt_hot_size=" SIZE_FORMAT "K, non_profiled_size=" SIZE_FORMAT "K, jbolt_tmp_size=" SIZE_FORMAT "K\n"
        "alignment=" SIZE_FORMAT,
        no_space,
        cache_size/K, profiled_size/K,         non_nmethod_size/K,         jbolt_hot_size/K,         non_profiled_size/K,         jbolt_tmp_size/K,
        rs.size()/K,  profiled_space.size()/K, non_nmethod_space.size()/K, jbolt_hot_space.size()/K, non_profiled_space.size()/K, jbolt_tmp_space.size()/K,
        alignment));
  }
}

int JBoltManager::reorder_phase() {
  return OrderAccess::load_acquire(&_reorder_phase);
}

bool JBoltManager::reorder_phase_available_to_collecting() {
  assert(!auto_mode(), "two-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Collecting, &_reorder_phase, JBoltReorderPhase::Available) == JBoltReorderPhase::Available;
}
 
bool JBoltManager::reorder_phase_collecting_to_reordering() {
  assert(!auto_mode(), "two-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Reordering, &_reorder_phase, JBoltReorderPhase::Collecting) == JBoltReorderPhase::Collecting;
}
 
bool JBoltManager::reorder_phase_available_to_profiling() {
  return Atomic::cmpxchg(JBoltReorderPhase::Profiling, &_reorder_phase, JBoltReorderPhase::Available) == JBoltReorderPhase::Available;
}
 
bool JBoltManager::reorder_phase_profiling_to_reordering() {
  assert(auto_mode(), "one-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Reordering, &_reorder_phase, JBoltReorderPhase::Profiling) == JBoltReorderPhase::Profiling;
}
 
bool JBoltManager::reorder_phase_reordering_to_available() {
  assert(auto_mode(), "one-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Available, &_reorder_phase, JBoltReorderPhase::Reordering) == JBoltReorderPhase::Reordering;
}
 
bool JBoltManager::reorder_phase_profiling_to_available() {
  assert(auto_mode(), "one-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Available, &_reorder_phase, JBoltReorderPhase::Profiling) == JBoltReorderPhase::Profiling;
}

bool JBoltManager::reorder_phase_profiling_to_waiting() {
  return Atomic::cmpxchg(JBoltReorderPhase::Waiting, &_reorder_phase, JBoltReorderPhase::Profiling) == JBoltReorderPhase::Profiling;
}
 
bool JBoltManager::reorder_phase_waiting_to_reordering() {
  assert(auto_mode(), "one-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Reordering, &_reorder_phase, JBoltReorderPhase::Waiting) == JBoltReorderPhase::Waiting;
}
 
bool JBoltManager::reorder_phase_waiting_to_available() {
  assert(auto_mode(), "one-phase only");
  return Atomic::cmpxchg(JBoltReorderPhase::Available, &_reorder_phase, JBoltReorderPhase::Waiting) == JBoltReorderPhase::Waiting;
}

bool JBoltManager::reorder_phase_reordering_to_end() {
  return Atomic::cmpxchg(JBoltReorderPhase::End, &_reorder_phase, JBoltReorderPhase::Reordering) == JBoltReorderPhase::Reordering;
}

bool JBoltManager::reorder_phase_is_waiting() {
  return OrderAccess::load_acquire(&_reorder_phase) == JBoltReorderPhase::Waiting;
}

bool JBoltManager::reorder_phase_is_available() {
  bool res = (OrderAccess::load_acquire(&_reorder_phase) == JBoltReorderPhase::Available);
  assert(!res || auto_mode(), "one-phase only");
  return res;
}
 
bool JBoltManager::reorder_phase_is_collecting() {
  bool res = (OrderAccess::load_acquire(&_reorder_phase) == JBoltReorderPhase::Collecting);
  assert(!res || !auto_mode(), "two-phase only");
  return res;
}
 
bool JBoltManager::reorder_phase_is_profiling() {
  bool res = (OrderAccess::load_acquire(&_reorder_phase) == JBoltReorderPhase::Profiling);
  return res;
}
 
bool JBoltManager::reorder_phase_is_reordering() {
  return OrderAccess::load_acquire(&_reorder_phase) == JBoltReorderPhase::Reordering;
}

bool JBoltManager::reorder_phase_is_profiling_or_waiting() {
  int p = OrderAccess::load_acquire(&_reorder_phase);
  return p == JBoltReorderPhase::Profiling || p == JBoltReorderPhase::Waiting;
}

bool JBoltManager::reorder_phase_is_collecting_or_reordering() {
  int p = OrderAccess::load_acquire(&_reorder_phase);
  assert(p != JBoltReorderPhase::Collecting || !auto_mode(), "two-phase only");
  return p == JBoltReorderPhase::Collecting || p == JBoltReorderPhase::Reordering;
}

Method* JBoltManager::cur_reordering_method() {
  return OrderAccess::load_acquire(&_cur_reordering_method);
}

void JBoltManager::set_cur_reordering_method(Method* method) {
  OrderAccess::release_store(&_cur_reordering_method, method);
}

int JBoltManager::inc_reorderable_method_cnt() {
  return Atomic::add(+1, &_reorderable_method_cnt);
}

bool JBoltManager::can_reorder_now() {
  return OrderAccess::load_acquire(&_reorderable_method_cnt) >= _reorder_method_threshold_cnt;
}

bool JBoltManager::should_reorder_now() {
  return OrderAccess::load_acquire(&_reorderable_method_cnt) == _reorder_method_threshold_cnt;
}

int JBoltManager::primary_hot_seg() {
  return OrderAccess::load_acquire(&_primary_hot_seg);
}

int JBoltManager::secondary_hot_seg() {
  return OrderAccess::load_acquire(&_secondary_hot_seg);
}

bool JBoltManager::force_sweep() {
  return OrderAccess::load_acquire(&_force_sweep);
}

static bool is_valid_time(const char* timeStr) {
  // hh:mm
  if (strlen(timeStr) != 5) return false;

  if (timeStr[2] != ':') return false;

  if (timeStr[0] < '0' || timeStr[0] > '2') return false;
  if (timeStr[1] < '0' || timeStr[1] > '9') return false;
  if (timeStr[3] < '0' || timeStr[3] > '5') return false;
  if (timeStr[4] < '0' || timeStr[4] > '9') return false;

  int hour = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
  int minute = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;

  return true;
}

void JBoltManager::remove_duplicate_time(GrowableArray<char*>* times) {
  for (int i = 0; i < times->length(); ++i) {
    char* time = times->at(i);
    bool exists = false;
    for (int j = 0; j < _rescheduling_time->length(); ++j) {
      char* uniqueTime = _rescheduling_time->at(j);
      if (strcmp(time, uniqueTime) == 0) {
        exists = true;
        log_warning(jbolt)("time %s is duplicated in JBoltRescheduling", time);
        break;
      }
    }
    if (!exists) {
      if (_rescheduling_time->length() >= 10) {
        // support max 10 time to reschedule
        log_warning(jbolt)("JBoltRescheduling support up to 10 time settings, any excess will be ignored.");
        return;
      }
      log_trace(jbolt)("Set time trigger at %s", time);
      _rescheduling_time->append(time);
    }
  }
}

static int time_comparator(char** time1, char** time2) { 
  int hour1 = ((*time1)[0] - '0') * 10 + ((*time1)[1] - '0');
  int minute1 = ((*time1)[3] - '0') * 10 + ((*time1)[4] - '0');
  int hour2 = ((*time2)[0] - '0') * 10 + ((*time2)[1] - '0');
  int minute2 = ((*time2)[3] - '0') * 10 + ((*time2)[4] - '0');
    
  if (hour1 == hour2) {
      return (minute1 > minute2) ? 1 : ((minute1 == minute2) ? 0 : -1);
  }
  return (hour1 > hour2) ? 1 : ((hour1 == hour2) ? 0 : -1);
}

void JBoltManager::parse_rescheduling() {
  if (!FLAG_IS_CMDLINE(JBoltRescheduling)) return;

  if (JBoltRescheduling == NULL || strlen(JBoltRescheduling) == 0) {
    vm_exit_during_initialization("JBoltRescheduling is set but is null");
  }

  const int buflen = 1024;
  if (strlen(JBoltRescheduling) > buflen) {
    vm_exit_during_initialization("JBoltRescheduling is too long");
  }

  if (!auto_mode()) {
    log_warning(jbolt)("JBoltRescheduling is ignored because it is not in auto mode.");
    return;
  }

  ResourceMark rm;
  _rescheduling_time = new (ResourceObj::C_HEAP, mtTracing) GrowableArray<char*>(1, mtTracing);
  GrowableArray<char*>* tmp_time = new (ResourceObj::C_HEAP, mtTracing) GrowableArray<char*>(1, mtTracing);

  const char* rescheduling_str = JBoltRescheduling;
  const char* start = rescheduling_str;
  const char* end = strchr(rescheduling_str, ',');
  char timeStr[buflen] = {0};

  while (end != NULL) {
    size_t len = (size_t)(end - start);
    strncpy(timeStr, start, buflen);
    timeStr[len] = '\0';

    if (is_valid_time(timeStr)) {
      tmp_time->append(strdup(timeStr));
    }
    else {
      vm_exit_during_initialization(err_msg("Invalid time %s in JBoltRescheduling", timeStr));
    }

    start = end + 1;
    end = strchr(start, ',');
  }

  if (*start != '\0') {
    strncpy(timeStr, start, buflen);
    timeStr[strlen(start)] = '\0';

    if (is_valid_time(timeStr)) {
      tmp_time->append(strdup(timeStr));
    }
    else {
      vm_exit_during_initialization(err_msg("Invalid time %s in JBoltRescheduling", timeStr));
    }
  }

  remove_duplicate_time(tmp_time);
  _rescheduling_time->sort(&time_comparator);

  delete tmp_time;
}

GrowableArray<char*>* JBoltManager::rescheduling_time() {
  return _rescheduling_time;
}

int JBoltManager::clear_manager() {
  /* _hot_methods_sorted, _hot_methods_vis and _sampled_methods_refs have been cleared in other pos, don't delete again */
  guarantee(_hot_methods_sorted == NULL, "sanity");
  guarantee(_hot_methods_vis == NULL, "sanity");
  guarantee(_sampled_methods_refs == NULL, "sanity");
  // Re-allocate them
  _hot_methods_sorted = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<JBoltMethodKey>(1, mtCompiler);
  _hot_methods_vis = new (ResourceObj::C_HEAP, mtCompiler) MethodKeyMap();
  _sampled_methods_refs = new (ResourceObj::C_HEAP, mtTracing) StackFrameKeyMap();

  if (_sampled_methods_hotcount_stored != NULL) {
    MethodHotCountMap* tmp = _sampled_methods_hotcount_stored;
    _sampled_methods_hotcount_stored = NULL;
    delete tmp;
  }
  _sampled_methods_hotcount_stored = new (ResourceObj::C_HEAP, mtTracing) MethodHotCountMap();

  return 0;
}
 
/**
 * Invoked in JBoltControlThread::prev_control_schedule().
 * Expect to only execute in auto mode while JBolt.start triggered.
 * Clear JBolt related data structures to restore a initial env same as sample never happening.
*/
int JBoltManager::clear_last_sample_datas() {
  int ret = 0;
  // Clear _table_jbolt in JfrStackTraceRepository
  ret = JfrStackTraceRepository::clear_jbolt();
  // Clear JBoltCallGraph
  ret = JBoltCallGraph::callgraph_instance().clear_instance();
  // Clear JBoltManager
  ret = clear_manager();
 
  return ret;
}
 
/**
 * Invoked in JBoltControlThread::prev_control_schedule().
 * Swap primary hot segment with secondary hot segment
 */
void JBoltManager::swap_semi_jbolt_segs() {
  guarantee(reorder_phase_is_waiting(), "swap must happen in reorder phase Profiling.");
  int tmp = Atomic::xchg(OrderAccess::load_acquire(&_primary_hot_seg), &_secondary_hot_seg);
  Atomic::xchg(tmp, &_primary_hot_seg);
  OrderAccess::release_store(&_hot_codecache_full, false);
}
 
/**
 * Invoked in JBoltControlThread::post_control_schdule().
 * Free scondary hot segment space for next reorder.
 */
void JBoltManager::clear_secondary_hot_seg(TRAPS) {
  guarantee(reorder_phase_is_available(), "secondary clear must happen in reorder phase Available.");
  // scan secondary hot seg and recompile alive nmethods to non-profiled
  ResourceMark rm(THREAD);
  // We cannot alloc weak handle within CodeCache_lock because of the mutex rank check.
  // So instead we keep the methods alive only within the scope of this method.
  JBoltUtils::MetaDataKeepAliveMark mdm(THREAD);
  const GrowableArray<Metadata*>& to_recompile = mdm.kept();
 
  {
    MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    CodeHeap* sec_hot = CodeCache::get_code_heap(secondary_hot_seg());
    for (CodeBlob* cb = (CodeBlob*) sec_hot->first(); cb != NULL; cb = (CodeBlob*) sec_hot->next(cb)) {
      nmethod* nm = cb->as_nmethod_or_null();
      Method* m = nm->method();
      if (nm && nm->get_state() == CompiledMethod::in_use && m != NULL) {
        mdm.add(m);
      }
    }
  }
 
  for (int i = 0; i < to_recompile.length(); ++i) {
    Method* m = (Method*) to_recompile.at(i);
    methodHandle method(THREAD, m);
    CompileTaskInfo* cti = create_compile_task_info(method);
    if (cti == NULL) continue;
    guarantee(cti->try_select(), "method is on stack, should be ok");
    assert(cti->hot_method() == NULL, "sanity");
    methodHandle hot_method;
 
    bool recompile_result = enqueue_recompile_task(cti, method, hot_method, THREAD);
    if(recompile_result) {
      check_compiled_result(method(), CodeBlobType::MethodNonProfiled, THREAD);
    }
    delete cti;
  }

  OrderAccess::release_store(&_force_sweep, true);
  // need 2 cleaning passes before not_entrant converting to zombie, @see nmethod::mark_as_seen_on_stack
  NMethodSweeper::force_sweep();
  NMethodSweeper::force_sweep();
  // this time sweep converting to zombie
  NMethodSweeper::force_sweep();
  // this time sweep cleaning zombie
  NMethodSweeper::force_sweep();
  OrderAccess::release_store(&_force_sweep, false);
  log_info(jbolt)("Sweep secondary codecache: %s", CodeCache::get_code_heap_name(JBoltManager::secondary_hot_seg()));
  print_code_heaps();
}

/**
 * Invoked in ciEnv::register_method() in CompilerThread.
 * Controls where the new nmethod should be allocated.
 *
 * Returns CodeBlobType::All if it is not determined by JBolt logic.
 */
int JBoltManager::calc_code_blob_type(Method* method, CompileTask* task, TRAPS) {
  assert(UseJBolt && reorder_phase_is_collecting_or_reordering(), "sanity");
  const int not_care = CodeBlobType::All;

  // Only cares about non-profiled segment.
  int lvl = task->comp_level();
  if (lvl != CompLevel_full_optimization && lvl != CompLevel_simple) {
    return not_care;
  }

  // Ignore on-stack-replacement.
  if (task->osr_bci() != InvocationEntryBci) {
    return not_care;
  }

  int cur_reorder_phase = reorder_phase();
  // Do nothing after reordering.
  if (cur_reorder_phase != JBoltReorderPhase::Collecting && cur_reorder_phase != JBoltReorderPhase::Reordering) {
    return not_care;
  }
  // Only cares about the current reordering method.
  if (cur_reorder_phase == JBoltReorderPhase::Reordering) {
    if (cur_reordering_method() == method) {
      log_trace(jbolt)("Compiling to JBolt heap: method=%s.", method->name_and_sig_as_C_string());
      return primary_hot_seg();
    }
    return not_care;
  }
  guarantee(cur_reorder_phase == JBoltReorderPhase::Collecting, "sanity");
  assert(!auto_mode(), "sanity");

  JBoltMethodKey method_key(method);
  JBoltMethodValue* method_value = _hot_methods_vis->get(method_key);
  if (method_value == NULL) {
    return not_care;
  }

  // Register the method and the compile task.
  if (method_value->get_comp_info() == NULL) {
    CompileTaskInfo* cti = new CompileTaskInfo(method, task->osr_bci(), task->comp_level(), (int) task->compile_reason(),
                                                       task->hot_method(), task->hot_count());
    if (method_value->set_comp_info(cti)) {
      int cnt = inc_reorderable_method_cnt();
      log_trace(jbolt)("Reorderable method found: cnt=%d, lvl=%d, p=%p, method=%s.",
              cnt, task->comp_level(), method, method->name_and_sig_as_C_string());
      if (is_power_of_2(_reorder_method_threshold_cnt - cnt)) {
        log_info(jbolt)("Reorderable cnt: %d/%d/%d", cnt, _reorder_method_threshold_cnt, _hot_methods_sorted->length());
      }
      if (cnt == _reorder_method_threshold_cnt) {
        log_info(jbolt)("Time to reorder: %d/%d/%d", cnt, _reorder_method_threshold_cnt, _hot_methods_sorted->length());
        _start_reordering_thread = THREAD;
      }
    } else {
      delete cti;
    }
  }

  return secondary_hot_seg();
}

/*
 * Invoked in CodeCache::allocate()
 * set _hot_codecache_full to stop recompilation early
 */
void JBoltManager::handle_full_jbolt_code_cache() {
  log_warning(jbolt)("%s is full, will stop recompilation", CodeCache::get_code_heap_name(primary_hot_seg()));
  OrderAccess::release_store(&_hot_codecache_full, true);
}

/**
 * Check if reordering should start.
 * The reordering should only start once (for now).
 * We don't do this check in "if (cnt == _reorder_method_threshold_cnt)" in calc_code_blob_type()
 * because it will cause an assert error: "Possible safepoint reached by thread that does not allow it".
 */
void JBoltManager::check_start_reordering(TRAPS) {
  // _start_reordering_thread is set and tested in the same thread. No need to be atomic.
  if (_start_reordering_thread == THREAD) {
    _start_reordering_thread = NULL;
    if (JBoltControlThread::get_thread() == NULL) {
      assert(can_reorder_now(), "sanity");
      log_info(jbolt)("Starting JBoltControlThread to reorder.");
      JBoltControlThread::start_thread(CHECK_AND_CLEAR);
    }
  }
}

/**
 * The task will be added to the compile queue and be compiled just like other tasks.
 */
CompileTask* JBoltManager::create_a_task_instance(CompileTaskInfo* cti, const methodHandle& method, const methodHandle& hot_method, TRAPS) {
  int osr_bci = cti->osr_bci();
  int comp_level = cti->comp_level();
  CompileTask::CompileReason comp_reason = (CompileTask::CompileReason) cti->comp_reason();
  int hot_count = cti->hot_count();
  bool is_blocking = true;

  // init a task (@see CompileBroker::create_compile_task())
  CompileTask* task = CompileTask::allocate();
  int compile_id = CompileBroker::assign_compile_id(method, osr_bci);
  task->initialize(compile_id, method, osr_bci, comp_level,
                  hot_method, hot_count, comp_reason,
                  is_blocking);
  return task;
}

/**
 * Print the failure reason if something is wrong in recompilation.
 */
bool JBoltManager::check_compiled_result(Method* method, int check_blob_type, TRAPS) {
  CompiledMethod* cm = method->code();
  if (cm == NULL) {
    log_trace(jbolt)("Recompilation failed because of null nmethod. method=%s", method->name_and_sig_as_C_string());
    return false;
  }
  nmethod* nm = cm->as_nmethod_or_null();
  if (nm == NULL) {
    log_trace(jbolt)("Recompilation failed because the code is not a nmethod. method=%s", method->name_and_sig_as_C_string());
    return false;
  }
  int code_blob_type = CodeCache::get_code_blob_type(nm);
  if (code_blob_type != check_blob_type) {
    log_trace(jbolt)("Recompilation failed because the nmethod is not in heap [%s]: it's in [%s]. method=%s",
                        CodeCache::get_code_heap_name(check_blob_type), CodeCache::get_code_heap_name(code_blob_type), method->name_and_sig_as_C_string());
    return false;
  }
  log_trace(jbolt)("Recompilation good: code=%p, size=%d, method=%s, heap=%s.",
                    nm, nm->size(), method->name_and_sig_as_C_string(), CodeCache::get_code_heap_name(check_blob_type));
  return true;
}

/**
 * Create the compile task instance and enqueue into compile queue
 */
bool JBoltManager::enqueue_recompile_task(CompileTaskInfo* cti, const methodHandle& method, const methodHandle& hot_method, TRAPS) {
  CompileTask* task = NULL;
  CompileQueue* queue = CompileBroker::compile_queue(cti->comp_level());
  { MutexLocker locker(MethodCompileQueue_lock, THREAD);
    if (CompileBroker::compilation_is_in_queue(method)) {
      log_trace(jbolt)("JBOLT won't compile as \"compilation is in queue\": method=%s.", method->name_and_sig_as_C_string());
      return false;
    }
 
    task = create_a_task_instance(cti, method, hot_method, CHECK_AND_CLEAR_false);
    if (task == NULL) {
      log_trace(jbolt)("JBOLT won't compile as \"task instance is NULL\": method=%s.", method->name_and_sig_as_C_string());
      return false;
    }
    queue->add(task);
  }
 
  // Same waiting logic as CompileBroker::wait_for_completion().
  { MonitorLocker ml(task->lock(), THREAD);
    while (!task->is_complete() && !CompileBroker::is_compilation_disabled_forever()) {
      ml.wait();
    }
  }
 
  CompileBroker::wait_for_completion(task);
  task = NULL; // freed
  return true;
}
 
/**
 * Recompilation is to move the nmethod to _primary_hot_seg.
 */
bool JBoltManager::recompile_one(CompileTaskInfo* cti, const methodHandle& method, const methodHandle& hot_method, TRAPS) {
  ResourceMark rm(THREAD);

  if (cti->osr_bci() != InvocationEntryBci) {
    log_trace(jbolt)("We don't handle on-stack-replacement nmethods: method=%s.", method->name_and_sig_as_C_string());
    return false;
  }

  if (log_is_enabled(Trace, jbolt)) {
    const char* heap_name = NULL;
    CompiledMethod* cm = method->code();
    if (cm == NULL) heap_name = "<null>";
    else if (!cm->is_nmethod()) heap_name = "<not-nmethod>";
    else heap_name = CodeCache::get_code_heap_name(CodeCache::get_code_blob_type(cm));
    log_trace(jbolt)("Start to recompile & reorder: heap=%s, method=%s.", heap_name, method->name_and_sig_as_C_string());
  }

  // Add a compilation task.
  set_cur_reordering_method(method());
  bool ret = enqueue_recompile_task(cti, method, hot_method, CHECK_AND_CLEAR_false);
  ret = ret && check_compiled_result(method(), primary_hot_seg(), CHECK_AND_CLEAR_false);
  return ret;
}

/**
 * This method is invoked in a new thread JBoltControlThread.
 * Recompiles the methods in the order list one by one (serially) based on the hot order.
 * The methods to recompile were almost all in MethodJBoltTmp, and will in install in
 * MethodJBoltHot after recompilation.
 */
void JBoltManager::reorder_all_methods(TRAPS) {
  guarantee(UseJBolt && reorder_phase_is_reordering(), "sanity");
  log_info(jbolt)("Start to reorder!");
  print_code_heaps();

  ResourceMark rm(THREAD);
  for (int i = 0; i < _hot_methods_sorted->length(); ++i) {
    JBoltMethodKey k = _hot_methods_sorted->at(i);
    JBoltMethodValue* v = _hot_methods_vis->get(k);
    if (v == NULL) continue;
    CompileTaskInfo* cti = v->get_comp_info();
    if (cti == NULL) continue;
    if (!cti->try_select()) continue;

    methodHandle method(THREAD, cti->method());
    methodHandle hot_method(THREAD, cti->hot_method());

    if (!recompile_one(cti, method, hot_method, THREAD) && OrderAccess::load_acquire(&_hot_codecache_full)) {
      // JBolt codecache is full, stop early
      break;
    }
    if (HAS_PENDING_EXCEPTION) {
      Handle ex(THREAD, PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      LogTarget(Warning, jbolt) lt;
      if (lt.is_enabled()) {
        LogStream ls(lt);
        ls.print("Failed to recompile the method: %s.", method->name_and_sig_as_C_string());
        java_lang_Throwable::print(ex(), &ls);
      }
    }
  }

  log_info(jbolt)("JBolt reordering succeeds.");
  print_code_heaps();

}
 
void JBoltManager::clear_structures() {
  delete _sampled_methods_refs;
  _sampled_methods_refs = NULL;
  JBoltCallGraph::deinitialize();
  set_cur_reordering_method(NULL);
  delete _hot_methods_sorted;
  _hot_methods_sorted = NULL;
  delete _hot_methods_vis;
  _hot_methods_vis = NULL;
}

void JBoltManager::print_code_heap(outputStream& ls, CodeHeap* heap, const char* name) {
  for (CodeBlob* cb = (CodeBlob*) heap->first(); cb != NULL; cb = (CodeBlob*) heap->next(cb)) {
    nmethod* nm = cb->as_nmethod_or_null();
    Method* m = nm != NULL ? nm->method() : NULL;
    ls.print_cr("%s %p %d alive=%s, zombie=%s, nmethod=%s, entrant=%s, name=[%s %s %s]",
                                name,
                                cb, cb->size(),
                                B_TF(cb->is_alive()),
                                B_TF(cb->is_zombie()),
                                B_TF(cb->is_nmethod()),
                                nm ? B_TF(!nm->is_not_entrant()) : "?",
                                m ? m->method_holder()->name()->as_C_string() : cb->name(),
                                m ? m->name()->as_C_string() : NULL,
                                m ? m->signature()->as_C_string() : NULL);
  }
}

void JBoltManager::print_code_heaps() {
  {
    LogTarget(Debug, jbolt) lt;
    if (!lt.is_enabled()) return;
    LogStream ls(lt);
    MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    CodeCache::print_summary(&ls, true);
  }

  {
    LogTarget(Trace, jbolt) lt;
    if (!lt.is_enabled()) return;
    LogStream ls(lt);
    CodeHeap* hot_heap = CodeCache::get_code_heap(CodeBlobType::MethodJBoltHot);
    CodeHeap* tmp_heap = CodeCache::get_code_heap(CodeBlobType::MethodJBoltTmp);

    ResourceMark rm;
    if (hot_heap == NULL) {
      ls.print_cr("The jbolt hot heap is null.");
    } else {
      print_code_heap(ls, hot_heap, "hot");
    }
    if (tmp_heap == NULL) {
      ls.print_cr("The jbolt tmp heap is null.");
    } else {
      print_code_heap(ls, tmp_heap, "tmp");
    }
  }
}

void JBoltManager::dump_nmethod_count(fileStream& file, nmethod* nm, CodeBlob* cb) {
  int hotcount = 0;
  if (cb->is_alive() && !nm->is_not_entrant() && _sampled_methods_hotcount_stored->get(nm->method()) != NULL) {
      hotcount = *(_sampled_methods_hotcount_stored->get(nm->method()));
  }
  file.print_cr("  sample count: %d", hotcount);
}

void JBoltManager::dump_code_heap_with_count(const char* filename, CodeHeap* heap) {
  if (heap == NULL) return;

  fileStream invocation_count_file(filename, "w+");
  uint64_t total = 0;
  if (invocation_count_file.is_open()) {
    invocation_count_file.print("%s:", heap->name());
    invocation_count_file.print_cr(" size=" SIZE_FORMAT "Kb used=" SIZE_FORMAT
                  "Kb max_used=" SIZE_FORMAT "Kb free=" SIZE_FORMAT "Kb",
                  (size_t)(heap->high_boundary() - heap->low_boundary())/K, (size_t)(heap->high_boundary() - heap->low_boundary() - heap->unallocated_capacity())/K,
                  heap->max_allocated_capacity()/K, heap->unallocated_capacity()/K);
    invocation_count_file.print_cr(" bounds [" INTPTR_FORMAT ", " INTPTR_FORMAT ", " INTPTR_FORMAT "]",
                    p2i(heap->low_boundary()),
                    p2i(heap->high()),
                    p2i(heap->high_boundary()));
    for (CodeBlob* cb = (CodeBlob*) heap->first(); cb != NULL; cb = (CodeBlob*) heap->next(cb)) {
      nmethod* nm = cb->as_nmethod_or_null();
      invocation_count_file.print_cr("###%lu %s size=%dB %p %p %p state=%d name=%s alive=%s nmethod=%s use=%s entrant=%s zombie=%s level=%d code=%p",
                                  total++,
                                  "np",
                                  cb->size(),
                                  cb, cb->code_begin(), cb->data_end(),
                                  nm ? nm->get_state() : ILL_NM_STATE,
                                  (nm && nm->method()) ? nm->method()->name_and_sig_as_C_string() : "NULL",
                                  B_TF(cb->is_alive()),
                                  B_TF(cb->is_nmethod()),
                                  nm ? B_TF(nm->is_in_use()) : "?",
                                  nm ? B_TF(!nm->is_not_entrant()) : "?",
                                  nm ? B_TF(nm->is_zombie()) : "?",
                                  nm ? nm->comp_level() : -1,
                                  (nm && nm->method()) ? nm->method()->code() : 0);
      if (nm && nm->method()) {
        dump_nmethod_count(invocation_count_file, nm, cb);
      }
    }
  }
  else {
    log_info(jbolt)("%s open error\n", filename);
  }
}

void JBoltManager::dump_code_heaps_with_count() {
  if (!EnableDumpGraph) {
    ShouldNotReachHere();
    return;
  }

  MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
  CodeHeap* np_heap = CodeCache::get_code_heap(CodeBlobType::MethodNonProfiled);
  CodeHeap* hot_heap = (UseJBolt && !JBoltDumpMode) ? CodeCache::get_code_heap(CodeBlobType::MethodJBoltHot) : NULL;
  CodeHeap* tmp_heap = (UseJBolt && !JBoltDumpMode) ? CodeCache::get_code_heap(CodeBlobType::MethodJBoltTmp) : NULL;
 
  ResourceMark rm;
  time_t current_time;
  struct tm p;
  char oldpath[PATH_LENGTH];
  char dirname[PATH_LENGTH];

  time(&current_time);
  localtime_r(&current_time, &p);
  sprintf(dirname, "JBOLT.%d.%d.%d.%02d:%02d:%02d",1900+p.tm_year,1+p.tm_mon,p.tm_mday,p.tm_hour,p.tm_min,p.tm_sec);  
  
  mkdir(dirname, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
  if (getcwd(oldpath, PATH_LENGTH) != NULL) {
    if (chdir(dirname) == OS_ERR) {
        warning("Can't change to directory %s", dirname);
        return;
    }
    dump_code_heap_with_count("count_np.txt", np_heap);
    dump_code_heap_with_count("count_hot.txt", hot_heap);
    dump_code_heap_with_count("count_tmp.txt", tmp_heap);
    if (chdir(oldpath) == OS_ERR) {
        warning("Can't change to directory %s", oldpath);
    }
  }
}

#undef B_TF