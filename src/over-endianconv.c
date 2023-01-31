#include <stdint.h>

// 将*p指向的16位无符号整数从小端序切换到大端序
void memrev16(void *p) {
    unsigned char *x = p, t;
    //[0,1]->[1,0]
    t = x[0];
    x[0] = x[1];
    x[1] = t;
}

// 将*p指向的32位无符号整数从小端序切换到大端序
void memrev32(void *p) {
    unsigned char *x = p, t;
    // [0,1,2,3]->[3,1,2,0]->[3,2,1,0]
    t = x[0];
    x[0] = x[3];
    x[3] = t;
    t = x[1];
    x[1] = x[2];
    x[2] = t;
}

// 将*p指向的64位无符号整数从小端序切换到big endian
void memrev64(void *p) {
    unsigned char *x = p, t;
    // [0,1,2,3,4,5,6,7] ->[7,6,5,4,3,2,1,0]
    t = x[0];
    x[0] = x[7];
    x[7] = t;
    t = x[1];
    x[1] = x[6];
    x[6] = t;
    t = x[2];
    x[2] = x[5];
    x[5] = t;
    t = x[3];
    x[3] = x[4];
    x[4] = t;
}

uint16_t intrev16(uint16_t v) {
    memrev16(&v);
    return v;
}

uint32_t intrev32(uint32_t v) {
    memrev32(&v);
    return v;
}

uint64_t intrev64(uint64_t v) {
    memrev64(&v);
    return v;
}

#ifdef REDIS_TEST
#    include <stdio.h>

#    define UNUSED(x) (void)(x)
int endianconvTest(int argc, char *argv[], int flags) {
    char buf[32];

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    sprintf(buf, "ciaoroma");
    memrev16(buf);
    printf("%s\n", buf);

    sprintf(buf, "ciaoroma");
    memrev32(buf);
    printf("%s\n", buf);

    sprintf(buf, "ciaoroma");
    memrev64(buf);
    printf("%s\n", buf);

    return 0;
}
#endif
