/*
 * Portal handler functions.
 *
 * Copyright (C) 2008, Bernhard Kauer <bk@vmmon.org>
 *
 * This file is part of Vancouver.nova.
 *
 * Vancouver.nova is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * Vancouver.nova is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */
EXCP_FUNC(got_exception,
	  Logging::printf("%s() #%x ",  __func__, utcb->head.pid);
	  Logging::printf("rip %x rsp %x  %x:%x %x:%x %x", utcb->eip, utcb->esp, 
			  utcb->edx, utcb->eax,
			  utcb->edi, utcb->esi,
			  utcb->ecx);
	  Logging::panic("\n");
	  )

EXCP_FUNC(do_gsi,
	  unsigned res;
	  Logging::printf("%s(%x, %x, %x) %p\n", __func__, utcb->msg[0], utcb->msg[1], utcb->msg[2], utcb); 
	  while (1)
	    {
	      if ((res = Nova::semdown(utcb->msg[0])))
		Logging::panic("%s(%x) request failed with %x\n", __func__, utcb->msg[0], res); 
	      SemaphoreGuard l(_lock);
	      MessageIrq msg(MessageIrq::ASSERT_IRQ, utcb->msg[1]);
	      _mb->bus_hostirq.send(msg);
	    }
	  )

EXCP_FUNC(do_stdin,
	  StdinConsumer *stdinconsumer = new StdinConsumer(reinterpret_cast<Vancouver*>(utcb->head.tls)->_cap_free++);
	  Sigma0Base::request_stdin(stdinconsumer, stdinconsumer->sm());
	 
	  while (1)
	    {
	      MessageKeycode *msg = stdinconsumer->get_buffer();
	      switch ((msg->keycode & ~KBFLAG_NUM) ^ _keyboard_modifier)
		{
		case KBFLAG_EXTEND0 | 0x7c: // printscr
		  //_debug = false;
		  //for (unsigned i=0; i < Motherboard::MAX_CPUS; i++)
		  //_debug ^= 1;
		  {
		    
		    unsigned const perf1 = 0x4f04;
		    unsigned const perf2 = 0x4f08;
		    unsigned long long c1;
		    unsigned long long c2;
		    // XXX NOTIMPLEMENTED perfcount(perf1, perf2, c1, c2);
		    Logging::printf("PERF %lld %lld\n", c1, c2);
		  }
		  break;
		case 0x7E: // scroll lock
		  Logging::printf("toggle HLT\n");
		  for (unsigned i=0; i < Motherboard::MAX_CPUS; i++)
		    _mb->vcpustate(i)->hazard ^= VirtualCpuState::HAZARD_DBGHLT;
		  break;
		case KBFLAG_EXTEND1 | KBFLAG_RELEASE | 0x77: // break
		  //_halifaxexecutor->_verbose = true;
		  //Logging::printf("enable _debug\n");
		  _debug = true;
		  _mb->dump_counters();
		  // XXX NOTIMPLEMENTED syscall(254, 0, 0, 0, 0);
		  //_mb->bus_pic.debug_dump();
		  break;
		  // HOME -> reset VM
		case KBFLAG_EXTEND0 | 0x6c:
		  {
		    SemaphoreGuard l(_lock);
		    MessageLegacy msg2(MessageLegacy::RESET, 0);
		    _mb->bus_legacy.send_fifo(msg2);
		  }
		  break;
		case KBFLAG_LCTRL | KBFLAG_RWIN |  KBFLAG_LWIN | 0x5:
		  Logging::printf("hz %x\n", _mb->vcpustate(0)->hazard);
		  _mb->dump_counters();
		  break;
		default:
		  break;
		}

	      SemaphoreGuard l(_lock);
	      _mb->bus_keycode.send(*msg);
	      stdinconsumer->free_buffer();
	    }
	  )
  
EXCP_FUNC(do_disk,
	  DiskConsumer *diskconsumer = new DiskConsumer(reinterpret_cast<Vancouver*>(utcb->head.tls)->_cap_free++);
	  Sigma0Base::request_disks_attach(diskconsumer, diskconsumer->sm());
	 
	  while (1)
	    {
	      MessageDiskCommit *msg = diskconsumer->get_buffer();
	      SemaphoreGuard l(_lock);
	      _mb->bus_diskcommit.send(*msg);
	      diskconsumer->free_buffer();
	    }
	  )

EXCP_FUNC(do_timer,
	  TimerConsumer *timerconsumer = new TimerConsumer(reinterpret_cast<Vancouver*>(utcb->head.tls)->_cap_free++);
	  Sigma0Base::request_timer_attach(timerconsumer, timerconsumer->sm());
	  while (1)
	    {
	     
	      COUNTER_INC("timer");
	      TimerItem *item = timerconsumer->get_buffer();
	      //Logging::printf("got timer irq %llx\n", Cpu::rdtsc() - time);
	      timerconsumer->free_buffer();

	      SemaphoreGuard l(_lock);
	      timeout_trigger();
	    }
	  )

EXCP_FUNC(do_network,
	  NetworkConsumer *network_consumer = new NetworkConsumer(reinterpret_cast<Vancouver*>(utcb->head.tls)->_cap_free++);
	  Sigma0Base::request_network_attach(network_consumer, network_consumer->sm());
    
	  while (1)
	    {
	      char *buf;
	      utcb->msg[1] = network_consumer->get_buffer(buf);
	      utcb->msg[0] = reinterpret_cast<unsigned long>(buf);
	      //Logging::printf("got network packet %x,%x\n", utcb->msg[0], utcb->msg[1]);

	      MessageNetwork msg(reinterpret_cast<const unsigned char *>(utcb->msg[0]), utcb->msg[1], 0);
	      assert(!_forward_pkt);
	      _forward_pkt = msg.buffer;
	      {
		SemaphoreGuard l(_lock);
		_mb->bus_network.send(msg);
	      }
	      _forward_pkt = 0;
	      network_consumer->free_buffer();
	    }
	  )

VM_FUNC(PT_RECALL,  do_recall, MTD_IRQ,
	//if (utcb->intr_info & 0x80000000)  Logging::printf("recall eip %x %x hz %x\n", utcb->eip, utcb->intr_info, _mb->vcpustate(0)->hazard);
	if (_mb->vcpustate(0)->hazard & VirtualCpuState::HAZARD_INIT)
	  vmx_init(utcb);
	else
	  {
	    SemaphoreGuard l(_lock);
	    if (_debug) Logging::printf("recall %x\n", utcb->eip);
	    COUNTER_INC("recall"); 
	    unsigned lastpid = utcb->head.pid;
	    utcb->head.pid = 1;
	    MessageExecutor msg(static_cast<CpuState*>(utcb), _mb->vcpustate(0));
	    _mb->bus_executor.send(msg, true, utcb->head.pid);
	    utcb->head.pid = lastpid;
	  }
	)

// the VMX portals follow
VM_FUNC(PT_VMX + 2,  vmx_triple, MTD_ALL,
	{
	  utcb->head.pid = 2;
	  if (!execute_all(static_cast<CpuState*>(utcb), _mb->vcpustate(0)))
	    Logging::panic("nobody to execute %s at %x:%x pid %d\n", __func__, utcb->cs.sel, utcb->eip, utcb->head.pid);
	  do_recall(utcb);
	}
	)
VM_FUNC(PT_VMX +  3,  vmx_init, MTD_ALL,
	Logging::printf("%s() mtr %x rip %x ilen %x cr0 %x efl %x\n", __func__, utcb->head.mtr, utcb->eip, utcb->inst_len, utcb->cr0, utcb->efl);
	utcb->head.pid = 3;
	if (!execute_all(static_cast<CpuState*>(utcb), _mb->vcpustate(0)))
	  Logging::panic("nobody to execute %s at %x:%x pid %d\n", __func__, utcb->cs.sel, utcb->eip, utcb->head.pid);
	do_recall(utcb);
	)
VM_FUNC(PT_VMX +  7,  vmx_irqwin, MTD_IRQ,
	COUNTER_INC("irqwin"); 
	do_recall(utcb);
	)
VM_FUNC(PT_VMX + 10,  vmx_cpuid, MTD_EIP | MTD_ACDB,
	utcb->head.pid = 10;
	COUNTER_INC("cpuid"); 
	if (!execute_all(static_cast<CpuState*>(utcb), _mb->vcpustate(0)))
	  Logging::panic("nobody to execute %s at %x:%x pid %d\n", __func__, utcb->cs.sel, utcb->eip, utcb->head.pid);
	// XXX call skip instruction for MTD_STATE update
	)
VM_FUNC(PT_VMX + 12,  vmx_hlt, MTD_EIP | MTD_IRQ,
	COUNTER_INC("hlt"); 
	skip_instruction(utcb);
	 
	// wait for irq
	Cpu::atomic_or<volatile unsigned>(&_mb->vcpustate(0)->hazard, VirtualCpuState::HAZARD_INHLT);
	if (~_mb->vcpustate(0)->hazard & VirtualCpuState::HAZARD_IRQ)
	  _mb->vcpustate(0)->block_sem->down();
	Cpu::atomic_and<volatile unsigned>(&_mb->vcpustate(0)->hazard, ~VirtualCpuState::HAZARD_INHLT);
	//Logging::printf("guest hlt at %x intr %x act %x efl %x\n", utcb->eip, utcb->intr_state, utcb->actv_state, utcb->efl);	 
	do_recall(utcb);
	//if (~utcb->intr_info & 0x80000000)  Logging::printf("guest hlt at %x intr %x act %x efl %x info %x\n", utcb->eip, utcb->intr_state, utcb->actv_state, utcb->efl, utcb->intr_info);
	)

VM_FUNC(PT_VMX + 18,  vmx_vmcall, MTD_EIP | MTD_ACDB | MTD_STA,
	unsigned long long c1;
	unsigned long long c2;
	unsigned const perf1 = 0x3c00;
	unsigned const perf2 = 0x2e41;
	if (utcb->eax & 0x80000000)
	  { 
	    // XXX NOTIMPLEMENTED
	    // syscall(254, utcb->eax & 0xffff, 0, 0, 0);
	    // perfcount(perf1, perf2, c1, c2);
	    // Logging::printf("PERF1 %lld %lld\n", c1, c2);
	    }
	_mb->dump_counters();
	if (~utcb->eax & 0x80000000) 
	  {	      
	    // XXX NOTIMPLEMENTED
	    // perfcount(perf1, perf2, c1, c2);
	    // Logging::printf("PERF2 %lld %lld\n", c1, c2);
	    // syscall(254, utcb->eax & 0xffff, 0, 0, 0);
	  }
	skip_instruction(utcb);
	)

VM_FUNC(PT_VMX + 30,  vmx_ioio, MTD_EIP | MTD_QUAL | MTD_ACDB | MTD_EFL | MTD_STA,
	if (_debug) Logging::printf("guest ioio at %x port %llx len %x\n", utcb->eip, utcb->qual[0], utcb->inst_len);
	//Logging::printf("ioio reason: %x\n", utcb->qual[0]);
	
	
	if (utcb->qual[0] & 0x10)
	  {
	    COUNTER_INC("IOS"); 
	    force_invalid_gueststate_intel(utcb);
	  }
	else
	  {
	    unsigned order = utcb->qual[0] & 7;
	    if (order > 2)  order = 2;
	    ioio_helper(utcb, utcb->qual[0] & 8, order);
	  }
	)

VM_FUNC(PT_VMX + 31,  vmx_rdmsr, MTD_EIP | MTD_ACDB | MTD_TSC | MTD_SYS,
	utcb->head.pid = 31;
	COUNTER_INC("rdmsr"); 
	if (!execute_all(static_cast<CpuState*>(utcb), _mb->vcpustate(0)))
	  Logging::panic("nobody to execute %s at %x:%x pid %d\n", __func__, utcb->cs.sel, utcb->eip, utcb->head.pid);
	)
VM_FUNC(PT_VMX + 32,  vmx_wrmsr, MTD_EIP | MTD_ACDB | MTD_TSC | MTD_SYS,
	utcb->head.pid = 32;
	COUNTER_INC("wrmsr"); 
	if (!execute_all(static_cast<CpuState*>(utcb), _mb->vcpustate(0)))
	  Logging::panic("nobody to execute %s at %x:%x pid %d\n", __func__, utcb->cs.sel, utcb->eip, utcb->head.pid);
	)

VM_FUNC(PT_VMX + 33,  vmx_invalid, MTD_ALL,
	{
	  //_debug |=  utcb->eip > 0xf0000000;
	  //Logging::printf("execute %s at %x:%x pid %d cr0 %x intr %x\n", __func__, utcb->cs.sel, utcb->eip, utcb->head.pid, utcb->cr0, utcb->inj_info);
	  utcb->efl |= 2;
	  instruction_emulation(utcb);
	  if (_mb->vcpustate(0)->hazard & VirtualCpuState::HAZARD_CTRL) 
	    {
	      Cpu::atomic_and<volatile unsigned>(&_mb->vcpustate(0)->hazard, ~VirtualCpuState::HAZARD_CTRL);
	      utcb->head.mtr =  mtd_untyped(utcb->head.mtr) | MTD_CTRL;
	      utcb->ctrl[0] = 1 << 3; // tscoffs
	      utcb->ctrl[1] = 1 << 1; // vmmcall
	    }
	  do_recall(utcb);
	})
VM_FUNC(PT_VMX + 48,  vmx_mmio, MTD_ALL,
	/**
	 * Idea: optimize the default case - mmio to general purpose register
	 * Need state: ACDB, BSD, EIP, EFL, CS, DS, SS, ES, RSP, CR, EFER
	 */
	 
	// make sure we do not inject the #PF!
	utcb->inj_info = ~0x80000000;
	MessageMemMap msg(utcb->qual[1] & ~0xfff, 0, 0);

	// do we have not mapped physram yet?
	if (_mb->bus_memmap.send(msg))
	  {
	    Logging::printf("%s(%llx) phys %lx ptr %p+%x eip %x\n", __func__, utcb->qual[1], msg.phys, msg.ptr, msg.count, utcb->eip);
	    utcb->head.mtr = empty_message();
	    utcb_add_mappings(utcb, true, reinterpret_cast<unsigned long>(msg.ptr), msg.count, msg.phys, 0x1c | 1);
	  }
	else
	    // this is an access to MMIO
	    vmx_invalid(utcb);
	)

// and now the SVM portals
VM_FUNC(PT_SVM + 0x64,  svm_vintr,   MTD_IRQ, vmx_irqwin(utcb); )
VM_FUNC(PT_SVM + 0x72,  svm_cpuid,   MTD_ALL, svm_invalid(utcb); )
VM_FUNC(PT_SVM + 0x78,  svm_hlt,     MTD_EIP | MTD_IRQ,  utcb->inst_len = 1; vmx_hlt(utcb); )
VM_FUNC(PT_SVM + 0x7b,  svm_ioio,    MTD_EIP | MTD_QUAL | MTD_ACDB | MTD_STA,
	{
	  if (utcb->qual[0] & 0x4)
	    {
	      COUNTER_INC("IOS"); 
	      force_invalid_gueststate_amd(utcb);
	    }
	  else
	    {
	      unsigned order = ((utcb->qual[0] >> 4) & 7) - 1;
	      if (order > 2)  order = 2;
	      utcb->inst_len = utcb->qual[1] - utcb->eip;
	      ioio_helper(utcb, utcb->qual[0] & 1, order);
	    }
	}
	)
VM_FUNC(PT_SVM + 0x7c,  svm_msr,     MTD_ALL, svm_invalid(utcb); )
VM_FUNC(PT_SVM + 0x7f,  svm_shutdwn, MTD_ALL, vmx_triple(utcb); )
VM_FUNC(PT_SVM + 0x81,  svm_vmmcall, MTD_EIP | MTD_ACDB | MTD_STA, utcb->inst_len = 3; vmx_vmcall(utcb); )
VM_FUNC(PT_SVM_END - 1, svm_npt,     MTD_ALL, vmx_mmio(utcb); )
VM_FUNC(PT_SVM_END - 0, svm_invalid, MTD_ALL, 
	  instruction_emulation(utcb);
	  if (_mb->vcpustate(0)->hazard & VirtualCpuState::HAZARD_CTRL) 
	    {
	      Cpu::atomic_and<volatile unsigned>(&_mb->vcpustate(0)->hazard, ~VirtualCpuState::HAZARD_CTRL);
	      utcb->head.mtr =  mtd_untyped(utcb->head.mtr) | MTD_CTRL;
	      utcb->ctrl[0] = 1 << 18; // cpuid
	      utcb->ctrl[1] = 1 << 0;  // vmrun
	    }
	do_recall(utcb);
	)
