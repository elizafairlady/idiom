xs = (0...100_000).to_a
puts(xs.reverse_each.reduce(0) { |acc, h| (acc + h) % 1_000_000_007 })
