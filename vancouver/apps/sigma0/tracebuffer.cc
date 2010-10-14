/*
 * Tracebuffer for the clients.
 *
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

#include "nul/motherboard.h"
#include "nul/generic_service.h"


/**
 * Tracebuffer service.
 *
 * Missing: trace-buffer output on debug key
 */
class Tracebuffer {
  unsigned long _size;
  unsigned long _pos;
  char        * _buf;

  long _anon_sessions;
  struct ClientData : public GenericClientData {
    long guid;
  };
  ClientDataStorage<ClientData> _storage;


  static void trace_putc(void *data, int value) {
    if (value < 0) return;
    Tracebuffer *t = reinterpret_cast<Tracebuffer *>(data);
    t->_buf[(t->_pos++) % t->_size] = value;
  }


  void trace_printf(const char *format, ...)
  {
    va_list ap;
    va_start(ap, format);
    Vprintf::vprintf(trace_putc, this, format, ap);
    va_end(ap);
  }

public:
  unsigned portal_func(Utcb *utcb, bool &free_cap) {
    ClientData *data = 0;
    unsigned res = ENONE;

    //Logging::printf("%s mtr %x/%x id %x t %x\n", __PRETTY_FUNCTION__, utcb->head.mtr.untyped(), utcb->head.mtr.typed(), utcb->get_identity(), utcb->msg[0]);
    check1(EPROTO, utcb->head.mtr.untyped() < 1);
    switch (utcb->msg[0]) {
    case ParentProtocol::TYPE_OPEN:
      check1(res, res = _storage.alloc_client_data(utcb, data, 0, utcb->get_received_cap()));
      free_cap = false;
      if (ParentProtocol::get_quota(utcb, data->parent_cap, "guid", data->guid, &data->guid))
	data->guid = --_anon_sessions;
      *utcb << Utcb::TypedMapCap(data->identity);
      Logging::printf("client data %x guid %lx parent %x\n", data->identity, data->guid, data->parent_cap);
      return res;
    case ParentProtocol::TYPE_CLOSE:
      check1(res, res = _storage.get_client_data(utcb, data));
      Logging::printf("close session for %lx\n", data->guid);
      return _storage.free_client_data(utcb, data, 0);
    case LogProtocol::TYPE_LOG:
      check1(EPROTO, utcb->head.mtr.untyped() < 2);
      check1(res, res = _storage.get_client_data(utcb, data));
      Logging::printf("[%4lx] %.*s\n", data->guid, sizeof(unsigned)*(utcb->head.mtr.untyped() - 1), reinterpret_cast<char *>(utcb->msg + 1));
      trace_printf("[%4lx] %.*s\n", data->guid, sizeof(unsigned)*(utcb->head.mtr.untyped() - 1), reinterpret_cast<char *>(utcb->msg + 1));
      return ENONE;
    default:
      return EPROTO;
    }
  }

public:
  Tracebuffer(unsigned long size, char *buf) : _size(size), _pos(0), _buf(buf) {}



#if 0
      case 4:
	Logging::printf("Trace buffer at %x bytes (%x).\n\n", _trace_pos, TRACE_BUF_SIZE);
	Logging::printf("%.*s", TRACE_BUF_SIZE - (_trace_pos % TRACE_BUF_SIZE), _trace_buf + (_trace_pos % TRACE_BUF_SIZE));
	if (_trace_pos % TRACE_BUF_SIZE) Logging::printf("%.*s", _trace_pos % TRACE_BUF_SIZE, _trace_buf);
	Logging::printf("\nEOF trace buffer\n\n");
	break;
      }
#endif
};

PARAM(tracebuffer,
      unsigned long size = argv[0];
      Tracebuffer *t = new Tracebuffer(size, new char[size]);
      MessageHostOp msg(MessageHostOp::OP_REGISTER_SERVICE, reinterpret_cast<unsigned long>(StaticPortalFunc<Tracebuffer>::portal_func), reinterpret_cast<unsigned long>(t));
      msg.ptr = const_cast<char *>("/log");
      check0(!mb.bus_hostop.send(msg), "registering the service failed");
      ,
      "tracebuffer:size - instanciate a tracebuffer for the clients")
