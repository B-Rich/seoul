/**
 * Physical Memory handling.
 *
 * Copyright (C) 2009, Bernhard Kauer <bk@vmmon.org>
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

#include "vmm/motherboard.h"

class MemoryController : public StaticReceiver<MemoryController>
{
  char *_physmem;
  unsigned long _start;
  unsigned long _end;

  const char *debug_getname() { return "MemoryController"; };
  void debug_dump() {  
    Device::debug_dump();
    Logging::printf(" %lx - %lx", _start, _end);
  };
  
public:
  /****************************************************/
  /* Physmem access                                   */
  /****************************************************/
  bool  receive(MessageMemRead &msg)
  {
    COUNTER_INC("read mem");
    if ((msg.phys < _start) || (msg.phys >= (_end - msg.count)))  return false;
    memcpy(msg.ptr, _physmem + msg.phys, msg.count);
    //if (msg.phys > 0x2d01b0 && msg.phys < 0x2d0200) Logging::printf("READ(%lx, %x, %x)\n", msg.phys, *reinterpret_cast<unsigned *>(msg.ptr), msg.count);
    return true;
  }


  bool  receive(MessageMemWrite &msg)
  {
    COUNTER_INC("write mem");
    if ((msg.phys < _start) || (msg.phys >= (_end - msg.count)))  return false;
    //if (msg.phys > 0x2d01b0 && msg.phys < 0x2d0200) 
    //Logging::printf("WRITE(%lx, %x, %x)\n", msg.phys, *reinterpret_cast<unsigned *>(msg.ptr), msg.count);
    memcpy(_physmem + msg.phys, msg.ptr, msg.count);
    return true;
  }


  bool  receive(MessageMemAlloc &msg)
  {
    //disable ALLOC?  return false;
    COUNTER_INC("allocmem");
    if ((msg.phys1 < _start) || (msg.phys1 >= (_end & ~0xfff)))  return false;
    if (msg.phys2 != ~0xffful && ((msg.phys2 < _start) || (msg.phys2 >= _end & ~0xfff)))  return false;
    if ((msg.phys2 != ~0xffful) && ((msg.phys1 | 0xffful) + 1) != msg.phys2)
      Logging::panic("mmap unimplemented for %lx, %lx", msg.phys1, msg.phys2);    
    *msg.ptr = _physmem + msg.phys1;
    //if (msg.phys1 == 0x373000) Logging::printf("ALLOC(%x, %x, %x)\n", msg.phys1, *reinterpret_cast<unsigned *>(*msg.ptr), msg.phys2);
    return true;
  }


  bool  receive(MessageMemMap &msg)
  {
    if ((msg.phys < _start) || (msg.phys >= _end))  return false;
    msg.phys = _start;
    msg.ptr = _physmem + _start;
    msg.count = _end - _start;
    return true;
  }


  MemoryController(char *physmem, unsigned long start, unsigned long end) : _physmem(physmem), _start(start), _end(end) {}
};


PARAM(mem,
      {
	MessageHostOp msg(MessageHostOp::OP_GUEST_MEM, 0);
	if (!mb.bus_hostop.send(msg))
	  Logging::panic("%s failed to get physical memory\n", __PRETTY_FUNCTION__);
	unsigned long start = ~argv[0] ? argv[0] : 0;	
	unsigned long end   = argv[1] > msg.len ? msg.len : argv[1];
	Logging::printf("physmem: %lx [%lx, %lx]\n", msg.value, start, end);
	Device *dev = new MemoryController(msg.ptr, start, end);
	// physmem access
	mb.bus_memwrite.add(dev,   &MemoryController::receive_static<MessageMemWrite>);
	mb.bus_memread.add(dev,    &MemoryController::receive_static<MessageMemRead>);
	mb.bus_memalloc.add(dev,   &MemoryController::receive_static<MessageMemAlloc>);
	mb.bus_memmap.add(dev,     &MemoryController::receive_static<MessageMemMap>);
	
      },
      "mem:start=0:end=~0 - create a memory controller that handles physical memory accesses.",
      "Example: 'mem:0,0xa0000' for the first 640k region",
      "Example: 'mem:0x100000' for all the memory above 1M")
