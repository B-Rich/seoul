#pragma once
#include "desc.h"
#include "service/cpu.h"
#include "service/helper.h"
#include "service/string.h"
#include "service/logging.h"

struct UtcbFrame;
struct Utcb
{
  enum {
    STACK_START = 512,
  };

  typedef struct Descriptor
  {
    unsigned short sel, ar;
    unsigned limit, base, res;
    void set(unsigned short _sel, unsigned _base, unsigned _limit, unsigned short _ar) { sel = _sel; base = _base; limit = _limit; ar = _ar; };
  } Descriptor;

  struct head {
    union {
      struct {
	unsigned short untyped;
	unsigned short typed;
      };
      unsigned mtr;
    };
    unsigned crd;
    unsigned tls;
    unsigned reserved;
  } head;
  union {
    struct {
      unsigned     mtd;
      unsigned     inst_len, eip, efl;
      unsigned     intr_state, actv_state, inj_info, inj_error;
      union {
	struct {
#define GREG(NAME)					\
	  union {					\
	    struct {					\
	      unsigned char           NAME##l, NAME##h;	\
	    };						\
	    unsigned short          NAME##x;		\
	    unsigned           e##NAME##x;		\
	  }
#define GREG16(NAME)				\
	  union {				\
	    unsigned short          NAME;	\
	    unsigned           e##NAME;		\
	  }
	  GREG(a);    GREG(c);    GREG(d);    GREG(b);
	  GREG16(sp); GREG16(bp); GREG16(si); GREG16(di);
	};
	unsigned gpr[8];
      };
      unsigned long long qual[2];
      unsigned     ctrl[2];
      unsigned long long tsc_off;
      unsigned     cr0, cr2, cr3, cr4;
      unsigned     dr7, sysenter_cs, sysenter_esp, sysenter_eip;
      Descriptor   es, cs, ss, ds, fs, gs;
      Descriptor   ld, tr, gd, id;
    };
    unsigned msg[(4096 - sizeof(struct head)) / sizeof(unsigned)];
  };


  void set_header(unsigned untyped, unsigned typed) { head.untyped = untyped; head.typed = typed; }
  unsigned *item_start() { return reinterpret_cast<unsigned *>(this + 1) - 2*head.typed; }


  unsigned get_nested_mtr(unsigned &typed_offset) {
    if (!msg[STACK_START]) {
      typed_offset = sizeof(msg) / sizeof(unsigned) - 2 * head.typed;
      return head.mtr;
    }
    unsigned old_ofs = msg[msg[STACK_START] + STACK_START] + STACK_START + 1;
    typed_offset = old_ofs + HEADER_SIZE/sizeof(unsigned) + (msg[old_ofs] & 0xffff);
    return msg[old_ofs];
  }


  unsigned get_received_cap() {
    unsigned typed_ofs;
    unsigned mtr = get_nested_mtr(typed_ofs);
    for (unsigned i=mtr >> 16; i > 0; i--)
      if (msg[typed_ofs + i * 2 - 1] & 1)
	return msg[typed_ofs + i * 2 - 2] >> Utcb::MINSHIFT;
    return 0;
  }

  unsigned get_identity(unsigned skip=0) {
    unsigned typed_ofs;
    unsigned mtr = get_nested_mtr(typed_ofs);
    //Logging::printf("iden %p %x %x %x\n", this, mtr, typed_ofs, msg[STACK_START]);
    for (unsigned i=mtr >> 16; i > 0; i--)
      if (~msg[typed_ofs + i * 2 - 1] & 1 && !skip--)
	return msg[typed_ofs + i * 2 - 2] >> Utcb::MINSHIFT;
    return 0;
  }


  // MessageBuffer abstraction
  struct TypedMapCap {
    unsigned value;
    void fill_words(unsigned *ptr) {   *ptr++ = value;  *ptr = 1;  }
    TypedMapCap(unsigned cap, unsigned attr = DESC_CAP_ALL) : value(cap << MINSHIFT | attr) {}
  };

  struct TypedIdentifyCap {
    unsigned value;
    void fill_words(unsigned *ptr) {   *ptr++ = value;  *ptr = 0;  }
    TypedIdentifyCap(unsigned cap, unsigned attr = DESC_CAP_ALL) : value(cap << MINSHIFT | attr) {}
  };

  /**
   * TODO: put error code at some fixed point
   */
  Utcb &  add_frame() {
    unsigned ofs = msg[STACK_START] + STACK_START + 1;

    //Logging::printf("add  %p frame at %x %x/%x\n", this, ofs, head.untyped, head.typed);
    // save header, untyped and typed items
    memcpy(msg+ofs, &head, HEADER_SIZE);
    ofs += HEADER_SIZE/sizeof(unsigned);
    memcpy(msg + ofs, msg, head.untyped*sizeof(unsigned));
    ofs += head.untyped;
    memcpy(msg + ofs, msg + sizeof(msg) / sizeof(unsigned) - head.typed* 2, head.typed * 2 * 4);
    ofs += head.typed * 2;
    msg[ofs++] = msg[STACK_START];
    msg[STACK_START] = ofs - STACK_START - 1;

    assert(ofs < sizeof(msg) / sizeof(unsigned));

    // init the header
    head.mtr = 0;
    head.crd = ~0;
    return *this;
  }

  void skip_frame() {
    //Logging::printf("skip %p frame at %x-%x\n", this, ofs, msg[ofs - 1] + STACK_START + 1);
    unsigned mtr = head.mtr;
    unsigned ofs = msg[STACK_START] + STACK_START + 1;
    msg[STACK_START] = msg[ofs - 1];
    memcpy(&head, msg + msg[STACK_START] + STACK_START + 1 , HEADER_SIZE);
    head.mtr = mtr;
  }

  void drop_frame() {
    unsigned ofs = msg[STACK_START] + STACK_START + 1;
    assert (ofs > STACK_START + 1);

    // XXX clear the UTCB to detect bugs
    memset(msg, 0xe8, 512 * 4);
    memset(msg+ofs, 0xd5, sizeof(msg) - ofs * 4);

    unsigned old_ofs = msg[ofs - 1] +  STACK_START + 1;
    //Logging::printf("drop %p frame %x-%x\n", this, old_ofs, ofs);
    ofs = old_ofs;
    memcpy(&head, msg+ofs, HEADER_SIZE);
    ofs += HEADER_SIZE/sizeof(unsigned);
    memcpy(msg, msg+ofs, head.untyped * sizeof(unsigned));
    ofs += head.untyped;
    memcpy(msg + sizeof(msg) / sizeof(unsigned) - head.typed* 2, msg + ofs, head.typed * 2 * 4);
    ofs += head.typed * 2;
    msg[STACK_START] = old_ofs - STACK_START - 1;
  }


  Utcb &  operator <<(unsigned value) {
    msg[head.untyped++] = value;
    return *this;
  }


  template <typename T>
  Utcb &  operator <<(T &value) {
    unsigned words = (sizeof(T) + sizeof(unsigned) - 1)/ sizeof(unsigned);
    *reinterpret_cast<T *>(msg + head.untyped) = value;
    head.untyped += words;
    return *this;
  }

  Utcb &  operator <<(const char *value) {
    unsigned olduntyped = head.untyped;
    unsigned slen =  strlen(value) + 1;
    head.untyped += (slen + sizeof(unsigned) - 1 ) / sizeof(unsigned);
    msg[head.untyped - 1] = 0;
    memcpy(msg + olduntyped, value, slen);
    return *this;
  }

  Utcb &  operator <<(TypedMapCap value) {
    head.typed++;
    value.fill_words(item_start());
    return *this;
  }

  Utcb &  operator <<(TypedIdentifyCap value) {
    head.typed++;
    value.fill_words(item_start());
    return *this;
  }

  Utcb &  operator <<(Crd value) { head.crd = value.value(); return *this; }

  template <typename T>
  bool  operator >>(T &value) {
    unsigned offset = msg[0];
    unsigned words = (sizeof(T) + sizeof(unsigned) - 1)/ sizeof(unsigned);
    if (offset + words > head.untyped) return true;
    msg[0] += words;
    value = *reinterpret_cast<T *>(msg + offset + 1);
    return false;
  }

  // XXX
  enum { MINSHIFT = 12 };
  enum {
    HEADER_SIZE = sizeof(struct head),
  };


};
enum {
  EXCEPTION_WORDS = 72,
};


