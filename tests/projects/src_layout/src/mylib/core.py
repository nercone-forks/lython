import mylib.sub.helper
from mylib.sub.helper import twice


def compute(n: int) -> int:
    return twice(n) + mylib.sub.helper.OFFSET
