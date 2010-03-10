/**
 * AHCI emulation.
 *
 * Copyright (C) 2008-2009, Bernhard Kauer <bk@vmmon.org>
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
#include "model/sata.h"
#include "model/pci.h"


class ParentIrqProvider
{
 public:
  virtual void trigger_irq (void * child) = 0;
};

/**
 * A port of an AhciController.
 *
 * State: unstable
 * Features: register set, FIS
 * Missing: plenty
 */
class AhciPort : public FisReceiver
{
  DBus<MessageMemWrite> *_bus_memwrite;
  DBus<MessageMemRead> *_bus_memread;
  FisReceiver *_drive;
  ParentIrqProvider *_parent;
  unsigned _ccs;
  unsigned _inprogress;
  bool _need_initial_fis;


#define  REGBASE "ahcicontroller.cc"
#include "reg.h"


  /**
   * Copy to the user.
   */
  bool copy_out(unsigned long address, void *ptr, unsigned count)
  {
    MessageMemWrite msg(address, ptr, count);
    if (!_bus_memwrite->send(msg))
      // XXX DMA bus abort
      Logging::panic("%s() could not copy out %x bytes to %lx\n", __PRETTY_FUNCTION__, count, address);
    return true;
  }

  /**
   * Copy from the user.
   */
  bool copy_in(unsigned long address, void *ptr, unsigned count)
  {
    MessageMemRead msg(address, ptr, count);
    if (!_bus_memread->send(msg))
      // XXX DMA bus abort
      Logging::panic("%s() could not copy in %x bytes from %lx\n", __PRETTY_FUNCTION__, count, address);
    return true;
  }

 public:

  void set_parent(ParentIrqProvider *parent, DBus<MessageMemWrite> *bus_memwrite, DBus<MessageMemRead> *bus_memread)
  {
    _parent = parent;
    _bus_memwrite = bus_memwrite;
    _bus_memread = bus_memread;
  }


  /**
   * Receive a FIS from the Device.
   */
  void receive_fis(unsigned fislen, unsigned *fis)
  {
    unsigned copy_offset;

    // fis receiving enabled?
    // XXX bug in 2.6.27?
    //if (!_need_initial_fis && ~PxCMD & 0x10) { Logging::printf("skip FIS %x\n", fis[0]); return; }

    // update status and error fields
    PxTFD = (PxTFD & 0xffff0000) | fis[0] >> 16;

    switch (fis[0] & 0xff)
      {
      case 0x34: // d2h register fis
	assert(fislen == 5);
	copy_offset = 0x40;

	if (_need_initial_fis)
	  {
	    // update signature
	    PxSIG = fis[1] << 8 | fis[3] & 0xff;
	    // set PxSTSS since a device is available
	    PxSSTS = (PxSSTS & ~0xfff) | 0x113;
	    Logging::printf("initial fis received %x %x\n", fis[1], fis[3]);
	    _need_initial_fis = false;
	  }

	// we finished the current command
	if (~fis[0] & 0x80000 && fis[4])  {
	  unsigned mask = 1 << (fis[4] - 1);
	  if (mask & ~_inprogress)
	    Logging::panic("XXX broken %x,%x inprogress %x\n", fis[0], fis[4], _inprogress);
	  _inprogress &= ~mask;
	  PxCI &= ~mask;
	  PxSACT &= ~mask;
	}
	else
	  Logging::printf("not finished %x,%x inprogress %x\n", fis[0], fis[4], _inprogress);
	break;
      case 0x41: // dma setup fis
	assert(fislen == 7);
	copy_offset = 0;
	break;
      case 0x5f: // pio setup fis
	assert(fislen == 5);
	copy_offset = 0x20;

	Logging::printf("PIO setup fis\n");
	break;
      default:
	assert(!"Invalid D2H FIS!");
      }

    // copy to user
    if (PxCMD & 0x10)  copy_out(PxFBU + copy_offset, fis, fislen * 4);
    if (fis[0] & 0x4000) _parent->trigger_irq(this);
  };


  bool set_drive(FisReceiver *drive)
  {
    if (_drive) return true;
    _drive = drive;
    _drive->set_peer(this);

    comreset();
    return false;
  }


  void comreset()
  {
    // reset all device registers except CL+FB to default values
    reset_PxIS();
    reset_PxIE();
    reset_PxCMD();
    reset_PxTFD();
    reset_PxSIG();
    reset_PxSSTS();
    reset_PxSCTL();
    reset_PxSERR();
    reset_PxCI();
    reset_PxSNTF();
    reset_PxFBS();

    _need_initial_fis = true;
    _inprogress = 0;

    if (_drive) {

      // we use the legacy reset mechanism to transmit a COMRESET to the SATA disk
      unsigned fis[5] = { 0x27, 0, 0, 0x04000000, 0};
      _drive->receive_fis(5, fis);
      // toggle SRST in the control register
      fis[3] = 0;
      _drive->receive_fis(5, fis);
    }
  }


  unsigned execute_command(unsigned value)
  {
    COUNTER_INC("ahci cmd");

      // try to execute all active commands
      for (unsigned i = 0; i < 32; i++)
	{
	  unsigned slot = (_ccs >= 31) ? 0 : _ccs + 1;
	  _ccs = slot;

	  // XXX check for busy bit
	  if (value & ~_inprogress & (1 << slot))
	    {
	      if (!PxTFD & 0x80)  break;
	      _inprogress |= 1 << slot;

	      unsigned  cl[4];
	      copy_in(PxCLBU +  slot * 0x20, cl, sizeof(cl));
	      unsigned clflags  = cl[0];

	      unsigned ct[clflags & 0x1f];
	      copy_in(cl[2], ct, sizeof(ct));
	      assert(~clflags & 0x20 && "ATAPI unimplemented");

	      // send a dma_setup_fis
	      // we reuse the reserved fields to send the PRD count and the slot
	      unsigned dsf[7] = {0x41, cl[2] + 0x80, 0, clflags >> 16, 0, cl[1], slot+1};
	      _drive->receive_fis(7, dsf);

	      // set BSY
	      PxTFD |= 0x80;
	      _drive->receive_fis(clflags & 0x1f, ct);
	    }
	}
      // make _css known
      PxCMD = (PxCMD & ~0x1f00) | ((_ccs & 0x1f) << 8);
      return 0;
  }


  AhciPort() : _drive(0), _parent(0) { AhciPort_reset(); };

};

#else
#ifndef AHCI_CONTROLLER
REGSET(AhciPort,
       REG_RW(PxCLB,    0x0, 0, 0xfffffc00)
       REG_RO(PxCLBU,   0x4, 0)
       REG_RW(PxFB,     0x8, 0, 0xffffff00)
       REG_RO(PxFBU,    0xc, 0)
       REG_WR(PxIS,    0x10, 0, 0xdfc000af, 0, 0xdfc000af, COUNTER_INC("IS");)
       REG_RW(PxIE,    0x14, 0, 0x7dc0007f)
       REG_WR(PxCMD,   0x18, 0, 0xf3000011, 0, 0,
	      // enable FRE
	      if ( PxCMD & 0x10 && ~oldvalue & 0x10) PxCMD |= 1 << 14;
	      // disable FRE
	      if (~PxCMD & 0x10 &&  oldvalue & 0x10) PxCMD &= ~(1 << 14);
	      // enable CR
	      if (PxCMD & 1 && ~oldvalue & 1) { PxCMD |= 1 << 15;  _ccs = 32; }
	      // disable CR
	      if (~PxCMD & 1 &&  oldvalue & 1)
		{
		  PxCMD &= ~(1 << 15);
		  // reset PxSACT
		  reset_PxSACT();
		  reset_PxCI();
		}
	      )
       REG_RW(PxTFD,   0x20, 0x7f, 0)
       REG_RW(PxSIG,   0x24, 0xffffffff, 0)
       REG_RW(PxSSTS,  0x28, 0, 0)
       REG_WR(PxSCTL,  0x2c, 0, 0x00000fff, 0, 0,
	      switch (PxSCTL & 0xf) {
	      case 1: comreset(); break;
	      case 2:
		// put device in offline mode
		reset_PxSSTS();
	      default:
		break;
	      })
       REG_WR(PxSERR,  0x30, 0, 0xffffffff, 0, 0xffffffff, )
       REG_WR(PxSACT,  0x34, 0, 0xffffffff, 0xffffffff, 0, )
       REG_WR(PxCI,    0x38, 0, 0xffffffff, 0xffffffff, 0, execute_command(PxCI); )
       REG_RO(PxSNTF,  0x3c, 0)
       REG_RO(PxFBS,   0x40, 0));


#else

REGSET(PCI,
       REG_RO(PCI_ID,        0x0, 0x275c8086)
       REG_RW(PCI_CMD_STS,   0x1, 0x100000, 0x0406)
       REG_RO(PCI_RID_CC,    0x2, 0x01060102)
       REG_RW(PCI_ABAR,      0x9, 0, ABAR_MASK)
       REG_RO(PCI_SS,        0xb, 0x275c8086)
       REG_RO(PCI_CAP,       0xd, 0x80)
       REG_RW(PCI_INTR,      0xf, 0x0100, 0xff)
       REG_RO(PCI_PID_PC,   0x20, 0x00008801)
       REG_RO(PCI_PMCS,     0x21, 0x0000)
       REG_RW(PCI_MSI_CTRL, 0x22, 0x00000005, 0x10000)
       REG_RW(PCI_MSI_ADDR, 0x23, 0, 0xffffffff)
       REG_RW(PCI_MSI_DATA, 0x24, 0, 0xffffffff));



REGSET(AhciController,
       REG_RW(REG_CAP,   0x0, 0x40149f00 | (AhciController::MAX_PORTS - 1), 0)
       REG_WR(REG_GHC,   0x4, 0x80000000, 0x3, 0x1, 0,
	      // reset HBA?
	      if (REG_GHC & 1) {
		for (unsigned i=0; i < MAX_PORTS; i++)  _ports[i].comreset();
		// set all registers to default values
		reset_REG_IS();
		reset_REG_GHC();
	      })
       REG_WR(REG_IS,    0x8, 0, 0xffffffff, 0x00000000, 0xffffffff, )
       REG_RW(REG_PI,    0xc, 1, 0)
       REG_RO(REG_VS,   0x10, 0x00010200)
       REG_RO(REG_CAP2, 0x24, 0x0));

#endif
#endif

#ifndef REGBASE

/**
 * An AhciController on a PCI card.
 *
 * State: unstable
 * Features: PCI cfg space, AHCI register set
 * Missing: MSI delivery
 */
class AhciController : public ParentIrqProvider,
		       public PciConfigHelper<AhciController>,
		       public StaticReceiver<AhciController>
{
  enum {
    MAX_PORTS = 32,
    ABAR_MASK = 0xffffe000,
  };
  DBus<MessageIrq> &_bus_irqlines;
  unsigned char _irq;
  AhciPort _ports[MAX_PORTS];

#define AHCI_CONTROLLER
#define  REGBASE "ahcicontroller.cc"
#include "reg.h"

  bool match_bar(unsigned long &address) {
    bool res = !((address ^ PCI_ABAR) & ABAR_MASK);
    address &= ~ABAR_MASK;
    return res;
  }


  const char* debug_getname() { return "AHCI"; }
 public:

  void trigger_irq (void * child) {
    unsigned index = reinterpret_cast<AhciPort *>(child) - _ports;
    if (~REG_IS & (1 << index))
      {
	REG_IS |= 1 << index;
	if (REG_GHC & 0x2)
	  {
	    unsigned irq = _irq;
	    if (PCI_MSI_CTRL & 0x10000) {
	      Logging::printf("MSI %x %x\n", PCI_MSI_ADDR, PCI_MSI_DATA);
	      // XXX FSB delivery
	      irq = PCI_MSI_DATA & 0xff;
	    }

	    MessageIrq msg(MessageIrq::ASSERT_IRQ, irq);


	    _bus_irqlines.send(msg);
	  }
      }
  };


  bool receive(MessageMemWrite &msg)
  {
    unsigned long addr = msg.phys;
    if (!match_bar(addr) || !(PCI_CMD_STS & 0x2))
      return false;

    check1(false, (msg.count != 4 || (addr & 0x3)), "%s() - unaligned or non-32bit access at %lx, %x", __PRETTY_FUNCTION__, msg.phys, msg.count);

    bool res;
    if (addr < 0x100)
      res = AhciController_write(addr, *reinterpret_cast<unsigned *>(msg.ptr));
    else if (addr < 0x100+MAX_PORTS*0x80)
      res = _ports[(addr - 0x100) / 0x80].AhciPort_write(addr & 0x7f, *reinterpret_cast<unsigned *>(msg.ptr));
    else
      return false;

    if (!res)  Logging::printf("%s(%lx) failed\n", __PRETTY_FUNCTION__, addr);
    return true;
  };


  bool receive(MessageMemRead &msg)
  {
    unsigned long addr = msg.phys;
    if (!match_bar(addr) || !(PCI_CMD_STS & 0x2))
      return false;

    check1(false, (msg.count != 4 || (addr & 0x3)), "%s() - unaligned or non-32bit access at %lx, %x", __PRETTY_FUNCTION__, msg.phys, msg.count);

    bool res;
    unsigned uvalue = 0;
    if (addr < 0x100)
      res = AhciController_read(addr, uvalue);
    else if (addr < 0x100+MAX_PORTS*0x80)
      res = _ports[(addr - 0x100) / 0x80].AhciPort_read(addr & 0x7f, uvalue);
    else
      return false;

    if (res)  *reinterpret_cast<unsigned *>(msg.ptr) = uvalue;
    return res;
  };


  bool receive(MessageAhciSetDrive &msg)
  {
    if (msg.port > MAX_PORTS || _ports[msg.port].set_drive(msg.drive)) return false;

    // enable it in the PI register
    REG_PI |= 1 << msg.port;

    /**
     * fix CAP, according to the spec this is unnneeded, but Linux
     * 2.6.24 checks and sometimes crash without it!
     */
    unsigned count = 0;
    unsigned value = REG_PI;
    for (;value; value >>= 1) { count += value & 1; }
    REG_CAP = (REG_CAP & ~0x1f) | (count - 1);
    return true;
  }

  bool receive(MessagePciConfig &msg) { return PciConfigHelper<AhciController>::receive(msg); }
  AhciController(Motherboard &mb, unsigned char irq) : _bus_irqlines(mb.bus_irqlines), _irq(irq)
  {
    for (unsigned i=0; i < MAX_PORTS; i++) _ports[i].set_parent(this, &mb.bus_memwrite, &mb.bus_memread);
    PCI_reset();
    AhciController_reset();
  };
};

PARAM(ahci,
      {
	AhciController *dev = new AhciController(mb, argv[1]);
	mb.bus_memwrite.add(dev, &AhciController::receive_static<MessageMemWrite>);
	mb.bus_memread.add(dev, &AhciController::receive_static<MessageMemRead>);

	// register PCI device
	mb.bus_pcicfg.add(dev, &AhciController::receive_static<MessagePciConfig>, PciHelper::find_free_bdf(mb.bus_pcicfg, argv[2]));

	// register for AhciSetDrive messages
	mb.bus_ahcicontroller.add(dev, &AhciController::receive_static<MessageAhciSetDrive>);

	// set default state, this is normally done by the BIOS
	// enable IRQ, busmaster DMA and memory accesses
	dev->PCI_write(0x1, 0x406);
	// set MMIO region and IRQ
	dev->PCI_write(0x9, argv[0]);
	dev->PCI_write(0xf, argv[1]);
      },
      "ahci:mem,irq,bdf - attach an AHCI controller to a PCI bus.",
      "Example: Use 'ahci:0xe0800000,14,0x30' to attach an AHCI controller to 00:06.0 on address 0xe0800000 with irq 14.",
      "If no bdf is given, the first free one is searched.",
      "The AHCI controllers are automatically numbered, starting with 0."
      );
#endif
