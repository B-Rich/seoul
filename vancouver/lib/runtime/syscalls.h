/*
 * NOVA system-call interface.
 *
 * Copyright (C) 2008, Bernhard Kauer <kauer@tudos.org>
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
#pragma once
#include "sys/hip.h"
#include "sys/utcb.h"

enum
{
  NOVA_IPC_CALL,
  NOVA_IPC_REPLY,
  NOVA_CREATE_PD,
  NOVA_CREATE_EC,
  NOVA_CREATE_SC,
  NOVA_CREATE_PT,
  NOVA_CREATE_SM,
  NOVA_REVOKE,
  NOVA_RECALL,
  NOVA_SEMCTL,
  NOVA_PERFCNT,
  NOVA_FLAG0          = 1 << 8,
  NOVA_FLAG1          = 1 << 9,
  NOVA_FLAG2          = 1 << 10,
  NOVA_IPC_SEND       = NOVA_IPC_CALL | NOVA_FLAG1 | NOVA_FLAG2,
  NOVA_IPC_SEND0      = NOVA_IPC_SEND | NOVA_FLAG0,
  NOVA_CREATE_PDVM    = NOVA_CREATE_PD| NOVA_FLAG0,
  NOVA_REVOKE_MYSELF  = NOVA_REVOKE | NOVA_FLAG0,
  NOVA_SEMCTL_UP      = NOVA_SEMCTL,
  NOVA_SEMCTL_DOWN    = NOVA_SEMCTL | NOVA_FLAG0,
};

enum ERROR
  {
    NOVA_ESUCCESS = 0,
    NOVA_ETIMEOUT,
    NOVA_ESYS,
    NOVA_ECAP,
    NOVA_EMEM,
  };


static inline
unsigned char
syscall(unsigned w0, unsigned w1, unsigned w2, unsigned w3, unsigned w4)
{
  asm volatile ("push %%ebp;"
		"mov %%ecx, %%ebp;"
		"mov %%esp, %%ecx;"
		"mov $1f, %%edx;"
		"sysenter;"
		"1: ;"
		"pop %%ebp;"
		: "+a" (w0), "+c"(w4)
		: "D" (w1), "S" (w2), "b"(w3)
		: "edx", "memory");
  return w0;
}

inline unsigned char  idc_call(unsigned idx_pt, Mtd mtd_send)
{  return syscall(NOVA_IPC_CALL, idx_pt, mtd_send.value(), 0, 0); }


inline unsigned char  idc_send(unsigned idx_pt, Mtd mtd_send)
{  return syscall(NOVA_IPC_SEND, idx_pt, mtd_send.value(), 0, 0); }


inline unsigned char  idc_send0(unsigned idx_pt, Mtd mtd_send)
{  return syscall(NOVA_IPC_SEND0, idx_pt, mtd_send.value(), 0, 0); }


extern "C" void __attribute__((noreturn)) __attribute__((regparm(1))) idc_reply_and_wait_fast(Utcb *utcb);

inline unsigned char  create_pd (unsigned idx_pd, unsigned utcb, Crd pt_crd, Qpd qpd, bool vcpus)
{  return syscall(vcpus ? NOVA_CREATE_PDVM : NOVA_CREATE_PD, idx_pd, utcb, qpd.value(), pt_crd.value()); }


inline unsigned char  create_ec(unsigned idx_ec, void *utcb, void *esp)
{  return syscall(NOVA_CREATE_EC, idx_ec, reinterpret_cast<unsigned>(utcb), reinterpret_cast<unsigned>(esp), 0); }


inline unsigned char  create_sc (unsigned idx_sc, unsigned idx_ec, Qpd qpd)
{  return syscall(NOVA_CREATE_SC, idx_sc, idx_ec, qpd.value(), 0); }


inline unsigned char  create_pt(unsigned idx_pt, unsigned idx_ec, void __attribute__((regparm(0))) (*eip)(Utcb *utcb), Mtd mtd)
{  return syscall(NOVA_CREATE_PT, idx_pt, idx_ec, mtd.value(), reinterpret_cast<unsigned>(eip)); }


inline unsigned char  create_sm(unsigned idx_sm, unsigned initial = 0)
{  return syscall(NOVA_CREATE_SM, idx_sm, initial, 0, 0); }


inline unsigned char  revoke(Crd crd, bool myself) 
{  return syscall(myself ? NOVA_REVOKE_MYSELF : NOVA_REVOKE, crd.value(), 0, 0, 0); }


inline unsigned char  recall(unsigned idx_ec)
{  return syscall(NOVA_RECALL, idx_ec, 0, 0, 0); }

inline unsigned char  semup(unsigned idx_sm) 
{  return syscall(NOVA_SEMCTL_UP, idx_sm, 0, 0, 0); }


inline unsigned char  semdown(unsigned idx_sm) 
{  return syscall(NOVA_SEMCTL_DOWN, idx_sm, 0, 0, 0); }


/**
 * Experimental perfcounter syscall.
 * Events are: ((eventnr << 8) | umask)
 */
static inline
unsigned char
perfcount(unsigned event1, unsigned event2, unsigned long long &count1, unsigned long long &count2)
{
  unsigned w0 = NOVA_PERFCNT;
  unsigned w3 = 0;
  unsigned w4 = 0;
  asm volatile ("push %%ebp;"
		"mov %%ecx, %%ebp;"
		"mov %%esp, %%ecx;"
		"mov $1f, %%edx;"
		"sysenter;"
		"1: ;"
		"mov %%ebp, %%ecx;"
		"pop %%ebp;"
		: "+a" (w0), "+D" (event1), "+S" (event2), "+b"(w3), "+c"(w4) :
		: "edx", "memory");
  count1 = event1 | static_cast<unsigned long long>(w3) << 32;
  count2 = event2 | static_cast<unsigned long long>(w4) << 32;
  return w0;
}
