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

/**
 * Lapic model.
 *
 * State: unstable
 * Features: MEM, MSR, MSR-base and CPUID, LVT, LINT0/1, EOI, prioritize IRQ, error, RemoteEOI, timer, IPI, lowest prio, reset, x2apic mode
 * Missing:  focus checking, CR8/TPR setting, BIOS ACPI tables
 * Difference:  read-only BSP flag, no interrupt polarity, lowest prio is round-robin
 * Documentation: Intel SDM Volume 3a Chapter 10 253668-033.
 */
class Lapic : public StaticReceiver<Lapic>
{
#define REGBASE "../model/lapic.cc"
#include "model/reg.h"
  enum {
    MAX_FREQ   = 200000000,
    LVT_MASK_BIT = 16,
    OFS_ISR   = 0,
    OFS_TMR   = 256,
    OFS_IRR   = 512,
    LVT_BASE  = _TIMER_offset,
    APIC_ADDR = 0xfee00000,
  };

  VCpu *    _vcpu;
  DBus<MessageMem>   & _bus_mem;
  DBus<MessageApic>  & _bus_apic;
  DBus<MessageTimer> & _bus_timer;
#include "model/simplediscovery.h"
  Clock *   _clock;

  unsigned  _initial_apic_id;
  unsigned long long _msr;
  unsigned  _timer;
  unsigned  _timer_clock_shift;
  unsigned  _timer_dcr_shift;
  timevalue _timer_start;
  unsigned  _vector[8*3];
  unsigned  _esr_shadow;
  unsigned  _isrv;
  bool      _lvtds[6];
  bool      _rirr[6];
  unsigned  _lowest_rr;


  bool sw_disabled() { return ~_SVR & 0x100; }
  bool hw_disabled() { return ~_msr & 0x800; }
  bool x2apic_mode() { return  (_msr & 0xc00) == 0xc00; }


  /**
   * Reset the APIC to some default state.
   */
  void reset(bool init) {
    // INIT preserves the APIC ID
    unsigned old_id = _ID;
    Lapic_reset();
    if (init) _ID = old_id;
    memset(_vector,  0, sizeof(_vector));
    memset(_lvtds,   0, sizeof(_lvtds));
    memset(_rirr,    0, sizeof(_rirr));
    _timer_dcr_shift = 1 + _timer_clock_shift;

    // RESET?
    if (!init) {
      _msr = 0;
      _ID = _initial_apic_id << 24;
      set_base_msr(APIC_ADDR | 0x800);

      // as we enable the APICs, we also perform the BIOS INIT
      Lapic_write(_LINT0_offset, 0x700);
      Lapic_write(_LINT1_offset, 0x400);
      Lapic_write(_SVR_offset,  0x1ff);
    }
  }


  /**
   * Update the APIC base MSR.
   */
  bool set_base_msr(unsigned long long value) {
    //Logging::printf("%s %llx %llx\n", __PRETTY_FUNCTION__, value, _msr);
    const unsigned long long mask = ((1ull << (Config::PHYS_ADDR_SIZE)) - 1) &  ~0x2ffull;
    bool was_x2apic_mode = x2apic_mode();
    // check
    if (value & ~mask || (value & 0xc00) == 0x400 || was_x2apic_mode && (value & 0xc00) == 0x800) return false;


    if ((_msr ^ value) & 0x800) {
      // update CPUID leaf 1 EDX
      CpuMessage msg(1, 3, ~(1 << 9), (value & 0x800) >> 2);
      _vcpu->executor.send(msg);
    }
    // we threat the BSP flag as read-only
    _msr = (value & ~0x100ull) | (_vcpu->is_ap() ? 0 : 0x100);

    if (!was_x2apic_mode && x2apic_mode()) {
      // init LDR + _ID
      register_write( _ID_offset, _ID,  false);
      register_write(_LDR_offset, _LDR, false);
      _ICR1 = 0;
    }

    // set them to default state
    if (hw_disabled()) reset(false);
    return true;
  }

  /**
   * Checks whether a timeout should trigger and returns the current
   * counter value.
   */
  unsigned get_ccr(timevalue now) {
    //if (_initial_apic_id) Logging::printf("ccr %llx start %llx ict %x timer %x\n",  _clock->time(), _timer_start, _ICT, _TIMER);
    if (!_ICT || !_timer_start)  return 0;

    timevalue delta = (now - _timer_start) >> _timer_dcr_shift;
    if (delta < _ICT)  return _ICT - delta;

    // we need to trigger the timer LVT
    trigger_lvt(_TIMER_offset - LVT_BASE);

    // one shot?
    if (~_TIMER & (1 << 17))  {
      _timer_start = 0;
      return 0;
    }

    // add periods to the time we started to detect the next overflow
    unsigned done = Math::mod64(delta, _ICT);
    _timer_start += (delta - done) << _timer_dcr_shift;

    return _ICT - done;
  }

  /**
   * Reprogramm a new host timer.
   */
  void update_timer(timevalue now) {
    unsigned value = get_ccr(now);
    //if (_initial_apic_id) Logging::printf("update timer %x time %llx\n", value,  now + (value << _timer_dcr_shift));
    if (!value || _TIMER & (1 << LVT_MASK_BIT)) return;
    MessageTimer msg(_timer, now + (value << _timer_dcr_shift));
    _bus_timer.send(msg);
  }


  /**
   * We send an IPI.
   */
  bool send_ipi(unsigned icr, unsigned dst) {
    COUNTER_INC("IPI");

    //Logging::printf("[%x] %s %x %x\n", _initial_apic_id, __func__, icr, dst);
    unsigned shorthand = (icr >> 18) & 0x3;
    unsigned event =  1 << ((icr >> 8) & 7);
    bool self = shorthand == 1 || shorthand == 2;

    // no logical destination mode with shorthand
    if (shorthand) icr &= MessageApic::ICR_DM;

    // LOWEST does not work in x2apic mode
    if (event == VCpu::EVENT_LOWEST && x2apic_mode()) return set_error(4);

    // self IPIs can only be fixed
    if (self && event != VCpu::EVENT_FIXED
	// we can not send EXTINT and RRD
	|| event & (VCpu::EVENT_EXTINT | VCpu::EVENT_RRD)
	//  and INIT deassert messages
	|| event == VCpu::EVENT_INIT && ~icr & MessageApic::ICR_ASSERT

	/**
	 * This is a strange thing in the manual: lowest priority with
	 * a broadcast shorthand is invalid. But what about physical
	 * destination mode with dst=0xff?
	 */
	|| event == VCpu::EVENT_LOWEST && shorthand == 2)
      return false;

    // send vector error check
    if ((event & (VCpu::EVENT_FIXED | VCpu::EVENT_LOWEST)) && (icr & 0xff) < 16)
      return set_error(5);

    // self IPI?
    if (shorthand == 1) dst = _ID;

    // broadcast?
    if (shorthand & 2) dst = ~0u;

    if (!x2apic_mode()) dst >>= 24;

    // level triggered IRQs are treated as edge triggered
    icr = icr & 0x4fff;
    if (event == VCpu::EVENT_LOWEST) {

      // we send them round-robin as EVENT_FIXED
      MessageApic msg((icr & ~0x700u), dst, 0);
      _lowest_rr = _bus_apic.send_rr(msg, _lowest_rr);
      // we could set an send accept error here, but that is not supported in the P4...
      return _lowest_rr;
    }
    //Logging::printf("%s %x %x\n", __func__, icr, dst);

    MessageApic msg(icr, dst, shorthand == 3 ? this : 0);
    return _bus_apic.send(msg);
  }


  /**
   * Scan for the highest bit in the ISR or IRR.
   */
  unsigned get_highest_bit(unsigned bit_offset) {
    for (int i=7; i >=0; i--) {
      unsigned value = _vector[(bit_offset >> 5) + i];
      if (value) return (i << 5) | Cpu::bsr(value);
    }
    return 0;
  }

  /**
   * Calc the PPR.
   */
  unsigned processor_prio() {
    unsigned res = _isrv & 0xf0;
    if (_TPR >= res) res = _TPR;
    return res;
  }

  /**
   * Check whether there is an IRQ above the processor prio to inject.
   */
  unsigned prioritize_irq() {
    unsigned irrv = get_highest_bit(OFS_IRR);
    if (irrv && (irrv & 0xf0) > processor_prio()) return irrv;
    return 0;
  }


  /**
   * Send upstream to the CPU that we have an IRQ.
   */
  void update_irqs() {
    COUNTER_INC("update irqs");
    CpuEvent msg(VCpu::EVENT_FIXED);
    if (prioritize_irq()) {
      COUNTER_INC("upstream");
      _vcpu->bus_event.send(msg);
    }
  }


  /**
   * Indicate an error and trigger the error LVT.
   */
  bool set_error(unsigned bit) {
    /**
     * Make sure we do not loop, if the ERROR LVT has also an invalid
     * vector programmed.
     */
    if (_esr_shadow & (1<< bit)) return true;
    Cpu::atomic_set_bit(&_esr_shadow, bit);
    return trigger_lvt(_ERROR_offset - LVT_BASE);
  }

  /**
   * Accept a fixed vector in the IRR.
   */
  void accept_vector(unsigned char vector, bool level, bool value) {

    // lower vectors are reserved
    if (vector < 16) set_error(6);
    else {
      Cpu::atomic_set_bit(_vector, OFS_IRR + vector, !level || value);
      Cpu::atomic_set_bit(_vector, OFS_TMR + vector, level);
    }
    update_irqs();
  }

  /**
   * Broadcast an EOI on the bus if it is level triggered.
   */
  void broadcast_eoi(unsigned vector) {
    if (!Cpu::get_bit(_vector, OFS_TMR + _isrv)) return;

    // we clear our LVT entries first
    if (vector == (_LINT0 & 0xff)) _rirr[_LINT0_offset - LVT_BASE] = false;
    if (vector == (_LINT1 & 0xff)) _rirr[_LINT1_offset - LVT_BASE] = false;

    // broadcast suppression?
    if (_SVR & 0x1000) return;

    MessageMem msg(false, MessageApic::IOAPIC_EOI, &vector);
    _bus_mem.send(msg);
  }

  /**
   *
   */

  bool register_read(unsigned offset, unsigned &value) {
    bool res = true;
    switch (offset) {
    case 0x0a:
      value = processor_prio();
      break;
    case 0x10 ... 0x27:
      value = _vector[offset - 0x10];
      break;
    case 0x39:
      value = get_ccr(_clock->time());
      break;
    default:
      if (!(res = Lapic_read(offset, value))) set_error(7);
    }

    // LVT bits
    if (in_range(offset, LVT_BASE, 6)) {
      if (_lvtds[offset - LVT_BASE]) value |= 1 << 12;
      if (_rirr[offset - LVT_BASE])  value |= MessageApic::ICR_ASSERT;
    }
    //if (_initial_apic_id)  Logging::printf("\t\tAPIC read %x value %x\n", offset, value);
    return res;
  }


  bool register_write(unsigned offset, unsigned value, bool strict) {
    bool res;
    if (_initial_apic_id && offset != 0xb && offset != 0x38 && offset != 0x30 && offset!= 0x31) Logging::printf("\t\tAPIC write %x value %x %x\n", offset, value, strict);
    if (sw_disabled() && in_range(offset, LVT_BASE, 6))  value |= 1 << 16;
    switch (offset) {
    case 0x9: // APR
    case 0xc: // RRD
      // the accesses are ignored
      return true;
    case 0xb: // EOI
      COUNTER_INC("lapic eoi");
      if (strict && value) return false;
      if (_isrv) {
	Cpu::set_bit(_vector, OFS_ISR + _isrv, false);
	broadcast_eoi(_isrv);

	// if we eoi the timer IRQ, rearm the timer
	if (_isrv == (_TIMER & 0xff)) update_timer(_clock->time());

	_isrv = get_highest_bit(OFS_ISR);
	update_irqs();
      }
      return true;
    default:
      if (!(res = Lapic_write(offset, value, strict))) {
	Logging::printf("LAPIC write %x at offset %x failed\n", value, offset);
	set_error(7);
      }
    }

    // mask all entries on SVR writes
    if (offset == _SVR_offset && sw_disabled())
      for (unsigned i=0; i < 6; i++) {
	register_read (i + LVT_BASE, value);
	register_write(i + LVT_BASE, value, false);
      }

    // do side effects of a changed LVT entry
    if (in_range(offset, LVT_BASE, 6)) {
      if (_lvtds[offset - LVT_BASE]) trigger_lvt(offset - LVT_BASE);
      if (offset == _TIMER_offset)   update_timer(_clock->time());

      // send EXTINT deassert messages when LINT0 was masked
      if (offset == _LINT0_offset && (value & (1 << LVT_MASK_BIT))) {
	CpuEvent msg(VCpu::DEASS_EXTINT);
	_vcpu->bus_event.send(msg);
      }
    }
    return res;
  }


  /**
   * Trigger an LVT entry.
   */
  bool trigger_lvt(unsigned num){
    assert(num < 6);
    unsigned lvt;
    Lapic_read(num + LVT_BASE, lvt);
    if (!num) COUNTER_INC("LVT0");
    //Logging::printf("%s %x %x %d %d _SVR %x\n", __func__, num, lvt, _lvtds[num], _rirr[num], _SVR);


    unsigned event = 1 << ((lvt >> 8) & 7);
    bool level =  (lvt & MessageApic::ICR_LEVEL && event == VCpu::EVENT_FIXED) || (event == VCpu::EVENT_EXTINT);

    // do not accept more IRQs if no EOI was performed
    if (_rirr[num]) return true;

    // masked or already pending?
    if (lvt & (1 << LVT_MASK_BIT)) {
      // level && masked - set delivery status bit
      if (level) _lvtds[num] = true;
      return true;
    }

    // perf IRQs are auto masked
    if (num == (_PERF_offset - LVT_BASE)) Cpu::atomic_set_bit(&_PERF, LVT_MASK_BIT);

    if (event == VCpu::EVENT_FIXED) {
      // set Remote IRR on level triggered IRQs
      _rirr[num] = level;
      accept_vector(lvt, level, true);
    }
    else if (event & (VCpu::EVENT_SMI | VCpu::EVENT_NMI | VCpu::EVENT_INIT | VCpu::EVENT_EXTINT)) {
      CpuEvent msg(event);
      _vcpu->bus_event.send(msg);
    }

    /**
     * It is not defined how invalid Delivery Modes are handled. We
     * simply drop SIPI, RRD and LOWEST here.
     */

    // we have delivered it
    _lvtds[num] = false;
    return true;
  }


  /**
   * Check whether we should accept the message.
   */
  bool accept_message(MessageApic &msg) {
    if (hw_disabled())   return false;
    if (msg.ptr == this) return false;

    if (x2apic_mode()) {
      // broadcast?
      if (msg.dst == ~0u)    return true;
      // physical DM
      if (~msg.icr & MessageApic::ICR_DM) return msg.dst == _ID;
      // logical DM
      return !((_LDR ^ msg.dst) & 0xffff0000) && _LDR & (1 << (msg.dst & 0xf));
    }

    unsigned dst = msg.dst << 24;
    //Logging::printf("IPI[%d] %x %x LDR %x DFR %x\n", _initial_apic_id, dst, msg.icr, _LDR, _DFR);

    // broadcast
    if (dst == 0xff000000)  return true;

    // physical DM
    if (~msg.icr & MessageApic::ICR_DM) return dst == _ID;

    // flat mode
    if ((_DFR >> 28) == 0xf) return !!(_LDR & dst);

    // cluster mode
    return !((_LDR ^ dst) & 0xf0000000) && _LDR & dst & 0x0fffffff;
  }


public:
  /**
   * Receive MMIO access.
   */
  bool  receive(MessageMem &msg)
  {
    if (!in_range(msg.phys, _msr & ~0xfffull, 0x1000) || hw_disabled() || x2apic_mode()) return false;
    if ((msg.phys & 0xf) || (msg.phys & 0xfff) >= 0x400) return false;
    if (msg.read)
      register_read((msg.phys >> 4) & 0x3f, *msg.ptr);
    else
      register_write((msg.phys >> 4) & 0x3f, *msg.ptr, false);
    return true;
  }

  /**
   * Receive MMIO map request.
   */
  bool  receive(MessageMemRegion &msg)
  {
    if ((_msr >> 12) != msg.page) return false;
    /**
     * We return true without setting msg.ptr and thus nobody else can
     * claim this region.
     */
    msg.start_page = msg.page;
    msg.count = 1;
    return true;
  }

  /**
   * Timeout for the APIC timer.
   */
  bool  receive(MessageTimeout &msg) {
    if (hw_disabled() || msg.nr != _timer) return false;

    // no need to call update timer here, as the CPU needs to do an
    // EOI first
    get_ccr(_clock->time());
    return true;
  }


  /**
   * Receive an IPI.
   */
  bool  receive(MessageApic &msg) {
    if (!accept_message(msg)) return false;
    assert(!(msg.icr & ~0xcfff));
    unsigned event = 1 << ((msg.icr >> 8) & 7);

    //Logging::printf("LAPIC received event %x\n", event);

    assert(event != VCpu::EVENT_RRD);
    assert(event != VCpu::EVENT_LOWEST);

    if (event == VCpu::EVENT_FIXED)
      accept_vector(msg.icr, msg.icr & MessageApic::ICR_LEVEL, msg.icr & MessageApic::ICR_ASSERT);
    else {
      if (event == VCpu::EVENT_SIPI) event |= (msg.icr & 0xff) << 8;
      if (event == VCpu::EVENT_EXTINT && (msg.icr & 0xc000) == 0x8000) event = VCpu::DEASS_EXTINT;

      // forward INIT, SIPI, SMI, NMI and EXTINT directly to the CPU core
      CpuEvent msg(event);
      _vcpu->bus_event.send(msg);
    }
    return true;
  }

  /**
   * Receive INTA cycle or RESET from the CPU.
   */
  bool  receive(LapicEvent &msg) {
    if (!hw_disabled() && msg.type == LapicEvent::INTA) {
      unsigned irrv = prioritize_irq();
      if (irrv) {
	assert(irrv > _isrv);
	Cpu::atomic_set_bit(_vector, OFS_IRR + irrv, false);
	Cpu::set_bit(_vector, OFS_ISR + irrv);
	_isrv = irrv;
	msg.value = irrv;
      } else
	msg.value = _SVR & 0xff;
    }
    else if (msg.type == LapicEvent::RESET || msg.type == LapicEvent::INIT)
      reset(msg.type == LapicEvent::INIT);
    return true;
  }


  /**
   * Receive RDMSR and WRMSR messages.
   */
  bool  receive(CpuMessage &msg) {
    if (msg.type == CpuMessage::TYPE_RDMSR) {
      msg.mtr_out |= MTD_GPR_ACDB;

      // handle APIC base MSR
      if (msg.cpu->ecx == 0x1b) {
	msg.cpu->edx_eax(_msr);
	return true;
      }

      // check whether the register is available
      if (!in_range(msg.cpu->ecx, 0x800, 64)
	  || !x2apic_mode()
	  || msg.cpu->ecx == 0x831
	  || msg.cpu->ecx == 0x80e) return false;

      // read register
      if (!register_read(msg.cpu->ecx & 0x3f, msg.cpu->eax)) return false;

      // only the ICR has an upper half
      msg.cpu->edx = (msg.cpu->ecx == 0x830) ? _ICR1 : 0;
      return true;
    }
    if (msg.type == CpuMessage::TYPE_WRMSR) {
      // handle APIC base MSR
      if (msg.cpu->ecx == 0x1b)  return set_base_msr(msg.cpu->edx_eax());


      // check whether the register is available
      if (!in_range(msg.cpu->ecx, 0x800, 64)
	  || !x2apic_mode()
	  || msg.cpu->ecx == 0x831
	  || msg.cpu->ecx == 0x80e
	  || msg.cpu->edx && msg.cpu->ecx != 0x830) return false;

      // self IPI?
      if (msg.cpu->ecx == 0x83f && msg.cpu->eax < 0x100 && !msg.cpu->edx)
	return send_ipi(0x40000 | msg.cpu->eax, 0);

      // write upper half of the ICR first
      unsigned old_ICR1 = _ICR1;
      if (msg.cpu->ecx == 0x830) _ICR1 = msg.cpu->edx;

      // write lower half in strict mode with reserved bit checking
      if (!register_write(msg.cpu->ecx & 0x3f, msg.cpu->eax, true)) {
	_ICR1 = old_ICR1;
	return false;
      }
      return true;
    }
    return false;
  }


  /**
   * Legacy pins.
   */
  bool  receive(MessageLegacy &msg) {
    if (hw_disabled())  return false;

    // the legacy PIC output and NMI are on LINT0/1
    if (msg.type == MessageLegacy::EXTINT)
      return trigger_lvt(_LINT0_offset - LVT_BASE);
    if (msg.type == MessageLegacy::NMI)
      return trigger_lvt(_LINT1_offset - LVT_BASE);
    return false;
  }


  bool  receive(MessageDiscovery &msg) {
    if (msg.type != MessageDiscovery::DISCOVERY) return false;

    // write the default APIC address to the MADT
    discovery_write_dw("APIC",  36,    APIC_ADDR, 4);
    // and that we have legacy PICs
    discovery_write_dw("APIC",  40,    1, 4);

    // add the LAPIC structure to the MADT
    unsigned length = discovery_length("APIC", 44);
    discovery_write_dw("APIC", length, (_initial_apic_id << 24) | 0x0800, 4);
    discovery_write_dw("APIC", length + 4, 1, 4);

    // NMI is connected to LINT1
    discovery_write_dw("APIC", length +  8, 0x00ff0604, 4);
    discovery_write_dw("APIC", length + 10,     0x0100, 2);
    return true;
  }


  Lapic(VCpu *vcpu, DBus<MessageMem> &bus_mem, DBus<MessageApic> &bus_apic,
	DBus<MessageTimer> &bus_timer, DBus<MessageDiscovery> &bus_discovery,
	Clock *clock, unsigned initial_apic_id)
    : _vcpu(vcpu), _bus_mem(bus_mem), _bus_apic(bus_apic), _bus_timer(bus_timer), _bus_discovery(bus_discovery),
      _clock(clock), _initial_apic_id(initial_apic_id)
  {

    _ID = initial_apic_id << 24;

    // find a FREQ that is not too high
    for (_timer_clock_shift=0; _timer_clock_shift < 32; _timer_clock_shift++)
      if ((_clock->freq() >> _timer_clock_shift) <= MAX_FREQ) break;

    Logging::printf("LAPIC freq %lld\n", _clock->freq() >> _timer_clock_shift);
    CpuMessage msg[] = {
      // propagate initial APIC id
      CpuMessage(1,  1, 0xffffff, _initial_apic_id << 24),
      CpuMessage(11, 3, 0, _initial_apic_id),
      // support for Lapic
      CpuMessage(1, 2, 0, 1 << 21),
      // support for APIC timer that does not sleep in C-states
      CpuMessage(6, 0, 0, 1 << 2),
    };
    for (unsigned i=0; i < sizeof(msg) / sizeof(*msg); i++)
      _vcpu->executor.send(msg[i]);

    // allocate a timer
    MessageTimer msg0;
    if (!_bus_timer.send(msg0))
      Logging::panic("%s can't get a timer", __PRETTY_FUNCTION__);
    _timer = msg0.nr;
  }
};


PARAM(lapic, {
    static unsigned lapic_count;
    if (!mb.last_vcpu) Logging::panic("no VCPU for this APIC");

    Lapic *dev = new Lapic(mb.last_vcpu, mb.bus_mem, mb.bus_apic, mb.bus_timer, mb.bus_discovery, mb.clock(), ~argv[0] ? argv[0]: lapic_count);
    lapic_count++;
    mb.bus_legacy.add(dev, &Lapic::receive_static<MessageLegacy>);
    mb.bus_apic.add(dev,     &Lapic::receive_static<MessageApic>);
    mb.bus_timeout.add(dev,  &Lapic::receive_static<MessageTimeout>);
    mb.bus_discovery.add(dev,  &Lapic::receive_static<MessageDiscovery>);
    mb.last_vcpu->executor.add(dev, &Lapic::receive_static<CpuMessage>);
    mb.last_vcpu->mem.add(dev, &Lapic::receive_static<MessageMem>);
    mb.last_vcpu->memregion.add(dev, &Lapic::receive_static<MessageMemRegion>);
    mb.last_vcpu->bus_lapic.add(dev, &Lapic::receive_static<LapicEvent>);
  },
  "lapic:inital_apic_id - provide an x2APIC for the last CPU",
  "Example: 'lapic:2'",
  "The first Lapic is dedicated the BSP.",
  "If no inital_apic_id is given the lapic number is used.");


#else
REGSET(Lapic,
       REG_RW(_ID,            0x02,          0, 0xff000000, if (x2apic_mode()) _ID = _initial_apic_id; )
       REG_RO(_VERSION,       0x03, 0x01050014)
       REG_RW(_TPR,           0x08,          0, 0xff,)
       REG_RW(_LDR,           0x0d,          0, 0xff000000, if (x2apic_mode()) _LDR = ((_initial_apic_id & ~0xf) << 12) | ( 1 << (_initial_apic_id & 0xf)); )
       REG_RW(_DFR,           0x0e, 0xffffffff, 0xf0000000,)
       REG_RW(_SVR,           0x0f, 0x000000ff, 0x11ff,
	      for (unsigned i=0; i < 6; i++)
		if (_lvtds[i]) trigger_lvt(i);)
       REG_RW(_ESR,           0x28,          0, 0xffffffff, _ESR = Cpu::xchg(&_esr_shadow, 0); return !value; )
       REG_RW(_ICR,           0x30,          0, 0x000ccfff, send_ipi(_ICR, _ICR1))
       REG_RW(_ICR1,          0x31,          0, 0xff000000,)
       REG_RW(_TIMER,         0x32, 0x00010000, 0x300ff, )
       REG_RW(_TERM,          0x33, 0x00010000, 0x107ff, )
       REG_RW(_PERF,          0x34, 0x00010000, 0x107ff, )
       REG_RW(_LINT0,         0x35, 0x00010000, 0x1a7ff, )
       REG_RW(_LINT1,         0x36, 0x00010000, 0x1a7ff, )
       REG_RW(_ERROR,         0x37, 0x00010000, 0x100ff, )
       REG_RW(_ICT,           0x38,          0, ~0u,
	      _timer_start = _clock->time();
	      update_timer(_timer_start); )
       REG_RW(_DCR,           0x3e,          0, 0xb
,
	      {
		timevalue now = _clock->time();
		unsigned  done = _ICT - get_ccr(now);
		_timer_dcr_shift = _timer_clock_shift + ((((_DCR & 0x3) | ((_DCR >> 1) & 4)) + 1) & 7);

		/**
		 * Move timer_start in the past, which what would be
		 * already done on this period with the current speed.
		 */
		_timer_start     = now - (done << _timer_dcr_shift);
		update_timer(now);
	      }
	      ))
#endif
