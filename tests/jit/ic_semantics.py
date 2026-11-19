# Global / nonlocal / attribute inline caches are shared by the RegVM and the
# native JIT. Every invalidation path must be observed by both.

HOT = 40
G = 1


def read_g(n):
    s = 0
    for i in range(n):
        s += G
    return s


for _ in range(HOT):
    assert read_g(100) == 100 * G

# Rebinding a global changes the module dict version: caches must miss.
G = 7
for _ in range(HOT):
    assert read_g(100) == 700

# Deleting then re-creating the global.
del G
try:
    read_g(1)
    assert False, "expected NameError"
except NameError:
    pass
G = 3
assert read_g(10) == 30


# A builtin, later shadowed by a module global of the same name.
def use_len(xs, n):
    total = 0
    for i in range(n):
        total += len(xs)
    return total


for _ in range(HOT):
    assert use_len([1, 2, 3], 50) == 150


def len(x):  # noqa: shadow the builtin at module level
    return 100


for _ in range(HOT):
    assert use_len([1, 2, 3], 50) == 5000
del len
assert use_len([1, 2], 10) == 20


# Closures: the free variable must win over a global with the same name.
LIMIT = 5


def make_counter(limit):
    def count(n):
        c = 0
        for i in range(n):
            if i < limit:  # closure variable, not the global LIMIT
                c += 1
        return c
    return count


c10 = make_counter(10)
c3 = make_counter(3)
for _ in range(HOT):
    assert c10(100) == 10
    assert c3(100) == 3


def uses_global_limit(n):
    c = 0
    for i in range(n):
        if i < LIMIT:
            c += 1
    return c


for _ in range(HOT):
    assert uses_global_limit(100) == 5
LIMIT = 50
for _ in range(HOT):
    assert uses_global_limit(100) == 50


# Class attribute changes bump type_version: attribute caches must miss.
class K:
    scale = 2

    def __init__(self, v):
        self.v = v


def scaled(objs):
    s = 0
    for o in objs:
        s += o.v * o.scale
    return s


ks = [K(i) for i in range(10)]
for _ in range(HOT):
    assert scaled(ks) == 90
K.scale = 3
for _ in range(HOT):
    assert scaled(ks) == 135
ks[0].scale = 1000      # instance attribute shadows the class attribute
assert scaled(ks) == 135 + 0 * 1000


# A property added to the class after the caches are warm.
class Q:
    def __init__(self):
        self.x = 1


def get_x(objs):
    s = 0
    for o in objs:
        s += o.x
    return s


qs = [Q() for _ in range(10)]
for _ in range(HOT):
    assert get_x(qs) == 10
Q.x = property(lambda self: 5)
for _ in range(HOT):
    assert get_x(qs) == 50


# Store caches across shapes: same site, objects built in different orders.
class S:
    pass


def mk(flag):
    s = S()
    if flag:
        s.a = 1
        s.b = 2
    else:
        s.b = 2
        s.a = 1
    return s


def bump_a(objs, n):
    for i in range(n):
        for o in objs:
            o.a = o.a + 1


objs = [mk(i % 2 == 0) for i in range(6)]
for _ in range(HOT):
    bump_a(objs, 3)
for o in objs:
    assert o.a == 1 + 3 * HOT and o.b == 2

print("inline cache semantics: ok")
