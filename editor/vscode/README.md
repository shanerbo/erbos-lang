# Potato VS Code Extension 🥔

Syntax highlighting for `.ptt` (potato) files.

## Install

```bash
# Symlink into VS Code extensions directory
ln -s $(pwd) ~/.vscode/extensions/erbos-lang
```

Then reload VS Code (`Cmd+Shift+P` → "Reload Window").

## Features

- Syntax highlighting for all keywords, operators, types
- String interpolation highlighting (`{var}` inside strings)
- Comment highlighting (`//` and `/* */`)
- Auto-closing brackets and quotes
- Auto-indentation on `{`

## Highlighted elements

| Element | Color category |
|---------|---------------|
| `spark`, `give`, `through`, `from`, `to`, `by`, `in`, `infi`, `stop`, `skip`, `nah`, `match`, `test` | Control keywords |
| `is`, `be`, `nomut`, `now`, `rep`, `ref`, `use`, `as`, `of`, `with`, `cap` | Declaration keywords |
| `and`, `or`, `not` | Logical operators |
| `eq`, `ne`, `gt`, `lt`, `ge`, `le`, `mod` | Comparison operators |
| `int`, `bool`, `byte`, `void`, `array`, `task` | Primitive types |
| `String`, `List`, `Map` | Stdlib types |
| `yell`, `assert` | Built-in functions |
| `.push`, `.pop`, `.get`, `.set`, `.keys`, `.len`, `.byte_at`, `.char_at`, `.to_string`, `.equals`, `.empty`, `.concat` | Stdlib methods |
| `Point`, `Node` (PascalCase) | Struct / enum names |
| `"hello {name}"` | Strings with interpolation |
| `true`, `false`, `nil` | Constants |
| `// comment` | Comments |
