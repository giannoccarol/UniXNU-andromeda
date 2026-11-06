#pragma once
#include <mutex>
typedef std::mutex IOLock;
inline IOLock *IOLockAlloc(){return new IOLock;}
inline void IOLockFree(IOLock *p){delete p;}
inline void IOLockLock(IOLock *p){p->lock();}
inline void IOLockUnlock(IOLock *p){p->unlock();}
