import timeit
import add

def bench_isspace():
  for i in range(100):
    add.isspace("   ")

if __name__ == '__main__':
    # For Python>=3.5 one can also write:
    print(timeit.timeit("bench_isspace()", globals=locals()))