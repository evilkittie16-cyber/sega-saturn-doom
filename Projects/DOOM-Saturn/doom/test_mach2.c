
#define FixedMul(a,b) (((long long)(a) * (b))>>16)
int test(int a, int b) {
    return (int)(FixedMul(a, b) >> 16);
}
