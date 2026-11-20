# Scalar recursion must either finish without effects or restart with normal
# Python frames. Exercise several CFGs, argument counts and failed assumptions.
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def triangular(n, acc):
    if n == 0:
        return acc
    return triangular(n - 1, acc + n)


def product(n):
    if n <= 1:
        return 1
    return n * product(n - 1)


for _ in range(40):
    assert fib(12) == 144
    assert triangular(40, 3) == 823
    assert product(10) == 3628800
assert fib(8.0) == 21.0
assert triangular(10, 0.5) == 55.5
def identity_rec(n, value):
    if n == 0:
        return value
    return identity_rec(n - 1, value)

for _ in range(40):
    assert identity_rec(5, 7) == 7
assert identity_rec(5, True) is True

# A symbolic callable must not be mistaken for an integer at a CFG merge.
def mixed(n, flag):
    if n == 0:
        return 7
    if flag:
        x = n
    else:
        x = mixed
    if flag:
        return mixed(n - 1, flag) + x
    return x(n - 1, flag)


for _ in range(40):
    assert mixed(10, 1) == 62
    assert mixed(10, 0) == 7

# Rebinding after compilation, including a mutation performed by a callee.
original = fib
fib = lambda n: 100
assert original(4) == 200
fib = original
assert fib(12) == 144

effects = []
def path_effect(n):
    if n < 0:
        effects.append(n)
        return 10
    if n == 0:
        return 0
    return path_effect(n - 2) + 1


for _ in range(40):
    assert path_effect(10) == 5
assert effects == []
assert path_effect(3) == 12
assert effects == [-1]  # failed speculation must not duplicate the append

# Overflow must take the interpreter's existing integer path, not silently use
# the native speculative result. The subtraction also overflows for MIN_I64.
def overflowing(n, x):
    if n == 0:
        return x
    return overflowing(n - 1, x + 1)


for _ in range(40):
    assert overflowing(5, 2) == 7
# Compare with the runtime's ordinary integer semantics (fixed width in TinyPy).
largest = 9223372036854775807
expected = largest + 1
assert overflowing(1, largest) == expected

def depth(n):
    if n == 0:
        return 0
    return depth(n - 1) + 1


for _ in range(40):
    assert depth(20) == 20
assert depth(150) == 150  # native-stack budget fallback
try:
    depth(100000)
    assert False
except RecursionError:
    pass
assert fib(10) == 55
print("scalar recursion ok")
