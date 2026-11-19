# Functions created by exec() with a plain dict as globals: their frames have
# no module namespace, so the global inline cache can never be filled. Native
# code must fall back instead of trusting the zeroed cache (this used to crash),
# and native code for functions that die must be released safely.
import gc

src = '''
def hot(n):
    s = 0
    for i in range(n):
        s += helper(i)
    return s


def helper(x):
    return x + K
'''

total = 0
for k in range(60):
    g = {'K': k}
    exec(src, g)
    for _ in range(3):
        total += g['hot'](200)
    g = None
    if k % 20 == 0:
        gc.collect()
gc.collect()

expected = 0
for k in range(60):
    expected += 3 * (sum(range(200)) + 200 * k)
assert total == expected, total

# Rebinding a name in the plain-dict globals is visible to the next call.
g = {'K': 1}
exec(src, g)
for _ in range(40):
    assert g['hot'](10) == sum(range(10)) + 10
g['K'] = 5
assert g['hot'](10) == sum(range(10)) + 50
del g['K']
try:
    g['hot'](1)
    assert False, "expected NameError"
except NameError:
    pass

print("exec_globals ok")
