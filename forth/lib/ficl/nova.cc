// -*- Mode: C++ -*-

#include <ficl.h>
#include <sys/syscalls.h>
#include <stdio.h>
#include <assert.h>

static unsigned *free_cap;

static void CapPtr(FICL_VM *pVM)
{
  stackPushPtr(pVM->pStack, &free_cap);
}

static void allocCap(FICL_VM *pVM)
{
  stackPushUNS(pVM->pStack, (*free_cap)++);
}

static void createSm(FICL_VM *pVM)
{
  unsigned cap  = stackPopUNS(pVM->pStack);
  unsigned init = stackPopUNS(pVM->pStack);

  stackPushUNS(pVM->pStack, create_sm(cap, init));
}

static void semUp(FICL_VM *pVM)
{
  unsigned cap = POPUNS();
  PUSHUNS(semup(cap));
}

static void semDown(FICL_VM *pVM)
{
  unsigned cap = stackPopUNS(pVM->pStack);
  stackPushUNS(pVM->pStack, semdown(cap));
}

extern "C"
void ficlCompileNova(FICL_SYSTEM *pSys)
{
  FICL_DICT *dp = pSys->dp;
  assert (dp);

  dictAppendWord(dp, "capptr",   CapPtr,    FW_DEFAULT);
  dictAppendWord(dp, "alloccap", allocCap,  FW_DEFAULT);  
  dictAppendWord(dp, "createsm", createSm,  FW_DEFAULT); // ( init capidx -- success )
  dictAppendWord(dp, "semup",    semUp,     FW_DEFAULT); // ( capidx -- success )
  dictAppendWord(dp, "semdown",  semDown,   FW_DEFAULT); // ( capidx -- success )
}
