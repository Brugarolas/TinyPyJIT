# PGO training workload (used by `build/tinypy.build.ts --pgo-train`).
#
# The regression tests give breadth but run mostly cold code, and the scripts
# in benchmarks/ keep their hot loops at module level (stack interpreter). This
# workload adds what real programs spend their time on inside functions: calls,
# methods, attribute and global access, recursion, allocation, containers and
# string work, so the profile does not teach the compiler that those are cold.
# Keep it balanced: whatever dominates here gets the best code layout.

LIMIT = 50


class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def add(self, o):
        return Vec(self.x + o.x, self.y + o.y)

    def dot(self, o):
        return self.x * o.x + self.y * o.y


class Particle(Vec):
    def __init__(self, x, y, mass):
        super().__init__(x, y)
        self.mass = mass

    def energy(self):
        return self.mass * self.dot(self)


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def calls(n):
    def inc(v):
        return v + 1
    s = 0
    for i in range(n):
        s = inc(s)
    return s


def objects(n):
    acc = Vec(0, 0)
    step = Vec(1, 2)
    total = 0
    for i in range(n):
        acc = acc.add(step)
        total += acc.dot(step) % 7
    return total


def particles(n):
    ps = [Particle(i, i + 1, i % 3 + 1) for i in range(64)]
    e = 0
    for _ in range(n):
        for p in ps:
            p.x = p.x + 1
            e += p.energy() % 11
    return e


def globals_and_branches(n):
    hits = 0
    for i in range(n):
        if i % LIMIT == 0 or i < LIMIT:
            hits += 1
        elif i & 1:
            hits -= 0
    return hits


def containers(n):
    d = {}
    xs = []
    for i in range(n):
        key = i % 97
        d[key] = d.get(key, 0) + i
        xs.append(i * 2)
        if len(xs) > 256:
            xs = xs[128:]
    return sum(xs) + len(d)


def strings(n):
    parts = []
    for i in range(n):
        parts.append(str(i % 1000))
        if len(parts) > 64:
            "".join(parts)
            parts = []
    return len(parts)


def floats(n):
    x = 0.5
    s = 0.0
    for i in range(n):
        x = x * 1.0001 + 0.25
        s += x / (i + 1)
    return s


def exceptions(n):
    caught = 0
    for i in range(n):
        try:
            if i % 5 == 0:
                raise ValueError(i)
            caught += 0
        except ValueError:
            caught += 1
    return caught


def generators(n):
    def gen(k):
        for i in range(k):
            yield i * i
    return sum(gen(n))


for _ in range(20):  # ~2 s with a release build: enough weight next to the test suite
    fib(22)
    calls(200000)
    objects(100000)
    particles(500)
    globals_and_branches(300000)
    containers(100000)
    strings(50000)
    floats(200000)
    exceptions(30000)
    generators(100000)
print("pgo workload: ok")
