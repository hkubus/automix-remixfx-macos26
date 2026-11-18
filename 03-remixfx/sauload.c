// sauload.c — in-process loader for the cache-extracted x86_64 SonicAudioUnits (AURemixFX).
// Maps TEXT/DATA_CONST/DATA at a fresh base, applies rebases + data binds + shared-GOT stub binds
// (resolved via dlsym against the running 26 process), then returns registerSonicAudioUnits().
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <mach/vm_page_size.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#define PAGE 0x1000UL
#define FLOOR(x) ((x) & ~(PAGE-1))
#define CEIL(x)  (((x)+PAGE-1) & ~(PAGE-1))

typedef struct { uint64_t vmaddr, vmsize, fileoff, filesize; uint32_t maxprot, initprot; char name[16]; } Seg;

static void* g_mem;        // reserved base B
static uint64_t g_imgbase; // preferred image base (0x7ffd28106000)
static long g_slide;

static void* filebuf; static size_t filelen;

static Seg segs[8]; static int nseg;
static uint64_t symoff, nsyms, stroff; // symtab
static uint64_t sect_unwind, sect_unwind_sz, sect_eh, sect_eh_sz;   // unwind sections (cache VAs)

static int parse_macho(void){
    struct mach_header_64* mh = (struct mach_header_64*)filebuf;
    if (mh->magic != MH_MAGIC_64){ fprintf(stderr,"bad magic\n"); return -1; }
    uint8_t* p = (uint8_t*)filebuf + sizeof(*mh);
    for (uint32_t i=0;i<mh->ncmds;i++){
        struct load_command* lc=(struct load_command*)p;
        if (lc->cmd==LC_SEGMENT_64){
            struct segment_command_64* s=(struct segment_command_64*)p;
            Seg* sg=&segs[nseg++];
            sg->vmaddr=s->vmaddr; sg->vmsize=s->vmsize; sg->fileoff=s->fileoff; sg->filesize=s->filesize;
            sg->maxprot=s->maxprot; sg->initprot=s->initprot; memcpy(sg->name,s->segname,16);
            struct section_64* sc=(struct section_64*)(s+1);
            for (uint32_t k=0;k<s->nsects;k++){
                if (!strncmp(sc[k].sectname,"__unwind_info",16)){ sect_unwind=sc[k].addr; sect_unwind_sz=sc[k].size; }
                else if (!strncmp(sc[k].sectname,"__eh_frame",16)){ sect_eh=sc[k].addr; sect_eh_sz=sc[k].size; }
            }
        } else if (lc->cmd==LC_SYMTAB){
            struct symtab_command* st=(struct symtab_command*)p;
            symoff=st->symoff; nsyms=st->nsyms; stroff=st->stroff;
        }
        p += lc->cmdsize;
    }
    g_imgbase = segs[0].vmaddr;
    return 0;
}

static int prot_of(uint32_t vmprot){
    int pr=0; if(vmprot&1)pr|=PROT_READ; if(vmprot&2)pr|=PROT_WRITE; if(vmprot&4)pr|=PROT_EXEC; return pr;
}

// map a page-aligned anon RW window covering [dst, dst+len); returns 0 ok
static int ensure_window(uint64_t dst, uint64_t len){
    uint64_t a=FLOOR(dst), b=CEIL(dst+len);
    void* r=mmap((void*)a,(size_t)(b-a),PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
    if(r==MAP_FAILED){ fprintf(stderr,"mmap window %#llx failed: %s\n",(unsigned long long)a,strerror(errno)); return -1; }
    return 0;
}

// Optional bind hook: lets the host substitute its own implementation for an import of the
// transplanted image (e.g. wrap AudioComponentRegister to interpose the AU factory).
static void* (*g_bind_hook)(const char* name, void* resolved);
void sau_set_bind_hook(void* (*hook)(const char* name, void* resolved)){ g_bind_hook = hook; }

static void* dlsym_strip_raw(const char* asmname);
static void* dlsym_strip(const char* asmname){
    void* a = dlsym_strip_raw(asmname);
    if (g_bind_hook) { void* h = g_bind_hook(asmname[0]=='_' ? asmname+1 : asmname, a); if (h) a = h; }
    return a;
}
static void* dlsym_strip_raw(const char* asmname){
    char buf[256];
    const char* n = (asmname[0]=='_') ? asmname+1 : asmname;   // strip one leading _
    void* a = dlsym(RTLD_DEFAULT, n);
    if(a) return a;
    // try truncated at '$' (variant suffix, e.g. atan$fenv_access_off)
    strncpy(buf,n,sizeof buf-1); buf[sizeof buf-1]=0;
    char* d=strchr(buf,'$'); if(d){ *d=0; a=dlsym(RTLD_DEFAULT,buf); if(a) return a; }
    // try stripping a 'platform_' prefix (e.g. _platform_memmove -> memmove)
    const char* p=strstr(n,"platform_"); if(p){ a=dlsym(RTLD_DEFAULT,p+9); if(a) return a; }
    return NULL;
}

#include <execinfo.h>
__attribute__((noinline))
static void sau_unresolved(void){
    fprintf(stderr,"\n*** sau_unresolved stub CALLED — an import was not bound ***\n");
    void* bt[16]; int n=backtrace(bt,16); backtrace_symbols_fd(bt,n,2);
    abort();
}

// anon-map one page of the cache's shared-GOT region (lies inside our reserved span, outside any segment)
static uint64_t g_gap[512]; static int g_ngap;
static void map_gap_page(uint64_t pg){
    for(int k=0;k<g_ngap;k++) if(g_gap[k]==pg) return;
    if(mmap((void*)pg,PAGE,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0)==MAP_FAILED)
        fprintf(stderr,"[sau] gotpage %#llx fail\n",(unsigned long long)pg);
    else if(g_ngap<512) g_gap[g_ngap++]=pg;
}
static int in_segment(uint64_t off){
    uint64_t va=g_imgbase+off;
    for(int k=0;k<nseg;k++){ if(strcmp(segs[k].name,"__LINKEDIT")==0) continue;
        if(va>=segs[k].vmaddr && va<segs[k].vmaddr+segs[k].vmsize) return 1; }
    return 0;
}

// ---- C++ exception support -------------------------------------------------------------------------------
// The image is only mapped by us, so libunwind can't find its unwind info and every exception thrown inside it
// (ausdk throws AUException for routine errors and catches it in its own dispatch) would terminate the process.
// 1) bind the EH-only imports (personality + catch type info) listed in <fixups>.ehbinds -> "SonicAudioUnits.ehbinds"
// 2) register __unwind_info / __eh_frame with libunwind's dynamic-unwind-sections hook.
struct unw_dynamic_unwind_sections { uintptr_t dso_base, dwarf_section; size_t dwarf_section_length;
                                     uintptr_t compact_unwind_section; size_t compact_unwind_section_length; };
static uintptr_t g_text_lo, g_text_hi;
static int find_sau_sections(uintptr_t addr, struct unw_dynamic_unwind_sections* info){
    if(addr<g_text_lo || addr>=g_text_hi) return 0;
    info->dso_base=(uintptr_t)g_mem;
    info->dwarf_section = sect_eh ? (uintptr_t)g_mem+(sect_eh-g_imgbase) : 0; info->dwarf_section_length=sect_eh_sz;
    info->compact_unwind_section = sect_unwind ? (uintptr_t)g_mem+(sect_unwind-g_imgbase) : 0; info->compact_unwind_section_length=sect_unwind_sz;
    return 1;
}
static void apply_eh_binds(const char* fixups){
    char path[1024]; snprintf(path,sizeof path,"%s",fixups);
    char* dot=strrchr(path,'.'); if(dot && !strcmp(dot,".fixups")) strcpy(dot,".ehbinds"); else strncat(path,".ehbinds",sizeof path-strlen(path)-1);
    FILE* f=fopen(path,"r"); if(!f){ fprintf(stderr,"[sau] no %s: C++ exceptions inside the image will abort\n",path); return; }
    char line[512]; int n=0, miss=0;
    while(fgets(line,sizeof line,f)){
        if(line[0]=='#'||line[0]=='\n') continue;
        unsigned long long off; char sym[256];
        if(sscanf(line,"%llx %255s",&off,sym)!=2) continue;
        if(!in_segment(off)) map_gap_page(FLOOR((uint64_t)g_mem+off));
        void* a=dlsym(RTLD_DEFAULT,sym);
        if(!a){ fprintf(stderr,"[sau] eh bind MISS %s\n",sym); miss++; continue; }
        *(uint64_t*)((uint64_t)g_mem+off)=(uint64_t)a; n++;
    }
    fclose(f);
    fprintf(stderr,"[sau] applied %d EH binds (%d miss)\n",n,miss);
}
static void register_unwind(void){
    static int done; if(done) return;
    int (*add)(int (*)(uintptr_t, struct unw_dynamic_unwind_sections*)) =
        (int (*)(int (*)(uintptr_t, struct unw_dynamic_unwind_sections*)))dlsym(RTLD_DEFAULT,"__unw_add_find_dynamic_unwind_sections");
    if(!add){ fprintf(stderr,"[sau] libunwind has no __unw_add_find_dynamic_unwind_sections\n"); return; }
    for(int k=0;k<nseg;k++) if(!strcmp(segs[k].name,"__TEXT")){
        g_text_lo=(uintptr_t)g_mem+(segs[k].vmaddr-g_imgbase); g_text_hi=g_text_lo+segs[k].vmsize; }
    int r=add(find_sau_sections);
    done = (r==0);
    fprintf(stderr,"[sau] unwind sections registered (unwind_info %#llx bytes, eh_frame %#llx bytes) -> %d\n",
            (unsigned long long)sect_unwind_sz,(unsigned long long)sect_eh_sz,r);
}

// fixup sidecar
static uint8_t* fx; static size_t fxlen; static size_t fxp;
static uint32_t fx_u32(void){ uint32_t v; memcpy(&v,fx+fxp,4); fxp+=4; return v; }
static int32_t  fx_i32(void){ int32_t v; memcpy(&v,fx+fxp,4); fxp+=4; return v; }
static int64_t  fx_i64(void){ int64_t v; memcpy(&v,fx+fxp,8); fxp+=8; return v; }
static uint64_t fx_u64(void){ uint64_t v; memcpy(&v,fx+fxp,8); fxp+=8; return v; }
static const char* fx_str(void){ uint16_t l; memcpy(&l,fx+fxp,2); fxp+=2; static char buf[256]; memcpy(buf,fx+fxp,l); buf[l]=0; fxp+=l; return buf; }

void* sau_load(const char* dylib, const char* fixups){
    // read files
    FILE* f=fopen(dylib,"rb"); if(!f){perror("open dylib");return NULL;}
    fseek(f,0,SEEK_END); filelen=ftell(f); fseek(f,0,SEEK_SET);
    filebuf=malloc(filelen); fread(filebuf,1,filelen,f); fclose(f);
    f=fopen(fixups,"rb"); if(!f){perror("open fixups");return NULL;}
    fseek(f,0,SEEK_END); fxlen=ftell(f); fseek(f,0,SEEK_SET);
    fx=malloc(fxlen); fread(fx,1,fxlen,f); fclose(f);
    if(memcmp(fx,"SAUFIX01",8)){fprintf(stderr,"bad fixups magic\n");return NULL;}
    fxp=8; uint64_t sideImgBase=fx_u64();

    if(parse_macho()) return NULL;
    if(g_imgbase!=sideImgBase){fprintf(stderr,"imgbase mismatch %#llx vs %#llx\n",(unsigned long long)g_imgbase,(unsigned long long)sideImgBase);return NULL;}

    // compute span
    uint64_t lo=~0ULL, hi=0;
    for(int i=0;i<nseg;i++){ if(strcmp(segs[i].name,"__LINKEDIT")==0) continue;
        if(segs[i].vmaddr<lo)lo=segs[i].vmaddr; uint64_t e=segs[i].vmaddr+segs[i].vmsize; if(e>hi)hi=e; }
    // also account for shared-GOT stub slots (may sit below DATA_CONST, above TEXT) — within [lo,hi] already since lo=TEXT, hi=DATA end
    uint64_t span=CEIL(hi-lo)+PAGE;
    void* B=mmap(NULL,(size_t)span,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(B==MAP_FAILED){perror("reserve span");return NULL;}
    g_mem=B; g_slide=(long)((uint64_t)B - g_imgbase);
    fprintf(stderr,"[sau] span=%#llx base=%p slide=%#lx\n",(unsigned long long)span,B,g_slide);

    // map real segments
    for(int i=0;i<nseg;i++){ Seg* s=&segs[i]; if(strcmp(s->name,"__LINKEDIT")==0) continue;
        uint64_t dst=(uint64_t)B+(s->vmaddr-g_imgbase);
        if(ensure_window(dst,s->vmsize)) return NULL;
        memcpy((void*)dst,(uint8_t*)filebuf+s->fileoff,(size_t)s->filesize);
        fprintf(stderr,"[sau] seg %-12s -> %#llx (vmsize %#llx)\n",s->name,(unsigned long long)dst,(unsigned long long)s->vmsize);
    }

    // rebases
    uint32_t nreb=fx_u32();
    for(uint32_t i=0;i<nreb;i++){ uint32_t off=fx_u32(); uint64_t* slot=(uint64_t*)((uint64_t)B+off); *slot += g_slide; }
    fprintf(stderr,"[sau] applied %u rebases\n",nreb);

    // data binds
    uint32_t ndb=fx_u32(); int dbfail=0;
    for(uint32_t i=0;i<ndb;i++){ int32_t off=fx_i32(); int64_t ad=fx_i64(); const char* nm=fx_str();
        void* a=dlsym_strip(nm); if(!a){ fprintf(stderr,"[sau] data bind MISS %s\n",nm); dbfail++; continue; }
        *(uint64_t*)((uint64_t)B+off)=(uint64_t)a+ad; }
    fprintf(stderr,"[sau] applied %u data binds (%d miss)\n",ndb,dbfail);

    // stub binds — slots are either in the image's own __got (inside a mapped segment)
    // or in the cache's shared GOT (an inter-segment gap page we must anon-map first).
    uint32_t nsb=fx_u32(); int sbfail=0;
    for(uint32_t i=0;i<nsb;i++){ uint32_t off=fx_u32(); const char* nm=fx_str();
        uint64_t dst=(uint64_t)B+off; uint64_t slotva=g_imgbase+off;
        int inseg=0;
        for(int k=0;k<nseg;k++){ if(strcmp(segs[k].name,"__LINKEDIT")==0) continue;
            if(slotva>=segs[k].vmaddr && slotva<segs[k].vmaddr+segs[k].vmsize){ inseg=1; break; } }
        if(!inseg) map_gap_page(FLOOR(dst));   // shared-GOT gap page: anon-map once
        void* a = strcmp(nm,"__SAU_UNRESOLVED__") ? dlsym_strip(nm) : NULL;
        if(!a){ if(strcmp(nm,"__SAU_UNRESOLVED__")) fprintf(stderr,"[sau] stub bind MISS %s -> diag stub\n",nm); a=(void*)sau_unresolved; sbfail++; }
        *(uint64_t*)dst=(uint64_t)a; }
    fprintf(stderr,"[sau] applied %u stub binds (%d miss)\n",nsb,sbfail);
    apply_eh_binds(fixups);

    // set final protections for segments
    for(int i=0;i<nseg;i++){ Seg* s=&segs[i]; if(strcmp(s->name,"__LINKEDIT")==0) continue;
        uint64_t dst=(uint64_t)B+(s->vmaddr-g_imgbase); uint64_t a=FLOOR(dst),b=CEIL(dst+s->vmsize);
        int pr=prot_of(s->initprot); if(pr==0)pr=PROT_READ;
        if(mprotect((void*)a,(size_t)(b-a),pr)) fprintf(stderr,"[sau] mprotect %s fail\n",s->name); }

    // find registerSonicAudioUnits in symtab
    struct nlist_64* syms=(struct nlist_64*)((uint8_t*)filebuf+symoff);
    char* strs=(char*)filebuf+stroff; uint64_t regva=0;
    for(uint64_t i=0;i<nsyms;i++){ const char* nm=strs+syms[i].n_un.n_strx;
        if(strcmp(nm,"_registerSonicAudioUnits")==0){ regva=syms[i].n_value; break; } }
    if(!regva){ fprintf(stderr,"[sau] registerSonicAudioUnits not in symtab\n"); return NULL; }
    register_unwind();
    void* reg=(void*)((uint64_t)B+(regva-g_imgbase));
    fprintf(stderr,"[sau] registerSonicAudioUnits @ %p\n",reg);
    return reg;
}
