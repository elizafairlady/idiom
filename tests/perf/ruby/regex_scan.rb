rx = /([A-Za-z]+|_+)[0-9]{2,3}/
acc = 0
200_000.times do
  acc += 1 if rx =~ "xxab123--__99--zz7"
end
puts acc
