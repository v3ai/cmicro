"""Write mids.txt: decimal strings exactly halfway between adjacent doubles
(and just above/below), the hardest inputs for correctly rounded strtod."""
import math, random, struct, sys
from decimal import Decimal, getcontext
getcontext().prec = 2000
random.seed(5)
out = []
for i in range(3000):
    u = random.getrandbits(63) & 0x7FEFFFFFFFFFFFFF
    if i % 10 == 0:
        u = random.getrandbits(52)  # subnormals
    x = struct.unpack("<d", struct.pack("<Q", u))[0]
    mid = (Decimal(x) + Decimal(math.nextafter(x, math.inf))) / 2
    out.append(format(mid, "f") if i % 2 else format(mid, "e"))
    out.append(format(mid + Decimal(10) ** (mid.adjusted() - 900), "e"))
    out.append(format(mid - Decimal(10) ** (mid.adjusted() - 900), "e"))
out += ["1.7976931348623158e308", "1.7976931348623157e308", "1.797693134862315807937e308",
        "2.4703282292062327e-324", "2.4703282292062328e-324", "4.9406564584124654e-324",
        "2.2250738585072011e-308", "2.2250738585072012e-308", "9007199254740993",
        "9007199254740992.5", "0.1e-9999", "1e99999"]
open(sys.argv[1] if len(sys.argv) > 1 else "mids.txt", "w").write("\n".join(out) + "\n")
