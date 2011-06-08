/** @file
 * PCI config space access.
 *
 * Copyright (C) 2009, Bernhard Kauer <bk@vmmon.org>
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

#include "nul/motherboard.h"
#include "sys/semaphore.h"

/**
 * Provide HW PCI config space access by bridging PCI cfg read/write
 * messages to the HW IO busses.
 *
 * State: stable
 * Documentation: pci3 spec
 */
struct PciConfigAccess : public StaticReceiver<PciConfigAccess>
{
  static const unsigned BASE = 0xcf8;
  DBus<MessageIOIn>  &_hwioin;
  DBus<MessageIOOut> &_hwioout;
  Semaphore          _lock;

  PciConfigAccess(DBus<MessageIOIn> &hwioin, DBus<MessageIOOut> &hwioout, unsigned semcap) : _hwioin(hwioin), _hwioout(hwioout), _lock(Semaphore(semcap)) { _lock.up(); };
  bool  receive(MessagePciConfig &msg) {

    if (msg.dword >= 0x40 || (msg.bdf >= 0x10000)) return false;

    SemaphoreGuard l(_lock);
    MessageIOOut msg1(MessageIOOut::TYPE_OUTL, BASE, 0x80000000 |  (msg.bdf << 8) | (msg.dword << 2));
    if (!_hwioout.send(msg1, true)) return false;

    switch (msg.type) {
    case MessagePciConfig::TYPE_WRITE: {
      MessageIOOut msg2(MessageIOOut::TYPE_OUTL, BASE+4, msg.value);
      return _hwioout.send(msg2, true);
    }
    case MessagePciConfig::TYPE_READ: {
      MessageIOIn msg3(MessageIOIn::TYPE_INL, BASE+4);
      bool res = _hwioin.send(msg3, true);
      msg.value = msg3.value;
      return res;
    }
    default:
      return false;
    }
  }
};


PARAM(pcicfg,
      {
	MessageHostOp msg0(MessageHostOp::OP_ALLOC_SEMAPHORE, 0UL);
	check0(!mb.bus_hostop.send(msg0), "%s could not allocate semaphore\n", __PRETTY_FUNCTION__);

	MessageHostOp msg1(MessageHostOp::OP_ALLOC_IOIO_REGION,  (PciConfigAccess::BASE << 8) | 3);
	check0(!mb.bus_hostop.send(msg1), "%s could not allocate ioports %x+8\n", __PRETTY_FUNCTION__, PciConfigAccess::BASE);

	Device *dev = new PciConfigAccess(mb.bus_hwioin, mb.bus_hwioout, msg0.value);
	mb.bus_hwpcicfg.add(dev, PciConfigAccess::receive_static<MessagePciConfig>);
      },
      "pcicfg - provide HW PCI config space access through IO ports 0xcf8/0xcfc.");
