# Exercise chained integer values, register pressure, loops, and fallback.

import gc


def chain(a, b, c):
    first = a + b
    second = first * 3
    third = second - c
    return third


def pressure(a, b, c, d, e, f, g, h):
    x0 = a + b
    x1 = c + d
    x2 = e + f
    x3 = g + h
    x4 = x0 + x2
    x5 = x1 + x3
    return x4 + x5


def sum_loop(n):
    value = 0
    i = 0
    while i < n:
        value = value + i
        i = i + 1
    return value


def polymorphic_add(a, b):
    return a + b


for i in range(250):
    assert chain(i, 7, 4) == (i + 7) * 3 - 4
    assert pressure(i, 2, 3, 4, 5, 6, 7, 8) == i + 35
    assert polymorphic_add(i, 9) == i + 9

# The native integer guard must deopt to ordinary Python semantics.
assert polymorphic_add('Tiny', 'Py') == 'TinyPy'
assert chain(11, 3, 4) == 38

for n in (0, 1, 10, 100, 4100, 20000):
    assert sum_loop(n) == n * (n - 1) // 2

# Force a major collection with no values hidden in native registers.
assert gc.collect() >= 0
assert pressure(1, 2, 3, 4, 5, 6, 7, 8) == 36
print('LIR + graph-color JIT smoke: ok')
