# Native JIT coverage: every path must match the interpreter exactly, both
# before a function gets hot (interpreted) and after (native code).
# Run with TINYPY_JIT_STATS=1 to see how much of it executed natively.

HOT = 40  # well above the function/loop JIT thresholds


def floordiv(a, b):
    return a // b


def mod(a, b):
    return a % b


for _ in range(HOT):
    assert floordiv(7, 2) == 3
    assert floordiv(-7, 2) == -4
    assert floordiv(7, -2) == -4
    assert floordiv(-7, -2) == 3
    assert floordiv(6, 3) == 2
    assert floordiv(-6, 3) == -2
    assert floordiv(0, 5) == 0
    assert mod(7, 3) == 1
    assert mod(-7, 3) == 2
    assert mod(7, -3) == -2
    assert mod(-7, -3) == -1
    assert mod(9, 3) == 0
    assert mod(-9, 3) == 0
    assert mod(0, -4) == 0

# Zero division raised from a native frame must still be a Python exception.
for _ in range(HOT):
    try:
        floordiv(1, 0)
        assert False, "expected ZeroDivisionError"
    except ZeroDivisionError:
        pass
    try:
        mod(1, 0)
        assert False, "expected ZeroDivisionError"
    except ZeroDivisionError:
        pass

# INT64_MIN operands deoptimize; results must equal the interpreter's.
INT64_MIN = -9223372036854775807 - 1
ref_div = floordiv(INT64_MIN, 7)
ref_mod = mod(INT64_MIN, 7)
for _ in range(HOT):
    assert floordiv(INT64_MIN, 7) == ref_div
    assert mod(INT64_MIN, 7) == ref_mod

# Mixed types fall back to the generic interpreter paths.
for _ in range(HOT):
    assert floordiv(7.5, 2) == 3.0
    assert mod(7.5, 2) == 1.5
    assert mod("%d-%s", (1, "x")) == "1-x"


def bits(a, b):
    return (a & b, a | b, a ^ b)


def shifts(a, n):
    return (a << n, a >> n)


for _ in range(HOT):
    assert bits(12, 10) == (8, 14, 6)
    assert bits(-1, 255) == (255, -1, -256)
    assert shifts(1, 10) == (1024, 0)
    assert shifts(-1024, 3) == (-8192, -128)
    assert shifts(5, 0) == (5, 5)
    assert shifts(1, 62) == (4611686018427387904, 0)


def arith_mix(n):
    acc = 0
    i = 0
    while i < n:
        acc = (acc * 31 + i // 3 - i % 7) & 0xFFFFFF
        acc = acc ^ (i << 2)
        i += 1
    return acc


def arith_mix_ref(n):
    # Same computation, kept cold by being called only once.
    acc = 0
    for i in [k for k in range(n)]:
        acc = (acc * 31 + i // 3 - i % 7) & 0xFFFFFF
        acc = acc ^ (i << 2)
    return acc


expected = arith_mix_ref(5000)
for _ in range(HOT):
    assert arith_mix(5000) == expected


# ---- range iteration ------------------------------------------------------

def range_sum(a, b, step):
    s = 0
    for i in range(a, b, step):
        s += i
    return s


def range_count(n):
    c = 0
    for _ in range(n):
        c += 1
    return c


for _ in range(HOT):
    assert range_sum(0, 100, 1) == 4950
    assert range_sum(0, 100, 7) == 735
    assert range_sum(100, 0, -1) == 5050
    assert range_sum(100, 0, -9) == 606
    assert range_sum(5, 5, 1) == 0
    assert range_sum(5, 0, 1) == 0
    assert range_sum(0, 5, -1) == 0
    assert range_count(0) == 0
    assert range_count(1) == 1
    assert range_count(10000) == 10000

# The loop variable keeps its last value after the loop.
def last_index(n):
    i = -1
    for i in range(n):
        pass
    return i

for _ in range(HOT):
    assert last_index(10) == 9
    assert last_index(0) == -1


# Non-range iterators take the generic interpreter path at the same opcode.
def list_sum(xs):
    s = 0
    for x in xs:
        s += x
    return s

data = list(range(100))
for _ in range(HOT):
    assert list_sum(data) == 4950
    assert list_sum((1, 2, 3)) == 6


# ---- partial compilation: loops mixing native and interpreted opcodes ------

COUNTER = [0]


def bump():
    COUNTER[0] += 1


def loop_with_calls(n):
    total = 0
    for i in range(n):
        total += i * 2
        if i % 100 == 0:
            bump()  # global + call: side exit, rarely taken
    return total


COUNTER[0] = 0
for _ in range(HOT):
    assert loop_with_calls(1000) == 999000
assert COUNTER[0] == HOT * 10


class Box:
    def __init__(self, v):
        self.v = v


def attr_loop(boxes):
    s = 0
    for b in boxes:
        s += b.v  # attribute access every iteration: entry gets disabled
    return s


boxes = [Box(i) for i in range(50)]
for _ in range(HOT):
    assert attr_loop(boxes) == 1225


def mixed_types(n):
    acc = 0
    for i in range(n):
        if i == n // 2:
            acc = acc + 0.5  # acc becomes a float halfway: guards must deopt
        acc = acc + i
    return acc


for _ in range(HOT):
    assert mixed_types(10) == 45.5


def nested(n):
    total = 0
    for i in range(n):
        for j in range(i):
            total += i ^ j
    return total


ref = 0
for i in range(60):
    for j in range(i):
        ref += i ^ j
for _ in range(HOT):
    assert nested(60) == ref


def early_return(n):
    for i in range(n):
        if i * i > 500:
            return i
    return -1


for _ in range(HOT):
    assert early_return(100) == 23
    assert early_return(5) == -1

print("native JIT coverage smoke: ok")
