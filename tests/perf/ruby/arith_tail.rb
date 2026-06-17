acc = 0
500_000.times do |i|
  acc = (acc + (i * 3) + 7) % 1_000_000_007
end
puts acc
