import gc
import buffer_exports as m

for reverse in (False, True):
    owner = m.Matrix(False, reverse)
    view = memoryview(owner)
    assert view.shape == (2, 3) and view.itemsize == 8 and view.format == 'd'
    assert view.strides == ((-24, -8) if reverse else (24, 8))
    assert view.tolist() == ([[5, 4, 3], [2, 1, 0]] if reverse else [[0, 1, 2], [3, 4, 5]])
    view[0, 0] = 42
    assert owner.get(5 if reverse else 0) == 42
    m.writable(owner)
    assert m.layout(owner, 2) == (2, 48, 8, True, True, True)
    if not reverse:
        m.contiguous(owner)
        assert m.layout(owner, 0) == (0, 48, 8, False, False, False)
        assert m.layout(owner, 1) == (2, 48, 8, True, False, False)
        for mode in (4, 5):
            assert m.layout(owner, mode) == (2, 48, 8, True, True, False)
        try:
            m.layout(owner, 3)
        except BufferError:
            pass
        else:
            raise AssertionError('row-major exported as column-major')
    else:
        try:
            m.contiguous(owner)
        except BufferError:
            pass
        else:
            raise AssertionError('noncontiguous SIMPLE buffer accepted')
    del owner
    gc.collect()
    assert m.alive() == 1 and view[0, 0] == 42
    view.release()
    del view
    gc.collect()
    assert m.alive() == 0

owner = m.Matrix(True, False)
view = memoryview(owner)
assert view.readonly
try:
    view[0, 0] = 1
except TypeError:
    pass
else:
    raise AssertionError('writable readonly view')
try:
    m.writable(owner)
except BufferError:
    pass
else:
    raise AssertionError('WRITABLE request accepted')
del view, owner
gc.collect()
assert m.alive() == 0
