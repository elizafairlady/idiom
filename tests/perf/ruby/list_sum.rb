xs = []
100_000.times do |i|
  xs << i
end
acc = 0
xs.reverse_each do |h|
  acc = (acc + h) % 1_000_000_007
end
puts acc
