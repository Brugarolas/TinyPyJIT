# Native floor division / modulo by constant divisors (strength-reduced in JIT
# islands) must match the generic operation exactly, including negative
# dividends, powers of two and large magnitudes.

def by_const(n):
    return (n % 12, n // 12, n % 7, n // 7, n % 8, n // 8, n % 1, n // 1,
            n % 3, n // 3, n % 1000, n // 1000, n % 32767, n // 32767, n % 2, n // 2)

def by_var(n, d):
    return n % d, n // d

DIVISORS = (12, 7, 8, 1, 3, 1000, 32767, 2)

def expected(n):
    out = []
    for d in DIVISORS:
        q, r = by_var(n, d)
        out.append(q)
        out.append(r)
    return tuple(out)

samples = [0, 1, -1, 5, -5, 11, -11, 12, -12, 13, -13, 99, -99, 1000, -1000,
           123456789, -123456789, 2**40 + 7, -(2**40) - 7, 2**62, -(2**62),
           9223372036854775807, -9223372036854775807]
for i in range(-300, 300):
    samples.append(i * 37)

for _ in range(3):  # warm up and run the compiled code
    for n in samples:
        assert by_const(n) == expected(n), (n, by_const(n), expected(n))

# Explicit spot checks of Python floor semantics.
assert by_const(-5)[0:2] == (7, -1)
assert by_const(-13)[0:2] == (11, -2)
assert by_const(-1)[4:6] == (7, -1)
print("ok")
