acc = 0
for i in range(500_000):
    acc = (acc + (i * 3) + 7) % 1_000_000_007
print(acc)
