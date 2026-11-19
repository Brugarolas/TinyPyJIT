import sys
import time
t0 = time.perf_counter()     # 1) start

class vec2:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __add__(self, other):
        return vec2(self.x + other.x, self.y + other.y)

    def __eq__(self, other):
        return self.x == other.x and self.y == other.y
    
    def __ne__(self, other):
        return not self == other

x = vec2(0, 0)
for i in range(10000000):
    x += vec2(1, 1)

assert x == vec2(10000000, 10000000)

t1 = time.perf_counter()     # 2) end
dt = t1 - t0                 # 3) difference
secs = dt                    # 4) already seconds

print(f"Time = {secs:.6f} s")  # 5) print
