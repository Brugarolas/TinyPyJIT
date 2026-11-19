import time

t0 = time.perf_counter()     # 1) start

for i in range(10000000):
    if i > 0:
        pass

t1 = time.perf_counter()     # 2) end
dt = t1 - t0                 # 3) difference
secs = dt                    # 4) already seconds

print(f"Time = {secs:.6f} s")  # 5) print
