/*
 * Parent protocol.
 *
 * Copyright (C) 2010, Bernhard Kauer <bk@vmmon.org>
 * Copyright (C) 2011, Alexander Boettcher <boettcher@tudos.org>
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
#pragma once

#include "sys/syscalls.h"
#include "sys/semaphore.h"
#include "nul/config.h"
#include "nul/error.h"

/**
 * The protocol our parent provides.  It allows to open a new session
 * and request per-CPU-portals.
 *
 * To be used by both clients and services.
 *
 * Missing:   restrict quota, get_quota
 */
struct ParentProtocol {

  /** Protocol operations (message types). */
  enum {
    // generic protocol operations (sent either to parent or to services)
    TYPE_INVALID = 0, ///< used as error indicator
    TYPE_OPEN, 	      ///< Get pseudonym (when sent to the parent) or open session (when sent to a service)
    TYPE_CLOSE,
    TYPE_GENERIC_END,

    // service specific operations
    TYPE_GET_PORTAL,
    TYPE_REGISTER,
    TYPE_UNREGISTER,
    TYPE_GET_QUOTA,
    TYPE_SINGLETON,
    TYPE_REQ_KILL,
  };

  /** Capabilities passed by parents to children. */
  enum {
    CAP_CHILD_ID  = 252,
    CAP_SC_USAGE  = 253,
    CAP_CHILD_EC  = 254,
    CAP_PARENT_ID = 255,
    CAP_PT_PERCPU = 256,
  };

  static Utcb & init_frame(Utcb &utcb, unsigned op, unsigned id) { return utcb.add_frame() << op << Utcb::TypedIdentifyCap(id); }

  /**
   * Low-level systemcall.
   */
  static unsigned call(Utcb &utcb, unsigned cap_base, bool drop_frame, bool percpu = true) {
    unsigned res;
    res = nova_call(cap_base + (percpu ? utcb.head.nul_cpunr : 0));
    if (!res) res = utcb.msg[0];
    if (drop_frame) utcb.drop_frame();
    return res;
  }

  static unsigned get_pseudonym(Utcb &utcb, const char *service, unsigned instance,
				unsigned cap_pseudonym, unsigned parent_id = CAP_PARENT_ID) {
    return call(init_frame(utcb, TYPE_OPEN, parent_id) << instance << Utcb::String(service)
						       << Crd(cap_pseudonym, 0, DESC_CAP_ALL),
		CAP_PT_PERCPU, true);
  }

  static unsigned release_pseudonym(Utcb &utcb, unsigned cap_pseudonym) {
    init_frame(utcb, TYPE_CLOSE, cap_pseudonym);
    return call(utcb, CAP_PT_PERCPU, true);
  }

  /** 
   * Ask parent to get the portal to talk to the service.
   */
  static unsigned get_portal(Utcb &utcb, unsigned cap_pseudonym, unsigned cap_portal, bool blocking, char const * service_name = 0) {

    init_frame(utcb, TYPE_GET_PORTAL, cap_pseudonym) << Crd(cap_portal, 0, DESC_CAP_ALL);
    unsigned res = call(utcb, CAP_PT_PERCPU, true);

    // block on the parent session until something happens
    if (res == ERETRY && blocking) {
      //Logging::printf("blocked - waiting for session %x %s\n", cap_pseudonym, service_name);
      res = nova_semdownmulti(cap_pseudonym);
      //Logging::printf("we woke up %u %s\n", res, service_name);
      if (res != ENONE) {
        res = nova_revoke(Crd(cap_pseudonym, 0, DESC_CAP_ALL), true);
        assert(res == ENONE);
      }
    }
    return res;
  }

  /**
   * @param revoke_mem Memory page used to substitute the memory that
   * was provided to the service by a client, but was revoked later.
   * Having such memory makes it easier for services to deal with
   * situations when the memory is revoked during service operation.
   */
  static unsigned
  register_service(Utcb &utcb, const char *service, unsigned cpu, unsigned pt,
                   unsigned cap_service, char * revoke_mem = 0)
  {
    assert(cap_service);
    init_frame(utcb, TYPE_REGISTER, CAP_PARENT_ID) << cpu << Utcb::String(service) << reinterpret_cast<unsigned>(revoke_mem)
						   << Utcb::TypedMapCap(pt) << Crd(cap_service, 0, DESC_CAP_ALL);
    return call(utcb, CAP_PT_PERCPU, true);
  };

  static unsigned unregister_service(Utcb &utcb, unsigned cap_service) {
    init_frame(utcb, TYPE_UNREGISTER, cap_service);
    return call(utcb, CAP_PT_PERCPU, true);
  }

  static unsigned get_quota(Utcb &utcb, unsigned cap_client_pseudonym, const char *name, long invalue, long *outvalue=0) {
    init_frame(utcb, TYPE_GET_QUOTA, CAP_PARENT_ID) << invalue << Utcb::String(name)
						    << Utcb::TypedIdentifyCap(cap_client_pseudonym);
    unsigned res = call(utcb, CAP_PT_PERCPU, false);
    if (!res && outvalue && utcb >> *outvalue)  res = EPROTO;
    utcb.drop_frame();
    return res;
  }

  /// @see check_singleton()
  static unsigned set_singleton(Utcb &utcb, unsigned cap_client_pseudonym, unsigned cap_local_session) {
    init_frame(utcb, TYPE_SINGLETON, CAP_PARENT_ID) << 1U << Utcb::TypedIdentifyCap(cap_client_pseudonym)
						    << Utcb::TypedMapCap(cap_local_session);
    return call(utcb, CAP_PT_PERCPU, true);
  }

  /// Services can use check_singleton() in conjunction with
  /// set_singleton() to enforce a one-session-per-client policy. This is
  /// not done automatically.
  static unsigned check_singleton(Utcb &utcb, unsigned cap_client_pseudonym, unsigned &cap_local_session,
				  Crd crd = Crd(0, 31, DESC_CAP_ALL)) {
    init_frame(utcb, TYPE_SINGLETON, CAP_PARENT_ID) << 2U << Utcb::TypedIdentifyCap(cap_client_pseudonym);
    utcb.head.crd_translate = crd.value();
    Utcb::Frame input(&utcb, sizeof(utcb.msg) / sizeof(unsigned));
    unsigned res = call(utcb, CAP_PT_PERCPU, false);
    cap_local_session = input.translated_cap().cap();
    utcb.drop_frame();
    return res;
  }
  static unsigned kill(Utcb &utcb, unsigned cap_client_pseudonym, unsigned service_cap = 0) {
    init_frame(utcb, TYPE_REQ_KILL, CAP_PARENT_ID) << Utcb::TypedIdentifyCap(cap_client_pseudonym);
    return call(utcb, service_cap ? service_cap : 0U + CAP_PT_PERCPU, true, !service_cap);
  }

};


/**
 * Generic protocol handling, hides the parent protocol
 * handling. Specific protocols will be inherited from it.
 * This is meant to be used only by clients.
 *
 * Features: session-open, parent-open, request-portal, optional blocking
 * Missing: restrict-quota
 */
class GenericProtocol : public ParentProtocol {
protected:
  const char *_service;
  unsigned    _instance;
  unsigned    _cap_base;	///< Base of the capability range. This cap refers to CAP_PSEUDONYM.
  Semaphore   _lock;
  bool        _blocking;
  bool        _disabled;
public:

  /// Client capabilities used to talk to the service 
  enum {
    CAP_PSEUDONYM,
    CAP_LOCK,
    CAP_SERVER_SESSION,
    CAP_SERVER_PT              ///< Portal for CPU0
  };

  /**
   * Call the server in a loop to resolve all faults.
   */
  unsigned call_server(Utcb &utcb, bool drop_frame) {
    unsigned res = EPERM;
    unsigned mtr = utcb.head.mtr;
    unsigned w0  = utcb.msg[0];
    while (!_disabled) {
    do_call:
      res = call(utcb, _cap_base + CAP_SERVER_PT, false);
      if (res == EEXISTS) {
        utcb.head.mtr = mtr;
        utcb.msg[0]   = w0;
        goto do_open_session;
      }
      if (res == NOVA_ECAP) goto do_get_portal;
      goto do_out;

    do_open_session:
      utcb.add_frame() << TYPE_OPEN << Utcb::TypedMapCap(_cap_base + CAP_PSEUDONYM)
		       << Crd(_cap_base + CAP_SERVER_SESSION, 0, DESC_CAP_ALL);
      res = call(utcb, _cap_base + CAP_SERVER_PT, true);
      if (res == ENONE)     goto do_call;
      if (res == EEXISTS)   goto do_get_pseudonym;
      if (res == NOVA_ECAP) goto do_get_portal;
      goto do_out;
    do_get_portal:
      {
        // we lock to avoid missing wakeups on retry
        SemaphoreGuard guard(_lock);
        res = ParentProtocol::get_portal(utcb, _cap_base + CAP_PSEUDONYM,
					 _cap_base + CAP_SERVER_PT + utcb.head.nul_cpunr, _blocking, _service);
      }
      if (res == ERETRY && _blocking) goto do_get_portal;
      if (res == ENONE)     goto do_call;
      if (res == EEXISTS)   goto do_get_pseudonym;
      goto do_out;

    do_get_pseudonym:
      res = ParentProtocol::get_pseudonym(utcb, _service, _instance, _cap_base + CAP_PSEUDONYM);
      if (res == ENONE)     goto do_call;
      _disabled = true;
      goto do_out;
    }
  do_out:
    if (drop_frame) utcb.drop_frame();
    return res;
  }

  /**
   * Destroy the object.
   *
   * - Revoke all caps and add caps back to cap allocator.
   */
  template <class T>
  void destroy(Utcb &utcb, unsigned portal_num, T * obj) {
    release_pseudonym(utcb, _cap_base + CAP_PSEUDONYM);
    obj->dealloc_cap(_cap_base, portal_num);
  }

  /**
   * Close the session to the parent.
   * - Revoke all caps we got from external and we created (default: revoke_lock = true)
   * - If revoke_lock == false than this object can be reused afterwards to create another session.
   */
  void close(Utcb &utcb, unsigned portal_num, bool revoke_lock = true, bool _release_pseudonym = true) {
    unsigned char res;
    if (_release_pseudonym) release_pseudonym(utcb, _cap_base + CAP_PSEUDONYM);

    if (revoke_lock) res = nova_revoke(Crd(_cap_base + CAP_LOCK, 0, DESC_CAP_ALL), true);
    res = nova_revoke(Crd(_cap_base + CAP_PSEUDONYM, 0, DESC_CAP_ALL), true);
    res = nova_revoke(Crd(_cap_base + CAP_SERVER_SESSION, 0, DESC_CAP_ALL), true);
    for (unsigned i=0; i < portal_num; i++)
      res = nova_revoke(Crd(_cap_base + CAP_SERVER_PT, 0, DESC_CAP_ALL), true);
    (void)res;
  }

  unsigned get_notify_sm() { return _cap_base + CAP_SERVER_SESSION; }

  Utcb & init_frame(Utcb &utcb, unsigned op) { return utcb.add_frame() << op << Utcb::TypedIdentifyCap(_cap_base + CAP_SERVER_SESSION); }

  GenericProtocol(const char *service, unsigned instance, unsigned cap_base, bool blocking)
    : _service(service), _instance(instance), _cap_base(cap_base), _lock(cap_base + CAP_LOCK), _blocking(blocking), _disabled(false)
  {
    unsigned res;
    res = nova_create_sm(cap_base + CAP_LOCK);
    assert(res == NOVA_ESUCCESS);
    _lock.up();
//    Logging::printf("New Protocol '%s' base %x\n", service, cap_base);
  }
};

