"""Non-compiled child class — tests attribute override visibility."""

from test_subclass_base import Base


class Sub(Base):
    MAPPING = {"a": 1, "b": 2, "c": 3}  # Full dict, not spread from parent
    FLAG = True


def test() -> None:
    b = Base()
    assert b.lookup("a") == 1
    assert b.lookup("c") == -1
    assert not b.check_flag()
    print("Base OK")

    s = Sub()
    assert s.lookup("a") == 1
    result = s.lookup("c")
    assert (
        result == 3
    ), f"Expected 3, got {result} — compiled method doesn't see subclass override!"
    assert s.check_flag(), "Expected True — compiled method doesn't see subclass override!"
    print("Sub OK — interpreted subclass overrides work!")


if __name__ == "__main__":
    test()
