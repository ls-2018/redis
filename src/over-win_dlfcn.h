typedef struct Dl_info {
    unsigned char *dli_sname;
    unsigned char *dli_saddr;
    unsigned char *dli_fname;
    unsigned char *dli_fbase;
} Dl_info;

extern int dladdr(const void *, Dl_info *);
