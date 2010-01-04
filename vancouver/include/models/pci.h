/**
 * Generic PCI classes.
 *
 * Copyright (C) 2008, Bernhard Kauer <bk@vmmon.org>
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

#pragma once
#include "models/register.h"

enum {
  PCI_DEVICE_PER_BUS = 32*8,
  PCI_CFG_SPACE_DWORDS = 64,
  PCI_CFG_SPACE_MASK = PCI_CFG_SPACE_DWORDS * 4 - 1,
};



/**
 * A template class that provides functions for easier PCI config space implementation.
 */
template <typename Y>
class PciDeviceConfigSpace : public HwRegisterSet< PciDeviceConfigSpace<Y> >
{
 public:
  /**
   * Match a given address to a pci-bar.
   */
  bool match_bar(int index, unsigned long &address)
  {
    unsigned value;
    if (HwRegisterSet< PciDeviceConfigSpace<Y> >::read_reg(index, value))
      {
	unsigned long mask = HwRegisterSet< PciDeviceConfigSpace<Y> >::get_reg_mask(index);
	bool res = !((address ^ value) & mask);
	address &= ~mask;
	return res;
      }
    return false;
  }


  /**
   * The PCI bus transaction function.
   */
  bool __attribute__((always_inline))  receive(MessagePciCfg &msg)
  {
    // config read/write type0 function 0
    if (!(msg.address & ~0xff))
      {
	bool res;
	if (msg.type == MessagePciCfg::TYPE_READ)
	  {
	    msg.value = 0;
	    res = HwRegisterSet< PciDeviceConfigSpace<Y> >::read_all_regs(msg.address, msg.value, 4);
	  }
	else
	  res = HwRegisterSet< PciDeviceConfigSpace<Y> >::write_all_regs(msg.address, msg.value, 4);
	return res;
      }
    return false;
  }
};
