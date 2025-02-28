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

#ifndef SHARE_JBOLT_JBOLT_GLOBALS_HPP
#define SHARE_JBOLT_JBOLT_GLOBALS_HPP

#include "runtime/globals.hpp"

#define JBOLT_FLAGS(develop,                                                \
                       develop_pd,                                          \
                       product,                                             \
                       product_pd,                                          \
                       diagnostic,                                          \
                       diagnostic_pd,                                       \
                       experimental,                                        \
                       notproduct,                                          \
                       range,                                               \
                       constraint)                                          \
                                                                            \
  experimental(bool, UseJBolt, false,                                       \
          "Enable JBolt feature.")                                          \
                                                                            \
  experimental(bool, JBoltDumpMode, false,                                  \
          "Trial run of JBolt. Collect profiling and dump it.")             \
                                                                            \
  experimental(bool, JBoltLoadMode, false,                                  \
          "Second run of JBolt. Load the profiling and reorder nmethods.")  \
                                                                            \
  experimental(intx, JBoltSampleInterval, 600,                              \
          "Sample interval(second) of JBolt dump mode"                      \
          "only useful in auto mode.")                                      \
          range(0, 36000)                                                   \
                                                                            \
  experimental(ccstr, JBoltOrderFile, NULL,                                 \
          "The JBolt method order file to dump or load.")                   \
                                                                            \
  diagnostic(double, JBoltReorderThreshold, 0.86,                           \
          "The threshold to trigger JBolt reorder in load mode.")           \
          range(0.1, 0.9)                                                   \
                                                                            \
  experimental(uintx, JBoltCodeHeapSize, 8*M ,                              \
          "Code heap size of MethodJBoltHot and MethodJBoltTmp heaps.")     \
                                                                            \
  experimental(ccstr, JBoltRescheduling, NULL,                              \
          "Trigger rescheduling at a fixed time every day.")                \
                                                                            \
  diagnostic(bool, EnableDumpGraph, false,                                  \
          "Enable JBolt.dumpgraph to produce source data files")            \
                                                                            \

// end of JBOLT_FLAGS

JBOLT_FLAGS(DECLARE_DEVELOPER_FLAG,    \
               DECLARE_PD_DEVELOPER_FLAG, \
               DECLARE_PRODUCT_FLAG,      \
               DECLARE_PD_PRODUCT_FLAG,   \
               DECLARE_DIAGNOSTIC_FLAG, \
               DECLARE_PD_DIAGNOSTIC_FLAG, \
               DECLARE_EXPERIMENTAL_FLAG, \
               DECLARE_NOTPRODUCT_FLAG,   \
               IGNORE_RANGE,              \
               IGNORE_CONSTRAINT)

#endif // SHARE_JBOLT_JBOLT_GLOBALS_HPP
