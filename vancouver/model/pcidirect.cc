/**
 * Directly-assigned PCI device.
 *
 * Copyright (C) 2007-2009, Bernhard Kauer <bk@vmmon.org>
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
#include "host/hostpci.h"
#include "models/pci.h"


/**
 * Directly assign a host PCI device to the guest.
 *
 * State: testing
 * Features: pcicfgspace, ioport operations, memory read/write, host irq
 * Missing: DMA remapping, MSI, mem-alloc
 * Documentation: PCI spec v.2.2
 */
class DirectPciDevice : public StaticReceiver<DirectPciDevice>, public HostPci
{
  Motherboard &_mb;
  enum
    {
      BARS = 6,
    };
  unsigned _bdf;
  unsigned _hostirq;
  unsigned _cfgspace[PCI_CFG_SPACE_DWORDS];
  unsigned _bars[BARS];
  unsigned _masks[BARS];
  const char *debug_getname() { return "DirectPciDevice"; }


  /**
   * Induce the number of the bars from the header-type.
   */
  unsigned count_bars()
  {
    switch((_cfgspace[3] >> 24) & 0x7f)
      {
      case 0: return 6;
      case 1: return 2;
      default: return 0;
      }
  }


  /**
   * Read the bars and the corresponding masks.
   */
  void read_bars()
    {

      // disable device
      unsigned cmd = conf_read(_bdf, 0x4);
      conf_write(_bdf, 0x4, cmd & ~0x7);

      // read bars and masks
      for (unsigned i=0; i < count_bars(); i++)
	{
	  unsigned a = 0x10 + i*4;
	  _bars[i] = conf_read(_bdf, a);
	  conf_write(_bdf, a, ~0U);
	  _masks[i] = conf_read(_bdf, a);
	  conf_write(_bdf, a, _bars[i]);
	}
      // reenable device
      conf_write(_bdf, 0x4, cmd);


      for (unsigned i=0; i < count_bars(); i++)
	{

	  unsigned  bar = _bars[i];
	  if (bar)
	    if ((bar & 1) == 1)
	      {
		MessageHostOp msg(MessageHostOp::OP_ALLOC_IOIO_REGION, ((bar & ~3) << 8) |  Cpu::bsf((~_masks[i] | 0x3)));
		_mb.bus_hostop.send(msg);
	      }
	    else
	      {
		MessageHostOp msg(MessageHostOp::OP_ALLOC_IOMEM, bar & ~0x1f, 1 << Cpu::bsr((~_masks[i] | 0xfff) + 1));
		if (_mb.bus_hostop.send(msg) && msg.ptr)
		  _bars[i] = reinterpret_cast<unsigned long>(msg.ptr) + (bar & 0x10);
		else
		  Logging::panic("can not map IOMEM region %lx+%x", msg.value, msg.len);
	      }
	  Logging::printf("%s() bar %x -> %x mask %x\n", __func__, bar, _bars[i], _masks[i]);

	  // skip upper part of 64bit bar
	  if ((bar & 0x6) == 0x4)
	    {
	      i++;
	      _masks[i] = 0;
	    }
	}
    }

  /**
   * Check whether the guest io address matches and translate to host
   * address.
   */
  bool match_bars(unsigned &address, unsigned size, bool iospace)
  {
    // check whether io decode is disabled
    if (iospace && ~_cfgspace[1] & 1 || !iospace && ~_cfgspace[1] & 2)
      return false;
    for (unsigned i=0; i < count_bars(); i++)
      {
	unsigned  bar = _cfgspace[4 + i];
	// XXX prefetch bit
	if (!_masks[i] || (bar & 1) != iospace || !in_range(address, bar & ~0x3, (~_masks[i] | 3) + 1 - size + 1))
	  continue;
	address = address - bar + _bars[i];
	return true;
      }
    return false;
  }

 public:


  bool receive(MessageIOIn &msg)
  {
    unsigned old_port = msg.port;
    unsigned new_port = msg.port;
    if (!match_bars(new_port, 1 << msg.type, true))  return false;
    msg.port = new_port;
    bool res = _mb.bus_hwioin.send(msg);
    msg.port = old_port;
    return res;
  }


  bool receive(MessageIOOut &msg)
  {
    unsigned old_port = msg.port;
    unsigned new_port = msg.port;
    if (!match_bars(new_port, 1 << msg.type, true))  return false;
    msg.port = new_port;
    bool res = _mb.bus_hwioout.send(msg);
    msg.port = old_port;
    return res;
  }


  bool receive(MessagePciConfig &msg)
  {
    if (!msg.bdf)
      {
	assert(msg.offset <= PCI_CFG_SPACE_MASK);
	assert(!(msg.offset & 3));
	if (msg.type == MessagePciConfig::TYPE_READ)
	  {
	    if (in_range(msg.offset, 0x10, BARS * 4))
	      memcpy(&msg.value, reinterpret_cast<char *>(_cfgspace) + msg.offset, 4);
	    else
	      msg.value = conf_read(_bdf, msg.offset & ~0x3) >> (8 * (msg.offset & 3));

	    // disable capabilities, thus MSI is not known
	    if (msg.offset == 0x4)   msg.value &= ~0x10;
	    if (msg.offset == 0x34)  msg.value &= ~0xff;

	    // disable multi-function devices
	    if (msg.offset == 0xc)   msg.value &= ~0x800000;
	    //Logging::printf("%s:%x -- %8x,%8x\n", __PRETTY_FUNCTION__, _bdf, msg.offset, msg.value);
	    return true;
	  }
	else
	  {
	    //unsigned old = _cfgspace[msg.offset >> 2];
	    unsigned mask = ~0u;
	    if (in_range(msg.offset, 0x10, BARS * 4))  mask &= _masks[(msg.offset - 0x10) >> 2];
	    _cfgspace[msg.offset >> 2] = _cfgspace[msg.offset >> 2] & ~mask | msg.value & mask;

	    //write through if not in the bar-range
	    if (!in_range(msg.offset, 0x10, BARS * 4))
	      conf_write(_bdf, msg.offset, _cfgspace[msg.offset >> 2]);

	    //Logging::printf("%s:%x -- %8x,%8x old %8x\n", __PRETTY_FUNCTION__, _bdf, msg.offset, _cfgspace[msg.offset >> 2], old);
	  return true;
	  }
      }
    return false;
  }


  bool receive(MessageIrq &msg)
  {
    if (msg.line != _hostirq)  return false;
    //Logging::printf("Forwarding irq message #%x  %x -> %x\n", msg.type, msg.line, _cfgspace[15] & 0xff);
    MessageIrq msg2(msg.type, _cfgspace[15] & 0xff);
    return _mb.bus_irqlines.send(msg2);
  }


  bool receive(MessageIrqNotify &msg)
  {
    unsigned irq = _cfgspace[15] & 0xff;
    if (in_range(irq, msg.baseirq, 8) && msg.mask & (1 << (irq & 0x7)))
      {
	//Logging::printf("Notify irq message #%x  %x -> %x\n", msg.mask, msg.baseirq, _hostirq);
	MessageHostOp msg2(MessageHostOp::OP_NOTIFY_IRQ, _hostirq);
	return _mb.bus_hostop.send(msg2);
      }
    return false;
  }


  bool receive(MessageMemWrite &msg)
  {
    unsigned addr = msg.phys;
    if (!match_bars(addr, msg.count, false))  return false;
    switch (msg.count)
      {
      case 4: 
	*reinterpret_cast<unsigned       *>(addr) = *reinterpret_cast<unsigned       *>(msg.ptr);
	break;
      case 2:
	*reinterpret_cast<unsigned short *>(addr) = *reinterpret_cast<unsigned short *>(msg.ptr);
	break;
      case 1:
	*reinterpret_cast<unsigned char  *>(addr) = *reinterpret_cast<unsigned char  *>(msg.ptr);
	break;
      default:
	memcpy(reinterpret_cast<void *>(addr), msg.ptr, msg.count);
      }
    return true;
  }


  bool receive(MessageMemRead &msg)
  {
    unsigned addr = msg.phys;
    if (!match_bars(addr, msg.count, false))  return false;
    switch (msg.count)
      {
      case 4: 
	*reinterpret_cast<unsigned       *>(msg.ptr) = *reinterpret_cast<unsigned       *>(addr);
	break;
      case 2:
	*reinterpret_cast<unsigned short *>(msg.ptr) = *reinterpret_cast<unsigned short *>(addr);
	break;
      case 1:
	*reinterpret_cast<unsigned char  *>(msg.ptr) = *reinterpret_cast<unsigned char  *>(addr);
	break;
      default:
	memcpy(msg.ptr, reinterpret_cast<void *>(addr), msg.count);
      }
    return true;
  }


  bool  receive(MessageMemMap &msg)
  {
    for (unsigned i=0; i < count_bars(); i++)
      {
	unsigned  bar = _cfgspace[4 + i];
	if (!_masks[i] || (bar & 1) || !in_range(msg.phys, bar & ~0x3, (~_masks[i] | 3) + 1))
	  continue;
	msg.ptr  = reinterpret_cast<char *>(_bars[i] & ~0xfff);
	msg.count = (~_masks[i] | 0xfff) + 1;
	Logging::printf(" MAP %lx+%x from %p\n", msg.phys, msg.count, msg.ptr);
	return true;
      }
    return false;
  }



  DirectPciDevice(Motherboard &mb, unsigned bdf, unsigned hostirq) : HostPci(mb.bus_hwpcicfg), _mb(mb), _bdf(bdf), _hostirq(hostirq), _bars(), _masks()
    {
      for (unsigned i=0; i < PCI_CFG_SPACE_DWORDS; i++) _cfgspace[i] = conf_read(_bdf, i<<2);
      read_bars();

#if 0
      // disable msi
      unsigned offset = find_cap(_bdf, 0x5);      if (offset)
	{
	  unsigned ctrl = conf_read(_bdf, offset);
	  Logging::printf("MSI cap @%x ctrl %x  - disabling\n", offset, ctrl);
	  ctrl &= 0xffff;
	  conf_write(_bdf, offset, ctrl);
	}
      // and msi-x
      offset = find_cap(_bdf, 0x11);
      if (offset)
	{
	  unsigned ctrl = conf_read(_bdf, offset);
	  ctrl &= 0xffff;
	  conf_write(_bdf, offset, ctrl);
	  Logging::printf("MSI-X cap @%x ctrl %x  - disabling\n", offset, ctrl);
	  conf_write(_bdf, offset, ctrl);
	}
#endif    
    }
};

PARAM(dpci,
      {
	HostPci  pci(mb.bus_hwpcicfg);
	unsigned irqline = ~0UL;
	unsigned irqpin;
	unsigned bdf = pci.search_device(argv[0], argv[1], argv[2], irqline, irqpin);
	if (!bdf)
	  Logging::panic("search_device(%lx,%lx,%lx) failed\n", argv[0], argv[1], argv[2]);
	else
	  {
	    
	    if (argv[5] != ~0UL) irqline = argv[5];
	    Logging::printf("search_device(%lx,%lx,%lx) hostirq %x bdf %x \n", argv[0], argv[1], argv[2], irqline, bdf);
	    DirectPciDevice *dev = new DirectPciDevice(mb, bdf, irqline);

	    // add to PCI bus
	    MessagePciBridgeAdd msg2(argv[4], dev, &DirectPciDevice::receive_static<MessagePciConfig>);
	    if (!mb.bus_pcibridge.send(msg2, argv[3] == ~0UL ? 0 : argv[3]))
	      Logging::printf("could not add PCI device to %lx:%lx\n", argv[3], argv[4]);

	    mb.bus_ioin.add(dev, &DirectPciDevice::receive_static<MessageIOIn>);
	    mb.bus_ioout.add(dev, &DirectPciDevice::receive_static<MessageIOOut>);
	    mb.bus_memread.add(dev, &DirectPciDevice::receive_static<MessageMemRead>);
	    mb.bus_memwrite.add(dev, &DirectPciDevice::receive_static<MessageMemWrite>);
	    mb.bus_memmap.add(dev, &DirectPciDevice::receive_static<MessageMemMap>);
	    if (irqline != ~0UL)
	      {
		mb.bus_hostirq.add(dev, &DirectPciDevice::receive_static<MessageIrq>);
		mb.bus_irqnotify.add(dev, &DirectPciDevice::receive_static<MessageIrqNotify>);
		MessageHostOp msg3(MessageHostOp::OP_ATTACH_HOSTIRQ, irqline | 0x100);
		mb.bus_hostop.send(msg3);
	      }
	    MessageHostOp msg4(MessageHostOp::OP_ASSIGN_PCI, bdf);
	    if (!mb.bus_hostop.send(msg4)) Logging::printf("DPCI: could not directly assign %x via iommu\n", bdf);
	  }
	Logging::printf("dpci arg done\n");
      },
      "dpci:class,subclass,instance,bus,devicefun,hostirq - makes the specified hostdevice directly accessible to the guest.",
      "Example: Use 'dpci:2,,0,,0x21,0x35' to attach the first network controller to 00:04.1 by forwarding hostirq 0x35.",
      "If class or subclass is ommited it is not compared. If the instance is ommited the last instance is used.",
      "If bus is ommited the first bus is used. If hostirq is ommited the irqline from the device is used instead.");
