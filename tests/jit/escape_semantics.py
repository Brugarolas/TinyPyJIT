# Escape analysis + scalar replacement: allocations whose objects never escape
# are removed by the native JIT. Nothing about that may be observable: deopts
# in the middle of an object's lifetime, frame introspection, identity, class
# changes and genuine escapes must all behave exactly like the interpreter.
import gc

N = 300  # enough iterations to compile the loops and run them natively


class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class P3:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z
        self.w = 0


# --- the basic patterns -----------------------------------------------------------

def swap_fib(n):
    a = 0
    b = 1
    for i in range(n):
        a, b = b, a + b
    return a


def tuple_paths(n):
    s = 0
    for i in range(n):
        t = (i, i * 2, 3)
        x, y, z = t
        s += t[0] + t[-1] + x * y + z
    return s


def list_paths(n):
    s = 0
    for i in range(n):
        xs = [i, i + 1]
        a, b = xs
        s += xs[1] - a + b
    return s


def objects(n):
    s = 0
    for i in range(n):
        p = P3(i, i * 2, 5)
        q = p
        s += p.x + q.y + p.z + q.w
    return s


for _ in range(3):
    assert swap_fib(90) == 2880067194370816120
    assert tuple_paths(N) == sum([i + 3 + i * i * 2 + 3 for i in range(N)])
    assert list_paths(N) == sum([(i + 1) - i + (i + 1) for i in range(N)])
    assert objects(N) == sum([i + i * 2 + 5 for i in range(N)])


# --- a deopt between the allocation and its uses ----------------------------------
# A huge product overflows the native int path: the interpreter resumes in the
# middle of the tuple's lifetime and must find the real tuple in its register.

BIG = 1 << 40


def overflow_mid_region(n, big):
    s = 0
    for i in range(n):
        t = (i, big)
        k = t[1] * big  # overflows int64 when big is huge
        s += t[0] + k % 7
    return s


for _ in range(3):
    assert overflow_mid_region(N, 3) == sum([i + 9 % 7 for i in range(N)])
assert overflow_mid_region(N, BIG) == sum([i + (BIG * BIG) % 7 for i in range(N)])


def overflow_mid_object(n, big):
    s = 0
    for i in range(n):
        p = Vec(i, big)
        k = p.y * big
        s += p.x + k % 11
    return s


for _ in range(3):
    assert overflow_mid_object(N, 3) == sum([i + 9 % 11 for i in range(N)])
assert overflow_mid_object(N, BIG) == sum([i + (BIG * BIG) % 11 for i in range(N)])


# --- frame introspection sees the object ------------------------------------------

def introspect(n, at):
    seen = None
    for i in range(n):
        p = Vec(i, i + 1)
        s = p.x + p.y
        if i == at:
            seen = locals()['p']
    return seen


for _ in range(3):
    assert introspect(N, -1) is None
got = introspect(N, N - 5)
assert type(got) is Vec and got.x == N - 5 and got.y == N - 4


def introspect_identity(n, at):
    same = None
    for i in range(n):
        p = Vec(i, i)
        s = p.x
        if i == at:
            a = locals()['p']
            b = locals()['p']
            same = a is b and a is p
    return same


for _ in range(3):
    introspect_identity(N, -1)
assert introspect_identity(N, 7) is True


# Two locals name the object: introspection must find one object in both.
def two_names(n, at):
    out = None
    for i in range(n):
        p = Vec(i, i + 1)
        q = p
        s = p.x + q.y
        if i == at:
            out = locals()
    return out


# One of the two names is overwritten; the other still holds the object.
def one_name_left(n, at):
    out = None
    for i in range(n):
        p = Vec(i, i + 1)
        q = p
        s = p.x + q.y
        q = 0
        if i == at:
            out = locals()
    return out


# Whether the second name dies depends on the path: still exact (not removed).
def path_dependent(n, at, kill_q):
    out = None
    for i in range(n):
        p = Vec(i, i + 1)
        q = p
        s = p.x + q.y
        if kill_q:
            q = 0
        if i == at:
            out = locals()
    return out


for _ in range(3):
    two_names(N, -1)
    one_name_left(N, -1)
    path_dependent(N, -1, True)
L = two_names(N, 9)
assert L['p'] is L['q'] and L['p'].x == 9 and L['q'].y == 10
L = one_name_left(N, 9)
assert L['q'] == 0 and type(L['p']) is Vec and L['p'].y == 10
L = path_dependent(N, 9, False)
assert L['p'] is L['q']
L = path_dependent(N, 9, True)
assert L['q'] == 0 and L['p'].x == 9


# `p = f(); q = p` must assign both locals (a lowering bug once dropped p).
def assign_chain(n):
    s = 0
    for i in range(n):
        p = Vec(i, 2)
        q = p
        s += p.x + q.y
    return s


for _ in range(3):
    assert assign_chain(N) == sum([i + 2 for i in range(N)])


def eval_sees_tuple(n, at):
    out = None
    for i in range(n):
        t = (i, i * 3)
        s = t[0] + t[1]
        if i == at:
            out = eval("t[1] - t[0]")
    return out


for _ in range(3):
    eval_sees_tuple(N, -1)
assert eval_sees_tuple(N, 10) == 20


# A forced object is a real object from then on: later reads see mutations.
def mutate_after_force(n, at):
    s = 0
    for i in range(n):
        p = Vec(i, 1)
        if i == at:
            exec("p.x = 1000")
        s += p.x
    return s


for _ in range(3):
    assert mutate_after_force(N, -1) == sum(range(N))
assert mutate_after_force(N, 3) == sum(range(N)) - 3 + 1000


# --- the GC keeps components alive while only the save area holds them -----------

def gc_holds_components(n, at):
    out = None
    for i in range(n):
        p = Vec([i, i, i], "s" + str(i))
        first = p.x[0]
        gc.collect()
        if i == at:
            out = locals()['p']
    return out


for _ in range(3):
    gc_holds_components(60, -1)
got = gc_holds_components(60, 40)
assert got.x == [40, 40, 40] and got.y == "s40"


# --- class changes after compilation ---------------------------------------------

class Moving:
    def __init__(self, a, b):
        self.a = a
        self.b = b


def use_moving(n):
    s = 0
    for i in range(n):
        m = Moving(i, 2)
        s += m.a * m.b
    return s


for _ in range(3):
    assert use_moving(N) == sum([i * 2 for i in range(N)])

# A data descriptor now shadows the field: reads must go through it.
Moving.a = property(lambda self: 1, lambda self, value: None)
assert use_moving(N) == sum([1 * 2 for i in range(N)])
del Moving.a
assert use_moving(N) == sum([i * 2 for i in range(N)])


def init_three(self, a, b):
    self.a = a + 100
    self.b = b


# A different __init__: the allocation must run it for real.
Moving.__init__ = init_three
assert use_moving(N) == sum([(i + 100) * 2 for i in range(N)])


# --- genuine escapes are left alone -----------------------------------------------

def escapes(n):
    keep = []
    for i in range(n):
        t = (i, i + 1)
        keep.append(t)
        p = Vec(i, -i)
        keep.append(p)
    return keep


for _ in range(3):
    kept = escapes(50)
    assert kept[20] == (10, 11) and kept[21].y == -10
    assert len(kept) == 100


def returned(i):
    return (i, i + 1)


def via_return(n):
    s = 0
    for i in range(n):
        a, b = returned(i)
        s += b - a
    return s


for _ in range(3):
    assert via_return(N) == N


def unpack_mismatch(n):
    for i in range(n):
        t = (i, i, i)
        a, b = t  # noqa: wrong arity on purpose
    return 0


try:
    unpack_mismatch(5)
    assert False, "expected ValueError"
except ValueError:
    pass


# --- exceptions inside an object's lifetime --------------------------------------

def raises_mid_region(n, zero_at):
    s = 0
    for i in range(n):
        p = Vec(i, i - zero_at)
        s += p.x // p.y
    return s


for _ in range(3):
    raises_mid_region(N, -1)
try:
    raises_mid_region(N, 50)
    assert False, "expected ZeroDivisionError"
except ZeroDivisionError:
    pass


# --- long loops: periodic native exits land inside the holding range -------------

def long_loop(n):
    s = 0
    for i in range(n):
        p = Vec(i, 1)
        s += p.x * p.y
    return s


assert long_loop(20000) == sum(range(20000))


# --- recursion with virtual objects in every frame ---------------------------------

def rec(n):
    if n == 0:
        return 0
    t = (n, 1)
    a, b = t
    return a * b + rec(n - 1)


for _ in range(20):
    assert rec(30) == sum(range(31))

print("escape_semantics ok")
