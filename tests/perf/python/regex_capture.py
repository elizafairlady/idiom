import re

rx = re.compile(r"([A-Za-z]+|_+)[0-9]{2,3}")
s = "xxab123--__99--zz7"
acc = 0
for _ in range(200_000):
    m = rx.search(s)
    acc += len(m.group(0))
print(acc)
