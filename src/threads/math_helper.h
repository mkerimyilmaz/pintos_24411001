#ifndef MATH_HELPER_H
#define MATH_HELPER_H

#include <stdint.h>
#define FLOAT_CONSTANT 15
#define MATH_HELPER_ONE (1 << FLOAT_CONSTANT)

static inline int int_to_fixed(int n) {
    return n << FLOAT_CONSTANT;
}

static inline int fixed_to_int(int x) {
    return x >> FLOAT_CONSTANT;
}

static inline int fixed_round(int x) {
    return (x >= 0) ? (x + (MATH_HELPER_ONE / 2)) >> FLOAT_CONSTANT 
                    : (x - (MATH_HELPER_ONE / 2)) >> FLOAT_CONSTANT;
}

static inline int fixed_add(int x, int y) {
    return x + y;
}

static inline int fixed_sub(int x, int y) {
    return x - y;
}

static inline int fixed_add_int(int x, int n) {
    return x + (n << FLOAT_CONSTANT);
}

static inline int fixed_sub_int(int x, int n) {
    return x - (n << FLOAT_CONSTANT);
}

static inline int fixed_mul(int x, int y) {
    return ((int64_t)x * y) / MATH_HELPER_ONE;
}

static inline int fixed_mul_int(int x, int n) {
    return x * n;
}

static inline int fixed_div(int x, int y) {
    return ((int64_t)x * MATH_HELPER_ONE) / y;
}


static inline int fixed_div_int(int x, int n) {
    return x / n;
}

#endif /* MATH_HELPER_H */
