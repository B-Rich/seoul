/*
 * Main sigma0 code.
 *
 * Copyright (C) 2008-2009, Bernhard Kauer <kauer@tudos.org>
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
#include "driver/logging.h"
#include "models/keyboard.h"
#include <sigma0.h>
#include "sys/elf.h"
#include "vmm/motherboard.h"
#include "host/screen.h"

using namespace Nova;

bool      startlate;
bool      noswitch;
unsigned  repeat;
unsigned  serial_port;
unsigned  console_id;
PARAM(startlate,  startlate = true;, "startlate - do not start all domains at bootup" );
PARAM(repeat,  repeat = argv[0], "repeat the domain start" );
PARAM(noswitch,  noswitch = true;, "do not switch to sigma0 console" );

Motherboard *global_mb;
class Sigma0 : public Sigma0Base, public StaticReceiver<Sigma0>
{
private:
  // capabilities
  unsigned  _cap_echo;
  unsigned  _cap_ec_requester;

  char     *args;

  // irq+worker synchronisation
  long      _lockcount;
  Semaphore _lock;
  // vga
  char    * _vga;
  VgaRegs   _vga_regs;
  // device data
  Motherboard *_mb;
  volatile unsigned _disk_requests_completed;

  // module data
  static const unsigned MAXDISKS = 32;
  static const unsigned long MEM_OFFSET = 1 << 31;
  static const unsigned MAXMODULES = 64;
  static const unsigned MAXDISKREQUESTS = DISKS_SIZE; // max number of outstanding disk requests per client
  struct ModuleInfo
  {
    Hip * hip;
    unsigned mod_nr;
    unsigned mod_count;
    char     *cmdline;
    unsigned long rip;
    char     *mem;
    unsigned long pmem;
    unsigned long physsize;
    unsigned long textbase;
    unsigned long console;
    bool          log;
    StdinProducer prod_stdin;
    unsigned char disks[MAXDISKS];
    unsigned char disk_count;
    struct {
      unsigned char disk;
      unsigned long usertag;
    } tags [MAXDISKREQUESTS];
    DiskProducer prod_disk;
    TimerProducer prod_timer;
    NetworkProducer prod_network;
    unsigned uid;
  } _modinfo[MAXMODULES];
  TimeoutList<MAXMODULES+2> _timeouts;
  unsigned _modcount;


  const char *debug_getname() { return "Sigma0"; };


  /**
   * Find a free tag for a client.
   */
  unsigned long find_free_tag(unsigned short client, unsigned char disknr, unsigned long usertag, unsigned long &tag)
  {
    struct ModuleInfo *module = _modinfo + client;

    assert (disknr < module->disk_count);
    for (unsigned i = 0; i < MAXDISKREQUESTS; i++)
      if (!module->tags[i].disk)
	{
	  module->tags[i].disk = disknr + 1;
	  module->tags[i].usertag = usertag;
	  tag = ((i+1) << 16) | client;
	  return MessageDisk::DISK_OK;
	}
    return MessageDisk::DISK_STATUS_BUSY;
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
   * Handle an attach request.
   */
  template <class CONSUMER, class PRODUCER>
  void handle_attach(ModuleInfo *modinfo, PRODUCER &res, Utcb *utcb)
  {
    //Logging::printf("\t\t%s(%x, %x) pid %x request crd %x\n", __func__, utcb->head.mtr.value(), utcb->msg[1], utcb->head.pid, utcb->head.crd);
    CONSUMER *con = reinterpret_cast<CONSUMER *>(utcb->msg[1]);
    if (convert_client_ptr(modinfo, con, sizeof(CONSUMER)))
      Logging::printf("(%x) consumer %p out of memory %lx\n", utcb->head.pid, con, modinfo->physsize);
    else
      {
	res = PRODUCER(con, utcb->head.crd >> MAPMINSHIFT);
	utcb->msg[0] = 0;
      }

    //XXX opening a CRD for everybody is a security risk, as another client could have alread mapped something at this cap!
    // alloc new cap for next request
    utcb->head.crd = (_cap_free++ << MAPMINSHIFT) | 3;
    utcb->head.mtr = untyped_words(1);
  }

  /**
   * Create the needed host devices aka instantiate the drivers.
   */
  unsigned create_host_devices(Hip *hip)
  {

    // prealloc timeouts for every module
    _timeouts.init();
    for (unsigned i=0; i < MAXMODULES; i++) _timeouts.alloc();

    if (hip->tsc_freq_khz != 0) {
      _mb = new Motherboard(new Clock(hip->tsc_freq_khz*1000));
    } else {
      // XXX Evil workaround for bogus TSC.
      _mb = new Motherboard(new Clock(2000000000UL));
    }

    global_mb = _mb;
    _mb->bus_hostop.add(this,  &Sigma0::receive_static<MessageHostOp>);
    _mb->bus_diskcommit.add(this, &Sigma0::receive_static<MessageDiskCommit>);
    _mb->bus_console.add(this, &Sigma0::receive_static<MessageConsole>);
    _mb->bus_timer.add(this,   &Sigma0::receive_static<MessageTimer>);
    _mb->bus_timeout.add(this, &Sigma0::receive_static<MessageTimeout>);
    _mb->bus_network.add(this, &Sigma0::receive_static<MessageNetwork>);
    _mb->parse_args(args);

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
    if (!noswitch) switch_view(_mb, 0);

    MessageLegacy msg3(MessageLegacy::RESET, 0);
    _mb->bus_legacy.send_fifo(msg3);
    _mb->bus_disk.debug_dump();
    return 0;
  }

  static void serial_send(long value) {
    if (serial_port && global_mb)
      {

	MessageIOIn  msg1(MessageIOIn ::TYPE_INB,  serial_port+0x5);
	msg1.value = 0x20;
	MessageIOOut msg2(MessageIOOut::TYPE_OUTB, serial_port, value);
	while (global_mb->bus_hwioin.send(msg1) && ~msg1.value & 0x20)
	  Cpu::pause();
	global_mb->bus_hwioout.send(msg2);
      }
  }
  struct PutcData
  {
    VgaRegs        *regs;
    unsigned short *screen;
  };


  static void putc(void *data, long value)
  {
    if (data)
      {
	PutcData *p = reinterpret_cast<PutcData *>(data);
	Screen::putc(0xf00 | value, p->screen, p->regs->cursor_pos);
      }
    if (value == '\n')  serial_send('\r');
    serial_send(value);
  }

  /**
   * Init the pager and the console.
   */
  unsigned preinit(Hip *hip)
  {
    Logging::init(putc, 0);
    Logging::printf("preinit %p\n\n", hip);
    check(init(hip));

    Logging::printf("create pf echo thread\n");
    debug_ec_name("s pf", 0);
    unsigned cap = create_ec_helper(reinterpret_cast<unsigned>(this));
    _cap_echo = _cap_free++;
    create_pt(_cap_echo, cap, empty_message(), do_map_wrapper);
    create_pt(14,        cap, MTD_QUAL | MTD_EIP, do_pf_wrapper);

    // get all ioports
    map_self(_boot_utcb, 0, 1 << (16+MAPMINSHIFT), CRD_IO);

    // map our arguments
    args = map_string(_boot_utcb, hip_module(hip, 0)->aux);

    // map vga memory
    Logging::printf("map vga memory\n");
    _vga = map_self(_boot_utcb, 0xa0000, 1<<17);

    // keep the boot screen
    memcpy(_vga + 0x1a000, _vga + 0x18000, 0x1000);

    static PutcData putcd;
    putcd.screen = reinterpret_cast<unsigned short *>(_vga + 0x18000);
    putcd.regs = &_vga_regs;
    _vga_regs.cursor_pos = 24*80*2;
    _vga_regs.offset = 0;
    Logging::init(putc, &putcd);

    // get serial port from the bios data area
    char *zeropage = map_self(_boot_utcb, 0x0, 1<<12);
    serial_port = *reinterpret_cast<unsigned short *>(zeropage + 0x400);
    //unmap(zeropage, 1<<12);

    return 0;
  };


  /**
   * Init the memory map from the hip.
   */
  unsigned __attribute__((noinline)) init_memmap(Utcb *utcb)
  {
    Logging::printf("init memory map\n");

    for (unsigned i = 0; i < hip_mem_desc_no(_hip); i++) {
      HipMem *hmem = hip_mem_desc(_hip, i);
      Logging::printf("\tmmap[%2x] addr %8llx len %8llx type %2d aux %8x\n", i, hmem->address, hmem->size, hmem->type, hmem->aux);
      if (hmem->type == MEM_AVAILABLE)
	_free_phys.add(Region(hmem->address, hmem->size, hmem->address));
    }

    for (unsigned i = 0; i < hip_mem_desc_no(_hip); i++) {
      HipMem *hmem = hip_mem_desc(_hip, i);
      if (hmem->type !=  MEM_AVAILABLE) _free_phys.del(Region(hmem->address, (hmem->size+ 0xfff) & ~0xffful));
      // make sure to remove the cmdline
      if (hmem->type == MEM_MULTIBOOT && hmem->aux)  {
	_free_phys.del(Region(hmem->aux, (strlen(map_string(utcb, hmem->aux)) + 0xfff) & ~0xffful));
      }
    }

    // reserve the very first 1MB
    _free_phys.del(Region(0, 1<<20));
    return 0;
  }


  void attach_drives(char *cmdline, ModuleInfo *modinfo)
  {
    for (char *p; p = strstr(cmdline,"sigma0::drive:"); cmdline = p + 1)
      {
	unsigned long  nr = strtoul(p+14, 0, 0);
	if (nr < _mb->bus_disk.count() && modinfo->disk_count < MAXDISKS)
	  modinfo->disks[modinfo->disk_count++] = nr;
	else
	  Logging::printf("Sigma0: ignore drive %lx during attach!\n", nr);
      }
  }


  unsigned create_requester()
  {
    _lock = Semaphore(&_lockcount, _cap_free++);
    check(create_sm(_lock.sm(), 0));
    Utcb *utcb;
    debug_ec_name("s rq", 0);
    _cap_ec_requester = create_ec_helper(reinterpret_cast<unsigned>(this), &utcb);

    // initialize the receive window
    utcb->head.crd = (_cap_free++ << MAPMINSHIFT) | 3;
    return 0;
  }


  void hip_append_mem(Hip *hip, uint64_t addr, uint64_t size, int type, uint32_t aux)
  {
    HipMem * mem = reinterpret_cast<HipMem *>(reinterpret_cast<char *>(hip) + hip->length);
    hip->length += hip->mem_desc_size;
    mem->address = addr;
    mem->size = size;
    mem->type = type;
    mem->aux = aux;
  }

  unsigned __attribute__((noinline)) start_modules(Utcb *utcb, unsigned mask)
  {
    unsigned long maxptr;
    HipMem *mod;
    unsigned mods = 0;
    for (unsigned i=0; mod = hip_module(_hip, i); i++) {
      char *cmdline = map_string(utcb, mod->aux);
      if (!strstr(cmdline, "sigma0::attach") && mask & (1 << mods++))
	{
	  bool vcpus = strstr(cmdline, "sigma0::vmm");
	  if (++_modcount >= MAXMODULES)
	    {
	      Logging::printf("to much modules to start -- increase MAXMODULES in %s\n", __FILE__);
	      _modcount--;
	      return __LINE__;
	    }
	  ModuleInfo *modinfo = _modinfo + _modcount;
	  memset(modinfo, 0, sizeof(*modinfo));
	  modinfo->cmdline   = cmdline;
	  modinfo->mod_nr    = i;
	  modinfo->mod_count = 1;
	  modinfo->log = strstr(cmdline, "sigma0::log");
	  Logging::printf("module(%x) '%s'\n", _modcount, cmdline);

	  // alloc memory
	  modinfo->physsize = 32 << 20;
	  char *p = strstr(cmdline, "sigma0::mem:");
	  if (p) modinfo->physsize = strtoul(p+12, 0, 0) << 20;
	  if (!(modinfo->pmem = _free_phys.alloc(modinfo->physsize, 22)) || !((modinfo->mem = map_self(utcb, modinfo->pmem, modinfo->physsize))))
	    {
	      _free_phys.debug_dump("free phys");
	      Logging::printf("(%x) could not allocate %ld MB physmem\n", _modcount, modinfo->physsize >> 20);
	      _modcount--;
	      return __LINE__;
	    }
	  Logging::printf("(%x) using memory: %ld MB at %lx\n", _modcount, modinfo->physsize >> 20, modinfo->pmem);

	  // allocate a console for it
	  MessageConsole msg1;
	  msg1.clientname = cmdline;
	  if (_mb->bus_console.send(msg1))  modinfo->console = msg1.id;


	  //memset(phys_mem, 0, modinfo->physsize);

	  // decode elf
	  maxptr = 0;
	  Elf::decode_elf(map_self(utcb, mod->address, (mod->size + 0xfff) & ~0xffful), modinfo->mem, modinfo->rip, maxptr, modinfo->physsize, MEM_OFFSET);
	  unsigned  slen = strlen(cmdline) + 1;
	  unsigned long mod_end = (modinfo->physsize - slen) & ~0xfff;
	  debug_ec_name(cmdline, 0);

	  // XXX fiasco hack to workaround himem and kmem allocator!
	  if (strstr(cmdline, "sigma0::fiascohack") && (mod_end > 0x4400000)) mod_end -= 0x4000000;
	  memcpy(modinfo->mem + mod_end, cmdline, slen);
	  attach_drives(cmdline, modinfo);

	  modinfo->hip = reinterpret_cast<Hip *>(modinfo->mem + mod_end - 0x1000);
	  memcpy(modinfo->hip, _hip, _hip->length);
	  modinfo->hip->length = modinfo->hip->mem_offset;
	  hip_append_mem(modinfo->hip, MEM_OFFSET, modinfo->physsize, 1, modinfo->pmem);
	  hip_append_mem(modinfo->hip, 0, 0, -2, mod_end + MEM_OFFSET);

	  // attach modules
	  for (;(mod = hip_module(_hip, ++i)) && strstr(map_string(utcb, mod->aux), "sigma0::attach"); 
		 modinfo->mod_count++)
	    ;
	  i--;
	  hip_fix_checksum(modinfo->hip);
	  assert(_hip->length > modinfo->hip->length);

	  // create special portal for every module, we start at 64k, to have enough space for static fields
	  unsigned pt = ((_cap_free+0xffff) & ~0xffff) + (_modcount << 4);
	  check(create_pt(pt+14, _cap_ec_requester,  MTD_EIP | MTD_QUAL, do_request_wrapper));
	  Logging::printf(vcpus ? "create VMM\n" : "create PD\n");
	  check(create_pd(_cap_free++, 0xbfffe000, obj_range(pt, 4), qpd(1, 10000), vcpus));
	}
    }
    return 0;
  }


  char *map_self(Utcb *utcb, unsigned long physmem, unsigned long size, unsigned rights = 0x1c | 1)
  {
    assert(size);
    unsigned long virt = 0;
    if ((rights & 3) == CRD_MEM)
      {
	unsigned long s = _virt_phys.find_phys(physmem, size);
	if (s)  return reinterpret_cast<char *>(s);
	virt = _free_virt.alloc(size, Cpu::minshift(s, size, 22));
	if (!virt) return 0;
      }
    unsigned old = utcb->head.crd;
    utcb->head.crd = 20<<7 | rights; // Map 32-bit of memory.
    char *res = reinterpret_cast<char *>(virt);
    unsigned long offset = 0;
    while (size > offset)
      {
	utcb->head.mtr = empty_message();
	unsigned long s = utcb_add_mappings(utcb, false, physmem + offset, size - offset, virt + offset, rights);
	Logging::printf("map self %lx -> %lx size %lx offset %lx s %lx\n", physmem, virt, size, offset, s);
	offset = size - s;
	unsigned err;
	if ((err = call(DCALL, _cap_echo, untyped_words(mtd_typed(utcb->head.mtr)*2))))
	  {
	    Logging::printf("map_self failed with %x\n", mtd_typed(utcb->head.mtr));
	    res = 0;
	    break;
	  }
      }
    utcb->head.crd = old;
    if ((rights & 3) == 1)  _virt_phys.add(Region(virt, size, physmem));
    return res;
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
      //if (res) unmap(res, size);
      size += 0x1000;
      res = map_self(utcb, src, size) + offset;
      //Logging::printf("%s size %x offset %x %lx -> %p\n", __func__, size, offset, src, res);
    } while (strnlen(res, size - offset) == (size - offset));
    return res;
  }


  void check_timeouts()
  {
    COUNTER_INC("check_to");
    static timevalue old;
    timevalue now = _mb->clock()->time();
    unsigned diff = (now - _timeouts.timeout());
    unsigned diff2 = (now - old);
    old = now;
    static unsigned c;
    //   if (now > _timeouts.timeout() && !((c++) & 0xf))   Logging::printf("%s now %llx to %llx d1 %d d2 %d\n", __func__, now, _timeouts.timeout(), diff / 2666, diff2 / 2666);
    unsigned nr;
    bool reprogram = false;
    while ((nr = _timeouts.trigger(now)))
      {
	reprogram |= _timeouts.cancel(nr) == 0;
	MessageTimeout msg1(nr);
	_mb->bus_timeout.send(msg1);
      }
    if (reprogram) {} // update timeout in HPET
  }


#define PT_FUNC_NORETURN(NAME, CODE)					\
  NOVA_REGPARM(0) NOVA_NORETURN static void  NAME##_wrapper(Utcb *utcb)	\
  { reinterpret_cast<Sigma0 *>(utcb->head.tls)->NAME(utcb); }		\
  									\
  NOVA_NORETURN void NAME(Utcb *utcb)					\
  {									\
    CODE;								\
  }

#define PT_FUNC(NAME, CODE, ...)					\
  NOVA_REGPARM(0) static void  NAME##_wrapper(Utcb *utcb)		\
  { reinterpret_cast<Sigma0 *>(utcb->head.tls)->NAME(utcb); }		\
									\
  void NAME(Utcb *utcb) __VA_ARGS__					\
  {									\
    CODE;								\
  }

#include "pt_funcs.h"


public:

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


  bool  receive(MessageTimer &msg)
  {
    switch (msg.type)
      {
      case MessageTimer::TIMER_NEW:
	msg.nr = _timeouts.alloc();
	return true;
      case MessageTimer::TIMER_CANCEL_TIMEOUT:
	_timeouts.cancel(msg.nr);
	break;
      case MessageTimer::TIMER_REQUEST_TIMEOUT:
	//Logging::printf("request to %x %llx\n", msg.nr, msg.abstime);
	_timeouts.request(msg.nr, msg.abstime);
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
	//Logging::printf("> timeout %x\n", msg.nr);
	TimerItem item(Cpu::rdtsc());
	_modinfo[msg.nr].prod_timer.produce(item);
	//Logging::printf("< timeout %x\n", msg.nr);
	return true;
      }
    return false;
  }



  bool  receive(MessageConsole &msg)
  {
    switch (msg.type)
      {
      case MessageConsole::TYPE_KEY:
	// forward the key to the named console
	for (unsigned i=1; i < MAXMODULES; i++)
	  if (_modinfo[i].console == msg.id)
	    {
	      //Logging::printf("got key %x for console %x.%x -> module %d\n", msg.keycode, msg.id, msg.view, i);
	      MessageKeycode item(msg.view, msg.keycode);
	      _modinfo[i].prod_stdin.produce(item);
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
	    MessageIOOut msg1(MessageIOOut::TYPE_OUTB, 0xcf9, 6);
	    MessageIOOut msg2(MessageIOOut::TYPE_OUTB, 0x92, 1);
	    _mb->bus_hwioout.send(msg1);
	    _mb->bus_hwioout.send(msg2);
	    return true;
	  }
	break;
      case MessageConsole::TYPE_START:
	{
	  unsigned res = start_modules(myutcb(), 1 << (msg.id+1));
	  if (res)
	    Logging::printf("start modules(%d) = %x\n", msg.id, res);
	  return true;
	}
      case MessageConsole::TYPE_DEBUG:
	if (!msg.id) _mb->dump_counters();
	//Logging::printf("DEBUG(%x) = %x\n", msg.id, syscall(254, msg.id, 0, 0, 0));
	Logging::printf("DEBUG(%x) -> not implemented\n", msg.id);
	return true;
      case MessageConsole::TYPE_ALLOC_CLIENT:
      case MessageConsole::TYPE_ALLOC_VIEW:
      case MessageConsole::TYPE_SWITCH_VIEW:
      case MessageConsole::TYPE_GET_MODEINFO:
      default:
	break;
      }
    return false;
  }


  bool  receive(MessageHostOp &msg)
  {
    bool res = true;
    switch (msg.type)
      {
      case MessageHostOp::OP_ALLOC_IOIO_REGION:
	break;
      case MessageHostOp::OP_ATTACH_HOSTIRQ:
	{
	  debug_ec_name("s ", msg.value);
	  Logging::printf("create ec for irq\n");
	  unsigned cap = create_ec_helper(reinterpret_cast<unsigned>(this));
	  unsigned pt = _cap_free++;
	  create_pt(pt, cap, empty_message(), do_gsi_wrapper);
	  if (!create_sc(_cap_free++, cap, qpd(2, 10000)))
	    {
	      myutcb()->msg[0] = _hip->pre + msg.value;
	      res = !call(SEND, pt , untyped_words(1));
	      Logging::printf("send for irq\n");
	    }
	  else
	    res = false;
	}
	break;
      case MessageHostOp::OP_ALLOC_IOMEM_REGION:
	{
	  unsigned shift = msg.value & 0x1f;
	  msg.ptr = map_self(myutcb(), msg.value & ~((1u << shift) - 1), 1u << shift);
	}
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
	  HipMem *mod  = hip_module(_hip, msg.module);
	  if (mod)
	    {
	      msg.start   =  map_self(myutcb(), mod->address, (mod->size + 0xfff) & ~0xffful);
	      msg.size    =  mod->size;
	      msg.cmdline =  map_string(myutcb(), mod->aux);
	      msg.cmdlen  =  strlen(msg.cmdline) + 1;
	    }
	  else
	    res = false;

	}
	break;
      case MessageHostOp::OP_UNMASK_IRQ:
      case MessageHostOp::OP_GET_UID:
      case MessageHostOp::OP_GUEST_MEM:
      default:
	Logging::panic("%s - unimplemented operation %x", __PRETTY_FUNCTION__, msg.type);
      }
    return res;
  }


  bool  receive(MessageDiskCommit &msg)
  {
    // user provided write?
    if (msg.usertag) {
      //Logging::printf("s0: commitbus->write(%lx, %x)\n", msg.usertag, msg.status);
      unsigned client = msg.usertag & 0xffff;
      unsigned short index = msg.usertag >> 16;
      assert(client <  MAXMODULES);
      assert(index);
      assert(index <= MAXDISKREQUESTS);
      MessageDiskCommit item(_modinfo[client].tags[index-1].disk-1, _modinfo[client].tags[index-1].usertag, msg.status);
      if (!_modinfo[client].prod_disk.produce(item))
	Logging::panic("produce disk (%x, %x) failed\n", client, index);
      _modinfo[client].tags[index-1].disk = 0;
    }
    _disk_requests_completed++;
    return true;
  }


  bool  receive(MessageNetwork &msg)
  {
    for (unsigned i=0; i < MAXMODULES; i++)
      if (i != msg.client) _modinfo[i].prod_network.produce(msg.buffer, msg.len);
    return true;
  }


  NOVA_NORETURN void run(Hip *hip)
  {
    unsigned res;
    if ((res = preinit(hip)))              Logging::panic("init() failed with %x\n", res);
    Logging::printf("Sigma0.nova:  hip %p caps %x memsize %x\n", hip, hip->sel, hip->mem_desc_size);

    if ((res = init_memmap(_boot_utcb)))          Logging::panic("init memmap failed %x\n", res);
    if ((res = create_requester()))     Logging::panic("create requester failed %x\n", res);
    if ((res = create_host_devices(hip)))  Logging::panic("create host devices failed %x\n", res);
    Logging::printf("start modules\n");
    if (!startlate)
      for (unsigned i=0; i <= repeat; i++)
	if ((res = start_modules(_boot_utcb, ~1U)))        Logging::printf("start modules failed %x\n", res);

    Logging::printf("INIT done\n");
    //_free_phys.debug_dump("free phys");
    //_free_virt.debug_dump("free virt");
    //_virt_phys.debug_dump("virt->phys");
    // unblock the requester and IRQ threads
    _lock.up();

    // block ourself since we have finished initialization
    block_forever();
  }

  static void start(Hip *hip) asm ("start") __attribute__((noreturn));
  Sigma0() : _modcount(0) {}
};


Sigma0 sigma0;
void Sigma0::start(Hip *hip)
{
  sigma0.run(hip);
}



extern "C" void  __exit(unsigned long status)
{
  //Motherboard *mb = global_mb;
  //global_mb = 0;
  Logging::printf("__exit(%lx)\n", status);
  if (global_mb) Sigma0::switch_view(global_mb);

  while (1)
    asm volatile ("ud2");
}
