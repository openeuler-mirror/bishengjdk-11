/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_RUNTIME_VIRTUALSPACE_HPP
#define SHARE_VM_RUNTIME_VIRTUALSPACE_HPP

#include "utilities/globalDefinitions.hpp"

// ReservedSpace is a data structure for reserving a contiguous address range.

class ReservedSpace {
  friend class VMStructs;
 protected:
  char*  _base;
  size_t _size;
  size_t _noaccess_prefix;
  size_t _alignment;
  bool   _special;
  int    _fd_for_heap;
 private:
  bool   _executable;

  // ReservedSpace
  ReservedSpace(char* base, size_t size, size_t alignment, bool special,
                bool executable);
 protected:
  void initialize(size_t size, size_t alignment, bool large,
                  char* requested_address,
                  bool executable);

 public:
  // Constructor
  ReservedSpace();
  // Initialize the reserved space with the given size. If preferred_page_size
  // is set, use this as minimum page size/alignment. This may waste some space
  // if the given size is not aligned to that value, as the reservation will be
  // aligned up to the final alignment in this case.
  ReservedSpace(size_t size, size_t preferred_page_size = 0);
  ReservedSpace(size_t size, size_t alignment, bool large,
                char* requested_address = NULL);
  ReservedSpace(size_t size, size_t alignment, bool large, bool executable);

  // Accessors
  char*  base()            const { return _base;      }
  size_t size()            const { return _size;      }
  char*  end()             const { return _base + _size; }
  size_t alignment()       const { return _alignment; }
  bool   special()         const { return _special;   }
  bool   executable()      const { return _executable;   }
  size_t noaccess_prefix() const { return _noaccess_prefix;   }
  bool is_reserved()       const { return _base != NULL; }
  void release();

  // Splitting
  ReservedSpace first_part(size_t partition_size, size_t alignment,
                           bool split = false, bool realloc = true);
  ReservedSpace last_part (size_t partition_size, size_t alignment);

  // These simply call the above using the default alignment.
  inline ReservedSpace first_part(size_t partition_size,
                                  bool split = false, bool realloc = true);
  inline ReservedSpace last_part (size_t partition_size);

#if INCLUDE_JBOLT
  static ReservedSpace static_first_part(ReservedSpace rs, size_t partition_size) {
    return rs.first_part(partition_size);
  }

  static ReservedSpace static_last_part(ReservedSpace rs, size_t partition_size) {
    return rs.last_part(partition_size);
  }
#endif

  // Alignment
  static size_t page_align_size_up(size_t size);
  static size_t page_align_size_down(size_t size);
  static size_t allocation_align_size_up(size_t size);
  static size_t allocation_align_size_down(size_t size);
  bool contains(const void* p) const {
    return (base() <= ((char*)p)) && (((char*)p) < (base() + size()));
  }
};

ReservedSpace
ReservedSpace::first_part(size_t partition_size, bool split, bool realloc)
{
  return first_part(partition_size, alignment(), split, realloc);
}

ReservedSpace ReservedSpace::last_part(size_t partition_size)
{
  return last_part(partition_size, alignment());
}

// Class encapsulating behavior specific of memory space reserved for Java heap.
class ReservedHeapSpace : public ReservedSpace {
 private:
  void try_reserve_heap(size_t size, size_t alignment, bool large,
                        char *requested_address);
  void try_reserve_range(char *highest_start, char *lowest_start,
                         size_t attach_point_alignment, char *aligned_HBMA,
                         char *upper_bound, size_t size, size_t alignment, bool large);
  void initialize_compressed_heap(const size_t size, size_t alignment, bool large);
  // Create protection page at the beginning of the space.
  void establish_noaccess_prefix();
 public:
  // Constructor. Tries to find a heap that is good for compressed oops.
  // heap_allocation_directory is the path to the backing memory for Java heap. When set, Java heap will be allocated
  // on the device which is managed by the file system where the directory resides.
  ReservedHeapSpace(size_t size, size_t forced_base_alignment, bool large, const char* heap_allocation_directory = NULL);
  // Returns the base to be used for compression, i.e. so that null can be
  // encoded safely and implicit null checks can work.
  char *compressed_oop_base() { return _base - _noaccess_prefix; }
};

// Class encapsulating behavior specific memory space for Code
class ReservedCodeSpace : public ReservedSpace {
 public:
  // Constructor
  ReservedCodeSpace(size_t r_size, size_t rs_align, bool large);
};

// VirtualSpace is data structure for committing a previously reserved address range in smaller chunks.

class VirtualSpace {
  friend class VMStructs;
 private:
  // Reserved area
  char* _low_boundary;
  char* _high_boundary;

  // Committed area
  char* _low;
  char* _high;

  // The entire space has been committed and pinned in memory, no
  // os::commit_memory() or os::uncommit_memory().
  bool _special;

  // Need to know if commit should be executable.
  bool   _executable;

  // MPSS Support
  // Each virtualspace region has a lower, middle, and upper region.
  // Each region has an end boundary and a high pointer which is the
  // high water mark for the last allocated byte.
  // The lower and upper unaligned to LargePageSizeInBytes uses default page.
  // size.  The middle region uses large page size.
  char* _lower_high;
  char* _middle_high;
  char* _upper_high;

  char* _lower_high_boundary;
  char* _middle_high_boundary;
  char* _upper_high_boundary;

  size_t _lower_alignment;
  size_t _middle_alignment;
  size_t _upper_alignment;

  // MPSS Accessors
  char* lower_high() const { return _lower_high; }
  char* middle_high() const { return _middle_high; }
  char* upper_high() const { return _upper_high; }

  char* lower_high_boundary() const { return _lower_high_boundary; }
  char* middle_high_boundary() const { return _middle_high_boundary; }
  char* upper_high_boundary() const { return _upper_high_boundary; }

  size_t lower_alignment() const { return _lower_alignment; }
  size_t middle_alignment() const { return _middle_alignment; }
  size_t upper_alignment() const { return _upper_alignment; }

 public:
  // Committed area
  char* low()  const { return _low; }
  char* high() const { return _high; }

  // Reserved area
  char* low_boundary()  const { return _low_boundary; }
  char* high_boundary() const { return _high_boundary; }

#if INCLUDE_AOT
  // Set boundaries for code section in AOT library.
  void set_low_boundary(char *p)  { _low_boundary = p; }
  void set_high_boundary(char *p) { _high_boundary = p; }
  void set_low(char *p)           { _low = p; }
  void set_high(char *p)          { _high = p; }
#endif

  bool special() const { return _special; }

 public:
  // Initialization
  VirtualSpace();
  bool initialize_with_granularity(ReservedSpace rs, size_t committed_byte_size, size_t max_commit_ganularity);
  bool initialize(ReservedSpace rs, size_t committed_byte_size);

  // Destruction
  ~VirtualSpace();

  // Reserved memory
  size_t reserved_size() const;
  // Actually committed OS memory
  size_t actual_committed_size() const;
  // Memory used/expanded in this virtual space
  size_t committed_size() const;
  // Memory left to use/expand in this virtual space
  size_t uncommitted_size() const;

  bool   contains(const void* p) const;

  // Operations
  // returns true on success, false otherwise
  bool expand_by(size_t bytes, bool pre_touch = false);
  void shrink_by(size_t bytes);
  void release();

  void check_for_contiguity() PRODUCT_RETURN;

  // Debugging
  void print_on(outputStream* out) PRODUCT_RETURN;
  void print();
};

#endif // SHARE_VM_RUNTIME_VIRTUALSPACE_HPP
