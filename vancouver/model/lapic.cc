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
  enum {
    LVT_DS     = 12,
    LVT_RIRR   = 14,
    LVT_MASK   = 16,
    LVT_LEVEL  = 15,
    LVT_LEVEL_MASK = 1 << LVT_LEVEL,
  };
  VCpu *_vcpu;
  unsigned long long _msr;
  bool in_x2apic_mode;
  unsigned _vector[8*3];
#define REGBASE "../model/lapic.cc"
#include "model/reg.h"

  /**
   * Update the APIC base MSR.
   */
  bool update_msr(unsigned long long value) {
    const unsigned long long mask = ((1ull << (Config::PHYS_ADDR_SIZE)) - 1) &  ~0x6ffull;
    if (value & ~mask) return false;
    if (_msr ^ value & 0x800) {
      CpuMessage msg(1, 3, 2, _msr & 0x800 ? 0 : 2);
      _vcpu->executor.send(msg);
    }
    // make the BSP flag read-only
    _msr = (value & ~0x100ull) | (_msr & 0x100);
    return true;
  }

  void send_ipi(unsigned icr, unsigned dst) {
    Logging::panic("%s", __PRETTY_FUNCTION__);
  }


  void set_vector(unsigned value) {
    unsigned vector = value & 0xff;

    // lower vectors are reserved
    if (vector < 16) return;
    Cpu::atomic_set_bit(_vector, 256*2 + vector, true);
    Cpu::atomic_set_bit(_vector, 256*1 + vector, value & LVT_LEVEL_MASK);
  }

  bool register_read(unsigned num, unsigned &value) {
    if (in_range(num, 0x10, 0x18)) {
      value = _vector[num - 0x10];
      return true;
    }
    return X2Apic_read(num, value);
  }

public:
  bool  receive(MessageMem &msg)
  {
    if (!in_range(_msr & ~0xfff, msg.phys, 0x1000)) return false;
    if ((msg.phys & 0xf) || (msg.phys & 0xfff) >= 0x40*4
	||  msg.read && !register_read((msg.phys >> 4) & 0x3f, *msg.ptr)
	|| !msg.read && !X2Apic_write((msg.phys >> 4) & 0x3f, *msg.ptr))
      {}// XXX error indication
    return true;
  }

  bool  receive(MessageMemRegion &msg)
  {
    if ((_msr >> 12) != msg.page) return false;
    /**
     * we return true without setting msg.ptr and thus nobody else can
     * claim this region
     */
    msg.start_page = msg.page;
    msg.count = 1;
    return true;
  }


  bool  receive(CpuMessage &msg) {
    switch (msg.type) {
    case CpuMessage::TYPE_RDMSR:
      // handle APIC base MSR
      if (msg.cpu->ecx == 0x1b) {
	msg.cpu->edx_eax(_msr);
	return true;
      }
      // check whether the register is available
      if (!in_range(msg.cpu->ecx, 0x800, 64)
	  || !in_x2apic_mode
	  || msg.cpu->ecx == 0x831
	  || msg.cpu->ecx == 0x80e) return false;

      // read register
      if (!register_read(msg.cpu->ecx & 0x3f, msg.cpu->eax)) return false;

      // only the ICR has an upper half
      msg.cpu->edx = (msg.cpu->ecx == 0x830) ? _ICR1 : 0;
      return true;
    case CpuMessage::TYPE_WRMSR: {
      // handle APIC base MSR
      if (msg.cpu->ecx == 0x1b)  return update_msr(msg.cpu->edx_eax());
      // check whether the register is available
      if (!in_range(msg.cpu->ecx, 0x800, 64)
	  || !in_x2apic_mode
	  || msg.cpu->ecx == 0x831
	  || msg.cpu->ecx == 0x80e
	  || msg.cpu->edx && msg.cpu->ecx != 0x830) return false;

      // self IPI?
      if (msg.cpu->ecx == 0x83f && msg.cpu->eax < 0x100 && !msg.cpu->edx) {
	send_ipi(0x40000| msg.cpu->eax, 0);
	return true;
      }

      unsigned old_ICR1 = _ICR1;
      // write upper half of the ICR
      if (msg.cpu->ecx == 0x830) _ICR1 = msg.cpu->edx;

      // write lower half in strict mode with reserved bit checking
      if (!X2Apic_write(msg.cpu->ecx & 0x3f, msg.cpu->eax, true)) {
	_ICR1 = old_ICR1;
	return false;
      }
      return true;
    }
    case CpuMessage::TYPE_CPUID:
    case CpuMessage::TYPE_CPUID_WRITE:
    case CpuMessage::TYPE_RDTSC:
    default:
      return false;
    }
  }


  bool trigger_lint(unsigned *lintptr) {
    unsigned lint = *lintptr;

    // masked? - set delivery status bit
    if (lint & (1 << LVT_MASK)) {
      Cpu::atomic_set_bit(lintptr, LVT_DS);
      return true;
    }

    // perf IRQs are auto masked
    if (lintptr == &_PERF) Cpu::atomic_set_bit(lintptr, LVT_MASK);

    unsigned event = (lint >> 8) & 7;
    switch (event) {
    case VCpu::EVENT_FIXED:
      // set Remote IRR on level triggered IRQs
      if (lint & LVT_LEVEL_MASK) Cpu::atomic_set_bit(lintptr, LVT_RIRR);
      set_vector(lint);
      break;
    case VCpu::EVENT_SMI:
    case VCpu::EVENT_NMI:
    case VCpu::EVENT_INIT :
    case VCpu::EVENT_EXTINT:
      {
	CpuEvent msg(event);
	_vcpu->bus_event.send(msg);
      }
      break;
    default:
      // other encodings are reserved, thus we simply drop them
      break;
    }
    return true;
  }


  bool  receive(MessageLegacy &msg) {
    if (msg.type == MessageLegacy::RESET)
      return update_msr(_vcpu->is_ap() ? 0xfee00800 : 0xfee00900);
    if (_vcpu->is_ap()) return false;

    // the BSP gets the legacy PIC output and NMI on LINT0/1
    if (msg.type == MessageLegacy::EXTINT)
      return trigger_lint(&_LINT0);
    if (msg.type == MessageLegacy::NMI)
      return trigger_lint(&_LINT1);
    return false;
  }

  X2Apic(VCpu *vcpu, unsigned initial_apic_id) : _vcpu(vcpu), in_x2apic_mode(false) {
    _ID = initial_apic_id;

    // propagate initial APIC id
    CpuMessage msg1(1,  1, 0xffffff, _ID << 24);
    CpuMessage msg2(11, 3, 0, _ID);
    _vcpu->executor.send(msg1);
    _vcpu->executor.send(msg2);
  }
};


PARAM(x2apic, {
    if (!mb.last_vcpu) Logging::panic("no VCPU for this APIC");

    X2Apic *dev = new X2Apic(mb.last_vcpu, argv[0]);
    mb.bus_legacy.add(dev, &X2Apic::receive_static<MessageLegacy>);
    mb.last_vcpu->executor.add(dev, &X2Apic::receive_static<CpuMessage>);
    mb.last_vcpu->mem.add(dev, &X2Apic::receive_static<MessageMem>);
    mb.last_vcpu->memregion.add(dev, &X2Apic::receive_static<MessageMemRegion>);
  },
  "x2apic:inital_apic_id - provide an x2 APIC for every CPU",
  "Example: 'x2apic:2'");
#else
      // XXX calculate CCR+PPR
REGSET(X2Apic,
       REG_RW(_ID,            0x02,          0, 0)
       REG_RO(_VERSION,       0x03, 0x00050014)
       REG_WR(_ICR,           0x30,          0, 0x000ccfff, 0, 0, send_ipi(_ICR, _ICR1))
       REG_RW(_ICR1,          0x31,          0, 0xff000000)
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
