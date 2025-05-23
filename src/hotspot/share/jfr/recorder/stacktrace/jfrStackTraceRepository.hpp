/*
 * Copyright (c) 2011, 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_JFR_RECORDER_STACKTRACE_JFRSTACKTRACEREPOSITORY_HPP
#define SHARE_JFR_RECORDER_STACKTRACE_JFRSTACKTRACEREPOSITORY_HPP

#include "jfr/recorder/stacktrace/jfrStackTrace.hpp"
#include "jfr/utilities/jfrAllocation.hpp"
#include "jfr/utilities/jfrTypes.hpp"

class JavaThread;
class JfrCheckpointWriter;
class JfrChunkWriter;

class JfrStackTraceRepository : public JfrCHeapObj {
  friend class JfrRecorder;
  friend class JfrRecorderService;
  friend class JfrThreadSampleClosure;
  friend class ObjectSampleCheckpoint;
  friend class ObjectSampler;
  friend class RecordStackTrace;
  friend class StackTraceBlobInstaller;
  friend class WriteStackTraceRepository;
#if INCLUDE_JBOLT
  friend class JBoltManager;
#endif

 private:
  static const u4 TABLE_SIZE = 2053;
  JfrStackTrace* _table[TABLE_SIZE];
  u4 _last_entries;
  u4 _entries;

#if INCLUDE_JBOLT
  // [jbolt]: an exclusive table for jbolt. It should be a subset of _table
  JfrStackTrace* _table_jbolt[TABLE_SIZE];
  u4 _last_entries_jbolt;
  u4 _entries_jbolt;  
  
  static size_t clear_jbolt();
  static size_t clear_jbolt(JfrStackTraceRepository& repo);  
  traceid add_trace_jbolt(const JfrStackTrace& stacktrace);
  static traceid add_jbolt(JfrStackTraceRepository& repo, const JfrStackTrace& stacktrace);
  static traceid add_jbolt(const JfrStackTrace& stacktrace);
#endif

  JfrStackTraceRepository();
  static JfrStackTraceRepository& instance();
  static JfrStackTraceRepository* create();
  bool initialize();
  static void destroy();

  bool is_modified() const;
  static size_t clear();
  static size_t clear(JfrStackTraceRepository& repo);
  size_t write(JfrChunkWriter& cw, bool clear);

  static const JfrStackTrace* lookup_for_leak_profiler(unsigned int hash, traceid id);
  static void record_for_leak_profiler(JavaThread* thread, int skip = 0);
  static void clear_leak_profiler();

  traceid add_trace(const JfrStackTrace& stacktrace);
  static traceid add(JfrStackTraceRepository& repo, const JfrStackTrace& stacktrace);
  static traceid add(const JfrStackTrace& stacktrace);
  traceid record_for(JavaThread* thread, int skip, JfrStackFrame* frames, u4 max_frames);

 public:
  static traceid record(Thread* thread, int skip = 0);
#if INCLUDE_JBOLT  
  const JfrStackTrace* const * get_stacktrace_table() const { return _table; }
  u4 get_entries_count() const { return _entries; }
  
  const JfrStackTrace* const * get_stacktrace_table_jbolt() const { return _table_jbolt; }
  u4 get_entries_count_jbolt() const { return _entries_jbolt; }
#endif
};

#endif // SHARE_JFR_RECORDER_STACKTRACE_JFRSTACKTRACEREPOSITORY_HPP
