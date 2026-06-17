xs = Enum.to_list(0..99_999)
xs
|> Enum.reverse()
|> Enum.reduce(0, fn h, acc -> rem(acc + h, 1_000_000_007) end)
|> IO.puts()
