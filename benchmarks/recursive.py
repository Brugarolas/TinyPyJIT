import time
t0 = time.perf_counter()     # 1) start

def f(n):
    if n == 900:
        return -1
    return f(n + 1)

assert f(0) == -1

t1 = time.perf_counter()     # 2) end
dt = t1 - t0                 # 3) difference
secs = dt                    # 4) already seconds

print(f"Time = {secs:.6f} s")  # 5) print