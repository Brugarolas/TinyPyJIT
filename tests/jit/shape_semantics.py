# Instance attributes are shape-backed. These checks pin down the Python-visible
# semantics, and run hot so the RegVM inline caches (and the native JIT, when
# enabled) exercise their shape guards, transitions and fallbacks.
import pickle
import json

HOT = 40


class P:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm1(self):
        return abs(self.x) + abs(self.y)


# Same class, different insertion orders -> different shapes, same semantics.
def make(order, a, b):
    p = P.__new__(P)
    if order:
        p.x = a
        p.y = b
    else:
        p.y = b
        p.x = a
    return p


def read_xy(p):
    return p.x * 10 + p.y


for i in range(HOT):
    assert read_xy(make(True, 1, 2)) == 12
    assert read_xy(make(False, 3, 4)) == 34   # polymorphic site: guard must miss
    assert read_xy(P(5, 6)) == 56


# Growth beyond the initial storage capacity, then deletion and re-insertion.
class Bag:
    pass


def fill(n):
    b = Bag()
    for i in range(n):
        setattr(b, "a" + str(i), i)
    return b


for _ in range(HOT):
    b = fill(20)
    assert b.a0 == 0 and b.a19 == 19
    assert len(b.__dict__.items()) == 20
    del b.a5
    assert not hasattr(b, "a5")
    try:
        b.a5
        assert False, "deleted attribute must raise"
    except AttributeError:
        pass
    try:
        del b.a5
        assert False, "double delete must raise"
    except AttributeError:
        pass
    assert len(b.__dict__.items()) == 19
    b.a5 = "back"
    assert b.a5 == "back"
    assert getattr(b, "a6") == 6
    assert getattr(b, "missing", 42) == 42


# Store IC: same-slot stores and transition stores must both stay correct.
def store_loop(n):
    p = P(0, 0)
    for i in range(n):
        p.x = i
        p.y = p.x * 2
    return p.x + p.y


for _ in range(HOT):
    assert store_loop(100) == 99 * 3


def add_late(p, v):
    p.z = v          # transition store on fresh objects, plain store afterwards
    return p.z


for i in range(HOT):
    q = P(1, 2)
    assert add_late(q, i) == i
    assert add_late(q, i + 1) == i + 1
    assert q.x == 1 and q.y == 2


# Data descriptors beat instance attributes; instance attributes beat methods.
class Prop:
    def __init__(self):
        self._v = 1

    @property
    def v(self):
        return self._v * 100

    @v.setter
    def v(self, value):
        self._v = value

    def m(self):
        return "method"


for _ in range(HOT):
    o = Prop()
    assert o.v == 100
    o.v = 3
    assert o.v == 300 and o._v == 3
    assert o.m() == "method"
    o.m = lambda: "instance"
    assert o.m() == "instance"
    del o.m
    assert o.m() == "method"


# Class attributes are visible until shadowed per instance.
class Counter:
    total = 7


for _ in range(HOT):
    c = Counter()
    assert c.total == 7
    c.total = 1
    assert c.total == 1 and Counter.total == 7


# __dict__ view
p = P(1, 2)
d = p.__dict__
assert d["x"] == 1 and d.get("y") == 2 and "x" in d and "q" not in d
d["w"] = 9
assert p.w == 9
del d["w"]
assert not hasattr(p, "w")
assert sorted([k for k, _ in p.__dict__.items()]) == ["x", "y"]


# Subclass instances
class P3(P):
    def __init__(self, x, y, z):
        super().__init__(x, y)
        self.z = z


for _ in range(HOT):
    t = P3(1, 2, 3)
    assert read_xy(t) == 12 and t.z == 3 and t.norm1() == 3


# pickle round trip keeps all attributes (including late-added ones)
src = P(1, 2)
src.extra = [1, 2, 3]
dst = pickle.loads(pickle.dumps(src))
assert dst.x == 1 and dst.y == 2 and dst.extra == [1, 2, 3]
assert type(dst) is P

# json through __dict__
assert json.loads(json.dumps(P(3, 4).__dict__)) == {"x": 3, "y": 4}
assert json.dumps(Bag().__dict__) == "{}"

# Many objects: storage is freed by the GC without leaks or crashes.
import gc
objs = [P(i, i) for i in range(10000)]
for o in objs:
    o.k = o.x + 1
del objs
gc.collect()

print("shape semantics: ok")
