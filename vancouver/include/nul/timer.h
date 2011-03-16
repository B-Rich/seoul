/**
 * Timer infrastucture.
 *
 * Copyright (C) 2007-2008, Bernhard Kauer <bk@vmmon.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
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
#include "service/cpu.h"
#include "service/math.h"


typedef unsigned long long timevalue;

/**
 * A clock returns the time in different time domains.
 *
 * The reference clock is the CPUs TSC.
 */
class Clock
{
 protected:
  timevalue _source_freq;
 public:
#ifdef TESTING
  virtual
#endif
  timevalue time() { return Cpu::rdtsc(); }

  /**
   * Returns the current clock in freq-time.
   */
  timevalue clock(timevalue freq) { return Math::muldiv128(time(), freq, _source_freq); }

  /**
   * Frequency of the clock.
   */
  timevalue freq() { return _source_freq; }

  /**
   * Returns a timeout in absolute TSC time.
   */
  timevalue abstime(timevalue thedelta, timevalue freq) {  return time() + Math::muldiv128(thedelta, _source_freq, freq); }


  /**
   * Returns a delta in another frequency domain from an absolute TSC value.
   */
  timevalue delta(timevalue theabstime, timevalue freq) {
    timevalue now = time();
    if (now > theabstime) return 0;
    return Math::muldiv128(theabstime - now, freq, _source_freq);
  }

  Clock(timevalue source_freq) : _source_freq(source_freq) {
    #ifndef NDEBUG
    Logging::printf("source freq %lld\n", source_freq);
    #endif
  }
};




/**
 * Keeping track of the timeouts.
 */
template <unsigned ENTRIES, typename DATA>
class TimeoutList
{
  class TimeoutEntry
  {
    friend class TimeoutList<ENTRIES, DATA>;
    TimeoutEntry *_next;
    TimeoutEntry *_prev;
    timevalue _timeout;
    DATA volatile * data;
  };

  TimeoutEntry  _entries[ENTRIES];
public:
  /**
   * Alloc a new timeout object.
   */
  unsigned alloc(DATA volatile * _data = 0)
  {
    unsigned i;
    for (i=1; i < ENTRIES; i++) {
      if (_entries[i].data) continue;
      _entries[i].data = _data;
      return i;
    }
    Logging::printf("can not alloc a timer!\n");
    return 0;
  }

  /**
   * Dealloc a timeout object.
   */
  unsigned dealloc(unsigned nr, bool withcancel = false) {
    if (!nr || nr > ENTRIES - 1) return 0;
    if (!_entries[nr].data) return 0;

    // should only be done when no no concurrent access happens ...
    if (withcancel) cancel(nr);
    _entries[nr].data = 0;
    return 1;
  }

  /**
   * Cancel a programmed timeout.
   */
  int cancel(unsigned nr)
  {
    if (!nr || nr >= ENTRIES)  return -1;
    TimeoutEntry *current = _entries+nr;
    if (current->_next == current) return -2;
    int res = _entries[0]._next != current;

    current->_next->_prev =  current->_prev;
    current->_prev->_next =  current->_next;
    current->_next = current->_prev = current;
    return res;
  }


  /**
   * Request a new timeout.
   */
  int request(unsigned nr, timevalue to)
  {
    if (!nr || nr > ENTRIES)  return -1;
    timevalue old = timeout();
    TimeoutEntry *current = _entries + nr;
    cancel(nr);

    // keep a sorted list here
    TimeoutEntry *t = _entries;
    do { t = t->_next; }  while (t->_timeout < to);

    current->_timeout = to;
    current->_next = t;
    current->_prev = t->_prev;
    t->_prev->_next = current;
    t->_prev = current;
    return timeout() == old;
  }

  /**
   * Get the head of the queue.
   */
  unsigned  trigger(timevalue now, DATA volatile ** data = 0) {
    if (now >= timeout()) {
      unsigned i = _entries[0]._next - _entries;
      if (data)
        *data = _entries[i].data;
      return i;
    }
    return 0;
  }

  timevalue timeout() { assert(_entries[0]._next); return _entries[0]._next->_timeout; }
  void init()
  {
    for (unsigned i = 0; i < ENTRIES; i++)
      {
        _entries[i]._prev = _entries + i;
        _entries[i]._next = _entries + i;
        _entries[i].data  = 0;
      }
    _entries[0]._timeout = ~0ULL;
  }
};
