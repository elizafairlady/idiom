xs=()
i=0
while (( i < 100000 )); do
  xs+=("$i")
  i=$(( i + 1 ))
done
acc=0
i=99999
while (( i >= 0 )); do
  acc=$(( (acc + xs[i]) % 1000000007 ))
  i=$(( i - 1 ))
done
printf '%s\n' "$acc"
