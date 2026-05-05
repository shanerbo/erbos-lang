# Erbos VS Code Extension

Syntax highlighting for `.erbos` files.

## Install

```bash
# Symlink into VS Code extensions directory
ln -s $(pwd) ~/.vscode/extensions/erbos-lang
```

Then reload VS Code (`Cmd+Shift+P` → "Reload Window").

## Features

- Syntax highlighting for all keywords, operators, types
- String interpolation highlighting (`{var}` inside strings)
- Comment highlighting (// and /* */)
- Auto-closing brackets and quotes
- Auto-indentation on `{`

## Highlighted elements

| Element | Color category |
|---------|---------------|
| `spark`, `give`, `through`, `infi`, `stop`, `skip`, `nah` | Control keywords |
| `is`, `be`, `nomut`, `now`, `rep`, `ref` | Declaration keywords |
| `and`, `or`, `not` | Logical operators |
| `eq`, `ne`, `gt`, `lt`, `ge`, `le`, `mod` | Comparison operators |
| `int`, `str`, `bool`, `void`, `list`, `map`, `task` | Types |
| `yell`, `len`, `str_concat` | Built-in functions |
| `Point`, `Node` (PascalCase) | Struct/type names |
| `"hello {name}"` | Strings with interpolation |
| `true`, `false` | Boolean constants |
| `// comment` | Comments |
