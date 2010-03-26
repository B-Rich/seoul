/**
 * Virtual Bios memory routines.
 *
 * Copyright (C) 2009-2010, Bernhard Kauer <bk@vmmon.org>
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

#include "nul/motherboard.h"
#include "executor/bios.h"

/**
 * Virtual Bios mem routines.
 * Features: int15
 * Missing:
 */
class VirtualBiosMem : public StaticReceiver<VirtualBiosMem>, public BiosCommon
{
  unsigned long _memsize;

  /**
   * Memory+PS2 INT.
   */
  bool handle_int15(MessageBios &msg)
  {
    CpuState *cpu = msg.cpu;
    COUNTER_INC("int15");
    // default is to clear CF
    cpu->efl &= ~1;
    switch (cpu->ax)
      {
      case 0x2400: // disable A20
      case 0x2401: // enable  A20
	{
	  MessageLegacy msg1(MessageLegacy::GATE_A20, cpu->al);
	  if (_mb.bus_legacy.send(msg1))
	    cpu->ax = 0;
	  else
	    error(msg, 0x24);
	}
	break;
      case 0xc201:            // reset mouse
	{
	  cpu->ax = 0x0001;
	  cpu->bx = 0xfaaa;
	}
	break;
      case 0xe820:           // get memory map
	{
	  if ((cpu->edx == 0x534D4150 && cpu->ecx >= 20))
	    {
	      Logging::printf("%s() ebx %x sizeof(mymap) %x rdi %x memsize %lx\n", __func__, cpu->ebx, 2, cpu->edi, _memsize);
	      struct mmap{
		unsigned long long base;
		unsigned long long size;
		unsigned type;
	      } mmap;
	      mmap.type = 1;
	      switch (cpu->ebx)
		{
		case 0:
		  mmap.base = 0;
		  mmap.size = 0xa0000;
		  cpu->ebx++;
		  break;
		case 1:
		  mmap.base = 1<<20;
		  mmap.size = _memsize - (1<<20);
		  cpu->ebx = 0;
		  break;
		default:
		  mmap.type = 0;
		  break;
		}

	      if (mmap.type)
		{
		  copy_out(cpu->es.base + cpu->di, &mmap, 20);
		  cpu->eax = cpu->edx;
		  cpu->ecx = 20;
		  cpu->edx = 0;
		  break;
		}
	    }
	  goto unsupported;
	}
      case 0x00c0:            // get rom configtable
      case 0x5300 ... 0x53ff: // apm installation check
      case 0x8800 ... 0x88ff: // get extended memory
      case 0xc000 ... 0xc0ff: // get configuration
      case 0xc100 ... 0xc1ff: // get ebda
      case 0xe801:            // get memsize
      case 0xe980:            // get intel speedstep information
      unsupported:
	// unsupported
	DEBUG(cpu);
	error(msg, 0x86);
	break;
      default:
	DEBUG(cpu);
      }
    msg.mtr_out |= MTD_GPR_ACDB | MTD_RFLAGS;
    return true;
  }

public:

  bool  receive(MessageBios &msg) {
    switch(msg.irq) {
    case 0x0f:  return true;
    case 0x10:  return true;
    case 0x11: // BIOS equipment word
      msg.cpu->ax = 0x34; // 80x25, ps2-mouse, no-floppy
      msg.mtr_out |= MTD_GPR_ACDB;
      return true;
    case 0x12: // get low memory
      msg.cpu->ax = read_bda(0x13);
      msg.mtr_out |= MTD_GPR_ACDB;
      return true;
    case 0x15:  return handle_int15(msg);
    case 0x17:  // printer
      error(msg, msg.cpu->ah);
      return true;
    default:    return false;
    }
  }

  VirtualBiosMem(Motherboard &mb) : BiosCommon(mb) {
    MessageHostOp msg1(MessageHostOp::OP_GUEST_MEM, 0);
    if (!mb.bus_hostop.send(msg1))
      Logging::panic("%s can't get physical memory size", __PRETTY_FUNCTION__);
    _memsize = msg1.len;
  }
};

PARAM(vbios_mem,
      mb.bus_bios.add(new VirtualBiosMem(mb), &VirtualBiosMem::receive_static<MessageBios>);
      ,
      "vbios_mem - provide memory related virtual BIOS functions.");

