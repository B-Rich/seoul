/**
 * MSR executor.
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

#include "nul/motherboard.h"

/**
 * Handle RDMSR, WRMSR and RDTSC instructions.
 *
 * State: unstable
 * Features: TSC, SYSENTER,
 * Missing: #GP, Perfcnt, MTRR, ...
 */
class MsrExecutor : public StaticReceiver<MsrExecutor>
{
  const char *debug_getname() { return "MsrExecutor"; };

  enum {
    MSR_TSC = 0x10,
    MSR_SYSENTER_CS = 0x174,
    MSR_SYSENTER_ESP,
    MSR_SYSENTER_EIP,
  };

  void set_value(CpuState *cpu, unsigned long long value)
  {
    cpu->eax = value;
    cpu->edx = value >> 32;
  };

  unsigned long long get_value(CpuState *cpu) {  return static_cast<unsigned long long>(cpu->edx) << 32 | cpu->eax; };

 public:
  bool  receive(MessageExecutor &msg)
  {
    switch (msg.cpu->head.pid)
      {
      case 16: // rdtsc
	set_value(msg.cpu, msg.cpu->tsc_off + Cpu::rdtsc());
	break;
      case 31: // rdmsr
	switch (msg.cpu->ecx)
	  {
	  case MSR_TSC:
	    set_value(msg.cpu, msg.cpu->tsc_off + Cpu::rdtsc());
	    break;
	  case MSR_SYSENTER_CS:
	  case MSR_SYSENTER_ESP:
	  case MSR_SYSENTER_EIP:
	    set_value(msg.cpu, (&msg.cpu->sysenter_cs)[msg.cpu->ecx - MSR_SYSENTER_CS]);
	    break;
	  case 0x1b: // APIC base MSR
	    set_value(msg.cpu, msg.vcpu->msr_apic);
	    break;
	  case 0x8b: // microcode
	    // MTRRs
	  case 0x250:
	  case 0x258:
	  case 0x259:
	  case 0x268 ... 0x26f:
	    set_value(msg.cpu, 0);
	    break;
	  default:
	    Logging::panic("unsupported rdmsr %x at %x",  msg.cpu->ecx, msg.cpu->eip);
	  }
	break;
      case 32: // wrmsr
	switch (msg.cpu->ecx)
	  {
	  case MSR_TSC:
	    msg.cpu->tsc_off = -Cpu::rdtsc() + get_value(msg.cpu);
	    Logging::printf("reset RDTSC to %llx at %x value %llx\n", msg.cpu->tsc_off, msg.cpu->eip, get_value(msg.cpu));
	    break;
	  case MSR_SYSENTER_CS:
	  case MSR_SYSENTER_ESP:
	  case MSR_SYSENTER_EIP:
	    (&msg.cpu->sysenter_cs)[msg.cpu->ecx - MSR_SYSENTER_CS] = get_value(msg.cpu);
	    Logging::printf("wrmsr[%x] = %llx\n", msg.cpu->ecx, get_value(msg.cpu));
	    break;
	  case 0x1b: // APIC base MSR
	    msg.vcpu->msr_apic = get_value(msg.cpu) & ((1ull << Config::PHYS_ADDR_SIZE) - 1) & ~0x6ffull;
	    break;
	  default:
	    Logging::panic("unsupported wrmsr %x <-(%x:%x) at %x",  msg.cpu->ecx, msg.cpu->edx, msg.cpu->eax, msg.cpu->eip);
	  }
	break;
      default:
	Logging::panic("unsupported pid %x at %x",  msg.cpu->head.pid, msg.cpu->eip);
      }
    msg.cpu->eip += msg.cpu->inst_len;
    msg.cpu->head.pid = 0;
    return true;
  }
};

PARAM(msr,
      {
	Device *dev = new MsrExecutor();
	mb.bus_executor.add(dev,  &MsrExecutor::receive_static, 16);
	mb.bus_executor.add(dev,  &MsrExecutor::receive_static, 31);
	mb.bus_executor.add(dev,  &MsrExecutor::receive_static, 32);
      },
      "msr - create a executor that handles the RDMSR, WRMSR and RDTSC instructions.");
