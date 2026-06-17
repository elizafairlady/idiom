from functools import reduce


def step(acc, i):
    return (acc + (i * 3) + 7) % 1_000_000_007


print(reduce(step, range(500_000), 0))
