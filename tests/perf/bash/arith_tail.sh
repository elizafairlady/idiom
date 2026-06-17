acc=0
i=0
while (( i < 500000 )); do
  acc=$(( (acc + (i * 3) + 7) % 1000000007 ))
  i=$(( i + 1 ))
done
printf '%s\n' "$acc"
