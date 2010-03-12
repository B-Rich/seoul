/**
 * Local APIC model.
 *
 * Copyright (C) 2010, Bernhard Kauer <bk@vmmon.org>
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

#ifndef REGBASE
#include "nul/motherboard.h"
#include "nul/vcpu.h"


class X2Apic : public StaticReceiver<X2Apic>
{
  bool in_x2apic_mode;
  unsigned long long _msr;
#define REGBASE "../model/lapic.cc"
#include "model/reg.h"

public:
  bool  receive(MessageMemRead &msg)
  {
    if (!in_range(_msr & ~0xfff, msg.phys, 0x1000) || msg.count != 4) return false;
    if (msg.phys & 0xf
	|| ((msg.phys & 0xfff) + msg.count) > 0x40*4
	|| !X2Apic_read((msg.phys >> 4) & 0x3f, reinterpret_cast<unsigned *>(msg.ptr)[0]))
      {}// XXX error indication
    return true;
  }


  bool  receive(MessageMemWrite &msg)
  {
    if (!in_range(_msr & ~0xfff, msg.phys, 0x1000) || msg.count != 4) return false;
    if (msg.phys & 0xf
	|| ((msg.phys & 0xfff) + msg.count) > 0x40*4
	|| !X2Apic_write((msg.phys >> 4) & 0x3f, reinterpret_cast<unsigned *>(msg.ptr)[0]))
      {}// XXX error indication
    return true;
  }


  bool  receive(CpuMessage &msg) {
    bool res = false;
    switch (msg.type) {
    case CpuMessage::TYPE_RDMSR:
      if (msg.cpu->ecx == 0x1b) {
	msg.cpu->edx_eax(_msr);
	res= true;
	break;
      }
      if (!in_x2apic_mode || !in_range(msg.cpu->ecx, 0x800, 64)) return false;
      if (msg.cpu->ecx != 0x831) {
	res = X2Apic_read(msg.cpu->ecx & 0x3f, msg.cpu->eax);
	msg.cpu->edx = (msg.cpu->ecx == 0x830) ? _ICR1 : 0;
      }
      break;
    case CpuMessage::TYPE_WRMSR:
      if (msg.cpu->ecx == 0x1b) {
	const unsigned long long mask = ((1ull << (Config::PHYS_ADDR_SIZE)) - 1) &  ~0x6ffull;
	_msr = msg.cpu->edx_eax() & mask;
	res= true;
	break;
      }
      if (!in_x2apic_mode || !in_range(msg.cpu->ecx, 0x800, 64)) return false;
      if (msg.cpu->ecx != 0x831) {
	res = X2Apic_write(msg.cpu->ecx & 0x3f, msg.cpu->eax);
	if (msg.cpu->ecx == 0x830) X2Apic_write(0x31, msg.cpu->edx);
      }
      break;
    case CpuMessage::TYPE_CPUID:
    case CpuMessage::TYPE_CPUID_WRITE:
    case CpuMessage::TYPE_RDTSC:
    default:
      break;
    }
    return false;
  }


  bool  receive(MessageLegacy &msg) {
    if (msg.type == MessageLegacy::RESET) {
      _msr = 0xfee00800;
      if (!_ID) _msr |= 0x100;
      return true;
    }
    return false;
  }

  X2Apic(unsigned initial_apic_id) : in_x2apic_mode(false) {
    _ID = initial_apic_id;
  }
};

#if 0
  if (index == 1 && msg.vcpu->apic->msr & 0x800) msg.cpu->edx |= 2u;
  // propagate initial APIC id
  if (index == 1) msg.cpu->ebx |= msg.vcpu->apic->initial_apic_id << 24;
#endif


PARAM(x2apic, {
    if (!mb.last_vcpu) Logging::panic("no VCPU for this APIC");

    X2Apic *dev = new X2Apic(argv[0]);
    mb.bus_legacy.add(dev, &X2Apic::receive_static<MessageLegacy>);
    mb.last_vcpu->executor.add(dev, &X2Apic::receive_static<CpuMessage>);
  },
  "x2apic:inital_apic_id - provide an x2 APIC for every CPU",
  "Example: 'x2apic:2'",
  "If no initial_apic_id is given, a new free one is used.");
#else
      // XXX calculate CCR+PPR
REGSET(X2Apic,
       REG_RW(_ID,            0x02,          0, 0)
       REG_RO(_VERSION,       0x03, 0x00050014)
       REG_RW(_ICR,           0x30,          0, ~0u)
       REG_RW(_ICR1,          0x31,          0, ~0u)
       REG_RW(_TIMER,         0x32, 0x00010000, 0x310ff)
       REG_RW(_TERM,          0x33, 0x00010000, 0x117ff)
       REG_RW(_PERF,          0x34, 0x00010000, 0x117ff)
       REG_RW(_LINT0,         0x35, 0x00010000, 0x1f7ff)
       REG_RW(_LINT1,         0x36, 0x00010000, 0x1f7ff)
       REG_RW(_ERROR,         0x37, 0x00010000, 0x110ff)
       REG_RW(_INITIAL_COUNT, 0x38,          0, ~0u)
       REG_RW(_CURRENT_COUNT, 0x39,          0, ~0u)
       REG_RW(_DIVIDE_CONFIG, 0x3e,          0, 0xb))
#endif
