defmodule Bench do
  def loop(i, n, acc) when i >= n, do: acc
  def loop(i, n, acc), do: loop(i + 1, n, rem(acc + i * 3 + 7, 1_000_000_007))
end

IO.puts(Bench.loop(0, 500_000, 0))
