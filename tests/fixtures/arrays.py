import gc
import numpy as np
import arrays as m

a = np.arange(12, dtype=np.float64).reshape(3, 4)
for view in (a, a.T, a[::-1, ::-1], np.asfortranarray(a), a[:0]):
    assert m.sum(view) == float(view.sum())
view = a[::2, ::2]
m.scale(view, 2)
assert a.tolist() == [[0, 1, 4, 3], [4, 5, 6, 7], [16, 9, 20, 11]]
a.setflags(write=False)
assert m.sum(a) == a.sum()
try:
    m.sum(np.arange(6, dtype=np.int32).reshape(2, 3))
except TypeError:
    pass
else:
    raise AssertionError('dtype conversion despite noconvert')

assert m.alive() == 0
owned = m.make()
assert owned.shape == (2, 3) and owned.dtype == np.float64
assert m.alive() == 1
alias = owned[:, ::2]
buffer = memoryview(owned)
del owned
gc.collect()
assert m.alive() == 1
alias[1, 1] = 42
assert buffer[1, 2] == 42
del alias, buffer
gc.collect()
assert m.alive() == 0
