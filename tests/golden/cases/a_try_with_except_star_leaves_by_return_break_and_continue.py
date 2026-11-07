# Why execution: which path a return, break or continue in the body of a
# try/except* takes -- past the handlers, through a finally, out of a loop --
# and what value it carries are only observable by running it.
from typing import Literal


def pick(n: int) -> int:
    try:
        if n < 0:
            raise ExceptionGroup("neg", [ValueError("v")])
        if n == 0:
            return 100
        print("body done", n)
    except* ValueError as eg:
        print("handled", len(eg.exceptions))
    return n * 2


def name(flag: bool) -> str:
    try:
        if flag:
            return "early"
    except* KeyError:
        print("key")
    return "late"


def nothing(flag: bool) -> None:
    try:
        if flag:
            print("returning")
            return
        raise ExceptionGroup("g", [TypeError("t")])
    except* TypeError:
        print("type")
    print("after")


def loop() -> int:
    total = 0
    for i in range(10):
        try:
            if i == 2:
                continue
            if i == 6:
                break
            if i == 4:
                raise ExceptionGroup("g", [ValueError("x")])
            print("i", i)
        except* ValueError:
            print("caught at", i)
        total = total + i
    return total


def with_finally(n: int) -> int:
    try:
        if n > 0:
            return n
        raise ExceptionGroup("g", [OSError("o")])
    except* OSError:
        print("os")
    finally:
        print("finally", n)
    return -1


def nested_loop() -> int:
    try:
        for j in range(5):
            if j == 3:
                return j
    except* ValueError:
        pass
    return -1


def handler_loop() -> int:
    count = 0
    try:
        raise ExceptionGroup("g", [ValueError("a")])
    except* ValueError:
        for k in range(3):
            if k == 1:
                break
            print("k", k)
    return count


def rest(n: int) -> int:
    try:
        if n != 0:
            raise ExceptionGroup("g", [ValueError("a"), KeyError("b")])
        return 1
    except* ValueError:
        print("value part")
    return 0


def maybe(n: int) -> int | None:
    try:
        if n != 0:
            return n
    except* ValueError:
        pass
    return None


class Box:
    def get(self, n: int) -> int:
        try:
            if n != 0:
                return n + 1
        except* ValueError:
            pass
        return 0


print(pick(0), pick(3), pick(-1))
print(name(True), name(False))
nothing(True)
nothing(False)
print(loop())
print(with_finally(5), with_finally(0))
print(nested_loop())
print(handler_loop())
print(rest(0))
try:
    rest(1)
except* KeyError as e:
    print("outer key", len(e.exceptions))
print(maybe(0), maybe(7))
print(Box().get(1), Box().get(0))
class P:
    def __init__(self, v: int) -> None:
        self.v = v


def lst(n: int) -> list[int]:
    try:
        if n > 0:
            return [n, n]
        raise ExceptionGroup("g", [ValueError("v")])
    except* ValueError:
        print("v")
    return []


def obj(n: int) -> P:
    try:
        if n > 0:
            return P(n)
    except* ValueError:
        pass
    return P(-1)


def nested(n: int) -> int:
    try:
        try:
            if n == 1:
                return 10
            if n == 2:
                raise ExceptionGroup("g", [KeyError("k")])
        except* ValueError:
            print("inner v")
        if n == 3:
            return 30
    except* KeyError:
        print("outer k")
    return 0


class Guard:
    def __enter__(self) -> "Guard":
        print("enter")
        return self

    def __exit__(self, *args: object) -> None:
        print("exit")


def in_with(n: int) -> str:
    try:
        with Guard():
            if n > 0:
                return "w" + str(n)
    except* OSError:
        print("os")
    return "none"


def raising_return(n: int) -> int:
    try:
        return 10 // n
    except* ZeroDivisionError:
        print("zero")
    return -1


print(lst(2), lst(0))
print(obj(4).v, obj(0).v)
print(nested(1), nested(2), nested(3), nested(4))
print(in_with(1), in_with(0))
print(raising_return(5), raising_return(0))

for i in range(5):
    try:
        if i == 1:
            continue
        if i == 3:
            break
        print("module", i)
    except* ValueError:
        pass
print("end")

j = 0
while True:
    try:
        print("w", j)
        break
    except* ValueError:
        pass
print("while done")


def loop_else() -> int:
    seen = 0
    for i in range(3):
        try:
            for j in range(2):
                if i == 0:
                    break
            else:
                break
            print("inner broke at", i)
        except* ValueError:
            pass
    return seen


def literal(n: int) -> Literal[3]:
    try:
        if n > 0:
            return 3
    except* ValueError:
        pass
    return 3


print(loop_else())
print(literal(1), literal(0))
