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
#include <time.h>

#include "classfile/javaClasses.inline.hpp"
#include "classfile/vmSymbols.hpp"
#include "jbolt/jBoltControlThread.hpp"
#include "jbolt/jBoltManager.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "runtime/atomic.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/sweeper.hpp"
 
JavaThread* volatile JBoltControlThread::_the_java_thread = NULL;
Monitor* JBoltControlThread::_control_wait_monitor = NULL;
Monitor* JBoltControlThread::_sample_wait_monitor = NULL;
jobject JBoltControlThread::_thread_obj = NULL;
int volatile JBoltControlThread::_signal = JBoltControlThread::SIG_NULL;
bool volatile JBoltControlThread::_abort = false;
intx volatile JBoltControlThread::_interval = 0;
 
static bool not_first = false;

void JBoltControlThread::init(TRAPS) {
  Handle string = java_lang_String::create_from_str("JBolt Control", CATCH);
  Handle thread_group(THREAD, Universe::system_thread_group());
  Handle thread_oop = JavaCalls::construct_new_instance(
          SystemDictionary::Thread_klass(),
          vmSymbols::threadgroup_string_void_signature(),
          thread_group,
          string,
          CATCH);
  _thread_obj = JNIHandles::make_global(thread_oop);
  _control_wait_monitor = new Monitor(Mutex::nonleaf, "JBoltControlMonitor");
  _sample_wait_monitor = new Monitor(Mutex::nonleaf, "JBoltSampleMonitor");
  OrderAccess::release_store(&_interval, JBoltSampleInterval);
}
 
void JBoltControlThread::start_thread(TRAPS) {
  guarantee(OrderAccess::load_acquire(&_the_java_thread) == NULL, "sanity");
  JavaThread* new_thread = new JavaThread(&thread_entry);
  if (new_thread->osthread() == NULL) {
    fatal("Failed to create JBoltControlThread as no os thread!");
    return;
  }
 
  Handle thread_oop(THREAD, JNIHandles::resolve_non_null(_thread_obj));
  {
    MutexLocker mu(Threads_lock, THREAD);
    java_lang_Thread::set_thread(thread_oop(), new_thread);
    java_lang_Thread::set_priority(thread_oop(), MinPriority);
    java_lang_Thread::set_daemon(thread_oop());
    new_thread->set_threadObj(thread_oop());
    Threads::add(new_thread);
    Thread::start(new_thread);
  }
  guarantee(Atomic::cmpxchg(new_thread, &_the_java_thread, (JavaThread*) NULL) == NULL, "sanity");
}

intx JBoltControlThread::sample_interval() {
  return OrderAccess::load_acquire(&_interval);
}
 
// Work to do before restarting a control schedule, twice and after only
bool JBoltControlThread::prev_control_schdule(TRAPS) {
  guarantee(JBoltManager::auto_mode(), "sanity");
  // Clear obsolete data structures
  if (JBoltManager::clear_last_sample_datas() != 0) {
    log_error(jbolt)("Something wrong happened in data clean, not going on...");
    return false;
  }
 
  // Restart JFR
  bufferedStream output;
  DCmd::parse_and_execute(DCmd_Source_Internal, &output, "JFR.start name=jbolt-jfr", ' ', THREAD);
  if (HAS_PENDING_EXCEPTION) {
    ResourceMark rm;
    log_warning(jbolt)("unable to start jfr jbolt-jfr");
    log_warning(jbolt)("exception type: %s", PENDING_EXCEPTION->klass()->external_name());
    // don't unwind this exception
    CLEAR_PENDING_EXCEPTION;
  }
 
  return true;
}
 
void JBoltControlThread::control_schdule(TRAPS) {
  guarantee(JBoltManager::auto_mode(), "sanity");

  { MonitorLocker locker(_sample_wait_monitor);
    // Perform time wait
    log_info(jbolt)("JBolt Starting Sample for %lds!!!", sample_interval());
    const jlong interval = (jlong) sample_interval();
    jlong cur_time = os::javaTimeMillis();
    const jlong end_time = cur_time + (interval * 1000);
    while ((end_time > cur_time) && OrderAccess::load_acquire(&_signal) != SIG_STOP_PROFILING) {
      int64_t timeout = (int64_t) (end_time - cur_time);
      locker.wait(timeout);
      cur_time = os::javaTimeMillis();
    }
  }
  // Close JFR
  guarantee(JBoltManager::reorder_phase_profiling_to_waiting(), "sanity");
  bufferedStream output;
  DCmd::parse_and_execute(DCmd_Source_Internal, &output, "JFR.stop name=jbolt-jfr", ' ', THREAD);
  if (HAS_PENDING_EXCEPTION) {
    ResourceMark rm;
    // JFR.stop maybe failed if a jfr recording is already stopped
    // but it's nothing worry, jbolt should continue to work normally
    log_warning(jbolt)("unable to stop jfr jbolt-jfr");
    log_warning(jbolt)("exception type: %s", PENDING_EXCEPTION->klass()->external_name());
    // don't unwind this exception
    CLEAR_PENDING_EXCEPTION;
  }
  if (Atomic::cmpxchg(false, &_abort, true) == /* should abort */ true) {
    return;
  }
 
  size_t total_nmethod_size = 0;
  // Init structures for load phase
  JBoltManager::init_auto_transition(&total_nmethod_size, CATCH);
 
  if (total_nmethod_size > JBoltCodeHeapSize) {
    log_warning(jbolt)("JBolt reordering not complete because JBolt CodeHeap is too small to place all ordered methods. Please use -XX:JBoltCodeHeapSize to enlarge");
    log_warning(jbolt)("JBoltCodeHeapSize=" UINTX_FORMAT " B ( need " UINTX_FORMAT " B).", JBoltCodeHeapSize, total_nmethod_size);
  }

  if (not_first) {
    // Exchange Hot Segment primary and secondary relationships
    JBoltManager::swap_semi_jbolt_segs();
  }

  if (!not_first && EnableDumpGraph) {
    // When EnableDumpGraph, dump initial code heaps for compared
    JBoltManager::dump_code_heaps_with_count();
  }
 
  guarantee(JBoltManager::reorder_phase_waiting_to_reordering(), "sanity");
  OrderAccess::release_store(&_signal, SIG_NULL);
 
  // Start reorder
  JBoltManager::reorder_all_methods(CATCH);
}
 
// Work to do after reordering, twice and after only
void JBoltControlThread::post_control_schdule(TRAPS) {
  JBoltManager::clear_secondary_hot_seg(THREAD);
}

struct tm JBoltControlThread::next_trigger_time(struct tm* localtime) {
  struct tm target_tm = *localtime;
  GrowableArray<char*>* rescheduling_time = JBoltManager::rescheduling_time();
  for (int i = 0; i < rescheduling_time->length(); ++i) {
    char* target_time = rescheduling_time->at(i);
    int target_hour = (target_time[0] - '0') * 10 + (target_time[1] - '0');
    int target_minute = (target_time[3] - '0') * 10 + (target_time[4] - '0');
    if (target_hour > localtime->tm_hour || (target_hour == localtime->tm_hour && target_minute > localtime->tm_min)) {
      target_tm.tm_hour = target_hour;
      target_tm.tm_min = target_minute;
      target_tm.tm_sec = 0;
      break;
    }
    if (i == rescheduling_time->length() - 1) {
      target_time = rescheduling_time->at(0);
      target_hour = (target_time[0] - '0') * 10 + (target_time[1] - '0');
      target_minute = (target_time[3] - '0') * 10 + (target_time[4] - '0');
      target_tm.tm_mday += 1;
      target_tm.tm_hour = target_hour;
      target_tm.tm_min = target_minute;
      target_tm.tm_sec = 0;
      mktime(&target_tm);
    }
  }

  return target_tm;
}

void JBoltControlThread::wait_for_next_trigger(TRAPS) {
  MonitorLocker locker(_control_wait_monitor);
  time_t current_time;
  struct tm p;
  time(&current_time);
  localtime_r(&current_time, &p);
  if (JBoltManager::rescheduling_time() != NULL && JBoltManager::rescheduling_time()->length() > 0) {
    struct tm target_tm = next_trigger_time(&p);
    log_info(jbolt)("next trigger is at %d.%d.%d.%02d:%02d:%02d",1900+target_tm.tm_year,1+target_tm.tm_mon,target_tm.tm_mday,target_tm.tm_hour,target_tm.tm_min,target_tm.tm_sec);
    while (OrderAccess::load_acquire(&_signal) != SIG_START_PROFILING) {
      long time_wait = mktime(&target_tm) - current_time;
      if (time_wait <= 0) {
        log_info(jbolt)("successfully trigger at %02d:%02d",target_tm.tm_hour,target_tm.tm_min);
        break;
      }
      locker.wait(time_wait * 1000);
      time(&current_time);
    }
  }
  else {
    while (OrderAccess::load_acquire(&_signal) != SIG_START_PROFILING) {
      locker.wait(60 * 1000);
    }
  }
}

void JBoltControlThread::thread_run_auto_loop(TRAPS) {
  do {
    OrderAccess::release_store(&_signal, SIG_NULL);
    if (not_first && !prev_control_schdule(THREAD)) continue;
    guarantee(JBoltManager::reorder_phase_available_to_profiling(), "sanity");
    control_schdule(THREAD);
    if (!JBoltManager::reorder_phase_reordering_to_available()) {
      // abort logic 
      guarantee(JBoltManager::reorder_phase_waiting_to_available(), "sanity");
      guarantee(Atomic::cmpxchg(SIG_NULL, &_signal, SIG_STOP_PROFILING) == SIG_STOP_PROFILING, "sanity");
    }
    else if (not_first) {
      post_control_schdule(THREAD);
    }
    not_first = true;
    wait_for_next_trigger(THREAD);
    JBoltManager::clear_structures();
  } while(true);
}

void JBoltControlThread::thread_run(TRAPS) {
  if (JBoltManager::auto_mode()) {
    thread_run_auto_loop(THREAD);
  } else {
    guarantee(JBoltManager::can_reorder_now(), "sanity");
    guarantee(JBoltManager::reorder_phase_collecting_to_reordering(), "sanity");
    JBoltManager::reorder_all_methods(CATCH);
    JBoltManager::clear_structures();
    guarantee(JBoltManager::reorder_phase_reordering_to_end(), "sanity");
    assert(JBoltLoadMode, "Only manual JBoltLoadMode can reach here");
  }
}

bool JBoltControlThread::notify_sample_wait(bool abort) {
  int old_sig = Atomic::cmpxchg(SIG_STOP_PROFILING, &_signal, SIG_NULL);
  if (old_sig == SIG_NULL) {
    MonitorLocker locker(_sample_wait_monitor);
    // abort implementation maybe not in order in extreme cases
    // add fence? or delete abort() if not so useful.
    OrderAccess::release_store(&_abort, abort);
    locker.notify();
    return true;
  }
  return false;
}
 
bool JBoltControlThread::notify_control_wait(intx interval) {
  int old_sig = Atomic::cmpxchg(SIG_START_PROFILING, &_signal, SIG_NULL);
  if (old_sig == SIG_NULL) {
    // this lock will be grabbed by ControlThread until it's waiting
    MonitorLocker locker(_control_wait_monitor);
    OrderAccess::release_store(&_interval, interval);
    locker.notify();
    return true;
  }
  return false;
}

JavaThread* JBoltControlThread::get_thread() {
  return OrderAccess::load_acquire(&_the_java_thread);
}