# `for k, v in <iterable>` fuses the iterator step with the 2-way unpack in
# native code; dict.items() then yields pairs without allocating tuples.

def sum_items(d):
    total = 0
    keys = []
    for k, v in d.items():
        total += v
        keys.append(k)
    return total, keys

def sum_pairs(seq):
    total = 0
    for a, b in seq:
        total += a * b
    return total

d = {i: i * 2 for i in range(100)}
for _ in range(30):
    total, keys = sum_items(d)
    assert total == 9900 and keys == list(range(100))
    assert sum_pairs([(1, 2), (3, 4), [5, 6]]) == 44
    assert sum_pairs(zip([1, 2, 3], [4, 5, 6])) == 32
    assert sum_items({}) == (0, [])
    assert sum_items({'a': 1, 'b': 2}) == (3, ['a', 'b'])

# Insertion order is kept, also after deletions.
e = {k: k for k in range(10)}
del e[3]
del e[7]
assert sum_items(e)[1] == [0, 1, 2, 4, 5, 6, 8, 9]

# Wrong arity still raises ValueError.
try:
    sum_pairs([(1, 2), (1, 2, 3)])
    exit(1)
except ValueError:
    pass

# Modifying the dict while iterating raises RuntimeError.
def mutate(d):
    for k, v in d.items():
        d[k + 1000] = v

for _ in range(20):
    try:
        mutate({1: 1, 2: 2})
        exit(1)
    except RuntimeError:
        pass

# Module-level loop (outlined): globals are assigned.
for name, value in {'x': 1, 'y': 2}.items():
    pass
assert name == 'y' and value == 2
print('ok')
