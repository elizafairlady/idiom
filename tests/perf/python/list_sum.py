xs = []
for i in range(100_000):
    xs.append(i)
acc = 0
for h in reversed(xs):
    acc = (acc + h) % 1_000_000_007
print(acc)
