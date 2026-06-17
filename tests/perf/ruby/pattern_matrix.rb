def classify(value)
  case value
  in [0, 0, x] then x + 1
  in [0, 1, x] then x + 2
  in [0, 2, x] then x + 3
  in [0, 3, x] then x + 4
  in [0, 4, x] then x + 5
  in [0, 5, x] then x + 6
  in [0, 6, x] then x + 7
  in [0, 7, x] then x + 8
  in [1, 0, x] then x + 9
  in [1, 1, x] then x + 10
  in [1, 2, x] then x + 11
  in [1, 3, x] then x + 12
  in [1, 4, x] then x + 13
  in [1, 5, x] then x + 14
  in [1, 6, x] then x + 15
  in [1, 7, x] then x + 16
  in [2, 0, x] then x + 17
  in [2, 1, x] then x + 18
  in [2, 2, x] then x + 19
  in [2, 3, x] then x + 20
  in [2, 4, x] then x + 21
  in [2, 5, x] then x + 22
  in [2, 6, x] then x + 23
  in [2, 7, x] then x + 24
  in [3, 0, x] then x + 25
  in [3, 1, x] then x + 26
  in [3, 2, x] then x + 27
  in [3, 3, x] then x + 28
  in [3, 4, x] then x + 29
  in [3, 5, x] then x + 30
  in [3, 6, x] then x + 31
  in [3, 7, x] then x + 32
  in [4, 0, x] then x + 33
  in [4, 1, x] then x + 34
  in [4, 2, x] then x + 35
  in [4, 3, x] then x + 36
  in [4, 4, x] then x + 37
  in [4, 5, x] then x + 38
  in [4, 6, x] then x + 39
  in [4, 7, x] then x + 40
  in [5, 0, x] then x + 41
  in [5, 1, x] then x + 42
  in [5, 2, x] then x + 43
  in [5, 3, x] then x + 44
  in [5, 4, x] then x + 45
  in [5, 5, x] then x + 46
  in [5, 6, x] then x + 47
  in [5, 7, x] then x + 48
  in [6, 0, x] then x + 49
  in [6, 1, x] then x + 50
  in [6, 2, x] then x + 51
  in [6, 3, x] then x + 52
  in [6, 4, x] then x + 53
  in [6, 5, x] then x + 54
  in [6, 6, x] then x + 55
  in [6, 7, x] then x + 56
  in [7, 0, x] then x + 57
  in [7, 1, x] then x + 58
  in [7, 2, x] then x + 59
  in [7, 3, x] then x + 60
  in [7, 4, x] then x + 61
  in [7, 5, x] then x + 62
  in [7, 6, x] then x + 63
  in [7, 7, x] then x + 64
  else 0
  end
end

acc = 0
100_000.times do |i|
  a = i % 8
  b = (i / 8) % 8
  acc = (acc + classify([a, b, i])) % 1_000_000_007
end
puts acc
