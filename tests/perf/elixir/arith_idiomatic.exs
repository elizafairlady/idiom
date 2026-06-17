0..499_999
|> Enum.reduce(0, fn i, acc -> rem(acc + (i * 3) + 7, 1_000_000_007) end)
|> IO.puts()
