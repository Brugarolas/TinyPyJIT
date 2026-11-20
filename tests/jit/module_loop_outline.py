# Module-level loops are compiled into synthetic functions so that the register
# VM and the JIT run them. Everything observable must stay module semantics.
import traceback

# Names assigned in the loop are module globals, also after the loop.
total = 0
for i in range(1000):
    total += i
assert total == 499500 and i == 999
assert 'i' in globals()

# while / break / continue / else
n = 0
while True:
    n += 1
    if n % 2 == 0:
        continue
    if n > 50:
        break
else:
    exit(1)
assert n == 51

hit_else = False
for k in range(3):
    pass
else:
    hit_else = True
assert hit_else and k == 2

# Functions defined in a loop are plain module functions: no closure snapshot.
for j in range(2):
    def get_j():
        return j
j = 'rebound'
assert get_j() == 'rebound'

for _ in range(1):
    def fact(x):
        return 1 if x <= 1 else x * fact(x - 1)
assert fact(5) == 120

# Classes defined in a loop
for idx in range(2):
    class Point:
        def __init__(self, x):
            self.x = x
        def double(self):
            return Point(self.x * 2)
assert Point(4).double().x == 8

# locals() / exec / eval behave as at module level
for _ in range(1):
    loc = locals()
    exec('made_by_exec = 42')
    seen = eval('made_by_exec + 1')
assert seen == 43 and made_by_exec == 42
assert isinstance(loc, dict)

# Exceptions raised in a loop and caught at module level
caught = None
try:
    for v in range(10):
        if v == 7:
            raise ValueError('seven')
except ValueError as e:
    caught = str(e)
assert caught == 'seven' and v == 7

try:
    for v in range(3):
        assert v < 2, 'too big %d' % v
except AssertionError as e:
    caught = str(e)
assert caught == 'too big 2'

# A traceback shows the module frame once, at the failing line.
try:
    for w in range(2):
        if w == 1:
            {}['missing']
except KeyError:
    text = traceback.format_exc()
assert text.count('module_loop_outline.py') == 1, text
assert 'line 81' in text, text

# Module-level loops still reject function-only statements.
for src in ['for x in range(3):\n    return 1\n', 'while True:\n    yield 1\n']:
    try:
        exec(src)
        exit(1)
    except SyntaxError:
        pass

# Hot loop: exercises native code with global loads and stores.
acc = 0
for q in range(200000):
    acc = (acc + q * 3) % 1000003
def reference():
    r = 0
    for q in range(200000):
        r = (r + q * 3) % 1000003
    return r
assert acc == reference()

print('ok')
