# NepLang

**NepLang** is a small programming language for Nepali developers. Its keywords are
romanized Nepali words for control flow and everyday actions (`vana` = print,
`yedi` = if, `natara` = else, `kaam` = function, `farkau` = return), mixed with plain
English for technical ideas.

```
kaam factorial(n) {
    yedi n <= 1 {
        farkau 1
    }
    farkau n * factorial(n - 1)
}

vana(factorial(6))    # 720
```

NepLang is written in C (C11) with no dependencies. You run a `.np` file the same way you
run a Python file: `neplang file.np` instead of `python file.py`. It has a REPL, a static
checker that catches mistakes before your program runs, a bytecode VM, and it can compile
programs to native executables.

---

## Table of contents

- [Installation](#installation)
  - [Arch Linux / Manjaro / CachyOS](#arch-linux--manjaro--cachyos)
  - [Ubuntu / Debian / Fedora / other Linux](#ubuntu--debian--fedora--other-linux)
  - [Windows](#windows)
  - [macOS](#macos)
  - [Prebuilt binaries](#prebuilt-binaries)
  - [Uninstall](#uninstall)
- [Running a program (like Python)](#running-a-program-like-python)
- [Syntax at a glance](#syntax-at-a-glance)
- [Language guide](#language-guide)
- [Built-in functions and the standard library](#built-in-functions-and-the-standard-library)
- [Error messages](#error-messages)
- [Keyword reference](#keyword-reference)
- [Current limitations](#current-limitations)
- [Development](#development)
- [Project layout](#project-layout)
- [Roadmap](#roadmap)

---

## Installation

Building from source takes a few seconds and always gives you the newest version. All you
need is `git`, `make` and a C compiler (`gcc` or `clang`). After installing, the `neplang`
command works from any folder, just like `python`.

### Arch Linux / Manjaro / CachyOS

```bash
sudo pacman -S --needed base-devel git
git clone https://github.com/charitraa/NepLang.git neplang
cd neplang/packaging/arch
makepkg -si
```

`makepkg -si` builds NepLang, runs its tests, and installs it as a normal pacman package
(`neplang` in `/usr/bin`, plus the man page and examples in `/usr/share/doc/neplang/examples`).
To update later: `git pull`, then run `makepkg -si` again in `packaging/arch`.

Or, without making a package:

```bash
cd neplang
make
sudo make install        # installs to /usr/local
```

### Ubuntu / Debian / Fedora / other Linux

1. Install the tools:

   ```bash
   sudo apt install build-essential git        # Ubuntu / Debian / Linux Mint
   sudo dnf install gcc make git               # Fedora
   ```

2. Download, build and install:

   ```bash
   git clone https://github.com/charitraa/NepLang.git neplang
   cd neplang
   make
   sudo make install
   ```

   `sudo make install` puts `neplang` in `/usr/local/bin` and installs the man page
   (`man neplang`). Without sudo, install into your home folder instead:

   ```bash
   make install PREFIX=$HOME/.local
   ```

   `~/.local/bin` must then be on your `PATH` (add `export PATH="$HOME/.local/bin:$PATH"`
   to `~/.bashrc` or `~/.zshrc`).

3. Check it:

   ```bash
   neplang --version
   neplang examples/hello_calc.np
   ```

### Windows

The easiest way is **MSYS2**, which gives you `gcc` and `make` and builds a normal Windows
`neplang.exe`.

1. Download and install MSYS2 from <https://www.msys2.org> (keep the default folder
   `C:\msys64`).
2. Open **MSYS2 UCRT64** from the Start menu and run:

   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make git
   git clone https://github.com/charitraa/NepLang.git neplang
   cd neplang
   make
   ./bin/neplang examples/hello_calc.np
   ```

3. To use `neplang` from **PowerShell** or **cmd** in any folder, add two folders to your
   `PATH`. Open PowerShell and run (change the first path if you cloned somewhere else; in
   MSYS2 your home folder is `C:\msys64\home\<your name>`):

   ```powershell
   $neplang = "C:\msys64\home\$env:USERNAME\neplang\bin"
   $gcc     = "C:\msys64\ucrt64\bin"
   [Environment]::SetEnvironmentVariable("Path", [Environment]::GetEnvironmentVariable("Path", "User") + ";$neplang;$gcc", "User")
   ```

   Close and reopen PowerShell, then:

   ```powershell
   neplang --version
   neplang hello.np
   ```

   (The `ucrt64\bin` folder provides the C compiler that `neplang build` uses.)

**Already have gcc but no make?** Build from PowerShell or cmd in the `neplang` folder:

```powershell
mkdir bin
gcc -std=c11 -O2 -o bin\neplang.exe src\lexer.c src\value.c src\ast.c src\parser.c src\modules.c src\classes.c src\checker.c src\interpreter.c src\rt.c src\ir.c src\vm.c src\cgen.c src\diagnostics.c src\main.c -lm
bin\neplang.exe examples\hello_calc.np
```

**WSL** (Linux inside Windows) also works: run `wsl --install` in an Administrator
PowerShell, restart, open **Ubuntu**, and follow the [Linux steps](#ubuntu--debian--fedora--other-linux).

### macOS

```bash
xcode-select --install          # installs clang, make and git
git clone https://github.com/charitraa/NepLang.git neplang
cd neplang
make
sudo make install
```

### Prebuilt binaries

Ready-made binaries are on the [Releases page](https://github.com/charitraa/NepLang/releases).
They can be older than the code in the repository, so build from source to get the newest
features.

```bash
curl -fsSL https://raw.githubusercontent.com/charitraa/NepLang/main/install.sh | sh     # Linux / macOS
```

```powershell
irm https://raw.githubusercontent.com/charitraa/NepLang/main/install.ps1 | iex          # Windows PowerShell
```

### Uninstall

```bash
sudo pacman -R neplang          # Arch, if installed with makepkg
sudo make uninstall             # if installed with 'sudo make install' (run in the neplang folder)
make uninstall PREFIX=$HOME/.local
```

On Windows, delete the `neplang` folder and remove it from your `PATH`.

---

## Running a program (like Python)

NepLang works like Python: write code in a file, then run the file.

| Python | NepLang | Does |
|---|---|---|
| `python hello.py` | `neplang hello.np` | run a program |
| `python` | `neplang` | interactive mode (REPL) |
| `python hello.py a b` | `neplang hello.np a b` | pass arguments (read them with `os.args()`) |
| | `neplang check hello.np` | only look for mistakes, don't run |
| | `neplang build hello.np -o hello` | make a native program you can run as `./hello` |
| `python --version` | `neplang --version` | show the version |

NepLang files end in `.np`. Make a file `namaste.np` in any text editor:

```
# my first NepLang program
naam = padha()              # read a line you type
vana("Namaste " + naam)
```

Run it:

```bash
neplang namaste.np
```

```
Sita
Namaste Sita
```

Or try code line by line in the REPL (type `exit` to leave):

```
$ neplang
> x = 10
> x * 2
20
> vana("Namaste")
Namaste
```

Run the examples from the `neplang` folder, e.g. `neplang examples/types.np`.

---

## Syntax at a glance

NepLang next to Python:

| What | Python | NepLang |
|---|---|---|
| print | `print(x)` | `vana(x)` |
| variable | `x = 5` | `x = 5` or, with a type, `int x = 5` |
| constant | | `const PI = 3.14` |
| if / elif / else | `if a > b:` / `elif` / `else:` | `yedi a > b { } natara yedi { } natara { }` |
| while | `while i < 10:` | `jabasamma i < 10 { }` |
| for over a range | `for i in range(0, 10):` | `ko_lagi i in 0..10 { }` |
| for over a list | `for x in items:` | `ko_lagi x in items { }` |
| break / continue | `break` / `continue` | `ruk` / `agadi` |
| function | `def add(a, b): return a + b` | `kaam add(a, b) { farkau a + b }` |
| True / False / None | `True` `False` `None` | `sacho` `jutho` `khali` |
| and / or / not | `and` `or` `not` | `ra` `wa` `hoina` |
| list | `[1, 2, 3]`, `len(a)`, `a.append(4)` | `[1, 2, 3]`, `lambai(a)`, `a.push(4)` |
| input | `input()` | `padha()` |
| f-string | `f"Hi {naam}"` | `"Hi ${naam}"` |
| class | `class Dog(Animal):` | `class Kukur(Janawar) { }` |
| self / constructor | `self`, `__init__` | `yo`, `banau` |
| import | `import math` | `import math` |
| comment | `# ...` | `# ...` |

Blocks use `{ }` instead of indentation, and each statement goes on its own line.

A complete program:

```
import math

kaam factorial(int n) int {
    yedi n <= 1 {
        farkau 1
    }
    farkau n * factorial(n - 1)
}

ko_lagi i in 1..6 {
    vana("${i}! = ${factorial(i)}")
}

ank = [72, 45, 91]
vana("ausat: " + math.round(float(ank.sum()) / ank.length(), 1))

class Manchhe {
    string naam
    int umar

    kaam banau(string naam, int umar) {
        yo.naam = naam
        yo.umar = umar
    }

    kaam bolau() {
        vana("Namaste, ma ${naam} hu, ${umar} barsa.")
    }
}

Manchhe("Sita", 22).bolau()
```

Output:

```
1! = 1
2! = 2
3! = 6
4! = 24
5! = 120
ausat: 69.3
Namaste, ma Sita hu, 22 barsa.
```

### Example programs

Run any of these from the `neplang` folder, e.g. `neplang examples/types.np`:

| File | Shows |
|---|---|
| `examples/hello_calc.np` | Arithmetic and operator precedence |
| `examples/variables.np` | Variables (simple-interest calculation) |
| `examples/control_flow.np` | `yedi`, `jabasamma`, `ko_lagi` (primes below 30) |
| `examples/functions.np` | `kaam` / `farkau`, recursion (factorial, GCD) |
| `examples/arrays.np` | Arrays, `lambai`, looping over an array, a 2D matrix |
| `examples/strings.np` | Strings, joining with `+`, reversing and counting characters |
| `examples/classes.np` | Classes: fields, methods, `banau`, private `_` members |
| `examples/types.np` | Types, `const`, floats, bools, `khali`, inheritance, interfaces, the standard library |

After `makepkg -si` on Arch they are also in `/usr/share/doc/neplang/examples/`.

---

## Language guide

### Comments and statements

Each statement goes on its own line. `#` starts a comment that runs to the end of the line.

```
vana(1)    # this is a comment
vana(2)
```

### Printing: `vana`

`vana(expression)` prints the value followed by a newline.

```
vana(10 + 20 * 3)    # 70
```

### Values

| Kind | Examples | `prakar(x)` |
|---|---|---|
| whole number | `42`, `-7` | `int` |
| decimal number | `3.14`, `2.0`, `1e3` | `float` |
| bool | `sacho` (true), `jutho` (false) | `bool` |
| string | `"Namaste"` | `string` |
| array | `[1, 2, 3]` | `array` |
| object | `Manchhe()` (see [Classes](#classes)) | the class name |
| nothing | `khali` | `khali` |

### Numbers and arithmetic

| Operator | Meaning | Example | Result |
|---|---|---|---|
| `+` `-` | add, subtract | `7 - 2` | `5` |
| `*` `/` | multiply, divide | `7 / 2`, `7.0 / 2` | `3`, `3.5` |
| `%` | remainder | `7 % 2` | `1` |
| `( )` | grouping | `(1 + 2) * 3` | `9` |
| unary `-` `+` | sign | `-5` | `-5` |

`*`, `/` and `%` bind tighter than `+` and `-`. Dividing two whole numbers gives a whole
number (`7 / 2` is `3`); if either side is a decimal number the result is one too
(`7 / 2.0` is `3.5`). Dividing by zero stops the program with an error. Convert with
`int(x)` (drops the fraction), `float(x)` and `string(x)`.

### Variables

You create a variable by assigning to it. There is no declaration keyword. Assigning to it
again updates it.

```
mool = 5000
dar = 10
byaj = mool * dar * 3 / 100
vana(byaj)    # 1500
```

Using a variable before it has a value is an error. Names may only use ASCII letters,
digits and `_`, so `baki` works but `bakī` does not.

`x += 1` is short for `x = x + 1`; `-=`, `*=`, `/=` and `%=` work the same way, also on array
items (`a[0] += 1`) and fields (`p.umar += 1`).

### Types and constants

A variable can be **declared** with a type. After that it only accepts values of that type,
and the checker reports mistakes before the program runs:

```
int umar = 22
string naam = "Sita"
float dar = 2          # an int given to a float becomes 2.0
bool thik = sacho
array suchi = [1, 2]
umar = "hello"         # Type error: expected int but received string.
```

- Types: `int`, `float`, `bool`, `string`, `array`, `object`, `any` (anything), or the name
  of a class or interface.
- `int? x = khali`: a `?` after the type also allows `khali`. Variables of a class type may
  always be `khali`.
- Without a value, a declared variable starts empty: `0`, `0.0`, `jutho`, `""`, `[]` or `khali`.
- A declaration must be the first time the name gets a value in its scope.
- `const MAX = 10` (or `const int MAX = 10`) makes a constant: changing it is an error.
- Untyped variables (`x = 1`) still work as before and can hold anything.

### Comparisons

`==  !=  <  <=  >  >=` give `sacho` or `jutho`. `<` and friends compare two numbers or two
strings (see [Strings](#strings)); `==` also compares bools, arrays (item by item) and
`khali` (`x == khali`). You can't chain comparisons: `a < b < c` is an error.

`ra` (and), `wa` (or) and `hoina` (not) combine conditions, and the right side of `ra`/`wa`
only runs when needed. A condition (`yedi`, `jabasamma`) must be a bool or a number (any
non-zero number counts as true). Bools are not numbers: `sacho + 1` is an error, but
`int(sacho)` is `1`.

### Conditionals: `yedi` / `natara yedi` / `natara`

```
ank = 75
yedi ank >= 80 {
    vana(1)
} natara yedi ank >= 60 {
    vana(2)
} natara {
    vana(3)
}
```

`natara` must be on the **same line** as the closing `}` before it.

### While loop: `jabasamma`

```
i = 0
jabasamma i < 3 {
    vana(i)
    i = i + 1
}
```

### For loop: `ko_lagi ... in a..b`

`ko_lagi` counts from `a` up to, but **not including**, `b`. Both bounds are evaluated
once, before the loop starts.

```
ko_lagi i in 1..4 {
    vana(i)       # 1, 2, 3
}
```

### Functions: `kaam` and `farkau`

`kaam` defines a function and `farkau` returns a value from it.

```
kaam jamma(a, b) {
    farkau a + b
}

vana(jamma(10, 20))    # 30
```

- Parameters are separated by commas. A function can have no parameters: `kaam f() { }`.
- `farkau` exits right away, even from inside loops.
- A function that ends without `farkau`, or uses a bare `farkau`, returns `khali`.
- You can call a function before its definition appears in the file.
- Recursion works. Recursion deeper than 1000 calls stops with a clear error.
- A function can read top-level variables. Assigning inside a function always creates or
  updates a **local** variable, so a function never changes a top-level one.
- Functions are defined at the top level. You can't define a function inside another.
- You can call a function as a statement by itself, e.g. `salam()`.

Parameters and the result can have types. The type of the result goes after the `)`;
`void` means the function gives nothing back:

```
kaam jamma(int a, int b) int {
    farkau a + b
}

kaam namaste(string naam) void {
    vana("Namaste " + naam)
}
```

A wrong argument (`jamma("a", 1)`) or result is a type error.

### Arrays

An array is a list of values in `[ ]`, separated by commas. Indexes start at `0`.

```
ank = [72, 45, 91]
vana(ank[0])          # 72
ank[1] = 50
vana(ank)             # [72, 50, 91]
vana(lambai(ank))     # 3

ko_lagi x in ank {
    vana(x)           # 72, 50, 91
}
```

- An array can hold any values, including other arrays: `m[i][j]`.
- A long array literal can span several lines.
- Using an index outside `0` to `lambai(a) - 1` is an error.
- Arrays are **shared**, not copied: after `b = a`, changing `b[0]` also changes `a[0]`.
  The same happens when you pass an array to a function. Use `a.copy()` for a copy.
- `==` compares arrays item by item: `[1, 2] == [1, 2]` is `sacho`.

Arrays have methods (see also [the standard library](#built-in-functions-and-the-standard-library)):

```
a = [3, 1, 2]
a.push(5)              # [3, 1, 2, 5]   (same as thapa(a, 5))
a.pop()                # 5              (same as hata(a))
a.sort()               # [1, 2, 3]
vana(a.contains(2))    # sacho
vana(a.join("-"))      # 1-2-3
vana(a.sum())          # 6
```

`push pop insert(i, x) remove(i) clear contains index_of reverse sort slice(from, to) join
copy sum min max length`

### Strings

A string is text in double quotes.

```
naam = "Charitra"
vana("Namaste " + naam)        # Namaste Charitra
vana("Umar: " + 22)            # Umar: 22
vana(lambai(naam))             # 8
vana(naam[0])                  # C

ko_lagi akshar in "abc" {
    vana(akshar)               # a, b, c
}
```

- `+` joins two strings. If one side is a number, it is turned into text first. `+` works
  left to right, so `"a" + 1 + 2` is `a12` but `1 + 2 + "a"` is `3a`.
- `==` and `!=` check whether two strings have the same text. `<`, `>`, `<=` and `>=` use
  dictionary order (`"apple" < "banana"`); every uppercase letter comes before every
  lowercase one. Comparing a string with a number is an error.
- `s[i]` is the character at index `i`, as a one-character string. `lambai(s)` is the
  number of characters. Devanagari works: `lambai("नमस्ते")` is `6`, counting each letter
  and vowel sign.
- Strings can't be changed in place: `s[0] = "x"` is an error. Build a new string with
  `+` instead.
- Inside a string, `\n` is a new line, `\t` is a tab, `\"` is a quote and `\\` is a
  backslash. A string must end on the line it starts on.
- When an array is printed, the strings inside it are shown with quotes:
  `vana(["a", 1])` prints `["a", 1]`.
- `"ab" * 3` repeats a string: `ababab`.
- `"Mero naam ${naam} ho"` puts the value of an expression into the text.

Strings have methods too: `"namaste".upper()` is `NAMASTE`.

`upper lower trim split(sep) contains find replace(old, new) starts_with ends_with
slice(from, to) repeat chars code reverse length`

### Classes

A `class` describes a kind of object: its **fields** (data) and **methods** (functions that
belong to it). Call the class like a function to create an object.

```
class Manchhe {
    naam = ""
    umar = 0

    kaam bolau() {
        vana("Namaste " + naam)
    }
}

manchhe = Manchhe()
manchhe.naam = "Charitra"
manchhe.umar = 22
manchhe.bolau()          # Namaste Charitra
vana(manchhe)            # Manchhe(naam="Charitra", umar=22)
```

**Fields.** Each field is listed in the class with its starting value, e.g. `umar = 0`.
Every new object gets fresh starting values, so `saman = []` gives each object its own
array. Using a field that the class doesn't list (`manchhe.umer = 5`) is an error, which
catches typos.

**Methods.** Methods are written with `kaam` inside the class and called with
`object.method(...)`. Inside a method, a field's bare name (`naam`) means that field of the
current object, and another method can be called by its bare name (`bolau()`).

**`yo` (this object).** `yo` is the object the method was called on. You need it when a
parameter has the same name as a field: the parameter wins, so write `yo.naam` to reach the
field. A method can also `farkau yo` so calls can be chained: `g.badhau().badhau()`.

**Constructor: `banau`.** A method named `banau` runs by itself when an object is created,
and receives the arguments given to the class:

```
class Bindu {
    x = 0
    y = 0

    kaam banau(x, y) {
        yo.x = x
        yo.y = y
    }
}

b = Bindu(3, 4)
vana(b.x)                # 3
```

`banau` can't return a value (the new object is always the result) and can't be called
directly. A class without `banau` is created with no arguments: `Manchhe()`.

**Private members.** A field or method whose name starts with `_` can only be used inside
the methods of its own class:

```
class Khata {
    _baki = 0

    kaam jamma(rakam) {
        _baki = _baki + rakam
    }

    kaam baki() {
        farkau _baki
    }
}

k = Khata()
k.jamma(100)
vana(k.baki())           # 100
vana(k._baki)            # error: '_baki' is private to class 'Khata'
```

- Objects are **shared**, like arrays: after `b = a`, changing `b.naam` also changes
  `a.naam`. Passing an object to a function shares it too.
- Classes are defined at the top level. A class can be used before its definition in the
  file. Class and function names share one namespace, so they can't clash.
- Objects can be compared with `khali` (`p == khali`), but two objects can't be compared with
  `==`; compare their fields instead.

**Typed fields.** A field can have a type, like a variable. Without a value it starts empty:

```
class Manchhe {
    string naam
    int umar
}
```

**Inheritance.** `class Kukur(Janawar)` makes `Kukur` a child of `Janawar`: it gets all of
`Janawar`'s fields and methods, and can replace (override) methods with its own versions.
`super.method(...)` calls the parent's version, and `super(...)` inside `banau` runs the
parent's constructor:

```
class Janawar {
    string naam
    kaam banau(string n) {
        naam = n
    }
    kaam bolau() string {
        farkau "..."
    }
}

class Kukur(Janawar) {
    kaam banau(string n) {
        super(n)
    }
    kaam bolau() string {
        farkau naam + ": Bhau bhau"
    }
}

vana(Kukur("Tommy").bolau())    # Tommy: Bhau bhau
```

A class has at most one parent class. An overriding method takes the same number of
parameters as the one it replaces. Private `_` members are shared between a class and its
parents and children.

**Interfaces.** An `interface` lists methods without bodies. A class that names it must
define them all:

```
interface Bolne {
    kaam bolau() string
}

class Suga(Bolne) {
    kaam bolau() string {
        farkau "Namaste!"
    }
}

kaam bolaideu(Bolne b) {       # any object that is a Bolne
    vana(b.bolau())
}
bolaideu(Suga())
```

List the parent class and interfaces together: `class Kukur(Janawar, Bolne, Khelne)`.
An interface can extend other interfaces: `interface Naam(Aakar)`. Interfaces can't make
objects.

---

## Built-in functions and the standard library

Always available:

| Function | Does |
|---|---|
| `lambai(x)` | length of an array or string |
| `thapa(a, x)` / `hata(a)` | push onto / pop from an array |
| `padha()` | read a line from the keyboard (a string) |
| `sankhya(s)` | text to a number (`"42"` → `42`, `"2.5"` → `2.5`) |
| `int(x)` `float(x)` `string(x)` `bool(x)` | convert |
| `prakar(x)` | the type of a value, as a string (`"int"`, `"Manchhe"`, ...) |

The standard library is built in; `import` a module to use it:

```
import math
vana(math.sqrt(16))      # 4.0
vana(math.PI)            # 3.141592653589793
```

| Module | Functions |
|---|---|
| `math` | `sqrt pow abs floor ceil round(x[, digits]) min max sin cos tan exp log log10 random randint(a, b) seed`, constants `PI E` |
| `string` | the string methods as functions (`string.upper(s)`), plus `from_code(n)` and `join(array, sep)` |
| `array` | the array methods as functions (`array.push(a, x)`), plus `range(n)` / `range(a, b)` |
| `io` | `write(x)` (no newline), `read()`, `read_file(path)`, `write_file(path, text)`, `append_file(path, text)`, `exists(path)` |
| `time` | `now()` (seconds), `millis()`, `sleep(ms)` |
| `os` | `args()` (the program's command-line arguments), `env(name)`, `exit([code])` |

---

## Error messages

NepLang tries to give errors a beginner can understand. Each error names the file, line
and column, shows the line of code, and points to the problem:

```
Error in program.np at line 5, column 6:

    vana(bhitra)
         ^

Variable 'bhitra' is not defined. Assign it a value first, e.g. 'bhitra = 0'.
```

Only the first syntax error is reported. Fix it and run the program again. Keywords can't
be used as variable names (`yedi = 5` is an error). Before a program runs, the checker
reports every mistake it can prove: undefined names (with a suggestion such as
`Did you mean 'umar'?`), wrong argument counts, type errors such as
`Type error: expected int but received string.`, changed constants, and missing interface
methods.

---

## Keyword reference

| Keyword | Meaning | English equivalent |
|---|---|---|
| `vana` | say / print | `print` |
| `yedi` | if | `if` |
| `natara` | otherwise | `else` |
| `jabasamma` | as long as | `while` |
| `ko_lagi` | for | `for` |
| `in` | in | `in` |
| `kaam` | work / task | `function` |
| `farkau` | return / send back | `return` |
| `class` | class | `class` |
| `interface` | interface | `interface` |
| `super` | the parent class | `super` |
| `yo` | this | `this` / `self` |
| `ra` / `wa` / `hoina` | and / or / not | `and` / `or` / `not` |
| `ruk` / `agadi` | stop / go ahead | `break` / `continue` |
| `sacho` / `jutho` | true / false | `true` / `false` |
| `khali` | empty | `null` / `None` |
| `const` | constant | `const` |
| `import` | import | `import` |

`banau` ("create") is not a keyword, but a method with that name is a class's constructor.
`lambai` ("length") is a built-in function. The type names (`int`, `string`, ...) are not
keywords either.

---

## Current limitations

These parts of the original design are **not** available yet:

- generic types (`array<int>`), dictionaries/maps, closures and functions as values
- concurrency (`async`/`await`, threads, channels) and the `http`, `json`, `crypto` and
  `database` modules
- `neplang format`, `neplang test`, and Nepali error messages (`--lang=ne`)
- `"${...}"` pieces can't contain a `"` (a string ends at the next quote); write `\${` for a literal `${`
- `x += e` on an array item or field evaluates the target twice (`a[f()] += 1` calls `f` twice)

Expressions may span lines after an operator, or inside `( )`, call arguments and `[ ]`.

---

## Development

```bash
make          # build bin/neplang
make debug    # build with AddressSanitizer + UBSan (use while developing)
make test     # build, then run every test suite under tests/ (interpreter + VM)
make test-all # also compile every test to a native executable
make clean    # delete bin/
```

The compiler is built with `-Wall -Wextra -Wpedantic` and should build with zero warnings.

### Tests

`tests/run_tests.sh` is a plain bash script:

- Each folder in `tests/` is a suite (`calculator/`, `variables/`, `conditionals/`,
  `loops/`, `functions/`, ...).
- `NAME.np` + `NAME.expected`: the program is run and its output must match the
  `.expected` file exactly.
- `NAME.error.np`: the program must fail with a non-zero exit code (syntax or runtime
  error).

To add a test, drop the files into a suite folder. No registration is needed.

### How it works

```
source.np → Lexer → Parser → AST → imports → classes → checker ─┬→ Interpreter (tree-walk)
                                                              └→ IR → optimizer ─┬→ bytecode VM
                                                                                 └→ C → native executable
```

| File | Role |
|---|---|
| `src/lexer.c` | Turns source text into tokens; keywords live in the `keywords[]` table |
| `src/parser.c` | Recursive-descent parser that builds the AST |
| `src/ast.c` / `ast.h` | AST node types, constructors, and `ast_free` |
| `src/modules.c` | `import`: splices files in, enables standard-library modules |
| `src/classes.c` | Resolves inheritance and interfaces |
| `src/checker.c` | Static checks and type errors before anything runs |
| `src/interpreter.c` | Walks the AST and runs the program |
| `src/ir.c`, `vm.c`, `cgen.c` | IR + optimizer, bytecode VM, C code generator |
| `src/rt.c` / `rt.h` | Runtime shared by all backends: operators, types, objects, the standard library |
| `src/value.c` / `value.h` | Runtime values |
| `src/diagnostics.c` | Shared error reporter (file, line, column, caret) |
| `src/main.c` | Command line, REPL, and the pipeline |

### Releasing prebuilt binaries

`.github/workflows/release.yml` builds static binaries for Linux (x86_64, arm64),
macOS (universal) and Windows (x86_64). It runs the test suite on each platform and
attaches the binaries, plus a `SHA256SUMS` file, to a GitHub Release. To publish a
release, push a version tag:

```bash
git tag v0.5.0
git push origin v0.5.0
```

`install.sh` and `install.ps1` always download from the **latest** release. To install a
specific version, set `NEPLANG_VERSION=v0.5.0` first.

---

## Project layout

```
src/          lexer, parser, AST, interpreter, diagnostics, main
tests/        per-milestone test cases (input + expected output)
examples/     example .np programs
docs/         language design notes
Makefile      build commands
install.sh    one-line installer for Linux / macOS
install.ps1   one-line installer for Windows
.github/      release workflow (builds the prebuilt binaries)
```

---

## Roadmap

1. ✅ Calculator: integer arithmetic
2. ✅ Variables
3. ✅ Conditionals: `yedi` / `natara`
4. ✅ Loops: `jabasamma`, `ko_lagi`
5. ✅ Functions: `kaam` / `farkau`
6. ✅ Arrays
7. ✅ Strings
8. ✅ Structs / classes
9. ✅ Symbol table (hashed, scoped)
10. ✅ Static checker (type inference: undefined names, argument counts, impossible operations)
11. ✅ Modules (`import`)
12. ✅ Bytecode VM (`--vm`)
13. ✅ Intermediate representation (`--ir`, with constant folding and dead-code removal)
14. ✅ Native code generation (`--emit-c`, `--build`: NepLang → C → executable)
15. ✅ Type system: `int float bool string array object any`, `?` nullable, class types, `void`, `const`
16. ✅ Inheritance, interfaces and `super`
17. ✅ Standard library (`math string array io time os`) and methods on arrays and strings
18. ✅ REPL and `run` / `check` / `build` commands

### Ways to run a program

```bash
neplang                          # interactive REPL
neplang prog.np [args...]        # tree-walking interpreter (or: neplang run prog.np)
neplang check prog.np            # report errors without running
neplang build prog.np -o prog    # native executable (same as --build)
neplang --vm prog.np             # compile to bytecode, run on the virtual machine
neplang --ir prog.np             # print the optimized intermediate representation
neplang --emit-c prog.np         # print the generated C source
neplang --build prog.np -o prog  # native executable (needs a C compiler: cc, or $CC)
```

All execution paths share one runtime (`src/rt.c`) so they print identical output and
identical error messages, every feature included (native programs report runtime errors without the
source line). The runtime sources are built into the `neplang` binary, so `--build`
only needs a C compiler (even with a downloaded prebuilt binary); set `NEPLANG_RUNTIME` to a
runtime folder (e.g. the checkout's `src/`) to use those files instead. Native speed on
`fib(30)`: interpreter 0.80 s, VM 0.52 s, native 0.10 s.

### Modules

`import "helpers.np"` (or `import helpers`, same as `import "helpers.np"`) splices another
file's functions, classes and variables into the program. Files are looked up next to the
importing file, then in each directory of `NEPLANG_PATH`; each file loads once, cycles are
harmless. The names of the standard-library modules (`import math`, ...) always mean the
built-in module; import a file with such a name with quotes: `import "math.np"`.

### The REPL

Run `neplang` with no file to type code and see results right away. A bare expression is
printed (unless it is `khali`); an open `{` continues on the next line; an error doesn't end
the session; a function can be redefined. Type `exit` or press Ctrl-D to leave.

```
> x = 5
> x * 2
10
> kaam sq(n) {
...     farkau n * n
... }
> sq(x)
25
```

---

## Contributing

Work on NepLang goes one milestone at a time. Please keep each change to the current
milestone, add tests for any new feature (including at least one `.error.np` case), and
make sure `make test` passes with no compiler warnings.
