// gnyx_daemon.c — 安卓高能英雄 ESP 数据层 daemon（root，独立进程，免注入）
// 平移自 iOS esp_data.dylib v19/v20：pawn表/骨骼/POV/矩阵+本地pawn标尺验证 全量逻辑
// 读写后端：/proc/<pid>/mem（默认）— 内核后端 kp/ko 接口预留
// 快照：/data/local/tmp/esp_state.json（悬浮窗 app 消费）
// UE4 对象内偏移与 iOS 实测同源复用（LP64 ABI 一致），模块偏移需重新扫描
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/uio.h>
#include <stdarg.h>
#include <sys/stat.h>

// ---- 偏移档案（iOS 实测同源，安卓待验证）----
#define OFF_PERSISTENTLEVEL 0x30
#define OFF_GAMESTATE       0x360
#define OFF_GAMEINSTANCE    0x3b0
#define OFF_ACTORS          0x98
#define OFF_ACTORS_NUM      0xA0
#define OFF_UOBJ_CLASS      0x10
#define OFF_UOBJ_NAME       0x18
#define OFF_UOBJ_OUTER      0x20
#define OFF_PAWN_PS         0x2c0
#define OFF_PAWN_PC         0x2d8
#define OFF_PC_LOCALPAWN    0x2d0
#define OFF_PC_CMGR         0x388
#define OFF_PAWN_CAPSULE    0x310
#define OFF_PAWN_SKEL       0x300
#define OFF_CAP_C2W         0x200
#define OFF_SKEL_BONES      0x600   // 组件空间 24 骨双缓冲之一
#define OFF_PAWN_PS2        0x2ac   // 血量
#define OFF_CMGR_POV_LOC    0x1ef0
#define OFF_CMGR_POV_YAW    0x1f08
#define OFF_CMGR_POV_PITCH  0x1f0c
#define OFF_CMGR_POV_ROLL   0x1f10
#define OFF_CMGR_POV_FOV    0x1f14

// ---- 读写后端 ----
static int g_memfd=-1;
static pid_t g_pid=0;
// 预留：内核后端句柄（kp=kpatch kerneeld / ko=自研 .ko /dev 节点）
static int g_backend=0; // 0=proc_mem 1=kp 2=ko
static int kp_open(pid_t p){ (void)p; return -1; }   // TODO: APatch kernel API
static int ko_open(pid_t p){ (void)p; return -1; }   // TODO: ioctl 驱动
static int rw_open(pid_t p){
    g_pid=p;
    if(g_backend==1) return kp_open(p);
    if(g_backend==2) return ko_open(p);
    char pth[64]; snprintf(pth,sizeof(pth),"/proc/%d/mem",p);
    g_memfd=open(pth,O_RDONLY);
    return g_memfd;
}
// safe read：任何失败返回 0，绝不崩溃
static int safe_rd(uintptr_t addr,void* out,size_t n){
    if(g_backend==0){
        ssize_t r=pread(g_memfd,out,n,(int64_t)addr);
        return r==(ssize_t)n;
    }
    // kp/ko 后端 TODO
    return 0;
}
static uint64_t rd64(uintptr_t a){ uint64_t v=0; safe_rd(a,&v,8); return v; }
static uint32_t rd32(uintptr_t a){ uint32_t v=0; safe_rd(a,&v,4); return v; }
static float rdf(uintptr_t a){ float v=0; safe_rd(a,&v,4); return v; }
static int inheap(uint64_t p){
    return p>0x10000000ULL && p<0x8000000000ULL && (p&7)==0;
}

// ---- 模块枚举：/proc/<pid>/maps 找主二进制 + 大模块 ----
#define MAXMOD 16
typedef struct { char name[64]; uintptr_t base; uintptr_t end; } Mod;
static Mod g_mods[MAXMOD]; static int g_nmod=0;
static void enum_modules(void){
    char pth[64]; snprintf(pth,sizeof(pth),"/proc/%d/maps",g_pid);
    FILE* f=fopen(pth,"r"); if(!f) return;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        uintptr_t s,e; char perms[8],path[256]; path[0]=0;
        if(sscanf(line,"%lx-%lx %7s %*s %*s %*s %255[^\n]",&s,&e,perms,path)<3) continue;
        if(!strstr(path,".so") && !strstr(path,".apk") && !strstr(path,"AClient")) continue;
        // 同名合并取最小基址
        int found=0;
        for(int i=0;i<g_nmod;i++) if(!strcmp(g_mods[i].name,path)){
            if(s<g_mods[i].base) g_mods[i].base=s;
            if(e>g_mods[i].end) g_mods[i].end=e;
            found=1; break;
        }
        if(!found&&g_nmod<MAXMOD){
            snprintf(g_mods[g_nmod].name,64,"%s",path);
            g_mods[g_nmod].base=s; g_mods[g_nmod].end=e; g_nmod++;
        }
    }
    fclose(f);
}

// ---- 日志 ----
static FILE* g_log=NULL;
static void logf_(const char* fmt,...){
    if(!g_log) return;
    va_list ap; va_start(ap,fmt);
    time_t t=time(NULL); struct tm* tm=localtime(&t);
    fprintf(g_log,"[%02d:%02d:%02d] ",tm->tm_hour,tm->tm_min,tm->tm_sec);
    vfprintf(g_log,fmt,ap); fprintf(g_log,"\n"); fflush(g_log);
    va_end(ap);
}

// ---- GWorld 特征扫描（免 GNames：结构互指验证）----
static uint64_t g_gw=0;
static int is_uworld_like(uint64_t w){
    if(!inheap(w)) return 0;
    uint64_t pl=rd64(w+OFF_PERSISTENTLEVEL);
    uint64_t gs=rd64(w+OFF_GAMESTATE);
    uint64_t gi=rd64(w+OFF_GAMEINSTANCE);
    if(!inheap(pl)||!inheap(gs)||!inheap(gi)) return 0;
    // PersistentLevel.Actors 数组非空（TArray@0x98 Num@0xA0）
    uint64_t arr=rd64(pl+OFF_ACTORS);
    int num=(int)rd32(pl+OFF_ACTORS_NUM);
    if(!inheap(arr)||num<1||num>20000) return 0;
    return 1;
}
static void scan_gworld(void){
    for(int mi=0;mi<g_nmod&&!g_gw;mi++){
        uintptr_t b=g_mods[mi].base,e=g_mods[mi].end;
        if(e-b>0x20000000) e=b+0x20000000;  // 单模块上限 512MB
        for(uintptr_t a=b+0x1000;a+8<e;a+=8){
            uint64_t v=rd64(a);
            if(!inheap(v)) continue;
            if(is_uworld_like(v)){ g_gw=v;
                logf_("[gw] GWorld @ %s+0x%lx = 0x%lx",g_mods[mi].name,(unsigned long)(a-b),(unsigned long)v);
                return;
            }
            // 快进：跳过明显非候选区（每 4096 块批量预读优化留给后续版本）
        }
    }
}

// ---- GNames 扫描（可选增强：验证 FNamePool 块结构 + "None" 首串）----
static uint64_t g_gnpool=0;       // FNamePool.Blocks 数组地址
static int g_npblock=0;
static uint64_t g_blkptr[8];      // 已见块指针
static int g_blk_used=0;
static uint64_t g_last_refresh=0;
static int fname_read(uint32_t fidx,char* out,int cap){
    if(!g_gnpool) return 0;
    uint32_t blk=fidx>>16, off=(fidx&0xFFFF)*2;
    if(blk>=0x4000) return 0;
    uint64_t bptr=rd64(g_gnpool+blk*8);
    if(!inheap(bptr)) return 0;
    uint16_t hdr=0; if(!safe_rd(bptr+off,&hdr,2)) return 0;
    int len=hdr>>6;
    if(len<=0||len>128) return 0;
    if(!safe_rd(bptr+off+2,out,len)) return 0;
    out[len]=0;
    return 1;
}
static void scan_gnames(void){
    if(g_gnpool) return;
    for(int mi=0;mi<g_nmod;mi++){
        uintptr_t b=g_mods[mi].base,e=g_mods[mi].end;
        if(e-b>0x20000000) e=b+0x20000000;
        for(uintptr_t a=b+0x1000;a+16<e;a+=8){
            uint64_t p0=rd64(a);
            if(!inheap(p0)) continue;
            int32_t cur=0;
            if(!safe_rd(a+8,&cur,4)) continue;
            if(cur<1||cur>0x2000) continue;
            // 读块0开头：应为 "None"（hdr=0x28, len=4）
            uint16_t hdr=0;
            if(!safe_rd(p0,&hdr,2)) continue;
            if(hdr>>6!=4) continue;
            char s[8]={0};
            if(!safe_rd(p0+2,s,4)) continue;
            if(!strcmp(s,"None")){
                g_gnpool=a; g_npblock=cur;
                logf_("[gn] FNamePool.Blocks @ 0x%lx blocks=%d",(unsigned long)a,cur);
                return;
            }
        }
    }
}

// ---- 类名（GNames 就绪时可用）----
static int cls_name(uint64_t obj,char* out,int cap){
    uint64_t cls=rd64(obj+OFF_UOBJ_CLASS);
    if(!inheap(cls)) return 0;
    uint32_t nidx=rd32(cls+OFF_UOBJ_NAME);
    return fname_read(nidx,out,cap);
}

// ---- pawn 识别（iOS 终极法：查 CharacterMovement 组件特征）----
// 无 GNames 时用结构启发式：胶囊组件存在且 C2W 合理
static int is_pawn_like(uint64_t a){
    uint64_t cap=rd64(a+OFF_PAWN_CAPSULE);
    if(!inheap(cap)) return 0;
    float x=rdf(cap+OFF_CAP_C2W+4),y=rdf(cap+OFF_CAP_C2W+8),z=rdf(cap+OFF_CAP_C2W+12);
    if(!isfinite(x)||!isfinite(y)||!isfinite(z)) return 0;
    if(fabsf(x)>5e5f||fabsf(y)>5e5f||fabsf(z)>5e5f) return 0;
    if(x==0&&y==0&&z==0) return 0;
    return 1;
}

// ---- ESP 采集状态 ----
#define MAXP 24
#define RBONES 24
typedef struct {
    uint64_t addr; float pos[3]; float hp; int local; int psi;
    float bones[RBONES*3]; int nb;
} Pawn;
static Pawn g_pawns[MAXP]; static int g_npawn=0;
static uint64_t g_pc=0, g_cmgr=0;

static void refresh_pc(void){
    // GWorld→PersistentLevel→Actors 遍历找 PlayerController（结构特征：有 CameraManager 指针+本地pawn指针）
    if(!g_gw) return;
    uint64_t pl=rd64(g_gw+OFF_PERSISTENTLEVEL);
    uint64_t arr=rd64(pl+OFF_ACTORS);
    int num=(int)rd32(pl+OFF_ACTORS_NUM);
    if(!inheap(arr)||num<1) return;
    int win=num>600?600:num;
    for(int i=0;i<win;i++){
        uint64_t a=rd64(arr+i*8);
        if(!inheap(a)) continue;
        uint64_t cm=rd64(a+OFF_PC_CMGR);
        uint64_t lp=rd64(a+OFF_PC_LOCALPAWN);
        if(inheap(cm)&&inheap(lp)){
            // 验证 cmgr 指向对象含合理 POV
            float fov=rdf(cm+OFF_CMGR_POV_FOV);
            if(fov>40&&fov<150){ g_pc=a; g_cmgr=cm;
                logf_("[pc] PC=0x%lx CMgr=0x%lx fov=%.0f",(unsigned long)a,(unsigned long)cm,fov);
                return;
            }
        }
    }
}

// ---- 矩阵锁定（v20 多区域轮转+标尺）----
static int g_mv_off=-1,g_mp_off=-1;
static uint64_t g_mv_base=0,g_mp_base=0;
static float g_last_mv[16],g_last_mp[16];
static int g_mv_hits=0,g_mp_hits=0;
static uint64_t g_mv_lastb=0,g_mp_lastb=0; static int g_mv_lasto=-1,g_mp_lasto=-1;
static int g_badn=0; static struct{ uint64_t b; int o; } g_bad[16];
static int is_bad(uint64_t b,int o){ for(int i=0;i<g_badn;i++) if(g_bad[i].b==b&&g_bad[i].o==o) return 1; return 0; }
static void add_bad(uint64_t b,int o){ if(g_badn<16){g_bad[g_badn].b=b;g_bad[g_badn].o=o;g_badn++;} else {g_badn=0;g_bad[g_badn].b=b;g_bad[g_badn].o=o;g_badn++;} }
static int mat_is_view(const float* f){
    if(fabsf(f[3])>1e-2f||fabsf(f[7])>1e-2f||fabsf(f[11])>1e-2f) return 0;
    if(fabsf(f[15]-1.f)>1e-2f) return 0;
    for(int r=0;r<3;r++){ int b=r*4;
        float n=sqrtf(f[b]*f[b]+f[b+1]*f[b+1]+f[b+2]*f[b+2]);
        if(n<0.9f||n>1.1f) return 0; }
    for(int r=0;r<3;r++) for(int c=r+1;c<3;c++){
        float d=f[r*4]*f[c*4]+f[r*4+1]*f[c*4+1]+f[r*4+2]*f[c*4+2];
        if(fabsf(d)>0.06f) return 0; }
    if(!isfinite(f[12])||!isfinite(f[13])||!isfinite(f[14])) return 0;
    if(fabsf(f[12])+fabsf(f[13])+fabsf(f[14])>5e6f) return 0;
    return 1;
}
static int mat_is_proj(const float* f){
    if(fabsf(f[1])>1e-3f||fabsf(f[2])>1e-3f||fabsf(f[3])>1e-3f) return 0;
    if(fabsf(f[4])>1e-3f||fabsf(f[6])>1e-3f||fabsf(f[7])>1e-3f) return 0;
    if(fabsf(f[8])>1e-3f||fabsf(f[9])>1e-3f) return 0;
    if(fabsf(f[12])>1e-3f||fabsf(f[13])>1e-3f||fabsf(f[15])>1e-3f) return 0;
    if(fabsf(fabsf(f[11])-1.f)>1e-2f) return 0;
    if(fabsf(f[14])<1.f||fabsf(f[14])>1e5f) return 0;
    if(f[0]<0.05f||f[0]>10.f) return 0;
    if(fabsf(f[5])<0.05f||fabsf(f[5])>10.f) return 0;
    if(fabsf(f[10])<0.5f) return 0;
    return 1;
}
static float g_lp[3]; static int g_lp_have=0;
static int verify_local(float wx,float wy,float wz,const float* mv,const float* mp){
    float ex=wx*mv[0]+wy*mv[4]+wz*mv[8]+mv[12];
    float ey=wx*mv[1]+wy*mv[5]+wz*mv[9]+mv[13];
    float ez=wx*mv[2]+wy*mv[6]+wz*mv[10]+mv[14];
    float cx=ex*mp[0]+ey*mp[4]+ez*mp[8]+mp[12];
    float cy=ex*mp[1]+ey*mp[5]+ez*mp[9]+mp[13];
    float cw=ex*mp[3]+ey*mp[7]+ez*mp[11]+mp[15];
    if(cw<10.f||cw>100000.f) return 0;
    float nx=cx/cw,ny=cy/cw;
    if(isnan(nx)||isnan(ny)) return 0;
    if(nx<-0.35f||nx>0.35f) return 0;
    int in1=(ny>=-0.9f&&ny<=0.3f);
    int in2=(ny>=-0.3f&&ny<=0.9f);
    return in1||in2;
}
static void scan_region(uint64_t base){
    if(!inheap(base)) return;
    for(int o=0;o<=0x20000;o+=16){
        if(is_bad(base,o)) continue;
        float f[16];
        if(!safe_rd(base+o,f,64)) continue;
        if(g_mv_off<0&&mat_is_view(f)){
            if(o!=g_mv_lasto||base!=g_mv_lastb){ g_mv_hits=0; g_mv_lasto=o; g_mv_lastb=base; }
            if(++g_mv_hits>=3){ g_mv_off=o; g_mv_base=base; memcpy(g_last_mv,f,64);
                logf_("[mat] VIEW @0x%lx+0x%x",(unsigned long)base,o); }
        }
        if(g_mp_off<0&&mat_is_proj(f)){
            if(o!=g_mp_lasto||base!=g_mp_lastb){ g_mp_hits=0; g_mp_lasto=o; g_mp_lastb=base; }
            if(++g_mp_hits>=3){ g_mp_off=o; g_mp_base=base; memcpy(g_last_mp,f,64);
                logf_("[mat] PROJ @0x%lx+0x%x xs=%.3f ys=%.3f",(unsigned long)base,o,f[0],f[5]); }
        }
        if(g_mv_off>=0&&g_mp_off>=0) return;
    }
}
static void mat_tick(void){
    float f[16];
    if(g_mv_off>=0){
        int ok=inheap(g_mv_base)&&safe_rd(g_mv_base+g_mv_off,f,64)&&mat_is_view(f);
        if(ok) memcpy(g_last_mv,f,64);
        else { add_bad(g_mv_base,g_mv_off); g_mv_off=-1; g_mv_hits=0; }
    }
    if(g_mp_off>=0){
        int ok=inheap(g_mp_base)&&safe_rd(g_mp_base+g_mp_off,f,64)&&mat_is_proj(f);
        if(ok) memcpy(g_last_mp,f,64);
        else { add_bad(g_mp_base,g_mp_off); g_mp_off=-1; g_mp_hits=0; }
    }
    static int vfail=0;
    if(g_mv_off>=0&&g_mp_off>=0&&g_lp_have){
        if(verify_local(g_lp[0],g_lp[1],g_lp[2],g_last_mv,g_last_mp)) vfail=0;
        else if(++vfail>=4){
            add_bad(g_mv_base,g_mv_off); add_bad(g_mp_base,g_mp_off);
            logf_("[mat] local-proj fail x4, blacklist & rescan");
            g_mv_off=-1; g_mp_off=-1; g_mv_hits=0; g_mp_hits=0; vfail=0;
        }
    } else vfail=0;
    if(g_mv_off>=0&&g_mp_off>=0) return;
    static int rota=0;
    uint64_t bases[4]; int nb=0;
    if(inheap(g_cmgr)) bases[nb++]=g_cmgr;
    uint64_t lp=rd64(g_pc+0x368); if(inheap(lp)) bases[nb++]=lp;
    if(inheap(g_pc)) bases[nb++]=g_pc;
    if(inheap(g_gw)) bases[nb++]=g_gw;
    if(nb==0) return;
    if(rota>=nb) rota=0;
    scan_region(bases[rota]);
    rota++;
}

// ---- 主采集循环：pawn 表 + 骨骼 + POV + 快照 ----
static uint64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static void dump_pawns(void){
    g_npawn=0;
    if(!g_pc) return;
    uint64_t lpm=rd64(g_pc+OFF_PC_LOCALPAWN);
    uint64_t pl=rd64(g_gw+OFF_PERSISTENTLEVEL);
    uint64_t arr=rd64(pl+OFF_ACTORS);
    int num=(int)rd32(pl+OFF_ACTORS_NUM);
    if(!inheap(arr)||num<1) return;
    int win=num>600?600:num;
    for(int i=0;i<win&&g_npawn<MAXP;i++){
        uint64_t a=rd64(arr+i*8);
        if(!inheap(a)) continue;
        if(!is_pawn_like(a)) continue;
        uint64_t cap=rd64(a+OFF_PAWN_CAPSULE);
        Pawn* p=&g_pawns[g_npawn];
        p->addr=a;
        p->pos[0]=rdf(cap+OFF_CAP_C2W+4);
        p->pos[1]=rdf(cap+OFF_CAP_C2W+8);
        p->pos[2]=rdf(cap+OFF_CAP_C2W+12);
        p->hp=rdf(a+OFF_PAWN_PS2);
        p->local=(a==lpm);
        // 骨骼（iOS：+0x300 组件 +0x600 组件空间 24 骨，48B FTransform，T@+16）
        uint64_t sk=rd64(a+OFF_PAWN_SKEL);
        p->nb=0;
        if(inheap(sk)){
            int32_t n=0; safe_rd(sk+OFF_SKEL_BONES,&n,4);
            if(n>=RBONES&&n<200){
                float* bt=(float*)malloc(RBONES*3*4);
                for(int b=0;b<RBONES;b++){
                    if(safe_rd(sk+OFF_SKEL_BONES+8+b*48+16,bt+b*3,12)) p->nb++;
                }
                memcpy(p->bones,bt,RBONES*3*4);
                free(bt);
            }
        }
        g_npawn++;
    }
    // 本地 pawn 坐标喂标尺
    for(int i=0;i<g_npawn;i++) if(g_pawns[i].local){
        g_lp[0]=g_pawns[i].pos[0]; g_lp[1]=g_pawns[i].pos[1]; g_lp[2]=g_pawns[i].pos[2];
        g_lp_have=1;
    }
}
static void write_snapshot(void){
    FILE* j=fopen("/data/local/tmp/esp_state.json.tmp","w");
    if(!j) return;
    fprintf(j,"{\"ts\":%llu,\"n\":%d,\"pawns\":[",(unsigned long long)now_ms(),g_npawn);
    for(int i=0;i<g_npawn;i++){
        Pawn* p=&g_pawns[i];
        fprintf(j,"%s{\"addr\":%lu,\"local\":%d,\"hp\":%.1f,\"pos\":[%.2f,%.2f,%.2f],\"nb\":%d,\"bones\":[",
                i?",":"",(unsigned long)p->addr,p->local,p->hp,p->pos[0],p->pos[1],p->pos[2],p->nb);
        for(int b=0;b<p->nb;b++)
            fprintf(j,"%s[%.1f,%.1f,%.1f]",b?",":"",p->bones[b*3],p->bones[b*3+1],p->bones[b*3+2]);
        fprintf(j,"]");
    }
    fprintf(j,"],\"cam\":{");
    if(inheap(g_cmgr)){
        fprintf(j,"\"locked\":1,\"cmgr\":%lu,\"loc\":[%.2f,%.2f,%.2f],\"yaw\":%.2f,\"pitch\":%.2f,\"fov\":%.2f",
                (unsigned long)g_cmgr,
                rdf(g_cmgr+OFF_CMGR_POV_LOC),rdf(g_cmgr+OFF_CMGR_POV_LOC+4),rdf(g_cmgr+OFF_CMGR_POV_LOC+8),
                rdf(g_cmgr+OFF_CMGR_POV_YAW),rdf(g_cmgr+OFF_CMGR_POV_PITCH),rdf(g_cmgr+OFF_CMGR_POV_FOV));
        if(g_mv_off>=0&&g_mp_off>=0){
            fprintf(j,",\"mbv\":%lu,\"mbp\":%lu,\"mv_off\":%d,\"mp_off\":%d,\"mv\":[",
                    (unsigned long)g_mv_base,(unsigned long)g_mp_base,g_mv_off,g_mp_off);
            for(int q=0;q<16;q++) fprintf(j,"%s%.5g",q?",":"",g_last_mv[q]);
            fprintf(j,"],\"mp\":[");
            for(int q=0;q<16;q++) fprintf(j,"%s%.5g",q?",":"",g_last_mp[q]);
            fprintf(j,"]");
        }
        fprintf(j,"}");
    } else fprintf(j,"\"locked\":0}");
    fprintf(j,"}\n");
    fclose(j);
    rename("/data/local/tmp/esp_state.json.tmp","/data/local/tmp/esp_state.json");
}

// ---- 自动找游戏进程 ----
static pid_t find_gnyx(void){
    DIR* d=opendir("/proc"); if(!d) return 0;
    struct dirent* de;
    while((de=readdir(d))){
        if(de->d_name[0]<'0'||de->d_name[0]>'9') continue;
        int pid=atoi(de->d_name);
        char pth[64],cmd[256]; cmd[0]=0;
        snprintf(pth,sizeof(pth),"/proc/%d/cmdline",pid);
        FILE* f=fopen(pth,"r"); if(!f) continue;
        size_t n=fread(cmd,1,255,f); fclose(f); cmd[n]=0;
        if(strstr(cmd,"com.tencent.tmgp.gnyx")||strstr(cmd,"AClient")){
            closedir(d); return pid;
        }
    }
    closedir(d);
    return 0;
}

int main(int argc,char** argv){
    pid_t pid=argc>1?atoi(argv[1]):find_gnyx();
    if(!pid){ printf("no gnyx process found\n"); return 1; }
    g_log=fopen("/data/local/tmp/gnyx_daemon.log","a");
    logf_("=== daemon start, pid=%d backend=proc_mem ===",pid);
    if(rw_open(pid)<0){ logf_("open mem failed"); return 1; }
    enum_modules();
    logf_("modules=%d:",g_nmod);
    for(int i=0;i<g_nmod;i++) logf_("  %s [0x%lx,0x%lx)",g_mods[i].name,(unsigned long)g_mods[i].base,(unsigned long)g_mods[i].end);
    // 初始扫描（异步收敛：每轮扫一部分，渐进定位）
    for(int round=0;round<200;round++){
        if(!g_gw){ scan_gworld(); }
        if(g_gw&&!g_pc){ refresh_pc(); }
        if(g_gw&&!g_gnpool){ scan_gnames(); }
        if(g_gw&&g_pc){ break; }
        usleep(500000);
    }
    if(!g_gw){ logf_("GWorld not found in 100s, exit"); return 2; }
    if(!g_pc){ logf_("PC not found (lobby?), keep trying in loop"); }
    uint64_t last_snap=0;
    while(1){
        uint64_t t=now_ms();
        if(g_pc&&!inheap(rd64(g_pc+OFF_PC_LOCALPAWN)==0?g_pc:g_pc)) { /* keep */ }
        // 换局重置：PC 失效重找
        if(!inheap(g_pc)||!inheap(g_cmgr)){ g_pc=0; g_cmgr=0; refresh_pc(); }
        if(g_pc){
            dump_pawns();
            mat_tick();
        }
        if(t-last_snap>=500){ write_snapshot(); last_snap=t; }
        usleep(120000);  // ~8Hz 采集，快照 2Hz
    }
    return 0;
}
