parent = self()

acc = Enum.reduce(1..100_000, 0, fn _, acc ->
  spawn(fn -> send(parent, :done) end)
  receive do
    :done -> acc + 1
  end
end)

IO.puts(acc)
