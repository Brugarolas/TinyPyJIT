# Native code keeps CALL / LOAD_METHOD (and IC misses) inside native code by
# calling back into the RegVM. Everything a callee can do - raise, recurse,
# rebind globals, change classes, shadow methods - must be observed exactly as
# the interpreter would.

HOT = 60


# --- exceptions raised by a callee unwind through native frames ----------------

def maybe_fail(i):
    if i == 77:
        raise ValueError("boom %d" % i)
    return i


def caller(n):
    s = 0
    for i in range(n):
        s += maybe_fail(i)
    return s


for _ in range(HOT):
    assert caller(50) == sum(range(50))

caught = None
try:
    caller(100)
except ValueError as e:
    caught = str(e)
assert caught == "boom 77", caught

# The frame that raised is unwound: later calls still run correctly.
for _ in range(HOT):
    assert caller(60) == sum(range(60))


# --- a failing callee in the middle of an expression -------------------------

def div(a, b):
    return a // b


def ratio_sum(xs):
    total = 0
    for x in xs:
        total += div(100, x)
    return total


for _ in range(HOT):
    assert ratio_sum([1, 2, 4, 5]) == 100 + 50 + 25 + 20
try:
    ratio_sum([1, 0])
    assert False, "expected ZeroDivisionError"
except ZeroDivisionError:
    pass


# --- recursion: native -> helper -> native, and the recursion limit ------------

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


for _ in range(5):
    assert fib(18) == 2584


def depth(n):
    if n == 0:
        return 0
    return 1 + depth(n - 1)


for _ in range(HOT):
    assert depth(50) == 50

hit_limit = False
try:
    depth(100000)
except RecursionError:
    hit_limit = True
assert hit_limit
assert depth(30) == 30


# --- a global rebound by a callee while the caller runs natively ---------------

SCALE = 1


def bump_scale():
    global SCALE
    SCALE = SCALE + 1


def read_scale(n, bump_at):
    s = 0
    for i in range(n):
        if i == bump_at:
            bump_scale()
        s += SCALE
    return s


for _ in range(HOT):
    SCALE = 1
    assert read_scale(10, -1) == 10
SCALE = 1
assert read_scale(10, 5) == 5 * 1 + 5 * 2


# --- a site first reached after compilation (no cache yet): NameError ----------

def late_global(n, use_missing):
    s = 0
    for i in range(n):
        s += i
    if use_missing:
        s += MISSING_NAME  # noqa: never defined
    return s


for _ in range(HOT):
    assert late_global(20, False) == sum(range(20))
try:
    late_global(20, True)
    assert False, "expected NameError"
except NameError:
    pass
MISSING_NAME = 1000
assert late_global(20, True) == sum(range(20)) + 1000


# --- methods replaced on the class by a callee mid-loop -------------------------

class Counter:
    def __init__(self):
        self.n = 0

    def step(self):
        return 1


def step_two(self):
    return 2


def swap_method():
    Counter.step = step_two


def run_steps(c, n, swap_at):
    s = 0
    for i in range(n):
        if i == swap_at:
            swap_method()
        s += c.step()
    return s


c = Counter()
for _ in range(HOT):
    assert run_steps(c, 10, -1) == 10
assert run_steps(c, 10, 4) == 4 * 1 + 6 * 2
Counter.step = lambda self: 1


# --- an instance attribute shadowing the method, set by a callee ----------------

class Greeter:
    def hello(self):
        return "method"


def shadow(g):
    g.hello = lambda: "instance"


def greet_loop(g, n, shadow_at):
    out = []
    for i in range(n):
        if i == shadow_at:
            shadow(g)
        out.append(g.hello())
    return out


g = Greeter()
for _ in range(HOT):
    assert greet_loop(g, 4, -1) == ["method"] * 4
g2 = Greeter()
assert greet_loop(g2, 4, 2) == ["method", "method", "instance", "instance"]
del g2.hello
assert greet_loop(g2, 2, -1) == ["method", "method"]


# --- builtin / bound / static / class methods through native call sites --------

class Mixed:
    base = 10

    def inst(self, x):
        return x + 1

    @staticmethod
    def stat(x):
        return x * 2

    @classmethod
    def cls(cls, x):
        return cls.base + x


def mixed_calls(m, n):
    xs = []
    s = 0
    for i in range(n):
        xs.append(i)
        s += m.inst(i) + m.stat(i) + m.cls(i) + len(xs) + abs(-i)
    return s


m = Mixed()
expected = sum([(i + 1) + i * 2 + (10 + i) + (i + 1) + i for i in range(30)])
for _ in range(HOT):
    assert mixed_calls(m, 30) == expected


# --- keyword arguments and *args fall back to the generic call path -------------

def kw(a, *rest, b=5):
    return a + b + len(rest)


def kw_calls(n):
    s = 0
    for i in range(n):
        s += kw(i) + kw(i, b=1) + kw(i, 2, 3, b=0)
    return s


for _ in range(HOT):
    assert kw_calls(10) == sum([(i + 5) + (i + 1) + (i + 2) for i in range(10)])


# --- a recursive function whose loop compiles while outer activations run ------

def nested_loops(level):
    s = 0
    for i in range(40):
        s += i
    if level > 0:
        s += nested_loops(level - 1)
    for j in range(40):
        s += j
    return s


one = 2 * sum(range(40))
for _ in range(HOT):
    assert nested_loops(6) == 7 * one


# --- generator and closure calls from native code ------------------------------

def gen(n):
    for i in range(n):
        yield i


def make_adder(k):
    def add(x):
        return x + k
    return add


add3 = make_adder(3)


def gen_and_closure(n):
    s = 0
    for i in range(n):
        for v in gen(3):
            s += v
        s += add3(i)
    return s


for _ in range(HOT):
    assert gen_and_closure(10) == 10 * 3 + sum([i + 3 for i in range(10)])

print("call_semantics ok")
