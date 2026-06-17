xs = Enum.to_list(0..99_999)
acc = Enum.reduce(Enum.reverse(xs), 0, fn h, acc -> rem(acc + h, 1_000_000_007) end)
IO.puts(acc)
