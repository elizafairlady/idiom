def classify(value):
    match value:
        case (0, 0, x): return x + 1
        case (0, 1, x): return x + 2
        case (0, 2, x): return x + 3
        case (0, 3, x): return x + 4
        case (0, 4, x): return x + 5
        case (0, 5, x): return x + 6
        case (0, 6, x): return x + 7
        case (0, 7, x): return x + 8
        case (1, 0, x): return x + 9
        case (1, 1, x): return x + 10
        case (1, 2, x): return x + 11
        case (1, 3, x): return x + 12
        case (1, 4, x): return x + 13
        case (1, 5, x): return x + 14
        case (1, 6, x): return x + 15
        case (1, 7, x): return x + 16
        case (2, 0, x): return x + 17
        case (2, 1, x): return x + 18
        case (2, 2, x): return x + 19
        case (2, 3, x): return x + 20
        case (2, 4, x): return x + 21
        case (2, 5, x): return x + 22
        case (2, 6, x): return x + 23
        case (2, 7, x): return x + 24
        case (3, 0, x): return x + 25
        case (3, 1, x): return x + 26
        case (3, 2, x): return x + 27
        case (3, 3, x): return x + 28
        case (3, 4, x): return x + 29
        case (3, 5, x): return x + 30
        case (3, 6, x): return x + 31
        case (3, 7, x): return x + 32
        case (4, 0, x): return x + 33
        case (4, 1, x): return x + 34
        case (4, 2, x): return x + 35
        case (4, 3, x): return x + 36
        case (4, 4, x): return x + 37
        case (4, 5, x): return x + 38
        case (4, 6, x): return x + 39
        case (4, 7, x): return x + 40
        case (5, 0, x): return x + 41
        case (5, 1, x): return x + 42
        case (5, 2, x): return x + 43
        case (5, 3, x): return x + 44
        case (5, 4, x): return x + 45
        case (5, 5, x): return x + 46
        case (5, 6, x): return x + 47
        case (5, 7, x): return x + 48
        case (6, 0, x): return x + 49
        case (6, 1, x): return x + 50
        case (6, 2, x): return x + 51
        case (6, 3, x): return x + 52
        case (6, 4, x): return x + 53
        case (6, 5, x): return x + 54
        case (6, 6, x): return x + 55
        case (6, 7, x): return x + 56
        case (7, 0, x): return x + 57
        case (7, 1, x): return x + 58
        case (7, 2, x): return x + 59
        case (7, 3, x): return x + 60
        case (7, 4, x): return x + 61
        case (7, 5, x): return x + 62
        case (7, 6, x): return x + 63
        case (7, 7, x): return x + 64
        case _: return 0


acc = 0
for i in range(100_000):
    a = i % 8
    b = (i // 8) % 8
    acc = (acc + classify((a, b, i))) % 1_000_000_007
print(acc)
