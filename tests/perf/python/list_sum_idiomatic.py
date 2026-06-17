from functools import reduce


xs = list(range(100_000))
print(reduce(lambda acc, h: (acc + h) % 1_000_000_007, reversed(xs), 0))
