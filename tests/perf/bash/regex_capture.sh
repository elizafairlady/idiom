rx='([[:alpha:]]+|_+)[0-9]{2,3}'
s='xxab123--__99--zz7'
acc=0
i=0
while (( i < 200000 )); do
  if [[ "$s" =~ $rx ]]; then
    acc=$(( acc + ${#BASH_REMATCH[0]} ))
  fi
  i=$(( i + 1 ))
done
printf '%s\n' "$acc"
