/*
 * Copyright (c) 2003, 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
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
#include "memory/metaspaceShared.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/thread.inline.hpp"

frame JavaThread::pd_last_frame() {
  assert(has_last_Java_frame(), "must have last_Java_sp() when suspended");
  return frame(_anchor.last_Java_sp(), _anchor.last_Java_fp(), _anchor.last_Java_pc());
}

// For Forte Analyzer AsyncGetCallTrace profiling support - thread is
// currently interrupted by SIGPROF
bool JavaThread::pd_get_top_frame_for_signal_handler(frame* fr_addr,
  void* ucontext, bool isInJava) {

  assert(Thread::current() == this, "caller must be current thread");
  return pd_get_top_frame(fr_addr, ucontext, isInJava);
}

bool JavaThread::pd_get_top_frame_for_profiling(frame* fr_addr, void* ucontext, bool isInJava) {
  return pd_get_top_frame(fr_addr, ucontext, isInJava);
}

inline unsigned int stringHash(const char* str) {
    unsigned int seed = 13;
    unsigned int hash = 0;
    while(*str) {
        hash = hash * seed + (*str++);
    }

    return (hash & 0x7fffffff);
}

void JavaThread::os_linux_aarch64_options(int apc, char **name) {
  if (name == NULL) {
    return;
  }
  VM_Version::get_cpu_model();
  if (VM_Version::is_hisi_enabled()) {
    int i = 0;
    int step = 0;
    while (name[i] != NULL) {
      if (stringHash(name[i]) == 1396789436) {
        if (FLAG_IS_DEFAULT(ActiveProcessorCount) && (UseG1GC || UseParallelGC || UseZGC) && apc > 8)
          FLAG_SET_DEFAULT(ActiveProcessorCount, 8);
        break;
      }
      i++;
    }
  }
}

bool JavaThread::pd_get_top_frame(frame* fr_addr, void* ucontext, bool isInJava) {
  assert(this->is_Java_thread(), "must be JavaThread");
  JavaThread* jt = (JavaThread *)this;

  // If we have a last_Java_frame, then we should use it even if
  // isInJava == true.  It should be more reliable than ucontext info.
  if (jt->has_last_Java_frame() && jt->frame_anchor()->walkable()) {
    *fr_addr = jt->pd_last_frame();
    return true;
  }

  // At this point, we don't have a last_Java_frame, so
  // we try to glean some information out of the ucontext
  // if we were running Java code when SIGPROF came in.
  if (isInJava) {
    ucontext_t* uc = (ucontext_t*) ucontext;

    intptr_t* ret_fp;
    intptr_t* ret_sp;
    ExtendedPC addr = os::Linux::fetch_frame_from_ucontext(this, uc,
      &ret_sp, &ret_fp);
    if (addr.pc() == NULL || ret_sp == NULL ) {
      // ucontext wasn't useful
      return false;
    }

    if (MetaspaceShared::is_in_trampoline_frame(addr.pc())) {
      // In the middle of a trampoline call. Bail out for safety.
      // This happens rarely so shouldn't affect profiling.
      return false;
    }

    frame ret_frame(ret_sp, ret_fp, addr.pc());
    if (!ret_frame.safe_for_sender(jt)) {
#ifdef COMPILER2
      frame ret_frame2(ret_sp, NULL, addr.pc());
      if (!ret_frame2.safe_for_sender(jt)) {
        // nothing else to try if the frame isn't good
        return false;
      }
      ret_frame = ret_frame2;
#else
      // nothing else to try if the frame isn't good
      return false;
#endif /* COMPILER2 */
    }
    *fr_addr = ret_frame;
    return true;
  }

  // nothing else to try
  return false;
}

void JavaThread::cache_global_variables() { }

