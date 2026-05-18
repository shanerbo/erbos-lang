# Potato Examples 🥔

Runnable example programs live in
[`../examples/`](../examples/). Each file is a self-contained
program with a `spark { }` entry point; run any of them with:

```bash
./erbos run examples/<name>.ptt
```

| File | What it shows |
|------|---------------|
| `examples/hello.ptt` | minimal entry point + `yell` |
| `examples/fizzbuzz.ptt` | `through` range loop + `?{ } nah { }` chain |
| `examples/concat.ptt` | string interpolation, `+`, methods |
| `examples/loop.ptt` | `infi` + `stop` / `skip` |
| `examples/recursion.ptt` | recursive `give` |
| `examples/funcs.ptt` | free functions, parameters, return types |
| `examples/methods.ptt` | `Type.method(self ref Type, ...)` |
| `examples/struct_func.ptt` | struct construction + factory function |
| `examples/list_methods.ptt` | `List of T` push / pop / iter |
| `examples/map_methods.ptt`, `map_iter.ptt`, `map_update.ptt` | `Map of K, V` |
| `examples/move_valid.ptt`, `give_ownership.ptt` | move semantics |
| `examples/linked.ptt` | self-referential struct |
| `examples/interp.ptt` | string interpolation deep dive |
| `examples/kitchen_sink.ptt` | end-to-end demo touching most features |
| `examples/task_test.ptt` | `task` placeholder (runtime not yet wired) |

The narrative reference for each language feature is in
[`language-guide.md`](language-guide.md). The canonical grammar
for value formation is [`language-law.md`](language-law.md).
