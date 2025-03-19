#ifndef __THREAD_FIXED_POINT_H
#define __THREAD_FIXED_POINT_H

typedef int constant_point;
#define FLOAT_CONSTANT 15
// Convert x to integer (rounding toward zero):	x / f
// convert x to integer rounding to nearest!
// (x + f / 2) / f if x >= 0,
// (x - f / 2) / f if x <= 0.
// Add x and y:	x + y
// Subtract y from x:	x - y
// Add x and n:	x + n * f
// Subtract n from x:	x - n * f
// Multiply x by y:	((int64_t) x) * y / f
// Multiply x by n:	x * n
// Divide x by y:	((int64_t) x) * f / y
// Divide x by n:	x / n
#define FIXED_POINT_CONVERT(x) ((constant_point)(x << FLOAT_CONSTANT))
#define FIXED_POINT_ROUND_TO_INT(x) (x >= 0 ? ((x + (1 << (FLOAT_CONSTANT - 1))) >> FLOAT_CONSTANT ) : ((x - (1 << (FLOAT_CONSTANT - 1))) >> FLOAT_CONSTANT ))
#define FIXED_POINT_ADD(x,y) (x + y)
#define FIXED_POINT_ADD_INT(x,n) (x + (n << FLOAT_CONSTANT  ))
#define FIXED_POINT_SUBTRACT(x,y) (x - y)
#define FIXED_POINT_MULTIPLY(x,y) (x * y)
#define FIXED_POINT_MULTIPLY_INT(x,n) ((constant_point)(((int64_t) x) * n >> FLOAT_CONSTANT  ))
#define FIXED_POINT_DIVIDE(x,y) (x / y)
#define FIXED_POINT_DIVIDE_INT(x,n) ((constant_point)((((int64_t) x) << FLOAT_CONSTANT  ) / n))

#endif /* thread/fixed_point.h */

