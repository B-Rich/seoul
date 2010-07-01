/*
 * Main sigma0 code.
 *
 * Copyright (C) 2008-2009, Bernhard Kauer <bk@vmmon.org>
 *
 * This file is part of Vancouver.
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

#include "nul/motherboard.h"
#include "host/keyboard.h"
#include "host/screen.h"
#include "host/dma.h"
#include "nul/program.h"
#include "service/elf.h"
#include "service/logging.h"
#include "sigma0/sigma0.h"

// global VARs
unsigned     startlate;
unsigned     repeat;
unsigned     console_id;
Motherboard *global_mb;



// extra params
PARAM(startlate,  startlate = argv[0], "startlate:mask=~0 - do not start all modules at bootup.",
      "Example: 'startlate:0xfffffffc' - starts only the first and second module")

PARAM(repeat,     repeat = argv[0],    "repeat:count - start the modules multiple times")



/**
 * Sigma0 application class.
 */
struct Sigma0 : public Sigma0Base, public NovaProgram, public StaticReceiver<Sigma0>
{
  enum {
    MAXCPUS            = 256,
    MAXPCIDIRECT       = 64,
    MAXMODULES         = 64,
    CPUGSI             = 0,
    MEM_OFFSET         = 1ul << 31
  };

  // a mapping from virtual cpus to the physical numbers
  unsigned  _cpunr[MAXCPUS];
  unsigned  _numcpus;
  unsigned  _last_affinity;

  // data per physical CPU number
  struct {
    unsigned  cap_ec_worker;
    unsigned  cap_ec_echo;
    unsigned  cap_pt_echo;
  } _percpu[MAXCPUS];


  // synchronisation of GSIs+worker
  long      _lockcount;
  Semaphore _lock;

  // putc+vga
  char    * _vga;
  VgaRegs   _vga_regs;
  struct PutcData
  {
    VgaRegs        *regs;
    unsigned short *screen;
  } putcd;

  // device data
  Motherboard *_mb;

  // module data
  struct ModuleInfo
  {
    unsigned        mod_nr;
    unsigned        mod_count;
    unsigned        cap_pd;
    unsigned        cpunr;
    unsigned long   rip;
    bool            log;
    bool            dma;
    char *          mem;
    unsigned long   physsize;
    unsigned        uid;
    void *          hip;
  } _modinfo[MAXMODULES];

  // IRQ handling
  unsigned  _msivector;        // next gsi vector
  unsigned long long _gsi;     // bitfield per used GSI
  unsigned _pcidirect[MAXPCIDIRECT];

  unsigned _uid;


  char *map_self(Utcb *utcb, unsigned long physmem, unsigned long size, unsigned rights = DESC_MEM_ALL | DESC_DPT)
  {
    assert(size);

    // we align to order but not more than 4M
    unsigned order = Cpu::bsr(size | 1);
    if (order < 12 || order > 22) order = 22;

    unsigned alignment = (1 << order) - 1;
    unsigned long ofs = physmem & alignment;
    physmem -= ofs;
    size    += ofs;
    if (size & alignment) size += alignment + 1 - (size & alignment);

    unsigned long virt = 0;
    if ((rights & 3) == 1)
      {
	unsigned long s = _virt_phys.find_phys(physmem, size);
	if (s)  return reinterpret_cast<char *>(s) + ofs;
	virt = _free_virt.alloc(size, Cpu::minshift(physmem, size, 22));
	if (!virt) return 0;
      }
    unsigned old = utcb->head.crd;
    utcb->head.crd = Crd(0, 20, rights).value();
    char *res = reinterpret_cast<char *>(virt);
    unsigned long offset = 0;
    while (size > offset)
      {
	utcb->head.mtr = Mtd();
	unsigned long s = add_mappings(utcb, false, physmem + offset, size - offset, virt + offset, rights);
	Logging::printf("map self %lx -> %lx size %lx offset %lx s %lx typed %x\n", physmem, virt, size, offset, s, utcb->head.mtr.typed());
	offset = size - s;
	unsigned err;
	if ((err = idc_call(_percpu[Cpu::cpunr()].cap_pt_echo, Mtd(utcb->head.mtr.typed() * 2, 0))))
	  {
	    Logging::printf("map_self failed with %x mtr %x\n", err, utcb->head.mtr.typed());
	    res = 0;
	    break;
	  }
      }
    utcb->head.crd = old;
    if ((rights & 3) == 1)  _virt_phys.add(Region(virt, size, physmem));
    return res ? res + ofs : 0;
  }


  /**
   * Command lines need to be mapped.
   */
  char *map_string(Utcb *utcb, unsigned long src)
  {
    unsigned size = 0;
    unsigned offset = src & 0xfffull;
    src &= ~0xfffull;
    char *res = 0;
    do {
      //XXX if (res) unmap_self(res, size);
      size += 0x1000;
      res = map_self(utcb, src, size) + offset;
    } while (strnlen(res, size - offset) == (size - offset));
    return res;
  }





  bool attach_irq(unsigned gsi, unsigned sm_cap)
  {
    Logging::printf("create ec for gsi %x\n", gsi);
    Utcb *u = 0;
    unsigned cap = create_ec_helper(reinterpret_cast<unsigned>(this), &u, false, 0, _cpunr[CPUGSI % _numcpus]);
    u->msg[0] =  sm_cap;
    u->msg[1] =  gsi;
    return !create_sc(alloc_cap(), cap, Qpd(3, 10000));
  }

  bool reraise_irq(unsigned gsi)
  {
    // XXX Is there a better way to get from GSI to semaphore capability?
    return !semup(_hip->cfg_exc + 3 + (gsi & 0xFF));
  }

  unsigned attach_msi(MessageHostOp *msg, unsigned cpunr) {
    msg->msi_gsi = _hip->cfg_gsi - ++_msivector;
    unsigned irq_cap = _hip->cfg_exc + 3 + msg->msi_gsi;
    Logging::printf("%s %x cap %x cpu %x\n", __func__, msg->msi_gsi, irq_cap, cpunr);
    assign_gsi(irq_cap, cpunr, msg->value, &msg->msi_address, &msg->msi_value);
    return irq_cap;
  }


  /**
   * Converts client ptr to a pointer in our addressspace.
   * Returns true on error.
   */
  template<typename T>
  bool convert_client_ptr(ModuleInfo *modinfo, T *&ptr, unsigned size)
  {
    unsigned long offset = reinterpret_cast<unsigned long>(ptr);
    if (offset < MEM_OFFSET || modinfo->physsize + MEM_OFFSET <= offset || size > modinfo->physsize + MEM_OFFSET - offset)
      return true;
    ptr = reinterpret_cast<T *>(offset + modinfo->mem - MEM_OFFSET);
    return false;
  }

  /**
   * Prepare UTCB for receiving a new cap.
   */
  void
  prepare_cap_recv(Utcb *utcb)
  {
    //XXX opening a CRD for everybody is a security risk, as another client could have alread mapped something at this cap!
    // alloc new cap for next request
    utcb->head.crd = (alloc_cap() << Utcb::MINSHIFT) | 3;
    utcb->head.mtr = Mtd(1, 0);
  }


  /**
   * Handle an attach request.
   */
  template <class CONSUMER, class PRODUCER>
  void handle_attach(ModuleInfo *modinfo, PRODUCER &res, Utcb *utcb)
  {
    CONSUMER *con = reinterpret_cast<CONSUMER *>(utcb->msg[1]);
    if (convert_client_ptr(modinfo, con, sizeof(CONSUMER)))
      Logging::printf("(%x) consumer %p out of memory %lx\n", modinfo - _modinfo, con, modinfo->physsize);
    else
      {
	res = PRODUCER(con, utcb->head.crd >> Utcb::MINSHIFT);
	utcb->msg[0] = 0;
      }

    prepare_cap_recv(utcb);
  }


  /**
   * Create the needed host devices aka instantiate the drivers.
   */
  unsigned create_host_devices(Utcb *utcb, Hip *hip)
  {

    _mb = new Motherboard(new Clock(hip->freq_tsc*1000));
    global_mb = _mb;
    _mb->bus_hostop.add(this,  &Sigma0::receive_static<MessageHostOp>);
    init_timeouts();
    init_disks();
    init_network();
    _mb->parse_args(map_string(utcb, hip->get_mod(0)->aux));
    init_console();
    MessageLegacy msg3(MessageLegacy::RESET, 0);
    _mb->bus_legacy.send_fifo(msg3);
    return 0;
  }

  static void serial_send(long value) {
    if (global_mb)
      {
	MessageSerial msg(1, value);
	global_mb->bus_serial.send(msg);
      }
  }


  static void putc(void *data, int value)
  {
    if (value < 0) return;
    if (data)
      {
	PutcData *p = reinterpret_cast<PutcData *>(data);
	Screen::vga_putc(0xf00 | value, p->screen, p->regs->cursor_pos);
      }
    if (value == '\n')  serial_send('\r');
    serial_send(value);
  }

  static void fancy_output(const char *st, unsigned maxchars)
  {
    unsigned lastchar = 0;
    for (unsigned x=0; x < maxchars && st[x]; x++)
      {
	unsigned value = st[x];
	if (value == '\n' || value == '\t' || value == '\r' || value == 0) value = ' ';
	if (value == ' ' && lastchar == value) continue;
	Logging::printf("%c", value);
	lastchar = value;
      }
    Logging::printf("'\n");
  }

  unsigned create_worker_threads(Hip *hip, int cpunr) {
    for (int i=0; i < (hip->mem_offs - hip->cpu_offs) / hip->cpu_size; i++) {

      Hip_cpu *cpu = reinterpret_cast<Hip_cpu *>(reinterpret_cast<char *>(hip) + hip->cpu_offs + i*hip->cpu_size);
      if (~cpu->flags & 1 || (cpunr!= -1 && i != cpunr)) continue;
      _cpunr[_numcpus++] = i;

      Logging::printf("Cpu[%x]: %x:%x:%x\n", i, cpu->package, cpu->core, cpu->thread);

      // have we created it already?
      if (_percpu[i].cap_ec_echo)  continue;
      _percpu[i].cap_ec_echo = create_ec_helper(reinterpret_cast<unsigned>(this), 0, true, 0, i);
      _percpu[i].cap_pt_echo = alloc_cap();
      check1(1, create_pt(_percpu[i].cap_pt_echo, _percpu[i].cap_ec_echo, do_map_wrapper, Mtd()));

      Utcb *utcb = 0;
      _percpu[i].cap_ec_worker = create_ec_helper(reinterpret_cast<unsigned>(this), &utcb, true, 0, i);

      // initialize the receive window
      utcb->head.crd = (alloc_cap() << Utcb::MINSHIFT) | 3;
    }
    return 0;
  }

  /**
   * Init the pager, console and map initial resources.
   */
  unsigned __attribute__((noinline)) preinit(Utcb *utcb, Hip *hip)
  {
    // make sure we can call static functions
    utcb->head.tls = reinterpret_cast<unsigned long>(this);

    Logging::init(putc, 0);
    Logging::printf("preinit %p\n\n", hip);
    check1(1, init(hip));

    Logging::printf("create lock\n");
    _lock = Semaphore(&_lockcount, alloc_cap());
    check1(2, create_sm(_lock.sm()));

    Logging::printf("create pf echo+worker threads\n");
    check1(3, create_worker_threads(hip, Cpu::cpunr()));

    // create pf and gsi boot wrapper on this CPU
    assert(_percpu[Cpu::cpunr()].cap_ec_echo);
    check1(4, create_pt(14, _percpu[Cpu::cpunr()].cap_ec_echo, do_pf_wrapper,    Mtd(MTD_QUAL | MTD_RIP_LEN, 0)));
    unsigned gsi = _cpunr[CPUGSI % _numcpus];
    check1(5, create_pt(30, _percpu[gsi].cap_ec_echo, do_thread_startup_wrapper, Mtd(MTD_RSP | MTD_RIP_LEN, 0)));

    // map vga memory
    Logging::printf("map vga memory\n");
    _vga = map_self(utcb, 0xa0000, 1<<17);

    // keep the boot screen
    memcpy(_vga + 0x1a000, _vga + 0x18000, 0x1000);

    putcd.screen = reinterpret_cast<unsigned short *>(_vga + 0x18000);
    putcd.regs = &_vga_regs;
    _vga_regs.cursor_pos = 24*80*2;
    _vga_regs.offset = 0;
    Logging::init(putc, &putcd);


    // get all ioports
    map_self(utcb, 0, 1 << (16+Utcb::MINSHIFT), DESC_IO_ALL);

    // map all IRQs
    utcb->head.crd = Crd(0, 31, DESC_CAP_ALL).value();
    for (unsigned gsi=0; gsi < hip->cfg_gsi; gsi++) {
      utcb->msg[gsi * 2 + 0] = hip->cfg_exc + 3 + gsi;
      utcb->msg[gsi * 2 + 1] = Crd(gsi, 0, DESC_CAP_ALL).value();
    }
    unsigned res;
    if ((res = idc_call(_percpu[Cpu::cpunr()].cap_pt_echo, Mtd(hip->cfg_gsi * 2, 0))))
      Logging::panic("map IRQ semaphore() failed = %x", res);
    return 0;
  };


  /**
   * Request memory from the memmap.
   */
  static void *sigma0_memalign(unsigned long size, unsigned long align) {
    if (!size) return 0;
    if (align < sizeof(unsigned long)) align = sizeof(unsigned long);

    Sigma0 *s0 = reinterpret_cast<Sigma0 *>(myutcb()->head.tls);
    unsigned long pmem = s0->_free_phys.alloc(size, Cpu::bsr(align | 1));
    void *res;
    if (!pmem || !(res = s0->map_self(myutcb(), pmem, size))) Logging::panic("%s(%lx, %lx) EOM!\n", __func__, size, align);
    //Logging::printf("alloc(%lx,%lx) - pmem %lx ptr %p\n", size, align, pmem, res);
    memset(res, 0, size);
    return res;
  }


  /**
   * Init the memory map from the hip.
   */
  unsigned init_memmap(Utcb *utcb)
  {
    Logging::printf("init memory map\n");

    // our image
    extern char __image_start, __image_end;
    _virt_phys.add(Region(reinterpret_cast<unsigned long>(&__image_start), &__image_end - &__image_start, _hip->get_mod(0)->addr));


    for (int i=0; i < (_hip->length - _hip->mem_offs) / _hip->mem_size; i++)
      {
	Hip_mem *hmem = reinterpret_cast<Hip_mem *>(reinterpret_cast<char *>(_hip) + _hip->mem_offs) + i;
	Logging::printf("\tmmap[%2x] addr %8llx len %8llx type %2d aux %8x\n", i, hmem->addr, hmem->size, hmem->type, hmem->aux);
	if (hmem->type == 1)
	  _free_phys.add(Region(hmem->addr, hmem->size, hmem->addr));
      }
    for (int i=0; i < (_hip->length - _hip->mem_offs) / _hip->mem_size; i++)
      {
	Hip_mem *hmem = reinterpret_cast<Hip_mem *>(reinterpret_cast<char *>(_hip) + _hip->mem_offs) + i;
	if (hmem->type !=  1) _free_phys.del(Region(hmem->addr, (hmem->size+ 0xfff) & ~0xffful));

	// make sure to remove the cmdline as well
	if (hmem->type == -2 && hmem->aux)
	  _free_phys.del(Region(hmem->aux, (strlen(map_string(utcb, hmem->aux)) + 0xfff) & ~0xffful));
      }

    // reserve the very first 1MB
    _free_phys.del(Region(0, 1<<20));

    // switch to another allocator
    memalign = sigma0_memalign;
    return 0;
  }



  unsigned __attribute__((noinline)) start_modules(Utcb *utcb, unsigned long mask)
  {
    unsigned long maxptr;
    Hip_mem *mod;
    unsigned mods = 0;
    for (unsigned i=1; mod = _hip->get_mod(i); i++) {

	char *cmdline = map_string(utcb, mod->aux);
	if (!strstr(cmdline, "sigma0::attach") && mask & (1 << mods++)) {

	  // search for a free module
	  unsigned module = 1;
	  while (module < MAXMODULES && _modinfo[module].mem)  module++;

	  if (module >= MAXMODULES) {
	    Logging::printf("to much modules to start -- increase MAXMODULES in %s\n", __FILE__);
	    return __LINE__;
	  }

	  // init module parameters
	  ModuleInfo *modinfo = _modinfo + module;
	  memset(modinfo, 0, sizeof(*modinfo));
	  modinfo->mod_nr    = i;
	  modinfo->mod_count = 1;
	  char *p = strstr(cmdline, "sigma0::cpu");
	  modinfo->cpunr     = _cpunr[( p ? strtoul(p+12, 0, 0) : ++_last_affinity) % _numcpus];
	  modinfo->dma       = strstr(cmdline, "sigma0::dma");
	  modinfo->log       = strstr(cmdline, "sigma0::log");

	  Logging::printf("module(%x) '", module);
	  fancy_output(cmdline, 4096);

	  // alloc memory
	  unsigned long psize_needed = Elf::loaded_memsize(map_self(utcb, mod->addr, (mod->size + 0xfff) & ~0xffful));
	  p = strstr(cmdline, "sigma0::mem:");
	  if (p)
	    modinfo->physsize = strtoul(p+12, 0, 0) << 20;
	  else
	    modinfo->physsize = psize_needed;

	  unsigned long pmem = 0;
	  if ((psize_needed > modinfo->physsize)
	      || !(pmem = _free_phys.alloc(modinfo->physsize, 22))
	      || !((modinfo->mem = map_self(utcb, pmem, modinfo->physsize))))
	    {
	      if (pmem) _free_phys.add(Region(pmem, modinfo->physsize));
	      _free_phys.debug_dump("free phys");
	      _virt_phys.debug_dump("virt phys");
	      _free_virt.debug_dump("free virt");
	      Logging::printf("(%x) could not allocate %ld MB physmem needed %ld MB\n", module, modinfo->physsize >> 20, psize_needed >> 20);
	      return __LINE__;
	    }
	  Logging::printf("(%x) using memory: %ld MB (%lx) at %lx\n", module, modinfo->physsize >> 20, modinfo->physsize, pmem);

	  // allocate a console for it
	  alloc_console(module, cmdline);

	  /**
	   * We memset the client memory to make sure we get an
	   * deterministic run and not leak any information between
	   * clients.
	   */
	  memset(modinfo->mem, 0, modinfo->physsize);

	  // decode elf
	  maxptr = 0;
	  Elf::decode_elf(map_self(utcb, mod->addr, (mod->size + 0xfff) & ~0xffful), modinfo->mem, modinfo->rip, maxptr, modinfo->physsize, MEM_OFFSET);
	  attach_drives(cmdline, module);

	  unsigned  slen = strlen(cmdline) + 1;
	  assert(slen +  _hip->length + 2*sizeof(Hip_mem) < 0x1000);
	  modinfo->hip = new (0x1000) char[0x1000];
	  Hip * modhip = reinterpret_cast<Hip *>(modinfo->hip);
	  memcpy(reinterpret_cast<char *>(modhip) + 0x1000 - slen, cmdline, slen);

	  memcpy(modhip, _hip, _hip->mem_offs);
	  modhip->length = modhip->mem_offs;
	  modhip->append_mem(MEM_OFFSET, modinfo->physsize, 1, pmem);
	  modhip->append_mem(0, 0, -2, reinterpret_cast<unsigned long>(_hip) + 0x1000 - slen);
	  modhip->fix_checksum();
	  assert(_hip->length > modhip->length);

	  // count attached modules
	  while ((mod = _hip->get_mod(++i)) && strstr(map_string(utcb, mod->aux), "sigma0::attach"))
	    modinfo->mod_count++;
	  i--;


	  // create special portal for every module, we start at 64k, to have enough space for static fields
	  unsigned pt = 0x10000 + (module << 5);
	  assert(_percpu[modinfo->cpunr].cap_ec_worker);
	  check1(6, create_pt(pt+14, _percpu[modinfo->cpunr].cap_ec_worker, do_request_wrapper, Mtd(MTD_RIP_LEN | MTD_QUAL, 0)));
	  check1(7, create_pt(pt+30, _percpu[modinfo->cpunr].cap_ec_worker, do_startup_wrapper, Mtd()));

	  Logging::printf("create PD%s on CPU %d\n", modinfo->dma ? " with DMA" : "", modinfo->cpunr);
	  modinfo->cap_pd = alloc_cap();
	  check1(8, create_pd(modinfo->cap_pd, 0xbfffe000, Crd(pt, 5, DESC_CAP_ALL), Qpd(1, 10000), modinfo->cpunr));
	}
    }
    return 0;
  }


  /**
   * Kill the given module.
   */
  unsigned kill_module(unsigned module) {
    if (module < 1 || module > MAXMODULES || !_modinfo[module].mem || !_modinfo[module].physsize) return __LINE__;

    ModuleInfo *modinfo = _modinfo + module;

    // unmap the service portal
    revoke(Crd(0x10000 + (module << 5), 5, DESC_CAP_ALL), false); // XXX kill it

    // and the memory
    revoke_all_mem(modinfo->mem, modinfo->physsize, DESC_MEM_ALL, false);

    // change the tag
    Vprintf::snprintf(_console_data[module].tag, sizeof(_console_data[module].tag), "DEAD - CPU(%x) MEM(%ld)", modinfo->cpunr, modinfo->physsize >> 20);

    // free resources
    Region * r = _virt_phys.find(reinterpret_cast<unsigned long>(modinfo->mem));
    assert(r);
    assert(r->size >= modinfo->physsize);
    _free_phys.add(Region(r->phys, modinfo->physsize));
    modinfo->physsize = 0;
    // XXX free more, such as GSIs, IRQs, Producer, Consumer, Console...


    // XXX mark module as free -> we can not do this currently as we can not free the request portals
    //modinfo->mem = 0;
    return __LINE__;
  }


  /**
   * Assign a PCI device to a PD. It makes sure only the first will
   * get it.
   *
   * Returns 0 on success.
   */
  unsigned assign_pci_device(unsigned pd_cap, unsigned bdf, unsigned vfbdf)
  {
    for (unsigned i=0; i < MAXPCIDIRECT; i++)
      {
	if (!_pcidirect[i])
	  {
	    unsigned res = assign_pci(pd_cap, bdf, vfbdf);
	    if (!res) _pcidirect[i] = vfbdf ? vfbdf : bdf;
	    return res;
	  }
	if ((vfbdf == 0) && (bdf == _pcidirect[i]))
	  return 1;
	if (vfbdf != 0 && vfbdf == _pcidirect[i])
	  return 2;
      }
    return 3;
  }


/*
 * Sigma0 portal functions.
 */
#define PT_FUNC_NORETURN(NAME, CODE)					\
  static unsigned long NAME##_wrapper(unsigned pid, Utcb *utcb) __attribute__((regparm(1), noreturn)) \
  { reinterpret_cast<Sigma0 *>(utcb->head.tls)->NAME(pid, utcb); }	\
  									\
  void __attribute__((always_inline, noreturn))  NAME(unsigned pid, Utcb *utcb) \
  {									\
    CODE;								\
  }

#define PT_FUNC(NAME, CODE, ...)					\
  static unsigned long NAME##_wrapper(unsigned pid, Utcb *utcb) __attribute__((regparm(1))) \
  { return reinterpret_cast<Sigma0 *>(utcb->head.tls)->NAME(pid, utcb); } \
  									\
  unsigned long __attribute__((always_inline))  NAME(unsigned pid, Utcb *utcb) __VA_ARGS__ \
  {									\
    CODE;								\
    return utcb->head.mtr.value();					\
  }


  PT_FUNC_NORETURN(do_pf,
#if 0
		   _free_phys.debug_dump("free phys");
		   _free_virt.debug_dump("free virt");
		   _virt_phys.debug_dump("virt->phys");
#endif
		   Logging::panic("got #PF at %llx eip %x error %llx esi %x edi %x ecx %x\n", utcb->qual[1], utcb->eip, utcb->qual[0], utcb->esi, utcb->edi, utcb->ecx);
		   )

  PT_FUNC(do_map,
	  // make sure we have enough words to reply
	  //Logging::printf("\t\t%s(%x, %x, %x, %x) pid %x\n", __func__, utcb->head.mtr.value(), utcb->msg[0], utcb->msg[1], utcb->msg[2], pid);
	  assert(~utcb->head.mtr.untyped() & 1);
	  return Mtd(0, utcb->head.mtr.untyped()/2).value();
	  )

  PT_FUNC(do_thread_startup,
	  // XXX Make this generic.
	  utcb->eip = reinterpret_cast<unsigned long>(&do_gsi_wrapper);
	  )


  PT_FUNC_NORETURN(do_gsi,
		   unsigned char res;
		   unsigned gsi = utcb->msg[1] & 0xff;
		   bool shared = utcb->msg[1] >> 8;
		   unsigned cap_irq = utcb->msg[0];
		   {
		     SemaphoreGuard s(_lock);
		     Logging::printf("%s(%x) initial vec %x\n", __func__, cap_irq,  gsi);
		   }
		   MessageIrq msg(shared ? MessageIrq::ASSERT_NOTIFY : MessageIrq::ASSERT_IRQ, gsi);
		   while (!(res = semdown(cap_irq)))
		     {
		       SemaphoreGuard s(_lock);
		       _mb->bus_hostirq.send(msg);
		       if (msg.line == 1)
			 COUNTER_SET("absto", _timeouts.timeout());
		     }
		   {
		     SemaphoreGuard s(_lock);
		     Logging::printf("%s(%x, %x) request failed with %x\n", __func__, gsi, cap_irq, res);
		   }
		   Logging::panic("failed");
		   )


  PT_FUNC(do_startup,
	  unsigned short client = (pid & 0xffe0) >> 5;
	  ModuleInfo *modinfo = _modinfo + client;
	  {
	    SemaphoreGuard s(_lock);
	    Logging::printf("[%02x] eip %lx mem %p size %lx\n",
			    client, modinfo->rip, modinfo->mem, modinfo->physsize);
	  }

	  utcb->eip = modinfo->rip;
	  utcb->esp = reinterpret_cast<unsigned long>(_hip);
	  utcb->head.mtr = Mtd(MTD_RIP_LEN | MTD_RSP, 0);
	  )


  PT_FUNC(do_request,
	  unsigned short client = (pid & 0xffe0) >> 5;
	  ModuleInfo *modinfo = _modinfo + client;
	  COUNTER_INC("request");

	  //Logging::printf("[%02x] request (%x,%x,%x) mtr %x\n", client, utcb->msg[0],  utcb->msg[1],  utcb->msg[2], utcb->head.mtr.value());
	  // XXX check whether we got something mapped and do not map it back but clear the receive buffer instead
	  SemaphoreGuard l(_lock);
	  if (utcb->head.mtr.untyped() < 0x1000)
	    {
	      if (!request_timeouts(client, utcb) && !request_disks(client, utcb) && !request_console(client, utcb) && !request_network(client, utcb))
		switch (utcb->msg[0])
		  {
		  case REQUEST_PUTS:
		    {
		      char * buffer = reinterpret_cast<PutsRequest *>(utcb->msg+1)->buffer;
		      if (convert_client_ptr(modinfo, buffer, 4096)) return false;;
		      if (modinfo->log)
			Logging::printf("[%02x, %lx] %.*s\n",  client, _console_data[client].console, sizeof(utcb->msg)-1, buffer);
		      utcb->msg[0] = 0;
		      break;
		    }
		  case REQUEST_HOSTOP:
		    {
		      MessageHostOp *msg = reinterpret_cast<MessageHostOp *>(utcb->msg+1);
		      if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg)) goto fail;

		      switch (msg->type)
			{
			case MessageHostOp::OP_GET_MODULE:
			  {
			    if (modinfo->mod_count <= msg->module)  break;
			    Hip_mem *mod  = _hip->get_mod(modinfo->mod_nr + msg->module);
			    char *cstart  = msg->start;
			    char *s   = cstart;
			    char *cmdline = map_string(utcb, mod->aux);
			    unsigned slen = strlen(cmdline) + 1;
			    // msg destroyed!

			    // align the end of the module to get the cmdline on a new page.
			    unsigned long msize =  (mod->size + 0xfff) & ~0xffful;
			    if (convert_client_ptr(modinfo, s, msize + slen)) goto fail;

			    memcpy(s, map_self(utcb, mod->addr, (mod->size | 0xfff)+1), mod->size);
			    s += msize;
			    char *p = strstr(cmdline, "sigma0::attach");
			    unsigned clearsize = 14;
			    if (!p) { p = cmdline + slen - 1; clearsize = 0; }
			    memcpy(s, cmdline, p - cmdline);
			    memset(s + (p - cmdline), ' ', clearsize);
			    memcpy(s + (p - cmdline) + clearsize, p + clearsize, slen - (p - cmdline));
			    // build response
			    memset(utcb->msg, 0, sizeof(unsigned) + sizeof(*msg));
			    utcb->head.mtr = Mtd(1 + sizeof(*msg)/sizeof(unsigned), 0);
			    msg->start   = cstart;
			    msg->size    = mod->size;
			    msg->cmdline = cstart + msize;
			    msg->cmdlen  = slen;
			  }
			  break;
			case MessageHostOp::OP_GET_UID:
			  {
			    /**
			     * A client needs a uniq-ID for shared
			     * identification, such as MAC addresses.
			     * For simplicity we use our client number.
			     * Using a random number would also be
			     * possible.  For debugging reasons we
			     * simply increment and add the client number.
			     */
			    msg->value = (client << 8) + ++modinfo->uid;
			    utcb->msg[0] = 0;
			  }
			  break;
			case MessageHostOp::OP_ASSIGN_PCI:
			  if (modinfo->dma)
			    {
			      utcb->msg[0] = assign_pci_device(modinfo->cap_pd, msg->value, msg->len);
			      Logging::printf("assign_pci() PD %x bdf %lx vfbdf %x = %x\n", client, msg->value, msg->len, utcb->msg[0]);
			      break;
			    }
			  break;
			case MessageHostOp::OP_ATTACH_IRQ:
			  if ((msg->value & 0xff) < _hip->cfg_gsi) {
			    // XXX make sure only one gets it
			    Logging::printf("[%02x] gsi %lx granted\n", client, msg->value);
			    unsigned gsi_cap = _hip->cfg_exc + 3 + (msg->value & 0xff);
			    assign_gsi(gsi_cap, modinfo->cpunr);
			    utcb->head.mtr = Mtd(1);
			    utcb->msg[0] = 0;
			    add_mappings(utcb, false, gsi_cap << Utcb::MINSHIFT, 1 << Utcb::MINSHIFT, 0, DESC_CAP_ALL);
			  }
			  else {
			    Logging::printf("[%02x] irq request dropped %x pre %x nr %x\n", client, utcb->msg[2], _hip->cfg_exc, utcb->msg[2] >> Utcb::MINSHIFT);
			    goto fail;
			  }
			  break;
			case MessageHostOp::OP_ATTACH_MSI:
			  {
			    unsigned cap = attach_msi(msg, modinfo->cpunr);
			    utcb->msg[0] = 0;
			    utcb->head.mtr = Mtd(1 + sizeof(*msg)/ sizeof(unsigned));
			    add_mappings(utcb, false, cap << Utcb::MINSHIFT, 1 << Utcb::MINSHIFT, 0, DESC_CAP_ALL);
			  }
			  break;
			case MessageHostOp::OP_ALLOC_IOIO_REGION:
			  // XXX make sure only one gets it
			  utcb->head.mtr = Mtd(1);
			  utcb->msg[0] = 0;
			  add_mappings(utcb, false, (msg->value >> 8) << Utcb::MINSHIFT, (1 << (Utcb::MINSHIFT + msg->value & 0xff)), 0, DESC_CAP_ALL);
			  break;
			case MessageHostOp::OP_ALLOC_IOMEM:
			  {
			    // XXX make sure it is not physmem and only one gets it
			    unsigned long addr = msg->value & ~0xffful;
			    char *ptr = map_self(utcb, addr, msg->len);
			    utcb->head.mtr = Mtd(1);
			    utcb->msg[1] = 0;
			    add_mappings(utcb, false, reinterpret_cast<unsigned long>(ptr), msg->len, 0, DESC_MEM_ALL);
			    Logging::printf("[%02x] iomem %lx+%x granted from %p\n", client, addr, msg->len, ptr);
			  }
			  break;
			case MessageHostOp::OP_RERAISE_IRQ:
			case MessageHostOp::OP_GUEST_MEM:
			case MessageHostOp::OP_ALLOC_FROM_GUEST:
			case MessageHostOp::OP_VIRT_TO_PHYS:
			case MessageHostOp::OP_NOTIFY_IRQ:
			case MessageHostOp::OP_VCPU_CREATE_BACKEND:
			case MessageHostOp::OP_VCPU_BLOCK:
			case MessageHostOp::OP_VCPU_RELEASE:
			default:
			  // unhandled
			  Logging::printf("[%02x] unknown request (%x,%x,%x) dropped \n", client, utcb->msg[0],  utcb->msg[1],  utcb->msg[2]);
			  goto fail;
			}
		    }
		    break;
		  case REQUEST_PCICFG:
		    {
		      MessagePciConfig *msg = reinterpret_cast<MessagePciConfig *>(utcb->msg+1);
		      if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg))
			utcb->msg[0] = ~0x10u;
		      else
			utcb->msg[0] = !_mb->bus_hwpcicfg.send(*msg);
		    }
		    break;
		  case REQUEST_ACPI:
		    {
		      MessageAcpi *msg = reinterpret_cast<MessageAcpi *>(utcb->msg+1);
		      if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg))
			utcb->msg[0] = ~0x10u;
		      else
			if (msg->type == MessageAcpi::ACPI_GET_IRQ)
			  utcb->msg[0] = !_mb->bus_acpi.send(*msg, true);
		    }
		    break;
		  default:
		    Logging::printf("[%02x] unknown request (%x,%x,%x) dropped \n", client, utcb->msg[0],  utcb->msg[1],  utcb->msg[2]);
		    goto fail;
		  }
	      return utcb->head.mtr.value();
	    fail:
	      utcb->msg[0] = ~0x10u;
	      utcb->head.mtr = Mtd(1, 0);
	    }
	  else
	    {
	      Logging::printf("[%02x] map for %x for %llx err %llx at %x\n", client, utcb->head.mtr.value(), utcb->qual[1], utcb->qual[0], utcb->eip);

	      // we can not overmap -> thus remove all rights first if the PTE was present
	      if (utcb->qual[0] & 1) {
		revoke_all_mem(modinfo->mem, modinfo->physsize, DESC_MEM_ALL, false);
		revoke_all_mem(modinfo->hip, 0x1000, DESC_MEM_ALL, false);
	      }

	      utcb->head.mtr = Mtd(0, 0);
	      add_mappings(utcb, true, reinterpret_cast<unsigned long>(modinfo->mem), modinfo->physsize, MEM_OFFSET, DESC_MEM_ALL);
	      add_mappings(utcb, true, reinterpret_cast<unsigned long>(modinfo->hip), 0x1000, reinterpret_cast<unsigned long>(_hip), DESC_MEM_ALL);
	    }
	  )




  bool  receive(MessageHostOp &msg)
  {
    bool res = true;
    switch (msg.type)
      {
      case MessageHostOp::OP_ATTACH_MSI:
	{
	  unsigned cap = attach_msi(&msg, _cpunr[CPUGSI % _numcpus]);
	  res = attach_irq(msg.msi_gsi, cap);
	}
	break;
      case MessageHostOp::OP_ATTACH_IRQ:
	{
	  unsigned gsi = msg.value & 0xff;
	  if (_gsi & (1 << gsi)) return true;
	  _gsi |=  1 << gsi;
	  unsigned irq_cap = _hip->cfg_exc + 3 + gsi;
	  assign_gsi(irq_cap, _cpunr[CPUGSI % _numcpus]);
	  res = attach_irq(gsi, irq_cap);
	}
	break;
      case MessageHostOp::OP_RERAISE_IRQ:
	res = reraise_irq(msg.value & 0xFF);
	break;
      case MessageHostOp::OP_ALLOC_IOIO_REGION:
	break;
      case MessageHostOp::OP_ALLOC_IOMEM:
	msg.ptr = map_self(myutcb(), msg.value, msg.len, DESC_MEM_ALL);
	break;
      case MessageHostOp::OP_VIRT_TO_PHYS:
      {
	Region * r = _virt_phys.find(msg.value);
	if (r)
	  {
	    msg.phys_len = r->end() - msg.value;
	    msg.phys     = r->phys  + msg.value - r->virt;
	  }
	else
	  res = false;
      }
      break;
      case MessageHostOp::OP_GET_MODULE:
	{
	  Hip_mem *mod  = _hip->get_mod(msg.module);
	  if (mod)
	    {
	      msg.start   =  map_self(myutcb(), mod->addr, (mod->size + 0xfff) & ~0xffful);
	      msg.size    =  mod->size;
	      msg.cmdline =  map_string(myutcb(), mod->aux);
	      msg.cmdlen  =  strlen(msg.cmdline) + 1;
	    }
	  else
	    res = false;
	}
	break;
      case MessageHostOp::OP_ASSIGN_PCI:
	res = !assign_pci_device(_hip->cfg_exc + 0, msg.value, msg.len);
	break;
      case MessageHostOp::OP_GET_UID:
	msg.value = ++_uid;
	break;
      case MessageHostOp::OP_NOTIFY_IRQ:
      case MessageHostOp::OP_GUEST_MEM:
      case MessageHostOp::OP_ALLOC_FROM_GUEST:
      case MessageHostOp::OP_VCPU_CREATE_BACKEND:
      case MessageHostOp::OP_VCPU_BLOCK:
      case MessageHostOp::OP_VCPU_RELEASE:
      default:
	Logging::panic("%s - unimplemented operation %x", __PRETTY_FUNCTION__, msg.type);
      }
    return res;
  }

  /************************************************************
   *   Network support                                        *
   ************************************************************/


  /**
   * Network data.
   */
  NetworkProducer _prod_network[MAXMODULES];

  void init_network() {  _mb->bus_network.add(this, &Sigma0::receive_static<MessageNetwork>); }

  /**
   * Handle network requests from other PDs.
   */
  bool request_network(unsigned client, Utcb *utcb) {

    ModuleInfo *modinfo = _modinfo + client;

    switch(utcb->msg[0]) {
    case REQUEST_NETWORK_ATTACH:
      handle_attach<NetworkConsumer>(modinfo, _prod_network[client], utcb);
      break;
    case REQUEST_NETWORK:
      {
	MessageNetwork *msg = reinterpret_cast<MessageNetwork *>(utcb->msg+1);
	if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg))
	  utcb->msg[0] = ~0x10u;
	else
	  {
	    MessageNetwork msg2 = *msg;
	    if (convert_client_ptr(modinfo, msg2.buffer, msg2.len)) return false;
	    msg2.client = client;
	    _mb->bus_network.send(msg2);
	  }
      }
      break;
    default:  return false;
    }
    return true;
  }

  bool  receive(MessageNetwork &msg)
  {
    for (unsigned i=0; i < MAXMODULES; i++)
      if (i != msg.client) _prod_network[i].produce(msg.buffer, msg.len);
    return true;
  }


  /************************************************************
   *   DISK support                                           *
   ************************************************************/

  enum {
    MAXDISKS           = 32,
    MAXDISKREQUESTS    = DISKS_SIZE  // max number of outstanding disk requests per client
  };

  // per client data
  struct DiskData {
    DiskProducer    prod_disk;
    unsigned char   disks[MAXDISKS];
    unsigned char   disk_count;
    struct {
      unsigned char disk;
      unsigned long usertag;
    } tags [MAXDISKREQUESTS];
  } _disk_data[MAXMODULES];


  /**
   * Global init.
   */
  void init_disks() {  _mb->bus_diskcommit.add(this, &Sigma0::receive_static<MessageDiskCommit>); }

  /**
   * Attach drives to a module.
   */
  void attach_drives(char *cmdline, unsigned client)
  {
    for (char *p; p = strstr(cmdline,"sigma0::drive:"); cmdline = p + 1)
      {
	unsigned long  nr = strtoul(p+14, 0, 0);
	if (nr < _mb->bus_disk.count() && _disk_data[client].disk_count < MAXDISKS)
	  _disk_data[client].disks[_disk_data[client].disk_count++] = nr;
	else
	  Logging::printf("Sigma0: ignore drive %lx during attach!\n", nr);
      }
  }


  /**
   * Find a free disk tag for a client.
   */
  unsigned long find_free_tag(unsigned short client, unsigned char disknr, unsigned long usertag, unsigned long &tag) {

    DiskData *disk_data = _disk_data + client;

    assert (disknr < disk_data->disk_count);
    for (unsigned i = 0; i < MAXDISKREQUESTS; i++)
      if (!disk_data->tags[i].disk)
	{
	  disk_data->tags[i].disk = disknr + 1;
	  disk_data->tags[i].usertag = usertag;
	  tag = ((i+1) << 16) | client;
	  return MessageDisk::DISK_OK;
	}
    return MessageDisk::DISK_STATUS_BUSY;
  }



  /**
   * Handle disk requests from other PDs.
   */
  bool request_disks(unsigned client, Utcb *utcb) {

    ModuleInfo *modinfo = _modinfo + client;
    DiskData  *disk_data = _disk_data + client;

    switch(utcb->msg[0]) {
    case REQUEST_DISKS_ATTACH:
      handle_attach<DiskConsumer>(modinfo, disk_data->prod_disk, utcb);
      break;
    case REQUEST_DISK:
      {
	MessageDisk *msg = reinterpret_cast<MessageDisk *>(utcb->msg+1);
	if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg))
	  return false;
	else
	  {
	    MessageDisk msg2 = *msg;

	    if (msg2.disknr >= disk_data->disk_count) { msg->error = MessageDisk::DISK_STATUS_DEVICE; return Mtd(utcb->head.mtr.untyped(), 0).value(); }
	    switch (msg2.type)
	      {
	      case MessageDisk::DISK_GET_PARAMS:
		if (convert_client_ptr(modinfo, msg2.params, sizeof(*msg2.params))) return false;
		break;
	      case MessageDisk::DISK_WRITE:
	      case MessageDisk::DISK_READ:
		if (convert_client_ptr(modinfo, msg2.dma, sizeof(*msg2.dma)*msg2.dmacount)) return false;

		if (msg2.physoffset - MEM_OFFSET > modinfo->physsize) return false;
		if (msg2.physsize > (modinfo->physsize - msg2.physoffset + MEM_OFFSET))
		  msg2.physsize = modinfo->physsize - msg2.physoffset + MEM_OFFSET;
		msg2.physoffset += reinterpret_cast<unsigned long>(modinfo->mem) - MEM_OFFSET;

		utcb->msg[0] = find_free_tag(client, msg2.disknr, msg2.usertag, msg2.usertag);
		if (utcb->msg[0]) break;
		assert(msg2.usertag);
		break;
	      case MessageDisk::DISK_FLUSH_CACHE:
		break;
	      default:
		return false;
	      }
	    msg2.disknr = disk_data->disks[msg2.disknr];
	    msg->error = _mb->bus_disk.send(msg2) ? MessageDisk::DISK_OK : MessageDisk::DISK_STATUS_DEVICE;
	    utcb->msg[0] = 0;
	  }
      }
      break;
    default:
      return false;
    }
    return true;
  }


  bool  receive(MessageDiskCommit &msg)
  {
    // user provided write?
    if (msg.usertag) {
      unsigned client = msg.usertag & 0xffff;
      unsigned short index = msg.usertag >> 16;
      assert(client <  MAXMODULES);
      assert(index);
      assert(index <= MAXDISKREQUESTS);
      MessageDiskCommit item( _disk_data[client].tags[index-1].disk-1, _disk_data[client].tags[index-1].usertag, msg.status);
      if (!_disk_data[client].prod_disk.produce(item))
	Logging::panic("produce disk (%x, %x) failed\n", client, index);
      _disk_data[client].tags[index-1].disk = 0;
    }
    return true;
  }



  /************************************************************
   *   CONSOLE support                                        *
   ************************************************************/

  struct ConsoleData {
    unsigned long   console;
    StdinProducer   prod_stdin;
    char            tag[256];
  } _console_data[MAXMODULES];

  /**
   * Init the console subsystem
   */
  void init_console() {

    _mb->bus_console.add(this, &Sigma0::receive_static<MessageConsole>);
    // alloc consoles for ourself
    MessageConsole msg1;
    msg1.clientname = 0;
    _mb->bus_console.send(msg1);
    console_id = msg1.id;

    // open 3 views
    MessageConsole msg2("sigma0",  _vga + 0x18000, 0x1000, &_vga_regs);
    msg2.id = console_id;
    _mb->bus_console.send(msg2);
    msg2.name = "HV";
    msg2.ptr += 0x1000;
    _mb->bus_console.send(msg2);
    msg2.name = "boot";
    msg2.ptr += 0x1000;
    _mb->bus_console.send(msg2);
    switch_view(_mb, 0);
  }


  void alloc_console(unsigned client, const char *cmdline) {
    ModuleInfo *modinfo = _modinfo + client;

    // format the tag
    Vprintf::snprintf(_console_data[client].tag, sizeof(_console_data[client].tag), "CPU(%x) MEM(%ld) %s", modinfo->cpunr, modinfo->physsize >> 20, cmdline);

    MessageConsole msg1;
    msg1.clientname = _console_data[client].tag;
    if (_mb->bus_console.send(msg1))  _console_data[client].console = msg1.id;

  };

  /**
   * Switch to our view.
   */
  static void switch_view(Motherboard *mb, int view=0)
  {
    MessageConsole msg;
    msg.type = MessageConsole::TYPE_SWITCH_VIEW;
    msg.id = console_id;
    msg.view = view;
    mb->bus_console.send(msg);
  }


  /**
   * Handle console requests from other PDs.
   */
  bool request_console(unsigned client, Utcb *utcb) {
    ModuleInfo *modinfo = _modinfo + client;

    switch(utcb->msg[0]) {
    case REQUEST_STDIN_ATTACH:
      handle_attach<StdinConsumer>(modinfo, _console_data[client].prod_stdin, utcb);
      break;
    case REQUEST_CONSOLE:
      {
	MessageConsole *msg = reinterpret_cast<MessageConsole *>(utcb->msg+1);
	if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg)) return false;
	{
	  MessageConsole msg2 = *msg;
	  if ((msg2.type != MessageConsole::TYPE_ALLOC_VIEW
	       && msg2.type != MessageConsole::TYPE_SWITCH_VIEW
	       && msg2.type != MessageConsole::TYPE_GET_MODEINFO
	       && msg2.type != MessageConsole::TYPE_GET_FONT)
	      || (msg2.type == MessageConsole::TYPE_ALLOC_VIEW
		  && (convert_client_ptr(modinfo, msg2.ptr, msg2.size)
		      || convert_client_ptr(modinfo, msg2.name, 4096)
		      || convert_client_ptr(modinfo, msg2.regs, sizeof(*msg2.regs))))
	      || (msg2.type == MessageConsole::TYPE_GET_FONT
		  &&  convert_client_ptr(modinfo, msg2.ptr, 0x1000))
	      || (msg2.type == MessageConsole::TYPE_GET_MODEINFO
		  && convert_client_ptr(modinfo, msg2.info, sizeof(*msg2.info)))
	      || !_console_data[client].console)
	    break;
	  msg2.id = _console_data[client].console;
	  // alloc a new console and set the name from the commandline
	  utcb->msg[0] = !_mb->bus_console.send(msg2);
	  if (!utcb->msg[0])      msg->view = msg2.view;
	}
      }
      break;
    default: return false;
    }
    return true;
  }



  bool  receive(MessageConsole &msg) {

    switch (msg.type) {
    case MessageConsole::TYPE_KEY:
      // forward the key to the named console
      for (unsigned i=1; i < MAXMODULES; i++)
	if (_console_data[i].console == msg.id)
	  {
	    MessageKeycode item(msg.view, msg.keycode);
	    _console_data[i].prod_stdin.produce(item);
	    return true;
	  }
      Logging::printf("drop key %x at console %x.%x\n", msg.keycode, msg.id, msg.view);
      break;
    case MessageConsole::TYPE_RESET:
      if (msg.id == 0)
	{
	  Logging::printf("flush disk caches for reboot\n");
	  for (unsigned i=0; i <  _mb->bus_disk.count(); i++)
	    {
	      MessageDisk msg2(MessageDisk::DISK_FLUSH_CACHE, i, 0, 0, 0, 0, 0, 0);
	      if (!_mb->bus_disk.send(msg2))  Logging::printf("could not flush disk %d\n", i);
	    }
	  Logging::printf("reset System\n");
	  MessageIOOut msg1(MessageIOOut::TYPE_OUTB, 0xcf9, 2);
	  MessageIOOut msg2(MessageIOOut::TYPE_OUTB, 0xcf9, 6);
	  MessageIOOut msg3(MessageIOOut::TYPE_OUTB,  0x92, 1);
	  _mb->bus_hwioout.send(msg1, true);
	  _mb->bus_hwioout.send(msg2, true);
	  _mb->bus_hwioout.send(msg3, true);
	  return true;
	}
      break;
    case MessageConsole::TYPE_START:
      {
	unsigned res = start_modules(myutcb(), 1 << msg.id);
	if (res)
	  Logging::printf("start modules(%d) = %x\n", msg.id, res);
      }
      return true;
    case MessageConsole::TYPE_KILL:
      {
	unsigned res = kill_module(msg.id);
	if (res)   Logging::printf("kill module(%d) = %x\n", msg.id, res);
      }
      return true;
    case MessageConsole::TYPE_DEBUG:
      switch (msg.id) {
      case 0:  _mb->dump_counters(); break;
      case 1:  check_timeouts(); break;
      case 2:
	Logging::printf("to %llx now %llx\n", _timeouts.timeout(), _mb->clock()->time());
	break;
      case 3:
	if (msg.id == 3) {
	  static unsigned unmap_count;
	  unmap_count--;
	  for (unsigned i = 1; i <= MAXMODULES; i++)
	    if (_modinfo[i].mem) {
	      unsigned rights = (unmap_count & 7) << 2;
	      Logging::printf("revoke all rights %x\n", rights);
	      revoke_all_mem(_modinfo[i].mem, _modinfo[i].physsize, rights, false);
	    }
	}
	break;
      }
      Logging::printf("DEBUG(%x) = %x\n", msg.id, nova_syscall2(254, msg.id));
      return true;
    case MessageConsole::TYPE_ALLOC_CLIENT:
    case MessageConsole::TYPE_ALLOC_VIEW:
    case MessageConsole::TYPE_SWITCH_VIEW:
    case MessageConsole::TYPE_GET_MODEINFO:
    case MessageConsole::TYPE_GET_FONT:
    default:
      break;
    }
    return false;
  }




  /************************************************************
   *   TIMEOUT support                                        *
   ************************************************************/

  TimeoutList<MAXMODULES+2> _timeouts;
  TimerProducer             _prod_timer[MAXMODULES];


  void init_timeouts() {

    // prealloc timeouts for every module
    _timeouts.init();
    for (unsigned i=0; i < MAXMODULES; i++) _timeouts.alloc();
    _mb->bus_timer.add(this,   &Sigma0::receive_static<MessageTimer>);
    _mb->bus_timeout.add(this, &Sigma0::receive_static<MessageTimeout>);
  }


  /**
   * Handle timeout requests from other PDs.
   */
  bool request_timeouts(unsigned client, Utcb *utcb) {

    ModuleInfo *modinfo = _modinfo + client;
    switch(utcb->msg[0]) {
    case REQUEST_TIMER_ATTACH:
      assert(client < sizeof(_prod_timer)/sizeof(_prod_timer[0]));
      handle_attach<TimerConsumer>(modinfo, _prod_timer[client], utcb);
      break;
    case REQUEST_TIMER:
      {
	MessageTimer *msg = reinterpret_cast<MessageTimer *>(utcb->msg+1);
	if (utcb->head.mtr.untyped()*sizeof(unsigned) < sizeof(unsigned) + sizeof(*msg))
	  utcb->msg[0] = ~0x10u;
	else
	  if (msg->type == MessageTimer::TIMER_REQUEST_TIMEOUT)
	    {
	      msg->nr = client;
	      if (_mb->bus_timer.send(*msg))
		utcb->msg[0] = 0;
	    }
      }
      break;
    case REQUEST_TIME:
      {
	MessageTime msg;
	if (_mb->bus_time.send(msg))
	  {
	    // we assume the same mb->clock() source here
	    *reinterpret_cast<MessageTime *>(utcb->msg+1) = msg;
	    utcb->head.mtr = Mtd((sizeof(msg)+2*sizeof(unsigned) - 1)/sizeof(unsigned), 0);
	    utcb->msg[0] = 0;
	  }
      }
      break;
    default:
      return false;
    }
    return true;
  }

  /**
   * Check whether timeouts have fired.
   */
  void check_timeouts() {

    COUNTER_INC("check_to");
    timevalue now = _mb->clock()->time();
    unsigned nr;
    while ((nr = _timeouts.trigger(now)))
      {
	_timeouts.cancel(nr);
	MessageTimeout msg1(nr);
	_mb->bus_timeout.send(msg1);
      }
    if (_timeouts.timeout() != ~0ULL)
      {
	// update timeout upstream
	MessageTimer msg(MessageTimeout::HOST_TIMEOUT, _timeouts.timeout());
	_mb->bus_timer.send(msg);
      }
  }


  bool  receive(MessageTimer &msg)
  {
    switch (msg.type)
      {
      case MessageTimer::TIMER_NEW:
	msg.nr = _timeouts.alloc();
	return true;
      case MessageTimer::TIMER_REQUEST_TIMEOUT:
	if (msg.nr != MessageTimeout::HOST_TIMEOUT)
	  {
	    if (!_timeouts.request(msg.nr, msg.abstime))
	      {
		MessageTimer msg2(MessageTimeout::HOST_TIMEOUT, _timeouts.timeout());
		_mb->bus_timer.send(msg2);
	      }
	  }
	break;
      default:
	return false;
      }
    return true;
  }


  bool  receive(MessageTimeout &msg)
  {
    if (msg.nr < MAXMODULES)
      {
	TimerItem item(Cpu::rdtsc());
	assert(msg.nr < sizeof(_prod_timer)/sizeof(_prod_timer[0]));
	_prod_timer[msg.nr].produce(item);
	return true;
      }
    if (msg.nr == MessageTimeout::HOST_TIMEOUT)  check_timeouts();
    return false;
  }

  /************************************************************
   * Application                                              *
   ************************************************************/

  void  __attribute__((noreturn)) run(Utcb *utcb, Hip *hip)
  {
    unsigned res;
    if ((res = preinit(utcb, hip)))              Logging::panic("init() failed with %x\n", res);
    Logging::printf("Sigma0.nova:  hip %p caps %x memsize %x\n", hip, hip->cfg_cap, hip->mem_size);

    if ((res = init_memmap(utcb)))               Logging::panic("init memmap failed %x\n", res);
    if ((res = create_worker_threads(hip, -1)))  Logging::panic("create worker threads failed %x\n", res);
    if ((res = create_host_devices(utcb, hip)))  Logging::panic("create host devices failed %x\n", res);
    Logging::printf("start modules\n");
    for (unsigned i=0; i <= repeat; i++)
      if ((res = start_modules(utcb, ~startlate)))    Logging::printf("start modules failed %x\n", res);

    Logging::printf("INIT done\n");

    // unblock the worker and IRQ threads
    _lock.up();

    // block ourself since we have finished initialization
    block_forever();
  }

  static void start(Hip *hip, Utcb *utcb) asm ("start") __attribute__((noreturn));
  Sigma0() :  _numcpus(0), _modinfo(), _gsi(0), _pcidirect()  {}
};


void Sigma0::start(Hip *hip, Utcb *utcb) {
  static Sigma0 sigma0;
  sigma0.run(utcb, hip);
}


void  do_exit(const char *msg)
{
  Logging::printf("__exit(%s)\n", msg);
  if (global_mb) Sigma0::switch_view(global_mb);

  while (1)
    asm volatile ("ud2a" : : "a"(msg));
}
