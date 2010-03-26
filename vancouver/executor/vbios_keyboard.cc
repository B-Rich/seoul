/**
 * Virtual Bios keyboard routines.
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
#include "host/keyboard.h"

/**
 * Virtual Bios keyboard routines.
 * Features: keybuffer
 * Missing: shift state in bda.
 */
class VirtualBiosKeyboard : public StaticReceiver<VirtualBiosKeyboard>, public BiosCommon
{
  Motherboard *_hostmb;
  unsigned    _lastkey;

  /**
   * Converts our internal keycode format into the BIOS one.
   */
  static unsigned keycode2bios(unsigned value)
  {
    static struct {
      unsigned short code;
      unsigned keycode;
    } bios_key_map[] = {
      { 72 << 8,  KBFLAG_EXTEND0 | 0x75}, // up
      { 80 << 8,  KBFLAG_EXTEND0 | 0x72}, // down
      { 77 << 8,  KBFLAG_EXTEND0 | 0x74}, // right
      { 75 << 8,  KBFLAG_EXTEND0 | 0x6b}, // left
      { 59 << 8,  0x05}, // F1
      { 60 << 8,  0x06}, // F2
      { 61 << 8,  0x04}, // F3
      { 62 << 8,  0x0c}, // F4
      { 63 << 8,  0x03}, // F5
      { 64 << 8,  0x0b}, // F6
      { 65 << 8,  0x83}, // F7
      { 66 << 8,  0x0a}, // F8
      { 67 << 8,  0x01}, // F9
      { 68 << 8,  0x09}, // F10
      {133 << 8,  0x78}, // F11
      {134 << 8,  0x07}, // F12
      { 71 << 8,  KBFLAG_EXTEND0 | 0x6c}, // home
      { 82 << 8,  KBFLAG_EXTEND0 | 0x70}, // insert
      { 83 << 8,  KBFLAG_EXTEND0 | 0x71}, // delete
      { 79 << 8,  KBFLAG_EXTEND0 | 0x69}, // end
      { 73 << 8,  KBFLAG_EXTEND0 | 0x7d}, // pgup
      { 81 << 8,  KBFLAG_EXTEND0 | 0x7a}, // pgdown
      {110 << 8,   0x76},                 // esc
      {       8,   0x66},                 // backspace
    };

    value = value & ~KBFLAG_NUM;

    // handle both shifts the same
    if (value & KBFLAG_RSHIFT) value = value & ~KBFLAG_RSHIFT | KBFLAG_LSHIFT;
    for (unsigned i=0; i < sizeof(bios_key_map) / sizeof(bios_key_map[0]); i++)
      if (bios_key_map[i].keycode == value)
	return bios_key_map[i].code;
    unsigned *ascii_map = GenericKeyboard::get_ascii_map();
    for (unsigned i=0; i<128; i++)
      if (ascii_map[i] == value)
	return (value << 8) | i;
    return 0;
  }


  /**
   * Handle the Keyboard IRQ.
   */
  bool handle_int09(CpuState *cpu)
  {
    MessageIrq msg(MessageIrq::ASSERT_IRQ, 1);
    _hostmb->bus_hostirq.send(msg);
    return true;
  }


  /**
   * Keyboard INT handler.
   */
  bool handle_int16(MessageBios &msg)
  {
    CpuState *cpu = msg.cpu;
    //COUNTER_INC("int16");
    //DEBUG;
    unsigned short next  = read_bda(0x1a);
    unsigned short first = read_bda(0x1c);
    unsigned short start = read_bda(0x80);
    unsigned short end   = read_bda(0x82);

    switch (cpu->ah)
      {
      case 0x00:  // get keystroke
	{
	  if (first != next)
	    {
	      cpu->ax = read_bda(next);
	      next += 2;
	      if (next > end)
		next = start;
	      write_bda(0x1a, next, 2);
	    }
	  else
	    // we should block here until the next IRQ arives, but we return a zero keycode instead
	    cpu->ax = 0;
	}
	break;
      case 0x01: // check keystroke
	{
	  if (first != next)
	    {
	      cpu->efl &= ~0x40;
	      cpu->ax = read_bda(next);
	    }
	  else
	    cpu->efl |= 0x40;
	break;
	}
      case 0x02: // get shift flag
	cpu->al = 0;
	if (_lastkey & KBFLAG_NUM)                    cpu->al |= 1 << 5;
	if (_lastkey & (KBFLAG_LALT | KBFLAG_RALT))   cpu->al |= 1 << 3;
	if (_lastkey & (KBFLAG_LCTRL | KBFLAG_RCTRL)) cpu->al |= 1 << 2;
	if (_lastkey & KBFLAG_LSHIFT)                 cpu->al |= 1 << 1;
	if (_lastkey & KBFLAG_RSHIFT)                 cpu->al |= 1 << 0;
	break;
      case 0x03: // set typematic
	// ignored
	break;
      default:
	DEBUG(cpu);
      }
    msg.mtr_out |= MTD_RFLAGS | MTD_GPR_ACDB;
    return true;
  }

public:
  /**
   * Handle messages from the keyboard host driver.
   */
  bool  receive(MessageKeycode &msg)
  {
    if (msg.keyboard == 0)
      {
	_lastkey = msg.keycode;
	unsigned value = keycode2bios(msg.keycode);
	unsigned short next  = read_bda(0x1a);
	unsigned short first = read_bda(0x1c);
	unsigned short start = read_bda(0x80);
	unsigned short end   = read_bda(0x82);
	//Logging::printf("%s() %x key %x bios %x\n", __PRETTY_FUNCTION__, msg.keyboard, msg.keycode, value);

	first += 0x2;
	if (first >= end)   first = start;
	if (value && first != next)
	  {
	    write_bda(read_bda(0x1c), value, 2);
	    write_bda(0x1c, first, 2);
	  }
	return true;
      }
    Logging::printf("%s() ignored %x %x\n", __PRETTY_FUNCTION__, msg.keyboard, msg.keycode);
    return false;
  }


  /**
   * Answer HostRequests from DummyHostDevices.
   */
  bool  receive(MessageHostOp &msg)
  {
    switch (msg.type)
      {
      case MessageHostOp::OP_ALLOC_IOIO_REGION:
      case MessageHostOp::OP_ALLOC_IOMEM:
      case MessageHostOp::OP_ATTACH_IRQ:
	// we have all ports and irqs
	return true;
      case MessageHostOp::OP_ASSIGN_PCI:
      case MessageHostOp::OP_ATTACH_MSI:
	return false;
      case MessageHostOp::OP_NOTIFY_IRQ:
      case MessageHostOp::OP_VIRT_TO_PHYS:
      case MessageHostOp::OP_GUEST_MEM:
      case MessageHostOp::OP_ALLOC_FROM_GUEST:
      case MessageHostOp::OP_GET_MODULE:
      case MessageHostOp::OP_GET_UID:
      case MessageHostOp::OP_VCPU_CREATE_BACKEND:
      case MessageHostOp::OP_VCPU_BLOCK:
      case MessageHostOp::OP_VCPU_RELEASE:
      default:
	Logging::panic("%s - unimplemented operation %x", __PRETTY_FUNCTION__, msg.type);
      }
  };

  /**
   * Forward IO messages to the device models and vice-versa.
   */
  bool  receive(MessageIOIn &msg)  { return _mb.bus_ioin.send(msg); }
  bool  receive(MessageIOOut &msg) { return _mb.bus_ioout.send(msg); }
  bool  receive(MessageLegacy &msg) { return _hostmb->bus_legacy.send_fifo(msg); }


  bool  receive(MessageBios &msg) {
    switch(msg.irq) {
    case 0x09:  return handle_int09(msg.cpu);
    case 0x16:  return handle_int16(msg);
    default:    return false;
    }
  }
  bool  receive(MessageDiscovery &msg) {
    if (msg.type != MessageDiscovery::DISCOVERY) return false;

    unsigned start = 0x1e001e;
    unsigned end   = 0x2f001e;
    MessageDiscovery msg1("bda", 0x1a, &start, 4);
    MessageDiscovery msg2("bda", 0x80, &end,   4);
    _mb.bus_discovery.send(msg1);
    _mb.bus_discovery.send(msg2);
    return true;
  }


  VirtualBiosKeyboard(Motherboard &mb) : BiosCommon(mb) {

    // create hostmb and hostkeyb
      _hostmb = new Motherboard(mb.clock());
      _hostmb->bus_keycode.add(this, &VirtualBiosKeyboard::receive_static<MessageKeycode>);
      _hostmb->bus_hostop .add(this, &VirtualBiosKeyboard::receive_static<MessageHostOp>);
      _hostmb->bus_hwioin .add(this, &VirtualBiosKeyboard::receive_static<MessageIOIn>);
      _hostmb->bus_hwioout.add(this, &VirtualBiosKeyboard::receive_static<MessageIOOut>);
      _mb.bus_legacy      .add(this, &VirtualBiosKeyboard::receive_static<MessageLegacy>);
      _mb.bus_discovery   .add(this, &VirtualBiosKeyboard::receive_static<MessageDiscovery>);

      char args[] = "hostkeyb:0,0x60,1,,1";
      _hostmb->parse_args(args);


  }
};

PARAM(vbios_keyboard,
      mb.bus_bios.add(new VirtualBiosKeyboard(mb), &VirtualBiosKeyboard::receive_static<MessageBios>);
      ,
      "vbios_keyboard - provide keyboard related virtual BIOS functions.");
