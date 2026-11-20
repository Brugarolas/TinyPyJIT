# Compare-and-branch and truthiness branches on non-int operands run the
# interpreter's generic operations from native code (no deoptimization).

class Box:
    def __init__(self, v):
        self.v = v
    def __eq__(self, other):
        if isinstance(other, Box):
            return self.v == other.v
        if other == 'boom':
            raise ValueError('eq boom')
        return False
    def __ne__(self, other):
        return not self == other
    def __lt__(self, other):
        return self.v < other.v

class Truthy:
    def __init__(self, n):
        self.n = n
    def __len__(self):
        return self.n

class Explodes:
    def __bool__(self):
        raise RuntimeError('bool boom')

def classify(x):
    if x == None:
        return 'none'
    if x == 3:
        return 'three'
    if x < 10:
        return 'small'
    return 'big'

def truth(x):
    if x:
        return 1
    return 0

def not_truth(x):
    return not x

def box_cmp(a, b):
    if a == b:
        return 'eq'
    if a < b:
        return 'lt'
    return 'ge'

for _ in range(50):
    assert classify(None) == 'none'
    assert classify(3) == 'three'
    assert classify(2.5) == 'small'
    assert classify(10.0) == 'big'
    assert classify(42) == 'big'
    assert truth(None) == 0 and truth(0) == 0 and truth(7) == 1
    assert truth('') == 0 and truth('x') == 1
    assert truth([]) == 0 and truth([1]) == 1
    assert truth(0.0) == 0 and truth(0.5) == 1
    assert truth(Truthy(0)) == 0 and truth(Truthy(2)) == 1
    assert truth(Box(1)) == 1
    assert not_truth(None) is True and not_truth('a') is False
    assert box_cmp(Box(1), Box(1)) == 'eq'
    assert box_cmp(Box(1), Box(2)) == 'lt'
    assert box_cmp(Box(3), Box(2)) == 'ge'

# Exceptions raised by the generic operation leave through the error exit.
for _ in range(20):
    try:
        box_cmp(Box(1), 'boom')
        exit(1)
    except ValueError as e:
        assert str(e) == 'eq boom'
    try:
        truth(Explodes())
        exit(1)
    except RuntimeError as e:
        assert str(e) == 'bool boom'

# Linked-list style code (None checks on attributes) stays native.
class Node:
    def __init__(self, v):
        self.v = v
        self.next = None

def length(head):
    n = 0
    while head:
        n += 1
        head = head.next
    return n

head = None
for i in range(100):
    node = Node(i)
    node.next = head
    head = node
for _ in range(30):
    assert length(head) == 100
print('ok')
