// -*- Mode: C++
// Common routines for 82576 PF and VFs.

#include <host/host82576.h>

void
Base82576::spin(unsigned micros)
{
  timevalue done = _clock->abstime(micros, 1000000);
  while (_clock->time() < done)
    Cpu::pause();
}

bool
Base82576::wait(volatile uint32 &reg, uint32 mask, uint32 value,
		unsigned timeout_micros)
{
  timevalue timeout = _clock->abstime(timeout_micros, 1000000);
  
  while ((reg & mask) != value) {
    Cpu::pause();
    if (_clock->time() >= timeout)
      return false;
  }
  return true;
}

void
Base82576::msg(unsigned level, const char *msg, ...)
{
  if ((level & _msg_level) != 0) {
    va_list ap;
    va_start(ap, msg);
    Logging::printf("82576 %02x: ", _bdf & 0xFF);
    Logging::vprintf(msg, ap);
    va_end(ap);
  }
}

// EOF
