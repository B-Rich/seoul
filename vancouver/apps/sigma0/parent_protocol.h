/*
 * Parent protocol implementation in sigma0.
 *
 * Copyright (C) 2010, Bernhard Kauer <bk@vmmon.org>
 * Copyright (C) 2010-2011, Alexander Boettcher <boettcher@tudos.org>
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

/**
 * Missing: kill a client, mem+cap quota support
 */
struct ClientData : public GenericClientData {
  char const    * name;
  unsigned        len;

  static unsigned get_quota(Utcb &utcb, unsigned parent_cap, const char *name, long value_in, long *value_out=0) {
    Utcb::Frame input = utcb.get_nested_frame();
//    Logging::printf("get quota for '%s' amount %lx from %x for %x by %x\n", name, value_in, parent_cap, input.identity(1), input.identity(0));
    if (!strcmp(name, "mem") || !strcmp(name, "cap")) return ENONE;
    if (!strcmp(name, "guid")) {
      unsigned long s0_cmdlen;
      char const *pos, *cmdline = get_client_cmdline(input.identity(0), s0_cmdlen);
      if (cmdline && (pos = strstr(cmdline, "quota::guid")) && pos < cmdline + s0_cmdlen) {
        *value_out = get_client_number(parent_cap);
//        Logging::printf("send clientid %lx from %x\n", *value_out, parent_cap);
        return ENONE;
      }
    }
    return ERESOURCE;
  }
  void session_close(Utcb &utcb) {}
};
struct ServerData : public ClientData {
  unsigned        cpu;
  unsigned        pt;
  char *          mem_revoke;
};

ALIGNED(8) ClientDataStorage<ClientData, Sigma0, false> _client;
ALIGNED(8) ClientDataStorage<ServerData, Sigma0, false> _server;

static unsigned get_client_number(unsigned cap) {
  cap -= CLIENT_PT_OFFSET;
  if ((cap % (1 << CLIENT_PT_SHIFT)) != ParentProtocol::CAP_PARENT_ID) return ~0;
  return cap >> CLIENT_PT_SHIFT;
}

static char const * get_client_cmdline(unsigned identity, unsigned long &s0_cmdlen) {
  unsigned clientnr = get_client_number(identity);
  if (clientnr >= MAXMODULES) return 0;
  s0_cmdlen = sigma0->_modinfo[clientnr].sigma0_cmdlen;
  return sigma0->_modinfo[clientnr].cmdline;
}

static char * get_client_memory(unsigned identity, unsigned client_mem_revoke) {
  unsigned clientnr = get_client_number(identity);
  //that are we - so service is in this pd
  if (clientnr == 0) return reinterpret_cast<char *>(client_mem_revoke);

  ModuleInfo * modinfo = sigma0->get_module(clientnr);
  if (!modinfo) return 0;
  char * mem_revoke = reinterpret_cast<char *>(client_mem_revoke);
  if (sigma0->convert_client_ptr(modinfo, mem_revoke, 0x1000U)) return 0;
  else return mem_revoke;
}

unsigned get_portal(Utcb &utcb, unsigned cap_client, unsigned &portal) {
  ClientData *cdata;
  ServerData volatile *sdata;
  unsigned res;

  ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client);
  if ((res = _client.get_client_data(utcb, cdata, cap_client))) return res;
  //Logging::printf("\tfound session cap %x for client %x %.*s\n", cap_client, cdata->pseudonym, cdata->len, cdata->name);

  ClientDataStorage<ServerData, Sigma0, false>::Guard guard_s(&_server);
  for (sdata = _server.next(); sdata; sdata = _server.next(sdata))
     if (sdata->cpu == utcb.head.nul_cpunr && cdata->len == sdata->len-1 && !memcmp(cdata->name, sdata->name, cdata->len)) {
       // check that the server portal still exists, if not free the server-data and tell the client to retry
       unsigned crdout;
       if (nova_syscall(NOVA_LOOKUP, Crd(sdata->pt, 0, DESC_CAP_ALL).value(), 0, 0, 0, &crdout) || !crdout) {
         free_service(utcb, sdata);
         return ERETRY;
       }
       portal = sdata->pt;
       return ENONE;
     }
  // we do not have a server portal yet, thus tell the client to retry later
  return ERETRY;
}

unsigned check_permission(unsigned identity, const char *request, unsigned request_len,
                          unsigned instance, char const * &cmdline, unsigned &namelen)
{
  /**
   * Parse the cmdline for "name::" prefixes and check whether the
   * postfix matches the requested name.
   */
  unsigned long s0_cmdlen;
  cmdline = get_client_cmdline(identity, s0_cmdlen);
  char const * cmdline_end = cmdline + s0_cmdlen;
  if (!cmdline) return EPROTO;
  cmdline = strstr(cmdline, "name::");
  while (cmdline && cmdline < cmdline_end) {
    cmdline += 6;
    namelen = strcspn(cmdline, " \t\r\n\f");
    if ((request_len > namelen) || (0 != memcmp(cmdline + namelen - request_len, request, request_len))) {
      cmdline = strstr(cmdline + namelen, "name::");
      continue;
    }
    if (instance--) continue;
    return ENONE;
  }
  // we do not have the permissions
  return EPERM;
}

unsigned free_service(Utcb &utcb, ServerData volatile *sdata) {
  dealloc_cap(sdata->pt);
  delete sdata->name;
  ServerData::get_quota(utcb, sdata->pseudonym, "cap", -1);
  ServerData::get_quota(utcb, sdata->pseudonym, "mem", -sdata->len);
  return _server.free_client_data(utcb, sdata, this);
}

void notify_service(Utcb &utcb, ClientData volatile *c) {

  //revoke identity cap so that services can detect that client is gone
  unsigned res = nova_revoke(Crd(c->identity, 0, DESC_CAP_ALL), false);
  assert(res == ENONE);

  {
    ClientDataStorage<ServerData, Sigma0, false>::Guard guard_s(&_server);
    for (ServerData volatile * sdata = _server.next(); sdata; sdata = _server.next(sdata))
      if (sdata->cpu == utcb.head.nul_cpunr && c->len == sdata->len-1 &&
          !memcmp(c->name, sdata->name, c->len) && sdata->mem_revoke)
            *sdata->mem_revoke = 1; //flag revoke
  }

}

unsigned portal_func(Utcb &utcb, Utcb::Frame &input, bool &free_cap) {
  unsigned res;
  unsigned op;
  if (input.get_word(op)) return EPROTO;
  //Logging::printf("parent request words %x type %x id %x+%x\n", input.untyped(), op, input.identity(), input.identity(1));
  switch (op) {
  case ParentProtocol::TYPE_OPEN:
    {
      unsigned instance, request_len, service_name_len;
      const char *request, *service_name;
      ClientData *cdata;

      if (input.get_word(instance) || !(request = input.get_zero_string(request_len))) return EPROTO;
      if (res = check_permission(input.identity(0), request, request_len, instance,
                                 service_name, service_name_len)) return res;

      // check whether such a session is already known from our client
      {
        ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client);
        for (ClientData volatile * c = _client.next(); c; c = _client.next(c))
          if (c->name == service_name && c->pseudonym == input.identity()) {
            utcb << Utcb::TypedMapCap(c->identity);
            return ENONE;
          }
      }

      res = _client.alloc_client_data(utcb, cdata, input.identity(), this);
      if (res) {
        if (res != ERESOURCE) return res;

        unsigned count = 0;
        ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client, utcb, this);
        ClientData volatile * data = _client.get_invalid_client(utcb, this);
        while (data) {
          Logging::printf("s0: found dead client - freeing datastructure\n");
          count++;

          notify_service(utcb, data);

          _client.free_client_data(utcb, data, this);
          data = _client.get_invalid_client(utcb, this, data);
        }
        if (count > 0) return ERETRY;
        else return ERESOURCE;
      }

      cdata->name = service_name;
      cdata->len  = service_name_len;
      utcb << Utcb::TypedMapCap(cdata->identity);
      return ENONE;
    }

  case ParentProtocol::TYPE_CLOSE:
    {
      ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client, utcb, this);
      ClientData *cdata;

      if ((res = _client.get_client_data(utcb, cdata, input.identity()))) return res;
      //Logging::printf("pp: close session for %x for %x\n", cdata->identity, cdata->pseudonym);
      return _client.free_client_data(utcb, cdata, this);
    }
  case ParentProtocol::TYPE_GET_PORTAL:
    {
      unsigned portal, res;
      if (res = get_portal(utcb, input.identity(), portal)) return res;
      utcb << Utcb::TypedMapCap(portal);
      return res;
    }
  case ParentProtocol::TYPE_REGISTER:
    {
      ServerData *sdata;
      char *request;
      unsigned cpu, request_len;
      if (input.get_word(cpu) || !(request = input.get_zero_string(request_len))) return EPROTO;

      // search for an allowed namespace
      unsigned long s0_cmdlen;
      char const * cmdline, * _cmdline = get_client_cmdline(input.identity(0), s0_cmdlen);
      if (!_cmdline) return EPROTO;
      //Logging::printf("\tregister client %x @ cpu %x _cmdline '%.10s' servicename '%.10s'\n", input.identity(), cpu, _cmdline, request);
      cmdline = strstr(_cmdline, "namespace::");
      if (!cmdline || cmdline > _cmdline + s0_cmdlen) return EPERM;
      cmdline += 11;
      unsigned namespace_len = strcspn(cmdline, " \t");

      QuotaGuard<ServerData> guard1(utcb, input.identity(), "mem", request_len + namespace_len + 1);
      QuotaGuard<ServerData> guard2(utcb, input.identity(), "cap", 1, &guard1);
      check1(res, res = guard2.status());
      res = _server.alloc_client_data(utcb, sdata, input.identity(), this);
      if (res == ENONE) guard2.commit();
      else {
        if (res != ERESOURCE) return res;

        unsigned count = 0;
        ClientDataStorage<ServerData, Sigma0, false>::Guard guard_s(&_server, utcb, this);
        ServerData volatile * sdata = _server.get_invalid_client(utcb, this);
        while (sdata) {
          Logging::printf("s0: found dead server - freeing datastructure\n");
          count++;
          free_service(utcb, sdata);
          sdata = _server.get_invalid_client(utcb, this, sdata);
        }
        if (count > 0) return ERETRY;
        else return ERESOURCE;
      }

      sdata->len = namespace_len + request_len + 1;
      char * tmp = new char[sdata->len];
      sdata->name = tmp;
      memcpy(tmp, cmdline, namespace_len);
      memcpy(tmp + namespace_len, request, request_len);
      tmp[sdata->len - 1] = 0;
      sdata->cpu  = cpu;
      sdata->pt   = input.received_cap();

      unsigned client_mem_revoke;
      if (!input.get_word(client_mem_revoke))
        sdata->mem_revoke = get_client_memory(input.identity(), client_mem_revoke);

      {
        ClientDataStorage<ServerData, Sigma0, false>::Guard guard_s(&_server);
        for (ServerData volatile * s2 = _server.next(); s2; s2 = _server.next(s2))
          if (s2->len == sdata->len && !memcmp(sdata->name, s2->name, sdata->len) && sdata->cpu == s2->cpu && sdata->pt != s2->pt) {
            free_service(utcb, sdata);
            return EEXISTS;
          }
      }

      // wakeup clients that wait for us
      {
        ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client);
        for (ClientData volatile * c = _client.next(); c; c = _client.next(c))
        if (c->len == sdata->len-1 && !memcmp(c->name, sdata->name, c->len)) {
          //Logging::printf("\tnotify client %x\n", c->pseudonym);
          unsigned res = nova_semup(c->identity);
          assert(res == ENONE); //c->identity is allocated by us
        }
      }

      utcb << Utcb::TypedMapCap(sdata->identity);
      free_cap = false;
      return ENONE;
    }

  case ParentProtocol::TYPE_UNREGISTER:
    {
      ClientDataStorage<ServerData, Sigma0, false>::Guard guard(&_server, utcb, this);
      ServerData *sdata;

      if ((res = _server.get_client_data(utcb, sdata, input.identity()))) return res;
      //Logging::printf("pp: unregister %s cpu %x\n", sdata->name, sdata->cpu);
      return free_service(utcb, sdata);
    }
  case ParentProtocol::TYPE_GET_QUOTA:
    {
      ClientData *cdata;
      ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client);
      if ((res = _client.get_client_data(utcb, cdata, input.identity(1))))  return res;

      long invalue;
      char *request;
      unsigned request_len;
      if (input.get_word(invalue) || !(request = input.get_zero_string(request_len))) return EPROTO;

      long outvalue = invalue;
      res = ClientData::get_quota(utcb, cdata->pseudonym, request,  invalue, &outvalue);
      utcb << outvalue;
      return res;
    }
  case ParentProtocol::TYPE_REQ_KILL:
    {
      ClientDataStorage<ClientData, Sigma0, false>::Guard guard_c(&_client, utcb, this);

      // find all sessions for a given client
      for (ClientData volatile * c = _client.next(); c; c = _client.next(c)) {
        if (c->pseudonym != input.identity(1)) continue;

        notify_service(utcb, c);

        Logging::printf("s0: freeing session to service on behalf of a dying client\n");
        res = _client.free_client_data(utcb, c, this);
        assert(res == ENONE);
      }
      return ENONE;
    }
  default:
    return EPROTO;
  }
}
