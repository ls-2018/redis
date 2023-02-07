#ifndef __REDIS_ASSERT_H__
#define __REDIS_ASSERT_H__

#include "over-config.h"

#define assert(_e) (likely((_e)) ? (void)0 : (_serverAssert(#_e, __FILE__, __LINE__), redis_unreachable()))
#define panic(...) _serverPanic(__FILE__, __LINE__, __VA_ARGS__), redis_unreachable()

void _serverAssert(char *estr, char *file, int line);

void _serverPanic(const char *file, int line, const char *msg, ...);

#endif
