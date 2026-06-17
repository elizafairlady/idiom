defmodule Bench do
  def classify({0, 0, x}), do: x + 1
  def classify({0, 1, x}), do: x + 2
  def classify({0, 2, x}), do: x + 3
  def classify({0, 3, x}), do: x + 4
  def classify({0, 4, x}), do: x + 5
  def classify({0, 5, x}), do: x + 6
  def classify({0, 6, x}), do: x + 7
  def classify({0, 7, x}), do: x + 8
  def classify({1, 0, x}), do: x + 9
  def classify({1, 1, x}), do: x + 10
  def classify({1, 2, x}), do: x + 11
  def classify({1, 3, x}), do: x + 12
  def classify({1, 4, x}), do: x + 13
  def classify({1, 5, x}), do: x + 14
  def classify({1, 6, x}), do: x + 15
  def classify({1, 7, x}), do: x + 16
  def classify({2, 0, x}), do: x + 17
  def classify({2, 1, x}), do: x + 18
  def classify({2, 2, x}), do: x + 19
  def classify({2, 3, x}), do: x + 20
  def classify({2, 4, x}), do: x + 21
  def classify({2, 5, x}), do: x + 22
  def classify({2, 6, x}), do: x + 23
  def classify({2, 7, x}), do: x + 24
  def classify({3, 0, x}), do: x + 25
  def classify({3, 1, x}), do: x + 26
  def classify({3, 2, x}), do: x + 27
  def classify({3, 3, x}), do: x + 28
  def classify({3, 4, x}), do: x + 29
  def classify({3, 5, x}), do: x + 30
  def classify({3, 6, x}), do: x + 31
  def classify({3, 7, x}), do: x + 32
  def classify({4, 0, x}), do: x + 33
  def classify({4, 1, x}), do: x + 34
  def classify({4, 2, x}), do: x + 35
  def classify({4, 3, x}), do: x + 36
  def classify({4, 4, x}), do: x + 37
  def classify({4, 5, x}), do: x + 38
  def classify({4, 6, x}), do: x + 39
  def classify({4, 7, x}), do: x + 40
  def classify({5, 0, x}), do: x + 41
  def classify({5, 1, x}), do: x + 42
  def classify({5, 2, x}), do: x + 43
  def classify({5, 3, x}), do: x + 44
  def classify({5, 4, x}), do: x + 45
  def classify({5, 5, x}), do: x + 46
  def classify({5, 6, x}), do: x + 47
  def classify({5, 7, x}), do: x + 48
  def classify({6, 0, x}), do: x + 49
  def classify({6, 1, x}), do: x + 50
  def classify({6, 2, x}), do: x + 51
  def classify({6, 3, x}), do: x + 52
  def classify({6, 4, x}), do: x + 53
  def classify({6, 5, x}), do: x + 54
  def classify({6, 6, x}), do: x + 55
  def classify({6, 7, x}), do: x + 56
  def classify({7, 0, x}), do: x + 57
  def classify({7, 1, x}), do: x + 58
  def classify({7, 2, x}), do: x + 59
  def classify({7, 3, x}), do: x + 60
  def classify({7, 4, x}), do: x + 61
  def classify({7, 5, x}), do: x + 62
  def classify({7, 6, x}), do: x + 63
  def classify({7, 7, x}), do: x + 64
  def classify(_), do: 0

  def loop(i, n, acc) when i >= n, do: acc
  def loop(i, n, acc) do
    a = rem(i, 8)
    b = rem(div(i, 8), 8)
    loop(i + 1, n, rem(acc + classify({a, b, i}), 1_000_000_007))
  end
end

IO.puts(Bench.loop(0, 100_000, 0))
