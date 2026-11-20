import gc

class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class Child(Point):
    pass


def make(cls, x, y):
    return cls(x, y)


for i in range(80):
    for cls in [Point, Child]:
        p = make(cls, [i], {"y": i})
        assert type(p) is cls
        assert p.x == [i] and p.y == {"y": i}
        p.extra = i
        assert p.extra == i

# Keep pointer-valued fields alive across collections; allocation really escapes.
kept = [make(Point, [i], [i + 1]) for i in range(100)]
gc.collect()
for i in range(100):
    assert kept[i].x == [i] and kept[i].y == [i + 1]

class Wide:
    def __init__(self, x):
        self.a = x
        self.b = 2
        self.c = 3
        self.d = 4
        self.e = 5
        self.f = 6
        self.a = 7


for _ in range(80):
    w = Wide([1])
    assert w.a == 7 and w.b == 2 and w.f == 6

# __init__, descriptors and __new__ can all change after warmup.
old_init = Point.__init__
def changed(self, x, y):
    self.x = y
    self.y = x
Point.__init__ = changed
p = make(Point, 2, 3)
assert p.x == 3 and p.y == 2
Point.__init__ = old_init

def get_x(self):
    return self.saved
def set_x(self, value):
    self.saved = value + 100
Point.x = property(get_x, set_x)
for _ in range(40):
    p = make(Point, 2, 3)
    assert p.x == 102 and p.y == 3
del Point.x
assert make(Point, 2, 3).x == 2

sentinel = Point(8, 9)
def new_point(cls, x, y):
    return sentinel
Point.__new__ = new_point
assert make(Point, 10, 11) is sentinel
assert sentinel.x == 10 and sentinel.y == 11
print("constructor fast path ok")
