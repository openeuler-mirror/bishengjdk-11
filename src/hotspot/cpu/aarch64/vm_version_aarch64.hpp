/*
 * Copyright (c) 1997, 2016, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_AARCH64_VM_VM_VERSION_AARCH64_HPP
#define CPU_AARCH64_VM_VM_VERSION_AARCH64_HPP

#include "runtime/abstract_vm_version.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/sizes.hpp"

class VM_Version : public Abstract_VM_Version {
  friend class JVMCIVMStructs;

protected:
  static int _cpu;
  static int _model;
  static int _model2;
  static int _variant;
  static int _revision;
  static int _stepping;
  static int _initial_sve_vector_length;

  static int _zva_length;
  static int _dcache_line_size;
  static int _icache_line_size;

  // Read additional info using OS-specific interfaces
  static void get_os_cpu_info();


  // Sets the SVE length and returns a new actual value or negative on error.
  // If the len is larger than the system largest supported SVE vector length,
  // the function sets the largest supported value.
  static int set_and_get_current_sve_vector_length(int len);
  static int get_current_sve_vector_length();

public:
  // Initialization
  static void initialize();

  // Asserts
  static void assert_is_initialized() {
  }

  static bool expensive_load(int ld_size, int scale) {
    if (cpu_family() == CPU_ARM) {
      // Half-word load with index shift by 1 (aka scale is 2) has
      // extra cycle latency, e.g. ldrsh w0, [x1,w2,sxtw #1].
      if (ld_size == 2 && scale == 2) {
        return true;
      }
    }
    return false;
  }

  // Arm can assign codes that are not published in the manual.
  // Apple's code is defined in
  // https://github.com/apple/darwin-xnu/blob/33eb983/osfmk/arm/cpuid.h#L62
  enum Family {
    CPU_ARM       = 'A',
    CPU_BROADCOM  = 'B',
    CPU_CAVIUM    = 'C',
    CPU_DEC       = 'D',
    CPU_HISILICON = 'H',
    CPU_INFINEON  = 'I',
    CPU_MOTOROLA  = 'M',
    CPU_NVIDIA    = 'N',
    CPU_AMCC      = 'P',
    CPU_QUALCOM   = 'Q',
    CPU_MARVELL   = 'V',
    CPU_INTEL     = 'i',
    CPU_APPLE     = 'a',
  };

  enum Feature_Flag {
    CPU_FP           = (1<<0),
    CPU_ASIMD        = (1<<1),
    CPU_EVTSTRM      = (1<<2),
    CPU_AES          = (1<<3),
    CPU_PMULL        = (1<<4),
    CPU_SHA1         = (1<<5),
    CPU_SHA2         = (1<<6),
    CPU_CRC32        = (1<<7),
    CPU_LSE          = (1<<8),
    CPU_SVE          = (1<<22),
    CPU_SVE2         = (1<<28),
    // flags above must follow Linux HWCAP
    CPU_STXR_PREFETCH= (1 << 29),
    CPU_A53MAC       = (1 << 30),
    CPU_DMB_ATOMICS  = (1 << 31),
  };

  static int get_cpu_model();
  static int cpu_family()                     { return _cpu; }
  static int cpu_model()                      { return _model; }
  static int cpu_model2()                     { return _model2; }
  static int cpu_variant()                    { return _variant; }
  static int cpu_revision()                   { return _revision; }
  static int get_initial_sve_vector_length()  { return _initial_sve_vector_length; };

  static bool is_hisi_enabled() {
    if (_cpu == CPU_HISILICON && (_model == 0xd01 || _model == 0xd02 || _model == 0xd03 ||
                           _model == 0xd22 || _model == 0xd45)) {
      return true;
    }
    return false;
  }

  static bool is_zva_enabled() { return 0 <= _zva_length; }
  static int zva_length() {
    assert(is_zva_enabled(), "ZVA not available");
    return _zva_length;
  }

  static int icache_line_size() { return _icache_line_size; }
  static int dcache_line_size() { return _dcache_line_size; }

#ifdef __APPLE__
  // Is the CPU running emulated (for example macOS Rosetta running x86_64 code on M1 ARM (aarch64)
  static bool is_cpu_emulated();
#endif
};

#endif // CPU_AARCH64_VM_VM_VERSION_AARCH64_HPP
