import gc
import semantics as m


def rejects(error, fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except error:
        return
    raise AssertionError(f"expected {error.__name__}")


if scenario == 'binding_arguments':
    assert m.parameters(1) == 321
    assert m.parameters(1, c=4, b=5) == 451
    rejects(TypeError, m.parameters, a=1)
    rejects(TypeError, m.parameters, 1, 2, 3)
    rejects(TypeError, m.parameters, 1, unknown=3)
    rejects(TypeError, m.parameters, 1, 2, b=3)
    assert m.overload(42) == 'integer'
    assert m.overload('42') == 'text'
    rejects(TypeError, m.overload, object())
    assert m.strict(42) == 42
    rejects(TypeError, m.strict, 4.2)
    assert m.args(1, 2, x=3) == (2, 1)
elif scenario == 'binding_containers':
    assert m.containers({'a': [1, 2], 'b': []}) == {'a': [1, 2], 'b': []}
    rejects(TypeError, m.containers, {'a': [object()]})
    assert m.optional(None) is None
    assert m.optional(42) == 42
    assert m.variant(42) == 42
    assert m.variant('text') == 'text'
    assert m.tuple((42, 'text')) == (42, 'text')
    rejects(TypeError, m.tuple, (1,))
elif scenario == 'binding_inheritance':
    class Cat(m.Animal):
        def speak(self):
            return 'meow'

    class Broken(m.Animal):
        def speak(self):
            raise LookupError('override')

    assert isinstance(m.Dog(), m.Animal)
    assert m.speak(m.Dog()) == 'woof'
    assert m.speak(Cat()) == 'meow'
    assert m.nullable(None) == 'none'
    rejects(TypeError, m.required, None)
    rejects(LookupError, m.speak, Broken())
    rejects(RuntimeError, m.speak, m.Animal())
elif scenario == 'binding_lifetimes':
    assert m.alive() == 0
    parent = m.Parent()
    child = parent.get()
    assert parent.get() is child
    del parent
    gc.collect()
    assert m.alive() == 1 and child.get() == 42
    del child
    gc.collect()
    assert m.alive() == 0
    keeper = m.Keeper()
    item = m.Item(42)
    keeper.set(item)
    del item
    gc.collect()
    assert m.alive() == 1 and keeper.get() == 42
    del keeper
    gc.collect()
    assert m.alive() == 0
elif scenario == 'binding_unique':
    assert m.unique_alive() == 0
    obj = m.make_unique()
    assert obj.get() == 42 and m.unique_alive() == 1
    assert m.consume(obj) == 42
    rejects(ValueError, obj.get)
    del obj
    gc.collect()
    assert m.unique_alive() == 0
    obj = m.Unique(21)
    assert m.consume(obj) == 21
    del obj
    gc.collect()
    assert m.unique_alive() == 0
elif scenario == 'binding_exceptions':
    assert issubclass(m.DomainError, ValueError)
    for kind, error in enumerate((m.DomainError, ValueError, IndexError, MemoryError)):
        rejects(error, m.fail, kind)
    assert m.parameters(1) == 321
elif scenario == 'free_threaded':
    import sys
    import threading
    assert not sys._is_gil_enabled()
    errors = []

    def worker():
        try:
            m.meet()
            for i in range(1000):
                assert m.parameters(i) == i + 320
                assert m.speak(m.Dog()) == 'woof'
                assert m.containers({'x': [i]}) == {'x': [i]}
        except BaseException as error:
            errors.append(error)

    workers = [threading.Thread(target=worker) for _ in range(2)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    assert not errors, errors
    assert not sys._is_gil_enabled()
else:
    raise AssertionError(f'unknown scenario: {scenario}')
