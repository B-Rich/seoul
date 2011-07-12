/*
 * Copyright (C) 2011, Alexander Boettcher <boettcher@tudos.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of NUL (NOVA user land).
 *
 * NUL is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * NUL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#include <nul/program.h>
#include <sigma0/sigma0.h> // Sigma0Base object
#include <sigma0/console.h>

#include <nul/compiler.h>
#include <nul/service_admission.h>
#include <nul/timer.h>
#include <nul/service_timer.h>
#include <service/math.h>
#include <service/cmdline.h>

#include <host/keyboard.h>

#include "util/capalloc_partition.h"

#define CONST_CAP_RANGE 16U

namespace ab {
class AdmissionService : public CapAllocatorAtomicPartition<1 << CONST_CAP_RANGE>, public NovaProgram, public ProgramConsole
{
  static char * flag_revoke;

private:

  struct ClientData : public GenericClientData {
    cap_sel statistics;
    char name[32];
    struct {
      unsigned idx;
      unsigned cpu;
      unsigned prio;
      unsigned quantum;
      timevalue m_last1;
      timevalue m_last2;
      char name[32];
    } scs[64];
  };

  struct ClientData idle_scs; 
  struct ClientData own_scs;

  timevalue global_sum[Config::MAX_CPUS];
  timevalue global_prio[Config::MAX_CPUS][256]; //XXX MAX_PRIO

  ALIGNED(8) ClientDataStorage<ClientData, AdmissionService> _storage;

  bool enable_top;
  bool enable_measure;
  bool enable_log;
  bool enable_verbose;

public:

  AdmissionService() : CapAllocatorAtomicPartition<1 << CONST_CAP_RANGE>(1), NovaProgram(), ProgramConsole() {}

  void init_service(Hip * hip) {
    unsigned long long base = alloc_cap_region(1 << CONST_CAP_RANGE, 12);
    assert(base && !(base & 0xFFFULL));
    _divider  = hip->cpu_desc_count();
    _cap_base = base;
    enable_verbose = enable_top = enable_measure = enable_log = false;
  }

  inline unsigned alloc_cap(unsigned num = 1, unsigned cpu = ~0U) { //XXX quirk as long as CapAllocatorAtomic can not handle num > 1
    if (num > 1) return CapAllocator::alloc_cap(num);
    else return CapAllocatorAtomicPartition::alloc_cap(num, cpu);
  }
  inline void dealloc_cap(unsigned cap, unsigned count = 1) {
    assert(count == 1); CapAllocatorAtomicPartition::dealloc_cap(cap, count);
  }

  inline unsigned alloc_crd() { return Crd(alloc_cap(), 0, DESC_CAP_ALL).value(); }

  #include "top.h"

  void check_clients(Utcb &utcb) {
    ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
    ClientData volatile * data = _storage.get_invalid_client(utcb, this);
    while (data) {
      Logging::printf("ad: found dead client - freeing datastructure\n");
      _storage.free_client_data(utcb, data, this);
      data = _storage.get_invalid_client(utcb, this, data);
    }
  }

  unsigned portal_func(Utcb &utcb, Utcb::Frame &input, bool &free_cap)
    {
      unsigned op, res;
      check1(EPROTO, input.get_word(op));

      switch (op) {
      case ParentProtocol::TYPE_OPEN:
      {
        unsigned idx = input.received_cap();
        unsigned cap_session = 0;

        if (enable_verbose && !idx) Logging::printf("  open - invalid cap recevied\n");
        if (!idx) return EPROTO;

        //check whether we have already a session with this client
        res = ParentProtocol::check_singleton(utcb, idx, cap_session);
        if (!res && cap_session)
        {
         //XXX check whether pseudo is really invalid otherwise unnecessary to do that
          ClientData volatile *data = 0;
          ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
          while (data = _storage.next(data)) {
            if (data->identity == cap_session) {
              dealloc_cap(data->pseudonym); //replace old pseudonym, first pseudnym we got via parent and gets obsolete as soon as client becomes running
              if (enable_verbose) Logging::printf("  open - session rebind pseudo=%x->%x\n", data->pseudonym, idx);
              data->pseudonym = idx;
              utcb << Utcb::TypedMapCap(data->identity);
              free_cap = false;
              return ENONE;
            }
          }
        }

        ClientData *data = 0;
        res = _storage.alloc_client_data(utcb, data, idx, this);
        if (enable_verbose && res) Logging::printf("  alloc_client - res %x\n", res);
        if (res == ERESOURCE) { check_clients(utcb); return ERETRY; } //force garbage collection run
        else if (res) return res;
        if (*flag_revoke) { check_clients(utcb); *flag_revoke = 0; }

        res = ParentProtocol::set_singleton(utcb, data->pseudonym, data->identity);
        assert(!res);

        free_cap = false;
        if (enable_verbose) Logging::printf("**** created admission client 0x%x 0x%x\n", data->pseudonym, data->identity);
        utcb << Utcb::TypedMapCap(data->identity);
        return res;
      }
      case ParentProtocol::TYPE_CLOSE:
      {
        ClientData *data = 0;
        ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
        check1(res, res = _storage.get_client_data(utcb, data, input.identity()));
        return _storage.free_client_data(utcb, data, this);
      }
      case AdmissionProtocol::TYPE_SET_NAME:
      {
        ClientData *data = 0;
        ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
        res = _storage.get_client_data(utcb, data, input.identity());
        if (res) return res;

        unsigned len;
        char const * name = input.get_zero_string(len);
        if (!name || !len) return EPROTO;

        memcpy(data->name, name, len > sizeof(data->name) - 1 ? sizeof(data->name) - 1 : len);
        data->name[sizeof(data->name) - 1] = 0;
        return ENONE;
      }
      case AdmissionProtocol::TYPE_SC_ALLOC:
      case AdmissionProtocol::TYPE_SC_PUSH:
        {
          AdmissionProtocol::sched sched;
          unsigned i, cpu, len, idx = input.received_cap();
          bool self = false;

          check1(EPROTO, !idx);
          check1(EPROTO, input.get_word(sched));
          check1(EPROTO, input.get_word(cpu) && cpu < _divider); //check that cpu < number of cpus
          char const * name = input.get_zero_string(len);
          if (!name) return EPROTO;

          if (op == AdmissionProtocol::TYPE_SC_PUSH)
            check1(EPROTO, input.get_word(self));

          ClientData *data = 0;
          ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
          if (res = _storage.get_client_data(utcb, data, input.identity())) return res;

          if (op == AdmissionProtocol::TYPE_SC_PUSH) {
            timevalue computetime;
            //check for sigma0 - only sigma0 is allowed to push
            if (NOVA_ESUCCESS != nova_ctl_sc(idx, computetime)) return EPROTO;
            if (self) data = &own_scs;
          }

          again:

          for (i=0; i < sizeof(data->scs) / sizeof(data->scs[0]); i++) {
            if (data->scs[i].idx) continue;
            if (0 != Cpu::cmpxchg4b(&data->scs[i].idx, 0, 0xaffe)) goto again;
            break;
          }
          if (i >= sizeof(data->scs) / sizeof(data->scs[0])) return ERESOURCE;

          if (sched.type < sched.TYPE_APERIODIC || sched.type > sched.TYPE_SYSTEM) sched.type = sched.TYPE_APERIODIC; //sanity check
          data->scs[i].prio    = (op == AdmissionProtocol::TYPE_SC_PUSH) ? sched.type : (sched.type > sched.TYPE_PERIODIC ? sched.TYPE_PERIODIC : sched.type);
          data->scs[i].quantum = 10000U;
          data->scs[i].cpu = cpu;
          data->scs[i].m_last1 = data->scs[i].m_last2 = 0;
          memcpy(data->scs[i].name, name, len > sizeof(data->scs[i].name) - 1 ? sizeof(data->scs[i].name) - 1 : len);
          data->scs[i].name[sizeof(data->scs[i].name) - 1] = 0;

          if (op == AdmissionProtocol::TYPE_SC_PUSH) {
            data->scs[i].idx = idx; //got from outside
            free_cap = false;
          } else {
            unsigned idx_sc = alloc_cap(1, cpu);
            unsigned char res;
            //XXX security bug -- force ec to be on right cpu //XXX
            res = nova_create_sc(idx_sc, idx, Qpd(data->scs[i].prio, data->scs[i].quantum));
            if (res != NOVA_ESUCCESS) {
              memset(&data->scs[i], 0, sizeof(data->scs[i]));
              dealloc_cap(idx_sc);
              Logging::printf("create_sc failed %x\n", res);
              return EPROTO;
            }
            data->scs[i].idx = idx_sc;
          }
//          *reinterpret_cast<unsigned long *>(0xaffe) = 0xaffe;
          if (enable_verbose) Logging::printf("created sc - prio=%u quantum=%u cpu=%u\n", data->scs[i].prio, data->scs[i].quantum, data->scs[i].cpu);
          return ENONE;
        }
        break;
      case AdmissionProtocol::TYPE_GET_USAGE_CAP:
        {
          ClientData *client;
          ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
          if (res = _storage.get_client_data(utcb, client, input.identity())) return res;

          if (!(client->statistics = alloc_cap())) return ERESOURCE;
          if (NOVA_ESUCCESS != nova_create_sm(client->statistics)) {
            dealloc_cap(client->statistics); client->statistics = 0; return ERESOURCE;
          }

          utcb << Utcb::TypedMapCap(client->statistics);
          return ENONE;
        }
        break;
      case AdmissionProtocol::TYPE_REBIND_USAGE_CAP:
      case AdmissionProtocol::TYPE_SC_USAGE:
        {
          ClientData *caller;
          cap_sel stats;

          ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, utcb, this);
          if (res = _storage.get_client_data(utcb, caller, input.identity())) return res; //caller
          if (!(stats = input.identity(1))) return EPROTO; //client statistic cap

          //input.dump_typed_items();
          //check whether provided stat cap match to one of our client
          ClientData volatile *client = 0;
          for (client = _storage.next(client); client; client = _storage.next(client))
            if (client->statistics == stats) break;
          if (!client) return EPERM;

          if (op == AdmissionProtocol::TYPE_SC_USAGE) {
            uint64 time_con = get_usage(client);
            utcb << time_con;
            return ENONE;
          } else if (op == AdmissionProtocol::TYPE_REBIND_USAGE_CAP) {
            nova_revoke(Crd(client->statistics, 0, DESC_CAP_ALL), false); //rebind - revoke old mapping
            utcb << Utcb::TypedMapCap(client->statistics);
            return ENONE;
          } else return EPROTO;
        }
        break;
      default:
        return EPROTO;
      }
    }

  /*
   * Calculates subsumed time of all SCs of a client on all CPUs
   */
  timevalue get_usage(ClientData volatile * data) {
    unsigned i;
    timevalue time_con = 0;
    for (i=0; i < sizeof(data->scs) / sizeof(data->scs[0]); i++) {
      if (!data->scs[i].idx) continue;
      time_con += data->scs[i].m_last1;
    }
    return time_con;
  }

  /*
   * Calculates subsumed time of all SCs of a client per CPU.
   * Result is passed on UTCB.
   */
  unsigned get_usage(Utcb & utcb, ClientData volatile * data) {
    unsigned i;

    for (phy_cpu_no cpunr=0; cpunr < Global::hip.cpu_desc_count(); cpunr++) {
      Hip_cpu const *cpu = &Global::hip.cpus()[cpunr];
      if (not cpu->enabled()) continue;

      timevalue time_con = 0;
      bool avail = false;

      for (i=0; i < sizeof(data->scs) / sizeof(data->scs[0]); i++) {
        if (!data->scs[i].idx || data->scs[i].cpu != cpunr) continue;
        time_con += data->scs[i].m_last1 - data->scs[i].m_last2;
        avail = true;
      }
      if (!avail) continue;

      timevalue rest;
      splitfloat(time_con, rest, cpunr);

      unsigned _util = time_con;
      unsigned _rest = rest;
      utcb << cpunr << _util << _rest;
    }

    utcb << ~0UL;
    return ENONE;
  }

  bool start_service (Utcb *utcb, Hip * hip)
    {
      flag_revoke = new (0x1000) char[0x1000];
      if (!flag_revoke) return false;

      const char * service_name = "/admission";
      unsigned res;
      unsigned exc_base_wo, exc_base_pf, pt_wo, pt_pf;
      unsigned service_cap = alloc_cap();
      Utcb *utcb_wo, *utcb_pf;
      
      memset(&idle_scs, 0, sizeof(idle_scs));
      memset(&own_scs , 0, sizeof(idle_scs));
      memcpy(&own_scs.name, "admission", 9);

      for (unsigned cpunr = 0; cpunr < hip->cpu_desc_count(); cpunr++) {
        Hip_cpu const *cpu = &hip->cpus()[cpunr];
        if (not cpu->enabled()) continue;

        idle_scs.scs[cpunr].idx = 512 + cpunr;
        idle_scs.scs[cpunr].cpu = cpunr;

        exc_base_wo = alloc_cap(16);
        exc_base_pf = alloc_cap(16);
        if (!exc_base_wo || !exc_base_pf) return false;
        pt_wo       = alloc_cap();
        pt_pf       = exc_base_wo + 0xe;

        unsigned cap_ec = create_ec_helper(this, cpunr, exc_base_wo, &utcb_wo, 0, alloc_cap());
        if (!cap_ec) return false;
        unsigned cap_pf = create_ec_helper(this, cpunr, exc_base_pf, &utcb_pf, 0, alloc_cap());
        if (!cap_pf) return false;

        utcb_wo->head.crd = alloc_crd();
        utcb_wo->head.crd_translate = Crd(_cap_base, CONST_CAP_RANGE, DESC_CAP_ALL).value();
        utcb_pf->head.crd = 0;

        unsigned long portal_func = reinterpret_cast<unsigned long>(StaticPortalFunc<AdmissionService>::portal_func);
        res = nova_create_pt(pt_wo, cap_ec, portal_func, 0);
        if (res) return false;

        res = ParentProtocol::register_service(*utcb, service_name, cpunr, pt_wo, service_cap, flag_revoke);
        if (res) return !res;
      }

      return true;
    }

  bool run_statistics(Utcb * utcb, Hip * hip) {
    assert(enable_measure);

    Clock * _clock = new Clock(hip->freq_tsc);
    if (!_clock) return false;

    TimerProtocol * timer_service = new TimerProtocol(alloc_cap(TimerProtocol::CAP_SERVER_PT + hip->cpu_desc_count()));
    TimerProtocol::MessageTimer msg(_clock->abstime(0, 1000));
    unsigned res = timer_service->timer(*utcb, msg);
    if (res) return false;

    KernelSemaphore sem = KernelSemaphore(timer_service->get_notify_sm());

    StdinConsumer stdinconsumer;
    if (enable_top)
      Sigma0Base::request_stdin(utcb, &stdinconsumer, sem.sm());

    {
      ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, *utcb, this);
      measure_scs(hip);
    }

    unsigned update = true;
    unsigned show = 0, client_num = 0;
    unsigned timeout = 2000; //2s

    while (true) {
      if (enable_top) {
        while (stdinconsumer.has_data()) {
          MessageInput *kmsg = stdinconsumer.get_buffer();
          if (!(kmsg->data & KBFLAG_RELEASE)) {
            bool _update = true;
            if ((kmsg->data & 0x7f) == 77) { show = show == 2 ? 0 : 2; } //"p"
            else if ((kmsg->data & 0x7f) == 44) show = 0; //"t"
            else if ((kmsg->data & 0x3ff) == KBCODE_DOWN) { if (show == 0 && client_num) client_num--; show = 0; }
            else if ((kmsg->data & 0x3ff) == KBCODE_UP)   { if (show == 0 && client_num < HEIGHT - 3) client_num++; show = 0; }
            else if ((kmsg->data & 0x7f) == KBCODE_ENTER) { show = show == 1 ? 0 : 1; }
            else _update = false;
            update = update ? update : _update;
          }
          stdinconsumer.free_buffer();
        }
      }

      {
        ClientDataStorage<ClientData, AdmissionService>::Guard guard_c(&_storage, *utcb, this);
        unsigned tcount;

        if (!timer_service->triggered_timeouts(*utcb, tcount) && tcount) measure_scs(hip);

        if (enable_top && (update || tcount)) {
          if (show == 0)
            top_dump_scs(*utcb, hip, client_num);
          else if (show == 1)
            top_dump_client(client_num);
          else
            top_dump_prio(hip);
        }
      }

      TimerProtocol::MessageTimer to(_clock->abstime(timeout, 1));
      if (timer_service->timer(*utcb,to)) Logging::printf("failure - programming timer\n");

      sem.downmulti();
      update = false;
    }
  }
 
  NORETURN
  void run(Utcb *utcb, Hip *hip)
  {

    init(hip);
    init_mem(hip);
    init_service(hip);

    console_init("admission service", new Semaphore(alloc_cap(), true));

    if (!start_service(utcb, hip))
      Logging::printf("failure - starting admission service\n");

    char *cmdline = reinterpret_cast<char *>(hip->get_mod(0)->aux);
    char *args[16];
    unsigned argv = Cmdline::parse(cmdline, args, sizeof(args)/sizeof(char *));
    for (unsigned i=1; i < argv; i++) {
      if (!strcmp("top", args[i])) enable_top = true;
      if (!strcmp("measure", args[i])) enable_measure = true;
      if (!strcmp("log", args[i])) enable_log = true;
      if (!strcmp("verbose", args[i])) enable_verbose = true;
    }

    enable_measure = enable_measure || enable_top;

    Logging::printf("admission service: log=%s measure=%s top=%s verbose=%s\n",
                    enable_log ? "yes" : "no", enable_measure ? "yes" : "no",
                    enable_top ? "yes" : "no", enable_verbose ? "yes" : "no");
    if (enable_log) _console_data.log = new LogProtocol(alloc_cap(LogProtocol::CAP_SERVER_PT + hip->cpu_desc_count()));
    if (enable_measure && !run_statistics(utcb, hip))
      Logging::printf("failure - running statistic loop\n");

    block_forever();
  }
};

  char *   AdmissionService::flag_revoke;
  unsigned AdmissionService::cursor_pos;
} /* namespace */

ASMFUNCS(ab::AdmissionService, NovaProgram)

