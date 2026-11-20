def exchange(xs, i, j):
    xs[i], xs[j] = xs[j], xs[i]
    return xs[i]


for _ in range(80):
    xs = [10, 20, 30]
    assert exchange(xs, 0, -1) == 30
    assert xs == [30, 20, 10]
    assert exchange(xs, -3, -1) == 10

for index in [3, -4]:
    try:
        exchange([1, 2, 3], index, 0)
        assert False
    except IndexError:
        pass

# Generic fallback remains inside the native caller and may run Python code.
class Box:
    def __init__(self):
        self.xs = [1, 2, 3]
    def __getitem__(self, i):
        return self.xs[i] + 10
    def __setitem__(self, i, value):
        self.xs[i] = value + 100


b = Box()
assert exchange(b, 0, 2) == 123
assert b.xs == [113, 2, 111]

def read(xs, index):
    return xs[index]


def write(xs, index, value):
    xs[index] = value
    return xs


for _ in range(80):
    assert read([4, 5], 1) == 5
    assert write([0, 0], -1, 9) == [0, 9]
assert read([1, 2, 3], slice(0, 2, 1)) == [1, 2]
assert read((5, 6), -1) == 6
assert write({}, "key", 17) == {"key": 17}
try:
    write([], 0, 1)
    assert False
except IndexError:
    pass

def resized(xs, f):
    first = xs[0]
    f(xs)
    xs[-1] = first
    return xs[-1]


def grow(xs):
    for i in range(100):
        xs.append(i)


for _ in range(40):
    assert resized([55], grow) == 55

# Result overwrites the container register and can itself be a pointer.
def nested(xs):
    xs = xs[0]
    return xs[0]


for _ in range(40):
    assert nested([["alive"]]) == "alive"

# Validate sorting, since the timing-only sort benchmark has no output check.
def qsort(a, left, right):
    if left >= right:
        return
    mid = a[(left + right) // 2]
    i, j = left, right
    while i <= j:
        while a[i] < mid:
            i += 1
        while a[j] > mid:
            j -= 1
        if i <= j:
            a[i], a[j] = a[j], a[i]
            i += 1
            j -= 1
    qsort(a, left, j)
    qsort(a, i, right)


for n in [0, 1, 2, 10, 1000]:
    values = [(i * 73) % 101 - 50 for i in range(n)]
    expected = sorted(values)
    qsort(values, 0, len(values) - 1)
    assert values == expected

# TinyPy allows builtin type mutations; CPython rejects them. Guards must
# handle mutations both after native compilation and before a new compilation.
old_get = list.__getitem__
old_set = list.__setitem__
def replacement_get(self, index):
    return 1234
def replacement_set(self, index, value):
    return None

changed = False
try:
    list.__getitem__ = replacement_get
    changed = True
except TypeError:
    pass
if changed:
    list.__setitem__ = replacement_set
    assert read([1, 2], 0) == 1234
    untouched = [1, 2]
    write(untouched, 0, 99)
    def compiled_after_mutation(xs):
        xs[0] = 9
        return xs[0]
    for _ in range(40):
        assert compiled_after_mutation(untouched) == 1234
    list.__getitem__ = old_get
    list.__setitem__ = old_set
    assert untouched == [1, 2]
    assert read(untouched, 0) == 1
    assert compiled_after_mutation(untouched) == 9
print("native list paths ok")
