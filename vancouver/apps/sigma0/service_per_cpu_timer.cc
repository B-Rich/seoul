/*
 * Per CPU Timer service.
 *
 * Copyright (C) 2011, Julian Stecklina <jsteckli@os.inf.tu-dresden.de>
 * Copyright (C) 2010, Bernhard Kauer <bk@vmmon.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
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


#include <nul/motherboard.h>
#include <nul/compiler.h>
#include <nul/capalloc.h>
#include <nul/baseprogram.h>
#include <sys/semaphore.h>
#include <host/hpet.h>
#include <host/rtc.h>
#include <nul/topology.h>
#include <nul/generic_service.h>
#include <nul/service_timer.h>
#include <service/lifo.h>
#include <nul/timer.h>

// Each CPU has a thread that maintains a CPU-local timeout list. From
// this list the next timeout on this CPU is calculated. If a CPU does
// not have its own HPET timer, it adds this timeout to the timeout
// list of another CPU. CPU-to-CPU mapping tries to respect CPU
// topology.

// TODO:
// - sync initial counter value to 1.1.1970 to make REQUEST_TIME work.
// 
// - User specifies TSC at timer program time and relative delay
//   relative to nominal TSC frequency.
//
// - PIT mode is not as exact as it could be. Do we care? 

// Tick at least this often between overflows of the lower 32-bit of
// the HPET main counter. If we don't tick between overflows, the
// overflow might not be detected correctly.
#define MIN_TICKS_BETWEEN_HPET_WRAP 4

// If more than this amount of cycles pass while reading the HPET
// counter, consider the TSC and HPET counter read to be out of sync
// and try again.
#define MAX_HPET_READ_TIME 20000

// If our HPET counter estimation error exceeds this value, the
// assumed TSC clock frequency is adjusted.
#define HPET_ESTIMATION_TOLERANCE 100 /* HPET ticks */

// Resolution of our TSC clocks per HPET clock measurement. Lower
// resolution mean larger error in HPET counter estimation.
#define CPT_RESOLUTION   /* 1 divided by */ (1U<<13) /* clocks per hpet tick */

#define CLIENTS ((1 << Config::MAX_CLIENTS_ORDER) + 32 + 10)

// PIT
#define PIT_FREQ            1193180ULL
#define PIT_DEFAULT_PERIOD  1000ULL /* us */
#define PIT_IRQ  2
#define PIT_PORT 0x40

class ClockSyncInfo {
private:
  ALIGNED(16) union {
    struct {
      volatile uint64 last_tsc;
      volatile uint64 last_hpet;
    };
    volatile uint64 raw[2];
  };

  void operator=(const ClockSyncInfo& copy_from) { __builtin_trap(); }
public:
  uint64 tsc() const { return last_tsc; }
  uint64 hpet() const { return last_hpet; }

  explicit
  ClockSyncInfo(uint64 tsc = 0, uint64 hpet = 0)
    : last_tsc(tsc), last_hpet(hpet)
  {}

  uint64 estimate_hpet(uint32 frac_clocks_per_tick)
  {
    uint64 lhpet, ltsc;
    do {
      lhpet  = last_hpet;
      ltsc  = last_tsc;
      // XXX Ought to be a better way...
    } while ((lhpet != last_hpet) || (ltsc != last_tsc));

    // XXX Does this handle overflow correctly?
    uint64 diff = Cpu::rdtsc() - ltsc;
    uint64 res = diff * CPT_RESOLUTION;
    Math::div64(res, frac_clocks_per_tick);
    return lhpet + res;
  }

  // Not thread-safe. Call only from one thread!
  void fetch(volatile uint32 &r)
  {
    uint64 newv;
    uint64 tsc1, tsc2;
    unsigned tries = 0;

    do {
      tsc1 = Cpu::rdtsc();
      newv = r;
      tsc2 = Cpu::rdtsc();
      tries ++;
    } while ((tsc2 - tsc1) > MAX_HPET_READ_TIME);

    // 2 tries is ok, 3 is fishy...
    if (tries > 2) {
      Logging::printf("CPU%u needed %u tries to get sample.\n",
                      BaseProgram::mycpu(),
                      tries);
    }

    // Handle overflows
    unsigned of = 0;
    if (static_cast<uint32>(newv) < static_cast<uint32>(last_hpet))
      of = 1;
    newv |= (((last_hpet >> 32) + of) << 32);

    ALIGNED(16) volatile struct {
      uint64 tsc;
      uint64 hpet;
    } vals;

    vals.tsc = tsc1;
    vals.hpet = newv;

    asm ("movdqa %2, %%xmm0 ;"
         "movdqa %%xmm0, %0 " : "=m" (raw[0]), "=m" (raw[1]) :
         "m" (vals.tsc), "m" (vals.hpet) : "xmm0");
  }
} ALIGNED(16);

class PerCpuTimerService : private BasicHpet,
                           public StaticReceiver<PerCpuTimerService>,
                           public CapAllocator<PerCpuTimerService>,
                           private BasicRtc

{
  Motherboard      &_mb;
  #include "host/simplehwioout.h"
  static const unsigned MAX_TIMERS = 24;
  HostHpetRegister *_reg;
  uint32            _timer_freq;
  uint64            _nominal_tsc_ticks_per_timer_tick;
  unsigned          _usable_timers;
  uint64            _pit_ticks;

  struct Timer {
    unsigned       _no;
    HostHpetTimer *_reg;

    Timer() : _reg(NULL) {}
  } _timer[MAX_TIMERS];

  struct ClientData : public GenericClientData {
    // XXX Difficult to implement atomic access to these two
    // fields. Restructure worker so that you can CALL it. This has
    // the nice benefit of helping.
    // volatile uint64 now;
    // volatile uint64 delta;
    volatile uint64   abstimeout;
    volatile unsigned count;

    unsigned nr;
    unsigned cpu;

    ClientData * volatile lifo_next;
  };

  ALIGNED(8)
  ClientDataStorage<ClientData, PerCpuTimerService> _storage;

  struct RemoteSlot {
    ClientData data;
  };

  struct PerCpu {
    ClockSyncInfo         *clock_sync;
    KernelSemaphore        worker_sm;
    bool                   has_timer;
    Timer                 *timer;
    uint32                 frac_clocks_per_tick;
    AtomicLifo<ClientData> work_queue;
    TimeoutList<CLIENTS, ClientData> abstimeouts;

    // Used by CPUs without timer
    KernelSemaphore remote_sm;   // for cross cpu wakeup
    RemoteSlot     *remote_slot; // where to store crosscpu timeouts

    // Used by CPUs with timer
    RemoteSlot *slots;          // Array
    unsigned    slot_count;     // with this many entries
    unsigned    irq;            // our irq number
    unsigned    ack;            // what do we have to write to ack? (0 if nothing)
  };

  PerCpu *_per_cpu;
  KernelSemaphore _workers_up;


  uint32 assigned_irqs;

  bool attach_timer_irq(DBus<MessageHostOp> &bus_hostop, Timer *timer, unsigned cpu)
  {
    // Prefer MSIs. No sharing, no routing problems, always edge triggered.
    if (not (_reg->config & LEG_RT_CNF) and
        (timer->_reg->config & FSB_INT_DEL_CAP)) {
      MessageHostOp msg1(MessageHostOp::OP_ATTACH_MSI, 0UL, 1, cpu);
      if (not bus_hostop.send(msg1)) Logging::panic("MSI allocation failed.");

      Logging::printf("\tHostHpet: Timer %u -> GSI %u CPU %u (%llx:%x)\n",
                      timer->_no, msg1.msi_gsi, cpu, msg1.msi_address, msg1.msi_value);

      _per_cpu[cpu].irq = msg1.msi_gsi;
      _per_cpu[cpu].ack = 0;

      timer->_reg->msi[0]  = msg1.msi_value;
      timer->_reg->msi[1]  = msg1.msi_address;
      timer->_reg->config |= FSB_INT_EN_CNF;

      return true;
    } else {
      // If legacy is enabled, only allow IRQ2
      uint32 allowed_irqs  = (_reg->config & LEG_RT_CNF) ? (1U << 2) : timer->_reg->int_route;
      uint32 possible_irqs = ~assigned_irqs & allowed_irqs;

      if (possible_irqs == 0) {
        Logging::printf("No IRQs left.\n");
        return false;
      }

      unsigned irq = Cpu::bsr(possible_irqs);
      assigned_irqs |= (1U << irq);

      MessageHostOp msg(MessageHostOp::OP_ATTACH_IRQ, irq, 1, cpu);
      if (not bus_hostop.send(msg)) Logging::panic("Could not attach IRQ.\n");

      _per_cpu[cpu].irq = irq;
      _per_cpu[cpu].ack = (irq < 16) ? 0 : (1U << timer->_no);

      Logging::printf("\tHostHpet: Timer %u -> IRQ %u (assigned %x ack %x).\n",
                      timer->_no, irq, assigned_irqs, _per_cpu[cpu].ack);


      timer->_reg->config &= ~(0x1F << 9) | INT_TYPE_CNF;
      timer->_reg->config |= (irq << 9) |
        ((irq < 16) ? 0 /* Edge */: INT_TYPE_CNF /* Level */);
    }

    return true;
  }

public:

  static void do_per_cpu_thread(void *t) REGPARM(0) NORETURN
  { reinterpret_cast<PerCpuTimerService *>(t)->per_cpu_thread(); }


  void process_new_timeout_requests(PerCpu *per_cpu)
  {
    // Check for new timeouts to be programmed.
    ClientData *next = NULL;
    for (ClientData *head = per_cpu->work_queue.dequeue_all(); head; head = next) {
      unsigned nr = head->nr;
      next = Cpu::xchg(&(head->lifo_next), static_cast<ClientData *>(NULL));
      uint64 t = head->abstimeout;
      // XXX Set abstimeout to zero here?
      per_cpu->abstimeouts.cancel(nr);
      per_cpu->abstimeouts.request(nr, t);
      //Logging::printf("CPU%u: New timeout at %016llx (now %llx)\n", BaseProgram::mycpu(), t, _pit_ticks);
    }

    // Process cross-CPU timeouts. slot_count is zero for CPUs without
    // a timer.
    for (unsigned i = 0; i < per_cpu->slot_count; i++) {
      RemoteSlot &cur = per_cpu->slots[i];
      uint64 to;

      do {
        to = cur.data.abstimeout;
        if (to == 0ULL)
          // No need to program a timeout
          goto next;
      } while (not __sync_bool_compare_and_swap(&cur.data.abstimeout, to, 0));

      per_cpu->abstimeouts.cancel(cur.data.nr);
      per_cpu->abstimeouts.request(cur.data.nr, to);
      //Logging::printf("CPU%u: Remote timeout %u at %016llx\n", BaseProgram::mycpu(), i, to);
    next:
      ;
    }
  }

  // Returns the next timeout.
  uint64 handle_expired_timers(PerCpu *per_cpu, uint64 now)
  {
    ClientData volatile *data;
    unsigned nr;
    while ((nr = per_cpu->abstimeouts.trigger(now, &data))) {
      per_cpu->abstimeouts.cancel(nr);
      assert(data);
      Cpu::atomic_xadd(&data->count, 1U);
      unsigned res = nova_semup(data->identity);
      if (res != NOVA_ESUCCESS) Logging::panic("ts: sem cap disappeared\n");
    }

    return per_cpu->abstimeouts.timeout();
  }

  void update_hpet_estimation(PerCpu *per_cpu)
  {
    // Update our clock estimation stuff, as a side effect we get a
    // new HPET main counter value.
    uint64 estimated_main = per_cpu->clock_sync->estimate_hpet(per_cpu->frac_clocks_per_tick);
    per_cpu->clock_sync->fetch(_reg->counter[0]);
        
    int64 diff = estimated_main - per_cpu->clock_sync->hpet();
        
    // Slightly adapt clocks per tick when our estimation is off.
    if (diff > HPET_ESTIMATION_TOLERANCE)
      per_cpu->frac_clocks_per_tick += 1;
        
    if (diff < -HPET_ESTIMATION_TOLERANCE)
      per_cpu->frac_clocks_per_tick -= 1;
        
    // Sanity
    if (((diff < 0LL) ? -diff : diff) > 1000000LL) {
          
      Logging::printf("CPU%u est %016llx real %016llx diff %016llx\n",
                      BaseProgram::mycpu(), estimated_main, per_cpu->clock_sync->hpet(), diff);
      Logging::printf("CPU%u worker died...\n", BaseProgram::mycpu());
      while (1) per_cpu->worker_sm.down();
    }
  }

  void per_cpu_thread() NORETURN
  {
    unsigned cpu = BaseProgram::mycpu();
    PerCpu * const our = &_per_cpu[cpu];

    if (_reg)
      our->clock_sync->fetch(_reg->counter[0]);

    if (_reg and our->has_timer) {
      Logging::printf("CPU%u up. We own a timer. Enable interrupts.\n", cpu);
      our->timer->_reg->config |= INT_ENB_CNF;
    }
    _workers_up.up();

    goto again;
    while (1) {
      our->worker_sm.downmulti();
    again:
      process_new_timeout_requests(our);

      if (_reg)
        update_hpet_estimation(our);

      uint64 now = _reg ? our->clock_sync->hpet() : _pit_ticks;
      uint64 next_to = handle_expired_timers(our, now);

      // Generate at least some IRQs between wraparound IRQs to make
      // overflow detection robust. Only needed with HPETs.
      if (_reg)
        if ((next_to == ~0ULL /* no next timeout */ ) or
            ((next_to - our->clock_sync->hpet()) > (0x100000000ULL/MIN_TICKS_BETWEEN_HPET_WRAP)))
          next_to = our->clock_sync->hpet() + 0x100000000ULL/MIN_TICKS_BETWEEN_HPET_WRAP;

      if (our->has_timer) {
        if (_reg) {
          // HPET timer programming

          // Program a new timeout. Top 32-bits are discarded.
          our->timer->_reg->comp[0] = next_to;
          
          // Check whether we might have missed that interrupt.
          if ((static_cast<int32>(next_to - _reg->counter[0])) <= 8) {
            COUNTER_INC("TO lost/past");
            //Logging::printf("CPU%u: Next timeout too close!\n", cpu);
            goto again;
          }
        } else {
          // Periodic mode. No programming necessary.
        }
      } else {
        // Tell timer_cpu that we have a new timeout.
        
        if (next_to == ~0ULL) continue;
        //Logging::printf("CPU%u: Cross core wakeup at %llu!\n", cpu, next_to);
        our->remote_slot->data.abstimeout = next_to;
        MEMORY_BARRIER;
        our->remote_sm.up();
      }
    }
  }

  bool receive(MessageIrq &msg)
  {
    unsigned cpu = BaseProgram::mycpu();
    if ((msg.type == MessageIrq::ASSERT_IRQ) && (_per_cpu[cpu].irq == msg.line)) {

      if (_reg) {
        // ACK the IRQ in non-MSI HPET mode.
        if (_per_cpu[cpu].ack != 0)
          _reg->isr = _per_cpu[cpu].ack;
      } else {
        // PIT mode. Increment our clock.
        while (not __sync_bool_compare_and_swap(&_pit_ticks, _pit_ticks, _pit_ticks+1))
          {}
      }

      MEMORY_BARRIER;
      _per_cpu[cpu].worker_sm.up();

      return true;
    }

    return false;
  }

  bool hpet_init(bool hpet_force_legacy)
  {
    // Find and map HPET
    bool          legacy_only   = hpet_force_legacy;
    unsigned long hpet_addr     = get_hpet_address(_mb.bus_acpi);
    if (hpet_addr == 0) {
      Logging::printf("No HPET found.\n");
      return false;
    }

    MessageHostOp msg1(MessageHostOp::OP_ALLOC_IOMEM, hpet_addr, 1024);
    if (!_mb.bus_hostop.send(msg1) || !msg1.ptr) {
      Logging::printf("%s failed to allocate iomem %lx+0x400\n", __PRETTY_FUNCTION__, hpet_addr);
      return false;
    }

    _reg = reinterpret_cast<HostHpetRegister *>(msg1.ptr);
    Logging::printf("HPET at %08lx -> %p.\n", hpet_addr, _reg);

    // XXX Check for old AMD HPETs and go home. :)
    uint8  hpet_rev    = _reg->cap & 0xFF;
    uint16 hpet_vendor = _reg->cap >> 16;
    Logging::printf("HPET vendor %04x revision %02x:%s%s\n", hpet_vendor, hpet_rev,
                    (_reg->cap & LEG_RT_CAP) ? " LEGACY" : "",
                    (_reg->cap & BIT64_CAP) ? " 64BIT" : " 32BIT"
                    );
    switch (hpet_vendor) {
    case 0x8086:                // Intel
      // Everything OK.
      break;
    case 0x4353:                // AMD
      if (hpet_rev < 0x10) {
        Logging::printf("It's one of those old broken AMD HPETs. Use legacy mode.\n");
        legacy_only = true;
      }
      break;
    default:
      // Before you blindly enable features for other HPETs, check
      // Linux and FreeBSD source for quirks!
      Logging::printf("Unknown HPET vendor ID. We only trust legacy mode.\n");
      legacy_only = true;
    }

    if (legacy_only and not (_reg->cap & LEG_RT_CAP)) {
      // XXX Implement PIT mode
      Logging::printf("We want legacy mode, but the timer doesn't support it.\n");
      return false;
    }

    // Figure out how many HPET timers are usable
    Logging::printf("HostHpet: cap %x config %x period %d\n", _reg->cap, _reg->config, _reg->period);
    unsigned timers = ((_reg->cap >> 8) & 0x1F) + 1;

    _usable_timers = 0;
    for (unsigned i=0; i < timers; i++) {
      Logging::printf("\tHpetTimer[%d]: config %x int %x\n", i, _reg->timer[i].config, _reg->timer[i].int_route);
      if ((_reg->timer[i].config | _reg->timer[i].int_route) == 0) {
        Logging::printf("\t\tTimer seems bogus. Ignore.\n");
        continue;
      }

      // if ((_reg->timer[i].config & FSB_INT_DEL_CAP) == 0) {
      //   Logging::printf("\t\tSkip timer %u. Not MSI capable.\n", i);
      //   continue;
      // }

      _timer[_usable_timers]._no   = i;
      _timer[_usable_timers]._reg = &_reg->timer[i];
      _usable_timers++;
    }

    if (_usable_timers == 0) {
      // XXX Can this happen?
      Logging::printf("No suitable timer.\n");
      return false;

    }

    if (legacy_only) {
      Logging::printf("HostHpet: Use one timer in legacy mode.\n");
      _usable_timers = 1;
    } else
      Logging::printf("HostHpet: Found %u usable timers.\n", _usable_timers);

    if (_usable_timers > _mb.hip()->cpu_count()) {
      _usable_timers = _mb.hip()->cpu_count();
      Logging::printf("HostHpet: More timers than CPUs. (Good!) Use only %u timers.\n",
                      _usable_timers);
    }

    for (unsigned i = 0; i < _usable_timers; i++) {
      // Interrupts will be disabled now. Will be enabled when the
      // corresponding per_cpu thread comes up.
      _timer[i]._reg->config |= MODE32_CNF;
      _timer[i]._reg->config &= ~(INT_ENB_CNF | TYPE_CNF);
      _timer[i]._reg->comp64 = 0;
    }

    // Disable counting and IRQs. Program legacy mode as requested.
    _reg->isr     = ~0U;
    _reg->config &= ~(ENABLE_CNF | LEG_RT_CNF);
    _reg->main    = 0ULL;
    _reg->config |= (legacy_only ? LEG_RT_CNF : 0);

    // HPET configuration

    uint64 freq = 1000000000000000ULL;
    Math::div64(freq, _reg->period);
    _timer_freq = freq;
    Logging::printf("HPET ticks with %u HZ.\n", _timer_freq);

    return true;
  }

  // Start HPET counter at value. HPET might be 32-bit. In this case,
  // the upper 32-bit of value are ignored.
  void hpet_start(uint64 value)
  {
    assert((_reg->config & ENABLE_CNF) == 0);
    _reg->main    = value;
    _reg->config |= ENABLE_CNF;
  }

  void start_thread(ServiceThreadFn fn,
                    unsigned prio, unsigned cpu)
  {
    MessageHostOp msg = MessageHostOp::alloc_service_thread(fn, this, prio, cpu);
    if (!_mb.bus_hostop.send(msg))
      Logging::panic("%s thread creation failed", __func__);
  }

  // Initialize PIT to tick every period_us microseconds.
  void pit_init(unsigned period_us)
  {
    MessageHostOp msg1(MessageHostOp::OP_ALLOC_IOIO_REGION, (PIT_PORT << 8) |  2);
    if (not _mb.bus_hostop.send(msg1))
      Logging::panic("Couldn't grab PIT ports.\n");

    unsigned long long value = PIT_FREQ*period_us;
    Math::div64(value, 1000000);

    if ((value == 0) || (value > 65535)) {
      Logging::printf("Bogus PIT period %uus. Set to default (%llu us)\n",
                      period_us, PIT_DEFAULT_PERIOD);
      period_us = PIT_DEFAULT_PERIOD;
      value = (PIT_FREQ*PIT_DEFAULT_PERIOD) / 1000000ULL;
    }

    outb(0x34, PIT_PORT + 3);
    outb(value, PIT_PORT);
    outb(value>>8, PIT_PORT);

    _timer_freq = 1000000U / period_us;

    Logging::printf("PIT initalized. Ticks every %uus (period %llu, %uHZ).\n",
                    period_us, value, _timer_freq);
  }

  unsigned alloc_crd() { return alloc_cap() << Utcb::MINSHIFT | DESC_TYPE_CAP; }

  unsigned portal_func(Utcb &utcb, Utcb::Frame &input, bool &free_cap) {
    unsigned res = ENONE;
    unsigned op;

    check1(EPROTO, input.get_word(op));

    unsigned cpu = BaseProgram::mycpu();
    PerCpu * const our = &_per_cpu[cpu];

    switch (op) {
    case ParentProtocol::TYPE_OPEN:
      {
        ClientData *data = 0;
        check1(res, res = _storage.alloc_client_data(utcb, data, input.received_cap()));
        free_cap = false;
        data->nr = our->abstimeouts.alloc(data);
        data->cpu = cpu;
        if (!data->nr) return EABORT;

        utcb << Utcb::TypedMapCap(data->identity);
        //Logging::printf("ts:: new client data %x parent %x\n", data->identity, data->pseudonym);
        return res;
      }
    case ParentProtocol::TYPE_CLOSE:
      {
        ClientData volatile *data = 0;
        ClientDataStorage<ClientData, PerCpuTimerService>::Guard guard_c(&_storage, utcb);
        check1(res, res = _storage.get_client_data(utcb, data, input.identity()));

        Logging::printf("ts:: close session for %x\n", data->identity);
        // XXX We are leaking an abstimeout slot!! XXX
        return _storage.free_client_data(utcb, data);
      }
    case TimerProtocol::TYPE_REQUEST_TIMER:
      {
        ClientData volatile *data = 0;
        ClientDataStorage<ClientData, PerCpuTimerService>::Guard guard_c(&_storage, utcb);
        if (res = _storage.get_client_data(utcb, data, input.identity())) return res;

        TimerProtocol::MessageTimer msg = TimerProtocol::MessageTimer(0);
        if (input.get_word(msg)) return EABORT;
        if (data->cpu != cpu)    return EABORT;

        COUNTER_INC("request to");
        assert(data->nr < CLIENTS);

        int64 diff = msg.abstime - _mb.clock()->time();
        if (diff < 0) {
          unsigned res = nova_semup(data->identity);
          assert(res == NOVA_ESUCCESS);
          return ENONE;
        }

        uint64 udiff = diff;
        // if (diff > max_to) {
        //   // Clamp diff to avoid division overflow.
        //   Logging::printf("Timeout %llx too large, capping to %llx.\n", diff, max_to);
        //   diff = max_to;
        // }

        // XXX muldiv128
       
        Math::div64(udiff, static_cast<uint32>((_nominal_tsc_ticks_per_timer_tick/CPT_RESOLUTION)));
        
        uint64 estimated_main;
        if (_reg)
          estimated_main = our->clock_sync->estimate_hpet(our->frac_clocks_per_tick);
        else
          estimated_main = _pit_ticks + 1; // Compute from next tick.

        data->abstimeout = estimated_main + udiff;
        MEMORY_BARRIER;
        if (!data->lifo_next)
          our->work_queue.enqueue(data);

        our->worker_sm.up();
      }
      return ENONE;
    case TimerProtocol::TYPE_REQUEST_LAST_TIMEOUT:
      {
        ClientData volatile *data = 0;
        ClientDataStorage<ClientData, PerCpuTimerService>::Guard guard_c(&_storage, utcb);
        if (res = _storage.get_client_data(utcb, data, input.identity())) return res;

        utcb << Cpu::xchg(&data->count, 0U);
        return ENONE;
      }
    case TimerProtocol::TYPE_REQUEST_TIME:
      {
        ClientData volatile *data = 0;
        ClientDataStorage<ClientData, PerCpuTimerService>::Guard guard_c(&_storage, utcb);
        if (res = _storage.get_client_data(utcb, data, input.identity())) return res;

        MessageTime msg;
        uint64 counter = _reg ?
          our->clock_sync->estimate_hpet(our->frac_clocks_per_tick) :
          _pit_ticks;

        msg.wallclocktime = Math::muldiv128(counter, MessageTime::FREQUENCY, _timer_freq);
        msg.timestamp = _mb.clock()->time();
        utcb << msg;
        return ENONE;
      }
    default:
      return EPROTO;
    }
  }

  // Returns initial value of timecounter register.
  uint64
  wallclock_init()
  {
    // RTC ports
    MessageHostOp msg1(MessageHostOp::OP_ALLOC_IOIO_REGION, (BasicRtc::_iobase << 8) |  1);
    if (not _mb.bus_hostop.send(msg1))
      Logging::panic("%s failed to allocate ports %x+2\n", __PRETTY_FUNCTION__, BasicRtc::_iobase);

    rtc_sync(_mb.clock());
    uint64 secs = rtc_wallclock();

    return secs * _timer_freq;
  }


  PerCpuTimerService(Motherboard &mb, unsigned cap, unsigned cap_order,
           bool hpet_force_legacy, bool force_pit, unsigned pit_period_us)
    : CapAllocator<PerCpuTimerService>(cap, cap, cap_order),
      BasicRtc(mb.bus_hwioin, mb.bus_hwioout, 0x70),
      _mb(mb), _bus_hwioout(mb.bus_hwioout), assigned_irqs(0)
  {
    unsigned cpus = mb.hip()->cpu_count();
    _per_cpu = new(64) PerCpu[cpus];

    if (force_pit or not hpet_init(hpet_force_legacy)) {
      _reg = NULL;
      _usable_timers = 1;
      Logging::printf("HPET initialization failed. Try PIT instead.\n");
      pit_init(pit_period_us);
    }

    // HPET: Counter is running, IRQs are off.
    // PIT:  PIT is programmed to run in periodic mode, if HPET didn't work for us.

    uint64 clocks_per_tick = static_cast<uint64>(mb.hip()->freq_tsc) * 1000 * CPT_RESOLUTION;
    Math::div64(clocks_per_tick, _timer_freq);
    Logging::printf("%llu+%04llu/%u TSC ticks per timer tick.\n", clocks_per_tick/CPT_RESOLUTION, clocks_per_tick%CPT_RESOLUTION, CPT_RESOLUTION);
    _nominal_tsc_ticks_per_timer_tick = clocks_per_tick;

    // Get wallclock time
    uint64 initial_counter = wallclock_init();
    if (_reg)
      hpet_start(initial_counter);
    else
      _pit_ticks = initial_counter;

    unsigned cpu_cpu[cpus];
    unsigned part_cpu[cpus];

    size_t n = mb.hip()->cpu_desc_count();
    Topology::divide(mb.hip()->cpus(), n,
                     _usable_timers,
                     part_cpu,
                     cpu_cpu);

    // Bootstrap IRQ handlers. IRQs are disabled. Each worker enables
    // its IRQ when it comes up.

    for (unsigned i = 0; i < _usable_timers; i++) {
      unsigned cpu = part_cpu[i];

      _per_cpu[cpu].has_timer = true;
      if (_reg)
        _per_cpu[cpu].timer = &_timer[i];

      // We allocate a couple of unused slots if there is an odd
      // combination of CPU count and usable timers. Who cares.
      _per_cpu[cpu].slots = new RemoteSlot[mb.hip()->cpu_count() / _usable_timers];

      if (_reg)
        attach_timer_irq(mb.bus_hostop, &_timer[i], cpu);
      else {
        // Attach to PIT instead.
        MessageHostOp msg(MessageHostOp::OP_ATTACH_IRQ, PIT_IRQ, 1, cpu);
        if (not mb.bus_hostop.send(msg)) Logging::panic("Could not attach IRQ.\n");
        _per_cpu[i].irq = PIT_IRQ;
      }
    }
    mb.bus_hostirq.add(this, receive_static<MessageIrq>);

    // Create remote slot mapping and initialize per cpu data structure
    for (unsigned i = 0; i < cpus; i++) {
      _per_cpu[i].frac_clocks_per_tick = _nominal_tsc_ticks_per_timer_tick;
      _per_cpu[i].worker_sm = KernelSemaphore(alloc_cap(), true);
      // Provide initial hpet counter to get high 32-bit right.
      _per_cpu[i].clock_sync = new(16) ClockSyncInfo(0, initial_counter);
      _per_cpu[i].abstimeouts.init();
    }

    for (unsigned i = 0; i < cpus; i++) {
      if (not _per_cpu[i].has_timer) {
        PerCpu &remote = _per_cpu[cpu_cpu[i]];

        _per_cpu[i].remote_sm   = remote.worker_sm;
        _per_cpu[i].remote_slot = &remote.slots[remote.slot_count];

        // Fake a ClientData for this CPU.
        _per_cpu[i].remote_slot->data.identity   = _per_cpu[i].worker_sm.sm();
        _per_cpu[i].remote_slot->data.abstimeout = 0;
        _per_cpu[i].remote_slot->data.nr = remote.abstimeouts.alloc(&_per_cpu[i].remote_slot->data);

        Logging::printf("CPU%u maps to CPU%u slot %u.\n",
                        i, cpu_cpu[i], remote.slot_count);

        remote.slot_count ++;
      }
    }

    // Bootstrap per CPU workers
    _workers_up = KernelSemaphore(alloc_cap(), true);

    Logging::printf("Waiting for per CPU workers to come up...\n");

    for (unsigned i = 0; i < cpus; i ++)
      start_thread(PerCpuTimerService::do_per_cpu_thread, 1, i);

    for (unsigned i = 0; i < 1; i++)
      _workers_up.down();

    Logging::printf(" ... done.\n");
  }

};

static bool default_force_hpet_legacy = false;
static bool default_force_pit         = false;

PARAM(timer_hpet_legacy, default_force_hpet_legacy = true);
PARAM(timer_force_pit,   default_force_pit         = true);

PARAM(service_per_cpu_timer,
      unsigned cap_region = alloc_cap_region(1 << 12, 12);
      bool     hpet_legacy   = (argv[0] == ~0U) ? default_force_hpet_legacy : argv[0];
      bool     force_pit     = (argv[1] == ~0U) ? default_force_pit : argv[1];
      unsigned pit_period_us = (argv[2] == ~0U) ? PIT_DEFAULT_PERIOD : argv[2];

      PerCpuTimerService *h = new(16) PerCpuTimerService(mb, cap_region, 12, hpet_legacy, force_pit, pit_period_us);

      MessageHostOp msg(h, "/timer", reinterpret_cast<unsigned long>(StaticPortalFunc<PerCpuTimerService>::portal_func));
      msg.crd_t = Crd(cap_region, 12, DESC_TYPE_CAP).value();
      if (!cap_region || !mb.bus_hostop.send(msg))
        Logging::panic("starting of timer service failed");
      ,
      "service_per_cpu_timer:[hpet_force_legacy=0][,force_pit=0][,pit_period_us]");
