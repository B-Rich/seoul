/**
 * Generic math helper functions.
 *
 * Copyright (C) 2007-2009, Bernhard Kauer <bk@vmmon.org>
 *
 * This file is part of Vancouver.
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


#include <nova/cpu.h>

#ifdef __LP64__
/* #define split64(INPUT, HIGH, LOW)   LOW = INPUT & 0xffffffff; HIGH = INPUT >> 0x20; */
/* #define union64(HIGH, LOW)          ((static_cast<unsigned long long>(HIGH) << 0x20) | LOW) */
#else
/* split64 and union64 already defined in cpu.h */
#endif

class Math
{
 public:
  /**
   * We are limited here by the ability to divide through a unsigned
   * long value, thus factor and divisor needs to be less than 1<<32.
   */
  static unsigned long long muldiv128(unsigned long long value, unsigned long factor, unsigned long divisor)
    {

#ifdef __LP64__
      unsigned long high;
      asm volatile("mul %4; div %5;" :  "=a"(value), "=d"(high): "0"(value), "1"(0), "r"(factor), "r"(divisor));
      return value;
#else
      unsigned low, high;
      split64(value, high, low);
      unsigned long long lower = static_cast<unsigned long long>(low)*factor;
      unsigned long long upper = static_cast<unsigned long long>(high)*factor;
      unsigned rem = div64(upper, divisor);
      lower += static_cast<unsigned long long>(rem) << 32;
      div64(lower, divisor);

      // this cuts the 96 bits to 64bits
      return (upper << 32) + lower;
#endif
    }

  /**
   * Divide a 64bit value through a 32bit value. Returns the remainder.
   */
  static  unsigned div64(unsigned long long &value, unsigned divisor)
  {
#ifdef __LP64__
    unsigned res = value % divisor;
    value = value / divisor;
    return res;
#else
    unsigned vhigh;
    unsigned vlow;
    split64(value, vhigh, vlow);
    unsigned rem  = vhigh % divisor;
    vhigh = vhigh / divisor;
    asm volatile ("divl %2" : "+a"(vlow), "+d"(rem) : "rm"(divisor));
    value = union64(vhigh, vlow);
    return rem;
#endif
  }


  /**
   * Divide a 64bit signed value through a 32bit value. Returns the remainder.
   */
  static  int idiv64(long long &value, int divisor)
  {
#ifdef __LP64__
    value = value / divisor;
    return value % divisor;
#else
    bool sv, sd;
    if ((sv = value < 0))  value = -value;
    if ((sd = divisor < 0))  divisor = -divisor;

    unsigned long long v = value;
    unsigned rem =  div64(v, static_cast<unsigned>(divisor));
    value = v;
    if (sv) rem = -rem;
    if (sv ^ sd) value = -value;

    return rem;
#endif
  }

  static inline int imod64(long long value, int divisor)
  {
    return idiv64(value, divisor);
  }
  static inline unsigned mod64(unsigned long long value, int divisor)
  {
    return div64(value, divisor);
  }

  static void from_bcd(unsigned char &value)  {  value = (value & 0xf) + (value >> 4) * 10; }
  static void to_bcd(unsigned char &value)  {  value =  ((value / 10) << 4) + (value % 10); }
  static unsigned long htonl(unsigned long value) { asm volatile ("bswap %0" : "+r"(value)); return value;;}
  static unsigned short htons(unsigned short value) { asm volatile ("xchg %%al, %%ah" : "+a"(value)); return value;;}
};
