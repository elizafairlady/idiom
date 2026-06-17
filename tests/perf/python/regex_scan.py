import re

rx = re.compile(r"([A-Za-z]+|_+)[0-9]{2,3}")
acc = 0
for _ in range(200_000):
    if rx.search("xxab123--__99--zz7"):
        acc += 1
print(acc)
