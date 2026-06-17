rx = ~r/([A-Za-z]+|_+)[0-9]{2,3}/
s = "xxab123--__99--zz7"
acc = Enum.reduce(1..200_000, 0, fn _, acc ->
  [m | _] = Regex.run(rx, s)
  acc + byte_size(m)
end)
IO.puts(acc)
