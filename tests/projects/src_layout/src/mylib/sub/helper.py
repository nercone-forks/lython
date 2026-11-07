from .. import VERSION

OFFSET = 1


def twice(n: int) -> int:
    print("helper sees version", VERSION)
    return n * 2
