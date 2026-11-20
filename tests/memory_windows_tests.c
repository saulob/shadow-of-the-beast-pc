/* Windows guest memory: sparse backing, on-demand commitment, aliases, protection and release.
 * Includes the implementation so commitment can be inspected and failed on demand. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_log_flush(void) {}
int32_t *runtime_errno(void) { static int32_t value; return &value; }
int32_t runtime_guest_errno(int e) { return e; }
#include <windows.h>
#include <psapi.h>
#include <fcntl.h>
#include <io.h>
static int fail_section_creation, fail_commit;
static LPVOID WINAPI test_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
    if (fail_commit && (type & MEM_COMMIT)) { SetLastError(ERROR_COMMITMENT_LIMIT); return NULL; }
    return VirtualAlloc(address,size,type,protection);
}
static HANDLE WINAPI test_create_section(HANDLE file, SECURITY_ATTRIBUTES *security, ULONG access, ULONG protection,
                                         ULONG attributes, ULONG64 size, PCWSTR name,
                                         MEM_EXTENDED_PARAMETER *parameters, ULONG count) {
    if (fail_section_creation) { SetLastError(ERROR_COMMITMENT_LIMIT); return NULL; }
    return CreateFileMapping2(file,security,access,protection,attributes,size,name,parameters,count);
}
#define CreateFileMapping2 test_create_section
#define VirtualAlloc test_virtual_alloc
#include "../src/runtime_memory.c"
#undef CreateFileMapping2
#undef VirtualAlloc
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s (WinError %lu)\n",__LINE__,#x,GetLastError()); } } while(0)

enum { READERS = 4, READS_PER_READER = 5000, SLICE = 9 * PAGE, READ_BYTES = 131072, VIEW_PAGES = 4 * 9 + 8 };
typedef enum { RACE_PROTECT = 1, RACE_SPLIT = 2 } RaceKind;
typedef struct { int fd; unsigned slice, misalign; unsigned long io_error[3], other, mismatches, done; unsigned char *view; } Reader;
static unsigned char *race_view;
static int64_t race_phys;
static volatile LONG race_stop;

/* Like the GPU's page tracking: a write to a protected page unprotects it and the instruction runs again. */
static LONG CALLBACK race_unprotect(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    if (record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || record->NumberParameters<2) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at=(uintptr_t)record->ExceptionInformation[1];
    if (at<(uintptr_t)race_view || at>=(uintptr_t)race_view+VIEW_PAGES*PAGE) return EXCEPTION_CONTINUE_SEARCH;
    runtime_memory_gpu_protect(at&~(uintptr_t)4095,4096,1,1);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI race_protect(LPVOID unused) {
    (void)unused;
    while (!race_stop) {
        runtime_memory_gpu_protect((uintptr_t)race_view,VIEW_PAGES*PAGE,0,0);
        runtime_memory_gpu_protect((uintptr_t)race_view,VIEW_PAGES*PAGE,1,0);
    }
    return 0;
}
/* Maps the whole range, then over its first page: the split unmaps the pages after it for a moment. */
static DWORD WINAPI race_split(LPVOID unused) {
    (void)unused;
    while (!race_stop) {
        void *whole=race_view, *first=race_view;
        direct_map(&whole,VIEW_PAGES*PAGE,3,MAP_FIXED_FLAG,race_phys,0);
        direct_map(&first,PAGE,3,MAP_FIXED_FLAG,race_phys,0);
    }
    return 0;
}
static DWORD WINAPI race_reader(LPVOID parameter) {
    Reader *r=parameter;
    for (unsigned i=0;i<READS_PER_READER;++i) {
        const int64_t offset=(int64_t)((i*7u+r->slice)%32)*PAGE+(int64_t)r->misalign;
        unsigned char *into=r->view+PAGE+r->slice*SLICE+r->misalign;
        const int64_t n=runtime_file_pread(r->fd,into,READ_BYTES,offset);
        if (n!=READ_BYTES) {
            const unsigned long e=compat_io_error();
            if (e==ERROR_INVALID_USER_BUFFER) ++r->io_error[0];
            else if (e==ERROR_NOACCESS) ++r->io_error[1];
            else ++r->other;
            if (n==-EIO) ++r->io_error[2];
            continue;
        }
        for (unsigned p=0;p<READ_BYTES;p+=4096+(i&1)*123)
            if (into[p]!=(unsigned char)(((uint64_t)offset+p)/PAGE)) { ++r->mismatches; break; }
        ++r->done;
    }
    return 0;
}
static void race_case(const char *label, int kinds, int fd, unsigned misalign, int expect_clean) {
    Reader readers[READERS]={0};
    HANDLE threads[READERS+2]; unsigned count=0;
    race_stop=0;
    for (unsigned i=0;i<READERS;++i) {
        readers[i]=(Reader){.fd=fd,.slice=i,.misalign=misalign,.view=race_view};
        threads[count++]=CreateThread(NULL,0,race_reader,&readers[i],0,NULL);
    }
    HANDLE protect=(kinds&RACE_PROTECT) ? CreateThread(NULL,0,race_protect,NULL,0,NULL) : NULL;
    HANDLE split=(kinds&RACE_SPLIT) ? CreateThread(NULL,0,race_split,NULL,0,NULL) : NULL;
    WaitForMultipleObjects(count,threads,TRUE,INFINITE);
    race_stop=1;
    if (protect) WaitForSingleObject(protect,INFINITE);
    if (split) WaitForSingleObject(split,INFINITE);
    unsigned long invalid=0,noaccess=0,eio=0,other=0,bad=0,done=0;
    for (unsigned i=0;i<READERS;++i) {
        invalid+=readers[i].io_error[0]; noaccess+=readers[i].io_error[1]; eio+=readers[i].io_error[2];
        other+=readers[i].other; bad+=readers[i].mismatches; done+=readers[i].done;
        CloseHandle(threads[i]);
    }
    if (protect) CloseHandle(protect);
    if (split) CloseHandle(split);
    printf("Windows memory: %-34s %lu reads ok, 1784=%lu, 998=%lu, EIO=%lu, other=%lu, wrong bytes=%lu\n",label,done,invalid,noaccess,eio,
           other,bad);
    if (expect_clean) {
        CHECK(done==(unsigned long)READERS*READS_PER_READER);
        CHECK(invalid==0 && noaccess==0 && eio==0 && other==0 && bad==0);
    }
}
static void file_reads_race_memory_changes(void) {
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH,dir);
    snprintf(dir+strlen(dir),sizeof(dir)-strlen(dir),"sotb_io_race_%lu",GetCurrentProcessId());
    CreateDirectoryA(dir,NULL);
    snprintf(path,sizeof(path),"%s/pak.bin",dir);
    FILE *out=fopen(path,"wb");
    CHECK(out!=NULL);
    if (!out) return;
    for (unsigned p=0;p<64;++p) for (uint64_t b=0;b<PAGE;++b) fputc((int)p,out);
    fclose(out);
    CHECK(runtime_file_mount("/race",dir)==0);
    const int64_t fd=runtime_file_open("/race/pak.bin",0,0);
    CHECK(fd>=3);
    CHECK(direct_allocate(0,INT64_MAX,VIEW_PAGES*PAGE,0,3,&race_phys)==0);
    void *view=NULL;
    CHECK(direct_map(&view,VIEW_PAGES*PAGE,3,0,race_phys,0)==0);
    race_view=view;
    void *handler=AddVectoredExceptionHandler(1,race_unprotect);
    if (fd>=3 && race_view) {
        race_case("quiet",0,(int)fd,0,1);
        race_case("GPU page protection",RACE_PROTECT,(int)fd,0,1);
        race_case("GPU page protection, unaligned",RACE_PROTECT,(int)fd,1000,1);
        race_case("map split/merge",RACE_SPLIT,(int)fd,0,1);
        race_case("map split/merge, unaligned",RACE_SPLIT,(int)fd,1000,1);
        race_case("split/merge + protection",RACE_SPLIT|RACE_PROTECT,(int)fd,513,1);
        CHECK(direct_unmap(race_view,VIEW_PAGES*PAGE)==0);
    }
    RemoveVectoredExceptionHandler(handler);
    CHECK(direct_release(race_phys,VIEW_PAGES*PAGE)==0);
    if (fd>=3) runtime_file_close((int)fd);
    DeleteFileA(path);
    RemoveDirectoryA(dir);
}
static uint64_t system_commit(void) {
    PERFORMANCE_INFORMATION info;
    memset(&info,0,sizeof(info)); info.cb=sizeof(info);
    K32GetPerformanceInfo(&info,sizeof(info));
    return (uint64_t)info.CommitTotal*info.PageSize;
}
static DWORD state_at(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address,&info,sizeof(info)) ? info.State : 0;
}
static DWORD protect_at(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address,&info,sizeof(info)) ? info.Protect : 0;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    MEMORY_BASIC_INFORMATION info;
    fail_section_creation=1;
    CHECK(runtime_memory_prepare()!=0);
    CHECK(backing_base==NULL && section==NULL);
    fail_section_creation=0;
    fail_commit=1;
    CHECK(runtime_memory_prepare()!=0);
    fail_commit=0;
    CHECK(backing_base!=NULL);
    if (!backing_base) return 1;
    CHECK(VirtualQuery(backing_base,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(runtime_memory_prepare()==0);
    CHECK(VirtualQuery(backing_base+POOL_SIZE+FLEX_SPAN-PAGE,&info,sizeof(info)) && info.State==MEM_RESERVE);

    void *at=NULL;
    CHECK(map_flexible(&at,4*PAGE,3,0)==0);
    if (!at) return 1;
    unsigned char *code=at;
    code[0]=0xb8; code[1]=42; code[2]=0; code[3]=0; code[4]=0; code[5]=0xc3;
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READWRITE);
    CHECK(kernel_mprotect(at,4*PAGE,5)==0);
    CHECK(protect_at(at)==PAGE_EXECUTE_READ);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(state_at(backing_base+POOL_SIZE+4*PAGE)==MEM_RESERVE);

    const unsigned char tracked=23;
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(protect_at(at)==PAGE_NOACCESS);
    CHECK(runtime_memory_write_backing((uintptr_t)at+PAGE,&tracked,1));
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,0);
    CHECK(protect_at(at)==PAGE_READONLY);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(((unsigned char *)at)[PAGE]==tracked);
    runtime_memory_gpu_protect((uintptr_t)at+PAGE,PAGE,0,0);
    CHECK(protect_at(at)==PAGE_READWRITE && protect_at((char *)at+PAGE)==PAGE_NOACCESS && protect_at((char *)at+2*PAGE)==PAGE_READWRITE);
    runtime_memory_gpu_protect((uintptr_t)at+PAGE,PAGE,1,1);
    CHECK(protect_at((char *)at+PAGE)==PAGE_READWRITE);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_NOACCESS);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_READONLY);
    CHECK(kernel_mprotect(at,4*PAGE,5)==0);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(protect_at(at)==PAGE_EXECUTE);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_EXECUTE_READ);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    const unsigned char marker=17;
    CHECK(runtime_memory_write_backing((uintptr_t)at,&marker,1));
    CHECK(*(unsigned char *)at==marker);
    CHECK(direct_unmap(at,4*PAGE)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.State==MEM_RESERVE);
    at=NULL; CHECK(map_flexible(&at,4*PAGE,3,0)==0); CHECK(*(unsigned char *)at==0); CHECK(direct_unmap(at,4*PAGE)==0);

    void *executable=NULL;
    CHECK(map_flexible(&executable,PAGE,7,0)==0);
    if (executable) { *(unsigned char *)executable=0xc3; CHECK(VirtualQuery(executable,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READWRITE); CHECK(direct_unmap(executable,PAGE)==0); }
    const unsigned char ret=0xc3;
    CHECK(map_flexible(&executable,PAGE,5,0)==0);
    if (executable) {
        CHECK(protect_at(executable)==PAGE_EXECUTE_READ);
        CHECK(runtime_memory_write_backing((uintptr_t)executable,&ret,1));
        ((void (*)(void))executable)();
        CHECK(kernel_mprotect(executable,PAGE,7)==0 && protect_at(executable)==PAGE_EXECUTE_READWRITE);
        CHECK(kernel_mprotect(executable,PAGE,3)==0 && protect_at(executable)==PAGE_READWRITE);
        CHECK(direct_unmap(executable,PAGE)==0);
    }
    int64_t phys=-1;
    void *direct_code=NULL;
    CHECK(direct_allocate(0,INT64_MAX,PAGE,0,3,&phys)==0);
    CHECK(direct_map(&direct_code,PAGE,7,0,phys,0)==0);
    if (direct_code) {
        *(unsigned char *)direct_code=ret;
        CHECK(protect_at(direct_code)==PAGE_EXECUTE_READWRITE);
        ((void (*)(void))direct_code)();
        CHECK(kernel_mprotect(direct_code,PAGE,5)==0 && protect_at(direct_code)==PAGE_EXECUTE_READ);
        CHECK(kernel_mprotect(direct_code,PAGE,3)==0 && protect_at(direct_code)==PAGE_READWRITE);
        CHECK(direct_unmap(direct_code,PAGE)==0);
    }
    direct_code=NULL;
    CHECK(direct_map(&direct_code,PAGE,5,0,phys,0)==0);
    if (direct_code) {
        CHECK(protect_at(direct_code)==PAGE_EXECUTE_READ);
        ((void (*)(void))direct_code)();
        CHECK(kernel_mprotect(direct_code,PAGE,7)==0 && protect_at(direct_code)==PAGE_EXECUTE_READWRITE);
        CHECK(direct_unmap(direct_code,PAGE)==0);
    }
    CHECK(direct_release(phys,PAGE)==0);

    unsigned char *split=NULL;
    CHECK(map_flexible((void **)&split,8*PAGE,3,0)==0);
    if (split) {
        split[0]=0x11; split[7*PAGE]=0x77;
        CHECK(direct_unmap(split+4*PAGE,4*PAGE)==0);
        CHECK(split[0]==0x11);
        void *other=NULL; CHECK(map_flexible(&other,4*PAGE,3,0)==0);
        if (other) { memset(other,0x22,4*PAGE); CHECK(split[0]==0x11 && split[3*PAGE]==0); CHECK(direct_unmap(other,4*PAGE)==0); }
        CHECK(direct_unmap(split,2*PAGE)==0);
        const unsigned char patch=0x33;
        CHECK(runtime_memory_write_backing((uintptr_t)split+3*PAGE,&patch,1));
        CHECK(split[3*PAGE]==patch && split[2*PAGE]==0);
        CHECK(direct_unmap(split+2*PAGE,2*PAGE)==0);
        CHECK(flexible_bytes==0);
    }
    unsigned char *middle=NULL;
    CHECK(map_flexible((void **)&middle,8*PAGE,3,0)==0);
    if (middle) {
        void *hole=middle+3*PAGE;
        CHECK(map_flexible(&hole,2*PAGE,3,MAP_FIXED_FLAG)==0);
        CHECK(flexible_bytes==8*PAGE);
        CHECK(direct_unmap(middle,3*PAGE)==0 && direct_unmap(middle+5*PAGE,3*PAGE)==0);
        CHECK(direct_unmap(hole,2*PAGE)==0);
        CHECK(flexible_bytes==0);
    }

    const uint64_t charged=128u<<20;
    CHECK(direct_allocate(0,INT64_MAX,charged,0,3,&phys)==0);
    void *first=NULL,*second=NULL;
    const uint64_t commit_before=system_commit();
    CHECK(direct_map(&first,charged,3,0,phys,0)==0);
    const uint64_t commit_first=system_commit();
    CHECK(direct_map(&second,charged,3,0,phys,0)==0);
    const uint64_t commit_second=system_commit();
    CHECK((int64_t)(commit_first-commit_before)>=(int64_t)(charged*3/4));
    CHECK((int64_t)(commit_second-commit_first)<(int64_t)(charged/2));
    CHECK(direct_unmap(first,charged)==0); CHECK(direct_unmap(second,charged)==0); CHECK(direct_release(phys,charged)==0);

    CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    first=NULL; second=NULL;
    CHECK(direct_map(&first,8*PAGE,3,0,phys,0)==0); CHECK(direct_map(&second,8*PAGE,3,0,phys,0)==0);
    CHECK(first && second && first!=second);
    ((uint64_t *)first)[3]=UINT64_C(0x1122334455667788);
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    ((uint64_t *)second)[5]=UINT64_C(0x8877665544332211);
    CHECK(((uint64_t *)first)[5]==UINT64_C(0x8877665544332211));

    void *failed=NULL; fail_commit=1;
    CHECK(map_flexible(&failed,PAGE,3,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    void *replacement=first; fail_commit=1;
    CHECK(map_flexible(&replacement,PAGE,3,MAP_FIXED_FLAG)==NO_MEMORY);
    replacement=first;
    CHECK(direct_map(&replacement,PAGE,3,MAP_FIXED_FLAG,phys,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    void *start=NULL,*end=NULL; uint32_t prot=0;
    CHECK(query_protection(first,&start,&end,&prot)==0 && start==first && end>=(void *)((char *)first+8*PAGE) && prot==3);
    replacement=first;
    CHECK(direct_map(&replacement,PAGE,3,MAP_FIXED_FLAG,phys,0)==0 && replacement==first);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788) && ((uint64_t *)second)[5]==UINT64_C(0x8877665544332211));
    CHECK(map_flexible(&failed,PAGE,3,0)==0); CHECK(direct_unmap(failed,PAGE)==0);

    CHECK(kernel_mprotect(first,2*PAGE,1)==0);
    CHECK(direct_unmap((char *)first+2*PAGE,2*PAGE)==0);
    CHECK(VirtualQuery(first,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(direct_unmap(first,2*PAGE)==0); CHECK(direct_unmap((char *)first+4*PAGE,4*PAGE)==0);
    CHECK(direct_unmap(second,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    void *reused=NULL; CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    CHECK(direct_map(&reused,8*PAGE,3,0,phys,0)==0);
    CHECK(((uint64_t *)reused)[3]==0);
    CHECK(direct_unmap(reused,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);

    const int64_t untouched=INT64_C(2)<<30;
    CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    void *partial=NULL; CHECK(direct_map(&partial,2*PAGE,3,0,phys,0)==0);
    if (partial) *(unsigned char *)partial=0x77;
    CHECK(state_at(backing_base+phys+2*PAGE)==MEM_RESERVE);
    CHECK(direct_unmap(partial,2*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    CHECK(state_at(backing_base+phys+2*PAGE)==MEM_RESERVE);
    partial=NULL; CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    CHECK(direct_map(&partial,2*PAGE,3,0,phys,0)==0 && partial && *(unsigned char *)partial==0);
    CHECK(direct_unmap(partial,2*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);

    unsigned char *whole=NULL,*alias=NULL;
    CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    CHECK(direct_map((void **)&whole,8*PAGE,3,0,phys,0)==0 && direct_map((void **)&alias,8*PAGE,3,0,phys,0)==0);
    if (whole && alias) {
        whole[0]=0x10; whole[3*PAGE]=0x13; whole[7*PAGE]=0x17;
        CHECK(direct_release(phys+2*PAGE,3*PAGE)==0);
        CHECK(state_at(whole+2*PAGE)==MEM_RESERVE && state_at(alias+2*PAGE)==MEM_RESERVE);
        CHECK(state_at(whole)==MEM_COMMIT && state_at(whole+5*PAGE)==MEM_COMMIT);
        CHECK(whole[0]==0x10 && alias[0]==0x10 && whole[7*PAGE]==0x17 && alias[7*PAGE]==0x17);
        alias[PAGE]=0x21;
        CHECK(whole[PAGE]==0x21);
        CHECK(direct_release(phys,8*PAGE)==INVALID);
        void *refilled=NULL; int64_t again=-1;
        CHECK(direct_allocate(phys+2*PAGE,INT64_MAX,3*PAGE,0,3,&again)==0 && again==phys+(int64_t)(2*PAGE));
        CHECK(direct_map(&refilled,3*PAGE,3,0,again,0)==0);
        if (refilled) {
            CHECK(((unsigned char *)refilled)[PAGE]==0);
            CHECK(direct_unmap(refilled,3*PAGE)==0);
        }
        CHECK(direct_release(again,3*PAGE)==0);
        CHECK(whole[0]==0x10 && alias[7*PAGE]==0x17);
        CHECK(direct_unmap(whole,2*PAGE)==0 && direct_unmap(whole+5*PAGE,3*PAGE)==0);
        CHECK(direct_unmap(alias,2*PAGE)==0 && direct_unmap(alias+5*PAGE,3*PAGE)==0);
        CHECK(direct_release(phys,2*PAGE)==0 && direct_release(phys+5*PAGE,3*PAGE)==0);
    }

    int64_t high=(int64_t)(POOL_SIZE-65536); CHECK(direct_allocate(high,(int64_t)POOL_SIZE,65536,65536,3,&phys)==0);
    CHECK(direct_release(phys,65536)==0);
    CHECK(VirtualQuery(backing_base+high,&info,sizeof(info)) && info.State==MEM_RESERVE);

    CHECK(pool_expand(0,INT64_MAX,65536,65536,&phys)==0);
    void *reserved=NULL; CHECK(pool_reserve(NULL,65536,65536,0,&reserved)==0);
    CHECK(pool_commit(reserved,65536,3,3,0)==0); memset(reserved,0x5a,65536);
    CHECK(pool_decommit(reserved,65536,0)==0);
    CHECK(VirtualQuery(reserved,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(pool_commit(reserved,65536,3,3,0)==0);
    CHECK(pool_decommit(reserved,65536,0)==0);
    file_reads_race_memory_changes();
    printf("Windows memory: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
