/**
 * Cpu abstraction with inline asm.
 *
 * Copyright (C) 2007-2010, Bernhard Kauer <bk@vmmon.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of Vancouver and was developed for Vancouver.
 *
 * Vancouver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Vancouver is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#pragma once
#include "service/math.h"

class Cpu
{
 public:
  static  void  pause() { asm volatile("pause"); }

  template <typename T> static  void  atomic_and(T *ptr, T value) { __sync_and_and_fetch(ptr, value); }
  template <typename T> static  void  atomic_or(T *ptr, T value)  { __sync_or_and_fetch(ptr, value); }

  static void atomic_set_bit(unsigned *vector, unsigned bit, bool value=true) {
    unsigned index = bit >> 5;
    unsigned mask  = 1 << (bit & 0x1f);
    if (value && ~vector[index] & mask) atomic_or (vector+index,  mask);
    if (!value && vector[index] & mask) atomic_and(vector+index, ~mask);
  }

  static void set_bit(unsigned *vector, unsigned bit, bool value=true) {
    unsigned index = bit >> 5;
    unsigned mask  = 1 << (bit & 0x1f);
    if (value)  vector[index] |= mask;
    if (!value) vector[index] &= ~mask;
  }

  static bool get_bit(unsigned *vector, unsigned bit) { return vector[bit >> 5] & (1 << (bit & 0x1f)); }

  static  unsigned xchg(volatile unsigned *x, unsigned y) {
    asm volatile ("xchg %1, %0": "+m"(*x), "+r"(y) :: "memory");
    return y;
  }

  static  unsigned cmpxchg4b(unsigned *var, unsigned oldvalue, unsigned newvalue) {
    return __sync_val_compare_and_swap(reinterpret_cast<unsigned *>(var), oldvalue, newvalue); }

  static  unsigned cmpxchg4b(volatile void *var, unsigned oldvalue, unsigned newvalue) {
    return __sync_val_compare_and_swap(reinterpret_cast<volatile unsigned *>(var), oldvalue, newvalue); }

  static  unsigned long long cmpxchg8b(void *var, unsigned long long oldvalue, unsigned long long newvalue) {
    return __sync_val_compare_and_swap(reinterpret_cast<unsigned long long *>(var), oldvalue, newvalue); }

  static  unsigned long long cmpxchg8b(volatile void *var, unsigned long long oldvalue, unsigned long long newvalue) {
    return __sync_val_compare_and_swap(reinterpret_cast<volatile unsigned long long *>(var), oldvalue, newvalue); }

  static  unsigned long  atomic_xadd(unsigned long volatile *ptr, unsigned long value) { return __sync_fetch_and_add(ptr, value); }
  static  long           atomic_xadd(long *ptr, long value)                   { return __sync_fetch_and_add(ptr, value); }

  static unsigned long long rdtsc() {
    unsigned low, high;
    asm volatile("rdtsc" :  "=a"(low), "=d"(high));
    return union64(high, low);
  }

  static  unsigned bsr(unsigned value) { return __builtin_clz(value) ^ 0x1F; }
  static  unsigned bsf(unsigned value) { return __builtin_ctz(value); }

  static  unsigned minshift(unsigned long start, unsigned long size, unsigned minshift = 31) {
    unsigned shift = Cpu::bsf(start | (1ul << (8*sizeof(unsigned long)-1)));
    if (shift < minshift) minshift = shift;
    shift = Cpu::bsr(size | 1);
    if (shift < minshift) return shift;
    return minshift;
  }

  static  unsigned cpuid(unsigned eax, unsigned &ebx, unsigned &ecx, unsigned &edx) {
    asm volatile ("cpuid": "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));
    return eax;
  }

  template<unsigned operand_size>
    static void move(void *tmp_dst, void *tmp_src) {
    // XXX aliasing!
    if (operand_size == 0) *reinterpret_cast<unsigned char *>(tmp_dst) = *reinterpret_cast<unsigned char *>(tmp_src);
    if (operand_size == 1) *reinterpret_cast<unsigned short *>(tmp_dst) = *reinterpret_cast<unsigned short *>(tmp_src);
    if (operand_size == 2) *reinterpret_cast<unsigned int *>(tmp_dst) = *reinterpret_cast<unsigned int *>(tmp_src);
    //asm volatile ("" : : : "memory");
  }

  /**
   * Transfer bytes from src to dst.
   */
  static void move(void * tmp_dst, void *tmp_src, unsigned order) {
    switch (order) {
    case 0:  move<0>(tmp_dst, tmp_src); break;
    case 1:  move<1>(tmp_dst, tmp_src); break;
    case 2:  move<2>(tmp_dst, tmp_src); break;
    default: asm volatile ("ud2a");
    }
  }
};
