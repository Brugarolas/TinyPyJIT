# Escape analysis across control flow: uses after branches, aliases that die on
# only some paths, len() of virtual tuples/lists and field writes on virtual
# objects. Every case must behave exactly like the interpreter, including when
# the object is materialized (deopt, frame introspection) in the middle.

N = 300
BIG = 1 << 40


class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class Box:
    def __init__(self, v):
        self.v = v
        self.n = 0


# --- uses after an if / inside both branches ----------------------------------------

def after_if(n, flag):
    s = 0
    for i in range(n):
        p = Vec(i, 2)
        if flag:
            s += 1
        s += p.x * p.y
    return s


def in_branches(n):
    s = 0
    for i in range(n):
        t = (i, i + 1, i + 2)
        if i % 3 == 0:
            s += t[0]
        elif i % 3 == 1:
            a, b, c = t
            s += b * c
        else:
            s += t[-1]
    return s


def ref_in_branches(n):
    s = 0
    for i in range(n):
        if i % 3 == 0:
            s += i
        elif i % 3 == 1:
            s += (i + 1) * (i + 2)
        else:
            s += i + 2
    return s


for _ in range(3):
    assert after_if(N, True) == sum([i * 2 + 1 for i in range(N)])
    assert after_if(N, False) == sum([i * 2 for i in range(N)])
    assert in_branches(N) == ref_in_branches(N)


# --- aliases that die on only some paths ---------------------------------------------

def some_paths(n):
    s = 0
    other = Vec(-1, -1)
    for i in range(n):
        p = Vec(i, 1)
        q = p
        if i % 2:
            q = other
        s += p.x + q.x
    return s


def either_object(n):
    s = 0
    for i in range(n):
        if i % 2:
            t = (i, 1)
        else:
            t = (1, i)
        a, b = t
        s += a - b
    return s


for _ in range(3):
    assert some_paths(N) == sum([i + (-1 if i % 2 else i) for i in range(N)])
    assert either_object(N) == sum([(i - 1) if i % 2 else (1 - i) for i in range(N)])


# Introspection sees exactly the names that still hold the object on this path.
def names_on_path(n, at, kill):
    out = None
    for i in range(n):
        p = Vec(i, i)
        q = p
        if kill:
            q = None
        s = p.x
        if i == at:
            out = locals()
    return out


for _ in range(3):
    names_on_path(N, -1, True)
    names_on_path(N, -1, False)
L = names_on_path(N, 11, False)
assert L['p'] is L['q'] and L['p'].x == 11
L = names_on_path(N, 11, True)
assert L['q'] is None and L['p'].x == 11


# A value carried around the loop: real on entry from the interpreter, then
# virtual on every later iteration.
def carried(n):
    t = (0, 1)
    for i in range(n):
        a, b = t
        t = (b, (a + b) % 1000003)
    return t[0]


def carried_ref(n):
    a, b = 0, 1
    for i in range(n):
        a, b = b, (a + b) % 1000003
    return a


for _ in range(3):
    assert carried(5000) == carried_ref(5000)


# --- len() of virtual tuples and lists -------------------------------------------------

def lengths(n):
    s = 0
    for i in range(n):
        t = (i, i, i)
        xs = [i, i + 1]
        s += len(t) * 10 + len(xs) + xs[1] - t[0]
    return s


for _ in range(3):
    assert lengths(N) == N * (30 + 2 + 1)


def len_then_branch(n):
    s = 0
    for i in range(n):
        t = (i, 1)
        k = len(t)
        if i % 2:
            s += k
        else:
            s += t[0]
    return s


for _ in range(3):
    assert len_then_branch(N) == sum([2 if i % 2 else i for i in range(N)])


def len_shadowed(n):
    s = 0
    for i in range(n):
        t = (i, i)
        s += len(t)
    return s


for _ in range(3):
    assert len_shadowed(N) == 2 * N
len = lambda x: 99  # noqa: E731 - the cached builtin is gone; the call must run
assert len_shadowed(N) == 99 * N
del len
assert len_shadowed(N) == 2 * N


# --- field writes on virtual objects ------------------------------------------------------

def field_writes(n):
    s = 0
    for i in range(n):
        p = Vec(i, 0)
        p.y = p.x * 2
        p.x = p.y + 1
        s += p.x + p.y
    return s


def counter(n):
    s = 0
    for i in range(n):
        b = Box(i)
        b.n = b.n + 1
        b.n = b.n + b.v
        s += b.n
    return s


for _ in range(3):
    assert field_writes(N) == sum([(2 * i + 1) + 2 * i for i in range(N)])
    assert counter(N) == sum([1 + i for i in range(N)])


# A write, then the object is materialized: it carries the written value.
def written_then_seen(n, at):
    seen = None
    for i in range(n):
        p = Vec(i, 0)
        p.y = i * 3
        if i == at:
            seen = locals()['p']
        s = p.y
    return seen


for _ in range(3):
    written_then_seen(N, -1)
got = written_then_seen(N, 17)
# Same field layout as an object built for real (TinyPy does not promise insertion order).
assert got.x == 17 and got.y == 51
assert [k for k, _ in got.__dict__.items()] == [k for k, _ in Vec(0, 0).__dict__.items()]


def written_then_deopt(n, big):
    s = 0
    for i in range(n):
        p = Vec(i, 0)
        p.y = big
        k = p.y * big  # overflows int64 when big is huge: deopt mid-lifetime
        s += p.x + p.y % 7 + k % 5
    return s


for _ in range(3):
    assert written_then_deopt(N, 3) == sum([i + 3 + 9 % 5 for i in range(N)])
assert written_then_deopt(N, BIG) == sum([i + BIG % 7 + (BIG * BIG) % 5 for i in range(N)])


# Writes after a branch, and a property added later intercepts them.
def write_after_branch(n, flag):
    s = 0
    for i in range(n):
        p = Vec(i, 1)
        if flag:
            s += 1
        p.y = p.x + 10
        s += p.y
    return s


for _ in range(3):
    assert write_after_branch(N, True) == sum([i + 10 + 1 for i in range(N)])


class Spy:
    calls = 0


def spy_set(self, value):
    Spy.calls += 1


Vec.y = property(lambda self: 5, spy_set)
assert write_after_branch(N, False) == 5 * N
assert Spy.calls == 2 * N  # __init__ and the field write both go through the setter
del Vec.y
assert write_after_branch(N, False) == sum([i + 10 for i in range(N)])


# --- genuine escapes stay real ----------------------------------------------------------------

def stored_elsewhere(n):
    holder = Box(None)
    s = 0
    for i in range(n):
        p = Vec(i, 1)
        holder.v = p  # the object escapes into another object
        s += p.x
    return holder.v.x + s


def deleted_field(n):
    s = 0
    for i in range(n):
        p = Vec(i, 1)
        del p.y
        s += p.x + len(p.__dict__.items())
    return s


for _ in range(3):
    assert stored_elsewhere(N) == (N - 1) + sum(range(N))
    assert deleted_field(N) == sum([i + 1 for i in range(N)])

print("escape_paths ok")
