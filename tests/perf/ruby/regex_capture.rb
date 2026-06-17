rx = /([A-Za-z]+|_+)[0-9]{2,3}/
s = "xxab123--__99--zz7"
acc = 0
200_000.times do
  m = rx.match(s)
  acc += m[0].length
end
puts acc
