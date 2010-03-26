/**
 * Cpu abstraction with inline asm.
 *
 * Copyright (C) 2007-2009, Bernhard Kauer <bk@vmmon.org>
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
  static  void  hlt()   { asm volatile("hlt"); }
  template <typename T> static  void  atomic_and(T *ptr, T value) { asm volatile ("lock; and %1, (%0)" :: "r"(ptr), "q"(value)); }
  template <typename T> static  void  atomic_or(T *ptr, T value)  { asm volatile ("lock; or %1, (%0)" :: "r"(ptr), "q"(value)); }
  static  unsigned cmpxchg(volatile void *var, unsigned oldvalue, unsigned newvalue)
  {
    asm volatile ("lock; cmpxchg %2, (%0)": "+r"(var), "+a"(oldvalue): "r"(newvalue) : "memory");
    return oldvalue;
  }
  static  unsigned long long cmpxchg8b(volatile void *var, unsigned long long oldvalue, unsigned long long newvalue)
  {
      unsigned olow;
      unsigned ohigh;
      split64(oldvalue, ohigh, olow);
      unsigned nlow;
      unsigned nhigh;
      split64(newvalue, nhigh, nlow);
      asm volatile ("lock; cmpxchg8b (%0)": "+r"(var), "+d"(ohigh), "+a"(olow): "c"(nhigh), "b"(nlow) : "memory");
      return  union64(ohigh, olow);
  }
  static  long  atomic_xadd(long *ptr, long value) { asm volatile ("lock; xadd %0, (%1)" : "+r"(value) : "r"(ptr)); return value; }

  static unsigned long long rdtsc()
  {
    unsigned low, high;
    asm volatile("rdtsc" :  "=a"(low), "=d"(high));
    return union64(high, low);
  }

  static   unsigned bsr(unsigned value)
  {
    unsigned res = 0;
    asm volatile ("bsr %1, %0" : "=r"(res) : "r"(value));
    return res;
  }

  static  unsigned bsf(unsigned value)
  {
    unsigned res = 0;
    asm volatile ("bsf %1, %0" : "=r"(res) : "r"(value));
    return res;
  }

  static  unsigned minshift(unsigned long start, unsigned long size, unsigned minshift = 31)
  {
    unsigned shift = Cpu::bsf(start | (1ul << (8*sizeof(unsigned long)-1)));
    if (shift < minshift) minshift = shift;
    shift = Cpu::bsr(size | 1);
    if (shift < minshift) minshift = shift;
    return minshift;
  }

  static  unsigned cpuid(unsigned eax, unsigned &ebx, unsigned &ecx, unsigned &edx)
  {
    asm volatile ("cpuid": "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));
    return eax;
  }

  /**
   * Return the CPU number.
   */
  static  unsigned cpunr()
  {
    unsigned ebx, ecx, edx;
    cpuid(0x1, ebx, ecx, edx);
    return (ebx >> 24) & 0xff;
  }


  template<unsigned operand_size>
  static void move(void *tmp_dst, void *tmp_src)
  {
    // XXX aliasing!
    if (operand_size == 0) *reinterpret_cast<unsigned char *>(tmp_dst) = *reinterpret_cast<unsigned char *>(tmp_src);
    if (operand_size == 1) *reinterpret_cast<unsigned short *>(tmp_dst) = *reinterpret_cast<unsigned short *>(tmp_src);
    if (operand_size == 2) *reinterpret_cast<unsigned int *>(tmp_dst) = *reinterpret_cast<unsigned int *>(tmp_src);
    //asm volatile ("" : : : "memory");
  }

  /**
   * Transfer bytes from src to dst.
   */
  static void move(void * tmp_dst, void *tmp_src, unsigned order)
  {
    switch (order)
      {
      case 0:  move<0>(tmp_dst, tmp_src); break;
      case 1:  move<1>(tmp_dst, tmp_src); break;
      case 2:  move<2>(tmp_dst, tmp_src); break;
      default: asm volatile ("ud2a");
      }
  }
};
