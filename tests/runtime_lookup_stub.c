#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name){for(size_t i=0;i<count;++i)if(!strcmp(table[i].name,name))return (uintptr_t)table[i].function;return 0;}
