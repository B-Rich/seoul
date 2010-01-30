/**
 * HostHpet driver.
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

/**
 * Use the HPET as timer backend.
 *
 * State:    unstable
 * Features: periodic timer, support different timers, one-shot, HPET ACPI table, MSI
 */
class HostHpet : public StaticReceiver<HostHpet>
{
  enum {
    MAX_FREQUENCY = 10000,
  };
  DBus<MessageTimeout> &_bus_timeout;
  Clock *_clock;
  struct HostHpetRegister {
    volatile unsigned cap;
    volatile unsigned period;
    unsigned res0[2];
    volatile unsigned config;
    unsigned res1[3];
    volatile unsigned isr;
    unsigned res2[51];
    union {
      volatile unsigned  counter[2];
      volatile unsigned long long main;
    };
    unsigned res3[2];
    struct HostHpetTimer {
      volatile unsigned config;
      volatile unsigned int_route;
      volatile unsigned comp[2];
      volatile unsigned msi[2];
      unsigned res[2];
    } timer[24];
  } *_regs;
  unsigned _timer;
  unsigned _irq;
  timevalue _freq;
  unsigned _mindelta;

  const char *debug_getname() {  return "HostHPET"; }
public:
  unsigned irq() { return _irq; }


  bool  receive(MessageIrq &msg)
  {
    COUNTER_SET("HPET main", _regs->counter[0]);
    COUNTER_SET("HPET cmp",  _regs->timer[_timer].comp[0]);
    COUNTER_SET("HPET isr",  _regs->isr);
    if (msg.line == _irq && msg.type == MessageIrq::ASSERT_IRQ)
      {
	COUNTER_INC("HPET irq");

	// reset the irq output
	_regs->isr = 1 << _timer;

	MessageTimeout msg2(MessageTimeout::HOST_TIMEOUT);
	_bus_timeout.send(msg2);
	return true;
      }
    return false;
  }


  bool  receive(MessageTimer &msg)
  {
    if (msg.nr != MessageTimeout::HOST_TIMEOUT) return false;
    if (msg.abstime == ~0ull) return false;

    COUNTER_INC("HPET reprogram");

    //_regs->isr = 1 << _timer;
    timevalue delta = _clock->delta(msg.abstime, _freq);
    if (delta < _mindelta) delta = _mindelta;
    unsigned oldvalue = _regs->counter[0];
    _regs->timer[_timer].comp[0] = oldvalue + delta;
    // we read them back to avoid PCI posting problems on ATI chipsets
    _regs->timer[_timer].comp[0];
    unsigned newvalue = _regs->counter[0];
    if (~_regs->isr & (1 << _timer) && (newvalue - oldvalue) >= delta)
      {
	COUNTER_INC("HPET lost");
	MessageTimeout msg2(MessageTimeout::HOST_TIMEOUT);
	_bus_timeout.send(msg2);

      }
    COUNTER_SET("HPET isr", _regs->isr);
    COUNTER_SET("HPET ov", oldvalue);
    COUNTER_SET("HPET da", delta);
    COUNTER_SET("HPET nv", newvalue);
    return true;
  }


  HostHpet(DBus<MessageTimeout> &bus_timeout, DBus<MessageHostOp> &bus_hostop, Clock *clock, void *iomem, unsigned timer, unsigned theirq, bool level)
    : _bus_timeout(bus_timeout), _clock(clock), _regs(reinterpret_cast<HostHpetRegister *>(iomem)), _timer(timer), _irq(theirq)
  {
    Logging::printf("HostHpet: cap %x config %x period %d ", _regs->cap, _regs->config, _regs->period);
    if (_regs->period > 0x05f5e100 || !_regs->period) Logging::panic("Invalid HPET period");

    _freq = 1000000000000000ull;
    Math::div64(_freq, _regs->period);
    Logging::printf(" freq %lld\n", _freq);
    _mindelta = Math::muldiv128(1, _freq, MAX_FREQUENCY);

    unsigned num_timer = ((_regs->cap & 0x1f00) >> 8) + 1;
    if (num_timer < _timer)  Logging::panic("Timer %x not supported", _timer);
    for (unsigned i=0; i < num_timer; i++)
      Logging::printf("\tHpetTimer[%d]: config %x int %x\n", i, _regs->timer[i].config, _regs->timer[i].int_route);

    // get the IRQ number
    bool legacy = false;
    if (_irq == ~0u)
      {
	MessageHostOp msg1(MessageHostOp::OP_GET_MSIVECTOR, 0);

	// MSI supported?
	if ((_regs->timer[_timer].config & (1<<15)) &&  bus_hostop.send(msg1))
	  {
	    _irq = msg1.value;
	    _regs->timer[_timer].msi[0] = MSI_VALUE + _irq;
	    _regs->timer[_timer].msi[1] = MSI_ADDRESS;
	    // enable MSI
	    _regs->timer[_timer].config |= 1<<14;
	  }
	else
	  {
	    legacy = _regs->cap & 0x8000 && _timer < 2;
	    if (legacy && _timer == 0) _irq = 2;
	    if (legacy && _timer == 1) _irq = 8;
	    if (!legacy)
	      {
		if (!_regs->timer[_timer].int_route)  Logging::panic("No IRQ routing possible for timer %x", _timer);
		_irq = Cpu::bsf(_regs->timer[_timer].int_route);
	      }
	    if (!legacy && ~_regs->timer[_timer].int_route & (1 << _irq))  Logging::panic("IRQ routing to GSI %x impossible for timer %x", _irq, _timer);
	  }
      }

    Logging::printf("HostHpet: using counter %x GSI 0x%02x (%s%s)\n", _timer, _irq, level ? "level" : "edge", legacy ? ", legacy" : "");

    // enable timer in non-periodic 32bit mode
    _regs->timer[_timer].config = (_regs->timer[_timer].config & ~0xa) | (_irq << 9) | 0x104 | (level ? 2 : 0);

    // enable main counter and legacy mode
    _regs->config |= legacy ? 3 : 1;
  }
};

PARAM(hosthpet,
      {
	unsigned long address = argv[0];

	// get address from HPET ACPI table
	if (address == ~0ul)
	  {
	    MessageAcpi msg0("HPET");
	    if (!mb.bus_acpi.send(msg0) || !msg0.table)  { Logging::printf("Warning: no HPET ACPI table -> no HostHpet\n"); return; }

	    struct HpetAcpiTable
	    {
	      char res[40];
	      unsigned char gas[4];
	      unsigned long address[2];
	    };
	    HpetAcpiTable *table = reinterpret_cast<HpetAcpiTable *>(msg0.table);
	    if (table->gas[0])     Logging::panic("HPET access must be MMIO but is %d", table->gas[0]);
	    if (table->address[1]) Logging::panic("HPET must be below 4G");
	    address = table->address[0];
	  }

	// alloc MMIO region
	MessageHostOp msg1(MessageHostOp::OP_ALLOC_IOMEM, address, 1024);
	if (!mb.bus_hostop.send(msg1) || !msg1.ptr)  Logging::panic("%s failed to allocate iomem %lx+0x400\n", __PRETTY_FUNCTION__, address);

	// check whether this looks like an HPET
	unsigned cap = *reinterpret_cast<volatile unsigned *>(msg1.ptr);
	if (cap == ~0u || !(cap & 0xff))
	  {
	    Logging::printf("This is not an HPET at %lx value %x\n", address, cap);
	    return;
	  }


	// create device
	HostHpet *dev = new HostHpet(mb.bus_timeout, mb.bus_hostop, mb.clock(), msg1.ptr, ~argv[1] ? argv[1] : 0, argv[2], argv[3]);
	mb.bus_hostirq.add(dev, &HostHpet::receive_static<MessageIrq>);
	mb.bus_timer.add(dev, &HostHpet::receive_static<MessageTimer>);

	// allocate hostirq
	MessageHostOp msg2(MessageHostOp::OP_ATTACH_HOSTIRQ, dev->irq());
	if (!(msg2.value == ~0U || mb.bus_hostop.send(msg2)))
	  Logging::panic("%s failed to attach hostirq %lx\n", __PRETTY_FUNCTION__, msg2.value);
      },
      "hosthpet:address,timer=0,irq=~0u,level=1 - use the host HPET as timer.",
      "If no address is given, the ACPI HPET table is used.",
      "If no irq is given, either the legacy or the lowest possible IRQ is used.",
      "Example: 'hosthpet:0xfed00000,1' - for the second timer of the hpet at 0xfed00000.");

