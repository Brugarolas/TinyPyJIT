# Randomized dict operations checked against a simple model (insertion-ordered
# key list + value map built on lists). Covers clustered int keys, strided keys,
# strings, deletions (tombstones), reinsertion and compaction/rehash paths.
import random

random.seed(12345)

class Model:
    def __init__(self):
        self.keys = []
        self.vals = []

    def find(self, k):
        for i in range(len(self.keys)):
            if self.keys[i] == k:
                return i
        return -1

    def set(self, k, v):
        i = self.find(k)
        if i >= 0:
            self.vals[i] = v
        else:
            self.keys.append(k)
            self.vals.append(v)

    def pop(self, k):
        i = self.find(k)
        if i < 0:
            return False
        del self.keys[i]
        del self.vals[i]
        return True

def check(d, m):
    assert len(d) == len(m.keys), (len(d), len(m.keys))
    assert list(d.keys()) == m.keys
    assert list(d.values()) == m.vals

def run(keyspace, rounds):
    d = {}
    m = Model()
    for r in range(rounds):
        op = random.randint(0, 9)
        k = keyspace[random.randint(0, len(keyspace) - 1)]
        if op < 5:
            d[k] = r
            m.set(k, r)
        elif op < 8:
            present = k in d
            assert present == (m.find(k) >= 0)
            if present:
                del d[k]
                assert m.pop(k)
        else:
            assert d.get(k, None) == (m.vals[m.find(k)] if m.find(k) >= 0 else None)
        if r % 97 == 0:
            check(d, m)
    check(d, m)

run(list(range(64)), 3000)                      # dense consecutive ints (one cluster)
run([i * 1024 for i in range(64)], 3000)        # strided ints
run([-i for i in range(64)], 2000)              # negative ints
run(['k' + str(i) for i in range(64)], 2000)    # strings
run([i * 65536 + 7 for i in range(40)], 2000)   # large stride

# Delete everything then reinsert, many times (exercises tombstones + compaction).
keys = list(range(1024))
random.shuffle(keys)
d = {k: k for k in keys}
for rnd in range(20):
    for k in keys:
        del d[k]
    assert len(d) == 0
    for k in keys:
        d[k] = k + rnd
    assert len(d) == 1024
    assert list(d.keys()) == keys
    assert d[keys[5]] == keys[5] + rnd

# Big dict, sequential then sparse deletions.
big = {i: i for i in range(100000)}
for i in range(0, 100000, 3):
    del big[i]
assert len(big) == 66666
assert big[1] == 1 and 3 not in big and big[99998] == 99998
for i in range(0, 100000, 3):
    big[i] = -i
assert len(big) == 100000 and big[3] == -3

print('ok')
