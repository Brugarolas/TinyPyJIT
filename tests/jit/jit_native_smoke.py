# Verify native integer arithmetic, native loop entries and deoptimization.


def arithmetic(a, b):
    return (a + b) * (a - b)


def series(n):
    acc = 0
    i = 0
    while i < n:
        acc = acc + i
        i = i + 1
    return acc


def dynamic_add(a, b):
    return a + b


for i in range(40):
    assert arithmetic(i + 9, 3) == (i + 12) * (i + 6)
    assert dynamic_add(i, 5) == i + 5

assert dynamic_add("hello", " world") == "hello world"

for i in range(10):
    assert series(1000 + i) == (1000 + i) * (999 + i) // 2

assert series(100000) == 4999950000

import gc
assert gc.collect() >= 0
assert arithmetic(9, 3) == 72
print("native JIT smoke: ok")
