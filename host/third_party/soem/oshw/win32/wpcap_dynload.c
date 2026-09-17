/* ============================================================================
 *  wpcap_dynload.c — 运行时动态加载 wpcap.dll（Npcap / WinPcap）
 *
 *  MinGW-w64 的 ld 不支持 --delayload；直接链接 wpcap.lib/Packet.lib 会让
 *  整个 exe 在没有 Npcap 的机器上加载失败。这里改为：不链接导入库，由本文件
 *  提供 SOEM win32 层用到的 6 个 pcap 函数，首次调用时用 LoadLibraryA 载入
 *  wpcap.dll 并 GetProcAddress 解析；DLL 不存在时返回错误而非崩溃。
 *
 *  SOEM(oshw/win32) 只用到：pcap_open / pcap_close / pcap_sendpacket /
 *  pcap_next_ex / pcap_findalldevs / pcap_freealldevs。
 * ==========================================================================*/
#define HAVE_REMOTE
#include <windows.h>
#include <pcap.h>

static HMODULE g_wpcap = NULL;

static void *ec_load_sym(const char *name)
{
    if (g_wpcap == NULL)
        g_wpcap = LoadLibraryA("wpcap.dll");
    if (g_wpcap == NULL)
        g_wpcap = LoadLibraryA("Packet.dll"); /* 兜底：部分环境 */
    if (g_wpcap == NULL)
        return NULL;
    return (void *)GetProcAddress(g_wpcap, name);
}

/* 抓包驱动是否可用（供上位机判断，避免直接调用） */
int ec_wpcap_available(void)
{
    return ec_load_sym("pcap_findalldevs") != NULL;
}

pcap_t *pcap_open(const char *source, int snaplen, int flags, int read_timeout,
                  struct pcap_rmtauth *auth, char *errbuf)
{
    typedef pcap_t *(*F)(const char *, int, int, int, struct pcap_rmtauth *, char *);
    static F f = NULL;
    if (f == NULL) f = (F)ec_load_sym("pcap_open");
    if (f == NULL) { if (errbuf) errbuf[0] = 0; return NULL; }
    return f(source, snaplen, flags, read_timeout, auth, errbuf);
}

void pcap_close(pcap_t *p)
{
    typedef void (*F)(pcap_t *);
    static F f = NULL;
    if (f == NULL) f = (F)ec_load_sym("pcap_close");
    if (f) f(p);
}

int pcap_sendpacket(pcap_t *p, const u_char *buf, int size)
{
    typedef int (*F)(pcap_t *, const u_char *, int);
    static F f = NULL;
    if (f == NULL) f = (F)ec_load_sym("pcap_sendpacket");
    if (f == NULL) return -1;
    return f(p, buf, size);
}

int pcap_next_ex(pcap_t *p, struct pcap_pkthdr **pkt_header, const u_char **pkt_data)
{
    typedef int (*F)(pcap_t *, struct pcap_pkthdr **, const u_char **);
    static F f = NULL;
    if (f == NULL) f = (F)ec_load_sym("pcap_next_ex");
    if (f == NULL) return -1;
    return f(p, pkt_header, pkt_data);
}

int pcap_findalldevs(pcap_if_t **alldevsp, char *errbuf)
{
    typedef int (*F)(pcap_if_t **, char *);
    static F f = NULL;
    if (f == NULL) f = (F)ec_load_sym("pcap_findalldevs");
    if (f == NULL) { if (errbuf) errbuf[0] = 0; return -1; }
    return f(alldevsp, errbuf);
}

void pcap_freealldevs(pcap_if_t *alldevs)
{
    typedef void (*F)(pcap_if_t *);
    static F f = NULL;
    if (f == NULL) f = (F)ec_load_sym("pcap_freealldevs");
    if (f) f(alldevs);
}
